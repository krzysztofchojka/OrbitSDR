#pragma once

// =====================================================================
//  WSPR decoder (Weak Signal Propagation Reporter, 2-minute slots).
//
//  A C++ port of wsprd, the WSPR decoder of WSJT-X:
//    Copyright 2001-2015 Joe Taylor, K1JT; 2014-2015 Steven Franke, K9AN;
//    Fano decoder 1994 Phil Karn, KA9Q; lookup3 hash by Bob Jenkins;
//    via rtlsdr-wsprd (2016 Guenael Jouchet, VA2GKA).
//    License: GNU GPL v3 (the same as OrbitSDR).
//  Changes: no FFTW / files / globals, the hash table lives in the object,
//  the subtracted signal is rebuilt from the decoded bits.
//
//  Input: one slot of complex baseband at 375 S/s, 120 s (45000 samples),
//  starting at the even minute, centred on 1500 Hz above the dial
//  frequency (a WSPR signal sits within +-100 Hz of it).
// =====================================================================

#include <vector>
#include <string>
#include <complex>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cctype>
#include <cstdio>
#include <algorithm>
#include "../../DSP.h"   // fft()

namespace wspr {

constexpr int NSYM = 162;
constexpr int NBITS = 81;
constexpr int NSPERSYM = 256;
constexpr int SAMPLE_RATE = 375;
constexpr int SLOT_SAMPLES = 120 * SAMPLE_RATE;
constexpr float DF = 375.0f / 256.0f;
constexpr float DT = 1.0f / 375.0f;
constexpr float DF05 = DF * 0.5f;
constexpr float DF15 = DF * 1.5f;
constexpr float TWOPIDT = 2.0f * 3.14159265358979f * DT;
constexpr int FFT_N = 512;
constexpr int HASHTAB_SIZE = 32768;
constexpr int MAX_CANDIDATES = 200;

inline const uint8_t* syncVector() {
    static const uint8_t pr3[NSYM] = {
        1, 1, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 0, 0, 0, 1, 0,
        0, 1, 0, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 1,
        0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 0, 1,
        1, 0, 1, 0, 0, 0, 0, 1, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 0, 1,
        0, 0, 1, 0, 1, 1, 0, 0, 0, 1, 1, 0, 1, 0, 1, 0, 0, 0, 1, 0,
        0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 1, 1, 1, 0, 1, 1, 0, 0, 1, 1,
        0, 1, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1, 1,
        0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 0, 1, 1, 0, 0, 0, 1, 1, 0,
        0, 0 };
    return pr3;
}

// Soft-decision metric for 2-FSK, Es/No = 6 dB (wsprd metric_tables[2])
inline const float* metricTable() {
    static const float t[256] = {
        0.9999f, 0.9998f, 0.9998f, 0.9998f, 0.9998f, 0.9998f, 0.9997f, 0.9997f, 0.9997f, 0.9997f,
        0.9997f, 0.9996f, 0.9996f, 0.9996f, 0.9995f, 0.9995f, 0.9994f, 0.9994f, 0.9994f, 0.9993f,
        0.9993f, 0.9992f, 0.9991f, 0.9991f, 0.9990f, 0.9989f, 0.9988f, 0.9988f, 0.9988f, 0.9986f,
        0.9985f, 0.9984f, 0.9983f, 0.9982f, 0.9980f, 0.9979f, 0.9977f, 0.9976f, 0.9974f, 0.9971f,
        0.9969f, 0.9968f, 0.9965f, 0.9962f, 0.9960f, 0.9957f, 0.9953f, 0.9950f, 0.9947f, 0.9941f,
        0.9937f, 0.9933f, 0.9928f, 0.9922f, 0.9917f, 0.9911f, 0.9904f, 0.9897f, 0.9890f, 0.9882f,
        0.9874f, 0.9863f, 0.9855f, 0.9843f, 0.9832f, 0.9819f, 0.9806f, 0.9792f, 0.9777f, 0.9760f,
        0.9743f, 0.9724f, 0.9704f, 0.9683f, 0.9659f, 0.9634f, 0.9609f, 0.9581f, 0.9550f, 0.9516f,
        0.9481f, 0.9446f, 0.9406f, 0.9363f, 0.9317f, 0.9270f, 0.9218f, 0.9160f, 0.9103f, 0.9038f,
        0.8972f, 0.8898f, 0.8822f, 0.8739f, 0.8647f, 0.8554f, 0.8457f, 0.8357f, 0.8231f, 0.8115f,
        0.7984f, 0.7854f, 0.7704f, 0.7556f, 0.7391f, 0.7210f, 0.7038f, 0.6840f, 0.6633f, 0.6408f,
        0.6174f, 0.5939f, 0.5678f, 0.5410f, 0.5137f, 0.4836f, 0.4524f, 0.4193f, 0.3850f, 0.3482f,
        0.3132f, 0.2733f, 0.2315f, 0.1891f, 0.1435f, 0.0980f, 0.0493f, 0.0000f, -0.0510f, -0.1052f,
        -0.1593f, -0.2177f, -0.2759f, -0.3374f, -0.4005f, -0.4599f, -0.5266f, -0.5935f, -0.6626f, -0.7328f,
        -0.8051f, -0.8757f, -0.9498f, -1.0271f, -1.1019f, -1.1816f, -1.2642f, -1.3459f, -1.4295f, -1.5077f,
        -1.5958f, -1.6818f, -1.7647f, -1.8548f, -1.9387f, -2.0295f, -2.1152f, -2.2154f, -2.3011f, -2.3904f,
        -2.4820f, -2.5786f, -2.6730f, -2.7652f, -2.8616f, -2.9546f, -3.0526f, -3.1445f, -3.2445f, -3.3416f,
        -3.4357f, -3.5325f, -3.6324f, -3.7313f, -3.8225f, -3.9209f, -4.0248f, -4.1278f, -4.2261f, -4.3193f,
        -4.4220f, -4.5262f, -4.6214f, -4.7242f, -4.8234f, -4.9245f, -5.0298f, -5.1250f, -5.2232f, -5.3267f,
        -5.4332f, -5.5342f, -5.6431f, -5.7270f, -5.8401f, -5.9350f, -6.0407f, -6.1418f, -6.2363f, -6.3384f,
        -6.4536f, -6.5429f, -6.6582f, -6.7433f, -6.8438f, -6.9478f, -7.0789f, -7.1894f, -7.2714f, -7.3815f,
        -7.4810f, -7.5575f, -7.6852f, -7.8071f, -7.8580f, -7.9724f, -8.1000f, -8.2207f, -8.2867f, -8.4017f,
        -8.5287f, -8.6347f, -8.7082f, -8.8319f, -8.9448f, -9.0355f, -9.1885f, -9.2095f, -9.2863f, -9.4186f,
        -9.5064f, -9.6386f, -9.7207f, -9.8286f, -9.9453f, -10.0701f, -10.1735f, -10.3001f, -10.2858f, -10.5427f,
        -10.5982f, -10.7361f, -10.7042f, -10.9212f, -11.0097f, -11.0469f, -11.1155f, -11.2812f, -11.3472f, -11.4988f,
        -11.5327f, -11.6692f, -11.9376f, -11.8606f, -12.1372f, -13.2539f
    };
    return t;
}

// --- lookup3 hashlittle() by Bob Jenkins, masked to 15 bits (wsprd nhash) ---
inline uint32_t nhash(const std::string& key, uint32_t initval = 146) {
    auto rot = [](uint32_t x, int k) { return (x << k) | (x >> (32 - k)); };
    size_t length = key.size();
    const uint8_t* k = (const uint8_t*)key.data();
    uint32_t a, b, c;
    a = b = c = 0xdeadbeef + (uint32_t)length + initval;
    auto mix = [&]() {
        a -= c; a ^= rot(c, 4);  c += b;
        b -= a; b ^= rot(a, 6);  a += c;
        c -= b; c ^= rot(b, 8);  b += a;
        a -= c; a ^= rot(c, 16); c += b;
        b -= a; b ^= rot(a, 19); a += c;
        c -= b; c ^= rot(b, 4);  b += a;
    };
    while (length > 12) {
        a += k[0] + ((uint32_t)k[1] << 8) + ((uint32_t)k[2] << 16) + ((uint32_t)k[3] << 24);
        b += k[4] + ((uint32_t)k[5] << 8) + ((uint32_t)k[6] << 16) + ((uint32_t)k[7] << 24);
        c += k[8] + ((uint32_t)k[9] << 8) + ((uint32_t)k[10] << 16) + ((uint32_t)k[11] << 24);
        mix();
        length -= 12;
        k += 12;
    }
    switch (length) {
    case 12: c += (uint32_t)k[11] << 24; [[fallthrough]];
    case 11: c += (uint32_t)k[10] << 16; [[fallthrough]];
    case 10: c += (uint32_t)k[9] << 8;   [[fallthrough]];
    case 9:  c += k[8];                  [[fallthrough]];
    case 8:  b += (uint32_t)k[7] << 24;  [[fallthrough]];
    case 7:  b += (uint32_t)k[6] << 16;  [[fallthrough]];
    case 6:  b += (uint32_t)k[5] << 8;   [[fallthrough]];
    case 5:  b += k[4];                  [[fallthrough]];
    case 4:  a += (uint32_t)k[3] << 24;  [[fallthrough]];
    case 3:  a += (uint32_t)k[2] << 16;  [[fallthrough]];
    case 2:  a += (uint32_t)k[1] << 8;   [[fallthrough]];
    case 1:  a += k[0]; break;
    case 0:  return c & 32767;
    }
    c ^= b; c -= rot(b, 14);
    a ^= c; a -= rot(c, 11);
    b ^= a; b -= rot(a, 25);
    c ^= b; c -= rot(b, 16);
    a ^= c; a -= rot(c, 4);
    b ^= a; b -= rot(a, 14);
    c ^= b; c -= rot(b, 24);
    return c & 32767;
}

// --- K=32, r=1/2 convolutional code (Layland-Lushbaugh) ---
constexpr uint32_t POLY1 = 0xf2d05351u;
constexpr uint32_t POLY2 = 0xe4613c47u;

inline int parity32(uint32_t x) {
    x ^= x >> 16; x ^= x >> 8; x ^= x >> 4;
    return (0x6996 >> (x & 0xf)) & 1;
}
inline int encodeSym(uint32_t state) { return (parity32(state & POLY1) << 1) | parity32(state & POLY2); }

// Input bytes high bit first; output: one bit per symbol, POLY1 first
inline void convEncode(uint8_t* symbols, const uint8_t* data, int nbytes) {
    uint32_t st = 0;
    for (int b = 0; b < nbytes; b++)
        for (int i = 7; i >= 0; i--) {
            st = (st << 1) | ((data[b] >> i) & 1);
            const int s = encodeSym(st);
            *symbols++ = (uint8_t)(s >> 1);
            *symbols++ = (uint8_t)(s & 1);
        }
}

inline uint8_t bitReverse8(uint8_t i) {
    return (uint8_t)((((uint64_t)i * 0x80200802ULL) & 0x0884422110ULL) * 0x0101010101ULL >> 32);
}
inline void deinterleave(uint8_t* sym) {
    uint8_t tmp[NSYM];
    int p = 0;
    for (int i = 0; p < NSYM; i++) {
        const uint8_t j = bitReverse8((uint8_t)i);
        if (j < NSYM) tmp[p++] = sym[j];
    }
    std::memcpy(sym, tmp, NSYM);
}
inline void interleave(uint8_t* sym) {
    uint8_t tmp[NSYM];
    int p = 0;
    for (int i = 0; p < NSYM; i++) {
        const uint8_t j = bitReverse8((uint8_t)i);
        if (j < NSYM) tmp[j] = sym[p++];
    }
    std::memcpy(sym, tmp, NSYM);
}

// 50 payload bits (+31 zero tail bits, 11 bytes) -> 162 channel symbols 0..3
inline void channelSymbols(const uint8_t data[11], uint8_t out[NSYM]) {
    uint8_t bits[176] = {0};
    convEncode(bits, data, 11);
    interleave(bits);
    const uint8_t* s = syncVector();
    for (int i = 0; i < NSYM; i++) out[i] = (uint8_t)(2 * bits[i] + s[i]);
}

// Soft decision Fano sequential decoder (Phil Karn, KA9Q). 0 = decoded.
inline int fano(uint8_t* data, const uint8_t* symbols, int nbits, const int mettab[2][256], int delta, int maxcycles) {
    struct Node { uint32_t encstate; long gamma; int metrics[4]; int tm[2]; int i; };
    std::vector<Node> nodes((size_t)nbits + 1);
    Node* first = nodes.data();
    Node* lastnode = &nodes[(size_t)nbits - 1];
    Node* tail = &nodes[(size_t)nbits - 31];
    for (Node* np = first; np <= lastnode; np++) {
        np->metrics[0] = mettab[0][symbols[0]] + mettab[0][symbols[1]];
        np->metrics[1] = mettab[0][symbols[0]] + mettab[1][symbols[1]];
        np->metrics[2] = mettab[1][symbols[0]] + mettab[0][symbols[1]];
        np->metrics[3] = mettab[1][symbols[0]] + mettab[1][symbols[1]];
        symbols += 2;
    }
    Node* np = first;
    np->encstate = 0;
    int lsym = encodeSym(np->encstate);
    int m0 = np->metrics[lsym];
    int m1 = np->metrics[3 ^ lsym];
    if (m0 > m1) { np->tm[0] = m0; np->tm[1] = m1; }
    else { np->tm[0] = m1; np->tm[1] = m0; np->encstate++; }
    np->i = 0;
    const long long maxc = (long long)maxcycles * nbits;
    long t = 0;
    np->gamma = 0;
    long long i;
    for (i = 1; i <= maxc; i++) {
        const long ngamma = np->gamma + np->tm[np->i];
        if (ngamma >= t) {
            if (np->gamma < t + delta) while (ngamma >= t + delta) t += delta;
            np[1].gamma = ngamma;
            np[1].encstate = np->encstate << 1;
            if (++np == lastnode + 1) break;
            lsym = encodeSym(np->encstate);
            if (np >= tail) np->tm[0] = np->metrics[lsym];
            else {
                m0 = np->metrics[lsym];
                m1 = np->metrics[3 ^ lsym];
                if (m0 > m1) { np->tm[0] = m0; np->tm[1] = m1; }
                else { np->tm[0] = m1; np->tm[1] = m0; np->encstate++; }
            }
            np->i = 0;
            continue;
        }
        for (;;) {
            if (np == first || np[-1].gamma < t) {
                t -= delta;
                if (np->i != 0) { np->i = 0; np->encstate ^= 1; }
                break;
            }
            if (--np < tail && np->i != 1) { np->i++; np->encstate ^= 1; break; }
        }
    }
    const Node* p = &nodes[7];
    for (int n = 0; n < nbits / 8; n++) { data[n] = (uint8_t)p->encstate; p += 8; }
    return i >= maxc ? -1 : 0;
}

struct Spot {
    float freq = 0;     // Hz from the centre (1500 Hz above the dial frequency)
    float snr = 0;      // dB in 2500 Hz
    float dt = 0;       // s, relative to the nominal start (1 s after the even minute)
    float drift = 0;    // Hz over the transmission
    float sync = 0;
    std::string message;   // "SP5ABC KO02 37"
    std::string call, grid;
    int dbm = 0;
};

class Decoder {
    std::vector<std::string> hashtab = std::vector<std::string>(HASHTAB_SIZE);
    int mettab[2][256];

