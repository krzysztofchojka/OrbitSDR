#pragma once

#include <vector>
#include <string>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <functional>
#include "ToneDemod.h"

// =====================================================================
//  SSTV decoder core (no UI code, so it can be tested on its own).
//
//  Input: the 12 kHz frequency track from ToneDemod.
//
//  - Waits for a VIS header (1900 Hz leader, 1200 Hz start bit, 7 data
//    bits + even parity at 30 ms each) and starts the matching mode.
//  - A mode can also be started manually; the line phase is then found
//    by folding the sync pulses of the first lines.
//  - Every received line's sync pulse is located and a robust straight
//    line fit (line start vs. line number) gives the real line period.
//    This removes the slant caused by sound card / SDR clock errors.
//    The whole image is re-rendered from the stored frequency track
//    whenever the fit improves, so early lines get straightened too.
// =====================================================================

namespace imgdec {

enum class SstvColor { GBR, Robot36, Robot72, PD };

struct SstvSegment { double startMs; double durMs; };

struct SstvMode {
    const char* name;
    int vis;
    int width;
    int height;          // image lines
    double lineMs;       // duration of one sync period
    double syncMs;
    double syncOffMs;    // sync start relative to line start
    double firstOffMs;   // extra delay before line 0 (Scottie starts with a lone sync)
    SstvColor color;
    SstvSegment seg[4];  // GBR: G,B,R | Robot36: Y,chroma,separator | Robot72: Y,R-Y,B-Y | PD: Y0,R-Y,B-Y,Y1
    int syncLines() const { return color == SstvColor::PD ? height / 2 : height; }
};

inline const std::vector<SstvMode>& sstvModes() {
    auto pd = [](const char* n, int vis, int w, int h, double pixMs) {
        double p = w * pixMs;
        return SstvMode{ n, vis, w, h, 22.08 + 4.0 * p, 20.0, 0.0, 0.0, SstvColor::PD,
                         { {22.08, p}, {22.08 + p, p}, {22.08 + 2 * p, p}, {22.08 + 3 * p, p} } };
    };
    static const std::vector<SstvMode> modes = {
        { "Robot 36",   8, 320, 240, 150.0,   9.0, 0.0, 0.0, SstvColor::Robot36, { {12.0, 88.0}, {106.0, 44.0}, {100.0, 4.5}, {0, 0} } },
        { "Robot 72",  12, 320, 240, 300.0,   9.0, 0.0, 0.0, SstvColor::Robot72, { {12.0, 138.0}, {156.0, 69.0}, {231.0, 69.0}, {0, 0} } },
        { "Martin M1", 44, 320, 256, 446.446, 4.862, 0.0, 0.0, SstvColor::GBR, { {5.434, 146.432}, {152.438, 146.432}, {299.442, 146.432}, {0, 0} } },
        { "Martin M2", 40, 320, 256, 226.798, 4.862, 0.0, 0.0, SstvColor::GBR, { {5.434, 73.216}, {79.222, 73.216}, {153.010, 73.216}, {0, 0} } },
        { "Scottie S1", 60, 320, 256, 428.22, 9.0, 279.48, 9.0, SstvColor::GBR, { {1.5, 138.24}, {141.24, 138.24}, {289.98, 138.24}, {0, 0} } },
        { "Scottie S2", 56, 320, 256, 277.692, 9.0, 179.128, 9.0, SstvColor::GBR, { {1.5, 88.064}, {91.064, 88.064}, {189.628, 88.064}, {0, 0} } },
        { "Scottie DX", 76, 320, 256, 1050.3, 9.0, 694.2, 9.0, SstvColor::GBR, { {1.5, 345.6}, {348.6, 345.6}, {704.7, 345.6}, {0, 0} } },
        pd("PD50",  93, 320, 256, 0.286),
        pd("PD90",  99, 320, 256, 0.532),
        pd("PD120", 95, 640, 496, 0.190),
        pd("PD160", 98, 512, 400, 0.382),
        pd("PD180", 96, 640, 496, 0.286),
        pd("PD240", 97, 640, 496, 0.382),
        pd("PD290", 94, 800, 616, 0.286),
    };
    return modes;
}

inline int sstvModeByVis(int vis) {
    const auto& m = sstvModes();
    for (size_t i = 0; i < m.size(); i++) if (m[i].vis == vis) return (int)i;
    return -1;
}

class SstvDecoder {
public:
    enum class State { Idle, Receiving };

