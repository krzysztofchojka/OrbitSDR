#pragma once

#include <vector>
#include <deque>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <functional>
#include "ToneDemod.h"

// =====================================================================
//  WEFAX (HF radiofax) decoder core (no UI code).
//
//  Input: the 12 kHz frequency track from ToneDemod (1500 Hz = black,
//  2300 Hz = white). A transmission is:
//    start tone (black/white at 300 Hz for IOC 576, 675 Hz for IOC 288, 5 s)
//    phasing    (~30 s of black lines with a short white pulse at the line start)
//    image      (LPM lines per minute, IOC * pi pixels per line)
//    stop tone  (450 Hz, 5 s), then black
//
//  The brightness of the whole reception is stored, so the picture can be
//  re-rendered at any time when the slant or shift settings change.
// =====================================================================

namespace imgdec {

class WefaxDecoder {
public:
    enum class State { Idle, Receiving };

    // --- settings ---
    int lpm = 120;
    int ioc = 576;
    bool autoApt = true;          // start/stop on the APT tones
    double manualSlantPpm = 0.0;  // added on top of the automatic correction
    double shiftPx = 0.0;         // horizontal shift in pixels

    std::function<void()> onImageComplete;

    // --- image (8-bit grey) ---
    int width = 1810;
    int lines = 0;
    std::vector<uint8_t> gray;    // width * lines
    int dirtyFrom = 0;            // first line changed since the UI last copied the image
    uint64_t version = 0;

    static constexpr int MAX_LINES = 6000;

private:
    static constexpr double FS = TRACK_RATE;
    State state = State::Idle;

    std::vector<uint8_t> lum;     // brightness from the reception start
    double phaseSamples = 0.0;    // automatic line start offset (from phasing)
    double autoSlantPpm = 0.0;    // automatic slant (from phasing)
    bool phasingDone = false;
    int renderedLines = 0;
    double renderedSlant = 0.0, renderedShift = 0.0;

    // --- APT tone detection ---
    long long absPos = 0;
    bool aptHigh = false;
    std::deque<long long> transitions;
    int startRun = 0, stopRun = 0, noToneRun = 0;
    int startIoc = 576;
    bool startToneSeen = false;
    long long stopToneBegin = -1;

public:
    State getState() const { return state; }
    bool isPhased() const { return phasingDone; }
    double effectiveSlantPpm() const { return autoSlantPpm + manualSlantPpm; }
    double receivedSeconds() const { return lum.size() / FS; }
    int lineWidthFor(int iocValue) const { return (int)std::lround(iocValue * 3.14159265358979323846); }

    void clear() {
        state = State::Idle;
        lum.clear();
        lines = 0; gray.clear(); dirtyFrom = 0; renderedLines = 0;
        phaseSamples = 0.0; autoSlantPpm = 0.0; phasingDone = false;
        version++;
    }

    void startManual() { begin(); }

    void stop() {
        if (state != State::Receiving) return;
        state = State::Idle;
        renderNew();
        if (lines > 0 && onImageComplete) onImageComplete();
    }

    // Re-render everything (after a settings change)
    void refresh() { renderAll(); }

    void process(const float* f, size_t n) {
        for (size_t i = 0; i < n; i++) {
            uint8_t v = freqToLevel(f[i]);
            detectApt(v);
            absPos++;
            if (state == State::Receiving) {
                lum.push_back(v);
                if (!phasingDone && lum.size() == (size_t)(20.0 * FS)) analysePhasing();
            }
        }
        if (state == State::Receiving) {
            renderNew();
            if (lines >= MAX_LINES) stop();
        }
    }

private:
    double linePeriod() const {
        return 60.0 / lpm * FS * (1.0 + (autoSlantPpm + manualSlantPpm) * 1e-6);
    }

    void begin() {
        if (state == State::Receiving) stop();
        width = lineWidthFor(ioc);
        lum.clear();
        lines = 0; gray.clear(); dirtyFrom = 0; renderedLines = 0;
        phaseSamples = 0.0; autoSlantPpm = 0.0; phasingDone = false;
        stopToneBegin = -1;
        state = State::Receiving;
        version++;
    }

    // ---------------- APT tones ----------------
    void detectApt(uint8_t v) {
        // Hysteresis slicer; every black<->white transition is recorded
        bool high = aptHigh ? (v > 90) : (v > 165);
        if (high != aptHigh) { transitions.push_back(absPos); aptHigh = high; }
        const long long window = (long long)(0.5 * FS);
        while (!transitions.empty() && transitions.front() < absPos - window) transitions.pop_front();

        if (absPos % 600 != 0) return;   // evaluate every 50 ms
        const double toneHz = transitions.size() / 2.0 / 0.5;
        auto isNear = [&](double target) { return std::abs(toneHz - target) < target * 0.1; };
        const bool start300 = isNear(300.0), start675 = isNear(675.0), stop450 = isNear(450.0);

        if (start300 || start675) { startRun++; if (startRun == 1) startIoc = start300 ? 576 : 288; }
        else startRun = 0;
        if (stop450) { if (stopRun == 0) stopToneBegin = absPos - window; stopRun++; }
        else stopRun = 0;

        if (!autoApt) { startToneSeen = false; return; }

        // Start tone for >= 1.5 s: wait for its end, then start receiving
        if (startRun >= 30) startToneSeen = true;
        if (startToneSeen && startRun == 0) {
            if (++noToneRun >= 4) {
                startToneSeen = false; noToneRun = 0;
                if (state == State::Receiving) stop();
                ioc = startIoc;
                begin();
            }
        } else noToneRun = 0;

        // Stop tone for >= 1.5 s ends the picture
        if (state == State::Receiving && stopRun >= 30) {
            // Drop the lines that contain the stop tone
            long long keep = std::max<long long>(0, stopToneBegin - (absPos - (long long)lum.size()));
            if ((size_t)keep < lum.size()) lum.resize((size_t)keep);
            renderAll();
            stop();
        }
    }

