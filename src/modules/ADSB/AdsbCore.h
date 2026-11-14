#pragma once

// =====================================================================
//  ADS-B / Mode S decoder core (1090 MHz), the job dump1090 does.
//
//  Demodulator: works on the magnitude of the raw I/Q at any sample rate
//  of 2 MS/s or more. The 1 Mbit/s PPM signal is read with areas under
//  the magnitude curve (a running sum interpolated between samples), so
//  2.0, 2.048, 2.4 or 3.2 MS/s all work without resampling:
//   - preamble: pulses at 0, 1.0, 3.5 and 4.5 us (0.5 us each),
//   - data from 8 us: bit = 1 when the first half of the 1 us bit period
//     has more energy than the second half.
//  Frames: CRC-24 (poly 0xFFF409), single bit errors of DF17/18 fixed.
//
//  Decoded: DF17/18 extended squitter (identification, airborne and
//  surface position with CPR, altitude, velocity), DF11 all-call, and
//  DF4/5/20/21 altitude / squawk for aircraft already known.
//
//  References: ICAO Annex 10 Vol IV, "The 1090 MHz Riddle" (J. Sun),
//  dump1090 (for the Gillham altitude decoding).
// =====================================================================

#include <vector>
#include <string>
#include <map>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <algorithm>

namespace adsb {

// ---------------------------------------------------------------------
//  Mode S CRC
// ---------------------------------------------------------------------
inline const uint32_t* crcTable() {
    static uint32_t t[256];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 256; i++) {
            uint32_t c = (uint32_t)i << 16;
            for (int j = 0; j < 8; j++) c = (c & 0x800000) ? ((c << 1) ^ 0xFFF409) : (c << 1);
            t[i] = c & 0xFFFFFF;
        }
        init = true;
    }
    return t;
}
// Remainder of the whole message: 0 for a good DF17/18/11(IID 0), the address for DF0/4/5/16/20/21
inline uint32_t syndrome(const uint8_t* m, int bytes) {
    const uint32_t* t = crcTable();
    uint32_t c = 0;
    for (int i = 0; i < bytes - 3; i++) c = ((c << 8) ^ t[((c >> 16) ^ m[i]) & 0xFF]) & 0xFFFFFF;
    const uint32_t parity = ((uint32_t)m[bytes - 3] << 16) | ((uint32_t)m[bytes - 2] << 8) | m[bytes - 1];
    return c ^ parity;
}

// syndrome -> bit to flip, for single bit errors in 112-bit messages (not in the DF field)
inline const std::map<uint32_t, int>& singleBitErrors() {
    static std::map<uint32_t, int> m;
    if (m.empty()) {
        uint8_t msg[14] = {0};
        for (int bit = 5; bit < 112; bit++) {
            msg[bit / 8] ^= (uint8_t)(0x80 >> (bit % 8));
            m[syndrome(msg, 14)] = bit;
            msg[bit / 8] ^= (uint8_t)(0x80 >> (bit % 8));
        }
    }
    return m;
}

inline const uint32_t* bitSyndromes112() {
    static uint32_t t[112];
    static bool init = false;
    if (!init) {
        for (int bit = 0; bit < 112; bit++) {
            uint8_t msg[14] = {0};
            msg[bit / 8] = (uint8_t)(0x80 >> (bit % 8));
            t[bit] = syndrome(msg, 14);
        }
        init = true;
    }
    return t;
}

inline uint32_t bits(const uint8_t* m, int first1, int count) {   // 1-based bit numbering as in the standard
    uint32_t v = 0;
    for (int i = 0; i < count; i++) {
        const int b = first1 - 1 + i;
        v = (v << 1) | ((m[b / 8] >> (7 - b % 8)) & 1);
    }
    return v;
}

