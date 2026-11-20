#pragma once

#include <vector>
#include <deque>
#include <complex>
#include <string>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <numeric>
#include <functional>
#include "../../DSP.h"   // fft()

// =====================================================================
//  Signal analyzer: says what kind of signal is in a channel.
//
//  Input: complex baseband of the channel (any rate, the channel is
//  about +-rate/2.5 wide). The analyzer
//
//   1. finds the transmissions (bursts): power against a noise floor
//      from minimum statistics; a signal that never stops is analysed
//      in windows of 0.5 s,
//   2. measures every burst:
//        - level / SNR, carrier offset, occupied bandwidth,
//        - envelope: constant (FM, FSK, GMSK) or not (AM, OOK, PSK, QAM),
//        - symbol rate: spectral line of the FM discriminator's
//          transitions (FSK) or of the envelope (OOK, PSK, QAM),
//        - FSK: number of levels (k-means on the discriminator at the
//          symbol instants) and their frequencies -> 2/4/8FSK, GMSK,
//        - PSK: spectral line of s^M (M = 2, 4, 8) -> BPSK/QPSK/8PSK,
//   3. slices FSK / OOK to symbols, looks for known sync words
//      (DMR, dPMR, NXDN, P25, YSF, D-STAR, POCSAG), a preamble, and the
//      part of the burst that repeats from burst to burst (a sync word
//      or header of an unknown protocol),
//   4. times the bursts: length, period, regularity.
//
//  No GUI here: the module (Analyzer_Decoder.h) draws the report.
// =====================================================================

namespace an {

using CF = std::complex<float>;
using CD = std::complex<double>;
constexpr double PI_D = 3.14159265358979323846;

enum class Family { None, Noise, Carrier, AnalogFM, AnalogAM, FSK, OOK, PSK, Linear };

struct SyncHit {
    std::string name;
    int pos = -1;        // first symbol of the first hit
    int count = 0;       // hits in this burst
    int errors = 0;      // mismatching symbols of the first hit
    bool inverted = false;
};

struct Report {
    bool valid = false;
    int seq = 0;
    double tStart = 0.0;          // seconds since the analyzer started (stream time)
    double durMs = 0.0;
    bool continuous = false;      // no bursts: a window of a signal that does not stop
    bool truncated = false;       // part of a transmission longer than one capture
    double rate = 0.0;            // sample rate of the analysed baseband
    double levelDb = 0.0, floorDb = 0.0, snrDb = 0.0;
    double offsetHz = 0.0;        // centre of the signal from the channel centre
    double bwHz = 0.0;            // occupied bandwidth (-26 dB)
    double envCv = 0.0;           // std / mean of the envelope
    double symRate = 0.0, symRateRaw = 0.0, symLine = 0.0;   // symLine: line / median (x)
    double powLine[3] = {0, 0, 0};                           // s^2, s^4, s^8 line / median (x)
    double envLine = 0.0, envLineHz = 0.0;                   // strongest line of |s|^2
    double modIndex = 0.0;        // FSK: 2 * outer deviation / symbol rate (2FSK); h
    Family family = Family::None;
    int levels = 0;
    std::vector<double> levelHz;  // FSK: frequency of each level, from the signal's centre
    double devHz = 0.0;           // FSK: outer deviation, analog FM: rms deviation
    double fit = 0.0;             // FSK / OOK: how well the levels explain the samples (0..1)
    double separation = 0.0;      // FSK: smallest gap between levels / their spread (> 2.2: clean)
    bool periodicTone = false;    // FM whose "symbols" just repeat: a tone, not data
    std::string modulation;       // short: "4FSK 4800 Bd"
    std::vector<std::string> notes;     // measurements, one per line
    std::vector<std::string> guesses;   // what it may be
    std::vector<uint8_t> symbols; // 0 .. levels-1 (0 = lowest frequency / no carrier)
    std::vector<SyncHit> syncs;
    std::string preamble;         // one period of the preamble pattern
    int preambleStart = 0, preambleLen = 0;   // symbols
    int fixedPos = -1, fixedLen = 0;   // part repeated in the earlier bursts
    int comparedBursts = 0;
    // plots
    std::vector<float> hist;      // FSK: discriminator at the symbol instants; else the whole discriminator
    float histLo = 0.0f, histHi = 0.0f;
    std::vector<float> eye;       // traces of eyeLen points (discriminator over two symbols)
    int eyeLen = 0;
    std::vector<CF> constel;      // linear modulations: symbol instants, derotated
};

struct BurstMark { double t = 0.0, dur = 0.0; };

// ---------------------------------------------------------------------
//  Known sync words: the sign of each symbol (dsdcc's tables, deps/dsdcc/dsd_sync.cpp,
//  1 = positive deviation, 3 = negative) and how many may be wrong.
// ---------------------------------------------------------------------
struct KnownSync { const char* name; int levels; const char* pattern; int tol; };
inline const KnownSync KNOWN_SYNCS[] = {
    {"DMR BS data",   4, "313333111331131131331131", 2},
    {"DMR BS voice",  4, "131111333113313313113313", 2},
    {"DMR MS data",   4, "311131133313133331131113", 2},
    {"DMR MS voice",  4, "133313311131311113313331", 2},
    {"dPMR FS1",      4, "111333331133131131111313", 2},
    {"dPMR FS4",      4, "333111113311313313333131", 2},
    {"NXDN",          4, "3131133313131331131",     1},
    {"YSF",           4, "31111311313113131131",    1},
    {"P25 phase 1",   4, "111113113311333313133333", 2},
    {"D-STAR header", 2, "131313131333133113131111", 2},
    {"D-STAR",        2, "313131313133131113313111", 2},
    // POCSAG frame sync codeword 0x7CD215D8 (1 = negative deviation is tested as "inverted")
    {"POCSAG",        2, "13333311331311311113131333133111", 2},
};

// One transmission (or a window of a steady signal) cut out by the detector, with everything
// the analysis needs: it can run on another thread while the detector goes on.
struct Capture {
    std::vector<CF> s;
    double t0 = 0.0, rate = 0.0;
    double floorDb = -200.0;
    bool haveFloor = false, continuous = false, continuation = false, truncated = false;
    double period = 0.0, jitter = 0.0;   // of the bursts so far
};

class Analyzer {
    double fs = 0.0;
    size_t blockLen = 1;
    // burst detector
    double blockAcc = 0.0;
    size_t blockN = 0;
    double segAcc = 0.0;            // 50 ms segments for the noise floor
    int segBlocks = 0;
    std::deque<double> segMeans;    // dB, last 5 s
    double floorDb_ = -200.0;
    bool haveFloor = false;
    double lastBlockDb = -200.0;
    bool on = false;
    int hang = 0;
    size_t lastAboveLen = 0;        // capture length at the last block above the off threshold
    std::vector<CF> capture;
    std::vector<CF> preRoll;        // the last few blocks (the start of a burst is just before it is seen)
    size_t preRollPos = 0;
    std::vector<CF> recent;         // last 0.5 s (continuous signals)
    size_t recentPos = 0;
    bool recentFull = false;
    double burstStartT = 0.0;
    double captureStartT = 0.0;
    bool captureIsContinuation = false;
    uint64_t total = 0;             // samples since reset
    double lastReportT = 0.0;
    int seq = 0;
    // earlier bursts (symbols) for the repeated part
    struct Past { int levels; double rate; std::vector<uint8_t> sym; };
    std::deque<Past> past;

