#pragma once

#include <algorithm>

// =====================================================================
//  RDS assisted gain tuning (WFM).
//
//  The RDS subcarrier (57 kHz, low level) is the first thing to suffer
//  from intermodulation, ADC overload or multipath, so the RDS block
//  error rate is a good measure of how clean the received signal is.
//
//  While the software AGC runs, this optimiser looks at the block error
//  rate every few seconds. If it is high, it tries a lower gain limit
//  (perturb and observe): a step that helps is kept and the next one is
//  tried, a step that doesn't help is undone and the other direction is
//  tried. When nothing helps it waits before trying again, a little longer
//  each time; on a station that never sent RDS it doesn't try again at all
//  (until the next retune), so the audio isn't disturbed by the trials.
//
//  The result is a cap for the AGC (the highest gain step it may use),
//  so the AGC's own overload protection keeps working. The optimiser can
//  only lower the gain below what the AGC would choose, or give back a
//  cap it set before.
// =====================================================================

class RdsGainOptimizer {
public:
    // Back to the plain AGC (call after a retune or when RDS / AGC is switched off)
    void reset() {
        state = State::Settle; cap = -1; prevCap = -1; t0 = -1.0; dir = -1; triedDirs = 0; baseMetric = 1.0;
        holdS = HOLD_MIN_S; everSynced = false;
    }

    // now: seconds, bler: RDS block error rate 0..1, step: the AGC's current gain step,
    // steps: number of gain steps. Returns the cap for the AGC (-1 = no cap).
    int update(double now, bool synced, double bler, int step, int steps) {
        if (steps <= 1 || step < 0) return cap;
        if (t0 < 0.0) t0 = now;
        const double metric = synced ? bler : 1.0;   // no sync at all = worst
        if (synced) everSynced = true;

        switch (state) {
        case State::Settle:
            if (now - t0 < SETTLE_S) break;
            baseMetric = metric;
            if (metric <= GOOD) enterHold(now);
            else startTrial(now, step, steps);
            break;

        case State::Trial:
            if (now - t0 < TRIAL_S) break;
            if (metric < baseMetric - MIN_GAIN) {
                // Better: keep it and go on in the same direction
                baseMetric = metric;
                triedDirs = 0;
                holdS = HOLD_MIN_S;
                if (metric <= GOOD) enterHold(now);
                else startTrial(now, step, steps);
            } else {
                // Not better: undo and try the other direction
                cap = prevCap;
                dir = -dir;
                if (++triedDirs >= 2) enterFailedHold(now);
                else startTrial(now, step, steps);
            }
            break;

        case State::Hold:
            // Reception got worse later on (fading, a different programme): look again.
            // A station that never sent RDS is left alone.
            if (everSynced && now - t0 > holdS && metric > GOOD) { triedDirs = 0; state = State::Settle; t0 = now - SETTLE_S; }
            break;
        }
        return cap;
    }

    bool isLimiting() const { return cap >= 0; }

private:
    enum class State { Settle, Trial, Hold };
    static constexpr double SETTLE_S = 4.0;   // RDS needs a moment to sync after a retune
    static constexpr double TRIAL_S = 4.0;    // time to judge one gain step
    static constexpr double HOLD_MIN_S = 20.0;   // pause before trying again ...
    static constexpr double HOLD_MAX_S = 300.0;  // ... growing up to this after failed rounds
    static constexpr double GOOD = 0.10;      // block error rate that needs no tuning
    static constexpr double MIN_GAIN = 0.05;  // improvement needed to keep a step
    static constexpr int STEP = 2;            // gain table steps per trial (~2-5 dB on R820T)

    State state = State::Settle;
    int cap = -1, prevCap = -1;
    double t0 = -1.0;
    int dir = -1;              // start by lowering the gain (overload is the usual problem)
    int triedDirs = 0;
    double baseMetric = 1.0;
    double holdS = HOLD_MIN_S;
    bool everSynced = false;

    void enterHold(double now) { state = State::Hold; t0 = now; }
    void enterFailedHold(double now) { enterHold(now); holdS = std::min(holdS * 2.0, HOLD_MAX_S); }

    void startTrial(double now, int step, int steps) {
        // Without a cap the AGC already uses the highest gain it considers safe,
        // so "up" only means giving back a cap that was set before
        const int base = (cap >= 0) ? cap : step;
        if (dir > 0 && cap < 0) {
            if (++triedDirs >= 2) { enterFailedHold(now); return; }
            dir = -1;
        }
        int next = std::clamp(base + dir * STEP, 0, steps - 1);
        if (next == base) {   // already at the end of the range in this direction
            if (++triedDirs >= 2) { enterFailedHold(now); return; }
            dir = -dir;
            next = std::clamp(base + dir * STEP, 0, steps - 1);
            if (next == base || (dir > 0 && cap < 0)) { enterFailedHold(now); return; }
        }
        prevCap = cap;
        cap = (dir > 0 && next >= steps - 1) ? -1 : next;   // back at the top = no cap
        state = State::Trial;
        t0 = now;
    }
};
