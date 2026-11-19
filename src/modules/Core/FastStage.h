#pragma once

#include <vector>
#include <complex>
#include <cmath>
#include <cstddef>
#include <algorithm>

// =====================================================================
//  Fast first stage of a channel: NCO mix + power-of-two halfband
//  decimation, in float, with the samples kept as separate real /
//  imaginary arrays (SoA) so the compiler vectorises the inner loops
//  (NEON on Apple Silicon / ARM, SSE / AVX on x86).
//
//  Same structure, filters and output rate as GroupDecimator (NCO ->
//  IQDecimator), which does the same in std::complex<double>: there the
//  complex products (NaN-checked, interleaved) can't be vectorised and the
//  full-rate part of the chain cost ~4x more. Float is ample here: the
//  halfbands' stopband is ~-74 dB, float rounding is ~-140 dB.
//
//  Input: the radio's I/Q (complex<double>) or another FastStage's output
//  (SoA float). Output: SoA float, or complex<double> for the narrow
//  receivers (NarrowChannel), at the decimated rate (cheap there).
// =====================================================================

#if defined(_MSC_VER)
#define FASTSTAGE_RESTRICT __restrict
#else
#define FASTSTAGE_RESTRICT __restrict__
#endif
#if defined(__clang__)
#define FASTSTAGE_VECTORIZE _Pragma("clang loop vectorize(enable) interleave(enable)")
#elif defined(__GNUC__)
#define FASTSTAGE_VECTORIZE _Pragma("GCC ivdep")
#else
#define FASTSTAGE_VECTORIZE
#endif

namespace faststage {

// Halfband low-pass, decimation by 2. Same coefficients as HalfbandDecim2 (Decimator.h).
class HalfbandF {
    int numTaps, center;
    float centerCoef = 0.5f;
    std::vector<float> coef;          // taps at odd distances 1, 3, 5 ... from the centre
    std::vector<float> wr, wi;        // history (numTaps-1) + the new block
    std::vector<float> er, ei, orr, oi;   // the block split into even (centre) / odd samples
    size_t phase = 0;                 // decimation phase carried over between blocks

    // y[k] = cc * e[k] + sum_j coef[j] * (o[k-(j+1)] + o[k+j]); no aliasing between the arrays
    void kernel(float* FASTSTAGE_RESTRICT y, const float* FASTSTAGE_RESTRICT e, const float* FASTSTAGE_RESTRICT o,
                size_t m, size_t nc) const {
        const float cc = centerCoef;
        for (size_t k = 0; k < m; k++) y[k] = cc * e[k];
        for (size_t j = 0; j < nc; j++) {
            const float c = coef[j];
            const float* FASTSTAGE_RESTRICT lo = o - (std::ptrdiff_t)(j + 1);
            const float* FASTSTAGE_RESTRICT hi = o + j;
            for (size_t k = 0; k < m; k++) y[k] += c * (lo[k] + hi[k]);
        }
    }

public:
    explicit HalfbandF(int taps) : numTaps(taps), center((taps - 1) / 2) {
        const double pi = 3.14159265358979323846;
        std::vector<double> h(numTaps);
        double sum = 0.0;
        for (int n = 0; n < numTaps; n++) {
            const int m = n - center;
            const double s = (m == 0) ? 0.5 : std::sin(pi * m / 2.0) / (pi * m);
            const double w = 0.42 - 0.5 * std::cos(2.0 * pi * n / (numTaps - 1))
                                  + 0.08 * std::cos(4.0 * pi * n / (numTaps - 1));
            h[n] = s * w;
            sum += h[n];
        }
        for (auto& v : h) v /= sum;
        centerCoef = (float)h[center];
        for (int d = 1; d <= center; d += 2) coef.push_back((float)h[center + d]);
        reset();
    }

    void reset() {
        wr.assign(numTaps - 1, 0.0f);
        wi.assign(numTaps - 1, 0.0f);
        phase = 0;
    }