    static const char* charset() { return "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ "; }

    static void unpack50(const uint8_t* d, int32_t& n1, int32_t& n2) {
        n1 = (d[0] << 20) | (d[1] << 12) | (d[2] << 4) | ((d[3] >> 4) & 15);
        n2 = ((d[3] & 15) << 18) | (d[4] << 10) | (d[5] << 2) | ((d[6] >> 6) & 3);
    }

    static bool unpackCall(int32_t n, char out[7]) {
        const char* c = charset();
        if (n >= 262177560) return false;
        char tmp[7];
        tmp[5] = c[n % 27 + 10]; n /= 27;
        tmp[4] = c[n % 27 + 10]; n /= 27;
        tmp[3] = c[n % 27 + 10]; n /= 27;
        tmp[2] = c[n % 10];      n /= 10;
        tmp[1] = c[n % 36];      n /= 36;
        tmp[0] = c[n];
        tmp[6] = 0;
        std::memcpy(out, tmp, 7);   // raw 6 characters (spaces kept)
        return true;
    }

    static std::string trimCall(const char raw[7]) {
        std::string s(raw, 6);
        while (!s.empty() && s.front() == ' ') s.erase(s.begin());
        while (!s.empty() && s.back() == ' ') s.pop_back();
        return s;
    }