    // --- settings (may be changed at any time) ---
    bool autoSlant = true;
    double manualSlantPpm = 0.0;   // added on top of the automatic correction
    double shiftPx = 0.0;          // horizontal image shift in pixels

    // Called when an image has been fully received (e.g. for auto save)
    std::function<void()> onImageComplete;

    // --- image (RGB, 3 bytes per pixel) ---
    int width = 320, height = 256;
    std::vector<uint8_t> rgb;
    int receivedLines = 0;       // image lines rendered so far
    uint64_t version = 0;        // bumped on every change of 'rgb'

private:
    static constexpr double FS = TRACK_RATE;
    State state = State::Idle;
    int modeIdx = -1;
    bool startedByVis = false;

    // --- VIS detection ---
    std::vector<float> visHist;
    long long visHistBase = 0;     // absolute index of visHist[0]
    long long absPos = 0;          // absolute index of the next sample
    float edgeRing[24] = {};           // 2 ms average: precise falling edge
    int edgePos = 0;
    double edgeSum = 0.0;
    bool edgeBelow = false;
    long long lastCrossDown = -1000000;
    float leadRing[120] = {};          // 10 ms average: robust leader detection in noise
    int leadPos = 0;
    double leadSum = 0.0;
    int leaderRun = 0;
    long long leaderArmedUntil = -1;   // a long enough leader just ended: look for the falling edge
    std::vector<long long> pendingEdges;
    int lastVis = -1;

    // --- image reception ---
    std::vector<float> track;      // frequency track from the image start
    std::vector<double> syncPrefix; // prefix sums of the frequency track (fast window means)
    double lineA = 0.0;            // model: line k starts at lineA + lineB * k (samples)
    double lineB = 0.0;
    double nominalB = 0.0;
    std::vector<double> syncLineIdx, syncLinePos;
    int linesMeasured = 0;
    bool phaseLocked = false;      // line phase known (from VIS or folding)
    bool refolded = false;         // the start time was re-estimated once already

public:
    SstvDecoder() { setImageSize(320, 256); }

    State getState() const { return state; }
    int getModeIdx() const { return modeIdx; }
    bool wasStartedByVis() const { return startedByVis; }
    int getLastVis() const { return lastVis; }
    int totalLines() const { return modeIdx >= 0 ? sstvModes()[modeIdx].height : 0; }

    double effectiveSlantPpm() const {
        if (nominalB <= 0.0) return manualSlantPpm;
        return (lineB / nominalB - 1.0) * 1e6 + manualSlantPpm;
    }

    void clear() {
        state = State::Idle;
        track.clear(); syncPrefix.assign(1, 0.0);
        syncLineIdx.clear(); syncLinePos.clear();
        receivedLines = 0; linesMeasured = 0;
        std::fill(rgb.begin(), rgb.end(), (uint8_t)18);
        version++;
    }

    void stop() {
        if (state != State::Receiving) return;
        render();
        state = State::Idle;
        if (receivedLines > 0 && onImageComplete) onImageComplete();
    }

    // Start receiving 'mode' right now (no VIS); the line phase is found from the sync pulses
    void startManual(int mode) { begin(mode, 0.0, false); }

    // Re-render after a settings change (slant / shift)
    void refresh() { if (modeIdx >= 0) render(); }

    void process(const float* f, size_t n) {
        for (size_t i = 0; i < n; i++) {
            const float v = f[i];
            detectVis(v);
            absPos++;
            if (state == State::Receiving) {
                track.push_back(v);
                syncPrefix.push_back(syncPrefix.back() + v);
            }
        }
        if (state == State::Receiving) updateReception();
    }

private:
    void setImageSize(int w, int h) {
        width = w; height = h;
        rgb.assign((size_t)w * h * 3, (uint8_t)18);
        version++;
    }

    void begin(int mode, double startOffsetSamples, bool byVis) {
        if (state == State::Receiving && receivedLines > 0) stop();
        const SstvMode& m = sstvModes()[mode];
        modeIdx = mode;
        startedByVis = byVis;
        setImageSize(m.width, m.height);
        track.clear(); syncPrefix.assign(1, 0.0);
        syncLineIdx.clear(); syncLinePos.clear();
        receivedLines = 0; linesMeasured = 0;
        nominalB = m.lineMs * FS / 1000.0;
        lineB = nominalB;
        lineA = startOffsetSamples + m.firstOffMs * FS / 1000.0;
        phaseLocked = byVis;
        refolded = false;
        state = State::Receiving;
    }