// ---------------------------------------------------------------------
//  Altitude / squawk fields
// ---------------------------------------------------------------------
inline int modeAToModeC(unsigned modeA) {
    unsigned fiveHundreds = 0, oneHundreds = 0;
    if ((modeA & 0xFFFF888B) || ((modeA & 0x000000F0) == 0)) return -9999;
    if (modeA & 0x0010) oneHundreds ^= 0x007;   // C1
    if (modeA & 0x0020) oneHundreds ^= 0x003;   // C2
    if (modeA & 0x0040) oneHundreds ^= 0x001;   // C4
    if ((oneHundreds & 5) == 5) oneHundreds ^= 2;
    if (oneHundreds > 5) return -9999;
    if (modeA & 0x0002) fiveHundreds ^= 0x0FF;  // D2
    if (modeA & 0x0004) fiveHundreds ^= 0x07F;  // D4
    if (modeA & 0x1000) fiveHundreds ^= 0x03F;  // A1
    if (modeA & 0x2000) fiveHundreds ^= 0x01F;  // A2
    if (modeA & 0x4000) fiveHundreds ^= 0x00F;  // A4
    if (modeA & 0x0100) fiveHundreds ^= 0x007;  // B1
    if (modeA & 0x0200) fiveHundreds ^= 0x003;  // B2
    if (modeA & 0x0400) fiveHundreds ^= 0x001;  // B4
    if (fiveHundreds & 1) oneHundreds = 6 - oneHundreds;
    return (int)(fiveHundreds * 5 + oneHundreds) - 13;
}
// 13-bit identity field (C1 A1 C2 A2 C4 A4 X B1 D1 B2 D2 B4 D4) -> Mode A code as hex digits ABCD
inline unsigned id13ToModeA(unsigned f) {
    unsigned h = 0;
    if (f & 0x1000) h |= 0x0010;  // C1
    if (f & 0x0800) h |= 0x1000;  // A1
    if (f & 0x0400) h |= 0x0020;  // C2
    if (f & 0x0200) h |= 0x2000;  // A2
    if (f & 0x0100) h |= 0x0040;  // C4
    if (f & 0x0080) h |= 0x4000;  // A4
    if (f & 0x0020) h |= 0x0100;  // B1
    if (f & 0x0010) h |= 0x0001;  // D1
    if (f & 0x0008) h |= 0x0200;  // B2
    if (f & 0x0004) h |= 0x0002;  // D2
    if (f & 0x0002) h |= 0x0400;  // B4
    if (f & 0x0001) h |= 0x0004;  // D4
    return h;
}
constexpr int NO_ALT = -100000;
// 12-bit altitude of extended squitter positions (feet)
inline int altitude12(unsigned ac) {
    if (ac == 0) return NO_ALT;
    if (ac & 0x10) {   // Q bit: 25 ft steps
        const unsigned n = ((ac & 0x0FE0) >> 1) | (ac & 0x000F);
        return (int)n * 25 - 1000;
    }
    const unsigned n13 = ((ac & 0x0FC0) << 1) | (ac & 0x003F);   // insert M = 0
    const int c = modeAToModeC(id13ToModeA(n13));
    return c < -12 ? NO_ALT : c * 100;
}
// 13-bit altitude of DF0/4/16/20 (feet)
inline int altitude13(unsigned ac) {
    if (ac == 0) return NO_ALT;
    if (ac & 0x40) return NO_ALT;   // M bit: metres (not used in practice)
    if (ac & 0x10) {
        const unsigned n = ((ac & 0x1F80) >> 2) | ((ac & 0x0020) >> 1) | (ac & 0x000F);
        return (int)n * 25 - 1000;
    }
    const int c = modeAToModeC(id13ToModeA(ac));
    return c < -12 ? NO_ALT : c * 100;
}

// ---------------------------------------------------------------------
//  CPR position decoding
// ---------------------------------------------------------------------
inline int cprNL(double lat) {
    lat = std::fabs(lat);
    if (lat < 1e-9) return 59;
    if (std::fabs(lat - 87.0) < 1e-9) return 2;
    if (lat > 87.0) return 1;
    const double a = 1.0 - std::cos(M_PI / 30.0);
    const double b = std::cos(M_PI / 180.0 * lat);
    return (int)std::floor(2.0 * M_PI / std::acos(1.0 - a / (b * b)));
}
inline double cprMod(double a, double b) { double r = std::fmod(a, b); return r < 0 ? r + b : r; }