    static constexpr double HYST_DB = 3.0;   // the burst ends this much below the trigger level
    static constexpr int HANG_BLOCKS = 20;          // 20 ms: OOK gaps don't end the burst
    static constexpr double MAX_CAPTURE_SEC = 1.0;
    static constexpr double MIN_BURST_SEC = 0.004;
    // the analysis looks at this many samples at most (a wide channel: a shorter piece; the
    // statistics need a few thousand symbols, not more)
    static constexpr size_t MAX_ANALYSIS = (size_t)1 << 17;
    static constexpr size_t MAX_STEADY = (size_t)1 << 16;

public:
    Report latest;
    int reports = 0;
    std::deque<BurstMark> marks;    // last 60 s
    // trigger (like an oscilloscope's): a transmission starts this much above the noise floor
    double triggerDb = 8.0;
    // also analyse signals that never stop (0.5 s windows every 3 s); off = bursts only
    bool steadyWindows = true;
    // Where the captures go: empty = analysed right here (the caller's thread), else handed
    // over (the module analyses them on its own thread with analyzeCapture()).
    std::function<void(Capture&&)> onCapture;
    // Where the reports go: empty = `latest` / `reports` of this object
    std::function<void(Report&&)> onReport;

    // Forget the earlier bursts (another signal)
    void resetHistory() { past.clear(); seq = 0; }

    void configure(double rate) {
        if (rate == fs) return;
        fs = rate;
        reset();
    }

    void reset() {
        blockLen = std::max<size_t>(1, (size_t)std::llround(fs * 0.001));
        blockAcc = 0.0; blockN = 0; segAcc = 0.0; segBlocks = 0;
        segMeans.clear();
        floorDb_ = -200.0; haveFloor = false; lastBlockDb = -200.0;
        on = false; hang = 0; lastAboveLen = 0;
        capture.clear();
        preRoll.assign(blockLen * 3, CF(0, 0)); preRollPos = 0;
        recent.assign(std::min(MAX_STEADY, (size_t)std::max(1.0, fs * 0.5)), CF(0, 0)); recentPos = 0; recentFull = false;
        total = 0; lastReportT = 0.0;
        past.clear();
        marks.clear();
        latest = Report();
        reports = 0;
    }

    double rate() const { return fs; }
    double now() const { return fs > 0 ? total / fs : 0.0; }
    double floorDb() const { return floorDb_; }
    double levelDb() const { return lastBlockDb; }
    bool inBurst() const { return on; }

    // Burst period from the last starts: 0 = not periodic (or not enough bursts)
    double period(double* jitter = nullptr) const {
        std::vector<double> st;
        for (const auto& m : marks) st.push_back(m.t);
        if (st.size() < 4) return 0.0;
        if (st.size() > 12) st.erase(st.begin(), st.end() - 12);
        std::vector<double> d;
        for (size_t i = 1; i < st.size(); i++) d.push_back(st[i] - st[i - 1]);
        std::vector<double> s = d;
        std::sort(s.begin(), s.end());
        const double med = s[s.size() / 2];
        if (med <= 0.0) return 0.0;
        double worst = 0.0;
        for (double x : d) worst = std::max(worst, std::fabs(x - med));
        if (worst > med * 0.03 + 0.003) return 0.0;
        if (jitter) *jitter = worst;
        return med;
    }

    void push(const CF* x, size_t n) {
        if (fs <= 0.0) return;
        for (size_t i = 0; i < n; i++) {
            const CF v = x[i];
            preRoll[preRollPos] = v;
            preRollPos = (preRollPos + 1) % preRoll.size();
            recent[recentPos] = v;
            if (++recentPos == recent.size()) { recentPos = 0; recentFull = true; }
            if (on) capture.push_back(v);
            blockAcc += (double)std::norm(v);
            total++;
            if (++blockN == blockLen) endBlock();
        }
    }

private:
    void endBlock() {
        const double p = blockAcc / (double)blockN;
        blockAcc = 0.0; blockN = 0;
        const double db = 10.0 * std::log10(p + 1e-20);
        lastBlockDb = db;
        // noise floor: the quietest 50 ms of the last 5 s
        segAcc += p;
        if (++segBlocks == 50) {
            segMeans.push_back(10.0 * std::log10(segAcc / 50.0 + 1e-20));
            if (segMeans.size() > 100) segMeans.pop_front();
            segAcc = 0.0; segBlocks = 0;
            floorDb_ = *std::min_element(segMeans.begin(), segMeans.end());
            haveFloor = true;
        }
        if (!haveFloor) return;
        const double t = now();
        if (!on) {
            if (db > floorDb_ + triggerDb) {
                on = true; hang = 0;
                // what came just before (the rising edge)
                capture.clear();
                for (size_t k = 0; k < preRoll.size(); k++) capture.push_back(preRoll[(preRollPos + k) % preRoll.size()]);
                lastAboveLen = capture.size();
                burstStartT = t - (double)capture.size() / fs;
                captureStartT = burstStartT;
                captureIsContinuation = false;
            } else if (steadyWindows && t - lastReportT > 3.0 && recentFull) {
                // nothing that comes and goes: look at the last half second anyway
                std::vector<CF> w(recent.size());
                for (size_t k = 0; k < recent.size(); k++) w[k] = recent[(recentPos + k) % recent.size()];
                emit(std::move(w), t - (double)recent.size() / fs, true, false, false);
            }
            return;
        }
        if (db > floorDb_ + std::max(1.0, triggerDb - HYST_DB)) { hang = 0; lastAboveLen = capture.size(); }
        else if (++hang >= HANG_BLOCKS) {
            on = false;
            capture.resize(std::min(capture.size(), lastAboveLen + blockLen));
            const double dur = (double)capture.size() / fs;
            if (captureIsContinuation || dur >= MIN_BURST_SEC) {
                addMark(burstStartT, t - burstStartT - HANG_BLOCKS * 0.001);
                emit(std::move(capture), captureStartT, false, captureIsContinuation, false);
            }
            capture.clear();
            return;
        }
        if ((double)capture.size() / fs >= MAX_CAPTURE_SEC || capture.size() >= MAX_ANALYSIS) {
            // a long transmission: analyse what we have, go on collecting
            const size_t n = capture.size();
            emit(std::vector<CF>(capture), captureStartT, false, captureIsContinuation, true);
            captureStartT += (double)n / fs;
            captureIsContinuation = true;
            capture.clear();
            lastAboveLen = 0;
        }
    }

    // A capture is ready: hand it over, or analyse it here
    void emit(std::vector<CF>&& s, double t0, bool continuous, bool continuation, bool truncated) {
        lastReportT = now();
        Capture c;
        if (s.size() > MAX_ANALYSIS) { s.resize(MAX_ANALYSIS); truncated = true; }
        c.s = std::move(s);
        c.t0 = t0; c.rate = fs; c.floorDb = floorDb_; c.haveFloor = haveFloor;
        c.continuous = continuous; c.continuation = continuation; c.truncated = truncated;
        c.period = period(&c.jitter);
        if (onCapture) onCapture(std::move(c));
        else analyzeCapture(c);
    }

    // One mark per transmission (a long one is marked when it ends, from its start)
    void addMark(double t, double dur) {
        BurstMark m; m.t = t; m.dur = std::max(dur, 0.0);
        marks.push_back(m);
        while (!marks.empty() && marks.front().t < now() - 60.0) marks.pop_front();
    }

    // ---------------- helpers ----------------
    static size_t pow2ceil(size_t n) { size_t p = 1; while (p < n) p <<= 1; return p; }
    static size_t pow2floor(size_t n) { size_t p = 1; while (p * 2 <= n) p <<= 1; return p; }

    // Magnitude spectrum of a real sequence (mean removed, Hann), bins 0..N/2
    static std::vector<double> realSpectrum(const std::vector<double>& x, size_t maxN, size_t& nfft) {
        nfft = std::min(pow2ceil(x.size()), maxN);
        std::vector<Complex> a(nfft, Complex(0, 0));
        const size_t n = std::min(x.size(), nfft);
        double mean = 0.0;
        for (size_t i = 0; i < n; i++) mean += x[i];
        mean /= std::max<size_t>(1, n);
        for (size_t i = 0; i < n; i++) {
            const double w = 0.5 - 0.5 * std::cos(2.0 * PI_D * i / std::max<size_t>(1, n - 1));
            a[i] = Complex((x[i] - mean) * w, 0.0);
        }
        fft(a);
        std::vector<double> m(nfft / 2 + 1);
        for (size_t i = 0; i < m.size(); i++) m[i] = std::abs(a[i]);
        return m;
    }

