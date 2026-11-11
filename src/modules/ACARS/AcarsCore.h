#pragma once

#include <vector>
#include <string>
#include <complex>
#include <cmath>
#include <cstdint>
#include <functional>
#include <algorithm>

// =====================================================================
//  ACARS (Aircraft Communications Addressing and Reporting System)
//
//  VHF AM, 2400 bit/s MSK (tones 1200 / 2400 Hz) on the AM audio.
//  Frame (ARINC 618): pre-key, '+' '*', SYN SYN, SOH, mode (1), aircraft
//  address (7), ACK (1), label (2), block id (1), [STX text], ETX or ETB,
//  BCS (CRC-16, 2 bytes), DEL. Characters: 7-bit ASCII + odd parity,
//  sent LSB first. The CRC (CRC-16/KERMIT) covers mode .. ETX/ETB.
//
//  Demodulator: audio -> mix the 1800 Hz centre to 0 Hz, then
//   - FM discriminator (+600 Hz = 2400 Hz tone, -600 Hz = 1200 Hz tone):
//     drives the bit clock (tone changes sit on bit boundaries); the
//     1200 Hz tone also means "this bit differs from the previous one",
//     which gives a simple non-coherent decoder,
//   - coherent detection (MSK = OQPSK with half-sine pulses): matched
//     filter, carrier phase loop, bits alternately on the I and Q axis.
//     Errors stay single bits here, so it is the main path.
//  The polarity / alignment ambiguities are covered by running several
//  parsers; only frames with a valid CRC are accepted.
// =====================================================================

namespace acars {

struct Message {
    char mode = 0;
    std::string reg;        // aircraft registration (address without the leading dots)
    char ack = 0;           // '!' = NAK
    std::string label;      // 2 characters ("_d" for the 0x7F label)
    char blockId = 0;
    bool downlink = false;  // aircraft -> ground (block id is a digit)
    std::string msn, flight;
    std::string text;
    bool more = false;      // ended with ETB: more blocks follow
    int fixedBits = 0;      // bits corrected with the parity + CRC
};

inline const char* labelDescription(const std::string& l) {
    struct L { const char* code; const char* text; };
    static const L table[] = {
        {"_d", "General response / no information"},
        {"H1", "Message to/from terminal (CMU)"},
        {"Q0", "Link test"},
        {"SQ", "Squitter (ground station beacon)"},
        {"SA", "Media advisory"},
        {"5Z", "Airline designated downlink"},
        {"QA", "OUT / fuel report"},
        {"QB", "OFF report"},
        {"QC", "ON report"},
        {"QD", "IN / fuel report"},
        {"AA", "ATC communications (FANS CPDLC)"},
        {"BA", "ATC communications (FANS CPDLC)"},
        {"A6", "ADS-C request"},
        {"B6", "ADS-C report"},
        {"C1", "Uplink to cockpit printer"},
    };
    for (const L& e : table) if (l == e.code) return e.text;
    return "";
}

// --- CRC-16/KERMIT (reflected 0x1021, init 0) ---
inline uint16_t crcUpdate(uint16_t crc, uint8_t b) {
    crc ^= b;
    for (int i = 0; i < 8; i++) crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0x8408) : (uint16_t)(crc >> 1);
    return crc;
}

inline bool oddParity(uint8_t b) {
    int n = 0;
    for (int i = 0; i < 8; i++) n += (b >> i) & 1;
    return (n & 1) != 0;
}

// --- frame parser for one bit stream ---
class Parser {
    static constexpr uint8_t SYN = 0x16, SOH = 0x01, ETX = 0x83, ETB = 0x97;   // with parity
    static constexpr size_t MAX_LEN = 250;
    enum State { WSYN, SYN2, WSOH, TXT, CRC1, CRC2 } state = WSYN;
    uint8_t outbits = 0;
    int nbits = 1;
    std::vector<uint8_t> buf;
    std::vector<size_t> parityErr;
    uint8_t crcBytes[2] = {0, 0};

    bool crcOk() const {
        uint16_t c = 0;
        for (uint8_t b : buf) c = crcUpdate(c, b);
        c = crcUpdate(c, crcBytes[0]);
        c = crcUpdate(c, crcBytes[1]);
        return c == 0;
    }

    void resync() { state = WSYN; nbits = 1; }

