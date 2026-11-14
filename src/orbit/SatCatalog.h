#pragma once

// =====================================================================
//  Satellite catalog: TLE groups, favourites, user TLEs, transmitters.
//
//  Sources (first that is configured):
//   1. the OrbitSDR API proxy (server/orbitsdr-api.php) from orbitsdr.ini:
//        api_url = https://example.com/orbitsdr-api.php
//        api_key = ORBITSDR_APP   (the default, public on purpose)
//   2. CelesTrak (TLE) and SatNOGS DB (transmitters) directly.
//
//  Everything is cached in <config>/tle/:
//   - a group is refreshed when it is older than REFRESH_HOURS and is
//     still used (with a warning) when offline, but marked stale after
//     STALE_DAYS (TLE accuracy degrades by ~1-2 km per day for LEO),
//   - favourites are kept in their own file, so they stay available even
//     if their group is never opened again,
//   - pasted / imported TLEs ("My TLEs") are never overwritten.
// =====================================================================

#include "SGP4.h"
#include "OrbitMath.h"
#include "Http.h"
#include "Json.h"
#include "../Settings.h"
#include "../PlatformUtils.h"
#include <map>
#include <set>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <functional>

namespace orbit {

constexpr double REFRESH_HOURS = 24.0;     // re-download a group after this long
constexpr double STALE_DAYS = 4.0;         // older cache: shown as stale
constexpr double RETRY_MINUTES = 15.0;     // after a failed download
constexpr double TX_REFRESH_DAYS = 7.0;    // transmitter lists change rarely

struct Transmitter {
    std::string desc;
    std::string mode;      // FM, AFSK, APT, CW, USB, BPSK, ...
    std::string type;      // Transmitter, Transceiver, Transponder
    long long downHz = 0;
    long long downHighHz = 0;   // transponder passband top (0 = single frequency)
    int baud = 0;
    bool alive = true;
    bool builtIn = false;
};

struct SatGroup { std::string id, label; bool remote; };

inline const std::vector<SatGroup>& satGroups() {
    static const std::vector<SatGroup> g = {
        {"favorites", "Favorites", false},
        {"amateur",   "Amateur radio", true},
        {"weather",   "Weather", true},
        {"stations",  "Space stations", true},
        {"cubesat",   "CubeSats", true},
        {"satnogs",   "SatNOGS", true},
        {"noaa",      "NOAA", true},
        {"goes",      "GOES", true},
        {"visual",    "Brightest (visual)", true},
        {"science",   "Science", true},
        {"education", "Education", true},
        {"geo",       "Geostationary", true},
        {"gnss",      "GNSS", true},
        {"custom",    "My TLEs (pasted)", false},
        {"search",    "Search results", false},
    };
    return g;
}

// --- orbitsdr.ini: "key = value" lines, # or ; comments, [sections] ignored ---
// The API key is public on purpose: OrbitSDR is open source, the key only tells the
// proxy that the request comes from OrbitSDR (the proxy caches and rate-limits anyway).
constexpr const char* DEFAULT_API_KEY = "ORBITSDR_APP";
constexpr const char* CONFIG_FILE_NAME = "orbitsdr.ini";

struct AppConfig {
    std::string apiUrl;                   // empty = CelesTrak / SatNOGS directly
    std::string apiKey = DEFAULT_API_KEY;
    std::string mapUrl;                   // empty = the proxy, then NASA
    std::string path;                     // where the file was found (empty = defaults)
};

// The user's own file (settings folder) wins over the one shipped next to the program
inline AppConfig loadAppConfig() {
    AppConfig e;
    const std::string exe = getExecutableDir();
    const std::vector<std::string> candidates = {
        getConfigDir() + PATH_SEP + CONFIG_FILE_NAME,
        exe + "/" + CONFIG_FILE_NAME,
        exe + "/../Resources/" + CONFIG_FILE_NAME,   // macOS .app bundle
        std::string("./") + CONFIG_FILE_NAME };
    for (const auto& p : candidates) {
        std::ifstream f(p);
        if (!f.is_open()) continue;
        std::string line;
        while (std::getline(f, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
            const size_t b = line.find_first_not_of(" \t");
            if (b == std::string::npos || line[b] == '#' || line[b] == ';' || line[b] == '[') continue;
            const size_t eq = line.find('=', b);
            if (eq == std::string::npos) continue;
            std::string k = line.substr(b, eq - b), v = line.substr(eq + 1);
            while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
            for (char& c : k) c = (char)std::tolower((unsigned char)c);
            const size_t vb = v.find_first_not_of(" \t");
            v = vb == std::string::npos ? "" : v.substr(vb);
            if (v.size() >= 2 && (v[0] == '"' || v[0] == '\'') && v.back() == v[0]) v = v.substr(1, v.size() - 2);
            if (k == "api_url") e.apiUrl = v;
            else if (k == "api_key") e.apiKey = v.empty() ? std::string(DEFAULT_API_KEY) : v;
            else if (k == "map_url") e.mapUrl = v;
        }
        e.path = p;
        break;
    }
    return e;
}

// Parses any amount of 2-line or 3-line element sets
inline std::vector<Satellite> parseTleText(const std::string& text) {
    std::vector<Satellite> out;
    std::vector<std::string> lines;
    {
        std::istringstream is(text);
        std::string l;
        while (std::getline(is, l)) {
            while (!l.empty() && (l.back() == '\r' || l.back() == ' ' || l.back() == '\t')) l.pop_back();
            size_t b = l.find_first_not_of(" \t");
            if (b == std::string::npos) continue;
            if (b > 0 && (l[b] == '1' || l[b] == '2') && l.size() - b >= 69) l = l.substr(b);
            lines.push_back(l);
        }
    }
    for (size_t i = 0; i + 1 < lines.size(); ) {
        const std::string& a = lines[i];
        if (a.size() >= 69 && a[0] == '1' && a[1] == ' ' && lines[i + 1].size() >= 69 && lines[i + 1][0] == '2') {
            Satellite s;
            std::string name = (i > 0 && !(lines[i - 1].size() >= 69 && (lines[i - 1][0] == '1' || lines[i - 1][0] == '2')))
                             ? lines[i - 1] : std::string();
            if (parseTle(name, a, lines[i + 1], s)) out.push_back(s);
            i += 2;
        } else i++;
    }
    return out;
}

inline std::string tleText(const Satellite& s) { return s.name + "\n" + s.line1 + "\n" + s.line2 + "\n"; }

inline double unixNow() { return jdToUnix(nowJd()); }

// CelesTrak answers a repeated download of the same query (within ~2 h) with a short text
// instead of the data ("GP data has not updated since your last successful download ...")
inline bool celestrakNotUpdated(const std::string& body) { return body.find("has not updated since") != std::string::npos; }
inline bool celestrakNoData(const std::string& body) { return body.find("No GP data found") != std::string::npos; }

// First line of an unexpected answer for the status line: HTML tags removed, shortened (UTF-8 safe)
inline std::string answerSnippet(const std::string& body) {
    std::string o;
    bool tag = false;
    for (char c : body) {
        if (c == '<') { tag = true; continue; }
        if (c == '>') { tag = false; continue; }
        if (tag) continue;
        if (c == '\r' || c == '\n') { if (o.find_first_not_of(' ') != std::string::npos) break; continue; }
        o += ((unsigned char)c < 32) ? ' ' : c;
        if (o.size() >= 80) {
            while (!o.empty() && ((unsigned char)o.back() & 0xC0) == 0x80) o.pop_back();
            if (!o.empty() && (unsigned char)o.back() >= 0xC0) o.pop_back();
            o += "...";
            break;
        }
    }
    const size_t b = o.find_first_not_of(' ');
    return b == std::string::npos ? std::string("empty answer") : o.substr(b);
}

inline std::string toUpperAscii(std::string s) {
    for (char& c : s) c = (char)std::toupper((unsigned char)c);
    return s;
}

class SatCatalog {
public:
    AppConfig env;   // orbitsdr.ini
    std::map<int, Satellite> sats;                  // every known satellite by NORAD number
    std::map<std::string, std::vector<int>> groups; // members of downloaded groups
    std::set<int> favorites, custom;
    std::vector<int> searchResults;
    std::string status;            // last message for the UI
    bool statusError = false;
    bool searching = false;        // an online search is running
    uint64_t version = 1;          // bumped whenever the satellite set changes

    void init() {
        env = loadAppConfig();
        dir = getConfigDir() + PATH_SEP + "tle";
        MKDIR(dir.c_str());
        loadMeta();
        for (const auto& g : satGroups()) {
            if (!g.remote) continue;
            std::string txt = readFile(groupFile(g.id));
            if (txt.empty()) continue;
            auto v = parseTleText(txt);
            std::vector<int> ids;
            for (auto& s : v) { ids.push_back(s.norad); merge(s); }
            groups[g.id] = ids;
        }
        for (auto& s : parseTleText(readFile(dir + PATH_SEP + "favorites.tle"))) merge(s);
        for (auto& s : parseTleText(readFile(dir + PATH_SEP + "custom.tle"))) { custom.insert(s.norad); sats[s.norad] = s; }
        {
            std::ifstream f(dir + PATH_SEP + "favorites.txt");
            int id;
            while (f >> id) favorites.insert(id);
        }
        if (favorites.empty() && !std::ifstream(dir + PATH_SEP + "favorites.txt").good()) {
            favorites = {25544};   // first start: the ISS
            saveFavorites();
        }
        version++;
        std::string src = env.apiUrl.empty() ? "CelesTrak" : "OrbitSDR API";
        setStatus("TLE source: " + src + (env.path.empty() ? "" : " (orbitsdr.ini)"), false);
    }

    bool usingApi() const { return !env.apiUrl.empty(); }

    // UI thread, every frame
    void poll() { http.poll(); }
    bool busy() const { return http.busy(); }

    // Downloads the group when it is missing or old (or always with `force`)
    void ensureGroup(const std::string& id, bool force = false) {
        if (id == "favorites") { ensureFavorites(force); return; }
        if (id == "custom" || id == "search") return;
        const std::string key = "group:" + id;
        const double now = unixNow();
        if (inFlight.count(key)) return;
        if (!force) {
            if (groups.count(id) && now - fetchedAt[key] < REFRESH_HOURS * 3600.0) return;
            if (now - lastAttempt[key] < RETRY_MINUTES * 60.0) return;
        }
        lastAttempt[key] = now;
        inFlight.insert(key);
        setStatus("Downloading " + groupLabel(id) + "...", false);
        fetchTle(tleUrls("group=" + urlEncode(id), "GROUP=" + urlEncode(id)), [this, id, key](const TleAnswer& a) {
            inFlight.erase(key);
            const double now = unixNow();
            if (a.sats.empty()) {
                if (a.notUpdated && groups.count(id)) {   // CelesTrak: nothing new since our last download
                    fetchedAt[key] = now;
                    saveMeta();
                    setStatus(groupLabel(id) + ": up to date", false);
                    return;
                }
                setStatus(groupLabel(id) + ": download failed (" + a.error + ")" + (groups.count(id) ? ", using cache" : ""), true);
                return;
            }
            std::vector<int> ids;
            std::string txt;
            for (auto& s : a.sats) { ids.push_back(s.norad); merge(s); satFetchedAt[s.norad] = now; txt += tleText(s); }
            groups[id] = ids;
            writeFile(groupFile(id), txt);
            fetchedAt[key] = now;
            saveMeta();
            saveFavoriteTles();
            version++;
            setStatus(groupLabel(id) + ": " + std::to_string(ids.size()) + " satellites updated", false);
        });
    }

    // Favourites that are in no fresh group: download them by catalog number
    void ensureFavorites(bool force = false) {
        const double now = unixNow();
        std::vector<int> need;
        for (int id : favorites) {
            if (custom.count(id)) continue;
            if (inFlight.count("sat:" + std::to_string(id))) continue;
            const bool known = sats.count(id) > 0;
            const double age = now - satFetchedAt[id];
            if (!force && known && age < REFRESH_HOURS * 3600.0) continue;
            if (!force && now - lastAttempt["sat:" + std::to_string(id)] < RETRY_MINUTES * 60.0) continue;
            need.push_back(id);
        }
        if (need.empty()) return;
        for (int id : need) {
            lastAttempt["sat:" + std::to_string(id)] = now;
            inFlight.insert("sat:" + std::to_string(id));
        }
        if (usingApi()) {
            // the proxy accepts up to 50 numbers in one request
            for (size_t i = 0; i < need.size(); i += 50) {
                std::vector<int> chunk(need.begin() + i, need.begin() + std::min(need.size(), i + 50));
                std::string list;
                for (int id : chunk) list += (list.empty() ? "" : ",") + std::to_string(id);
                fetchTle({apiUrl("catnr=" + list)}, [this, chunk](const TleAnswer& a) {
                    if (a.sats.empty()) {   // the proxy does not answer properly: ask CelesTrak
                        for (int id : chunk) fetchFavoriteDirect(id);
                        return;
                    }
                    for (int id : chunk) inFlight.erase("sat:" + std::to_string(id));
                    storeFavorites(a.sats);
                });
            }
        } else {
            for (int id : need) fetchFavoriteDirect(id);
        }
    }

    // Online search by name or catalog number (results go to the "search" group)
    //  1. at once: everything already on disk (downloaded groups, favourites, own TLEs)
    //  2. online: the proxy (when set), CelesTrak when the proxy fails
    void searchOnline(const std::string& query) {
        std::string q = query;
        while (!q.empty() && (q.back() == ' ' || q.back() == '\t')) q.pop_back();
        const size_t b = q.find_first_not_of(" \t");
        q = b == std::string::npos ? std::string() : q.substr(b);
        if (q.size() < 2) { setStatus("Type at least 2 characters to search online", true); return; }
        if (searching) { setStatus("A search is still running...", false); return; }
        const bool numeric = std::all_of(q.begin(), q.end(), [](char c) { return std::isdigit((unsigned char)c); });
        const std::string up = toUpperAscii(q);

        const std::vector<int> local = searchLocal(up, numeric);
        searchResults = local;
        version++;

        // the same query a moment ago: the answer cannot have changed (and CelesTrak asks not to repeat downloads)
        auto c = searchCache.find(up);
        if (c != searchCache.end() && unixNow() - c->second.first < 2 * 3600.0) {
            addSearchResults(c->second.second);
            setStatus("Found " + std::to_string(searchResults.size()) + " for \"" + q + "\" (cached search)", false);
            return;
        }

        searching = true;
        setStatus("Searching online for \"" + q + "\"..." +
                  (local.empty() ? std::string() : " (" + std::to_string(local.size()) + " already known)"), false);
        auto urls = tleUrls(numeric ? "catnr=" + q : "name=" + urlEncode(q), numeric ? "CATNR=" + q : "NAME=" + urlEncode(up));
        fetchTle(urls, [this, q, up, local](const TleAnswer& a) {
            searching = false;
            if (a.sats.empty()) {
                if (!local.empty())
                    setStatus("Found " + std::to_string(local.size()) + " in downloaded groups; online: " + a.error, false);
                else
                    setStatus("Nothing found for \"" + q + "\" (" + a.error + ")", true);
                return;
            }
            const double now = unixNow();
            std::vector<int> online;
            for (auto& s : a.sats) {
                merge(s);
                satFetchedAt[s.norad] = now;
                online.push_back(s.norad);
            }
            // newest first: "METEOR" also matches dozens of 1970s satellites, the list keeps 200
            std::sort(online.begin(), online.end(), std::greater<int>());
            searchCache[up] = {now, online};
            addSearchResults(online);
            saveFavoriteTles();
            setStatus("Found " + std::to_string(searchResults.size()) + " for \"" + q + "\" (" + std::to_string(a.sats.size()) +
                      " online) - shown in Search results", false);
        }, true);
    }

    // Adds pasted / imported TLE text to "My TLEs". Returns the NORAD numbers added.
    std::vector<int> addCustomTle(const std::string& text) {
        std::vector<int> added;
        for (auto& s : parseTleText(text)) {
            sats[s.norad] = s;
            custom.insert(s.norad);
            added.push_back(s.norad);
        }
        if (!added.empty()) { saveCustom(); version++; }
        return added;
    }

    void removeCustom(int id) {
        if (!custom.erase(id)) return;
        saveCustom();
        version++;
    }

    void toggleFavorite(int id) {
        if (favorites.count(id)) favorites.erase(id); else favorites.insert(id);
        saveFavorites();
        saveFavoriteTles();
        version++;
    }

    std::vector<int> members(const std::string& id) const {
        std::vector<int> v;
        if (id == "favorites") v.assign(favorites.begin(), favorites.end());
        else if (id == "custom") v.assign(custom.begin(), custom.end());
        else if (id == "search") v = searchResults;
        else { auto it = groups.find(id); if (it != groups.end()) v = it->second; }
        v.erase(std::remove_if(v.begin(), v.end(), [&](int n) { return !sats.count(n); }), v.end());
        return v;
    }

    const Satellite* get(int id) const { auto it = sats.find(id); return it == sats.end() ? nullptr : &it->second; }

    // Hours since the group was downloaded (-1: never)
    double groupAgeHours(const std::string& id) const {
        if (id == "favorites" || id == "custom" || id == "search") return -1.0;
        auto it = fetchedAt.find("group:" + id);
        if (it == fetchedAt.end() || it->second <= 0) return -1.0;
        return (unixNow() - it->second) / 3600.0;
    }

    static std::string groupLabel(const std::string& id) {
        for (const auto& g : satGroups()) if (g.id == id) return g.label;
        return id;
    }

    // --- transmitters (SatNOGS DB + a few built-in) ---
    const std::vector<Transmitter>& transmitters(int norad) {
        auto it = tx.find(norad);
        const std::string key = "tx:" + std::to_string(norad);
        const double now = unixNow();
        if (it == tx.end()) {
            // first ask for this satellite: the disk cache, then the network
            std::string cached = readFile(dir + PATH_SEP + "tx_" + std::to_string(norad) + ".json");
            tx[norad] = parseTransmitters(cached, norad);
            it = tx.find(norad);
        }
        const bool old = now - fetchedAt[key] > TX_REFRESH_DAYS * 86400.0;
        if (old && !inFlight.count(key) && now - lastAttempt[key] > RETRY_MINUTES * 60.0) {
            lastAttempt[key] = now;
            inFlight.insert(key);
            std::vector<std::string> urls;
            if (usingApi()) urls.push_back(apiUrl("transmitters=" + std::to_string(norad)));
            urls.push_back("https://db.satnogs.org/api/transmitters/?format=json&satellite__norad_cat_id=" + std::to_string(norad));
            fetchTransmitters(norad, key, urls, 0);
        }
        return it->second;
    }
    uint64_t txVersion = 1;

    // --- world map picture ---
    std::string mapFilePath() const { return getConfigDir() + PATH_SEP + "worldmap.jpg"; }
    // done(ok, why): `why` lists what every source answered when nothing worked
    void downloadMap(std::function<void(bool, const std::string&)> done) {
        std::vector<std::string> urls;
        if (!env.mapUrl.empty()) urls.push_back(env.mapUrl);
        if (usingApi()) urls.push_back(apiUrl("map=1"));
        // NASA Blue Marble, December 2004 (the old eoimages.gsfc.nasa.gov host is gone)
        urls.push_back("https://assets.science.nasa.gov/content/dam/science/esd/eo/images/bmng/bmng-topography-bathymetry/december/world.topo.bathy.200412.3x5400x2700.jpg");
        tryMap(urls, 0, std::move(done), std::string());
    }

private:
    std::string dir;
    std::map<std::string, double> fetchedAt, lastAttempt;   // unix seconds, by key
    std::map<int, double> satFetchedAt;
    std::set<std::string> inFlight;
    std::map<int, std::vector<Transmitter>> tx;
    std::map<std::string, std::pair<double, std::vector<int>>> searchCache;   // upper-case query -> (time, ids)
    HttpQueue http;

    void setStatus(const std::string& s, bool err) { status = s; statusError = err; }

    std::string apiUrl(const std::string& query) const {
        return env.apiUrl + (env.apiUrl.find('?') == std::string::npos ? "?" : "&") + query;
    }
    bool isApiUrl(const std::string& url) const { return usingApi() && url.rfind(env.apiUrl, 0) == 0; }
    // the key goes to our own proxy only, never to CelesTrak / SatNOGS / map hosts
    std::vector<std::string> headersFor(const std::string& url) const {
        std::vector<std::string> h;
        if (isApiUrl(url) && !env.apiKey.empty()) h.push_back("X-OrbitSDR-Key: " + env.apiKey);
        return h;
    }
    std::string sourceName(const std::string& url) const {
        if (isApiUrl(url)) return "OrbitSDR API";
        if (url.find("celestrak.org") != std::string::npos) return "CelesTrak";
        if (url.find("satnogs.org") != std::string::npos) return "SatNOGS";
        size_t a = url.find("://");
        a = a == std::string::npos ? 0 : a + 3;
        return url.substr(a, url.find('/', a) - a);
    }
    // the proxy first (when configured), CelesTrak directly as the fallback
    std::vector<std::string> tleUrls(const std::string& apiQuery, const std::string& celestrakQuery) const {
        std::vector<std::string> v;
        if (usingApi()) v.push_back(apiUrl(apiQuery));
        v.push_back("https://celestrak.org/NORAD/elements/gp.php?" + celestrakQuery + "&FORMAT=tle");
        return v;
    }

    struct TleAnswer {
        std::vector<Satellite> sats;
        std::string error;        // what every source answered (when sats is empty)
        bool notUpdated = false;  // CelesTrak: no newer data since our last download
    };
    // Tries the URLs in order until one answers with TLE data
    void fetchTle(std::vector<std::string> urls, std::function<void(const TleAnswer&)> done, bool urgent = false) {
        fetchTleFrom(std::move(urls), std::move(done), urgent, 0, TleAnswer{});
    }
    void fetchTleFrom(std::vector<std::string> urls, std::function<void(const TleAnswer&)> done, bool urgent, size_t i, TleAnswer acc) {
        if (i >= urls.size()) { done(acc); return; }
        const std::string url = urls[i];
        http.get(url, headersFor(url), [this, urls, done, urgent, i, acc](const HttpResult& r) mutable {
            auto v = r.ok() ? parseTleText(r.body) : std::vector<Satellite>();
            if (!v.empty()) { TleAnswer a; a.sats = std::move(v); done(a); return; }
            std::string why;
            if (celestrakNoData(r.body)) why = "nothing found";   // CelesTrak sends it with HTTP 404
            else if (!r.ok()) why = r.error + (r.body.empty() ? std::string() : " \"" + answerSnippet(r.body) + "\"");
            else if (celestrakNotUpdated(r.body)) { acc.notUpdated = true; why = "no newer data yet"; }
            else if (r.body.find_first_not_of(" \r\n\t") == std::string::npos) why = "nothing found";
            else why = "not TLE data: \"" + answerSnippet(r.body) + "\"";
            acc.error += (acc.error.empty() ? "" : "; ") + sourceName(urls[i]) + ": " + why;
            fetchTleFrom(urls, done, urgent, i + 1, acc);
        }, urgent);
    }

    void fetchFavoriteDirect(int id) {
        fetchTle({"https://celestrak.org/NORAD/elements/gp.php?CATNR=" + std::to_string(id) + "&FORMAT=tle"}, [this, id](const TleAnswer& a) {
            inFlight.erase("sat:" + std::to_string(id));
            if (!a.sats.empty()) { storeFavorites(a.sats); return; }
            if (a.notUpdated) { satFetchedAt[id] = unixNow(); saveMeta(); return; }   // what we have is current
            setStatus("Favorites update failed (" + a.error + "), using cache", true);
        });
    }
    void storeFavorites(const std::vector<Satellite>& v) {
        const double now = unixNow();
        for (const auto& s : v) { merge(s); satFetchedAt[s.norad] = now; }
        saveFavoriteTles();
        version++;
        setStatus("Favorites updated", false);
    }

    std::vector<int> searchLocal(const std::string& up, bool numeric) const {
        std::vector<int> v;
        const int num = numeric ? std::atoi(up.c_str()) : -1;
        for (const auto& kv : sats) {
            if (v.size() >= 200) break;
            if (kv.first == num || (!numeric && toUpperAscii(kv.second.name).find(up) != std::string::npos)) v.push_back(kv.first);
        }
        return v;
    }
    void addSearchResults(const std::vector<int>& ids) {
        for (int id : ids) {
            if (searchResults.size() >= 200) break;
            if (std::find(searchResults.begin(), searchResults.end(), id) == searchResults.end()) searchResults.push_back(id);
        }
        version++;
    }

    void fetchTransmitters(int norad, std::string key, std::vector<std::string> urls, size_t i) {
        http.get(urls[i], headersFor(urls[i]), [this, norad, key, urls, i](const HttpResult& r) {
            JsonValue j;
            if (!r.ok() || !parseJson(r.body, j) || j.type != JsonValue::Array) {
                if (i + 1 < urls.size()) fetchTransmitters(norad, key, urls, i + 1);   // proxy failed: SatNOGS directly
                else inFlight.erase(key);                                             // keep what we have
                return;
            }
            inFlight.erase(key);
            writeFile(dir + PATH_SEP + "tx_" + std::to_string(norad) + ".json", r.body);
            fetchedAt[key] = unixNow();
            saveMeta();
            tx[norad] = parseTransmitters(r.body, norad);
            txVersion++;
        });
    }

    std::string groupFile(const std::string& id) const { return dir + PATH_SEP + "group_" + id + ".tle"; }

    // Newer element sets win; pasted TLEs are never replaced by downloads
    void merge(const Satellite& s) {
        if (custom.count(s.norad)) return;
        auto it = sats.find(s.norad);
        if (it == sats.end() || it->second.epochJd <= s.epochJd) sats[s.norad] = s;
    }

    void tryMap(std::vector<std::string> urls, size_t i, std::function<void(bool, const std::string&)> done, std::string errs) {
        if (i >= urls.size()) { done(false, errs); return; }
        http.get(urls[i], headersFor(urls[i]), [this, urls, i, done, errs](const HttpResult& r) mutable {
            const auto* b = (const unsigned char*)r.body.data();
            const size_t n = r.body.size();
            const bool jpeg = n > 2 && b[0] == 0xFF && b[1] == 0xD8;
            const bool png = n > 8 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G';
            std::string why;
            if (!r.ok()) why = r.error;
            else if ((!jpeg && !png) || n < 20000) why = "not a JPEG / PNG picture: \"" + answerSnippet(r.body) + "\"";
            else {
                // written to a temporary file and closed before anyone reads it
                const std::string p = mapFilePath(), tmp = p + ".tmp";
                bool ok;
                { std::ofstream f(tmp, std::ios::binary); f.write(r.body.data(), (std::streamsize)n); ok = f.good(); }
                if (ok) { std::remove(p.c_str()); ok = std::rename(tmp.c_str(), p.c_str()) == 0; }
                if (ok) { done(true, std::string()); return; }
                why = "cannot write " + p;
            }
            errs += (errs.empty() ? "" : "; ") + sourceName(urls[i]) + ": " + why;
            tryMap(urls, i + 1, done, errs);
        });
    }

    std::vector<Transmitter> parseTransmitters(const std::string& json, int norad) const {
        std::vector<Transmitter> out;
        JsonValue j;
        if (!json.empty() && parseJson(json, j) && j.type == JsonValue::Array) {
            for (const auto& t : j.arr) {
                if (t.type != JsonValue::Object || t.isNull("downlink_low")) continue;
                Transmitter x;
                x.desc = t.getString("description");
                x.mode = t.getString("mode");
                x.type = t.getString("type");
                x.downHz = (long long)t.getNumber("downlink_low");
                x.downHighHz = t.isNull("downlink_high") ? 0 : (long long)t.getNumber("downlink_high");
                x.baud = (int)t.getNumber("baud");
                const std::string st = t.getString("status", "active");
                x.alive = t.getBool("alive", true) && st == "active";
                if (x.downHz > 0) out.push_back(x);
            }
        }
        if (out.empty()) out = builtInTransmitters(norad);
        // working transmitters first, then by frequency
        std::stable_sort(out.begin(), out.end(), [](const Transmitter& a, const Transmitter& b) {
            if (a.alive != b.alive) return a.alive;
            return a.downHz < b.downHz;
        });
        return out;
    }

    static std::vector<Transmitter> builtInTransmitters(int norad) {
        std::vector<Transmitter> v;
        auto add = [&](const char* d, const char* m, long long f, int baud = 0) {
            Transmitter t; t.desc = d; t.mode = m; t.downHz = f; t.baud = baud; t.builtIn = true; v.push_back(t);
        };
        if (norad == 25544) {   // ISS
            add("Voice / SSTV events", "FM", 145800000);
            add("APRS digipeater", "AFSK", 145825000, 1200);
            add("FM repeater (U/V) downlink", "FM", 437800000);
        } else if (norad == 27607) {
            add("SO-50 FM repeater", "FM", 436795000);
        }
        return v;
    }

    // --- files ---
    static std::string readFile(const std::string& p) {
        std::ifstream f(p, std::ios::binary);
        if (!f.is_open()) return "";
        std::stringstream ss; ss << f.rdbuf();
        return ss.str();
    }
    static void writeFile(const std::string& p, const std::string& data) {
        std::string tmp = p + ".tmp";
        { std::ofstream f(tmp, std::ios::binary); f << data; if (!f.good()) return; }
        std::remove(p.c_str());
        std::rename(tmp.c_str(), p.c_str());
    }

    void loadMeta() {
        std::ifstream f(dir + PATH_SEP + "meta.txt");
        std::string k; double v;
        while (f >> k >> v) {
            if (k.rfind("sat:", 0) == 0) satFetchedAt[std::atoi(k.c_str() + 4)] = v;
            else fetchedAt[k] = v;
        }
    }
    void saveMeta() {
        std::ostringstream o;
        o.precision(12);
        for (auto& kv : fetchedAt) if (kv.second > 0) o << kv.first << " " << kv.second << "\n";
        for (auto& kv : satFetchedAt) if (favorites.count(kv.first)) o << "sat:" << kv.first << " " << kv.second << "\n";
        writeFile(dir + PATH_SEP + "meta.txt", o.str());
    }
    void saveFavorites() {
        std::ostringstream o;
        for (int id : favorites) o << id << "\n";
        writeFile(dir + PATH_SEP + "favorites.txt", o.str());
    }
    void saveFavoriteTles() {
        std::string txt;
        for (int id : favorites) { auto it = sats.find(id); if (it != sats.end() && !custom.count(id)) txt += tleText(it->second); }
        writeFile(dir + PATH_SEP + "favorites.tle", txt);
        saveMeta();
    }
    void saveCustom() {
        std::string txt;
        for (int id : custom) { auto it = sats.find(id); if (it != sats.end()) txt += tleText(it->second); }
        writeFile(dir + PATH_SEP + "custom.tle", txt);
    }
};

} // namespace orbit