    static bool unpackGrid(int32_t ngrid, std::string& grid) {
        const char* c = charset();
        ngrid >>= 7;
        if (ngrid >= 32400) { grid = "XXXX"; return false; }
        const int dlat = (ngrid % 180) - 90;
        int dlong = (ngrid / 180) * 2 - 180 + 2;
        if (dlong < -180) dlong += 360;
        if (dlong > 180) dlong += 360;
        const int nlong = (int)(60.0 * (180.0 - dlong) / 5.0);
        int n1 = nlong / 240, n2 = (nlong - 240 * n1) / 24;
        grid.assign(4, ' ');
        grid[0] = c[10 + n1]; grid[2] = c[n2];
        const int nlat = (int)(60.0 * (dlat + 90) / 2.5);
        n1 = nlat / 240; n2 = (nlat - 240 * n1) / 24;
        grid[1] = c[10 + n1]; grid[3] = c[n2];
        return true;
    }

    static bool unpackPfx(int32_t nprefix, std::string& call) {
        if (nprefix < 60000) {
            char pfx[4] = {0, 0, 0, 0};
            int32_t n = nprefix;
            for (int i = 2; i >= 0; i--) {
                const int nc = n % 37;
                pfx[i] = nc <= 9 ? (char)(nc + 48) : nc <= 35 ? (char)(nc + 55) : ' ';
                n /= 37;
            }
            const char* p = std::strrchr(pfx, ' ');
            call = std::string(p ? p + 1 : pfx) + "/" + call;
        } else {
            const int nc = nprefix - 60000;
            if (nc >= 0 && nc <= 9) call += "/" + std::string(1, (char)(nc + 48));
            else if (nc >= 10 && nc <= 35) call += "/" + std::string(1, (char)(nc + 55));
            else if (nc >= 36 && nc <= 125) {
                call += "/";
                call += (char)((nc - 26) / 10 + 48);
                call += (char)((nc - 26) % 10 + 48);
            } else return false;
        }
        return true;
    }

