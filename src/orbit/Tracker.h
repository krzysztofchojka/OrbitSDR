#pragma once

// =====================================================================
//  Radio side of the satellite tracker: which satellite / transmitter
//  is tracked, and what frequency the radio should be on right now.
//
//  The map window sets it up; main.cpp applies the frequency every
//  frame to the tuner (VFO) or to the active decoder's channel. It
//  keeps running when the map window is closed.
//
//  Manual fine tuning while tracking is kept: if the user moves the
//  tuner / channel away from the commanded frequency, the difference
//  becomes an offset on top of the Doppler correction (handy for SSB
//  and for satellites whose transmitter is a few kHz off).
// =====================================================================

#include "OrbitMath.h"
#include "SatCatalog.h"
#include "../Demodulator.h"   // Mode
#include <string>
#include <algorithm>

namespace orbit {

enum class TrackMode { Off = 0, Frequency = 1, Doppler = 2 };
enum class TrackTarget { Tuner = 0, Decoder = 1 };

// What the radio should do for a transmitter mode
struct DemodHint {
    Mode mode = Mode::NFM;
    double bandwidthHz = 15000.0;
    std::string decoder;    // "SSTV", "APRS" ... or empty
    std::string text;       // for the UI: "NFM 15 kHz"
};

inline std::string upper(std::string s) { for (auto& c : s) c = (char)std::toupper((unsigned char)c); return s; }

inline DemodHint demodHintFor(const Transmitter& t) {
    DemodHint h;
    const std::string m = upper(t.mode), d = upper(t.desc);
    if (m == "APT") { h.mode = Mode::NFM; h.bandwidthHz = 40000; h.text = "NFM 40 kHz (APT)"; }
    else if (m == "CW" || m == "USB" || m == "SSB" || upper(t.type) == "TRANSPONDER") {
        h.mode = Mode::USB; h.bandwidthHz = m == "CW" ? 1000 : 2800; h.text = m == "CW" ? "USB 1 kHz (CW)" : "USB 2.8 kHz";
    }
    else if (m == "LSB") { h.mode = Mode::LSB; h.bandwidthHz = 2800; h.text = "LSB 2.8 kHz"; }
    else if (m == "AM") { h.mode = Mode::AM; h.bandwidthHz = 10000; h.text = "AM 10 kHz"; }
    else if (m.find("BPSK") != std::string::npos || m.find("QPSK") != std::string::npos || m == "LRPT" ||
             m == "HRPT" || m.find("OQPSK") != std::string::npos || m == "LORA" || m == "DVB-S2") {
        h.mode = Mode::RAW; h.bandwidthHz = std::clamp(t.baud > 0 ? t.baud * 2.0 : 150000.0, 5000.0, 200000.0);
        h.text = "RAW (digital, record I/Q)";
    }
    else {   // FM, AFSK, FSK, GMSK, GFSK...
        h.mode = Mode::NFM;
        h.bandwidthHz = (t.baud >= 9600) ? 25000 : 15000;
        h.text = h.bandwidthHz > 20000 ? "NFM 25 kHz" : "NFM 15 kHz";
    }
    if (d.find("SSTV") != std::string::npos) h.decoder = "SSTV";
    else if (d.find("APRS") != std::string::npos || (m == "AFSK" && t.baud == 1200 && t.downHz > 144000000 && t.downHz < 146000000))
        h.decoder = "APRS";
    return h;
}

class SatTracker {
public:
    // --- station ---
    Observer obs;
    double minElDeg = 0.0;

    // --- what is tracked ---
    int norad = 0;
    std::string satName;
    TrackMode mode = TrackMode::Off;
    TrackTarget target = TrackTarget::Tuner;
    long long downlinkHz = 0;
    Transmitter tx;            // the chosen transmitter (mode hints)
    bool setDemodMode = true;  // also switch the demodulator when tracking starts on the tuner

    // --- live values (update()) ---
    LookAngles la;
    double dopplerHz = 0.0;
    double dopplerRateHzS = 0.0;
    long long wantHz = 0;      // frequency the radio should be on now (incl. user offset)

    // --- applied by main.cpp ---
    long long lastCommandHz = 0;
    long long userOffsetHz = 0;
    std::string applyStatus;   // e.g. "enable a decoder first"
    bool demodApplied = false; // set once main.cpp has switched the demodulator
    std::string activeDecoderName;   // written by main.cpp every frame

    bool active() const { return mode != TrackMode::Off && norad != 0 && downlinkHz > 0; }

    void start(int id, const std::string& name, const Transmitter& t, long long hz, TrackMode m) {
        const bool same = (id == norad && hz == downlinkHz);
        norad = id; satName = name; tx = t; downlinkHz = hz; mode = m;
        if (!same) { userOffsetHz = 0; lastCommandHz = 0; demodApplied = false; }
        applyStatus.clear();
    }
    void stop() { mode = TrackMode::Off; lastCommandHz = 0; applyStatus.clear(); }

    // Called every frame (real time). Returns false when nothing is tracked.
    bool update(const SatCatalog& cat, double jd) {
        if (!active()) return false;
        const Satellite* s = cat.get(norad);
        if (!s) { applyStatus = "TLE missing"; return false; }
        la = look(*s, obs, jd);
        if (!la.ok) { applyStatus = "propagation failed (old TLE?)"; return false; }
        const LookAngles la2 = look(*s, obs, jd + 1.0 / 86400.0);
        dopplerHz = la.dopplerHz((double)downlinkHz);
        dopplerRateHzS = la2.ok ? la2.dopplerHz((double)downlinkHz) - dopplerHz : 0.0;
        const double corr = (mode == TrackMode::Doppler) ? dopplerHz : 0.0;
        wantHz = downlinkHz + (long long)std::llround(corr) + userOffsetHz;
        return true;
    }

    // Main.cpp: the radio is at `currentHz` now. A jump away from what we commanded is
    // the user tuning by hand: keep it as an offset.
    void noteRadioFrequency(long long currentHz) {
        if (lastCommandHz != 0 && std::llabs(currentHz - lastCommandHz) > 250) {
            userOffsetHz += currentHz - lastCommandHz;
            wantHz += currentHz - lastCommandHz;
            lastCommandHz = currentHz;
        }
    }

    void loadSettings(SettingsManager& s) {
        obs.latDeg = std::clamp((double)s.getFloat("trk_lat", (float)obs.latDeg), -90.0, 90.0);
        obs.lonDeg = std::clamp((double)s.getFloat("trk_lon", (float)obs.lonDeg), -180.0, 180.0);
        obs.altM = s.getFloat("trk_alt", (float)obs.altM);
        minElDeg = std::clamp((double)s.getFloat("trk_min_el", 0.0f), 0.0, 45.0);
        target = s.getInt("trk_target", 0) == 1 ? TrackTarget::Decoder : TrackTarget::Tuner;
        setDemodMode = s.getBool("trk_set_mode", true);
    }
    void saveSettings(SettingsManager& s) const {
        s.set("trk_lat", obs.latDeg);
        s.set("trk_lon", obs.lonDeg);
        s.set("trk_alt", obs.altM);
        s.set("trk_min_el", minElDeg);
        s.set("trk_target", (int)target);
        s.set("trk_set_mode", setDemodMode);
    }
};

} // namespace orbit
