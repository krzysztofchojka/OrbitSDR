#pragma once

#include <vector>
#include <complex>
#include <cstdint>
#include <cmath>
#include <array>
#include <functional>
#include <algorithm>

// =====================================================================
//  TETRA downlink physical layer: pi/4-DQPSK demodulator.
//
//  This is the ONE genuinely new piece of DSP for TETRA support. The
//  tetra-kit decoder we port (deps/tetra-kit) does everything from hard
//  bits upward (burst sync, lower MAC channel coding, MAC/LLC/MLE/MM/
//  CMCE/SDS/LIP) but assumes an external GNU Radio flowgraph for the
//  demod. OrbitSDR can't host GNU Radio, so this header reproduces that
//  front end natively and feeds the decoder the same stream it expects:
//  one hard bit per call of a sink, 36 kbit/s (18 ksym/s x 2 bits).
//
//  TETRA, EN 300 392-2:
//    - 18000 sym/s, pi/4-DQPSK, root-raised-cosine, roll-off 0.35
//    - 25 kHz channel, continuous downlink burst, 510 bits per burst
//    - dibit -> phase transition (clause 5.9):
//          0 0 -> +pi/4     0 1 -> +3pi/4
//          1 0 -> -pi/4     1 1 -> -3pi/4
//
//  Chain:  AGC -> matched RRC (at N samples/symbol) -> Gardner timing
//  recovery (-> 1 complex sample/symbol) -> differential detection with
//  an FLL that removes the residual carrier offset -> dibit decision ->
//  2 hard bits.
//
//  Bit-ordering / rotation-sign ambiguity (which of the two bits is sent
//  first, and the sign of the rotation after front-end mixing) is
//  resolved by the sync layer: TetraSync tries the mapping variants
//  against TETRA's known training sequences and locks onto the one that
//  matches, so the demod never has to guess the convention.
// =====================================================================

namespace tetra {

// ---- TETRA constants -------------------------------------------------
static constexpr double kPi         = 3.14159265358979323846;   // M_PI is not portable (MSVC)
static constexpr double SYMBOL_RATE = 18000.0;
static constexpr int    BURST_BITS  = 510;        // downlink burst length

// Root-raised-cosine impulse response, `sps` samples per symbol, `span`
// symbols each side. Normalised to unit energy.
inline std::vector<double> makeRRC(int sps, int span, double beta) {
    const int n = 2 * sps * span + 1;
    std::vector<double> h(n);
    const double Ts = 1.0;                         // symbol period in "symbol" units
    double sum2 = 0.0;
    for (int i = 0; i < n; i++) {
        const double t = (double)(i - n / 2) / sps; // in symbols
        double v;
        if (std::abs(t) < 1e-8) {
            v = 1.0 + beta * (4.0 / kPi - 1.0);
        } else if (beta > 0.0 && std::abs(std::abs(4.0 * beta * t / Ts) - 1.0) < 1e-6) {
            const double a = kPi / (4.0 * beta);
            v = (beta / std::sqrt(2.0)) *
                ((1.0 + 2.0 / kPi) * std::sin(a) + (1.0 - 2.0 / kPi) * std::cos(a));
        } else {
            const double pt = kPi * t;
            v = (std::sin(pt * (1.0 - beta)) + 4.0 * beta * t * std::cos(pt * (1.0 + beta))) /
                (pt * (1.0 - (4.0 * beta * t) * (4.0 * beta * t)));
        }
        h[i] = v;
        sum2 += v * v;
    }
    const double norm = 1.0 / std::sqrt(sum2);
    for (double& v : h) v *= norm;
    return h;
}

// =====================================================================
//  Demodulator: complex baseband -> symbol-rate samples + differential
//  phases. It does NOT commit to a bit convention; it hands each symbol's
//  differential phase to a sink, and the sync layer turns phases into
//  bits once the convention is resolved.
// =====================================================================
class TetraDemod {
    using Cf = std::complex<float>;
    using Cd = std::complex<double>;

