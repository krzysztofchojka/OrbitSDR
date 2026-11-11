#pragma once

#include "../../Decimator.h"
#include <vector>
#include <complex>
#include <cmath>
#include <cstdint>
#include <algorithm>

// =====================================================================
//  ISM channel: cuts the band around one frequency out of the raw IQ
//  stream and converts it to the format rtl_433 expects.
//
//    slow DC removal (the RTL-SDR spike at the centre frequency) ->
//    NCO mix to 0 Hz -> halfband decimation (power of two, down to
//    < 2x the output rate) -> windowed-sinc fractional resampler to the
//    exact output rate (250 kHz or 1 MHz) -> interleaved int16 I/Q.
// =====================================================================

namespace ism {

class FracResampler {
    using C = std::complex<double>;
    static constexpr int TAPS = 48;
    static constexpr int PHASES = 256;
    std::vector<double> table;   // (PHASES + 1) x TAPS
    std::vector<C> buf;          // history + unconsumed input
    double pos = 0.0, step = 1.0;
    bool passthrough = true;

public:
    // inRate >= outRate; cutoff is the one-sided passband edge in Hz
    void configure(double inRate, double outRate, double cutoffHz) {
        step = inRate / outRate;
        passthrough = std::abs(step - 1.0) < 1e-9 && cutoffHz >= 0.5 * outRate;
        const double pi = 3.14159265358979323846;
        const double fc2 = 2.0 * cutoffHz / inRate;   // normalized to the input rate
        const double half = TAPS / 2.0;
        table.assign((size_t)(PHASES + 1) * TAPS, 0.0);
        for (int p = 0; p <= PHASES; p++) {
            const double frac = (double)p / PHASES;
            double sum = 0.0;
            for (int k = 0; k < TAPS; k++) {
                const double t = frac + (half - 1.0) - k;   // distance from the output instant
                const double x = fc2 * t;
                const double s = std::abs(x) < 1e-12 ? 1.0 : std::sin(pi * x) / (pi * x);
                const double u = t / half;                   // -1 .. 1
                const double w = std::abs(u) >= 1.0 ? 0.0
                    : 0.42 + 0.5 * std::cos(pi * u) + 0.08 * std::cos(2.0 * pi * u);   // Blackman
                const double v = s * w;
                table[(size_t)p * TAPS + k] = v;
                sum += v;
            }
            if (sum != 0.0)
                for (int k = 0; k < TAPS; k++) table[(size_t)p * TAPS + k] /= sum;
        }
        reset();
    }

    void reset() {
        buf.assign(TAPS, C(0.0, 0.0));
        pos = TAPS / 2.0 - 1.0;
    }

    void process(const C* in, size_t n, std::vector<C>& out) {
        if (passthrough) { out.insert(out.end(), in, in + n); return; }
        buf.insert(buf.end(), in, in + n);
        const size_t half = TAPS / 2;
        for (;;) {
            const size_t i0 = (size_t)pos;
            if (i0 + half >= buf.size()) break;
            const double frac = pos - (double)i0;
            const int p = (int)std::lround(frac * PHASES);
            const double* h = &table[(size_t)p * TAPS];
            const C* x = &buf[i0 + 1 - half];
            double re = 0.0, im = 0.0;
            for (int k = 0; k < TAPS; k++) { re += x[k].real() * h[k]; im += x[k].imag() * h[k]; }
            out.emplace_back(re, im);
            pos += step;
        }
        // keep the samples still needed for the next outputs
        const size_t first = (size_t)pos + 1 - half;
        const size_t drop = std::min(first, buf.size());
        buf.erase(buf.begin(), buf.begin() + drop);
        pos -= (double)drop;
    }
};

class Channel {
    using C = std::complex<double>;
    static constexpr double PI_D = 3.14159265358979323846;
    double fs = 0.0, outRate = 0.0, offsetHz = 1e18;
    C nco = C(1.0, 0.0), ncoStep = C(1.0, 0.0);
    // DC estimate with a ~0.3 s time constant: removes the constant LO leakage / ADC
    // offset but barely touches a packet (a burst on the centre frequency is short)
    C dc = C(0.0, 0.0);
    double dcAlpha = 0.0;
    IQDecimator decim;
    FracResampler resampler;
    std::vector<C> mixed, resampled;

public:
    bool configure(double sampleRate, double channelOffsetHz, double outputRate) {
        if (sampleRate < outputRate) return false;
        if (sampleRate != fs || outputRate != outRate) {
            fs = sampleRate;
            outRate = outputRate;
            int f = 1;
            while (fs / (f * 2) >= outRate) f *= 2;
            decim.setFactor(f);
            const double mid = fs / decim.getFactor();
            resampler.configure(mid, outRate, 0.42 * outRate);
            dcAlpha = 1.0 / (0.3 * fs);
            offsetHz = 1e18;   // force the NCO update below
        }
        if (channelOffsetHz != offsetHz) {
            offsetHz = channelOffsetHz;
            ncoStep = std::polar(1.0, -2.0 * PI_D * offsetHz / fs);
        }
        return true;
    }

    void reset() {
        decim.reset();
        resampler.reset();
    }

    double outputRate() const { return outRate; }

    // Appends interleaved int16 I/Q samples at the output rate
    void process(const C* iq, size_t n, std::vector<int16_t>& out) {
        if (fs <= 0.0 || n == 0) return;
        mixed.resize(n);
        for (size_t i = 0; i < n; i++) {
            dc += (iq[i] - dc) * dcAlpha;
            mixed[i] = (iq[i] - dc) * nco;
            nco *= ncoStep;
        }
        nco /= std::abs(nco);   // keep the oscillator on the unit circle

        size_t cnt = 0;
        const C* d = decim.process(mixed.data(), n, cnt);
        resampled.clear();
        resampler.process(d, cnt, resampled);

        const double scale = 32767.0;   // IQ sources deliver +-1.0 full scale
        out.reserve(out.size() + resampled.size() * 2);
        for (const C& s : resampled) {
            out.push_back((int16_t)std::clamp(std::lround(s.real() * scale), -32767L, 32767L));
            out.push_back((int16_t)std::clamp(std::lround(s.imag() * scale), -32767L, 32767L));
        }
    }
};

} // namespace ism
