#pragma once

#include <vector>
#include <complex>
#include <cmath>
#include <cstdint>
#include <algorithm>

// =====================================================================
//  Shared DSP for the image decoders (SSTV, WEFAX).
//
//  Both modes are frequency modulated audio tones (1500 Hz = black,
//  2300 Hz = white, 1200 Hz = SSTV sync). ToneDemod turns 48 kHz audio
//  into an instantaneous frequency track at 12 kHz:
//    audio -> mix with the band centre -> FIR low-pass -> decimate /4
//          -> phase difference (FM discriminator) -> frequency in Hz
//  The output is clamped to the band, so noise clicks of the
//  discriminator can't dominate the pixel averages.
// =====================================================================

namespace imgdec {

constexpr double TRACK_RATE = 12000.0;      // rate of the frequency track
constexpr double BLACK_HZ = 1500.0;
constexpr double WHITE_HZ = 2300.0;
constexpr double SYNC_HZ = 1200.0;

class ToneDemod {
    using C = std::complex<double>;
    static constexpr double PI_D = 3.14159265358979323846;
    static constexpr int TAPS1 = 31;    // anti-alias filter at the audio rate
    static constexpr int TAPS2 = 127;   // channel filter at the track rate (sharp, ~0.5 kHz transition)

    double inRate = 48000.0;
    double centerHz = 1900.0;
    double halfBwHz = 1000.0;
    int decim = 4;
    C nco = C(1.0, 0.0), ncoStep = C(1.0, 0.0);
    int ncoNorm = 0;
    std::vector<double> taps1, taps2;
    std::vector<C> hist1, hist2;        // doubled ring buffers
    int pos1 = 0, pos2 = 0;
    int phase = 0;
    C last = C(1.0, 0.0);
    double smoothedAmp = 0.0;

    // Blackman windowed sinc low-pass, cutoff fc in cycles/sample, unity DC gain
    static std::vector<double> lowpass(int n, double fc) {
        std::vector<double> t(n);
        const int c = n / 2;
        double sum = 0.0;
        for (int i = 0; i < n; i++) {
            int m = i - c;
            double s = (m == 0) ? 2.0 * fc : std::sin(2.0 * PI_D * fc * m) / (PI_D * m);
            double w = 0.42 - 0.5 * std::cos(2.0 * PI_D * i / (n - 1)) + 0.08 * std::cos(4.0 * PI_D * i / (n - 1));
            t[i] = s * w;
            sum += t[i];
        }
        for (auto& v : t) v /= sum;
        return t;
    }

    static C fir(std::vector<C>& hist, int& pos, const std::vector<double>& taps, C x) {
        const int n = (int)taps.size();
        hist[pos] = x; hist[pos + n] = x;
        pos = (pos + 1) % n;
        C acc(0.0, 0.0);
        const C* h = &hist[pos];
        for (int k = 0; k < n; k++) acc += h[k] * taps[k];
        return acc;
    }

public:
    // centre / halfBandwidth: the audio band that is kept, e.g. 1750 +- 900 Hz for SSTV
    explicit ToneDemod(double audioRate = 48000.0, double centre = 1900.0, double halfBandwidth = 1000.0)
        : centerHz(centre), halfBwHz(halfBandwidth) { setRate(audioRate); }

    void setRate(double audioRate) {
        inRate = audioRate;
        decim = std::max(1, (int)std::lround(inRate / TRACK_RATE));
        ncoStep = std::polar(1.0, -2.0 * PI_D * centerHz / inRate);
        const double trackRate = inRate / decim;
        taps1 = lowpass(TAPS1, std::min(0.45 * trackRate, 4000.0) / inRate);
        taps2 = lowpass(TAPS2, halfBwHz / trackRate);
        reset();
    }

    void reset() {
        hist1.assign(TAPS1 * 2, C(0.0, 0.0));
        hist2.assign(TAPS2 * 2, C(0.0, 0.0));
        pos1 = pos2 = 0; phase = 0; last = C(1.0, 0.0); nco = C(1.0, 0.0); ncoNorm = 0; smoothedAmp = 0.0;
    }

