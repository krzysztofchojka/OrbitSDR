#pragma once

#include <vector>
#include <complex>
#include <cmath>
#include <algorithm>
#include <functional>
#include "../../Decimator.h"

// =====================================================================
//  Narrow channel receiver: cuts a few kHz out of the I/Q stream and
//  delivers complex baseband at an exact output rate (e.g. 12000 or
//  375 S/s), centred on the given offset.
//
//   NCO mix -> halfband decimation (power of two) -> low-pass FIR ->
//   fractional resampler (linear interpolation, the signal is well
//   oversampled there)
//
//  Used by the FT8 / FT4 / WSPR and CW decoders. Runs on the DSP thread.
// =====================================================================

class NarrowChannel {
    using C = std::complex<double>;
    static constexpr double PI_D = 3.14159265358979323846;
    double fs = 0.0, outRate = 0.0, cutoff = 0.0, transition = 0.0, minOver = 2.0, offsetHz = 1e18;
    double mid = 0.0;
    C nco = C(1.0, 0.0), ncoStep = C(1.0, 0.0);
    IQDecimator decim;
    std::vector<double> fir;
    std::vector<C> hist;
    size_t histPos = 0;
    std::vector<C> mixed;
    double phase = 0.0;      // resampler position between `prev` and `cur` (0..1)
    double step = 1.0;       // input samples per output sample
    C prev = C(0.0, 0.0);
    bool havePrev = false;

    void design() {
        int f = 1;
        // 2..4 x the output rate before the FIR (minOver 2): the linear interpolation then
        // leaves its images > 35 dB down. minOver 1 (wide channels, e.g. VDL2) is cheaper.
        while (fs / (f * 2) >= outRate * minOver) f *= 2;
        decim.setFactor(f);
        mid = fs / decim.getFactor();
        // Blackman window: transition width ~ 5.5 * rate / taps
        int taps = (int)std::ceil(5.5 * mid / std::max(1.0, transition)) | 1;
        taps = std::clamp(taps, 31, 255);
        fir.assign(taps, 0.0);
        const double fc = std::min(cutoff, mid * 0.45) / mid;
        double sum = 0.0;
        for (int i = 0; i < taps; i++) {
            const int m = i - taps / 2;
            const double s = m == 0 ? 2.0 * fc : std::sin(2.0 * PI_D * fc * m) / (PI_D * m);
            const double w = 0.42 - 0.5 * std::cos(2.0 * PI_D * i / (taps - 1)) + 0.08 * std::cos(4.0 * PI_D * i / (taps - 1));
            fir[i] = s * w;
            sum += fir[i];
        }
        for (double& v : fir) v /= sum;
        hist.assign(taps * 2, C(0.0, 0.0));
        histPos = 0;
        step = mid / outRate;
        phase = 0.0;
        havePrev = false;
    }

public:
    // cutoffHz: -6 dB edge of the channel filter (one side, the channel is +-cutoffHz);
    // transitionHz: width of the filter's slope (narrower = more taps)
    void configure(double sampleRate, double channelOffsetHz, double outputRate, double cutoffHz, double transitionHz,
                   double minOversample = 2.0) {
        if (sampleRate != fs || outputRate != outRate || cutoffHz != cutoff || transitionHz != transition || minOversample != minOver) {
            fs = sampleRate;
            minOver = minOversample;
            outRate = outputRate;
            cutoff = cutoffHz;
            transition = transitionHz;
            design();
            offsetHz = 1e18;
        }
        if (channelOffsetHz != offsetHz) {
            offsetHz = channelOffsetHz;
            ncoStep = std::polar(1.0, -2.0 * PI_D * offsetHz / fs);
        }
    }

    double inputRate() const { return fs; }

    void reset() {
        decim.reset();
        std::fill(hist.begin(), hist.end(), C(0.0, 0.0));
        phase = 0.0;
        havePrev = false;
    }

    // Appends the output samples to `out`
    void process(const C* in, size_t n, std::vector<std::complex<float>>& out) {
        if (fs <= 0.0 || n == 0) return;
        mixed.resize(n);
        for (size_t i = 0; i < n; i++) { mixed[i] = in[i] * nco; nco *= ncoStep; }
        nco /= std::abs(nco);
        size_t cnt = 0;
        const C* d = decim.process(mixed.data(), n, cnt);
        const size_t taps = fir.size();
        for (size_t i = 0; i < cnt; i++) {
            hist[histPos] = d[i];
            hist[histPos + taps] = d[i];
            histPos = (histPos + 1) % taps;
            C y(0.0, 0.0);
            const C* h = &hist[histPos];
            for (size_t k = 0; k < taps; k++) y += h[k] * fir[k];
            if (!havePrev) { prev = y; havePrev = true; continue; }
            // output samples that fall between prev (phase 0) and y (phase 1)
            while (phase < 1.0) {
                const C v = prev + (y - prev) * phase;
                out.push_back(std::complex<float>((float)v.real(), (float)v.imag()));
                phase += step;
            }
            phase -= 1.0;
            prev = y;
        }
    }
};

