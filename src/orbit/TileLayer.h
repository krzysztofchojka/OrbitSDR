#pragma once

// =====================================================================
//  Map tiles for zooming in (EOX Maps, WGS84 tile grid).
//
//  The map is equirectangular (lat / lon linear), and so is the EOX
//  "WGS84" tile matrix set: level z has 2^(z+1) x 2^z tiles of 256 px,
//  each 180 / 2^z degrees wide. Tiles are drawn straight, no reprojection.
//
//  Layers (https://maps.eox.at, attribution is shown on the map):
//   - s2cloudless-2024  Sentinel-2 cloudless mosaic, 10 m/px (~z13)
//                       CC BY-NC-SA 4.0: free, non-commercial, attribution
//   - osm               OpenStreetMap rendered by EOX (streets, names)
//   - overlay_bright    transparent borders + place names over the photos
//
//  Each tile is cached on disk (<settings>/tiles/<layer>/<z>/<y>/<x>.jpg)
//  and kept in memory while used. Requests go through the OrbitSDR API
//  proxy (?tile=layer/z/y/x, cached on the server for everybody) when it
//  is configured, else / on failure straight to EOX.
// =====================================================================

#include "Http.h"
#include "../PlatformUtils.h"
#include <SFML/Graphics.hpp>
#include <map>
#include <set>
#include <deque>
#include <vector>
#include <string>
#include <memory>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <chrono>

namespace orbit {

struct TileSource {
    std::string id, label, ext;
    int maxZ;                  // highest level asked for (deeper = the last level scaled up)
    std::string attribution;
};

inline const std::vector<TileSource>& tileSources() {
    static const std::vector<TileSource> v = {
        {"s2cloudless-2024", "Satellite photos (Sentinel-2)", "jpg", 14,
         "Sentinel-2 cloudless - https://s2maps.eu by EOX IT Services GmbH (Contains modified Copernicus Sentinel data 2024)"},
        {"osm", "Street map (OpenStreetMap)", "jpg", 16,
         "Data (c) OpenStreetMap contributors, rendering (c) EOX"},
        {"overlay_bright", "Place names / borders", "png", 14,
         "Overlay: (c) OpenStreetMap contributors, rendering (c) EOX"},
    };
    return v;
}

struct TileKey {
    int layer, z, y, x;
    bool operator<(const TileKey& o) const {
        if (layer != o.layer) return layer < o.layer;
        if (z != o.z) return z < o.z;
        if (y != o.y) return y < o.y;
        return x < o.x;
    }
};

class TileCache {
public:
    static constexpr size_t MAX_TEXTURES = 260;      // ~65 MB of video memory
    static constexpr int MAX_IN_FLIGHT = 6;          // parallel downloads (EOX / proxy)
    static constexpr int DISK_LOADS_PER_FRAME = 10;  // decoding JPEGs from disk per frame

    std::string apiUrl, apiKey;   // the OrbitSDR API proxy (empty = EOX directly)

    explicit TileCache(const std::string& dir) : dir(dir), http(4, 20) {}