    // Appends the decimated samples to outR / outI
    void process(const float* inR, const float* inI, size_t n, std::vector<float>& outR, std::vector<float>& outI) {
        if (n == 0) return;
        const size_t hist = (size_t)numTaps - 1;
        wr.resize(hist); wi.resize(hist);
        wr.insert(wr.end(), inR, inR + n);
        wi.insert(wi.end(), inI, inI + n);

        const size_t first = phase;
        const size_t count = n > first ? (n - first + 1) / 2 : 0;
        const size_t base = outR.size();
        outR.resize(base + count);
        outI.resize(base + count);
        float* yr = outR.data() + base;
        float* yi = outI.data() + base;
        const float* xr = wr.data() + first + center;   // xr[2k]: the centre tap of output k
        const float* xi = wi.data() + first + center;
        const size_t nc = coef.size();

        // Polyphase split: the centre taps are the even samples xr[2k], every other tap sits at
        // an odd distance d = 2j+1, i.e. on the odd samples: xr[2k-d] = O[k-(j+1)], xr[2k+d] =
        // O[k+j] with O[m] = xr[2m+1]. De-interleaved once, all inner loops are unit-stride.
        const size_t ob = (size_t)(center + 1) / 2;       // O[m] is stored at m + ob
        const size_t on = count + (size_t)center;
        er.resize(count); ei.resize(count); orr.resize(on); oi.resize(on);
        FASTSTAGE_VECTORIZE
        for (size_t k = 0; k < count; k++) { er[k] = xr[2 * k]; ei[k] = xi[2 * k]; }
        const float* xo = xr + 1 - 2 * (std::ptrdiff_t)ob;
        const float* xoi = xi + 1 - 2 * (std::ptrdiff_t)ob;
        FASTSTAGE_VECTORIZE
        for (size_t m = 0; m < on; m++) { orr[m] = xo[2 * m]; oi[m] = xoi[2 * m]; }

        constexpr size_t CH = 1024;                       // chunks that stay in L1
        for (size_t k0 = 0; k0 < count; k0 += CH) {
            const size_t m = std::min(count, k0 + CH) - k0;
            kernel(yr + k0, er.data() + k0, orr.data() + ob + k0, m, nc);
            kernel(yi + k0, ei.data() + k0, oi.data() + ob + k0, m, nc);
        }
        phase = first + 2 * count - n;

        std::copy(wr.end() - hist, wr.end(), wr.begin());
        std::copy(wi.end() - hist, wi.end(), wi.begin());
        wr.resize(hist); wi.resize(hist);
    }
};

// NCO mixer (frequency shift by -offsetHz). The phasor advances in double once per
// block of BLK samples; inside a block a precomputed float table rotates it, so the
// per-sample work is independent (vectorisable) and the phase never drifts.
class MixerF {
    static constexpr size_t BLK = 64;
    double fs = 0.0, offset = 1e18, w = 0.0;   // w: rad / sample
    std::complex<double> nco {1.0, 0.0}, stepBlk {1.0, 0.0};
    float tr[BLK], ti[BLK];
    bool identity = true;

    template <typename Load>
    void run(size_t n, float* oR, float* oI, Load load) {
        size_t i = 0;
        while (i < n) {
            const size_t m = std::min(BLK, n - i);
            const float br = (float)nco.real(), bi = (float)nco.imag();
            for (size_t k = 0; k < m; k++) {
                const float pr = br * tr[k] - bi * ti[k];
                const float pi = br * ti[k] + bi * tr[k];
                float xr, xi;
                load(i + k, xr, xi);
                oR[i + k] = xr * pr - xi * pi;
                oI[i + k] = xr * pi + xi * pr;
            }
            if (m == BLK) nco *= stepBlk;
            else nco *= std::polar(1.0, w * (double)m);
            i += m;
        }
        nco /= std::abs(nco);
    }

public:
    void configure(double sampleRate, double offsetHz) {
        if (sampleRate == fs && offsetHz == offset) return;
        fs = sampleRate;
        offset = offsetHz;
        w = -2.0 * 3.14159265358979323846 * offsetHz / fs;
        for (size_t k = 0; k < BLK; k++) { tr[k] = (float)std::cos(w * k); ti[k] = (float)std::sin(w * k); }
        stepBlk = std::polar(1.0, w * BLK);
        identity = offsetHz == 0.0;
    }