    // ---------------- VIS ----------------
    double visAvg(long long absFrom, long long absTo) const {
        long long a = std::max(absFrom, visHistBase) - visHistBase;
        long long b = std::min(absTo, visHistBase + (long long)visHist.size()) - visHistBase;
        if (b <= a) return 0.0;
        double s = 0.0;
        for (long long i = a; i < b; i++) s += visHist[(size_t)i];
        return s / (double)(b - a);
    }

    void detectVis(float v) {
        visHist.push_back(v);
        if (visHist.size() > 36000) {   // keep ~1.5 s of history
            size_t drop = visHist.size() - 18000;
            visHist.erase(visHist.begin(), visHist.begin() + (long)drop);
            visHistBase += (long long)drop;
        }

        edgeSum += v - edgeRing[edgePos];
        edgeRing[edgePos] = v;
        edgePos = (edgePos + 1) % 24;
        const double f2ms = edgeSum / 24.0;
        leadSum += v - leadRing[leadPos];
        leadRing[leadPos] = v;
        leadPos = (leadPos + 1) % 120;
        const double f10ms = leadSum / 120.0;

        // The 2 ms average crosses the 1900/1200 Hz midpoint 12 samples after the real edge
        const bool below = f2ms < 1550.0;
        if (below && !edgeBelow) {
            lastCrossDown = absPos - 12;
            if (leaderArmedUntil >= absPos) { pendingEdges.push_back(lastCrossDown); leaderArmedUntil = -1; }
        }
        edgeBelow = below;

        if (std::abs(f10ms - 1900.0) < 120.0) leaderRun++;
        else {
            if (leaderRun > (int)(0.15 * FS)) {
                // The slow average leaves the leader band after the edge already happened
                if (absPos - lastCrossDown < (long long)(0.02 * FS)) pendingEdges.push_back(lastCrossDown);
                else leaderArmedUntil = absPos + (long long)(0.005 * FS);
            }
            leaderRun = 0;
        }

        const long long ms = (long long)(FS / 1000.0);
        for (size_t p = 0; p < pendingEdges.size(); ) {
            const long long e = pendingEdges[p];
            if (absPos < e + 300 * ms + 2) { p++; continue; }
            pendingEdges.erase(pendingEdges.begin() + (long)p);
            evaluateVis(e);
        }
    }

    void evaluateVis(long long e) {
        const long long ms = (long long)(FS / 1000.0);
        // Start / stop bits only need to be clearly below the leader (1900) and the data
        // "0" (1300) is checked by the bits themselves. The tolerance is wide on purpose:
        // with a weak FM signal (satellites) the noise moves the averages by ~100 Hz, and a
        // tight limit threw away good headers. The leader, the even parity and a known mode
        // code still guard against false starts.
        auto near1200 = [](double f) { return std::abs(f - 1200.0) < 150.0; };
        if (!near1200(visAvg(e + 3 * ms, e + 27 * ms))) return;            // start bit
        // stop bit: kept off its edges, the image (sync / porch) follows right after it
        if (!near1200(visAvg(e + 274 * ms, e + 290 * ms))) return;
        int code = 0, ones = 0;
        for (int i = 0; i < 8; i++) {
            double f = visAvg(e + (30 * (i + 1) + 4) * ms, e + (30 * (i + 2) - 4) * ms);
            if (f < 1000.0 || f > 1400.0) return;
            int bit = (f < 1200.0) ? 1 : 0;
            ones += bit;
            if (i < 7) code |= bit << i;
        }
        if (ones % 2 != 0) return;                                          // even parity
        lastVis = code;
        int mode = sstvModeByVis(code);
        if (mode < 0) return;
        // Image data starts right after the stop bit
        const long long imageStart = e + 300 * ms;
        begin(mode, 0.0, true);
        // Samples after imageStart that have already arrived
        for (long long a = imageStart; a < visHistBase + (long long)visHist.size(); a++) {
            float v = visHist[(size_t)(a - visHistBase)];
            track.push_back(v);
            syncPrefix.push_back(syncPrefix.back() + v);
        }
    }

    // ---------------- reception ----------------
    static constexpr double NO_SCORE = -1e9;

    double windowMean(long long a, long long b) const {
        return (syncPrefix[(size_t)b] - syncPrefix[(size_t)a]) / (double)(b - a);
    }

