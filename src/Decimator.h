#pragma once

#include <vector>
#include <complex>
#include <cmath>
#include <cstddef>
#include <algorithm>
#include <cstdint>

// =====================================================================
//  Software IQ decimation (powers of two: /2, /4, /8, /16 ...)
//
//  The radio keeps running at a "safe" hardware sample rate (e.g. 2.048 MSps
//  for RTL-SDR, 2.0 MSps for SDRPlay) and the extra samples are filtered out
//  and dropped right after they arrive from USB. The rest of the app
//  (waterfall, demodulator, baseband recording) only sees the lower rate.
//
//  Implementation: a cascade of halfband FIR filters, each decimating by 2.
//  Every other halfband coefficient is zero and only every other output
//  sample is computed, so the cost is very low. The earlier stages can be
//  short (their aliasing lands outside the final passband); the last stage
//  is longer because it sets how sharp the spectrum edges are.
// =====================================================================

class HalfbandDecim2 {
    using C = std::complex<double>;
    int numTaps;
    int center;
    double centerCoef = 0.5;
    std::vector<double> coef;   // coefficients for odd offsets d = 1, 3, 5, ... (symmetric)
    std::vector<C> work;        // history (numTaps-1) + new samples
    size_t phase = 0;           // 0/1: decimation phase carried over between blocks

public:
    // taps must be of the form 4k+3 (so the outermost coefficients are non-zero)
    explicit HalfbandDecim2(int taps) : numTaps(taps), center((taps - 1) / 2) {
        const double pi = 3.14159265358979323846;
        std::vector<double> h(numTaps);
        double sum = 0.0;
        for (int n = 0; n < numTaps; n++) {
            int m = n - center;
            double s = (m == 0) ? 0.5 : std::sin(pi * m / 2.0) / (pi * m);
            // Blackman window (~ -74 dB stopband attenuation)
            double w = 0.42 - 0.5 * std::cos(2.0 * pi * n / (numTaps - 1))
                            + 0.08 * std::cos(4.0 * pi * n / (numTaps - 1));
            h[n] = s * w;
            sum += h[n];
        }
        for (auto& v : h) v /= sum;   // unity DC gain
        centerCoef = h[center];
        for (int d = 1; d <= center; d += 2) coef.push_back(h[center + d]);
        reset();
    }

    void reset() {
        work.assign(numTaps - 1, C(0.0, 0.0));
        phase = 0;
    }

    // Appends the decimated samples to the end of 'out'
    void process(const C* in, size_t n, std::vector<C>& out) {
        if (n == 0) return;
        const size_t hist = (size_t)numTaps - 1;
        work.resize(hist);
        work.insert(work.end(), in, in + n);

        const size_t nc = coef.size();
        size_t i = phase;
        for (; i < n; i += 2) {
            const C* x = work.data() + i + center;
            C acc = centerCoef * x[0];
            for (size_t k = 0; k < nc; k++) {
                const std::ptrdiff_t d = (std::ptrdiff_t)(2 * k + 1);
                acc += coef[k] * (x[-d] + x[d]);
            }
            out.push_back(acc);
        }
        phase = i - n;

        // Keep the last (numTaps-1) samples as history for the next block
        std::copy(work.end() - hist, work.end(), work.begin());
        work.resize(hist);
    }
};

class IQDecimator {
    using C = std::complex<double>;
    std::vector<HalfbandDecim2> stages;
    std::vector<C> bufA, bufB;
    int factor = 1;

public:
    // factor is rounded down to a power of two; 1 = no decimation
    void setFactor(int f) {
        stages.clear();
        factor = 1;
        int numStages = 0;
        while (factor * 2 <= f) { factor *= 2; numStages++; }
        for (int s = 0; s < numStages; s++) {
            bool last = (s == numStages - 1);
            stages.emplace_back(last ? 63 : 23);
        }
    }

    int getFactor() const { return factor; }
    bool active() const { return !stages.empty(); }

    void reset() { for (auto& s : stages) s.reset(); }

    // Returns a pointer to the output (valid until the next call) and its length in outCount.
    // With factor == 1 it simply returns the input.
    const C* process(const C* in, size_t n, size_t& outCount) {
        if (stages.empty()) { outCount = n; return in; }
        const C* src = in;
        size_t cnt = n;
        std::vector<C>* dst = &bufA;
        for (auto& st : stages) {
            dst->clear();
            st.process(src, cnt, *dst);
            src = dst->data();
            cnt = dst->size();
            dst = (dst == &bufA) ? &bufB : &bufA;
        }
        outCount = cnt;
        return src;
    }
};

// Decimation factor for a requested rate relative to the base hardware rate
// (rounded to a power of two, at least 2).
inline int computeDecimationFactor(uint32_t hwRate, uint32_t requestedRate) {
    if (requestedRate == 0 || requestedRate >= hwRate) return 1;
    double ratio = (double)hwRate / (double)requestedRate;
    int f = 1;
    while (f * 2 <= (int)std::lround(ratio)) f *= 2;
    return std::max(f, 2);
}