    // config
    double fs = 0.0;
    int    sps = 4;                   // samples/symbol after resampling (input is fs = sps*18000)
    std::vector<double> rrc;

    // AGC
    double agc = 1.0;

    // matched filter history (circular)
    std::vector<Cd> mfHist;
    size_t mfPos = 0;

    // Gardner timing recovery (works on matched-filtered stream)
    double mu = 0.0;                  // fractional sample offset [0,1)
    double timErr = 0.0;
    Cd  prevSample {0, 0};            // last matched-filter output (for interpolation)
    bool havePrev = false;
    int  phaseCnt = 0;                // 0..sps-1 position within a symbol (nominal)
    Cd  symPrev {1, 0};              // previous *symbol* strobe (for Gardner mid-point)
    Cd  symMid  {0, 0};             // the half-symbol strobe
    bool haveSym = false;

    // differential detection + FLL
    Cd  lastSym {1, 0};
    double freqAcc = 0.0;             // tracked carrier offset (rad/symbol)
    double fllBeta = 0.003;

    // ideal pi/4-DQPSK phase transitions
    static constexpr double kIdeal[4] = { kPi / 4, 3 * kPi / 4, -3 * kPi / 4, -kPi / 4 };

    static double wrap(double a) {
        while (a >  kPi) a -= 2 * kPi;
        while (a < -kPi) a += 2 * kPi;
        return a;
    }

    // nearest ideal transition; returns quadrant index 0..3 and the wrapped error
    static int nearestQuadrant(double ph, double& err) {
        int best = 0; double bestD = 1e9;
        for (int q = 0; q < 4; q++) {
            const double d = std::abs(wrap(ph - kIdeal[q]));
            if (d < bestD) { bestD = d; best = q; }
        }
        err = wrap(ph - kIdeal[best]);
        return best;
    }

public:
    // Called once per recovered symbol with:
    //   quadrant: index 0..3 of the nearest ideal pi/4-DQPSK transition
    //   phaseErr: residual phase error after FLL (for quality metrics)
    //   strobe:   the symbol-rate complex sample (for a constellation view)
    std::function<void(int quadrant, double phaseErr, std::complex<float> strobe)> onSymbol;

    double evmAcc = 0.0;              // running mean |phaseErr| (lower is better)
    double carrierHz = 0.0;          // estimated residual carrier offset

    void configure(double inputRate) {
        // pick sps so the input rate is an integer-ish multiple of the symbol rate
        sps = std::max(2, (int)std::lround(inputRate / SYMBOL_RATE));
        fs = inputRate;
        rrc = makeRRC(sps, 6, 0.35);
        mfHist.assign(rrc.size(), Cd{0, 0});
        mfPos = 0;
        reset();
    }

    void reset() {
        agc = 1.0;
        std::fill(mfHist.begin(), mfHist.end(), Cd{0, 0});
        mfPos = 0;
        mu = 0.0; timErr = 0.0; havePrev = false; haveSym = false;
        phaseCnt = 0; symPrev = Cd{1, 0}; lastSym = Cd{1, 0};
        freqAcc = 0.0; evmAcc = 0.0; carrierHz = 0.0;
    }

    bool ready() const { return fs > 0.0 && !rrc.empty(); }

    void process(const Cf* in, size_t n) {
        if (!ready()) return;
        const size_t taps = rrc.size();
        for (size_t i = 0; i < n; i++) {
            // --- AGC: track envelope, normalise ---
            Cd x(in[i].real(), in[i].imag());
            const double mag = std::abs(x);
            agc += 0.0005 * (1.0 - agc * mag);
            if (agc < 1e-6) agc = 1e-6;
            x *= agc;

            // --- matched RRC filter ---
            mfHist[mfPos] = x;
            Cd y{0, 0};
            size_t p = mfPos;
            for (size_t k = 0; k < taps; k++) {
                y += mfHist[p] * rrc[k];
                p = (p == 0) ? taps - 1 : p - 1;
            }
            mfPos = (mfPos + 1) % taps;

            // --- symbol timing: strobe every `sps` samples, nudged by Gardner ---
            // We take a mid-symbol sample (half way) and a full-symbol sample.
            feedTiming(y);
        }
    }

private:
    Cd prevMF {0, 0};
    Cd midMF  {0, 0};
    double sampleClock = 0.0;         // counts input samples, strobes near multiples of sps/2