// Global airborne decode from an even and an odd frame (lat/lon in 17-bit CPR units)
inline bool cprGlobal(int latE, int lonE, int latO, int lonO, bool oddNewest, double& lat, double& lon) {
    const double S = 131072.0;
    const double dE = 360.0 / 60.0, dO = 360.0 / 59.0;
    const double j = std::floor((59.0 * latE - 60.0 * latO) / S + 0.5);
    double rE = dE * (cprMod(j, 60) + latE / S), rO = dO * (cprMod(j, 59) + latO / S);
    if (rE >= 270) rE -= 360;
    if (rO >= 270) rO -= 360;
    if (std::fabs(rE) > 90 || std::fabs(rO) > 90) return false;
    if (cprNL(rE) != cprNL(rO)) return false;   // the frames straddle a longitude zone boundary
    const double rl = oddNewest ? rO : rE;
    const int nl = cprNL(rl);
    const int ni = std::max(nl - (oddNewest ? 1 : 0), 1);
    const double m = std::floor((lonE * (nl - 1) - lonO * nl) / S + 0.5);
    double rlon = (360.0 / ni) * (cprMod(m, ni) + (oddNewest ? lonO : lonE) / S);
    rlon -= std::floor((rlon + 180.0) / 360.0) * 360.0;
    lat = rl; lon = rlon;
    return true;
}
// Local decode near a reference position (airborne: within ~300 km; surface: `surface` = true, 45 km)
inline void cprLocal(int latC, int lonC, bool odd, bool surface, double refLat, double refLon, double& lat, double& lon) {
    const double S = 131072.0;
    const double span = surface ? 90.0 : 360.0;
    const double dLat = span / (odd ? 59.0 : 60.0);
    const double j = std::floor(refLat / dLat) + std::floor(cprMod(refLat, dLat) / dLat - latC / S + 0.5);
    lat = dLat * (j + latC / S);
    const int ni = std::max(cprNL(lat) - (odd ? 1 : 0), 1);
    const double dLon = span / ni;
    const double m = std::floor(refLon / dLon) + std::floor(cprMod(refLon, dLon) / dLon - lonC / S + 0.5);
    lon = dLon * (m + lonC / S);
    lon -= std::floor((lon + 180.0) / 360.0) * 360.0;
}

inline double distanceKm(double la1, double lo1, double la2, double lo2) {
    const double r = M_PI / 180.0;
    const double a = std::sin((la2 - la1) * r / 2), b = std::sin((lo2 - lo1) * r / 2);
    const double h = a * a + std::cos(la1 * r) * std::cos(la2 * r) * b * b;
    return 2 * 6371.0 * std::asin(std::min(1.0, std::sqrt(h)));
}

// ---------------------------------------------------------------------
//  Aircraft state
// ---------------------------------------------------------------------
struct Aircraft {
    uint32_t icao = 0;
    std::string callsign, category;
    int altFt = NO_ALT;          // barometric (or GNSS when no baro)
    bool altGnss = false;
    double speedKn = -1, trackDeg = -1, headingDeg = -1;
    int vrateFpm = 0; bool vrateValid = false;
    std::string squawk;
    bool onGround = false;
    double lat = 0, lon = 0; bool posValid = false;
    bool posFromRef = false;     // decoded near the receiver (until a global fix)
    double refLat[2] = {0, 0}, refLon[2] = {0, 0}, refTime[2] = {-1e9, -1e9};   // receiver-relative candidates
    double posTime = 0;          // decoder clock of the last position
    // CPR frames waiting for their pair
    int cprLat[2] = {0, 0}, cprLon[2] = {0, 0};
    double cprTime[2] = {-1e9, -1e9};
    bool cprSurface[2] = {false, false};
    int messages = 0;
    double firstSeen = 0;        // the table keeps this order, so rows do not jump around
    double lastSeen = 0;
    float signalDb = -100;       // smoothed
};