    bool finish(Message& m) {
        int fixed = 0;
        bool ok = crcOk();
        // One character with a parity error: try each of its bits
        if (!ok && parityErr.size() == 1) {
            uint8_t& b = buf[parityErr[0]];
            for (int bit = 0; bit < 8 && !ok; bit++) {
                b ^= (uint8_t)(1 << bit);
                ok = crcOk();
                if (!ok) b ^= (uint8_t)(1 << bit);
            }
            fixed = ok ? 1 : 0;
        }
        if (!ok || buf.size() < 13) return false;
        for (uint8_t b : buf) if (!oddParity(b)) return false;   // still broken somewhere

        auto ch = [&](size_t i) { return (char)(buf[i] & 0x7f); };
        m = Message();
        m.fixedBits = fixed;
        m.mode = ch(0);
        for (size_t i = 1; i <= 7; i++) m.reg += ch(i);
        m.reg.erase(0, m.reg.find_first_not_of('.'));
        m.ack = ch(8) == 0x15 ? '!' : ch(8);
        m.label += ch(9);
        m.label += ch(10) == 0x7f ? 'd' : ch(10);
        m.blockId = ch(11);
        m.more = buf.back() == ETB;
        if (ch(12) == 0x02) {
            for (size_t i = 13; i + 1 < buf.size(); i++) {
                char c = ch(i);
                if (c == '\r') continue;
                m.text += (c == '\n' || (c >= 0x20 && c < 0x7f)) ? c : '.';
            }
        }
        m.downlink = m.blockId >= '0' && m.blockId <= '9';
        if (m.downlink && m.text.size() >= 10) {
            m.msn = m.text.substr(0, 4);
            m.flight = m.text.substr(4, 6);
            m.text = m.text.substr(10);
        }
        return true;
    }

public:
    // Feeds one bit; returns true when a complete, valid message was decoded into 'm'
    bool pushBit(int bit, Message& m) {
        outbits = (uint8_t)((outbits >> 1) | (bit ? 0x80 : 0));
        if (--nbits > 0) return false;
        nbits = 8;
        switch (state) {
        case WSYN:
            if (outbits == SYN) state = SYN2; else nbits = 1;
            return false;
        case SYN2:
            if (outbits == SYN) state = WSOH; else resync();
            return false;
        case WSOH:
            if (outbits == SOH) { state = TXT; buf.clear(); parityErr.clear(); }
            else if (outbits != SYN) resync();
            return false;
        case TXT:
            if (!oddParity(outbits)) {
                parityErr.push_back(buf.size());
                if (parityErr.size() > 4) { resync(); return false; }
            }
            buf.push_back(outbits);
            if (outbits == ETX || outbits == ETB) state = CRC1;
            else if (buf.size() > MAX_LEN) resync();
            return false;
        case CRC1:
            crcBytes[0] = outbits;
            state = CRC2;
            return false;
        case CRC2: {
            crcBytes[1] = outbits;
            bool ok = finish(m);
            resync();
            return ok;
        }
        }
        return false;
    }
};

// --- MSK demodulator on AM audio ---
class Demod {
    using C = std::complex<double>;
    static constexpr double PI_D = 3.14159265358979323846;
    double rate = 0.0;
    double dcAvg = 0.0, dcAlpha = 0.0;
    C osc = {1.0, 0.0}, oscStep = {1.0, 0.0};
    // discriminator path (bit clock): low-pass ~1500 Hz
    std::vector<double> lpf;
    std::vector<C> hist;
    size_t hpos = 0;
    C prev = {0.0, 0.0};
    double fscale = 1.0;
    // coherent path: half-sine matched filter (2 bits long) + carrier phase loop
    std::vector<double> mf;
    std::vector<C> mhist;
    size_t mpos = 0;
    static constexpr size_t RING = 512;
    C ring[RING];
    uint64_t sampleIdx = 0;
    C carrier = {1.0, 0.0};   // estimated carrier phase (unit vector)
    unsigned bitIdx = 0;
    // clock
    double phase = 0.0, phaseStep = 0.0;
    double lastF = 0.0;
    double acc = 0.0;
    int integ[2] = {0, 0};
    Parser parsers[6];
    std::vector<std::string> recent;   // drop the same frame decoded by two paths

    static std::vector<double> lowPass(double cutoffHz, double rate, int taps) {
        std::vector<double> h(taps);
        const double fc = cutoffHz / rate;
        double sum = 0.0;
        for (int i = 0; i < taps; i++) {
            int m = i - taps / 2;
            double s = m == 0 ? 2.0 * fc : std::sin(2.0 * PI_D * fc * m) / (PI_D * m);
            double w = 0.42 - 0.5 * std::cos(2.0 * PI_D * i / (taps - 1)) + 0.08 * std::cos(4.0 * PI_D * i / (taps - 1));
            h[i] = s * w; sum += h[i];
        }
        for (double& v : h) v /= sum;
        return h;
    }

    static C fir(std::vector<C>& hb, size_t& pos, const std::vector<double>& h, C x) {
        const size_t n = h.size();
        hb[pos] = x; hb[pos + n] = x;
        pos = (pos + 1) % n;
        C y(0.0, 0.0);
        const C* p = &hb[pos];
        for (size_t k = 0; k < n; k++) y += p[k] * h[k];
        return y;
    }