    // --- per frame ---
    void beginFrame() { wanted.clear(); diskLoads = 0; frame++; }
    // Texture of the tile if it is ready; otherwise it is loaded / downloaded. `prio`: lower = sooner.
    const sf::Texture* get(const TileKey& k, double prio, bool fetch = true) {
        auto it = mem.find(k);
        if (it != mem.end()) { it->second.used = frame; return &it->second.tex; }
        if (!fetch || failed.count(k)) return nullptr;
        if (auto ra = retryAt.find(k); ra != retryAt.end() && clockSec() < ra->second) return nullptr;
        // on disk?
        if (diskLoads < DISK_LOADS_PER_FRAME && !onDiskMissing.count(k)) {
            diskLoads++;
            const std::string p = path(k);
            sf::Texture t;
            if (std::filesystem::exists(p) && t.loadFromFile(p)) return store(k, std::move(t));
            onDiskMissing.insert(k);
        }
        if (!inFlight.count(k)) wanted.push_back({prio, k});
        return nullptr;
    }
    // Only what is already in memory (for the fallback of missing tiles)
    const sf::Texture* peek(const TileKey& k) {
        auto it = mem.find(k);
        if (it == mem.end()) return nullptr;
        it->second.used = frame;
        return &it->second.tex;
    }
    // After drawing: start downloads (the centre of the view first), deliver finished ones
    void endFrame() {
        http.poll();
        std::sort(wanted.begin(), wanted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (const auto& w : wanted) {
            if ((int)inFlight.size() >= MAX_IN_FLIGHT) break;
            if (inFlight.count(w.second)) continue;
            fetch(w.second, !apiUrl.empty() && apiFailures < 3);
        }
        trim();
    }
    bool loading() const { return !inFlight.empty(); }
    int failedCount() const { return (int)failed.size(); }

private:
    struct Entry { sf::Texture tex; uint64_t used = 0; };
    std::string dir;
    HttpQueue http;
    std::map<TileKey, Entry> mem;
    std::set<TileKey> inFlight, failed, onDiskMissing;
    std::vector<std::pair<double, TileKey>> wanted;
    uint64_t frame = 0;
    int diskLoads = 0;
    int apiFailures = 0;   // the proxy does not serve tiles: stop asking it
    std::map<TileKey, double> retryAt;   // after a connection error
    static double clockSec() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

    const TileSource& src(const TileKey& k) const { return tileSources()[k.layer]; }
    std::string path(const TileKey& k) const {
        return dir + "/" + src(k).id + "/" + std::to_string(k.z) + "/" + std::to_string(k.y) + "/" + std::to_string(k.x) + "." + src(k).ext;
    }
    std::string tileQuery(const TileKey& k) const {
        return src(k).id + "/" + std::to_string(k.z) + "/" + std::to_string(k.y) + "/" + std::to_string(k.x);
    }

    const sf::Texture* store(const TileKey& k, sf::Texture&& t) {
        t.setSmooth(true);
        Entry& e = mem[k];
        e.tex = std::move(t);
        e.used = frame;
        return &e.tex;
    }

    void fetch(const TileKey& k, bool viaApi) {
        std::string url;
        std::vector<std::string> hdr;
        if (viaApi) {
            url = apiUrl + (apiUrl.find('?') == std::string::npos ? "?" : "&") + "tile=" + tileQuery(k);
            if (!apiKey.empty()) hdr.push_back("X-OrbitSDR-Key: " + apiKey);
        } else {
            url = "https://tiles.maps.eox.at/wmts/1.0.0/" + src(k).id + "/default/WGS84/" + std::to_string(k.z) + "/" +
                  std::to_string(k.y) + "/" + std::to_string(k.x) + "." + src(k).ext;
        }
        inFlight.insert(k);
        http.get(url, hdr, [this, k, viaApi](const HttpResult& r) {
            const auto* b = (const unsigned char*)r.body.data();
            const size_t n = r.body.size();
            const bool image = r.ok() && n > 8 && ((b[0] == 0xFF && b[1] == 0xD8) || (b[0] == 0x89 && b[1] == 'P'));
            if (!image) {
                if (viaApi) {   // proxy problem: straight to EOX
                    if (r.status != 404) apiFailures++;
                    inFlight.erase(k);
                    fetch(k, false);
                    return;
                }
                inFlight.erase(k);
                // 404 = no tile there (beyond the layer's depth, open sea for the labels): never ask again
                if (r.status == 404 || r.ok()) failed.insert(k);
                else retryAt[k] = clockSec() + 30.0;   // offline / server error: later
                return;
            }
            if (viaApi) apiFailures = 0;
            inFlight.erase(k);
            sf::Texture t;
            if (!t.loadFromMemory(r.body.data(), n)) { failed.insert(k); return; }
            store(k, std::move(t));
            onDiskMissing.erase(k);
            // to the disk cache
            const std::string p = path(k);
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(p).parent_path(), ec);
            std::ofstream f(p + ".tmp", std::ios::binary);
            f.write(r.body.data(), (std::streamsize)n);
            f.close();
            if (f.good()) { std::filesystem::rename(p + ".tmp", p, ec); }
        });
    }

    // Least recently used textures go first
    void trim() {
        if (mem.size() <= MAX_TEXTURES) return;
        std::vector<std::pair<uint64_t, TileKey>> v;
        for (const auto& kv : mem) if (kv.second.used != frame) v.push_back({kv.second.used, kv.first});
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (size_t i = 0; i < v.size() && mem.size() > MAX_TEXTURES; i++) mem.erase(v[i].second);
    }
};

} // namespace orbit