struct Frame {
    uint8_t m[14];
    int bytes = 0;
    int df = 0;
    uint32_t icao = 0;
    bool corrected = false;
    float signalDb = 0;
};

// ---------------------------------------------------------------------
//  Demodulator + decoder
// ---------------------------------------------------------------------
class Decoder {
public:
    std::map<uint32_t, Aircraft> aircraft;
    std::function<void(const Frame&, const Aircraft&)> onFrame;   // after the aircraft was updated
    double refLat = 0, refLon = 0; bool refValid = false;       // the receiver (for surface positions)
    double clock = 0;            // seconds, set by the module before process()
    // statistics
    long long preambles = 0, goodFrames = 0, fixedFrames = 0;

    void reset() { sum.clear(); carry.clear(); dcI = dcQ = 0; }

    // `mag`: magnitude samples at `rate` (consecutive blocks continue each other)
    void process(const float* mag, size_t n, double rate) {
        if (rate < 1.9e6) return;
        if (rate != lastRate) { reset(); lastRate = rate; }
        spu = rate / 1e6;   // samples per microsecond
        // samples kept from the previous block: a frame may start near its end
        std::vector<float>& buf = cur;
        buf.clear();
        buf.reserve(carry.size() + n);
        buf.insert(buf.end(), carry.begin(), carry.end());
        buf.insert(buf.end(), mag, mag + n);
        const size_t total = buf.size();
        // running sum: area(a, b) for fractional sample positions
        sum.assign(total + 1, 0.0);
        for (size_t i = 0; i < total; i++) sum[i + 1] = sum[i] + buf[i];
        // noise floor: median magnitude (frames do not move it, even on a busy channel)
        if (total > 64) {
            std::vector<float> sub;
            sub.reserve(total / 7 + 1);
            for (size_t k = 0; k < total; k += 7) sub.push_back(buf[k]);
            std::nth_element(sub.begin(), sub.begin() + sub.size() / 2, sub.end());
            const double med = sub[sub.size() / 2];
            noise = noise <= 0 ? med : noise * 0.8 + med * 0.2;
        }

        const double frameLen = (8.0 + 112.0) * spu;
        const size_t lastStart = total > (size_t)(frameLen + 2) ? total - (size_t)(frameLen + 2) : 0;
        size_t i = 0;
        while (i < lastStart) {
            // (half-sample steps: at 2.4 MS/s a whole-sample grid can miss the pulses by 0.2 us)
            double t = (double)i;
            if (!quickPreamble(t)) {
                if (!quickPreamble(t + 0.5)) { i++; continue; }
                t += 0.5;
            }
            // fine timing: best of a few sub-sample offsets
            double bestT = t, bestQ = -1;
            for (double f = -0.5; f <= 0.51; f += 0.25) {
                const double q = preambleQuality(t + f);
                if (q > bestQ) { bestQ = q; bestT = t + f; }
            }
            if (bestQ < 0.7) { i++; continue; }   // the CRC decides the rest
            preambles++;
            Frame fr;
            // the preamble gives the start to ~0.3 samples: try a few, the CRC tells which is right
            static const double tryOffsets[] = {0.0, 0.2, -0.2, 0.4, -0.4};
            bool ok = false;
            for (double o : tryOffsets) if ((ok = decodeFrameAt(bestT + o, fr))) break;
            if (ok) {
                handle(fr);
                i += (size_t)((8.0 + fr.bytes * 8.0) * spu);   // skip the frame
                continue;
            }
            i++;
        }
        // keep the tail for the next block
        const size_t keep = std::min(total, (size_t)(frameLen + 4));
        carry.assign(buf.end() - keep, buf.end());
    }

