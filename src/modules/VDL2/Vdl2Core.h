#pragma once

// =====================================================================
//  VDL Mode 2 (aircraft datalink, D8PSK 10500 symbols/s, 31.5 kbit/s,
//  25 kHz channels around 136.975 MHz).
//
//  Burst structure and decoding follow dumpvdl2 by Tomasz Lemiech
//  (GNU GPL v3, the same as OrbitSDR); the Reed-Solomon decoder is
//  Phil Karn's (KA9Q) libfec (LGPL). Re-written in C++ here:
//
//   preamble (16 symbols) -> header: 3 reserved bits + 17 bit length
//   (bit reversed) + 5 bit FEC (corrects 1 bit) -> data octets and RS
//   parity octets (RS(255,249) over GF(256), blocks interleaved),
//   everything after the preamble scrambled (x^15 + x + 1) ->
//   HDLC frames (flags 0x7E, bit stuffing, LSB first) -> AVLC frames
//   with FCS -> ACARS (I frames starting with FF FF 01), XID (link
//   management with aircraft / ground station positions), X.25 / ATN.
//
//  The demodulator is dumpvdl2's: 10 samples per symbol (105 kS/s),
//  preamble phase matching with a linear frequency fit, then one
//  differential phase decision per symbol.
// =====================================================================

#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <complex>
#include <functional>
#include <algorithm>
#include <array>
#include <cstdio>
#include "../ACARS/AcarsCore.h"   // acars::Message, labelDescription()

namespace vdl2 {

constexpr int SPS = 10;
constexpr int SYMBOL_RATE = 10500;
constexpr int SAMPLE_RATE = SPS * SYMBOL_RATE;   // 105 kS/s
constexpr int PREAMBLE_SYMS = 16;
constexpr int SYNC_BUFLEN = PREAMBLE_SYMS * SPS;
constexpr int RS_K = 249, RS_N = 255;
constexpr int TRLEN = 17, HDRFECLEN = 5;
constexpr int HEADER_LEN = 3 + TRLEN + HDRFECLEN;
constexpr uint16_t LFSR_IV = 0x6959u;
constexpr int SYNC_SKIP = 3;
constexpr float SYNC_THRESHOLD = 4.0f;
constexpr float PHERR_MAX = 1000.0f;
constexpr uint32_t MAX_FRAME_LENGTH = 0x3FFF, MAX_FRAME_LENGTH_CORRECTED = 0x1FFF;

inline uint32_t ones(int n) { return n >= 32 ? 0xFFFFFFFFu : ((1u << n) - 1u); }

inline uint32_t reverseBits(uint32_t v, int n) {
    uint32_t r = 0;
    for (int i = 0; i < n; i++) { r = (r << 1) | (v & 1u); v >>= 1; }
    return r;
}

inline int parity(uint32_t v) {
    int p = 0;
    while (v) { p ^= 1; v &= v - 1; }
    return p;
}

// --- header FEC ---
inline const uint32_t* headerH() {
    static const uint32_t H[HDRFECLEN] = {
        0b0000000011111111111110000u, 0b0011111100001111111101000u, 0b1100011100110000111100100u,
        0b1101101101010011001100010u, 0b0110100111100101010100001u };
    return H;
}
inline const uint32_t* syndTable() {
    static const uint32_t t[1 << HDRFECLEN] = {
        0b0000000000000000000000000u, 0b0000000000000000000000001u, 0b0000000000000000000000010u, 0b0100000000000000000000100u,
        0b0000000000000000000000100u, 0b0100000000000000000000010u, 0b1000000000000000000000000u, 0b0100000000000000000000000u,
        0b0000000000000000000001000u, 0b0010000000000000000000000u, 0b0001000000000000000000000u, 0b0000100000000000000000000u,
        0b0000010000000000000000000u, 0b1000100000000000000000000u, 0b0000001000000000000000000u, 0b0000000100000000000000000u,
        0b0000000000000000000010000u, 0b0000000010000000000000000u, 0b0100000000100000000000000u, 0b0000000001000000000000000u,
        0b0100000001000000000000000u, 0b0000000000100000000000000u, 0b0000000000010000000000000u, 0b1000000010000000000000000u,
        0b0000000000001000000000000u, 0b0000000000000100000000000u, 0b0000000000000010000000000u, 0b0000000000000001000000000u,
        0b0000000000000000100000000u, 0b0000000000000000010000000u, 0b0000000000000000001000000u, 0b0000000000000000000100000u };
    return t;
}
inline uint32_t headerSyndrome(uint32_t r) {
    uint32_t s = 0;
    for (int i = 0; i < HDRFECLEN; i++) s |= (uint32_t)parity(r & headerH()[i]) << (HDRFECLEN - 1 - i);
    return s;
}

// --- CRC-16-CCITT (reflected, as used by HDLC / AVLC and ACARS) ---
inline uint16_t crc16(const uint8_t* d, size_t n, uint16_t crc) {
    // built once; a function-local static object is initialised thread-safely (the channels
    // of a decoder run in parallel on the WorkPool)
    struct Table {
        uint16_t v[256];
        Table() {
            for (int i = 0; i < 256; i++) {
                uint16_t c = (uint16_t)i;
                for (int k = 0; k < 8; k++) c = (c & 1) ? (uint16_t)((c >> 1) ^ 0x8408) : (uint16_t)(c >> 1);
                v[i] = c;
            }
        }
    };
    static const Table table;
    while (n--) crc = (uint16_t)((crc >> 8) ^ table.v[(crc ^ *d++) & 0xff]);
    return crc;
}

// --- Reed-Solomon (255,249) over GF(2^8), poly 0x187, fcr 120, prim 1 (libfec) ---
class ReedSolomon {
    static constexpr int MM = 8, NN = 255, NROOTS = RS_N - RS_K, FCR = 120, PRIM = 1, A0 = NN;
    uint8_t alphaTo[NN + 1], indexOf[NN + 1], genpoly[NROOTS + 1];
    int iprim = 1;
    static int modnn(int x) {
        while (x >= NN) { x -= NN; x = (x >> MM) + (x & NN); }
        return x;
    }

public:
    ReedSolomon() {
        indexOf[0] = A0;
        alphaTo[A0] = 0;
        int sr = 1;
        for (int i = 0; i < NN; i++) {
            indexOf[sr] = (uint8_t)i;
            alphaTo[i] = (uint8_t)sr;
            sr <<= 1;
            if (sr & (1 << MM)) sr ^= 0x187;
            sr &= NN;
        }
        for (iprim = 1; (iprim % PRIM) != 0; iprim += NN) {}
        iprim /= PRIM;
        genpoly[0] = 1;
        for (int i = 0, root = FCR * PRIM; i < NROOTS; i++, root += PRIM) {
            genpoly[i + 1] = 1;
            for (int j = i; j > 0; j--) {
                if (genpoly[j] != 0) genpoly[j] = genpoly[j - 1] ^ alphaTo[modnn(indexOf[genpoly[j]] + root)];
                else genpoly[j] = genpoly[j - 1];
            }
            genpoly[0] = alphaTo[modnn(indexOf[genpoly[0]] + root)];
        }
        for (int i = 0; i <= NROOTS; i++) genpoly[i] = indexOf[genpoly[i]];
    }