    // Edge window: shorter than the shortest porch (Martin: 0.572 ms), so the picture
    // content after the porch can't pull the estimate
    static constexpr long long EDGE_W = (long long)(0.0005 * FS);

    // Sync pulse score in Hz for a pulse starting at s, based on the falling edge at s
    // and the rising edge at s+len. The position of a step doesn't depend on how bright
    // the neighbouring picture is, so the estimate is unbiased. Taking the smaller edge
    // means a pulse must have both edges: a single big edge, e.g. porch -> white
    // picture, can't win.
    double syncScore(long long s, int len) const {
        const long long w = std::min<long long>(EDGE_W, len / 2);
        if (s - w < 0 || s + len + w >= (long long)syncPrefix.size()) return NO_SCORE;
        double fall = windowMean(s - w, s) - windowMean(s, s + w);
        double rise = windowMean(s + len, s + len + w) - windowMean(s + len - w, s + len);
        return 2.0 * std::min(fall, rise);
    }

    void findPhaseByFolding(int lines) {
        const SstvMode& m = sstvModes()[modeIdx];
        const int len = (int)(m.syncMs * FS / 1000.0);
        const double syncOff = m.syncOffMs * FS / 1000.0;
        double best = NO_SCORE; double bestP = 0.0;
        for (double p = 0.0; p < lineB; p += 1.0) {
            double total = 0.0;
            for (int k = 0; k < lines; k++) {
                double sc = syncScore((long long)(p + k * lineB), len);
                if (sc > NO_SCORE) total += sc;
            }
            if (total > best) { best = total; bestP = p; }
        }
        lineA = bestP - syncOff;
        if (lineA < -lineB * 0.5) lineA += lineB;
    }

    void updateReception() {
        const SstvMode& m = sstvModes()[modeIdx];
        const int syncLen = (int)(m.syncMs * FS / 1000.0);
        const double syncOff = m.syncOffMs * FS / 1000.0;
        const double avail = (double)track.size();
        bool changed = false;

        // Manual start: find the line phase once a few lines have arrived, refine it later
        if (!phaseLocked) {
            int lines = (int)(avail / lineB);
            if (lines >= 3 && lines != linesMeasured) {
                findPhaseByFolding(std::min(lines, 20));
                linesMeasured = lines;
                changed = true;
                if (lines >= 20) { phaseLocked = true; linesMeasured = 0; }
            }
        }

        // Locate the sync pulse of every newly completed line
        if (phaseLocked) {
            while (true) {
                const int k = linesMeasured;
                if (k >= m.syncLines()) break;
                double expected = lineA + lineB * k + syncOff;
                double window = std::max(0.010 * FS, 0.02 * lineB);
                if (expected + window + syncLen + EDGE_W + 1 >= avail) break;
                long long bestS = -1; double best = NO_SCORE;
                for (long long s = (long long)(expected - window); s <= (long long)(expected + window); s++) {
                    double sc = syncScore(s, syncLen);
                    if (sc > best) { best = sc; bestS = s; }
                }
                // A real sync is at least ~300 Hz below the porch/picture on both sides
                if (best > 250.0 && windowMean(bestS, bestS + syncLen) < 1400.0) {
                    syncLineIdx.push_back((double)k);
                    syncLinePos.push_back((double)bestS - syncOff);
                }
                linesMeasured++;

                // Not a single sync found in the first lines: the start time is off, re-fold
                if (linesMeasured == 10 && syncLineIdx.size() < 3 && !refolded) {
                    refolded = true;
                    findPhaseByFolding(10);
                    linesMeasured = 0;
                    syncLineIdx.clear(); syncLinePos.clear();
                    break;
                }

                if (autoSlant) {
                    double a, b;
                    if (robustLineFit(syncLineIdx, syncLinePos, 0.002 * FS, a, b) &&
                        std::abs(b / nominalB - 1.0) < 0.01) {
                        lineA = a; lineB = b;
                    }
                }
                changed = true;
            }
        }

        // Count complete image lines
        const double slant = 1.0 + manualSlantPpm * 1e-6;
        int periods = (int)std::floor((avail - lineA) / (lineB * slant));
        periods = std::clamp(periods, 0, m.syncLines());
        int lines = (m.color == SstvColor::PD) ? periods * 2 : periods;
        if (lines != receivedLines) changed = true;

        if (changed) render();

        if (periods >= m.syncLines()) {
            state = State::Idle;
            if (onImageComplete) onImageComplete();
        }
    }