    // DC removal + magnitude from I/Q (the module calls this, then process())
    void magnitude(const double* iq, size_t n, std::vector<float>& out) {
        out.resize(n);
        for (size_t k = 0; k < n; k++) {
            const double I = iq[2 * k], Q = iq[2 * k + 1];
            // very slow: tuned exactly to 1090 MHz the carrier itself sits at DC
            dcI += (I - dcI) * 0.00001; dcQ += (Q - dcQ) * 0.00001;
            const double a = I - dcI, b = Q - dcQ;
            out[k] = (float)std::sqrt(a * a + b * b);
        }
    }

private:
    std::vector<double> sum;
    std::vector<float> cur, carry;   // this block (with the carried tail), tail for the next one
    double spu = 2.0, lastRate = 0, noise = 0;
    double dcI = 0, dcQ = 0;

    // area under the magnitude between times a and b (in samples, fractional)
    double area(double a, double b) const {
        auto at = [&](double x) {
            if (x <= 0) return 0.0;
            const double maxX = (double)(sum.size() - 1);
            if (x >= maxX) return sum.back();
            const size_t k = (size_t)x;
            const double f = x - k;
            return sum[k] + (sum[k + 1] - sum[k]) * f;
        };
        return at(b) - at(a);
    }
    // mean level over [a, b] microseconds from t
    double level(double t, double aUs, double bUs) const {
        const double a = t + aUs * spu, b = t + bUs * spu;
        return area(a, b) / std::max(1e-9, b - a);
    }

    bool quickPreamble(double t) const {
        const double p1 = level(t, 0.05, 0.45), p2 = level(t, 1.05, 1.45);
        if (p1 < noise * 1.8 || p2 < noise * 1.8) return false;
        const double g = level(t, 0.65, 0.85);
        return p1 > g * 1.3 && p2 > g * 1.3;
    }
    // ratio of the weakest pulse to the strongest gap (> 1 = looks like a preamble).
    // Windows stay away from the pulse edges: at 2.4 MS/s a sample spans an edge and the
    // interpolated area spreads it into the neighbouring gap.
    double preambleQuality(double t) const {
        const double p[4] = {level(t, 0.05, 0.45), level(t, 1.05, 1.45), level(t, 3.55, 3.95), level(t, 4.55, 4.95)};
        const double g[4] = {level(t, 0.65, 0.85), level(t, 1.75, 3.25), level(t, 4.15, 4.35), level(t, 5.25, 7.75)};
        const double pmin = std::min(std::min(p[0], p[1]), std::min(p[2], p[3]));
        const double gmax = std::max(std::max(g[0], g[1]), std::max(g[2], g[3]));
        if (pmin < noise * 1.8) return 0.0;
        return pmin / std::max(1e-12, gmax * 1.25);
    }

    // Bits by decision feedback: every sample is modelled as noise + A x (how much of it a
    // pulse covers). For bit k both values are tried with the pulse of bit k-1 already known
    // (and half a pulse for the unknown bit k+1); the better fit wins. At ~2 samples per bit a
    // sample often straddles two half-bits, which a plain "first half > second half" misreads.
    void readBits(double t, uint8_t* m, int nbits, float* conf = nullptr) const {
        const double h = 0.5 * spu;
        const double A = std::max(1e-9, (level(t, 0.05, 0.45) + level(t, 1.05, 1.45) + level(t, 3.55, 3.95) + level(t, 4.55, 4.95)) / 4.0 - noise);
        auto cov = [](double j, double p0, double p1) { return std::max(0.0, std::min(j + 1.0, p1) - std::max(j, p0)); };
        double prev0 = -1, prev1 = -1;   // pulse of the previous bit (none before the data)
        const int n = (int)cur.size();
        for (int k = 0; k < nbits; k++) {
            const double s = t + (8.0 + k) * spu;
            const int j0 = (int)std::floor(s), j1 = (int)std::floor(s + spu - 1e-9);
            double e[2] = {0, 0};
            for (int hyp = 0; hyp < 2; hyp++) {
                const double p0 = hyp ? s : s + h, p1 = p0 + h;
                for (int j = j0; j <= j1; j++) {
                    if (j < 0 || j >= n) continue;
                    const double c = std::min(1.0, cov(j, p0, p1) + cov(j, prev0, prev1) + 0.5 * cov(j, s + spu, s + spu + h));
                    const double d = cur[j] - (noise + A * c);
                    e[hyp] += d * d;
                }
            }
            const bool one = e[1] < e[0];
            if (one) m[k / 8] |= (uint8_t)(0x80 >> (k % 8));
            if (conf) conf[k] = (float)std::fabs(e[1] - e[0]);
            prev0 = one ? s : s + h; prev1 = prev0 + h;
        }
    }

