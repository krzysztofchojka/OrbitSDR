#pragma once

// =====================================================================
//  Ground-station geometry on top of SGP4: look angles, Doppler,
//  sub-satellite point, footprint, sun / night, eclipse and pass
//  prediction.
// =====================================================================

#include "SGP4.h"
#include <vector>
#include <ctime>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <cstdio>

namespace orbit {

constexpr double EARTH_ROT = 7.29211514670698e-5;   // rad/s
constexpr double C_KMS = 299792.458;
constexpr double WGS84_A = 6378.137;
constexpr double WGS84_F = 1.0 / 298.257223563;
constexpr double AU_KM = 149597870.7;

inline double nowJd() {
    using namespace std::chrono;
    const double unixSec = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count() / 1000.0;
    return unixSec / 86400.0 + 2440587.5;
}
inline double jdToUnix(double jd) { return (jd - 2440587.5) * 86400.0; }
inline double unixToJd(double u) { return u / 86400.0 + 2440587.5; }

// "2026-11-13 14:05:09" (UTC or local)
inline std::string formatJd(double jd, bool local, const char* fmt = "%Y-%m-%d %H:%M:%S") {
    std::time_t t = (std::time_t)std::llround(jdToUnix(jd));
    std::tm tmv{};
#ifdef _WIN32
    if (local) localtime_s(&tmv, &t); else gmtime_s(&tmv, &t);
#else
    if (local) localtime_r(&t, &tmv); else gmtime_r(&t, &tmv);
#endif
    char b[64];
    std::strftime(b, sizeof(b), fmt, &tmv);
    return b;
}

// "4m 05s", "1h 12m", "2d 3h"
inline std::string formatDuration(double seconds) {
    char b[48];
    const bool neg = seconds < 0;
    double s = std::fabs(seconds);
    if (s < 3600) std::snprintf(b, sizeof(b), "%s%dm %02ds", neg ? "-" : "", (int)(s / 60), (int)s % 60);
    else if (s < 86400) std::snprintf(b, sizeof(b), "%s%dh %02dm", neg ? "-" : "", (int)(s / 3600), ((int)s % 3600) / 60);
    else std::snprintf(b, sizeof(b), "%s%dd %dh", neg ? "-" : "", (int)(s / 86400), ((int)s % 86400) / 3600);
    return b;
}

inline double wrapLon(double lonDeg) {
    lonDeg = std::fmod(lonDeg + 180.0, 360.0);
    if (lonDeg < 0) lonDeg += 360.0;
    return lonDeg - 180.0;
}

struct Observer {
    double latDeg = 52.2297;   // Warsaw, until the user sets the station
    double lonDeg = 21.0122;
    double altM = 100.0;