    // Strongest line between lo and hi Hz: returns frequency, ratio = line / median
    static double findLine(const std::vector<double>& m, size_t nfft, double rate, double lo, double hi, double& ratio) {
        const double binHz = rate / nfft;
        size_t a = (size_t)std::max(1.0, std::ceil(lo / binHz)), b = (size_t)std::min((double)m.size() - 2, std::floor(hi / binHz));
        ratio = 0.0;
        if (b <= a + 4) return 0.0;
        std::vector<double> band(m.begin() + a, m.begin() + b + 1);
        std::nth_element(band.begin(), band.begin() + band.size() / 2, band.end());
        const double med = std::max(band[band.size() / 2], 1e-30);
        size_t k = a;
        for (size_t i = a; i <= b; i++) if (m[i] > m[k]) k = i;
        ratio = m[k] / med;
        // parabolic refinement
        double d = 0.0;
        const double y0 = m[k - 1], y1 = m[k], y2 = m[k + 1];
        const double den = y0 - 2 * y1 + y2;
        if (std::fabs(den) > 1e-30) d = 0.5 * (y0 - y2) / den;
        return (k + std::clamp(d, -0.5, 0.5)) * binHz;
    }

    // Like findLine, but a line must stand out from its own neighbourhood (+-8 %), not from
    // the whole band: the continuous part of a spectrum may slope a lot (|s|^2 of filtered PSK)
    static double findProminentLine(const std::vector<double>& m, size_t nfft, double rate, double lo, double hi, double& ratio) {
        const double binHz = rate / nfft;
        const size_t a = (size_t)std::max(2.0, std::ceil(lo / binHz)), b = (size_t)std::min((double)m.size() - 3, std::floor(hi / binHz));
        ratio = 0.0;
        if (b <= a + 4) return 0.0;
        size_t best = a;
        std::vector<double> nb;
        for (size_t k = a; k <= b; k++) {
            if (m[k] < m[k - 1] || m[k] < m[k + 1]) continue;
            const size_t w = std::max<size_t>(8, (size_t)(k * 0.08));
            nb.clear();
            for (size_t j = (k > w ? k - w : 1); j <= std::min(m.size() - 1, k + w); j++) if (j + 2 < k || j > k + 2) nb.push_back(m[j]);
            if (nb.size() < 8) continue;
            std::nth_element(nb.begin(), nb.begin() + nb.size() / 2, nb.end());
            const double rr = m[k] / std::max(nb[nb.size() / 2], 1e-30);
            if (rr > ratio) { ratio = rr; best = k; }
        }
        double d = 0.0;
        const double y0 = m[best - 1], y1 = m[best], y2 = m[best + 1];
        const double den = y0 - 2 * y1 + y2;
        if (std::fabs(den) > 1e-30) d = 0.5 * (y0 - y2) / den;
        return (best + std::clamp(d, -0.5, 0.5)) * binHz;
    }

    static double snapRate(double r) {
        static const double std_[] = {45.45, 50, 75, 100, 110, 150, 200, 250, 300, 512, 600, 1000, 1200, 1600, 2000, 2400,
                                      3000, 3200, 3600, 4000, 4800, 6000, 6250, 8000, 9600, 10000, 12000, 16000, 18000,
                                      19200, 20000, 24000, 25000, 32000, 36000, 38400, 48000, 50000, 57600, 64000, 76800, 96000, 100000};
        for (double s : std_) if (std::fabs(r - s) < s * 0.006) return s;
        return r;
    }

    static double interp(const std::vector<double>& v, double t) {
        if (t <= 0) return v.front();
        const size_t i = (size_t)t;
        if (i + 1 >= v.size()) return v.back();
        const double f = t - i;
        return v[i] * (1 - f) + v[i + 1] * f;
    }

    // 1-D k-means; returns sorted centres and the explained variance (0..1)
    static std::vector<double> kmeans(const std::vector<double>& v, int k, double& explained) {
        std::vector<double> s = v;
        std::sort(s.begin(), s.end());
        std::vector<double> c(k);
        for (int i = 0; i < k; i++) c[i] = s[(size_t)((i + 0.5) / k * (s.size() - 1))];
        std::vector<int> lab(v.size(), 0);
        for (int it = 0; it < 30; it++) {
            std::vector<double> sum(k, 0.0);
            std::vector<int> cnt(k, 0);
            for (size_t i = 0; i < v.size(); i++) {
                int best = 0;
                for (int j = 1; j < k; j++) if (std::fabs(v[i] - c[j]) < std::fabs(v[i] - c[best])) best = j;
                lab[i] = best; sum[best] += v[i]; cnt[best]++;
            }
            bool moved = false;
            for (int j = 0; j < k; j++) if (cnt[j]) { const double nc = sum[j] / cnt[j]; if (std::fabs(nc - c[j]) > 1e-9) moved = true; c[j] = nc; }
            if (!moved) break;
        }
        double mean = 0.0;
        for (double x : v) mean += x;
        mean /= std::max<size_t>(1, v.size());
        double tot = 0.0, within = 0.0;
        for (size_t i = 0; i < v.size(); i++) { tot += (v[i] - mean) * (v[i] - mean); within += (v[i] - c[lab[i]]) * (v[i] - c[lab[i]]); }
        explained = tot > 0 ? 1.0 - within / tot : 0.0;
        // a level nobody uses is no level
        std::vector<int> cnt(k, 0);
        for (int l : lab) cnt[l]++;
        for (int j = 0; j < k; j++) if (cnt[j] < (int)v.size() / (k * 12) + 1) explained *= 0.8;
        std::sort(c.begin(), c.end());
        return c;
    }

    // OOK: the length of one bit in samples. Every on / off run is a whole number of bits:
    // the longest unit that explains (nearly) all runs. 0 = none found.
    static double ookUnit(const std::vector<double>& env) {
        std::vector<double> e = env;
        std::sort(e.begin(), e.end());
        const double thr = (e[e.size() / 20] + e[e.size() * 19 / 20]) / 2.0;
        std::vector<double> runs;
        size_t start = 0;
        for (size_t i = 1; i <= env.size(); i++) {
            if (i == env.size() || (env[i] > thr) != (env[start] > thr)) {
                if (start > 0 && i < env.size()) runs.push_back((double)(i - start));   // not the cut ends
                start = i;
            }
        }
        if (runs.size() < 6) return 0.0;
        // runs of 1 sample are noise
        runs.erase(std::remove_if(runs.begin(), runs.end(), [](double x) { return x < 2.0; }), runs.end());
        if (runs.size() < 6) return 0.0;
        const double shortest = *std::min_element(runs.begin(), runs.end());
        for (int div = 1; div <= 4; div++) {
            const double u0 = shortest / div;
            if (u0 < 2.0) break;
            // average unit from the runs that fit
            double sumLen = 0.0, sumK = 0.0;
            int fit = 0;
            for (double x : runs) {
                const double k = std::round(x / u0);
                if (k >= 1 && std::fabs(x - k * u0) <= 0.3 * u0 + 1.0) { fit++; sumLen += x; sumK += k; }
            }
            if (fit >= (int)(runs.size() * 0.9)) return sumLen / sumK;
        }
        return 0.0;
    }

    // How far from the spectrum's centroid the carrier may still be
    static double carrierSearchHz(double bwHz) { return std::max(300.0, bwHz * 0.04); }