    // data[0..248] -> parity[0..5] (libfec encode_rs)
    void encode(const uint8_t* data, uint8_t* bb) const {
        std::memset(bb, 0, NROOTS);
        for (int i = 0; i < NN - NROOTS; i++) {
            const uint8_t fb = indexOf[data[i] ^ bb[0]];
            if (fb != A0) for (int j = 1; j < NROOTS; j++) bb[j] ^= alphaTo[modnn(fb + genpoly[NROOTS - j])];
            std::memmove(&bb[0], &bb[1], NROOTS - 1);
            bb[NROOTS - 1] = fb != A0 ? alphaTo[modnn(fb + genpoly[0])] : 0;
        }
    }

    // Corrects data[0..254] in place. Returns the number of corrected symbols, -1 = uncorrectable.
    int decode(uint8_t* data, const int* erasPos, int noEras) const {
        int degLambda, el, degOmega, i, j, r, k;
        uint8_t u, q, tmp, num1, num2, den, discrR;
        uint8_t lambda[NROOTS + 1], s[NROOTS], b[NROOTS + 1], t[NROOTS + 1], omega[NROOTS + 1];
        uint8_t root[NROOTS], reg[NROOTS + 1], loc[NROOTS];
        int count;
        for (i = 0; i < NROOTS; i++) s[i] = data[0];
        for (j = 1; j < NN; j++)
            for (i = 0; i < NROOTS; i++)
                s[i] = s[i] == 0 ? data[j] : (uint8_t)(data[j] ^ alphaTo[modnn(indexOf[s[i]] + (FCR + i) * PRIM)]);
        int synError = 0;
        for (i = 0; i < NROOTS; i++) { synError |= s[i]; s[i] = indexOf[s[i]]; }
        if (!synError) return 0;
        std::memset(&lambda[1], 0, NROOTS);
        lambda[0] = 1;
        if (noEras > 0) {
            lambda[1] = alphaTo[modnn(PRIM * (NN - 1 - erasPos[0]))];
            for (i = 1; i < noEras; i++) {
                u = (uint8_t)modnn(PRIM * (NN - 1 - erasPos[i]));
                for (j = i + 1; j > 0; j--) {
                    tmp = indexOf[lambda[j - 1]];
                    if (tmp != A0) lambda[j] ^= alphaTo[modnn(u + tmp)];
                }
            }
        }
        for (i = 0; i < NROOTS + 1; i++) b[i] = indexOf[lambda[i]];
        r = noEras;
        el = noEras;
        while (++r <= NROOTS) {
            discrR = 0;
            for (i = 0; i < r; i++)
                if (lambda[i] != 0 && s[r - i - 1] != A0) discrR ^= alphaTo[modnn(indexOf[lambda[i]] + s[r - i - 1])];
            discrR = indexOf[discrR];
            if (discrR == A0) {
                std::memmove(&b[1], b, NROOTS);
                b[0] = A0;
            } else {
                t[0] = lambda[0];
                for (i = 0; i < NROOTS; i++)
                    t[i + 1] = b[i] != A0 ? (uint8_t)(lambda[i + 1] ^ alphaTo[modnn(discrR + b[i])]) : lambda[i + 1];
                if (2 * el <= r + noEras - 1) {
                    el = r + noEras - el;
                    for (i = 0; i <= NROOTS; i++)
                        b[i] = lambda[i] == 0 ? (uint8_t)A0 : (uint8_t)modnn(indexOf[lambda[i]] - discrR + NN);
                } else {
                    std::memmove(&b[1], b, NROOTS);
                    b[0] = A0;
                }
                std::memcpy(lambda, t, NROOTS + 1);
            }
        }
        degLambda = 0;
        for (i = 0; i < NROOTS + 1; i++) {
            lambda[i] = indexOf[lambda[i]];
            if (lambda[i] != A0) degLambda = i;
        }
        std::memcpy(&reg[1], &lambda[1], NROOTS);
        count = 0;
        for (i = 1, k = iprim - 1; i <= NN; i++, k = modnn(k + iprim)) {
            q = 1;
            for (j = degLambda; j > 0; j--)
                if (reg[j] != A0) { reg[j] = (uint8_t)modnn(reg[j] + j); q ^= alphaTo[reg[j]]; }
            if (q != 0) continue;
            root[count] = (uint8_t)i;
            loc[count] = (uint8_t)k;
            if (++count == degLambda) break;
        }
        if (degLambda != count) return -1;
        degOmega = degLambda - 1;
        for (i = 0; i <= degOmega; i++) {
            tmp = 0;
            for (j = i; j >= 0; j--)
                if (s[i - j] != A0 && lambda[j] != A0) tmp ^= alphaTo[modnn(s[i - j] + lambda[j])];
            omega[i] = indexOf[tmp];
        }
        for (j = count - 1; j >= 0; j--) {
            num1 = 0;
            for (i = degOmega; i >= 0; i--)
                if (omega[i] != A0) num1 ^= alphaTo[modnn(omega[i] + i * root[j])];
            num2 = alphaTo[modnn(root[j] * (FCR - 1) + NN)];
            den = 0;
            for (i = std::min(degLambda, NROOTS - 1) & ~1; i >= 0; i -= 2)
                if (lambda[i + 1] != A0) den ^= alphaTo[modnn(lambda[i + 1] + i * root[j])];
            if (den == 0) return -1;
            if (num1 != 0) data[loc[j]] ^= alphaTo[modnn(indexOf[num1] + indexOf[num2] + NN - indexOf[den])];
        }
        return count;
    }
};

inline const ReedSolomon& rs() { static const ReedSolomon r; return r; }

inline int fecOctetCount(uint32_t len) { return len < 3 ? 0 : len < 31 ? 2 : len < 68 ? 4 : 6; }

// dumpvdl2 deinterleave(): `in` (column by column) -> table rows
inline bool deinterleave(const uint8_t* in, uint32_t len, uint32_t rows, std::vector<std::array<uint8_t, RS_N>>& out,
                         uint32_t fillwidth, uint32_t offset) {
    if (rows == 0 || fillwidth == 0) return false;
    uint32_t lastRowLen = len % fillwidth;
    if (lastRowLen == 0) lastRowLen = fillwidth;
    if (fillwidth + offset > (uint32_t)RS_N) return false;
    if (len > rows * fillwidth) return false;
    if (rows > 1 && len - lastRowLen < (rows - 1) * fillwidth) return false;
    uint32_t row = 0, col = offset;
    lastRowLen += offset;
    for (uint32_t i = 0; i < len; i++) {
        if (row == rows - 1 && col >= lastRowLen) { out[row][col] = 0; row = 0; col++; }
        out[row++][col] = in[i];
        if (row == rows) { row = 0; col++; }
    }
    return true;
}

// The inverse (for the test signal generator)
inline void interleave(const std::vector<std::array<uint8_t, RS_N>>& tab, uint32_t len, uint32_t rows, uint32_t fillwidth,
                       uint32_t offset, std::vector<uint8_t>& out) {
    uint32_t lastRowLen = len % fillwidth;
    if (lastRowLen == 0) lastRowLen = fillwidth;
    uint32_t row = 0, col = offset;
    lastRowLen += offset;
    for (uint32_t i = 0; i < len; i++) {
        if (row == rows - 1 && col >= lastRowLen) { row = 0; col++; }
        out.push_back(tab[row++][col]);
        if (row == rows) { row = 0; col++; }
    }
}

// HDLC: the next frame between flags, bit stuffing removed. Returns 1 = more data follows,
// 0 = the last frame, -1 = error.
inline int nextFrame(const std::vector<uint8_t>& src, size_t& start, size_t end, std::vector<uint8_t>& dst) {
restart:
    int ones_ = 0;
    dst.clear();
    size_t j = 0;
    size_t i;
    for (i = start; i < end; i++, start++) {
        const uint8_t bit = src[i];
        if (bit == 0 && ones_ == 5) { ones_ = 0; continue; }
        if (bit == 1 && ++ones_ > 6) return -1;
        dst.push_back(bit);
        if (bit == 0) {
            if (ones_ == 6) {
                if (j == 7) { start++; goto restart; }   // the opening flag
                if (j < 7) return -1;
                dst.resize(j - 7);                     // drop the closing flag
                start++;
                break;
            }
            ones_ = 0;
        }
        j++;
    }
    return start < end ? 1 : 0;
}

// ---------------------------------------------------------------------
//  Decoded frame
// ---------------------------------------------------------------------
enum AddrType { ADDR_AIRCRAFT = 1, ADDR_GS_ADM = 4, ADDR_GS_DEL = 5, ADDR_ALL = 7 };

inline const char* addrTypeName(int t) {
    switch (t) {
    case ADDR_AIRCRAFT: return "Aircraft";
    case ADDR_GS_ADM: case ADDR_GS_DEL: return "Ground station";
    case ADDR_ALL: return "All stations";
    default: return "reserved";
    }
}

struct Frame {
    uint32_t src = 0, dst = 0;   // 24 bit addresses (aircraft: ICAO address)
    int srcType = 0, dstType = 0;
    bool onGround = false;       // the A/G bit (dst status)
    bool response = false;       // C/R bit (src status)
    std::string kind;            // "I", "S RR", "U XID", ...
    std::string proto;           // "ACARS", "XID", "X.25", "-"
    bool hasAcars = false;
    acars::Message acars;
    bool acarsCrcOk = false;
    // XID parameters
    bool hasAcPos = false, hasGsPos = false;
    double acLat = 0, acLon = 0, gsLat = 0, gsLon = 0;
    int acAltFt = 0;
    std::string dstAirport;
    std::string summary;         // one line for the table
    std::vector<uint8_t> raw;    // AVLC frame without FCS
};

inline uint32_t parseDlcAddr(const uint8_t* b) {
    return reverseBits((uint32_t)(b[0] >> 1) | ((uint32_t)b[1] << 6) | ((uint32_t)b[2] << 13) | (((uint32_t)b[3] & 0xfe) << 20), 28) & ones(28);
}

inline void parseLocation(const uint8_t* b, double& lat, double& lon) {
    int la = (int)(((uint16_t)b[0] << 8 | b[1]) >> 4);
    int lo = (int)(((uint16_t)b[1] << 8 | b[2]) & 0xfff);
    if (la & 0x800) la -= 0x1000;   // 12 bit signed
    if (lo & 0x800) lo -= 0x1000;
    lat = la / 10.0;
    lon = lo / 10.0;
}

inline void parseXid(const uint8_t* p, size_t len, Frame& f) {
    if (len < 7 || p[0] != 0x82) return;
    size_t pos = 1;
    while (pos + 3 <= len) {
        const uint8_t gid = p[pos];
        const size_t glen = ((size_t)p[pos + 1] << 8) | p[pos + 2];
        pos += 3;
        if (pos + glen > len) return;
        if (gid == 0xF0 || gid == 0x80) {   // VDL private / public parameters (1 octet lengths)
            size_t q = pos;
            const size_t qend = pos + glen;
            while (q + 2 <= qend) {
                const uint8_t code = p[q];
                const size_t tl = p[q + 1];
                q += 2;
                if (tl == 0 || q + tl > qend) break;
                if (gid == 0xF0) {
                    if (code == 0x84 && tl >= 4) {   // aircraft location
                        parseLocation(p + q, f.acLat, f.acLon);
                        f.acAltFt = p[q + 3] * 1000;
                        f.hasAcPos = true;
                    } else if (code == 0xC8 && tl >= 3) {   // ground station location
                        parseLocation(p + q, f.gsLat, f.gsLon);
                        f.hasGsPos = true;
                    } else if (code == 0x83) {   // destination airport
                        f.dstAirport.assign((const char*)p + q, tl);
                    }
                }
                q += tl;
            }
        }
        pos += glen;
    }
}

// ACARS inside an AVLC I frame (after FF FF 01): libacars la_acars_parse()
inline bool parseAcars(const uint8_t* buf, size_t len, Frame& f) {
    if (len < 16 || buf[len - 1] != 0x7f) return false;   // DEL at the end
    len--;
    f.acarsCrcOk = crc16(buf, len, 0) == 0;
    len -= 2;
    std::vector<char> b(len);
    for (size_t i = 0; i < len; i++) b[i] = (char)(buf[i] & 0x7f);
    acars::Message& m = f.acars;
    if (b[len - 1] == 0x03) m.more = false;
    else if (b[len - 1] == 0x17) m.more = true;
    else return false;
    len--;
    size_t p = 0;
    m.mode = b[p++];
    std::string reg(b.data() + p, 7);
    p += 7;
    while (!reg.empty() && reg.front() == '.') reg.erase(reg.begin());
    m.reg = reg;
    m.ack = b[p++];
    if (m.ack == 0x15) m.ack = '!';
    else if (m.ack == 0x06) m.ack = '^';
    m.label.assign(b.data() + p, 2);
    p += 2;
    if (m.label[1] == 0x7f) m.label[1] = 'd';
    m.blockId = b[p++];
    if (m.blockId == 0) m.blockId = ' ';
    m.downlink = m.blockId >= '0' && m.blockId <= '9';
    if (p < len) {
        if (b[p] != 0x02) return false;   // STX
        p++;
        std::string text(b.data() + p, len - p);
        for (char& c : text) if (c == 0) c = '.';
        if (m.downlink && text.size() >= 10) {
            m.msn = text.substr(0, 4);
            m.flight = text.substr(4, 6);
            while (!m.flight.empty() && (m.flight.back() == ' ' || m.flight.back() == '.')) m.flight.pop_back();
            text = text.substr(10);
        }
        m.text = text;
    }
    f.hasAcars = true;
    return true;
}

// Parses one AVLC frame (with FCS). False when the FCS is wrong.
inline bool parseAvlc(const uint8_t* buf, size_t len, Frame& f) {
    if (len < 11) return false;
    if (crc16(buf, len, 0xFFFFu) != 0xF0B8u) return false;
    len -= 2;
    f.raw.assign(buf, buf + len);
    const uint32_t d = parseDlcAddr(buf), s = parseDlcAddr(buf + 4);
    f.dst = d & 0xFFFFFF; f.dstType = (d >> 24) & 7; f.onGround = (d >> 27) & 1;
    f.src = s & 0xFFFFFF; f.srcType = (s >> 24) & 7; f.response = (s >> 27) & 1;
    const uint8_t lcf = buf[8];
    const uint8_t* p = buf + 9;
    const size_t n = len - 9;
    char tmp[160];
    if ((lcf & 1) == 0) {
        f.kind = "I";
        if (n > 3 && p[0] == 0xff && p[1] == 0xff && p[2] == 0x01) {
            f.proto = "ACARS";
            if (!parseAcars(p + 3, n - 3, f)) f.proto = "ACARS (bad)";
        } else {
            f.proto = "X.25";
        }
    } else if ((lcf & 3) == 1) {
        static const char* sNames[] = {"RR", "RNR", "REJ", "SREJ"};
        f.kind = std::string("S ") + sNames[(lcf >> 2) & 3];
        f.proto = "-";
    } else {
        const int mfunc = (lcf >> 2) & 0x3b;
        const char* name = mfunc == 0x00 ? "UI" : mfunc == 0x03 ? "DM" : mfunc == 0x10 ? "DISC" : mfunc == 0x18 ? "UA"
                         : mfunc == 0x21 ? "FRMR" : mfunc == 0x2b ? "XID" : mfunc == 0x38 ? "TEST" : "?";
        f.kind = std::string("U ") + name;
        f.proto = "-";
        if (mfunc == 0x2b) { f.proto = "XID"; parseXid(p, n, f); }
    }
    // summary line
    if (f.hasAcars) {
        const acars::Message& m = f.acars;
        std::snprintf(tmp, sizeof(tmp), "ACARS %s%s%s label %s", m.reg.empty() ? "-" : m.reg.c_str(),
                      m.flight.empty() ? "" : " ", m.flight.c_str(), m.label.c_str());
        f.summary = tmp;
        if (!m.text.empty()) {
            std::string t = m.text;
            for (char& c : t) if (c == '\n' || c == '\r' || c == '\t') c = ' ';
            f.summary += " | " + t;
        } else if (*acars::labelDescription(m.label)) f.summary += std::string(" (") + acars::labelDescription(m.label) + ")";
    } else if (f.proto == "XID") {
        f.summary = "XID";
        if (f.hasAcPos) { std::snprintf(tmp, sizeof(tmp), "  aircraft at %.1f %.1f, %d ft", f.acLat, f.acLon, f.acAltFt); f.summary += tmp; }
        if (!f.dstAirport.empty()) f.summary += "  to " + f.dstAirport;
        if (f.hasGsPos) { std::snprintf(tmp, sizeof(tmp), "  ground station at %.1f %.1f", f.gsLat, f.gsLon); f.summary += tmp; }
    } else if (f.proto == "X.25") {
        std::snprintf(tmp, sizeof(tmp), "X.25 / ATN, %zu octets (CPDLC, CM ... not decoded)", n);
        f.summary = tmp;
    } else {
        f.summary = f.kind;
    }
    return true;
}

// ---------------------------------------------------------------------
//  Demodulator + burst decoder for one channel (input: 105 kS/s)
// ---------------------------------------------------------------------
class Receiver {
    enum class DState { INIT, SYNC };
    enum class Dec { HEADER, DATA, IDLE };
    DState dstate = DState::INIT;
    Dec dec = Dec::HEADER;
    float syncbuf[SYNC_BUFLEN] = {0};
    int syncbufidx = 0;
    int sclk = 0;
    float pherr[3] = {PHERR_MAX, PHERR_MAX, PHERR_MAX};
    float prevPhi = 0, dphi = 0, prevDphi = 0;
    float lrX[PREAMBLE_SYMS];
    float lrDenom = 0;
    std::vector<uint8_t> bits;
    size_t bitStart = 0, descPos = 0;
    uint16_t lfsr = LFSR_IV;
    uint32_t requested = HEADER_LEN;
    uint32_t datalen = 0, datalenOctets = 0, numBlocks = 0, fecOctets = 0, lastBlockLen = 0, syndrome = 0;
    float framePwr = 0;
    int framePwrCnt = 0;
    float magLp = 0, magNf = 0.01f;
    int nfcnt = 0;