    bool decodeFrameAt(double t, Frame& fr) {
        uint8_t m[14] = {0};
        float conf[112];
        readBits(t, m, 112, conf);
        const int df = m[0] >> 3;
        const bool longFrame = df >= 16;
        const int nbits = longFrame ? 112 : 56;
        if (!(df == 0 || df == 4 || df == 5 || df == 11 || df == 16 || df == 17 || df == 18 || df == 20 || df == 21)) return false;
        if (!longFrame) std::fill(m + 7, m + 14, 0);
        const int bytes = nbits / 8;
        uint32_t syn = syndrome(m, bytes);
        bool corrected = false;
        if (df == 17 || df == 18) {
            if (syn != 0) {
                int flip[2] = {-1, -1};
                const auto& tbl = singleBitErrors();
                auto it = tbl.find(syn);
                if (it != tbl.end()) flip[0] = it->second;   // any single bit
                else {
                    // two bits among the least certain ones (soft decision); never the DF field
                    const uint32_t* bs = bitSyndromes112();
                    int weak[10];
                    int nw = 0;
                    std::vector<std::pair<float, int>> order;
                    order.reserve(107);
                    for (int k = 5; k < 112; k++) order.push_back({conf[k], k});
                    std::partial_sort(order.begin(), order.begin() + 10, order.end());
                    for (int q = 0; q < 10; q++) weak[nw++] = order[q].second;
                    for (int a = 0; a < nw && flip[0] < 0; a++)
                        for (int b = a + 1; b < nw; b++)
                            if ((syn ^ bs[weak[a]] ^ bs[weak[b]]) == 0) { flip[0] = weak[a]; flip[1] = weak[b]; break; }
                    if (flip[0] < 0) return false;
                }
                for (int f : flip) if (f >= 0) m[f / 8] ^= (uint8_t)(0x80 >> (f % 8));
                if (syndrome(m, bytes) != 0) return false;
                corrected = true;
            }
            fr.icao = bits(m, 9, 24);
        } else if (df == 11) {
            if ((syn & 0xFFFF80) != 0) return false;   // IID in the low 7 bits
            fr.icao = bits(m, 9, 24);
        } else {
            // address / parity: the syndrome is the address; only aircraft we already know
            if (!aircraft.count(syn) || clock - aircraft[syn].lastSeen > 60.0) return false;
            fr.icao = syn;
        }
        std::copy(m, m + bytes, fr.m);
        fr.bytes = bytes;
        fr.df = df;
        fr.corrected = corrected;
        // signal: mean pulse level over the frame vs noise
        double pk = 0;
        for (int k = 0; k < nbits; k++) {
            const double s = t + (8.0 + k) * spu;
            pk += std::max(area(s, s + 0.5 * spu), area(s + 0.5 * spu, s + spu)) / (0.5 * spu);
        }
        pk /= nbits;
        fr.signalDb = (float)(20.0 * std::log10(std::max(1e-9, pk)));
        return true;
    }