    // ---------------- rendering ----------------
    void render() {
        if (modeIdx < 0) return;
        const SstvMode& m = sstvModes()[modeIdx];
        const double slant = 1.0 + manualSlantPpm * 1e-6;
        const double B = lineB * slant;
        const double scale = B / nominalB;                    // stretch of all in-line offsets
        const double msToS = FS / 1000.0 * scale;
        const double avail = (double)track.size();
        const double pixShift = shiftPx * (m.seg[0].durMs / m.width) * msToS;

        int periods = (int)std::floor((avail - lineA) / B);
        periods = std::clamp(periods, 0, m.syncLines());

        auto channel = [&](int k, int segIdx, int w, std::vector<uint8_t>& out) {
            const SstvSegment& s = m.seg[segIdx];
            const double start = lineA + B * k + s.startMs * msToS + pixShift;
            const double px = s.durMs * msToS / w;
            out.resize(w);
            for (int x = 0; x < w; x++) out[x] = freqToLevel(rangeAvg(track, start + x * px, start + (x + 1) * px));
        };
        auto put = [&](int y, int x, double r, double g, double b) {
            if (y < 0 || y >= height) return;
            size_t i = ((size_t)y * width + x) * 3;
            rgb[i]     = (uint8_t)std::clamp(r, 0.0, 255.0);
            rgb[i + 1] = (uint8_t)std::clamp(g, 0.0, 255.0);
            rgb[i + 2] = (uint8_t)std::clamp(b, 0.0, 255.0);
        };
        auto putYuv = [&](int y, int x, double Y, double V, double U) {
            put(y, x, Y + 1.402 * (V - 128.0),
                      Y - 0.344136 * (U - 128.0) - 0.714136 * (V - 128.0),
                      Y + 1.772 * (U - 128.0));
        };

        const int w = m.width;
        std::vector<uint8_t> c0, c1, c2, c3;

        if (m.color == SstvColor::GBR) {
            for (int k = 0; k < periods; k++) {
                channel(k, 0, w, c0); channel(k, 1, w, c1); channel(k, 2, w, c2);
                for (int x = 0; x < w; x++) put(k, x, c2[x], c0[x], c1[x]);
            }
        } else if (m.color == SstvColor::Robot72) {
            for (int k = 0; k < periods; k++) {
                channel(k, 0, w, c0); channel(k, 1, w, c1); channel(k, 2, w, c2);
                for (int x = 0; x < w; x++) putYuv(k, x, c0[x], c1[x], c2[x]);
            }
        } else if (m.color == SstvColor::PD) {
            for (int k = 0; k < periods; k++) {
                channel(k, 0, w, c0); channel(k, 1, w, c1); channel(k, 2, w, c2); channel(k, 3, w, c3);
                for (int x = 0; x < w; x++) {
                    putYuv(2 * k, x, c0[x], c1[x], c2[x]);
                    putYuv(2 * k + 1, x, c3[x], c1[x], c2[x]);
                }
            }
        } else { // Robot 36: every line carries Y plus either R-Y or B-Y
            std::vector<std::vector<uint8_t>> Y(periods), Cc(periods);
            std::vector<int> isV(periods, 0);
            for (int k = 0; k < periods; k++) {
                channel(k, 0, w, Y[k]); channel(k, 1, w, Cc[k]);
                const SstvSegment& sep = m.seg[2];
                double start = lineA + B * k + sep.startMs * msToS;
                double f = rangeAvg(track, start, start + sep.durMs * msToS);
                isV[k] = (f < 1900.0) ? 1 : 0;   // 1500 Hz separator = R-Y, 2300 Hz = B-Y
            }
            std::vector<uint8_t> neutral(w, 128);
            for (int k = 0; k < periods; k++) {
                const std::vector<uint8_t>* V = nullptr; const std::vector<uint8_t>* U = nullptr;
                for (int d : {0, -1, 1}) {
                    int j = k + d;
                    if (j < 0 || j >= periods) continue;
                    if (isV[j] && !V) V = &Cc[j];
                    if (!isV[j] && !U) U = &Cc[j];
                }
                if (!V) V = &neutral;
                if (!U) U = &neutral;
                for (int x = 0; x < w; x++) putYuv(k, x, Y[k][x], (*V)[x], (*U)[x]);
            }
        }

        receivedLines = (m.color == SstvColor::PD) ? periods * 2 : periods;
        version++;
    }
};

} // namespace imgdec