    void decoderReset() { dec = Dec::HEADER; requested = HEADER_LEN; bits.clear(); bitStart = 0; descPos = 0; }
    void demodReset() {
        decoderReset();
        sclk = 0;
        dstate = DState::INIT;
        pherr[1] = pherr[2] = PHERR_MAX;
        framePwr = 0; framePwrCnt = 0;
    }

    void descramble() {
        if (descPos < bitStart) descPos = bitStart;
        for (size_t i = descPos; i < bits.size(); i++) {
            const uint8_t bit = (uint8_t)(((lfsr >> 0) ^ (lfsr >> 14)) & 1);
            lfsr = (uint16_t)((lfsr >> 1) | (bit << 14));
            bits[i] ^= bit;
        }
        descPos = bits.size();
    }
    bool readWordMsb(uint32_t& w, int n) {
        if (bitStart + n > bits.size()) return false;
        w = 0;
        for (int i = 0; i < n; i++) w |= (uint32_t)(bits[bitStart++] & 1) << (n - i - 1);
        return true;
    }
    bool readLsb(uint8_t* out, uint32_t numBytes) {
        if (bitStart + 8ull * numBytes > bits.size()) return false;
        for (uint32_t i = 0; i < numBytes; i++) {
            out[i] = 0;
            for (int j = 0; j < 8; j++) out[i] |= (uint8_t)((bits[bitStart++] & 1) << j);
        }
        return true;
    }

