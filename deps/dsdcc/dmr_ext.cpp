///////////////////////////////////////////////////////////////////////////////////
// OrbitSDR additions to the DMR decoder of DSDcc (GPL v3, like DSDcc).          //
//                                                                               //
// DSDcc decodes the voice and the addresses of the embedded LC. Added here:     //
//  - the 5 bit checksum of the embedded LC, and its FLCO: talker alias and GPS  //
//    blocks no longer overwrite the addresses,                                  //
//  - service options: emergency, privacy (encrypted), broadcast, priority,      //
//  - the data bursts (BPTC 196,96): voice LC header, terminator with LC,        //
//    privacy (PI) header (algorithm, key id), CSBK, data header,                //
//  - text messages (SMS): UDT and packet data (rate 1/2 blocks) over IP / UDP.  //
// References: ETSI TS 102 361-1 (DMR air interface), section 9 and annex B.     //
///////////////////////////////////////////////////////////////////////////////////

#include <string.h>
#include <string>
#include <cmath>
#include "dmr.h"
#include "dsd_decoder.h"

namespace DSDcc
{

namespace
{

// --- Hamming codes of the BPTC: syndromes built from the parity equations ---

// Hamming (15,11,3): parity bits d[11..14]
inline int synd15(const unsigned char *d)
{
    const int c0 = d[0] ^ d[1] ^ d[2] ^ d[3] ^ d[5] ^ d[7] ^ d[8];
    const int c1 = d[1] ^ d[2] ^ d[3] ^ d[4] ^ d[6] ^ d[8] ^ d[9];
    const int c2 = d[2] ^ d[3] ^ d[4] ^ d[5] ^ d[7] ^ d[9] ^ d[10];
    const int c3 = d[0] ^ d[1] ^ d[2] ^ d[4] ^ d[6] ^ d[7] ^ d[10];
    return (c0 != d[11]) | ((c1 != d[12]) << 1) | ((c2 != d[13]) << 2) | ((c3 != d[14]) << 3);
}

// Hamming (13,9,3): parity bits d[9..12]
inline int synd13(const unsigned char *d)
{
    const int c0 = d[0] ^ d[1] ^ d[3] ^ d[5] ^ d[6];
    const int c1 = d[0] ^ d[1] ^ d[2] ^ d[4] ^ d[6] ^ d[7];
    const int c2 = d[0] ^ d[1] ^ d[2] ^ d[3] ^ d[5] ^ d[7] ^ d[8];
    const int c3 = d[0] ^ d[2] ^ d[4] ^ d[5] ^ d[8];
    return (c0 != d[9]) | ((c1 != d[10]) << 1) | ((c2 != d[11]) << 2) | ((c3 != d[12]) << 3);
}

// Corrects one bit error; true when the word is (now) valid
template <int N, int (*S)(const unsigned char *)>
bool fixHamming(unsigned char *d, bool& changed)
{
    // syndrome -> bit position; built once (a function-local static object is initialised
    // thread-safely, the decoders of several channels run in parallel)
    struct Table {
        int v[16];
        Table() {
            for (int i = 0; i < 16; i++) v[i] = -1;
            unsigned char t[N];
            for (int b = 0; b < N; b++)
            {
                memset(t, 0, N);
                t[b] = 1;
                v[S(t)] = b;
            }
        }
    };
    static const Table tab;
    const int* table = tab.v;
    const int s = S(d);
    if (s == 0) return true;
    if (table[s] < 0) return false;
    d[table[s]] ^= 1;
    changed = true;
    return true;
}

// BPTC (196,96): deinterleave, iterative row / column correction, extract the 96 bits.
bool bptc19696(const unsigned char *raw, unsigned char *out12)
{
    unsigned char d[196];
    for (int a = 0; a < 196; a++) d[a] = raw[(a * 181) % 196];

    bool ok = false;
    for (int it = 0; it < 5; it++)
    {
        bool changed = false;
        ok = true;
        unsigned char col[13];
        for (int c = 0; c < 15; c++)
        {
            for (int r = 0; r < 13; r++) col[r] = d[1 + c + r * 15];
            bool ch = false;
            if (!fixHamming<13, synd13>(col, ch)) ok = false;
            if (ch) { for (int r = 0; r < 13; r++) d[1 + c + r * 15] = col[r]; changed = true; }
        }
        for (int r = 0; r < 9; r++)
        {
            bool ch = false;
            if (!fixHamming<15, synd15>(&d[1 + r * 15], ch)) ok = false;
            if (ch) changed = true;
        }
        if (!changed) break;
    }

    unsigned char bits[96];
    int p = 0;
    for (int a = 4; a <= 11; a++) bits[p++] = d[a];
    for (int r = 1; r < 9; r++)
        for (int a = 1 + r * 15; a <= 11 + r * 15; a++) bits[p++] = d[a];
    for (int i = 0; i < 12; i++)
    {
        unsigned char v = 0;
        for (int b = 0; b < 8; b++) v = (v << 1) | bits[i * 8 + b];
        out12[i] = v;
    }
    return ok;
}

// GF(256), primitive polynomial x^8+x^4+x^3+x^2+1
unsigned char gmul(unsigned char a, unsigned char b)
{
    unsigned char r = 0;
    while (b)
    {
        if (b & 1) r ^= a;
        a = (a & 0x80) ? (unsigned char)((a << 1) ^ 0x1D) : (unsigned char)(a << 1);
        b >>= 1;
    }
    return r;
}

// Reed-Solomon (12,9) parity of a full LC (detection only)
bool rs129Ok(const unsigned char *b, unsigned char mask)
{
    static const unsigned char poly[3] = {64, 56, 14};
    unsigned char par[3] = {0, 0, 0};
    for (int i = 0; i < 9; i++)
    {
        const unsigned char f = b[i] ^ par[2];
        par[2] = par[1] ^ gmul(poly[2], f);
        par[1] = par[0] ^ gmul(poly[1], f);
        par[0] = gmul(poly[0], f);
    }
    return par[2] == (b[9] ^ mask) && par[1] == (b[10] ^ mask) && par[0] == (b[11] ^ mask);
}

// CRC-CCITT of the first 10 bytes against bytes 10..11 (with the data type mask)
bool crcCcittOk(const unsigned char *b, unsigned short mask)
{
    unsigned short crc = 0;
    for (int i = 0; i < 10; i++)
    {
        crc ^= (unsigned short)(b[i] << 8);
        for (int k = 0; k < 8; k++) crc = (crc & 0x8000) ? (unsigned short)((crc << 1) ^ 0x1021) : (unsigned short)(crc << 1);
    }
    crc = (unsigned short)(~crc) ^ mask;
    return (b[10] == (crc >> 8)) && (b[11] == (crc & 0xFF));
}

unsigned int u24(const unsigned char *b) { return ((unsigned int)b[0] << 16) | ((unsigned int)b[1] << 8) | b[2]; }

// --- Data and text messages (the reassembly follows SDRangel's dsdprotocoldmr.cpp and
//     dsdpackettext.cpp, GPL v3, by Jon Beniston M7RCE: ETSI TS 102 361-1 9.2.6 and B.3,
//     TS 102 361-4 UDT, the DMR text service over UDP and Motorola's TMS) ---

// CRC-CCITT of n bytes as the data header / UDT blocks carry it (inverted, no mask)
unsigned short dmrCcitt(const unsigned char *b, int n)
{
    unsigned short crc = 0;
    for (int i = 0; i < n; i++)
    {
        crc ^= (unsigned short)(b[i] << 8);
        for (int k = 0; k < 8; k++) crc = (crc & 0x8000) ? (unsigned short)((crc << 1) ^ 0x1021) : (unsigned short)(crc << 1);
    }
    return (unsigned short)~crc;
}

// MSB first CRC over bits
unsigned int crcBits(const unsigned char *bits, int nbBits, int order, unsigned int poly, unsigned int init, unsigned int finalXor)
{
    const unsigned int mask = order == 32 ? 0xFFFFFFFFu : ((1u << order) - 1);
    unsigned int crc = init & mask;
    for (int i = 0; i < nbBits; i++)
    {
        const unsigned int fb = ((crc >> (order - 1)) & 1) ^ (bits[i] & 1);
        crc = (crc << 1) & mask;
        if (fb) crc ^= poly;
    }
    return (crc ^ finalXor) & mask;
}

// The packet CRC-32 (B.3.9): 16 bit words, the low octet first
unsigned int dmrPacketCRC32(const unsigned char *bytes, int n)
{
    std::string bits((size_t)n * 8, '\0');
    int o = 0;
    for (int i = 0; i < n; i += 2)
    {
        const int idx[2] = {i + 1, i};
        for (int k = 0; k < 2; k++)
        {
            const unsigned char v = idx[k] < n ? bytes[idx[k]] : 0;
            for (int bit = 7; bit >= 0; bit--) bits[o++] = (char)((v >> bit) & 1);
        }
    }
    return crcBits((const unsigned char *)bits.data(), o, 32, 0x04C11DB7u, 0, 0);
}

void appendUTF8(std::string& t, unsigned int cp)
{
    if (cp < 0x20 || cp == 0x7F) cp = ' ';
    else if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (cp < 0x80) t.push_back((char)cp);
    else if (cp < 0x800) { t.push_back((char)(0xC0 | (cp >> 6))); t.push_back((char)(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) { t.push_back((char)(0xE0 | (cp >> 12))); t.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); t.push_back((char)(0x80 | (cp & 0x3F))); }
    else { t.push_back((char)(0xF0 | (cp >> 18))); t.push_back((char)(0x80 | ((cp >> 12) & 0x3F))); t.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); t.push_back((char)(0x80 | (cp & 0x3F))); }
}

void decodeUTF16(const unsigned char *b, int n, bool bigEndian, std::string& t)
{
    auto unit = [&](int i) { return bigEndian ? (((unsigned int)b[i] << 8) | b[i + 1]) : (((unsigned int)b[i + 1] << 8) | b[i]); };
    int i = 0;
    while (i + 1 < n)
    {
        unsigned int cp = unit(i);
        i += 2;
        if (cp == 0xFEFF && t.empty()) continue;
        if (cp == 0) break;   // the end of the text, padding follows
        if (cp >= 0xD800 && cp <= 0xDBFF)
        {
            if (i + 1 >= n) { appendUTF8(t, 0xFFFD); break; }
            const unsigned int lo = unit(i);
            if (lo < 0xDC00 || lo > 0xDFFF) cp = 0xFFFD;
            else { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); i += 2; }
        }
        appendUTF8(t, cp);
    }
}

unsigned short internetChecksum(const unsigned char *b, int n)
{
    unsigned int sum = 0;
    for (int i = 0; i < n; i += 2)
    {
        sum += ((unsigned int)b[i] << 8) | (i + 1 < n ? b[i + 1] : 0);
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (unsigned short)~sum;
}

// Motorola TMS: | length (2) | header (1) | address length (1) | address | options | text |
bool textFromTMS(const unsigned char *pdu, int n, std::string& t)
{
    if (n < 4) return false;
    int length = ((int)pdu[0] << 8) | pdu[1];
    if (length < 2 || length + 2 > n) length = n - 2;
    const unsigned char header = pdu[2];
    if ((header & 0x10) || (header & 0x0F)) return false;   // a control message, or not simple text
    int pos = 4 + pdu[3];
    if (pos > length + 2) return false;
    int encoding = 4;
    if (header & 0x80)
    {
        bool more = true;
        int option = 0;
        while (more && pos < length + 2)
        {
            const unsigned char byte = pdu[pos++];
            more = (byte & 0x80) != 0;
            if (option == 1) encoding = byte & 0x1F;
            option++;
        }
    }
    const int textBytes = length + 2 - pos;
    if (textBytes < 2) return false;
    bool bigEndian = false;   // UCS-2 little endian as Motorola sends it
    if (encoding != 4)
    {
        int zh = 0, zl = 0;
        for (int i = 0; i + 1 < textBytes; i += 2) { zh += pdu[pos + i] == 0; zl += pdu[pos + i + 1] == 0; }
        bigEndian = zh > zl;
    }
    decodeUTF16(pdu + pos, textBytes & ~1, bigEndian, t);
    return !t.empty();
}

bool textFromUdp(int sport, int dport, const unsigned char *p, int n, std::string& t, std::string& kind)
{
    t.clear();
    if (sport == 4007 || dport == 4007) { kind = "TMS"; return textFromTMS(p, n, t); }
    if (sport == 5016 || dport == 5016 || sport == 5017 || dport == 5017)
    {
        if (n < 2 || (n & 1)) return false;
        kind = "UTF-16BE/IP";
        decodeUTF16(p, n, true, t);
        return !t.empty();
    }
    return false;
}

bool textFromIPv4(const unsigned char *ip, int n, std::string& t, std::string& kind)
{
    if (n < 28) return false;
    const int hb = (ip[0] & 0x0F) * 4;
    const int total = ((int)ip[2] << 8) | ip[3];
    const int frag = ((ip[6] & 0x3F) << 8) | ip[7];
    if ((ip[0] >> 4) != 4 || hb < 20 || hb + 8 > n || total < hb + 8 || total > n || ip[9] != 17 || frag != 0
        || internetChecksum(ip, hb) != 0)
        return false;
    const unsigned char *udp = ip + hb;
    const int sport = ((int)udp[0] << 8) | udp[1], dport = ((int)udp[2] << 8) | udp[3];
    const int ul = ((int)udp[4] << 8) | udp[5];
    if (ul < 8 || hb + ul > total) return false;
    return textFromUdp(sport, dport, udp + 8, ul - 8, t, kind);
}

} // namespace

void DSDDMR::setText(int slot, unsigned int src, unsigned int dst, bool group, const char *kind, const std::string& text)
{
    SlotInfo& si = m_slotInfo[slot & 1];
    std::string t = text;
    while (!t.empty() && (t.back() == ' ' || t.back() == 0)) t.pop_back();
    if (t.empty()) return;
    si.textSrc = src;
    si.textDst = dst;
    si.textGroup = group;
    strncpy(si.textKind, kind, sizeof(si.textKind) - 1);
    si.textKind[sizeof(si.textKind) - 1] = 0;
    strncpy(si.text, t.c_str(), sizeof(si.text) - 1);
    si.text[sizeof(si.text) - 1] = 0;
    si.textSeq++;
    si.seq++;
}

void DSDDMR::parseDataHeader(const unsigned char *h, int slot)
{
    DataAssembly& a = m_dataAsm[slot & 1];
    a = DataAssembly();
    SlotInfo& si = m_slotInfo[slot & 1];
    const int dpf = h[0] & 0x0F;
    si.dataDpf = dpf;
    si.dataSap = h[1] >> 4;
    si.dataGroup = (h[0] & 0x80) != 0;
    si.dataUdtFormat = -1;
    si.dataBlocks = 0;
    si.dataDst = u24(&h[2]);
    si.dataSrc = u24(&h[5]);

    if (dpf == 0)   // UDT: octet 1's low nibble the format, octet 8 the pad nibbles and the appended blocks less one
    {
        const int sap = h[1] >> 4;
        const int blocks = (h[8] & 3) + 1;
        si.dataBlocks = blocks;
        si.dataUdtFormat = h[1] & 0x0F;
        if (sap != 0 || (h[0] & 0x70) != 0 || (h[8] & 4) != 0 || (h[9] & 0xC0) != 0) return;
        a.udt = true;
        a.group = si.dataGroup;
        a.format = h[1] & 0x0F;
        a.padNibbles = (h[8] >> 3) & 0x1F;
        a.blocksExpected = blocks;
        a.src = si.dataSrc;
        a.dst = si.dataDst;
        if (blocks * 96 - 16 - a.padNibbles * 4 <= 0) a = DataAssembly();
        return;
    }
    if (dpf != 2 && dpf != 3) return;   // only unconfirmed and confirmed packet data carry text
    const bool confirmed = dpf == 3;
    const bool response = (h[0] & 0x40) != 0;
    const bool compressed = (h[0] & 0x20) != 0;
    const int sap = h[1] >> 4;
    const int blocks = h[8] & 0x7F;
    const bool full = (h[8] & 0x80) != 0;
    si.dataBlocks = blocks;
    const bool supported = (compressed && sap == 3) || (!compressed && sap == 4);
    if (response != confirmed || !full || blocks <= 0 || blocks > (confirmed ? 153 : 128) || !supported
        || (!confirmed && (h[9] & 0xF0) != 0))
        return;
    a.packet = true;
    a.confirmed = confirmed;
    a.group = si.dataGroup;
    a.sap = sap;
    a.padOctets = (h[0] & 0x10) | (h[1] & 0x0F);
    a.blocksExpected = blocks;
    a.src = si.dataSrc;
    a.dst = si.dataDst;
}

void DSDDMR::parseUdtBlock(const unsigned char *b, int slot)
{
    DataAssembly& a = m_dataAsm[slot & 1];
    if (!a.udt || a.blocksReceived >= a.blocksExpected) return;
    memcpy(a.data + a.blocksReceived * 12, b, 12);
    a.blocksReceived++;
    if (a.blocksReceived != a.blocksExpected) return;
    const int total = a.blocksExpected * 12;
    const unsigned short rx = (unsigned short)((a.data[total - 2] << 8) | a.data[total - 1]);
    if (dmrCcitt(a.data, total - 2) != rx) { a = DataAssembly(); return; }
    const int useful = (total - 2) * 8 - a.padNibbles * 4;
    std::string t;
    const char *kind = 0;
    if (a.format == 3)   // ISO 7 bit, packed MSB first
    {
        for (int c = 0; c < useful / 7; c++)
        {
            unsigned int v = 0;
            for (int bit = 0; bit < 7; bit++)
            {
                const int pos = c * 7 + bit;
                v = (v << 1) | ((a.data[pos / 8] >> (7 - (pos & 7))) & 1);
            }
            appendUTF8(t, v);
        }
        kind = "UDT 7-bit";
    }
    else if (a.format == 4 && (useful & 7) == 0)   // ISO-8859-1
    {
        for (int i = 0; i < useful / 8; i++) appendUTF8(t, a.data[i]);
        kind = "UDT 8-bit";
    }
    else if (a.format == 7 && (useful & 15) == 0)  // UTF-16 big endian
    {
        decodeUTF16(a.data, useful / 8, true, t);
        kind = "UDT UTF-16";
    }
    if (kind) setText(slot, a.src, a.dst, a.group, kind, t);
    a = DataAssembly();
}

void DSDDMR::parsePacketBlock(const unsigned char *b, int slot)
{
    DataAssembly& a = m_dataAsm[slot & 1];
    if (!a.packet || a.blocksReceived >= a.blocksExpected) return;
    const unsigned char *data = b;
    int n = 12;
    if (a.confirmed)   // serial number (7 bits) and CRC-9 in front of 10 data octets
    {
        const int serial = b[0] >> 1;
        const unsigned int rx = ((((unsigned int)(b[0] & 1)) << 8) | b[1]) ^ 0x0F0;
        unsigned char bits[87];
        for (int i = 0; i < 80; i++) bits[i] = (b[2 + i / 8] >> (7 - (i & 7))) & 1;
        for (int i = 0; i < 7; i++) bits[80 + i] = (serial >> (6 - i)) & 1;
        if (crcBits(bits, 87, 9, 0x059, 0, 0x1FF) != rx || (a.nextSerial >= 0 && serial != a.nextSerial))
        {
            a = DataAssembly();
            return;
        }
        a.nextSerial = (serial + 1) & 0x7F;
        data = b + 2;
        n = 10;
    }
    if (a.dataBytes + n > (int)sizeof(a.data)) { a = DataAssembly(); return; }
    memcpy(a.data + a.dataBytes, data, n);
    a.dataBytes += n;
    a.blocksReceived++;
    if (a.blocksReceived == a.blocksExpected) publishPacketText(slot);
}

void DSDDMR::publishPacketText(int slot)
{
    DataAssembly& a = m_dataAsm[slot & 1];
    if (a.dataBytes < 4 || a.padOctets > a.dataBytes - 4) { a = DataAssembly(); return; }
    const int crcPos = a.dataBytes - 4;
    const unsigned int rx = (unsigned int)a.data[crcPos] | ((unsigned int)a.data[crcPos + 1] << 8)
        | ((unsigned int)a.data[crcPos + 2] << 16) | ((unsigned int)a.data[crcPos + 3] << 24);
    if (dmrPacketCRC32(a.data, crcPos) != rx) { a = DataAssembly(); return; }
    const int payload = crcPos - a.padOctets;
    std::string t, kind;
    bool ok = false;
    if (a.sap == 4) ok = textFromIPv4(a.data, payload, t, kind);
    else if (a.sap == 3 && payload >= 5)   // the DMR compressed UDP/IPv4 header
    {
        const unsigned char *c = a.data;
        const int opcode = ((c[3] >> 7) << 1) | (c[4] >> 7);
        const int spid = c[3] & 0x7F, dpid = c[4] & 0x7F;
        int pos = 5;
        int sport = spid == 1 ? 5016 : spid == 2 ? 5017 : -1;
        int dport = dpid == 1 ? 5016 : dpid == 2 ? 5017 : -1;
        if (opcode == 0 && spid == 0 && pos + 2 <= payload) { sport = ((int)c[pos] << 8) | c[pos + 1]; pos += 2; }
        if (opcode == 0 && dpid == 0 && pos + 2 <= payload) { dport = ((int)c[pos] << 8) | c[pos + 1]; pos += 2; }
        if (opcode == 0 && pos <= payload) ok = textFromUdp(sport, dport, c + pos, payload - pos, t, kind);
    }
    if (ok) setText(slot, a.src, a.dst, a.group, kind.c_str(), t);
    a = DataAssembly();
}

// Embedded LC (voice superframe): 7 Hamming rows of 16 bits. Rows 0-1 carry 11 LC bits,
// rows 2-6 carry 10 LC bits + one bit of the 5 bit checksum (column 10).
bool DSDDMR::embeddedLC(const unsigned char *raw, DMRAddresses& addresses)
{
    unsigned char bits[72];
    int p = 0;
    for (int r = 0; r < 7; r++)
    {
        const int n = r < 2 ? 11 : 10;
        for (int c = 0; c < n; c++) bits[p++] = raw[r * 16 + c];
    }
    unsigned char lc[9];
    for (int i = 0; i < 9; i++)
    {
        unsigned char v = 0;
        for (int b = 0; b < 8; b++) v = (v << 1) | bits[i * 8 + b];
        lc[i] = v;
    }
    int cs = 0;
    for (int r = 2; r < 7; r++) cs = (cs << 1) | raw[r * 16 + 10];
    int sum = 0;
    for (int i = 0; i < 9; i++) sum += lc[i];
    const int slot = (&addresses == &m_slot1Addresses) ? 0 : 1;
    if ((sum % 31) != cs)
    {
        m_slotInfo[slot].lcBad++;
        return false;
    }
    m_slotInfo[slot].lcOk++;
    handleLC(lc, slot, 1);
    const int flco = lc[0] & 0x3F;
    if (flco != 0 && flco != 3) return false;   // talker alias, GPS ...: not the addresses
    addresses.m_group = (flco == 0);
    addresses.m_target = u24(&lc[3]);
    addresses.m_source = u24(&lc[6]);
    return true;
}

void DSDDMR::updateAlias(SlotInfo& si)
{
    if (!(si.aliasMask & 1)) return;
    const int format = si.aliasRaw[0][0] >> 6;
    const int len = (si.aliasRaw[0][0] >> 1) & 0x1F;
    // the alias bits: header (49 bits in 7 bit format, else 48 from byte 3) + 3 blocks of 56
    unsigned char bytes[32];
    int nb = 0;
    unsigned char bitbuf[8 * 32];
    int nbits = 0;
    if (format == 0) bitbuf[nbits++] = si.aliasRaw[0][0] & 1;
    for (int blk = 0; blk < 4; blk++)
    {
        if (!(si.aliasMask & (1u << blk))) break;   // only the contiguous part
        const int from = blk == 0 ? 1 : 0;
        for (int i = from; i < 7; i++)
            for (int b = 7; b >= 0; b--) bitbuf[nbits++] = (si.aliasRaw[blk][i] >> b) & 1;
    }
    std::string out;
    if (format == 0)
    {
        for (int c = 0; c + 7 <= nbits && (int)out.size() < len; c += 7)
        {
            int v = 0;
            for (int b = 0; b < 7; b++) v = (v << 1) | bitbuf[c + b];
            out += (v >= 32 && v < 127) ? (char)v : '?';
        }
    }
    else
    {
        for (int c = 0; c + 8 <= nbits && nb < 32; c += 8)
        {
            int v = 0;
            for (int b = 0; b < 8; b++) v = (v << 1) | bitbuf[c + b];
            bytes[nb++] = (unsigned char)v;
        }
        if (format == 1)        // ISO 8 bit (Latin-1) -> UTF-8
        {
            for (int i = 0; i < nb && (int)i < len; i++)
            {
                const unsigned char v = bytes[i];
                if (v < 32) continue;
                if (v < 128) out += (char)v;
                else { out += (char)(0xC0 | (v >> 6)); out += (char)(0x80 | (v & 0x3F)); }
            }
        }
        else if (format == 2)   // UTF-8 (len = characters, the bytes are cut at the end)
        {
            for (int i = 0; i < nb; i++) if (bytes[i] >= 32 || bytes[i] >= 0x80) out += (char)bytes[i];
        }
        else                    // UTF-16 BE (basic plane only)
        {
            for (int i = 0; i + 1 < nb && (int)out.size() < 3 * len; i += 2)
            {
                const unsigned int v = (bytes[i] << 8) | bytes[i + 1];
                if (v < 32) continue;
                if (v < 0x80) out += (char)v;
                else if (v < 0x800) { out += (char)(0xC0 | (v >> 6)); out += (char)(0x80 | (v & 0x3F)); }
                else { out += (char)(0xE0 | (v >> 12)); out += (char)(0x80 | ((v >> 6) & 0x3F)); out += (char)(0x80 | (v & 0x3F)); }
            }
        }
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == 0)) out.pop_back();
    strncpy(si.alias, out.c_str(), sizeof(si.alias) - 1);
    si.alias[sizeof(si.alias) - 1] = 0;
}