    void feedTiming(const Cd& y) {
        // Two strobes per symbol for Gardner: at sampleClock crossing sps/2 and sps.
        sampleClock += 1.0;
        const double half = sps / 2.0;

        // linear-interpolate using mu for sub-sample timing
        // (simple, works because matched filter oversamples >= 2x)
        if (!havePrev) { prevMF = y; havePrev = true; return; }

        // mid-symbol strobe
        if (sampleClock >= half && (sampleClock - 1.0) < half) {
            const double frac = half - (sampleClock - 1.0);
            midMF = prevMF + (y - prevMF) * frac;
        }
        // full-symbol strobe
        if (sampleClock >= sps) {
            const double frac = sps - (sampleClock - 1.0);
            const Cd cur = prevMF + (y - prevMF) * std::clamp(frac, 0.0, 1.0);

            // Gardner timing error detector
            const double e = (cur.real() - symPrev.real()) * midMF.real() +
                             (cur.imag() - symPrev.imag()) * midMF.imag();
            timErr = e;
            // adjust the clock a touch toward the optimum
            sampleClock -= sps;
            sampleClock += 0.01 * e;        // timing loop gain
            sampleClock = std::clamp(sampleClock, -1.0, 1.0);

            emitSymbol(cur);
            symPrev = cur;
        }
        prevMF = y;
    }

    void emitSymbol(const Cd& sym) {
        // --- differential detection ---
        Cd diff = sym * std::conj(lastSym);
        lastSym = sym;
        double ph = std::atan2(diff.imag(), diff.real());

        // remove tracked carrier offset (FLL), then decide
        ph = wrap(ph - freqAcc);
        double err;
        const int q = nearestQuadrant(ph, err);

        // FLL: pull the running offset toward the residual error
        freqAcc = wrap(freqAcc + fllBeta * err);
        carrierHz = freqAcc * SYMBOL_RATE / (2.0 * kPi);

        // quality
        evmAcc += 0.01 * (std::abs(err) - evmAcc);

        if (onSymbol) onSymbol(q, err, std::complex<float>((float)sym.real(), (float)sym.imag()));
    }
};

// =====================================================================
//  Sync + bit mapping. Turns the stream of quadrant indices into hard
//  bits, resolving the convention ambiguity against TETRA's training
//  sequences, and reports burst lock. Downstream, the resolved bit sink
//  feeds the ported tetra-kit TetraDecoder::rxSymbol().
// =====================================================================
class TetraSync {
    // The begin/end of NORMAL_TRAINING_SEQ_3, as in tetra-kit decoder.cc:
    // a matched burst has these at positions 0 and 500 of the 510-bit window.
    const std::array<uint8_t, 12> TS3_BEGIN = {0,0,0,1,1,0,1,0,1,1,0,1};
    const std::array<uint8_t, 10> TS3_END   = {1,0,1,1,0,1,1,1,0,0};

    // Four candidate quadrant->dibit maps: 2 rotation signs x 2 bit orders.
    // Gray mapping of pi/4-DQPSK transitions (TETRA clause 5.9):
    //   +pi/4 -> 00, +3pi/4 -> 01, -3pi/4 -> 11, -pi/4 -> 10  (quadrant order in kIdeal)
    // Variants cover the sign flip (spectrum inversion) and bit-order swap.
    static const uint8_t MAP[4][4][2];