    static float paraVertex(float x, int d, float y1, float y2, float y3) {
        const float denom = (float)(d * 2 * d * (-d));
        const float A = (x * (y2 - y1) + (x - d) * (y1 - y3) + (x - 2 * d) * (y3 - y2)) / denom;
        const float B = (x * x * (y1 - y2) + (x - d) * (x - d) * (y3 - y1) + (x - 2 * d) * (x - 2 * d) * (y2 - y3)) / denom;
        return -B / (2 * A);
    }

    bool gotSync() {
        static const float PI_F = 3.14159265358979f;
        static const float prPhase[PREAMBLE_SYMS] = {
            0 * PI_F / 4, 3 * PI_F / 4, -3 * PI_F / 4, 1 * PI_F / 4, 1 * PI_F / 4, 2 * PI_F / 4, 0 * PI_F / 4, 4 * PI_F / 4,
            -3 * PI_F / 4, 4 * PI_F / 4, -2 * PI_F / 4, 3 * PI_F / 4, 1 * PI_F / 4, -2 * PI_F / 4, -3 * PI_F / 4, 0 * PI_F / 4 };
        float errvec[PREAMBLE_SYMS];
        float mean = 0, unwrap = 0;
        float prevErr = mean = errvec[0] = syncbuf[(syncbufidx + SPS) % SYNC_BUFLEN] - prPhase[0];
        for (int i = 1; i < PREAMBLE_SYMS; i++) {
            const float cur = syncbuf[(syncbufidx + (i + 1) * SPS) % SYNC_BUFLEN] - prPhase[i];
            const float diff = cur - prevErr;
            prevErr = cur;
            if (diff > PI_F) unwrap -= 2.0f * PI_F;
            else if (diff < -PI_F) unwrap += 2.0f * PI_F;
            errvec[i] = cur + unwrap;
            mean += errvec[i];
        }
        mean /= PREAMBLE_SYMS;
        for (float& e : errvec) e -= mean;
        float freqErr = 0;
        for (int i = 0; i < PREAMBLE_SYMS; i++) freqErr += lrX[i] * errvec[i];
        freqErr /= lrDenom;
        pherr[0] = 0;
        for (int i = 0; i < PREAMBLE_SYMS; i++) { const float e = errvec[i] - freqErr * lrX[i]; pherr[0] += e * e; }
        if (pherr[1] < SYNC_THRESHOLD && pherr[0] > pherr[1]) {
            const float vx = paraVertex((float)sclk, SYNC_SKIP, pherr[2], pherr[1], pherr[0]);
            sclk = -(int)std::lround(vx);
            int sp = syncbufidx - sclk;
            if (sp < 0) sp += SYNC_BUFLEN;
            sp %= SYNC_BUFLEN;
            prevPhi = syncbuf[sp];
            dphi = prevDphi;
            pherr[1] = pherr[2] = PHERR_MAX;
            return true;
        }
        pherr[2] = pherr[1];
        pherr[1] = pherr[0];
        prevDphi = freqErr;
        return false;
    }