    // How cleanly k levels separate the values: the smallest gap between neighbouring
    // levels against the spread around them (>~2.2: real levels; Gaussian noise ~1.4)
    static double separation(const std::vector<double>& v, int k, std::vector<double>& c, double& explained) {
        c = kmeans(v, k, explained);
        double within = 0.0;
        for (double x : v) {
            double best = 1e300;
            for (double y : c) best = std::min(best, std::fabs(x - y));
            within += best * best;
        }
        const double sd = std::sqrt(within / std::max<size_t>(1, v.size()));
        double gap = 1e300;
        for (int j = 1; j < k; j++) gap = std::min(gap, c[j] - c[j - 1]);
        // every level must be used
        std::vector<int> cnt(k, 0);
        for (double x : v) { int b = 0; for (int j = 1; j < k; j++) if (std::fabs(x - c[j]) < std::fabs(x - c[b])) b = j; cnt[b]++; }
        for (int j = 0; j < k; j++) if (cnt[j] < (int)v.size() / (k * 10) + 2) return 0.0;
        return gap / (2.0 * std::max(sd, 1e-9));
    }

    struct FskFit { double rate = 0, sep = 0, explained = 0, phase = 0; int levels = 0; std::vector<double> centres; std::vector<double> values; };

    // FSK at one symbol rate: integrate the discriminator over ~0.7 symbol, sample at the
    // eye's centre, try 2 / 4 / 8 levels
    static FskFit fskAt(const std::vector<double>& d, double fs, double rate) {
        FskFit f;
        f.rate = rate;
        const double sps = fs / rate;
        const int Li = std::max(1, (int)std::lround(sps * 0.7));
        std::vector<double> integ(d.size(), 0.0);
        double acc = 0.0;
        for (size_t i = 0; i < d.size(); i++) {
            acc += d[i];
            if (i >= (size_t)Li) acc -= d[i - Li];
            integ[i] = acc / std::min<size_t>(i + 1, Li);
        }
        f.values = sampleSymbols(integ, sps, f.phase);
        if (f.values.size() < 24) return f;
        for (int k : {2, 4, 8}) {
            if (k == 8 && f.values.size() < 120) break;
            std::vector<double> c;
            double e = 0.0;
            const double s = separation(f.values, k, c, e);
            if (s > f.sep) { f.sep = s; f.levels = k; f.centres = c; f.explained = e; }
        }
        return f;
    }

