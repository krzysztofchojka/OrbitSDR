#pragma once

// =====================================================================
//  Positions heard by the decoders (AIS ships, APRS stations, ACARS
//  aircraft, later ADS-B), shown on the satellite map.
//
//  Decoders call positionBus().report(...) from their DSP thread; the map
//  reads snapshot() on the UI thread. Every report is stamped with the
//  map's clock (main.cpp sets it every frame): real time while receiving
//  live, the recording's time while an old IQ file is played, so "last
//  30 minutes" means the same thing in both cases.
// =====================================================================

#include <string>
#include <vector>
#include <map>
#include <deque>
#include <mutex>
#include <atomic>
#include <chrono>

enum class PosKind { AIS = 0, APRS = 1, ACARS = 2, ADSB = 3, Count = 4 };

inline const char* posKindName(PosKind k) {
    switch (k) {
    case PosKind::AIS: return "AIS";
    case PosKind::APRS: return "APRS";
    case PosKind::ACARS: return "ACARS";
    case PosKind::ADSB: return "ADS-B";
    default: return "?";
    }
}

struct PosReport {
    PosKind kind = PosKind::AIS;
    std::string id;          // MMSI, callsign, registration, ICAO address
    std::string label;       // name to show (ship name, callsign, flight)
    std::string info;        // one line: type, speed, comment...
    double lat = 0, lon = 0;
    double courseDeg = -1;   // -1 = unknown
    double speedKn = -1;
    double altM = -1e9;      // aircraft altitude (-1e9 = unknown)
    char symTable = 0, symCode = 0;   // APRS symbol (0 = none)
    double unix = 0;         // map clock when heard
};

struct PosTrack {
    PosReport last;
    struct Pt { double lat, lon, unix; };
    std::deque<Pt> trail;    // oldest first, the last point is `last`
    int count = 0;
};

class PositionBus {
public:
    // main.cpp, every frame: the map's time (unix seconds)
    void setClock(double unix) { clock.store(unix); }
    double now() const {
        const double c = clock.load();
        if (c > 0) return c;
        return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    void report(PosReport r) {
        if (!(r.lat >= -90 && r.lat <= 90 && r.lon >= -180 && r.lon <= 180) || (r.lat == 0 && r.lon == 0)) return;
        if (r.unix <= 0) r.unix = now();
        std::lock_guard<std::mutex> l(mtx);
        PosTrack& t = tracks[{(int)r.kind, r.id}];
        if (r.label.empty()) r.label = t.last.label;          // keep what earlier messages told us
        if (r.info.empty()) r.info = t.last.info;
        if (r.symCode == 0) { r.symTable = t.last.symTable; r.symCode = t.last.symCode; }
        // a jump back in time (seek in a recording): the trail starts again
        if (!t.trail.empty() && r.unix < t.trail.back().unix - 1.0) t.trail.clear();
        if (t.trail.empty() || t.trail.back().lat != r.lat || t.trail.back().lon != r.lon) t.trail.push_back({r.lat, r.lon, r.unix});
        else t.trail.back().unix = r.unix;
        while (t.trail.size() > MAX_TRAIL) t.trail.pop_front();
        t.last = r;
        t.count++;
        version++;
    }

    // Name / details without a position (AIS static data, ...)
    void describe(PosKind kind, const std::string& id, const std::string& label, const std::string& info) {
        std::lock_guard<std::mutex> l(mtx);
        auto it = tracks.find({(int)kind, id});
        if (it == tracks.end()) return;
        if (!label.empty()) it->second.last.label = label;
        if (!info.empty()) it->second.last.info = info;
        version++;
    }

    std::vector<PosTrack> snapshot() const {
        std::lock_guard<std::mutex> l(mtx);
        std::vector<PosTrack> v;
        v.reserve(tracks.size());
        for (const auto& kv : tracks) v.push_back(kv.second);
        return v;
    }
    void clear(PosKind kind) {
        std::lock_guard<std::mutex> l(mtx);
        for (auto it = tracks.begin(); it != tracks.end();) {
            if (it->first.first == (int)kind) it = tracks.erase(it); else ++it;
        }
        version++;
    }
    size_t count(PosKind kind) const {
        std::lock_guard<std::mutex> l(mtx);
        size_t n = 0;
        for (const auto& kv : tracks) if (kv.first.first == (int)kind) n++;
        return n;
    }
    std::atomic<uint64_t> version {1};

private:
    static constexpr size_t MAX_TRAIL = 200;
    mutable std::mutex mtx;
    std::map<std::pair<int, std::string>, PosTrack> tracks;
    std::atomic<double> clock {0.0};
};

inline PositionBus& positionBus() {
    static PositionBus bus;
    return bus;
}