    // wsprd unpk_(); returns false when the message must not be shown
    bool unpack(const uint8_t* data, Spot& s) {
        int32_t n1, n2;
        unpack50(data, n1, n2);
        char raw[7];
        if (!unpackCall(n1, raw)) return false;
        std::string grid;
        if (!unpackGrid(n2, grid)) return false;
        std::string callsign = trimCall(raw);
        const int ntype = (n2 & 127) - 64;
        char b[48];
        if (ntype >= 0 && ntype <= 62) {
            const int nu = ntype % 10;
            if (nu == 0 || nu == 3 || nu == 7) {   // Type 1: call, 4 character grid, power
                s.dbm = ntype;
                std::snprintf(b, sizeof(b), "%s %s %d", callsign.c_str(), grid.c_str(), ntype);
                hashtab[nhash(callsign)] = callsign;
                s.call = callsign; s.grid = grid;
            } else {                               // Type 2: compound call, power
                int nadd = nu;
                if (nu > 3) nadd = nu - 3;
                if (nu > 7) nadd = nu - 7;
                const int32_t n3 = n2 / 128 + HASHTAB_SIZE * (nadd - 1);
                if (!unpackPfx(n3, callsign)) return false;
                const int ndbm = ntype - nadd;
                s.dbm = ndbm;
                std::snprintf(b, sizeof(b), "%s %d", callsign.c_str(), ndbm);
                const int nu2 = ndbm % 10;
                if (nu2 == 0 || nu2 == 3 || nu2 == 7) hashtab[nhash(callsign)] = callsign;
                else return false;
                s.call = callsign;
            }
        } else {                                   // Type 3: hashed call, 6 character grid, power
            const int ndbm = -(ntype + 1);
            std::string grid6;
            grid6 += raw[5];
            grid6.append(raw, 5);
            const int nu = ndbm % 10;
            bool ok = (nu == 0 || nu == 3 || nu == 7) &&
                      std::isalpha((unsigned char)grid6[0]) && std::isalpha((unsigned char)grid6[1]) &&
                      std::isdigit((unsigned char)grid6[2]) && std::isdigit((unsigned char)grid6[3]);
            while (!grid6.empty() && grid6.back() == ' ') grid6.pop_back();
            const int ihash = (n2 - ntype - 64) / 128;
            const std::string known = (ihash >= 0 && ihash < HASHTAB_SIZE) ? hashtab[ihash] : std::string();
            const std::string hc = "<" + (known.empty() ? std::string("...") : known) + ">";
            s.dbm = ndbm;
            std::snprintf(b, sizeof(b), "%s %s %d", hc.c_str(), grid6.c_str(), ndbm);
            s.call = hc; s.grid = grid6;
            if (ntype == -64) ok = false;
            if (!ok) return false;
        }
        s.message = b;
        return true;
    }