    // Samples of v at the symbol instants: the phase with the most energy (the eye's centre)
    static std::vector<double> sampleSymbols(const std::vector<double>& v, double sps, double& bestPh) {
        std::vector<double> out;
        if (sps < 1.0 || v.size() < sps * 4) return out;
        double mean = 0.0;
        for (double x : v) mean += x;
        mean /= v.size();
        double bestE = -1.0;
        bestPh = 0.0;
        const int phases = 16;
        for (int p = 0; p < phases; p++) {
            const double ph = sps * p / phases;
            double e = 0.0; size_t n = 0;
            for (double t = ph; t < v.size() - 1; t += sps) { const double y = interp(v, t) - mean; e += y * y; n++; }
            if (n && e / n > bestE) { bestE = e / n; bestPh = ph; }
        }
        for (double t = bestPh; t < v.size() - 1; t += sps) out.push_back(interp(v, t));
        return out;
    }

public:
    // ---------------- the analysis of one burst ----------------
    // Touches only the analysis' own state (earlier bursts, sequence number): it may run on
    // another thread than push(), one capture at a time.
    void analyzeCapture(const Capture& cap) {
        const std::vector<CF>& in = cap.s;
        const double fs = cap.rate;
        const bool continuous = cap.continuous;
        const double floorDb_ = cap.floorDb;
        const bool haveFloor = cap.haveFloor;
        Report r;
        r.valid = true;
        r.seq = ++seq;
        r.tStart = cap.t0;
        r.rate = fs;
        r.continuous = continuous;
        r.truncated = cap.truncated || cap.continuation;
        // drop the edges (filter ramp, detector delay)
        const size_t edge = std::min(in.size() / 10, (size_t)(fs * 0.002));
        std::vector<CD> s;
        s.reserve(in.size());
        for (size_t i = edge; i + edge < in.size(); i++) s.emplace_back(in[i].real(), in[i].imag());
        r.durMs = in.size() / fs * 1000.0;
        r.floorDb = floorDb_;
        if (s.size() < 64 || fs <= 0.0) { finish(r); return; }

        double p = 0.0;
        for (const auto& v : s) p += std::norm(v);
        p /= s.size();
        r.levelDb = 10.0 * std::log10(p + 1e-20);
        r.snrDb = haveFloor ? r.levelDb - floorDb_ : 0.0;

        // ---- spectrum (Welch): offset and occupied bandwidth ----
        {
            const size_t nfft = std::max<size_t>(64, std::min<size_t>(4096, pow2floor(s.size())));
            std::vector<double> P(nfft, 0.0);
            for (size_t st = 0; st + nfft <= s.size(); st += nfft / 2) {
                std::vector<Complex> a(nfft);
                for (size_t i = 0; i < nfft; i++) a[i] = s[st + i] * (0.5 - 0.5 * std::cos(2.0 * PI_D * i / (nfft - 1)));
                fft(a);
                for (size_t i = 0; i < nfft; i++) P[i] += std::norm(a[i]);
            }
            // shift: index 0 = -fs/2; smooth over 1/64 of the band
            std::vector<double> Q(nfft);
            for (size_t i = 0; i < nfft; i++) Q[i] = P[(i + nfft / 2) % nfft];
            const int sm = std::max<int>(1, (int)nfft / 128);
            std::vector<double> S(nfft, 0.0);
            for (int i = 0; i < (int)nfft; i++) {
                double acc = 0.0; int c = 0;
                for (int k = -sm; k <= sm; k++) if (i + k >= 0 && i + k < (int)nfft) { acc += Q[i + k]; c++; }
                S[i] = acc / c;
            }
            std::vector<double> sorted = S;
            std::sort(sorted.begin(), sorted.end());
            const double noise = sorted[sorted.size() / 8];
            const double peak = sorted.back();
            const double thr = std::max(peak / 400.0, noise * 2.0);
            int lo = -1, hi = -1;
            // only the band that is really the channel (the filter's skirts are excluded)
            const int a0 = (int)(nfft * 0.05), a1 = (int)(nfft * 0.95);
            for (int i = a0; i < a1; i++) if (S[i] > thr) { if (lo < 0) lo = i; hi = i; }
            const double binHz = fs / nfft;
            if (lo >= 0) {
                r.bwHz = (hi - lo + 1) * binHz;
                double num = 0.0, den = 0.0;
                for (int i = lo; i <= hi; i++) { const double w = std::max(0.0, S[i] - noise); num += w * ((i - (int)nfft / 2) * binHz); den += w; }
                if (den > 0) r.offsetHz = num / den;
            }
        }
        // move the signal's centre to 0 Hz
        {
            CD rot(1, 0);
            const CD step = std::polar(1.0, -2.0 * PI_D * r.offsetHz / fs);
            for (auto& v : s) { v *= rot; rot *= step; }
        }

        // ---- envelope ----
        std::vector<double> env(s.size());
        for (size_t i = 0; i < s.size(); i++) env[i] = std::abs(s[i]);
        double em = 0.0, ev = 0.0;
        for (double a : env) em += a;
        em /= env.size();
        for (double a : env) ev += (a - em) * (a - em);
        r.envCv = em > 0 ? std::sqrt(ev / env.size()) / em : 0.0;
        double fracLow = 0.0;
        {
            std::vector<double> e = env;
            std::nth_element(e.begin(), e.begin() + e.size() * 9 / 10, e.end());
            const double a90 = e[e.size() * 9 / 10];
            for (double a : env) if (a < 0.25 * a90) fracLow += 1.0;
            fracLow /= env.size();
        }
        // line of the envelope (power): linear modulations (PSK, QAM) have a strong one at the
        // symbol rate; FSK only weak ones (filter edges) and the burst's own ramps lower down
        {
            std::vector<double> x(s.size());
            for (size_t i = 0; i < s.size(); i++) x[i] = std::norm(s[i]);
            size_t nfft = 0;
            const auto m = realSpectrum(x, (size_t)1 << 19, nfft);
            const double lo = std::max({20.0, 6.0 * fs / s.size(), r.bwHz * 0.3});
            r.envLineHz = findProminentLine(m, nfft, fs, lo, std::min(fs * 0.48, std::max(lo * 2.0, r.bwHz * 1.2)), r.envLine);
        }
        const bool strong = r.snrDb > 10.0 || continuous;
        const bool ook = fracLow > 0.2 && r.envCv > 0.45 && r.snrDb > 12.0 && !continuous;
        // (FSK bursts at cv ~0.15 show chance lines up to ~40x; PSK with pulse shaping has cv > 0.2)
        const bool linearEnv = r.envLine > 25.0 && r.envCv > 0.18;
        const bool constEnv = r.envCv < 0.3 && !linearEnv;
        // a carrier (or CW) is narrower than the spectrum's resolution allows a modulation to be
        const bool narrow = r.bwHz < std::max(250.0, fs / 40.0);

        // ---- FM discriminator ----
        std::vector<double> d(s.size(), 0.0);
        for (size_t i = 1; i < s.size(); i++) d[i] = std::arg(s[i] * std::conj(s[i - 1])) * fs / (2.0 * PI_D);
        d[0] = d.size() > 1 ? d[1] : 0.0;
        double dm = 0.0, dv = 0.0;
        for (double x : d) dm += x;
        dm /= d.size();
        for (double x : d) dv += (x - dm) * (x - dm);
        const double dRms = std::sqrt(dv / d.size());
        // smoothed over about one symbol of a signal this wide
        const int L = std::clamp((int)std::lround(fs / std::max(200.0, r.bwHz * 0.5)), 1, 64);
        std::vector<double> ds(d.size(), 0.0);
        {
            double acc = 0.0;
            for (size_t i = 0; i < d.size(); i++) {
                acc += d[i];
                if (i >= (size_t)L) acc -= d[i - L];
                ds[i] = acc / std::min<size_t>(i + 1, L);
            }
        }

        // ---- symbol rate ----
        // The spectral line of the transitions (FSK: discriminator, else the envelope).
        // For FSK the line alone is not trusted (patterns give lines at fractions of the
        // rate): every candidate rate is tried and the one whose levels separate best wins.
        FskFit fsk;
        {
            std::vector<double> x(s.size(), 0.0);
            if (constEnv) for (size_t i = 1; i < ds.size(); i++) x[i] = std::fabs(ds[i] - ds[i - 1]);
            else if (ook) for (size_t i = 1; i < env.size(); i++) x[i] = std::fabs(env[i] - env[i - 1]);   // NRZ has a null at Rs; its edges don't
            else for (size_t i = 0; i < s.size(); i++) x[i] = std::norm(s[i]);
            size_t nfft = 0;
            const auto m = realSpectrum(x, (size_t)1 << 19, nfft);
            const double lo = std::max(20.0, 6.0 * fs / s.size());
            const double hi = constEnv ? fs / 2.2 : fs * 0.48;
            double ratio = 0.0;
            double f = 0.0;
            if (!constEnv && !ook) {
                // linear: the rate is 0.3 .. 1.2 x the bandwidth (roll-off 0 .. 1 and more)
                f = r.envLineHz; ratio = r.envLine;
            } else {
                f = findLine(m, nfft, fs, lo, hi, ratio);
            }
            r.symLine = ratio;
            if (ratio > 7.0) { r.symRateRaw = f; r.symRate = snapRate(f); }
            if (ook) {
                const double u = ookUnit(env);
                if (u > 0.0) { r.symRateRaw = fs / u; r.symRate = snapRate(fs / u); r.symLine = 0.0; }
            }

            if (constEnv && !narrow) {
                std::vector<double> cand;
                // the strongest lines (and their halves / doubles)
                const double binHz = fs / nfft;
                std::vector<std::pair<double, size_t>> peaks;
                for (size_t i = 2; i + 2 < m.size(); i++) {
                    const double fi = i * binHz;
                    if (fi < lo || fi > hi) continue;
                    if (m[i] >= m[i - 1] && m[i] >= m[i + 1] && m[i] >= m[i - 2] && m[i] >= m[i + 2]) peaks.push_back({m[i], i});
                }
                std::sort(peaks.begin(), peaks.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
                for (size_t k = 0; k < peaks.size() && k < 6; k++) {
                    double rr = 0.0;
                    const double fk = findLine(m, nfft, fs, (peaks[k].second - 1.5) * binHz, (peaks[k].second + 1.5) * binHz, rr);
                    for (double mult : {1.0, 2.0, 0.5}) cand.push_back(fk * mult);
                }
                static const double stdRates[] = {300, 512, 600, 1200, 1600, 2000, 2400, 3200, 3600, 4000, 4800, 6000, 6250,
                                                  8000, 9600, 10000, 12000, 16000, 19200, 24000, 32000, 38400, 48000, 64000, 76800, 96000};
                for (double sr : stdRates) cand.push_back(sr);
                std::vector<FskFit> fits;
                double bestSep = 0.0;
                for (double c : cand) {
                    if (c < lo || c > hi) continue;
                    auto ft = fskAt(d, fs, snapRate(c));
                    bestSep = std::max(bestSep, ft.sep);
                    fits.push_back(std::move(ft));
                }
                // a fraction of the rate samples every 2nd / 3rd symbol and separates as well:
                // the highest rate that is nearly as good as the best
                if (bestSep >= 2.2) {
                    for (auto& ft : fits)
                        if (ft.sep >= std::max(2.2, bestSep * 0.88) && ft.rate > fsk.rate) fsk = std::move(ft);
                    // a rate that is not a standard one: fine tuning
                    if (snapRate(fsk.rate) != fsk.rate || std::find(std::begin(stdRates), std::end(stdRates), fsk.rate) == std::end(stdRates)) {
                        for (double k = -0.006; k <= 0.0061; k += 0.001) {
                            auto ft = fskAt(d, fs, fsk.rate * (1.0 + k));
                            if (ft.sep > fsk.sep * 1.02) fsk = std::move(ft);
                        }
                    }
                    r.symRate = fsk.rate;
                    r.symRateRaw = fsk.rate;

                }
            }
        }

        // ---- s^M lines (PSK) ----
        bool pskLine[3] = {false, false, false};
        if (!constEnv && !ook) {
            const int Ms[3] = {2, 4, 8};
            double scale = 0.0;
            for (const auto& v : s) scale += std::abs(v);
            scale = scale > 0 ? s.size() / scale : 1.0;
            const size_t n = std::min(pow2ceil(s.size()), (size_t)1 << 18);
            for (int k = 0; k < 3; k++) {
                std::vector<Complex> a(n, Complex(0, 0));
                for (size_t i = 0; i < std::min(n, s.size()); i++) a[i] = std::pow(s[i] * scale, Ms[k]);
                fft(a);
                std::vector<double> m(n);
                for (size_t i = 0; i < n; i++) m[i] = std::abs(a[i]);
                // |sum s^M| / sum |s|^M at the strongest bin near the carrier (x M): ~0.3..1 for
                // M-PSK, ~1/sqrt(N) for anything else. Only near the carrier: pi/4-DQPSK has
                // lines at +-Rs/2 in s^2 and s^4.
                double norm = 0.0, norm2 = 0.0;
                for (size_t i = 0; i < std::min(n, s.size()); i++) {
                    const double a = std::pow(std::abs(s[i]) * scale, Ms[k]);
                    norm += a; norm2 += a * a;
                }
                const size_t lim = (size_t)std::ceil(carrierSearchHz(r.bwHz) * Ms[k] / (fs / n));
                double pk = m[0];
                for (size_t i = 1; i <= lim && i < n / 2; i++) pk = std::max({pk, m[i], m[n - i]});
                r.powLine[k] = norm > 0 ? pk / norm : 0.0;
                // what random phases give: sqrt(sum |s|^2M) / sum |s|^M (large for high M on a
                // varying envelope, e.g. noise), with margin for the max over the searched bins
                const double chance = norm > 0 ? std::sqrt(norm2) / norm : 1.0;
                pskLine[k] = r.powLine[k] > std::max(0.15, 4.5 * chance);
            }
        }
        const bool anyPsk = pskLine[0] || pskLine[1] || pskLine[2];

        // ---- the family ----
        const double sps = r.symRate > 0 ? fs / r.symRate : 0.0;
        if (r.snrDb < 6.0 && !continuous) r.family = Family::Noise;
        else if (ook && sps >= 2.0) r.family = Family::OOK;
        else if (constEnv) {
            if (narrow) r.family = Family::Carrier;
            else if (fsk.levels > 0) r.family = Family::FSK;
            else { r.family = Family::AnalogFM; r.symRate = 0.0; }
        } else {
            // complex Gaussian noise has an envelope cv of 0.52 and no lines
            const bool lines = r.symLine > 7.0 || linearEnv || anyPsk;
            if (linearEnv) { r.symRateRaw = r.envLineHz; r.symRate = snapRate(r.envLineHz); r.symLine = r.envLine; }
            if (!lines && (continuous || !strong) && std::fabs(r.envCv - 0.52) < 0.1) r.family = Family::Noise;
            else if (anyPsk) r.family = Family::PSK;
            else if (r.symLine > 7.0) r.family = Family::Linear;
            else r.family = Family::AnalogAM;
        }

        // ---- FSK: levels and symbols ----
        double eyePh = 0.0;
        std::vector<double> integ;
        if (r.family == Family::FSK) {
            const int Li = std::max(1, (int)std::lround(sps * 0.7));
            integ.assign(d.size(), 0.0);
            double acc = 0.0;
            for (size_t i = 0; i < d.size(); i++) {
                acc += d[i];
                if (i >= (size_t)Li) acc -= d[i - Li];
                integ[i] = acc / std::min<size_t>(i + 1, Li);
            }
            eyePh = fsk.phase;
            const auto& v = fsk.values;
            const auto& c = fsk.centres;
            r.levels = fsk.levels;
            r.fit = fsk.explained;
            r.separation = fsk.sep;
            const double mid = (c.front() + c.back()) / 2.0;
            for (double x : c) r.levelHz.push_back(x - mid);
            r.offsetHz += mid;
            r.devHz = (c.back() - c.front()) / 2.0;
            for (double x : v) {
                int best = 0;
                for (int j = 1; j < r.levels; j++) if (std::fabs(x - c[j]) < std::fabs(x - c[best])) best = j;
                r.symbols.push_back((uint8_t)best);
            }
            // the same few symbols over and over for the whole burst: a tone (or tones) on FM
            // sampled at a multiple of its frequency, not data
            for (int p = 1; p <= 16 && r.symbols.size() > 64; p++) {
                size_t same = 0;
                for (size_t i = p; i < r.symbols.size(); i++) same += r.symbols[i] == r.symbols[i - p];
                if (same >= (r.symbols.size() - p) * 0.97) { r.periodicTone = true; break; }
            }
            if (r.periodicTone) {
                r.family = Family::AnalogFM;
                r.offsetHz -= mid;
                r.levels = 0; r.levelHz.clear(); r.symbols.clear(); r.symRate = 0.0;
                integ.clear();
            } else {
                if (r.levels == 2) r.modIndex = 2.0 * r.devHz / r.symRate;
                else r.modIndex = (r.levelHz[1] - r.levelHz[0]) / r.symRate;   // between neighbours
                // histogram of the symbol instants (0 = the signal's centre)
                const auto mm = std::minmax_element(v.begin(), v.end());
                const double span = std::max(1.0, *mm.second - *mm.first);
                r.histLo = (float)(*mm.first - mid - span * 0.1);
                r.histHi = (float)(*mm.second - mid + span * 0.1);
                r.hist.assign(64, 0.0f);
                for (double x : v) {
                    const int b = (int)((x - mid - r.histLo) / (r.histHi - r.histLo) * 64);
                    if (b >= 0 && b < 64) r.hist[b] += 1.0f;
                }
                for (auto& x : integ) x -= mid;
            }
        }
        if (r.hist.empty()) {
            // whole discriminator (analog FM, PSK ...)
            std::vector<double> v = (r.family == Family::OOK) ? env : ds;
            std::vector<double> srt = v;
            std::sort(srt.begin(), srt.end());
            const double lo = srt[srt.size() / 100], hi = srt[srt.size() * 99 / 100];
            const double span = std::max(1e-9, hi - lo);
            r.histLo = (float)(lo - span * 0.1);
            r.histHi = (float)(hi + span * 0.1);
            r.hist.assign(64, 0.0f);
            for (double x : v) {
                const int b = (int)((x - r.histLo) / (r.histHi - r.histLo) * 64);
                if (b >= 0 && b < 64) r.hist[b] += 1.0f;
            }
        }

        // ---- OOK: symbols ----
        if (r.family == Family::OOK) {
            auto v = sampleSymbols(env, sps, eyePh);
            if (v.size() >= 8) {
                double e = 0;
                auto c = kmeans(v, 2, e);
                r.fit = e;
                r.levels = 2;
                const double thr = (c[0] + c[1]) / 2.0;
                for (double x : v) r.symbols.push_back(x > thr ? 1 : 0);
            }
        }

        // ---- eye diagram (FSK: discriminator; OOK: envelope) ----
        if ((r.family == Family::FSK || r.family == Family::OOK) && sps >= 1.8) {
            const std::vector<double>& src = r.family == Family::FSK ? integ : env;
            r.eyeLen = 33;
            int traces = 0;
            for (double t = eyePh + sps; t + sps < src.size() - 1 && traces < 200; t += sps, traces++)
                for (int k = 0; k < r.eyeLen; k++) r.eye.push_back((float)interp(src, t - sps + 2.0 * sps * k / (r.eyeLen - 1)));
        }

        // ---- constellation (linear modulations) ----
        if ((r.family == Family::PSK || r.family == Family::Linear) && sps >= 1.5) {
            // the s^M line gives the residual carrier offset and phase
            // the lowest order with a clear line (s^4 of BPSK has a line too)
            int M = 0;
            const int Ms[3] = {2, 4, 8};
            for (int k = 0; k < 3; k++) if (pskLine[k]) { M = Ms[k]; break; }
            double fo = 0.0, ph0 = 0.0;
            if (M) {
                const size_t n = std::min(pow2ceil(s.size()), (size_t)1 << 18);
                std::vector<Complex> a(n, Complex(0, 0));
                for (size_t i = 0; i < std::min(n, s.size()); i++) a[i] = std::pow(s[i] / std::max(em, 1e-12), M);
                fft(a);
                const size_t lim = (size_t)std::ceil(carrierSearchHz(r.bwHz) * M / (fs / n));
                size_t k = 0;
                for (size_t i = 1; i <= lim && i < n / 2; i++) {
                    if (std::abs(a[i]) > std::abs(a[k])) k = i;
                    if (std::abs(a[n - i]) > std::abs(a[k])) k = n - i;
                }
                const double f = (k < n / 2 ? (double)k : (double)k - n) * fs / n;
                fo = f / M;
                // phase at t = 0 of the line
                Complex acc(0, 0);
                for (size_t i = 0; i < s.size(); i++) acc += std::pow(s[i] / std::max(em, 1e-12), M) * std::polar(1.0, -2.0 * PI_D * f * i / fs);
                ph0 = std::arg(acc) / M;
                r.offsetHz += fo;
                if (M == 2) { r.levels = 2; }
                else if (M == 4) { r.levels = 4; }
                else { r.levels = 8; }
            }
            std::vector<double> pw(s.size());
            for (size_t i = 0; i < s.size(); i++) pw[i] = std::norm(s[i]);
            double ph = 0.0;
            sampleSymbols(pw, sps, ph);
            double rms = 0.0; int n = 0;
            for (double t = ph; t < s.size() - 1 && n < 800; t += sps, n++) rms += std::norm(s[(size_t)t]);
            rms = std::sqrt(rms / std::max(1, n));
            n = 0;
            for (double t = ph; t < s.size() - 1 && n < 800; t += sps, n++) {
                const size_t i = (size_t)t;
                const CD v = s[i] * std::polar(1.0, -(2.0 * PI_D * fo * i / fs + ph0 - (M == 4 || M == 8 ? PI_D / M : 0.0))) / std::max(rms, 1e-12);
                r.constel.emplace_back((float)v.real(), (float)v.imag());
            }
        }

        if (r.family == Family::AnalogFM) r.devHz = dRms * std::sqrt(2.0);

        // ---- protocol level ----
        if (!r.symbols.empty()) {
            findPreamble(r);
            findSyncs(r);
            findRepeated(r);
        }
        describe(r, cap.period, cap.jitter);
        finish(r);
    }

private:
    void finish(Report& r) {
        if (onReport) { onReport(std::move(r)); return; }
        latest = std::move(r);
        reports++;
    }

    static void findPreamble(Report& r) {
        const auto& s = r.symbols;
        for (int start = 0; start <= 48 && start < (int)s.size(); start++) {
            for (int p : {1, 2, 3, 4, 6, 8}) {
                int run = 0;
                while (start + run + p < (int)s.size() && s[start + run] == s[start + run + p]) run++;
                // a constant symbol is a carrier, not a preamble, unless it is long
                bool allSame = true;
                for (int k = 1; k < p; k++) if (s[start + k] != s[start]) allSame = false;
                if (p > 1 && allSame) continue;
                if (run + p >= std::max(16, 3 * p)) {
                    r.preambleStart = start;
                    r.preambleLen = run + p;
                    r.preamble.clear();
                    for (int k = 0; k < p; k++) r.preamble += (char)('0' + s[start + k]);
                    return;
                }
            }
        }
    }

    static void findSyncs(Report& r) {
        const auto& s = r.symbols;
        for (const auto& ks : KNOWN_SYNCS) {
            if (ks.levels != r.levels) continue;
            const int n = (int)std::strlen(ks.pattern);
            if ((int)s.size() < n) continue;
            for (int inv = 0; inv < 2; inv++) {
                SyncHit h;
                h.name = ks.name;
                h.inverted = inv != 0;
                for (int i = 0; i + n <= (int)s.size(); i++) {
                    int err = 0;
                    for (int k = 0; k < n && err <= ks.tol; k++) {
                        const bool pos = s[i + k] >= r.levels / 2;                  // upper half: positive
                        const bool want = (ks.pattern[k] == '1') != (inv != 0);
                        if (pos != want) err++;
                    }
                    if (err <= ks.tol) {
                        if (h.count == 0) { h.pos = i; h.errors = err; }
                        h.count++;
                        i += n - 1;
                    }
                }
                if (h.count) r.syncs.push_back(h);
            }
        }
        // fewest errors first among equals (a word and the inverse of another can both match)
        std::stable_sort(r.syncs.begin(), r.syncs.end(), [](const SyncHit& a, const SyncHit& b) {
            return a.count != b.count ? a.count > b.count : a.errors < b.errors; });
        std::vector<SyncHit> uniq;
        for (const auto& h : r.syncs) {
            bool dup = false;
            for (const auto& u : uniq) if (u.pos == h.pos && u.count == h.count) dup = true;
            if (!dup) uniq.push_back(h);
        }
        r.syncs = std::move(uniq);
    }

    // The part of this burst that the earlier bursts have too (aligned at their best shift)
    void findRepeated(Report& r) {
        const auto& s = r.symbols;
        const int N = (int)std::min<size_t>(s.size(), 600);
        std::vector<int> agree(N, 0);
        int compared = 0;
        for (const auto& p : past) {
            if (p.levels != r.levels || std::fabs(p.rate - r.symRate) > r.symRate * 0.01) continue;
            int bestShift = 0, bestM = -1;
            for (int sh = -64; sh <= 64; sh++) {
                int m = 0;
                for (int i = 0; i < N; i++) { const int j = i + sh; if (j >= 0 && j < (int)p.sym.size() && p.sym[j] == s[i]) m++; }
                if (m > bestM) { bestM = m; bestShift = sh; }
            }
            for (int i = 0; i < N; i++) { const int j = i + bestShift; if (j >= 0 && j < (int)p.sym.size() && p.sym[j] == s[i]) agree[i]++; }
            compared++;
        }
        r.comparedBursts = compared;
        if (compared >= 1) {
            // the longest run that (nearly) all earlier bursts share, after the preamble
            const int need = std::max(1, (int)std::ceil(compared * 0.75));
            int bestPos = -1, bestLen = 0, runPos = -1, misses = 0;
            const int from = std::min(N, r.preambleStart + r.preambleLen);
            for (int i = from; i <= N; i++) {
                const bool ok = i < N && agree[i] >= need;
                if (ok) { if (runPos < 0) { runPos = i; misses = 0; } continue; }
                // one stray symbol (an error) does not end the run
                if (runPos >= 0 && i < N - 1 && misses == 0 && agree[i + 1] >= need) { misses++; continue; }
                if (runPos >= 0 && i - runPos > bestLen) { bestLen = i - runPos; bestPos = runPos; }
                runPos = -1;
            }
            if (bestLen >= 12) { r.fixedPos = bestPos; r.fixedLen = bestLen; }
        }
        Past p;
        p.levels = r.levels; p.rate = r.symRate;
        p.sym.assign(s.begin(), s.begin() + N);
        past.push_back(std::move(p));
        if (past.size() > 6) past.pop_front();
    }

    static std::string fmtHz(double hz) {
        char b[32];
        if (std::fabs(hz) >= 1000.0) std::snprintf(b, sizeof(b), "%.2f kHz", hz / 1000.0);
        else std::snprintf(b, sizeof(b), "%.0f Hz", hz);
        return b;
    }
    static std::string fmtRate(double bd) {
        char b[32];
        if (bd >= 1000.0 && std::fabs(bd - std::round(bd)) < 0.01) std::snprintf(b, sizeof(b), "%.0f Bd", bd);
        else std::snprintf(b, sizeof(b), "%.1f Bd", bd);
        return b;
    }

    void describe(Report& r, double per, double jit) {
        char b[160];
        const double bd = r.symRate;
        switch (r.family) {
        case Family::None:     r.modulation = "-"; break;
        case Family::Noise:    r.modulation = "Noise (nothing found)"; break;
        case Family::Carrier:  r.modulation = "Unmodulated carrier"; break;
        case Family::AnalogFM: r.modulation = "FM (analog / no clear symbols)"; break;
        case Family::AnalogAM: r.modulation = "AM / SSB (analog?)"; break;
        case Family::OOK:      r.modulation = "OOK / ASK " + fmtRate(bd); break;
        case Family::FSK: {
            std::string m = std::to_string(r.levels) + "FSK";
            if (r.levels == 2 && std::fabs(r.modIndex - 0.5) < 0.12) m = "GMSK / MSK";
            r.modulation = m + " " + fmtRate(bd);
            break;
        }
        case Family::PSK:
            r.modulation = r.levels == 2 ? "BPSK " : r.levels == 4 ? "QPSK " : "8PSK / pi/4-DQPSK ";
            r.modulation += bd > 0 ? fmtRate(bd) : std::string("(rate ?)");
            break;
        case Family::Linear:   r.modulation = "Linear (PSK / QAM?) " + fmtRate(bd); break;
        }

        // measurements
        std::snprintf(b, sizeof(b), "Level %.1f dB, SNR %.1f dB", r.levelDb, r.snrDb);
        r.notes.push_back(b);
        std::snprintf(b, sizeof(b), "Centre %+.0f Hz from the channel, bandwidth %s", r.offsetHz, fmtHz(r.bwHz).c_str());
        r.notes.push_back(b);
        const bool lin = r.family == Family::PSK || r.family == Family::Linear;
        std::snprintf(b, sizeof(b), "Envelope %s (cv %.2f%s)", r.envCv < 0.3 && !lin ? "constant" : r.envCv < 0.45 ? "varying" : "strongly varying",
                      r.envCv, r.envLine > 25.0 ? ", line at the symbol rate" : "");
        r.notes.push_back(b);
        if (r.symRate > 0) {
            // pi/4-DQPSK looks like 8PSK in s^8 but carries 2 bits
            const int bitsPer = r.family == Family::PSK && r.levels == 8 ? 2 : r.levels == 8 ? 3 : r.levels == 4 ? 2 : 1;
            char br[48] = "";
            if (r.levels > 0 || r.family == Family::OOK) std::snprintf(br, sizeof(br), ", %.0f bit/s", r.symRate * bitsPer);
            if (r.family == Family::FSK)
                std::snprintf(b, sizeof(b), "Symbol rate %s (clearest eye)%s", fmtRate(r.symRate).c_str(), br);
            else
                std::snprintf(b, sizeof(b), "Symbol rate %.1f Bd (line %.0fx)%s%s", r.symRateRaw, r.symLine,
                              r.symRate != r.symRateRaw ? (" ~ " + fmtRate(r.symRate)).c_str() : "", br);
            r.notes.push_back(b);
        } else if (r.family != Family::Noise && r.family != Family::Carrier) {
            r.notes.push_back("No symbol rate line (analog, or too short / weak)");
        }
        if (r.family == Family::FSK) {
            std::string lv;
            for (double x : r.levelHz) { std::snprintf(b, sizeof(b), "%s%+.0f", lv.empty() ? "" : " / ", x); lv += b; }
            std::snprintf(b, sizeof(b), "Levels %s Hz, h = %.2f, eye %.1f", lv.c_str(), r.modIndex, r.separation);
            r.notes.push_back(b);
        }
        if (r.family == Family::AnalogFM) { std::snprintf(b, sizeof(b), "Deviation ~%s (peak, from rms)", fmtHz(r.devHz).c_str()); r.notes.push_back(b); }
        if (r.family == Family::PSK || r.family == Family::Linear) {
            std::snprintf(b, sizeof(b), "Carrier lines s^2 %.2f  s^4 %.2f  s^8 %.2f", r.powLine[0], r.powLine[1], r.powLine[2]);
            r.notes.push_back(b);
        }
        if (!r.preamble.empty()) {
            std::snprintf(b, sizeof(b), "Preamble: \"%s\" repeated, %d symbols", r.preamble.c_str(), r.preambleLen);
            r.notes.push_back(b);
        }
        for (size_t i = 0; i < r.syncs.size() && i < 3; i++) {
            const auto& h = r.syncs[i];
            std::snprintf(b, sizeof(b), "Sync %s%s: %dx (first at symbol %d, %d errors)", h.name.c_str(), h.inverted ? " (inverted)" : "",
                          h.count, h.pos, h.errors);
            r.notes.push_back(b);
        }
        if (r.fixedLen > 0) {
            std::snprintf(b, sizeof(b), "Same in the last %d bursts: %d symbols from symbol %d", r.comparedBursts, r.fixedLen, r.fixedPos);
            r.notes.push_back(b);
        }

        // timing


        // guesses
        auto& g = r.guesses;
        // one hit with errors may be chance in a long burst
        const bool hasSync = !r.syncs.empty() && (r.syncs[0].count >= 2 || r.syncs[0].errors == 0);
        const std::string top = hasSync ? r.syncs[0].name : "";
        auto nearRate = [&](double x) { return std::fabs(bd - x) < x * 0.01; };
        if (hasSync) {
            if (top.rfind("DMR", 0) == 0) g.push_back("DMR (switch on the Digital voice decoder)");
            else if (top.rfind("dPMR", 0) == 0) g.push_back("dPMR (Digital voice decoder)");
            else if (top == "NXDN") g.push_back(nearRate(4800) ? "NXDN 9600 (Digital voice decoder)" : "NXDN 4800 (Digital voice decoder)");
            else if (top == "YSF") g.push_back("Yaesu System Fusion (Digital voice decoder)");
            else if (top == "P25 phase 1") g.push_back("APCO P25 phase 1 (Digital voice decoder)");
            else if (top.rfind("D-STAR", 0) == 0) g.push_back("D-STAR (Digital voice decoder)");
            else if (top == "POCSAG") g.push_back("POCSAG pager");
        }
        if (r.family == Family::FSK && r.levels == 4 && !hasSync) {
            if (nearRate(4800)) g.push_back("4FSK 4800 Bd like DMR / P25 / NXDN96, but no known sync: proprietary data (radio modem, telemetry, RTK)");
            else if (nearRate(2400)) g.push_back("4FSK 2400 Bd like dPMR / NXDN48, but no known sync: proprietary data?");
            else if (nearRate(3600)) g.push_back("4FSK 3600 Bd: ProVoice / EDACS-like?");
            else g.push_back("4FSK data, unknown protocol");
        }
        if (r.family == Family::FSK && r.levels == 2 && !hasSync) {
            if (nearRate(512) || nearRate(1200) || nearRate(2400)) g.push_back("2FSK at a pager rate (POCSAG / FLEX?) - no POCSAG sync seen");
            if (r.modIndex > 0.38 && r.modIndex < 0.62) g.push_back("GMSK: D-STAR (4800), TETRAPOL (8000), Mobitex (8000), AIS (9600), ...");
            if (nearRate(9600) && r.modIndex >= 0.62) g.push_back("2FSK 9600: G3RUH packet / telemetry modem?");
            if (g.empty()) g.push_back("2FSK data, unknown protocol");
        }
        if (r.family == Family::FSK && r.levels == 8) g.push_back("8FSK: rare on VHF/UHF (some radio modems)");
        if (r.family == Family::OOK) g.push_back("OOK: remote controls, sensors, door bells (try the ISM decoder)");
        if (r.family == Family::PSK || r.family == Family::Linear) {
            if (nearRate(18000)) g.push_back("TETRA (pi/4-DQPSK 18 kBd) - Digital voice decoder");
            else if (nearRate(36000)) g.push_back("TETRA TEDS / TETRAPOL? (36 kBd)");
            else if (r.levels == 2) g.push_back("BPSK data (satellite telemetry, paging links, ...)");
            else g.push_back("Linear digital modulation, protocol unknown");
        }
        if (r.family == Family::AnalogFM) {
            if (r.periodicTone) g.push_back("FM with steady tone(s): FFSK/AFSK data idling, a test tone or a beacon");
            else if (r.bwHz > 80000.0) g.push_back("Wide FM: broadcast radio (listen in WFM)");
            else g.push_back("Analog FM: voice (listen in NFM), or FFSK/AFSK data on FM (listen: whistles = data)");
        }
        if (r.family == Family::AnalogAM) g.push_back("Analog AM / SSB voice? Listen in AM / USB");
        if (r.family == Family::Carrier) g.push_back("Carrier: beacon, spurious signal, or a transmitter keyed up without modulation");
        if (per > 0.0) {
            std::snprintf(b, sizeof(b), "Bursts every %.3f s (+-%.0f ms)", per, jit * 1000.0);
            r.notes.push_back(b);
            if (std::fabs(per - 1.0) < 0.01 && r.family == Family::FSK)
                g.push_back("Exactly once a second: typical of a GNSS RTK base station sending corrections");
            else if (per < 0.5) g.push_back("Fast regular bursts: a control channel / TDMA beacon?");
            else g.push_back("Regular bursts: automatic telemetry or a beacon");
        }
    }
};

} // namespace an