    void process(const std::complex<double>* in, size_t n, float* oR, float* oI) {
        const double* d = reinterpret_cast<const double*>(in);   // complex<double> is {re, im}
        if (identity) { for (size_t i = 0; i < n; i++) { oR[i] = (float)d[2 * i]; oI[i] = (float)d[2 * i + 1]; } return; }
        run(n, oR, oI, [d](size_t j, float& r, float& im) { r = (float)d[2 * j]; im = (float)d[2 * j + 1]; });
    }
    void process(const float* inR, const float* inI, size_t n, float* oR, float* oI) {
        if (identity) { std::copy(inR, inR + n, oR); std::copy(inI, inI + n, oI); return; }
        run(n, oR, oI, [inR, inI](size_t j, float& r, float& im) { r = inR[j]; im = inI[j]; });
    }
};

// Mix + decimate: drop-in for GroupDecimator (same configure semantics and output rate).
class Stage {
    double fs = 0.0, centre = 1e18;
    int factor = 1;
    MixerF mix;
    std::vector<HalfbandF> stages;
    std::vector<float> mr, mi, ar, ai, br, bi;   // mixed, ping-pong buffers
    std::vector<std::complex<double>> outC;
    const float* oR = nullptr;
    const float* oI = nullptr;
    size_t oN = 0;

    void buildStages(int f) {
        stages.clear();
        int ns = 0, ff = 1;
        while (ff * 2 <= f) { ff *= 2; ns++; }
        for (int s = 0; s < ns; s++) stages.emplace_back(s == ns - 1 ? 63 : 23);   // as IQDecimator
        factor = ff;
    }

    void decimate(size_t n) {
        const float* sr = mr.data();
        const float* si = mi.data();
        size_t cnt = n;
        bool toA = true;
        for (auto& st : stages) {
            std::vector<float>& dr = toA ? ar : br;
            std::vector<float>& di = toA ? ai : bi;
            dr.clear(); di.clear();
            st.process(sr, si, cnt, dr, di);
            sr = dr.data(); si = di.data(); cnt = dr.size();
            toA = !toA;
        }
        oR = sr; oI = si; oN = cnt;
    }

public:
    // centreHz: offset of the wanted band from the input's centre; spanHz: its width.
    // Returns true when the output changed (rate / centre).
    bool configure(double sampleRate, double centreHz, double spanHz) {
        int f = 1;
        while (sampleRate / (f * 2) >= spanHz * 1.25) f *= 2;   // ~25 % margin, as GroupDecimator
        if (sampleRate == fs && f == factor && std::fabs(centreHz - centre) < 1.0) return false;
        const bool rebuild = sampleRate != fs || f != factor;
        fs = sampleRate;
        centre = centreHz;
        if (rebuild) buildStages(f);
        mix.configure(fs, centre);
        return true;
    }
    double outputRate() const { return fs / factor; }
    double centreHz() const { return centre; }
    int decimation() const { return factor; }
    void reset() { for (auto& s : stages) s.reset(); }

    void process(const std::complex<double>* in, size_t n) {
        mr.resize(n); mi.resize(n);
        mix.process(in, n, mr.data(), mi.data());
        decimate(n);
    }
    void process(const float* inR, const float* inI, size_t n) {
        mr.resize(n); mi.resize(n);
        mix.process(inR, inI, n, mr.data(), mi.data());
        decimate(n);
    }

    // the last block's output
    const float* outR() const { return oR; }
    const float* outI() const { return oI; }
    size_t outN() const { return oN; }
    const std::complex<double>* outComplex() {
        outC.resize(oN);
        for (size_t i = 0; i < oN; i++) outC[i] = std::complex<double>(oR[i], oI[i]);
        return outC.data();
    }
};

} // namespace faststage