    // wsprd sync_and_demodulate(). mode 0: best time lag, 1: best frequency, 2: soft symbols
    static void syncAndDemod(const float* id, const float* qd, long np, uint8_t* symbols, float& freq,
                             int ifmin, int ifmax, float fstep, int& shift, int lagmin, int lagmax, int lagstep,
                             float drift, int symfac, float& sync, int mode) {
        float fsymb[NSYM];
        float c0[NSPERSYM], s0[NSPERSYM], c1[NSPERSYM], s1[NSPERSYM];
        float c2[NSPERSYM], s2[NSPERSYM], c3[NSPERSYM], s3[NSPERSYM];
        const uint8_t* pr3 = syncVector();
        float fbest = 0.0f, syncmax = -1e30f, fplast = -10000.0f;
        int bestShift = 0;
        if (mode == 0) { ifmin = 0; ifmax = 0; fstep = 0.0f; }
        else if (mode == 1) { lagmin = shift; lagmax = shift; }
        else { lagmin = shift; lagmax = shift; ifmin = 0; ifmax = 0; }

        for (int ifreq = ifmin; ifreq <= ifmax; ifreq++) {
            const float f0 = freq + ifreq * fstep;
            for (int lag = lagmin; lag <= lagmax; lag += lagstep) {
                float ss = 0.0f, totp = 0.0f;
                for (int i = 0; i < NSYM; i++) {
                    const float fp = f0 + (drift / 2.0f) * ((float)i - (float)NBITS) / (float)NBITS;
                    if (i == 0 || fp != fplast) {
                        const float d0 = TWOPIDT * (fp - DF15), d1 = TWOPIDT * (fp - DF05);
                        const float d2 = TWOPIDT * (fp + DF05), d3 = TWOPIDT * (fp + DF15);
                        const float cd0 = std::cos(d0), sd0 = std::sin(d0), cd1 = std::cos(d1), sd1 = std::sin(d1);
                        const float cd2 = std::cos(d2), sd2 = std::sin(d2), cd3 = std::cos(d3), sd3 = std::sin(d3);
                        c0[0] = 1; s0[0] = 0; c1[0] = 1; s1[0] = 0; c2[0] = 1; s2[0] = 0; c3[0] = 1; s3[0] = 0;
                        for (int j = 1; j < NSPERSYM; j++) {
                            c0[j] = c0[j - 1] * cd0 - s0[j - 1] * sd0; s0[j] = c0[j - 1] * sd0 + s0[j - 1] * cd0;
                            c1[j] = c1[j - 1] * cd1 - s1[j - 1] * sd1; s1[j] = c1[j - 1] * sd1 + s1[j - 1] * cd1;
                            c2[j] = c2[j - 1] * cd2 - s2[j - 1] * sd2; s2[j] = c2[j - 1] * sd2 + s2[j - 1] * cd2;
                            c3[j] = c3[j - 1] * cd3 - s3[j - 1] * sd3; s3[j] = c3[j - 1] * sd3 + s3[j - 1] * cd3;
                        }
                        fplast = fp;
                    }
                    float i0 = 0, q0 = 0, i1 = 0, q1 = 0, i2 = 0, q2 = 0, i3 = 0, q3 = 0;
                    for (int j = 0; j < NSPERSYM; j++) {
                        const int k = lag + i * NSPERSYM + j;
                        if (k > 0 && k < np) {
                            const float x = id[k], y = qd[k];
                            i0 += x * c0[j] + y * s0[j]; q0 += -x * s0[j] + y * c0[j];
                            i1 += x * c1[j] + y * s1[j]; q1 += -x * s1[j] + y * c1[j];
                            i2 += x * c2[j] + y * s2[j]; q2 += -x * s2[j] + y * c2[j];
                            i3 += x * c3[j] + y * s3[j]; q3 += -x * s3[j] + y * c3[j];
                        }
                    }
                    const float p0 = std::sqrt(i0 * i0 + q0 * q0), p1 = std::sqrt(i1 * i1 + q1 * q1);
                    const float p2 = std::sqrt(i2 * i2 + q2 * q2), p3 = std::sqrt(i3 * i3 + q3 * q3);
                    totp += p0 + p1 + p2 + p3;
                    const float cmet = (p1 + p3) - (p0 + p2);
                    ss = pr3[i] == 1 ? ss + cmet : ss - cmet;
                    if (mode == 2) fsymb[i] = pr3[i] == 1 ? p3 - p1 : p2 - p0;
                }
                ss = totp > 0 ? ss / totp : 0.0f;
                if (ss > syncmax) { syncmax = ss; bestShift = lag; fbest = f0; }
            }
        }
        if (mode <= 1) { sync = syncmax; shift = bestShift; freq = fbest; return; }
        sync = syncmax;
        float fsum = 0, f2sum = 0;
        for (int i = 0; i < NSYM; i++) { fsum += fsymb[i] / NSYM; f2sum += fsymb[i] * fsymb[i] / NSYM; }
        const float fac = std::sqrt(std::max(1e-20f, f2sum - fsum * fsum));
        for (int i = 0; i < NSYM; i++) {
            float v = symfac * fsymb[i] / fac;
            v = std::clamp(v, -128.0f, 127.0f);
            symbols[i] = (uint8_t)(int)(v + 128.0f);
        }
    }