void DSDDMR::handleLC(const unsigned char *lc, int slot, int origin)
{
    SlotInfo& si = m_slotInfo[slot & 1];
    const int flco = lc[0] & 0x3F;
    const int fid = lc[1];

    if (flco == 0 || flco == 3)   // group / unit to unit voice channel user
    {
        const unsigned int src = u24(&lc[6]), dst = u24(&lc[3]);
        const bool newCall = origin == 0 || !si.haveLC || si.callEnded || src != si.src || dst != si.dst;
        if (newCall && origin != 2)
        {
            si.aliasMask = 0;
            si.alias[0] = 0;
            si.haveGps = false;
            if (origin != 0 || si.callEnded) si.havePI = false;   // a PI header follows the voice LC header
        }
        si.haveLC = true;
        si.flco = flco;
        si.fid = fid;
        si.group = flco == 0;
        si.src = src;
        si.dst = dst;
        si.svcOpts = lc[2];
        si.emergency = (lc[2] & 0x80) != 0;
        si.privacy = (lc[2] & 0x40) != 0;
        si.broadcast = (lc[2] & 0x08) != 0;
        si.ovcm = (lc[2] & 0x04) != 0;
        si.priority = lc[2] & 0x03;
        si.callEnded = origin == 2;
        if (origin == 0) si.havePI = false;
        si.seq++;
        return;
    }
    if (fid != 0) return;   // manufacturer specific
    if (flco >= 4 && flco <= 7)   // talker alias header / blocks 1..3
    {
        const int blk = flco - 4;
        memcpy(si.aliasRaw[blk], &lc[2], 7);
        if (blk == 0) si.aliasMask = 1;   // a new header starts the alias again
        else si.aliasMask |= 1u << blk;
        updateAlias(si);
        si.seq++;
    }
    else if (flco == 8)   // GPS info
    {
        si.posErr = (lc[2] >> 1) & 7;
        int lon = ((lc[2] & 1) << 24) | (lc[3] << 16) | (lc[4] << 8) | lc[5];
        int lat = (lc[6] << 16) | (lc[7] << 8) | lc[8];
        if (lon & 0x1000000) lon -= 0x2000000;
        if (lat & 0x800000) lat -= 0x1000000;
        si.lon = lon * 360.0 / 33554432.0;
        si.lat = lat * 180.0 / 16777216.0;
        si.haveGps = si.posErr != 7 && std::abs(si.lat) <= 90.0 && std::abs(si.lon) <= 180.0;
        si.seq++;
    }
}