    void decodeBurst() {
        if (dec == Dec::HEADER) {
            lfsr = LFSR_IV;
            descramble();
            uint32_t header;
            if (!readWordMsb(header, HEADER_LEN)) { dec = Dec::IDLE; return; }
            header &= ones(TRLEN + HDRFECLEN);
            syndrome = headerSyndrome(header);
            header ^= syndTable()[syndrome];
            if ((header & ones(TRLEN + HDRFECLEN)) != header) { dec = Dec::IDLE; return; }
            header >>= HDRFECLEN;
            datalen = reverseBits(header & ones(TRLEN), TRLEN);
            if ((syndrome != 0 && datalen > MAX_FRAME_LENGTH_CORRECTED) || datalen > MAX_FRAME_LENGTH) { dec = Dec::IDLE; return; }
            datalenOctets = (datalen + 7) / 8;
            numBlocks = datalenOctets / RS_K;
            fecOctets = numBlocks * (RS_N - RS_K);
            lastBlockLen = datalenOctets % RS_K;
            if (lastBlockLen != 0) numBlocks++;
            fecOctets += fecOctetCount(lastBlockLen);
            if (lastBlockLen == 0) lastBlockLen = RS_K;
            if (fecOctets == 0) { dec = Dec::IDLE; return; }
            requested = 8 * (datalenOctets + fecOctets);
            dec = Dec::DATA;
            return;
        }
        if (dec != Dec::DATA) return;
        descramble();
        dec = Dec::IDLE;
        std::vector<uint8_t> data(datalenOctets), fec(fecOctets);
        if (!readLsb(data.data(), datalenOctets) || !readLsb(fec.data(), fecOctets)) return;
        std::vector<std::array<uint8_t, RS_N>> tab(numBlocks);
        for (auto& r : tab) r.fill(0);
        if (!deinterleave(data.data(), datalenOctets, numBlocks, tab, RS_K, 0)) return;
        uint32_t fecRows = numBlocks;
        if (fecOctetCount(lastBlockLen) == 0) fecRows--;
        if (fecRows > 0 && !deinterleave(fec.data(), fecOctets, fecRows, tab, RS_N - RS_K, RS_K)) return;
        int corrected = 0;
        std::vector<uint8_t> stream;
        stream.reserve(datalen + 16);
        for (uint32_t r = 0; r < numBlocks; r++) {
            int nfec = RS_N - RS_K;
            if (r == numBlocks - 1) nfec = fecOctetCount(lastBlockLen);
            if (nfec > 0) {
                const int eras = RS_N - RS_K - nfec;
                int pos[RS_N - RS_K];
                for (int i = 0; i < eras; i++) pos[i] = RS_K + nfec + i;
                const int ret = rs().decode(tab[r].data(), eras > 0 ? pos : nullptr, eras);
                if (ret < 0) { stats.rsFail++; return; }
                if (ret > 0) corrected += ret - eras;
            }
            const uint32_t n = r != numBlocks - 1 ? RS_K : lastBlockLen;
            for (uint32_t i = 0; i < n; i++) for (int j = 0; j < 8; j++) stream.push_back((tab[r][i] >> j) & 1);
        }
        if (datalen < stream.size()) stream.resize(datalen);
        size_t start = 0;
        std::vector<uint8_t> fb;
        for (int guard = 0; guard < 64; guard++) {
            const int ret = nextFrame(stream, start, stream.size(), fb);
            if (ret < 0) return;
            if (fb.size() % 8 != 0) return;
            std::vector<uint8_t> bytes(fb.size() / 8, 0);
            for (size_t i = 0; i < bytes.size(); i++)
                for (int j = 0; j < 8; j++) bytes[i] |= (uint8_t)(fb[i * 8 + j] << j);
            Frame f;
            if (parseAvlc(bytes.data(), bytes.size(), f)) {
                stats.frames++;
                if (onFrame) onFrame(f, 10.0f * std::log10(std::max(1e-20f, framePwr)), corrected);
            } else stats.badFcs++;
            if (ret == 0) break;
        }
    }

public:
    struct Stats { int bursts = 0, frames = 0, rsFail = 0, badFcs = 0; } stats;
    std::function<void(const Frame&, float levelDb, int corrected)> onFrame;