    void handle(const Frame& fr) {
        goodFrames++;
        if (fr.corrected) fixedFrames++;
        // DF11 only creates an aircraft when it has been heard before or comes from a clean frame
        Aircraft& a = aircraft[fr.icao];
        a.icao = fr.icao;
        if (a.messages == 0) a.firstSeen = clock;
        a.messages++;
        a.lastSeen = clock;
        a.signalDb = a.signalDb < -99 ? fr.signalDb : a.signalDb * 0.8f + fr.signalDb * 0.2f;
        const uint8_t* m = fr.m;
        switch (fr.df) {
        case 4: case 20: {
            const int alt = altitude13(bits(m, 20, 13));
            if (alt != NO_ALT) { a.altFt = alt; a.altGnss = false; }
            break;
        }
        case 5: case 21: {
            char b[8];
            std::snprintf(b, sizeof(b), "%04X", id13ToModeA(bits(m, 20, 13)));
            a.squawk = b;
            break;
        }
        case 17: case 18:
            decodeES(m, a);
            break;
        default: break;
        }
        if (fr.df == 4 || fr.df == 5 || fr.df == 20 || fr.df == 21) {
            const unsigned fs = bits(m, 6, 3);
            a.onGround = fs == 1 || fs == 3;
        }
        if (onFrame) onFrame(fr, a);
    }

    void decodeES(const uint8_t* m, Aircraft& a) {
        const int tc = (int)bits(m, 33, 5);
        if (tc >= 1 && tc <= 4) {   // identification
            static const char* cs = "#ABCDEFGHIJKLMNOPQRSTUVWXYZ##### ###############0123456789######";
            std::string s;
            for (int k = 0; k < 8; k++) s += cs[bits(m, 41 + 6 * k, 6)];
            while (!s.empty() && (s.back() == ' ' || s.back() == '#')) s.pop_back();
            if (!s.empty()) a.callsign = s;
            const int cat = (int)bits(m, 38, 3);
            static const char* catA[] = {"", "light", "small", "large", "high vortex", "heavy", "high performance", "rotorcraft"};
            static const char* catB[] = {"", "glider", "lighter than air", "parachutist", "ultralight", "", "UAV", "space vehicle"};
            static const char* catC[] = {"", "emergency vehicle", "service vehicle", "obstacle", "obstacle", "obstacle", "", ""};
            a.category = tc == 4 ? catA[cat] : tc == 3 ? catB[cat] : tc == 2 ? catC[cat] : "";
        } else if ((tc >= 9 && tc <= 18) || (tc >= 20 && tc <= 22)) {   // airborne position
            const int alt = tc <= 18 ? altitude12(bits(m, 41, 12)) : NO_ALT;
            if (alt != NO_ALT) { a.altFt = alt; a.altGnss = false; }
            else if (tc >= 20) {   // GNSS height in metres
                const unsigned h = bits(m, 41, 12);
                if (h) { a.altFt = (int)std::lround(h * 3.28084); a.altGnss = true; }
            }
            a.onGround = false;
            cprFrame(a, (int)bits(m, 54, 1), (int)bits(m, 55, 17), (int)bits(m, 72, 17), false);
        } else if (tc >= 5 && tc <= 8) {   // surface position
            a.onGround = true;
            const unsigned mov = bits(m, 38, 7);
            if (mov == 1) a.speedKn = 0;
            else if (mov >= 2 && mov <= 124) {
                static const struct { unsigned lo; double base, step; } t[] = {
                    {2, 0.125, 0.125}, {9, 1, 0.25}, {13, 2, 0.5}, {39, 15, 1}, {94, 70, 2}, {109, 100, 5}};
                double v = 175;
                for (int k = 5; k >= 0; k--) if (mov >= t[k].lo) { v = t[k].base + (mov - t[k].lo) * t[k].step; break; }
                a.speedKn = v;
            }
            if (bits(m, 45, 1)) a.trackDeg = bits(m, 46, 7) * 360.0 / 128.0;
            cprFrame(a, (int)bits(m, 54, 1), (int)bits(m, 55, 17), (int)bits(m, 72, 17), true);
        } else if (tc == 19) {   // velocity
            const int st = (int)bits(m, 38, 3);
            if (st == 1 || st == 2) {
                const int vew = (int)bits(m, 47, 10), vns = (int)bits(m, 58, 10);
                if (vew && vns) {
                    const double k = st == 2 ? 4.0 : 1.0;
                    const double vx = (vew - 1) * k * (bits(m, 46, 1) ? -1 : 1);
                    const double vy = (vns - 1) * k * (bits(m, 57, 1) ? -1 : 1);
                    a.speedKn = std::hypot(vx, vy);
                    a.trackDeg = cprMod(std::atan2(vx, vy) * 180.0 / M_PI, 360.0);
                }
            } else if (st == 3 || st == 4) {
                if (bits(m, 46, 1)) a.headingDeg = bits(m, 47, 10) * 360.0 / 1024.0;
                const int as = (int)bits(m, 58, 10);
                if (as) a.speedKn = (as - 1) * (st == 4 ? 4.0 : 1.0);
                if (a.trackDeg < 0 && a.headingDeg >= 0) a.trackDeg = a.headingDeg;
            }
            const int vr = (int)bits(m, 70, 9);
            if (vr) { a.vrateFpm = (vr - 1) * 64 * (bits(m, 69, 1) ? -1 : 1); a.vrateValid = true; }
        }
    }