// =====================================================================
//  Collects a sample stream into fixed UTC time slots (FT8 15 s, FT4
//  7.5 s, WSPR 120 s). The time of every sample comes from a running
//  clock that is gently pulled to the time stamps of the blocks (wall
//  clock for a radio, the recording's time for a file), so block jitter
//  doesn't move samples around; a jump (seek, gap) restarts the slot.
// =====================================================================

template <typename T>
class SlotAssembler {
    double period = 15.0, rate = 12000.0;
    double maxLateStart = 1.5;   // a slot that started later than this is dropped
    double nextT = -1.0;
    double slotStart = -1.0;
    size_t firstIdx = 0;
    std::vector<T> buf;
    bool active = false;

public:
    void configure(double periodSec, double sampleRate, double lateStartSec) {
        if (periodSec == period && sampleRate == rate && lateStartSec == maxLateStart) return;
        period = periodSec; rate = sampleRate; maxLateStart = lateStartSec;
        reset();
    }
    void reset() { nextT = -1.0; slotStart = -1.0; active = false; buf.clear(); }

    double currentSlotStart() const { return slotStart; }
    double secondsIntoSlot() const { return active && nextT > 0 ? nextT - slotStart : -1.0; }
    double periodSec() const { return period; }

    // t0: time (unix UTC) of x[0]. done(slotStart, samples) gets every completed slot.
    template <typename F>
    void push(const T* x, size_t n, double t0, F&& done) {
        if (n == 0) return;
        if (nextT < 0.0 || std::fabs(t0 - nextT) > 1.0) {   // start, or a jump: restart
            nextT = t0;
            active = false;
        } else {
            nextT += 0.02 * (t0 - nextT);                   // follow the block clock slowly
        }
        const double dt = 1.0 / rate;
        const size_t len = (size_t)std::llround(period * rate);
        for (size_t i = 0; i < n; i++) {
            const double s = std::floor(nextT / period) * period;
            if (!active || s != slotStart) {
                if (active && firstIdx <= (size_t)(maxLateStart * rate)) done(slotStart, buf);
                slotStart = s;
                buf.assign(len, T());
                firstIdx = (size_t)std::max(0.0, (nextT - s) * rate);
                active = true;
            }
            const size_t idx = (size_t)std::max(0.0, (nextT - slotStart) * rate);
            if (idx < len) buf[idx] = x[i];
            nextT += dt;
        }
    }
};

// =====================================================================
//  Shared first stage for a group of channels close together (VDL2,
//  ACARS): moves the group's centre to 0 Hz and decimates by a power of
//  two once, so the channels don't each have to mix and filter the full
//  sample rate (10 VDL2 channels at 2.4 MS/s: ~3x less work).
// =====================================================================
class GroupDecimator {
    using C = std::complex<double>;
    static constexpr double PI_D = 3.14159265358979323846;
    double fs = 0.0, centre = 1e18;
    int factor = 1;
    IQDecimator decim;
    C nco = C(1.0, 0.0), step = C(1.0, 0.0);
    std::vector<C> mixed;

public:
    // centreHz: offset of the group's centre from the radio's centre; spanHz: from the lowest
    // channel edge to the highest. Returns true when the output changed (channels must restart).
    bool configure(double sampleRate, double centreHz, double spanHz) {
        int f = 1;
        // keep ~25 % margin around the group (the halfband filters' slopes)
        while (sampleRate / (f * 2) >= spanHz * 1.25) f *= 2;
        if (sampleRate == fs && f == factor && std::fabs(centreHz - centre) < 1.0) return false;
        fs = sampleRate;
        factor = f;
        centre = centreHz;
        decim.setFactor(f);
        factor = decim.getFactor();
        step = std::polar(1.0, -2.0 * PI_D * centre / fs);
        return true;
    }
    double outputRate() const { return fs / factor; }
    double centreHz() const { return centre; }

    const C* process(const C* in, size_t n, size_t& outN) {
        mixed.resize(n);
        for (size_t i = 0; i < n; i++) { mixed[i] = in[i] * nco; nco *= step; }
        nco /= std::abs(nco);
        return decim.process(mixed.data(), n, outN);
    }
};