    // Output rate of process()
    double outRate() const { return inRate / decim; }

    // Smoothed signal amplitude (useful as a simple "signal present" level)
    double amplitude() const { return smoothedAmp; }

    // Appends one frequency value (Hz) per output sample to 'out'
    void process(const float* audio, size_t n, std::vector<float>& out) {
        for (size_t i = 0; i < n; i++) {
            C x = nco * (double)audio[i];
            nco *= ncoStep;
            if (++ncoNorm >= 4096) { nco /= std::abs(nco); ncoNorm = 0; }

            // Stage 1 runs at the audio rate, but its output is only needed every 'decim' samples
            const int n1 = TAPS1;
            hist1[pos1] = x; hist1[pos1 + n1] = x;
            pos1 = (pos1 + 1) % n1;
            if (++phase < decim) continue;
            phase = 0;
            C y(0.0, 0.0);
            const C* h = &hist1[pos1];
            for (int k = 0; k < n1; k++) y += h[k] * taps1[k];

            C acc = fir(hist2, pos2, taps2, y);

            double dphi = std::arg(acc * std::conj(last));
            last = acc;
            double f = centerHz + dphi * outRate() / (2.0 * PI_D);
            f = std::clamp(f, centerHz - halfBwHz, centerHz + halfBwHz);
            out.push_back((float)f);
            smoothedAmp = 0.999 * smoothedAmp + 0.001 * std::abs(acc);
        }
    }
};

// Average of v over the fractional sample range [a, b) using linear weights
template <typename T>
inline double rangeAvg(const std::vector<T>& v, double a, double b) {
    if (v.empty()) return 0.0;
    const double maxIdx = (double)v.size();
    a = std::clamp(a, 0.0, maxIdx);
    b = std::clamp(b, 0.0, maxIdx);
    if (b - a < 1e-9) {
        size_t i = std::min((size_t)a, v.size() - 1);
        return (double)v[i];
    }
    double sum = 0.0;
    size_t i0 = (size_t)a;
    size_t i1 = std::min((size_t)std::ceil(b), v.size());
    for (size_t i = i0; i < i1; i++) {
        double lo = std::max(a, (double)i);
        double hi = std::min(b, (double)i + 1.0);
        if (hi > lo) sum += (double)v[i] * (hi - lo);
    }
    return sum / (b - a);
}

inline uint8_t freqToLevel(double f) {
    double v = (f - BLACK_HZ) / (WHITE_HZ - BLACK_HZ) * 255.0;
    return (uint8_t)std::clamp(v, 0.0, 255.0);
}

// Robust straight line fit y = a + b*x with two rounds of outlier rejection.
// Returns false if there are too few points left.
inline bool robustLineFit(const std::vector<double>& xs, const std::vector<double>& ys,
                          double minResidual, double& a, double& b, size_t minPoints = 6) {
    std::vector<bool> use(xs.size(), true);
    for (int round = 0; round < 3; round++) {
        double sx = 0, sy = 0, sxx = 0, sxy = 0; size_t n = 0;
        for (size_t i = 0; i < xs.size(); i++) if (use[i]) {
            sx += xs[i]; sy += ys[i]; sxx += xs[i] * xs[i]; sxy += xs[i] * ys[i]; n++;
        }
        if (n < minPoints) return false;
        double den = n * sxx - sx * sx;
        if (std::abs(den) < 1e-12) return false;
        b = (n * sxy - sx * sy) / den;
        a = (sy - b * sx) / n;
        if (round == 2) break;
        std::vector<double> res;
        for (size_t i = 0; i < xs.size(); i++) if (use[i]) res.push_back(std::abs(ys[i] - (a + b * xs[i])));
        std::nth_element(res.begin(), res.begin() + res.size() / 2, res.end());
        double thr = std::max(minResidual, 3.0 * res[res.size() / 2]);
        for (size_t i = 0; i < xs.size(); i++) if (use[i] && std::abs(ys[i] - (a + b * xs[i])) > thr) use[i] = false;
    }
    return true;
}

} // namespace imgdec
