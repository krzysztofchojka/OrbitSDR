///////////////////////////////////////////////////////////////////////////////////
// OrbitSDR additions to the DMR decoder of DSDcc (GPL v3, like DSDcc).          //
//                                                                               //
// DSDcc decodes the voice and the addresses of the embedded LC. Added here:     //
//  - the 5 bit checksum of the embedded LC, and its FLCO: talker alias and GPS  //
//    blocks no longer overwrite the addresses,                                  //
//  - service options: emergency, privacy (encrypted), broadcast, priority,      //
//  - the data bursts (BPTC 196,96): voice LC header, terminator with LC,        //
//    privacy (PI) header (algorithm, key id), CSBK, data header.                //
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
    static int table[16];
    static bool built = false;
    if (!built)
    {
        for (int i = 0; i < 16; i++) table[i] = -1;
        unsigned char t[N];
        for (int b = 0; b < N; b++)
        {
            memset(t, 0, N);
            t[b] = 1;
            table[S(t)] = b;
        }
        built = true;
    }
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

} // namespace

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
    switch (m_dataType)
    {
    case DSDDMRDataPIHeader:
        bptc19696(m_dataBits, b);
        if (crcCcittOk(b, 0x6969))
        {
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
        if (rs129Ok(b, 0x96)) { si.lcOk++; handleLC(b, slot, 0); }
        else si.lcBad++;
        break;
    case DSDDMRDataTerminatorWithLC:
        bptc19696(m_dataBits, b);
        if (rs129Ok(b, 0x99)) { si.lcOk++; handleLC(b, slot, 2); }
        else si.lcBad++;
        si.callEnded = true;
        si.seq++;
        break;
    case DSDDMRDataCSBK:
        bptc19696(m_dataBits, b);
        if (crcCcittOk(b, 0xA5A5))
        {
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
        bptc19696(m_dataBits, b);
        if (crcCcittOk(b, 0xCCCC))
        {
            si.dataDst = u24(&b[2]);
            si.dataSrc = u24(&b[5]);
            si.seq++;
        }
        break;
    default:
        break;
    }
}

} // namespace DSDcc