    // wsprd subtract_signal2(): removes a decoded signal so weaker ones below it decode
    static void subtract(float* id, float* qd, long np, float f0, int shift, float drift, const uint8_t* chsym) {
        const int nfilt = 360;
        const int N = NSYM * NSPERSYM;
        std::vector<float> refi(N), refq(N), ci(SLOT_SAMPLES + 2 * nfilt, 0.0f), cq(SLOT_SAMPLES + 2 * nfilt, 0.0f);
        std::vector<float> cfi(SLOT_SAMPLES + 2 * nfilt, 0.0f), cfq(SLOT_SAMPLES + 2 * nfilt, 0.0f);
        float phi = 0.0f;
        for (int i = 0; i < NSYM; i++) {
            const float dphi = TWOPIDT * (f0 + (drift / 2.0f) * ((float)i - (float)NSYM / 2.0f) / ((float)NSYM / 2.0f) +
                                          ((float)chsym[i] - 1.5f) * DF);
            for (int j = 0; j < NSPERSYM; j++) {
                refi[i * NSPERSYM + j] = std::cos(phi);
                refq[i * NSPERSYM + j] = std::sin(phi);
                phi += dphi;
            }
        }
        float w[nfilt], partial[nfilt], norm = 0.0f;
        for (int i = 0; i < nfilt; i++) { w[i] = std::sin(3.14159265f * i / (nfilt - 1)); norm += w[i]; }
        for (int i = 0; i < nfilt; i++) w[i] /= norm;
        partial[0] = 0.0f;
        for (int i = 1; i < nfilt; i++) partial[i] = partial[i - 1] + w[i];
        for (int i = 0; i < N; i++) {
            const int k = shift + i;
            if (k > 0 && k < np) {
                ci[i + nfilt] = id[k] * refi[i] + qd[k] * refq[i];
                cq[i + nfilt] = qd[k] * refi[i] - id[k] * refq[i];
            }
        }
        for (int i = nfilt / 2; i < SLOT_SAMPLES - nfilt / 2; i++) {
            float a = 0, b = 0;
            for (int j = 0; j < nfilt; j++) { a += w[j] * ci[i - nfilt / 2 + j]; b += w[j] * cq[i - nfilt / 2 + j]; }
            cfi[i] = a; cfq[i] = b;
        }
        for (int i = 0; i < N; i++) {
            if (i < nfilt / 2) norm = partial[nfilt / 2 + i];
            else if (i > N - 1 - nfilt / 2) norm = partial[nfilt / 2 + N - 1 - i];
            else norm = 1.0f;
            if (norm <= 0.0f) continue;
            const int k = shift + i, j = i + nfilt;
            if (k > 0 && k < np) {
                id[k] -= (cfi[j] * refi[i] - cfq[j] * refq[i]) / norm;
                qd[k] -= (cfi[j] * refq[i] + cfq[j] * refi[i]) / norm;
            }
        }
    }

public:
    Decoder() {
        const float* m = metricTable();
        const float bias = 0.45f;
        for (int i = 0; i < 256; i++) {
            mettab[0][i] = (int)std::lround(10.0 * (m[i] - bias));
            mettab[1][i] = (int)std::lround(10.0 * (m[255 - i] - bias));
        }
    }