    void cprFrame(Aircraft& a, int odd, int latC, int lonC, bool surface) {
        a.cprLat[odd] = latC; a.cprLon[odd] = lonC; a.cprTime[odd] = clock; a.cprSurface[odd] = surface;
        double lat = 0, lon = 0;
        bool ok = false;
        if (!surface) {
            // global decode from a fresh pair, else local decode near the last position
            if (std::fabs(a.cprTime[0] - a.cprTime[1]) < 10.0 && !a.cprSurface[0] && !a.cprSurface[1])
                ok = cprGlobal(a.cprLat[0], a.cprLon[0], a.cprLat[1], a.cprLon[1], odd == 1, lat, lon);
            if (!ok && a.posValid && clock - a.posTime < 600.0) {
                cprLocal(latC, lonC, odd == 1, false, a.lat, a.lon, lat, lon);
                ok = distanceKm(lat, lon, a.lat, a.lon) < 50.0;
            }
            // no pair yet (short or weak pass): one frame is enough near the receiver.
            // A CPR zone is ~360 km tall, so the answer is unambiguous within ~280 km.
            // Confirmed by a frame of the other parity up to 60 s apart: with a wrong reference
            // (a recording made somewhere else) the even and odd answers land far apart.
            if (!ok && !a.posValid && refValid) {
                cprLocal(latC, lonC, odd == 1, false, refLat, refLon, lat, lon);
                if (distanceKm(lat, lon, refLat, refLon) < 280.0) {
                    a.refLat[odd] = lat; a.refLon[odd] = lon; a.refTime[odd] = clock;
                    const int o = 1 - odd;
                    if (clock - a.refTime[o] < 60.0 && distanceKm(lat, lon, a.refLat[o], a.refLon[o]) < 20.0) {
                        a.lat = lat; a.lon = lon; a.posValid = true; a.posTime = clock; a.posFromRef = true;
                    }
                }
                return;
            }
            // a global fix replaces a receiver-relative one (it may come from an old recording far away)
            if (ok && a.posFromRef && std::fabs(a.cprTime[0] - a.cprTime[1]) < 10.0) { a.posValid = false; a.posFromRef = false; }
        } else {
            // surface: 90 degree zones, needs a reference (the aircraft or the receiver)
            if (a.posValid && clock - a.posTime < 600.0) { cprLocal(latC, lonC, odd == 1, true, a.lat, a.lon, lat, lon); ok = true; }
            else if (refValid) { cprLocal(latC, lonC, odd == 1, true, refLat, refLon, lat, lon); ok = distanceKm(lat, lon, refLat, refLon) < 45.0; }
        }
        if (!ok) return;
        // sanity: no jumps faster than 1500 km/h, nothing beyond radio range of the receiver
        if (a.posValid && clock - a.posTime < 120.0) {
            const double dt = std::max(1.0, clock - a.posTime);
            if (distanceKm(lat, lon, a.lat, a.lon) / dt > 0.42 + 1.0 / dt) return;
        }
        // (no "too far from the receiver" test: recordings may come from anywhere, and a
        //  global decode with matching zones is unambiguous)
        a.lat = lat; a.lon = lon; a.posValid = true; a.posTime = clock;
    }
};

} // namespace adsb