    Vec3 ecef() const {
        const double lat = latDeg * DEG2RAD, lon = lonDeg * DEG2RAD;
        const double e2 = WGS84_F * (2.0 - WGS84_F);
        const double sl = std::sin(lat);
        const double n = WGS84_A / std::sqrt(1.0 - e2 * sl * sl);
        const double h = altM / 1000.0;
        return { (n + h) * std::cos(lat) * std::cos(lon), (n + h) * std::cos(lat) * std::sin(lon), (n * (1.0 - e2) + h) * sl };
    }
};

// TEME -> Earth fixed (rotation by GMST, polar motion ignored)
inline Vec3 temeToEcef(const Vec3& r, double theta) {
    const double c = std::cos(theta), s = std::sin(theta);
    return { c * r.x + s * r.y, -s * r.x + c * r.y, r.z };
}
inline Vec3 ecefToTeme(const Vec3& r, double theta) {
    const double c = std::cos(theta), s = std::sin(theta);
    return { c * r.x - s * r.y, s * r.x + c * r.y, r.z };
}

// Geodetic latitude / longitude (deg) and height (km) of an Earth-fixed point
inline void ecefToGeodetic(const Vec3& p, double& latDeg, double& lonDeg, double& altKm) {
    const double e2 = WGS84_F * (2.0 - WGS84_F);
    const double rxy = std::sqrt(p.x * p.x + p.y * p.y);
    double lat = std::atan2(p.z, rxy);
    double n = WGS84_A;
    for (int i = 0; i < 5; i++) {
        const double sl = std::sin(lat);
        n = WGS84_A / std::sqrt(1.0 - e2 * sl * sl);
        lat = std::atan2(p.z + n * e2 * sl, rxy);
    }
    const double sl = std::sin(lat), cl = std::cos(lat);
    altKm = (std::fabs(cl) > 1e-6) ? rxy / cl - n : std::fabs(p.z) - n * (1.0 - e2);
    latDeg = lat * RAD2DEG;
    lonDeg = std::atan2(p.y, p.x) * RAD2DEG;
    (void)sl;
}

// --- Sun (low precision, ~0.01 deg; plenty for the terminator and eclipses) ---
struct SunPos {
    Vec3 teme;        // km, Earth -> Sun (true-of-date equator, close enough to TEME)
    double raRad = 0, decRad = 0;
    double subLatDeg = 0, subLonDeg = 0;
};
inline SunPos sunPosition(double jd) {
    const double n = jd - 2451545.0;
    const double L = std::fmod(280.460 + 0.9856474 * n, 360.0) * DEG2RAD;
    const double g = std::fmod(357.528 + 0.9856003 * n, 360.0) * DEG2RAD;
    const double lam = L + (1.915 * std::sin(g) + 0.020 * std::sin(2 * g)) * DEG2RAD;
    const double eps = (23.439 - 0.0000004 * n) * DEG2RAD;
    const double distAu = 1.00014 - 0.01671 * std::cos(g) - 0.00014 * std::cos(2 * g);
    SunPos s;
    s.teme = Vec3(std::cos(lam), std::cos(eps) * std::sin(lam), std::sin(eps) * std::sin(lam)) * (distAu * AU_KM);
    s.raRad = std::atan2(std::cos(eps) * std::sin(lam), std::cos(lam));
    s.decRad = std::asin(std::sin(eps) * std::sin(lam));
    s.subLatDeg = s.decRad * RAD2DEG;
    s.subLonDeg = wrapLon((s.raRad - gmst(jd)) * RAD2DEG);
    return s;
}

// Elevation of the sun (deg) at a point of the Earth
inline double sunElevationAt(const SunPos& sun, double latDeg, double lonDeg) {
    const double lat = latDeg * DEG2RAD;
    const double dl = (lonDeg - sun.subLonDeg) * DEG2RAD;
    const double s = std::sin(lat) * std::sin(sun.decRad) + std::cos(lat) * std::cos(sun.decRad) * std::cos(dl);
    return std::asin(std::clamp(s, -1.0, 1.0)) * RAD2DEG;
}

// Satellite in the Earth's shadow (cylindrical shadow model)
inline bool inEclipse(const Vec3& rTeme, const SunPos& sun) {
    const Vec3 sh = sun.teme * (1.0 / sun.teme.norm());
    const double along = rTeme.dot(sh);
    if (along > 0) return false;
    const Vec3 perp = rTeme - sh * along;
    return perp.norm() < SGP_RE;
}

// Everything the UI shows about a satellite at one instant
struct LookAngles {
    bool ok = false;
    double azDeg = 0, elDeg = -90;
    double rangeKm = 0;
    double rangeRateKms = 0;   // + = moving away
    double subLatDeg = 0, subLonDeg = 0, altKm = 0;
    double speedKms = 0;
    bool eclipsed = false;
    Vec3 rTeme;