    void decide(double frac) {
        // The bit boundary lies 'frac' samples before the current discriminator sample,
        // which is delayed by the low-pass. Look up the matched filter output centred there.
        const double dl = (lpf.size() - 1) / 2.0, dm = (mf.size() - 1) / 2.0;
        const double m = (double)sampleIdx - dl - frac + dm;
        if (m < 1.0) return;
        const double fl = std::floor(m);
        const double t = m - fl;
        const uint64_t i0 = (uint64_t)fl;
        const C y = ring[i0 % RING] * (1.0 - t) + ring[(i0 + 1) % RING] * t;

        // OQPSK view of MSK: the bits alternate between the I and Q axis
        const C v = y * std::conj(carrier);
        const double mag = std::abs(v) + 1e-12;
        double d, err;
        if (bitIdx & 1) { d = v.imag(); err = -v.real() * (d >= 0 ? 1.0 : -1.0); }
        else            { d = v.real(); err =  v.imag() * (d >= 0 ? 1.0 : -1.0); }
        carrier *= std::polar(1.0, loopGain * err / mag);
        carrier /= std::abs(carrier);
        const int b = d > 0.0 ? 1 : 0;
        // the half-sine pulses alternate in sign every second bit: pattern ++-- (4 alignments)
        for (int s = 0; s < 4; s++) {
            const int flip = ((bitIdx + s) & 2) ? 1 : 0;
            Message msg;
            if (parsers[s].pushBit(b ^ flip, msg)) deliver(msg, s);
        }
        bitIdx++;
    }

public:
    std::function<void(const Message&)> onMessage;
    double loopGain = 0.08;
    int statsPath[6] = {0, 0, 0, 0, 0, 0};   // messages per parser (for testing)

    void configure(double sampleRate) {
        if (sampleRate == rate) return;
        rate = sampleRate;
        dcAlpha = 1.0 / (0.01 * rate);
        oscStep = std::polar(1.0, -2.0 * PI_D * 1800.0 / rate);
        const double spb = rate / 2400.0;   // samples per bit
        int taps = std::clamp((int)std::lround(spb * 4.0) | 1, 15, 255);
        lpf = lowPass(1500.0, rate, taps);
        hist.assign(lpf.size() * 2, C(0.0, 0.0));
        hpos = 0;
        int mtaps = std::max(5, (int)std::lround(2.0 * spb) | 1);
        mf.assign(mtaps, 0.0);
        double sum = 0.0;
        for (int i = 0; i < mtaps; i++) {
            const double t = (i - (mtaps - 1) / 2.0) / spb;   // in bits, -1 .. 1
            mf[i] = std::cos(PI_D * t / 2.0);
            sum += mf[i];
        }
        for (double& v : mf) v /= sum;
        mhist.assign(mf.size() * 2, C(0.0, 0.0));
        mpos = 0;
        for (auto& r : ring) r = C(0.0, 0.0);
        sampleIdx = 0;
        fscale = rate / (2.0 * PI_D * 600.0);   // +-1.0 = +-600 Hz
        phaseStep = 1.0 / spb;
        phase = 0.0; acc = 0.0;
    }

    double sampleRate() const { return rate; }

    void process(const float* x, size_t n) {
        if (rate <= 0.0) return;
        for (size_t i = 0; i < n; i++) {
            const double s = x[i];
            dcAvg += (s - dcAvg) * dcAlpha;
            const C c = (s - dcAvg) * osc;
            osc *= oscStep;

            ring[sampleIdx % RING] = fir(mhist, mpos, mf, c);
            const C y = fir(hist, hpos, lpf, c);
            const double f = std::arg(y * std::conj(prev)) * fscale;
            prev = y;

            // bit clock: a tone change belongs on a bit boundary (phase 0)
            if ((f > 0.0) != (lastF > 0.0)) {
                const double e = phase < 0.5 ? phase : phase - 1.0;
                phase -= 0.25 * e;
                if (phase < 0.0) phase += 1.0;
            }
            lastF = f;

            acc += std::clamp(f, -2.0, 2.0);
            phase += phaseStep;
            if (phase >= 1.0) {
                phase -= 1.0;
                // non-coherent path: the 1200 Hz tone means "the bit changed"
                const int toneHigh = acc > 0.0 ? 1 : 0;
                acc = 0.0;
                for (int p = 0; p < 2; p++) {
                    integ[p] ^= (toneHigh ^ 1);
                    Message msg;
                    if (parsers[4 + p].pushBit(integ[p] ^ p, msg)) deliver(msg, 4 + p);
                }
                // coherent path
                decide(phase / phaseStep);
            }
            sampleIdx++;
        }
        osc /= std::abs(osc);
    }

private:
    void deliver(const Message& m, int path) {
        statsPath[path]++;
        std::string key = m.reg + "|" + m.label + "|" + m.blockId + "|" + m.msn + "|" + m.text;
        if (std::find(recent.begin(), recent.end(), key) != recent.end()) return;
        recent.push_back(key);
        if (recent.size() > 8) recent.erase(recent.begin());
        if (onMessage) onMessage(m);
    }
};

} // namespace acars