    // ---------------- phasing ----------------
    // The first 20 s should be phasing lines: a uniform line (black) with one short
    // pulse (white, ~5 % of the line) at the line start. Each line is checked on its
    // own; only consecutive lines that really look like phasing lines are used, so
    // a short phasing period or a manual start in the middle of a chart can't
    // produce a bogus correction. The pulse position gives the line start, and its
    // drift from line to line gives the clock error (slant).
    void analysePhasing() {
        phasingDone = true;
        const double L = 60.0 / lpm * FS * (1.0 + manualSlantPpm * 1e-6);
        const int P = (int)std::lround(L);
        if (P <= 0) return;
        const int w = std::max(4, (int)(0.05 * P));
        const int nLines = (int)((lum.size() - w) / L);

        std::vector<double> xs, ys;
        std::vector<double> win(P);
        int rejectsInRow = 0;
        double prevPos = -1.0;
        for (int k = 0; k < nLines; k++) {
            const size_t base = (size_t)(k * L);
            if (base + P + w > lum.size()) break;
            // Moving sum over the pulse width (the window may run into the next line)
            double sum = 0.0;
            for (int i = 0; i < w; i++) sum += lum[base + i];
            double lineSum = 0.0;
            for (int j = 0; j < P; j++) {
                win[j] = sum;
                lineSum += lum[base + j];
                sum += lum[base + j + w] - lum[base + j];
            }
            const bool whitePulse = lineSum / P < 128.0;
            int bestJ = 0;
            for (int j = 1; j < P; j++)
                if (whitePulse ? (win[j] > win[bestJ]) : (win[j] < win[bestJ])) bestJ = j;

            // Is this a phasing line? Strong pulse and a uniform rest of the line.
            const double pulseMean = win[bestJ] / w;
            double outSum = 0.0; int outN = 0;
            for (int j = 0; j < P; j++) {
                int d = (j - bestJ + P) % P;
                if (d < w) continue;
                outSum += lum[base + j]; outN++;
            }
            const double outMean = outN ? outSum / outN : 0.0;
            int uniform = 0;
            for (int j = 0; j < P; j++) {
                int d = (j - bestJ + P) % P;
                if (d >= w && std::abs(lum[base + j] - outMean) < 50.0) uniform++;
            }
            const bool isPhasing = std::abs(pulseMean - outMean) > 100.0 && uniform > 0.85 * outN;

            if (!isPhasing) {
                if (!xs.empty() && ++rejectsInRow >= 3) break;   // phasing is over
                continue;
            }
            rejectsInRow = 0;
            double pos = (double)base + bestJ;
            if (prevPos >= 0.0) {   // keep positions continuous across the line boundary
                double expected = prevPos + L * (k - xs.back());
                if (pos - expected > P / 2.0) pos -= L;
                if (pos - expected < -P / 2.0) pos += L;
            }
            xs.push_back(k); ys.push_back(pos);
            prevPos = pos;
        }
        if (xs.size() < 8) return;   // no usable phasing: keep the picture as it is

        double a = ys[0] - xs[0] * L, b = L;
        if (robustLineFit(xs, ys, 0.002 * P, a, b, 8) && std::abs(b / L - 1.0) < 0.002) {
            autoSlantPpm = (b / L - 1.0) * 1e6;
        } else {
            b = L;
            a = ys[0] - xs[0] * L;
        }
        phaseSamples = std::fmod(a, b);
        if (phaseSamples < 0) phaseSamples += b;
        renderAll();
    }

    // ---------------- rendering ----------------
    void renderLine(int k, double L, double start0) {
        uint8_t* row = &gray[(size_t)k * width];
        const double base = start0 + k * L;
        const double px = L / width;
        for (int x = 0; x < width; x++)
            row[x] = (uint8_t)std::clamp(rangeAvg(lum, base + x * px, base + (x + 1) * px), 0.0, 255.0);
    }

    double startOffset(double L) const { return phaseSamples + shiftPx * (L / width); }

    void renderAll() {
        const double L = linePeriod();
        const double s0 = startOffset(L);
        int n = (int)std::floor((lum.size() - std::max(0.0, s0)) / L);
        n = std::clamp(n, 0, MAX_LINES);
        lines = n;
        gray.assign((size_t)width * lines, 0);
        for (int k = 0; k < lines; k++) renderLine(k, L, s0);
        renderedLines = lines;
        renderedSlant = manualSlantPpm; renderedShift = shiftPx;
        dirtyFrom = 0;
        version++;
    }

    void renderNew() {
        if (renderedSlant != manualSlantPpm || renderedShift != shiftPx) { renderAll(); return; }
        const double L = linePeriod();
        const double s0 = startOffset(L);
        int n = (int)std::floor((lum.size() - std::max(0.0, s0)) / L);
        n = std::clamp(n, 0, MAX_LINES);
        if (n <= renderedLines) return;
        gray.resize((size_t)width * n, 0);
        for (int k = renderedLines; k < n; k++) renderLine(k, L, s0);
        dirtyFrom = std::min(dirtyFrom, renderedLines);
        lines = n;
        renderedLines = n;
        version++;
    }
};

} // namespace imgdec