    Receiver() {
        float m = 0;
        for (int i = 0; i < PREAMBLE_SYMS; i++) m += i;
        m /= PREAMBLE_SYMS;
        for (int i = 0; i < PREAMBLE_SYMS; i++) { lrX[i] = i - m; lrDenom += (i - m) * (i - m); }
        demodReset();
    }

    float noiseFloorDb() const { return 20.0f * std::log10(magNf + 1e-6f); }

    void process(const std::complex<float>* x, size_t n) {
        static const uint8_t gray[8] = {0, 1, 3, 2, 6, 7, 5, 4};
        static const float PI_F = 3.14159265358979f;
        for (size_t k = 0; k < n; k++) {
            const float re = x[k].real(), im = x[k].imag();
            if (dec == Dec::IDLE) demodReset();
            if (dstate == DState::INIT) {
                syncbufidx = (syncbufidx + 1) % SYNC_BUFLEN;
                syncbuf[syncbufidx] = std::atan2(im, re);
                if (++sclk < SYNC_SKIP) continue;
                sclk = 0;
                const float mag = std::hypot(re, im);
                magLp = magLp * 0.9f + mag * 0.1f;
                if (++nfcnt == 1000) { nfcnt = 0; magNf = 0.85f * magNf + 0.15f * std::min(magLp, magNf) + 1e-6f; }
                if (gotSync()) { dstate = DState::SYNC; stats.bursts++; }
                continue;
            }
            if (++sclk < SPS) continue;
            sclk = 0;
            const float phi = std::atan2(im, re);
            float d = phi - prevPhi - dphi;
            while (d < 0) d += 2.0f * PI_F;
            while (d > 2.0f * PI_F) d -= 2.0f * PI_F;
            d /= PI_F / 4.0f;
            const int idx = (int)std::lround(d) % 8;
            const float pw = re * re + im * im;
            framePwr = (framePwr * framePwrCnt + pw) / (framePwrCnt + 1);
            framePwrCnt++;
            prevPhi = phi;
            const uint8_t g = gray[idx];
            bits.push_back((g >> 2) & 1);
            bits.push_back((g >> 1) & 1);
            bits.push_back(g & 1);
            if (bits.size() > 200000) { demodReset(); continue; }
            if (bits.size() - bitStart >= requested) decodeBurst();
        }
    }
};

// ---------------------------------------------------------------------
//  Test signal generator (the inverse of the above), 105 kS/s complex
// ---------------------------------------------------------------------
inline std::vector<uint8_t> avlcFrame(uint32_t srcAddr, int srcType, uint32_t dstAddr, int dstType, bool onGround,
                                      uint8_t lcf, const std::vector<uint8_t>& info) {
    auto addr = [](uint32_t a, int type, int status, uint8_t* out) {
        const uint32_t v = (a & 0xFFFFFF) | ((uint32_t)type << 24) | ((uint32_t)status << 27);
        const uint32_t r = reverseBits(v, 28);
        out[0] = (uint8_t)((r & 0x7F) << 1);
        out[1] = (uint8_t)((r >> 7) & 0x7F) << 1;
        out[2] = (uint8_t)((r >> 14) & 0x7F) << 1;
        out[3] = (uint8_t)(((r >> 21) & 0x7F) << 1);
    };
    std::vector<uint8_t> f(9);
    addr(dstAddr, dstType, onGround ? 1 : 0, &f[0]);
    addr(srcAddr, srcType, 0, &f[4]);
    f[7] |= 1;   // address extension bit: set in the last octet of the address field
    f[8] = lcf;
    f.insert(f.end(), info.begin(), info.end());
    const uint16_t fcs = (uint16_t)~crc16(f.data(), f.size(), 0xFFFF);
    f.push_back((uint8_t)(fcs & 0xff));
    f.push_back((uint8_t)(fcs >> 8));
    return f;
}

inline std::vector<std::complex<float>> modulateBurst(const std::vector<std::vector<uint8_t>>& frames, float amplitude) {
    // HDLC bit stream (LSB first, stuffed), frames separated by flags
    std::vector<uint8_t> hd;
    auto flag = [&]() { for (int i = 0; i < 8; i++) hd.push_back((0x7E >> i) & 1); };
    flag();
    for (const auto& fr : frames) {
        int ones_ = 0;
        for (uint8_t byte : fr)
            for (int i = 0; i < 8; i++) {
                const uint8_t b = (byte >> i) & 1;
                hd.push_back(b);
                if (b) { if (++ones_ == 5) { hd.push_back(0); ones_ = 0; } }
                else ones_ = 0;
            }
        flag();
    }
    const uint32_t datalen = (uint32_t)hd.size();
    const uint32_t datalenOctets = (datalen + 7) / 8;
    std::vector<uint8_t> data(datalenOctets, 0);
    for (size_t i = 0; i < hd.size(); i++) data[i / 8] |= (uint8_t)(hd[i] << (i % 8));
    // block layout exactly as the decoder computes it from the length
    uint32_t numBlocks = datalenOctets / RS_K;
    uint32_t fecOctets = numBlocks * (RS_N - RS_K);
    uint32_t lastBlockLen = datalenOctets % RS_K;
    if (lastBlockLen) numBlocks++;
    fecOctets += fecOctetCount(lastBlockLen);
    if (lastBlockLen == 0) lastBlockLen = RS_K;
    std::vector<std::array<uint8_t, RS_N>> tab(numBlocks);
    for (auto& r : tab) r.fill(0);
    for (uint32_t r = 0, p = 0; r < numBlocks; r++) {
        const uint32_t n = r == numBlocks - 1 ? lastBlockLen : RS_K;
        for (uint32_t i = 0; i < n; i++) tab[r][i] = data[p++];
        rs().encode(tab[r].data(), tab[r].data() + RS_K);
    }
    std::vector<uint8_t> dataTx, fecTx;
    interleave(tab, datalenOctets, numBlocks, RS_K, 0, dataTx);
    uint32_t fecRows = numBlocks;
    if (fecOctetCount(lastBlockLen) == 0) fecRows--;
    if (fecRows > 0)
    interleave(tab, fecOctets, fecRows, RS_N - RS_K, RS_K, fecTx);

    std::vector<uint8_t> tx;
    uint32_t hdr = reverseBits(datalen, TRLEN) << HDRFECLEN;
    uint32_t fecBits = 0;
    for (int i = 0; i < HDRFECLEN; i++) fecBits |= (uint32_t)parity(hdr & headerH()[i]) << (HDRFECLEN - 1 - i);
    hdr |= fecBits;
    for (int i = HEADER_LEN - 1; i >= 0; i--) tx.push_back((hdr >> i) & 1);
    for (uint8_t b : dataTx) for (int j = 0; j < 8; j++) tx.push_back((b >> j) & 1);
    for (uint8_t b : fecTx) for (int j = 0; j < 8; j++) tx.push_back((b >> j) & 1);
    while (tx.size() % 3) tx.push_back(0);
    uint16_t l = LFSR_IV;
    for (auto& b : tx) {
        const uint8_t bit = (uint8_t)(((l >> 0) ^ (l >> 14)) & 1);
        l = (uint16_t)((l >> 1) | (bit << 14));
        b ^= bit;
    }
    // symbols: preamble phases, then differential 8PSK (Gray)
    static const int gray[8] = {0, 1, 3, 2, 6, 7, 5, 4};
    static const int prPhase[PREAMBLE_SYMS] = {0, 3, 5, 1, 1, 2, 0, 4, 5, 4, 6, 3, 1, 6, 5, 0};   // eighths of a turn
    std::vector<int> ph;   // cumulative phase in eighths
    ph.push_back(0);   // ramp-up symbol
    for (int i = 0; i < PREAMBLE_SYMS; i++) ph.push_back(prPhase[i]);
    int cur = prPhase[PREAMBLE_SYMS - 1];
    for (size_t i = 0; i + 2 < tx.size(); i += 3) {
        const int g = (tx[i] << 2) | (tx[i + 1] << 1) | tx[i + 2];
        int idx = 0;
        for (int k = 0; k < 8; k++) if (gray[k] == g) idx = k;
        cur = (cur + idx) % 8;
        ph.push_back(cur);
    }
    for (int i = 0; i < 4; i++) ph.push_back(cur);
    // raised cosine pulses (alpha 0.6), symbols at their sampling instants
    const int span = 6;
    std::vector<std::complex<float>> out((ph.size() + 2 * span) * SPS, {0, 0});
    for (size_t s = 0; s < ph.size(); s++) {
        const std::complex<float> sym = std::polar(amplitude, (float)(ph[s] * 3.14159265358979 / 4.0));
        for (int k = -span * SPS; k <= span * SPS; k++) {
            const double t = (double)k / SPS;
            double h;
            const double a = 0.6;
            if (std::fabs(t) < 1e-9) h = 1.0;
            else if (std::fabs(std::fabs(2 * a * t) - 1.0) < 1e-9) h = 3.14159265358979 / 4 * std::sin(3.14159265358979 * t) / (3.14159265358979 * t);
            else h = std::sin(3.14159265358979 * t) / (3.14159265358979 * t) * std::cos(3.14159265358979 * a * t) / (1 - 4 * a * a * t * t);
            const long idx = (long)(s + span) * SPS + k;
            if (idx >= 0 && idx < (long)out.size()) out[idx] += sym * (float)h;
        }
    }
    return out;
}

} // namespace vdl2