    // Decodes one 2-minute slot (wsprd wspr_decode, 2 passes with subtraction)
    std::vector<Spot> decode(const std::vector<std::complex<float>>& iq) {
        std::vector<Spot> out;
        const int samples = (int)std::min<size_t>(iq.size(), SLOT_SAMPLES);
        if (samples < FFT_N * 8) return out;
        std::vector<float> idat(SLOT_SAMPLES, 0.0f), qdat(SLOT_SAMPLES, 0.0f);
        for (int i = 0; i < samples; i++) { idat[i] = iq[i].real(); qdat[i] = iq[i].imag(); }

        const float minsync1 = 0.10f;
        float minsync2 = 0.12f;
        const int iifac = 3, symfac = 50;
        int maxdrift = 4;
        const float minrms = 52.0f * (symfac / 64.0f);
        const int delta = 60, maxcycles = 10000;
        const float fmin = -110.0f, fmax = 110.0f;
        const int npasses = 2;

        struct Cand { float freq, snr; int shift; float drift, sync; };
        std::vector<float> allfreqs;
        std::vector<std::string> allcalls;

        float hann[FFT_N];
        for (int i = 0; i < FFT_N; i++) hann[i] = std::sin(0.006147931f * i);
        const int blocks = 4 * (SLOT_SAMPLES / FFT_N) - 1;
        std::vector<float> ps((size_t)FFT_N * blocks);   // ps[j * blocks + i]
        auto PS = [&](int j, int i) -> float& { return ps[(size_t)j * blocks + i]; };
        std::vector<Complex> buf(FFT_N);

        for (int ipass = 0; ipass < npasses; ipass++) {
            if (ipass == 1 && out.empty()) break;
            for (int i = 0; i < blocks; i++) {
                for (int j = 0; j < FFT_N; j++) {
                    const int k = i * 128 + j;
                    buf[j] = Complex(idat[k] * hann[j], qdat[k] * hann[j]);
                }
                fft(buf);
                for (int j = 0; j < FFT_N; j++) {
                    int k = j + FFT_N / 2;
                    if (k > FFT_N - 1) k -= FFT_N;
                    PS(j, i) = (float)std::norm(buf[k]);
                }
            }
            std::vector<float> psavg(FFT_N, 0.0f);
            for (int j = 0; j < FFT_N; j++) for (int i = 0; i < blocks; i++) psavg[j] += PS(j, i);

            float smspec[411];
            for (int i = 0; i < 411; i++) {
                smspec[i] = 0.0f;
                for (int j = -3; j <= 3; j++) smspec[i] += psavg[256 - 205 + i + j];
            }
            float tmpsort[411];
            std::memcpy(tmpsort, smspec, sizeof(smspec));
            std::sort(tmpsort, tmpsort + 411);
            const float noise = std::max(1e-30f, tmpsort[122]);
            const float minSnr = std::pow(10.0f, -8.0f / 10.0f);
            for (int j = 0; j < 411; j++) {
                smspec[j] = smspec[j] / noise - 1.0f;
                if (smspec[j] < minSnr) smspec[j] = 0.1f * minSnr;
            }

            std::vector<Cand> cands;
            for (int j = 1; j < 410 && (int)cands.size() < MAX_CANDIDATES; j++) {
                if (smspec[j] > smspec[j - 1] && smspec[j] > smspec[j + 1]) {
                    Cand c{};
                    c.freq = (j - 205) * (DF / 2.0f);
                    c.snr = 10.0f * std::log10(smspec[j]) - 26.3f;
                    if (c.freq >= fmin && c.freq <= fmax) cands.push_back(c);
                }
            }
            std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.snr > b.snr; });

            // coarse time / frequency / drift from the spectrogram
            for (Cand& c : cands) {
                float syncMax = -1e30f;
                const int if0 = (int)(c.freq / (DF / 2.0f)) + NSPERSYM;
                for (int ifr = if0 - 1; ifr <= if0 + 1; ifr++)
                    for (int k0 = -10; k0 < 22; k0++)
                        for (int idrift = -maxdrift; idrift <= maxdrift; idrift++) {
                            float ss = 0.0f, pw = 0.0f, sync = 0.0f;
                            for (int k = 0; k < NSYM; k++) {
                                const int ifd = ifr + (int)(((float)k - (float)NBITS) / (float)NBITS * (float)idrift / DF);
                                const int kindex = k0 + 2 * k;
                                if (kindex < blocks && kindex >= 0 && ifd >= 3 && ifd + 3 < FFT_N) {
                                    const float p0 = std::sqrt(PS(ifd - 3, kindex)), p1 = std::sqrt(PS(ifd - 1, kindex));
                                    const float p2 = std::sqrt(PS(ifd + 1, kindex)), p3 = std::sqrt(PS(ifd + 3, kindex));
                                    ss += (2 * syncVector()[k] - 1) * ((p1 + p3) - (p0 + p2));
                                    pw += p0 + p1 + p2 + p3;
                                    sync = pw > 0 ? ss / pw : 0.0f;
                                }
                            }
                            if (sync > syncMax) {
                                syncMax = sync;
                                c.shift = 128 * (k0 + 1);
                                c.drift = (float)idrift;
                                c.freq = (ifr - NSPERSYM) * (DF / 2.0f);
                                c.sync = sync;
                            }
                        }
            }