    int  variant = -1;                // resolved convention, -1 = still searching
    std::vector<uint8_t> win;         // sliding 510-bit window of hard bits (resolved variant)
    // variant search: the last BURST_BITS bits under each of the 4 mappings (ring buffers
    // sharing one write position); every new bit completes exactly one new candidate window
    uint8_t srch[4][BURST_BITS] = {};
    int  srchPos = 0, srchFill = 0;
    int  sinceLock = 1 << 30;
    int  bursts = 0;

public:
    // hard-bit sink: receives one resolved downlink bit at a time
    std::function<void(uint8_t bit)> onBit;
    // called when a burst is matched, with the raw bit-error score at the sync marks
    std::function<void(int score)> onBurst;

    bool   locked() const { return variant >= 0 && sinceLock < BURST_BITS * 4; }
    int    conventionVariant() const { return variant; }
    int    burstCount() const { return bursts; }

    void reset() { variant = -1; win.clear(); srchPos = 0; srchFill = 0; sinceLock = 1 << 30; bursts = 0; }

    void pushQuadrant(int q) {
        if (variant < 0) { searchVariant(q); return; }

        // emit the two bits of this symbol under the resolved variant
        const uint8_t* b = MAP[variant][q];
        pushBit(b[0]);
        pushBit(b[1]);
    }

private:
    void pushBit(uint8_t bit) {
        if (onBit) onBit(bit);
        win.push_back(bit);
        if ((int)win.size() > BURST_BITS) win.erase(win.begin());
        sinceLock++;
        if ((int)win.size() == BURST_BITS) checkBurst();
    }

    void checkBurst() {
        int sBegin = 0, sEnd = 0;
        for (size_t i = 0; i < TS3_BEGIN.size(); i++) sBegin += win[i] ^ TS3_BEGIN[i];
        for (size_t i = 0; i < TS3_END.size();   i++) sEnd   += win[500 + i] ^ TS3_END[i];
        if (sBegin == 0 && sEnd < 2) {
            bursts++;
            sinceLock = 0;
            if (onBurst) onBurst(sBegin + sEnd);
        }
    }

    // Before lock: derive the bits under every variant and test, for each new bit, the one
    // 510-bit window that ends there against the TS3 marks. Same alignments as a full
    // re-scan of the window would find, but O(1) per bit instead of O(window) per symbol.
    bool marksAt(const uint8_t* buf, int start) const {
        int sB = 0, sE = 0;
        for (size_t i = 0; i < TS3_BEGIN.size(); i++) {
            int k = start + (int)i; if (k >= BURST_BITS) k -= BURST_BITS;
            if (buf[k] != TS3_BEGIN[i]) return false;           // the begin mark must be exact
        }
        for (size_t i = 0; i < TS3_END.size(); i++) {
            int k = start + 500 + (int)i; if (k >= BURST_BITS) k -= BURST_BITS;
            sE += buf[k] ^ TS3_END[i];
            if (sE >= 2) return false;
        }
        return sB == 0;
    }

    void searchVariant(int q) {
        for (int h = 0; h < 2; h++) {
            for (int v = 0; v < 4; v++) srch[v][srchPos] = MAP[v][q][h];
            if (++srchPos == BURST_BITS) srchPos = 0;
            if (srchFill < BURST_BITS) { if (++srchFill < BURST_BITS) continue; }
            // window: oldest bit at srchPos (the next to be overwritten)
            for (int v = 0; v < 4; v++) {
                if (marksAt(srch[v], srchPos)) {
                    variant = v; sinceLock = 0; bursts = 1; srchFill = 0; srchPos = 0;
                    return;
                }
            }
        }
    }
};

// Quadrant order matches TetraDemod::kIdeal = {+pi/4, +3pi/4, -3pi/4, -pi/4}.
// v0: direct Gray;  v1: bit-order swapped;  v2: sign-inverted;  v3: both.
inline const uint8_t TetraSync::MAP[4][4][2] = {
    { {0,0},{0,1},{1,1},{1,0} },   // v0
    { {0,0},{1,0},{1,1},{0,1} },   // v1 (swap bits)
    { {1,0},{1,1},{0,1},{0,0} },   // v2 (sign invert = reverse quadrant order)
    { {0,1},{1,1},{1,0},{0,0} },   // v3
};

} // namespace tetra