void DSDDMR::processDataBurst()
{
    if (!m_slotTypeOK) return;
    const int slot = (m_slot == DSDDMRSlot2) ? 1 : 0;
    SlotInfo& si = m_slotInfo[slot];
    si.lastDataType = (int)m_dataType;
    m_syncCount++;
    unsigned char b[12];
    // a data transmission goes on only with its blocks (idle bursts may come between them)
    if (m_dataType != DSDDMRDataDataHeader && m_dataType != DSDDMRDataRate_1_2_Data && m_dataType != DSDDMRDataIdle)
        m_dataAsm[slot] = DataAssembly();
    switch (m_dataType)
    {
    case DSDDMRDataPIHeader:
        bptc19696(m_dataBits, b);
        if (crcCcittOk(b, 0x6969))
        {
            si.crcOk++; m_dsdDecoder->crcPassed(4);
            si.havePI = true;
            si.algId = b[0];
            si.piFid = b[1];
            si.keyId = b[2];
            si.mi = ((unsigned int)b[3] << 24) | ((unsigned int)b[4] << 16) | ((unsigned int)b[5] << 8) | b[6];
            si.seq++;
        }
        break;
    case DSDDMRDataVoiceLCHeader:
        bptc19696(m_dataBits, b);
        if (rs129Ok(b, 0x96)) { si.lcOk++; si.crcOk++; m_dsdDecoder->crcPassed(4); handleLC(b, slot, 0); }
        else si.lcBad++;
        break;
    case DSDDMRDataTerminatorWithLC:
        bptc19696(m_dataBits, b);
        if (rs129Ok(b, 0x99)) { si.lcOk++; si.crcOk++; m_dsdDecoder->crcPassed(4); handleLC(b, slot, 2); }
        else si.lcBad++;
        si.callEnded = true;
        si.seq++;
        break;
    case DSDDMRDataCSBK:
        bptc19696(m_dataBits, b);
        if (crcCcittOk(b, 0xA5A5))
        {
            si.crcOk++; m_dsdDecoder->crcPassed(4);
            si.csbkOpcode = b[0] & 0x3F;
            si.csbkFid = b[1];
            si.csbkCount++;
            if (si.csbkFid == 0 && (si.csbkOpcode == 0x3D || si.csbkOpcode == 0x04 || si.csbkOpcode == 0x05))
            {
                si.csbkDst = u24(&b[4]);
                si.csbkSrc = u24(&b[7]);
            }
            si.seq++;
        }
        break;
    case DSDDMRDataDataHeader:
        m_dataAsm[slot] = DataAssembly();
        if (bptc19696(m_dataBits, b) && crcCcittOk(b, 0xCCCC))
        {
            si.crcOk++; m_dsdDecoder->crcPassed(4);
            parseDataHeader(b, slot);
            si.dataSeq++;
            si.seq++;
        }
        break;
    case DSDDMRDataRate_1_2_Data:
        if (bptc19696(m_dataBits, b))
        {
            if (m_dataAsm[slot].udt) parseUdtBlock(b, slot);
            else parsePacketBlock(b, slot);
        }
        else m_dataAsm[slot] = DataAssembly();
        break;
    default:
        break;
    }
}

} // namespace DSDcc