            uint8_t symbols[NBITS * 2];
            uint8_t decdata[11];
            for (Cand& c : cands) {
                float freq = c.freq, drift = c.drift, sync = c.sync;
                int shift = c.shift;
                const int lagmin = shift - 128, lagmax = shift + 128, lagstep = 8;
                syncAndDemod(idat.data(), qdat.data(), samples, symbols, freq, 0, 0, 0.0f, shift, lagmin, lagmax, lagstep, drift, symfac, sync, 0);
                syncAndDemod(idat.data(), qdat.data(), samples, symbols, freq, -2, 2, 0.1f, shift, lagmin, lagmax, lagstep, drift, symfac, sync, 1);
                c.freq = freq; c.shift = shift; c.sync = sync;
                if (sync <= minsync1) continue;

                bool decoded = false;
                int ii = 0;
                for (int idt = 0; !decoded && idt <= 128 / iifac; idt++) {
                    ii = (idt + 1) / 2;
                    if (idt % 2 == 1) ii = -ii;
                    ii *= iifac;
                    int js = shift + ii;
                    syncAndDemod(idat.data(), qdat.data(), samples, symbols, freq, 0, 0, 0.0f, js, lagmin, lagmax, lagstep, drift, symfac, sync, 2);
                    float sq = 0.0f;
                    for (int i = 0; i < NSYM; i++) { const float y = (float)symbols[i] - 128.0f; sq += y * y; }
                    const float rms = std::sqrt(sq / NSYM);
                    if (sync > minsync2 && rms > minrms) {
                        deinterleave(symbols);
                        std::memset(decdata, 0, sizeof(decdata));
                        decoded = fano(decdata, symbols, NBITS, mettab, delta, maxcycles) == 0;
                    }
                }
                if (!decoded) continue;

                Spot s;
                if (!unpack(decdata, s)) continue;
                if (s.grid == "A000AA") continue;
                if (ipass == 0) {   // remove it, the next pass may find weaker signals under it
                    uint8_t data[11] = {0};
                    std::memcpy(data, decdata, 7);
                    data[6] &= 0xC0;
                    uint8_t chsym[NSYM];
                    channelSymbols(data, chsym);
                    subtract(idat.data(), qdat.data(), samples, freq, shift, drift, chsym);
                }
                bool dupe = false;
                for (size_t i = 0; i < allcalls.size(); i++)
                    if (allcalls[i] == s.call && std::fabs(freq - allfreqs[i]) < 3.0f) dupe = true;
                if (dupe) continue;
                allcalls.push_back(s.call);
                allfreqs.push_back(freq);
                s.freq = freq;
                s.snr = c.snr;
                s.sync = c.sync;
                s.dt = shift * DT - 1.0f;
                s.drift = drift;
                out.push_back(s);
            }
        }
        std::sort(out.begin(), out.end(), [](const Spot& a, const Spot& b) { return a.snr > b.snr; });
        return out;
    }
};

// --- Encoder (tests / signal generation): Type 1 message "CALL GRID DBM" ---
inline int callCharCode(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch == ' ') return 36;
    if (ch >= 'A' && ch <= 'Z') return ch - 'A' + 10;
    return -1;
}

inline bool packType1(const std::string& call, const std::string& grid, int dbm, uint8_t data[11]) {
    if (call.size() > 6 || grid.size() < 4) return false;
    char c6[6];
    std::memset(c6, ' ', 6);
    if (call.size() > 2 && std::isdigit((unsigned char)call[2])) for (size_t i = 0; i < call.size(); i++) c6[i] = call[i];
    else if (call.size() > 1 && std::isdigit((unsigned char)call[1])) for (size_t i = 0; i < call.size(); i++) c6[i + 1] = call[i];
    else return false;
    int v[6];
    for (int i = 0; i < 6; i++) v[i] = callCharCode(c6[i]);
    uint32_t n = (uint32_t)v[0];
    n = n * 36 + v[1];
    n = n * 10 + v[2];
    n = n * 27 + v[3] - 10;
    n = n * 27 + v[4] - 10;
    n = n * 27 + v[5] - 10;
    auto lc = [](char ch) { return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'A' && ch <= 'R' ? ch - 'A' : -1; };
    const int g0 = lc(grid[0]), g1 = lc(grid[1]), g2 = lc(grid[2]), g3 = lc(grid[3]);
    uint32_t m = (uint32_t)((179 - 10 * g0 - g2) * 180 + 10 * g1 + g3);
    m = m * 128 + dbm + 64;
    std::memset(data, 0, 11);
    data[0] = (uint8_t)(n >> 20);
    data[1] = (uint8_t)(n >> 12);
    data[2] = (uint8_t)(n >> 4);
    data[3] = (uint8_t)(((n & 0x0F) << 4) | ((m >> 18) & 0x0F));
    data[4] = (uint8_t)(m >> 10);
    data[5] = (uint8_t)(m >> 2);
    data[6] = (uint8_t)((m & 0x03) << 6);
    return true;
}

} // namespace wspr