    // Received frequency of a transmitter on `f` Hz (first order Doppler)
    double dopplerHz(double f) const { return -f * rangeRateKms / C_KMS; }
};

inline LookAngles look(const Satellite& sat, const Observer& obs, double jd, bool withSun = false) {
    LookAngles la;
    Vec3 r, v;
    if (!sat.valid || !propagateJd(sat, jd, r, v)) return la;
    const double theta = gmst(jd);
    const Vec3 rE = temeToEcef(r, theta);
    ecefToGeodetic(rE, la.subLatDeg, la.subLonDeg, la.altKm);

    // Observer in the inertial frame: position and velocity from the Earth's rotation
    const Vec3 oE = obs.ecef();
    const Vec3 oT = ecefToTeme(oE, theta);
    const Vec3 oV(-EARTH_ROT * oT.y, EARTH_ROT * oT.x, 0.0);
    const Vec3 rho = r - oT;
    la.rangeKm = rho.norm();
    la.rangeRateKms = rho.dot(v - oV) / la.rangeKm;

    // Topocentric south-east-zenith
    const Vec3 rhoE = rE - oE;
    const double lat = obs.latDeg * DEG2RAD, lon = obs.lonDeg * DEG2RAD;
    const double sla = std::sin(lat), cla = std::cos(lat), slo = std::sin(lon), clo = std::cos(lon);
    const double S = sla * clo * rhoE.x + sla * slo * rhoE.y - cla * rhoE.z;
    const double E = -slo * rhoE.x + clo * rhoE.y;
    const double Z = cla * clo * rhoE.x + cla * slo * rhoE.y + sla * rhoE.z;
    la.elDeg = std::asin(std::clamp(Z / la.rangeKm, -1.0, 1.0)) * RAD2DEG;
    double az = std::atan2(E, -S) * RAD2DEG;
    if (az < 0) az += 360.0;
    la.azDeg = az;
    la.speedKms = v.norm();
    la.rTeme = r;
    if (withSun) la.eclipsed = inEclipse(r, sunPosition(jd));
    la.ok = true;
    return la;
}

inline double elevationAt(const Satellite& sat, const Observer& obs, double jd) {
    LookAngles la = look(sat, obs, jd);
    return la.ok ? la.elDeg : -90.0;
}

// Footprint half-angle (Earth central angle, rad) for a satellite at `altKm`,
// seen above `minElDeg` elevation
inline double footprintAngle(double altKm, double minElDeg = 0.0) {
    if (altKm <= 0) return 0.0;
    const double e = minElDeg * DEG2RAD;
    const double c = SGP_RE * std::cos(e) / (SGP_RE + altKm);
    return std::acos(std::clamp(c, -1.0, 1.0)) - e;
}

// Point at `distRad` (central angle) and `bearingRad` from (lat, lon)
inline void destinationPoint(double latDeg, double lonDeg, double distRad, double bearingRad, double& outLat, double& outLon) {
    const double la1 = latDeg * DEG2RAD, lo1 = lonDeg * DEG2RAD;
    const double la2 = std::asin(std::sin(la1) * std::cos(distRad) + std::cos(la1) * std::sin(distRad) * std::cos(bearingRad));
    const double lo2 = lo1 + std::atan2(std::sin(bearingRad) * std::sin(distRad) * std::cos(la1),
                                        std::cos(distRad) - std::sin(la1) * std::sin(la2));
    outLat = la2 * RAD2DEG;
    outLon = lo2 * RAD2DEG;   // not wrapped: continuous around the centre
}

// --- Passes ---
struct Pass {
    double aosJd = 0, losJd = 0, tcaJd = 0;
    double maxElDeg = 0;
    double aosAzDeg = 0, losAzDeg = 0, tcaAzDeg = 0;
    bool valid = false;
    bool alwaysUp = false;   // above the horizon for the whole search window (GEO)
    double durationSec() const { return (losJd - aosJd) * 86400.0; }
};

namespace detail {
inline double bisectCrossing(const Satellite& s, const Observer& o, double a, double b, bool rising) {
    // a: below (rising) / above (setting), b: the other side
    for (int i = 0; i < 30 && (b - a) * 86400.0 > 0.2; i++) {
        const double m = 0.5 * (a + b);
        const bool up = elevationAt(s, o, m) > 0.0;
        if (up == rising) b = m; else a = m;
    }
    return 0.5 * (a + b);
}
inline double maxElevationTime(const Satellite& s, const Observer& o, double a, double b) {
    const double gr = 0.6180339887498949;
    double c = b - gr * (b - a), d = a + gr * (b - a);
    double fc = elevationAt(s, o, c), fd = elevationAt(s, o, d);
    for (int i = 0; i < 40 && (b - a) * 86400.0 > 0.5; i++) {
        if (fc > fd) { b = d; d = c; fd = fc; c = b - gr * (b - a); fc = elevationAt(s, o, c); }
        else         { a = c; c = d; fc = fd; d = a + gr * (b - a); fd = elevationAt(s, o, d); }
    }
    return 0.5 * (a + b);
}
}

// Next pass that reaches `minElDeg`, starting the search at `startJd`. A pass that is in
// progress at `startJd` is returned with its real AOS (in the past).
inline Pass findNextPass(const Satellite& s, const Observer& o, double startJd, double searchDays = 2.0, double minElDeg = 0.0) {
    Pass p;
    if (!s.valid) return p;
    const double period = s.periodMinutes();
    // Coarse step: short enough not to miss a low LEO pass, longer for high orbits
    const double stepSec = period < 200 ? 30.0 : period < 800 ? 120.0 : 300.0;
    const double step = stepSec / 86400.0;
    const double endJd = startJd + searchDays;

    double t = startJd;
    double el = elevationAt(s, o, t);
    if (el > 0.0) {
        // in a pass: go back to its AOS (at most one orbit)
        double tb = t;
        const double limit = t - std::max(period, 30.0) / 1440.0;
        while (tb > limit && elevationAt(s, o, tb) > 0.0) tb -= step;
        if (tb <= limit) {
            // up for a whole orbit: geostationary-like, look forward for a LOS
            double tf = t;
            while (tf < endJd && elevationAt(s, o, tf) > 0.0) tf += step;
            if (tf >= endJd) {
                p.valid = true; p.alwaysUp = true;
                p.aosJd = startJd; p.losJd = endJd;
                p.tcaJd = startJd;
                LookAngles la = look(s, o, startJd);
                p.maxElDeg = la.elDeg; p.aosAzDeg = p.losAzDeg = p.tcaAzDeg = la.azDeg;
                return p;
            }
        }
        t = tb;
    }

    while (t < endJd) {
        // find the next rise
        double prev = t;
        el = elevationAt(s, o, t);
        while (el <= 0.0 && t < endJd) {
            // far below the horizon a LEO satellite can't rise within a couple of minutes: skip faster
            const double jump = (period < 200 && el < -40.0) ? 4.0 * step : step;
            const double tn = t + jump;
            const double en = elevationAt(s, o, tn);
            if (en > 0.0 && jump > step) {   // overshot into a pass: walk it with the fine step
                double tf = t;
                while (elevationAt(s, o, tf + step) <= 0.0) tf += step;
                prev = tf; t = tf + step; el = elevationAt(s, o, t);
                break;
            }
            prev = t; t = tn; el = en;
        }
        if (t >= endJd) break;
        const double aos = (prev < t && elevationAt(s, o, prev) <= 0.0) ? detail::bisectCrossing(s, o, prev, t, true) : t;
        // find the set
        double t2 = t;
        double prev2 = t2;
        while (elevationAt(s, o, t2) > 0.0 && t2 < endJd + 1.0) { prev2 = t2; t2 += step; }
        const double los = detail::bisectCrossing(s, o, prev2, t2, false);
        const double tca = detail::maxElevationTime(s, o, aos, los);
        const LookAngles at = look(s, o, tca);
        if (at.elDeg >= minElDeg) {
            p.valid = true;
            p.aosJd = aos; p.losJd = los; p.tcaJd = tca;
            p.maxElDeg = at.elDeg; p.tcaAzDeg = at.azDeg;
            p.aosAzDeg = look(s, o, aos).azDeg;
            p.losAzDeg = look(s, o, los).azDeg;
            return p;
        }
        t = los + step;
    }
    return p;
}

inline std::vector<Pass> findPasses(const Satellite& s, const Observer& o, double startJd, int count, double searchDays, double minElDeg) {
    std::vector<Pass> out;
    double t = startJd;
    while ((int)out.size() < count) {
        Pass p = findNextPass(s, o, t, startJd + searchDays - t, minElDeg);
        if (!p.valid) break;
        out.push_back(p);
        if (p.alwaysUp) break;
        t = p.losJd + 60.0 / 86400.0;
        if (t >= startJd + searchDays) break;
    }
    return out;
}

// --- Maidenhead locator ---
inline std::string toMaidenhead(double latDeg, double lonDeg) {
    double lon = wrapLon(lonDeg) + 180.0, lat = std::clamp(latDeg, -89.999, 89.999) + 90.0;
    char b[7];
    b[0] = (char)('A' + (int)(lon / 20)); b[1] = (char)('A' + (int)(lat / 10));
    lon = std::fmod(lon, 20.0); lat = std::fmod(lat, 10.0);
    b[2] = (char)('0' + (int)(lon / 2)); b[3] = (char)('0' + (int)lat);
    lon = std::fmod(lon, 2.0); lat = std::fmod(lat, 1.0);
    b[4] = (char)('a' + (int)(lon * 12)); b[5] = (char)('a' + (int)(lat * 24));
    b[6] = 0;
    return b;
}

// Centre of a 4 or 6 character locator; false when it is not a locator
inline bool fromMaidenhead(std::string loc, double& latDeg, double& lonDeg) {
    for (auto& c : loc) c = (char)std::toupper((unsigned char)c);
    if (loc.size() != 4 && loc.size() != 6) return false;
    if (loc[0] < 'A' || loc[0] > 'R' || loc[1] < 'A' || loc[1] > 'R') return false;
    if (!std::isdigit((unsigned char)loc[2]) || !std::isdigit((unsigned char)loc[3])) return false;
    double lon = (loc[0] - 'A') * 20.0 + (loc[2] - '0') * 2.0;
    double lat = (loc[1] - 'A') * 10.0 + (loc[3] - '0') * 1.0;
    if (loc.size() == 6) {
        if (loc[4] < 'A' || loc[4] > 'X' || loc[5] < 'A' || loc[5] > 'X') return false;
        lon += (loc[4] - 'A') / 12.0 + 1.0 / 24.0;
        lat += (loc[5] - 'A') / 24.0 + 1.0 / 48.0;
    } else { lon += 1.0; lat += 0.5; }
    lonDeg = lon - 180.0;
    latDeg = lat - 90.0;
    return true;
}

} // namespace orbit
