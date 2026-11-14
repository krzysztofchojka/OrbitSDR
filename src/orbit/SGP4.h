#pragma once

// =====================================================================
//  SGP4 orbit propagator (near-earth model, WGS-72 constants).
//
//  A port of the reference implementation by D. Vallado ("Revisiting
//  Spacetrack Report #3", 2006), reduced to what OrbitSDR needs:
//  TLE parsing, initialisation and propagation to TEME position and
//  velocity.
//
//  Deep-space objects (period >= 225 min: GEO, Molniya, GNSS) are
//  propagated with the near-earth equations without the lunar / solar
//  terms. With TLEs refreshed every few days the error stays well
//  below a degree on the map, which is plenty for a ground station
//  display, but such satellites are flagged as `deepSpace`.
// =====================================================================

#include <cmath>
#include <string>
#include <cstdlib>
#include <cctype>

namespace orbit {

constexpr double PI     = 3.14159265358979323846;
constexpr double TWOPI  = 2.0 * PI;
constexpr double DEG2RAD = PI / 180.0;
constexpr double RAD2DEG = 180.0 / PI;

// WGS-72 (the constants the TLEs are generated with)
constexpr double SGP_MU      = 398600.8;          // km^3/s^2
constexpr double SGP_RE      = 6378.135;          // km
constexpr double SGP_J2      = 0.001082616;
constexpr double SGP_J3      = -0.00000253881;
constexpr double SGP_J4      = -0.00000165597;
constexpr double SGP_J3OJ2   = SGP_J3 / SGP_J2;
inline double sgpXke() { static const double x = 60.0 / std::sqrt(SGP_RE * SGP_RE * SGP_RE / SGP_MU); return x; }

struct Vec3 {
    double x = 0, y = 0, z = 0;
    Vec3() = default;
    Vec3(double a, double b, double c) : x(a), y(b), z(c) {}
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
    double dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    double norm() const { return std::sqrt(x * x + y * y + z * z); }
};

// Julian date from a calendar date (UTC)
inline double julianDate(int year, int mon, int day, int hr, int minute, double sec) {
    return 367.0 * year - std::floor((7 * (year + std::floor((mon + 9) / 12.0))) * 0.25)
         + std::floor(275 * mon / 9.0) + day + 1721013.5
         + ((sec / 60.0 + minute) / 60.0 + hr) / 24.0;
}

// Greenwich mean sidereal time (rad), IAU-82
inline double gmst(double jdUt1) {
    const double t = (jdUt1 - 2451545.0) / 36525.0;
    double temp = -6.2e-6 * t * t * t + 0.093104 * t * t
                + (876600.0 * 3600.0 + 8640184.812866) * t + 67310.54841;   // seconds
    temp = std::fmod(temp * DEG2RAD / 240.0, TWOPI);
    if (temp < 0.0) temp += TWOPI;
    return temp;
}

// One satellite: TLE elements + SGP4 state
struct Satellite {
    // --- from the TLE ---
    std::string name;
    std::string line1, line2;
    int norad = 0;
    std::string intlDesig;
    double epochJd = 0.0;
    double incloDeg = 0, nodeoDeg = 0, ecco = 0, argpoDeg = 0, moDeg = 0;
    double revPerDay = 0.0;
    double bstar = 0.0;
    bool valid = false;
    bool deepSpace = false;
    bool decayed = false;   // propagation found the orbit below the surface

    // --- SGP4 state ---
    int isimp = 0;
    double aycof = 0, con41 = 0, cc1 = 0, cc4 = 0, cc5 = 0, d2 = 0, d3 = 0, d4 = 0, delmo = 0, eta = 0,
           argpdot = 0, omgcof = 0, sinmao = 0, t2cof = 0, t3cof = 0, t4cof = 0, t5cof = 0,
           x1mth2 = 0, x7thm1 = 0, mdot = 0, nodedot = 0, xlcof = 0, xmcof = 0, nodecf = 0;
    double inclo = 0, nodeo = 0, argpo = 0, mo = 0, noUnkozai = 0;

    double periodMinutes() const { return revPerDay > 0 ? 1440.0 / revPerDay : 0.0; }
};

// "-11606-4" -> -0.11606e-4 (the implied-decimal fields of the TLE)
inline double tleExpField(const std::string& s) {
    std::string t;
    for (char c : s) if (c != ' ') t += c;
    if (t.empty()) return 0.0;
    double sign = 1.0;
    size_t i = 0;
    if (t[0] == '-') { sign = -1.0; i = 1; }
    else if (t[0] == '+') i = 1;
    size_t ePos = t.find_first_of("+-", i);
    std::string mant = t.substr(i, ePos == std::string::npos ? std::string::npos : ePos - i);
    int expo = 0;
    if (ePos != std::string::npos) expo = std::atoi(t.substr(ePos).c_str());
    double m = std::atof(("0." + mant).c_str());
    return sign * m * std::pow(10.0, expo);
}

inline bool tleChecksumOk(const std::string& line) {
    if (line.size() < 69) return false;
    int sum = 0;
    for (size_t i = 0; i < 68; i++) {
        char c = line[i];
        if (c >= '0' && c <= '9') sum += c - '0';
        else if (c == '-') sum += 1;
    }
    return (sum % 10) == (line[68] - '0');
}

// Initialises the SGP4 constants (Vallado's initl + sgp4init, near-earth part)
inline void sgp4Init(Satellite& s) {
    const double xke = sgpXke();
    const double x2o3 = 2.0 / 3.0;

    s.inclo = s.incloDeg * DEG2RAD;
    s.nodeo = s.nodeoDeg * DEG2RAD;
    s.argpo = s.argpoDeg * DEG2RAD;
    s.mo    = s.moDeg * DEG2RAD;
    const double noKozai = s.revPerDay * TWOPI / 1440.0;   // rad/min
    const double ecco = s.ecco;

    // --- initl ---
    const double ak = std::pow(xke / noKozai, x2o3);
    const double cosio = std::cos(s.inclo);
    const double cosio2 = cosio * cosio;
    const double eccsq = ecco * ecco;
    const double omeosq = 1.0 - eccsq;
    const double rteosq = std::sqrt(omeosq);
    const double d1 = 0.75 * SGP_J2 * (3.0 * cosio2 - 1.0) / (rteosq * omeosq);
    double del = d1 / (ak * ak);
    const double adel = ak * (1.0 - del * del - del * (1.0 / 3.0 + 134.0 * del * del / 81.0));
    del = d1 / (adel * adel);
    s.noUnkozai = noKozai / (1.0 + del);
    const double ao = std::pow(xke / s.noUnkozai, x2o3);
    const double sinio = std::sin(s.inclo);
    const double po = ao * omeosq;
    const double con42 = 1.0 - 5.0 * cosio2;
    s.con41 = -con42 - cosio2 - cosio2;
    const double posq = po * po;
    const double rp = ao * (1.0 - ecco);

    // --- sgp4init ---
    const double ss = 78.0 / SGP_RE + 1.0;
    const double qzms2t = std::pow((120.0 - 78.0) / SGP_RE, 4);

    s.isimp = (rp < (220.0 / SGP_RE + 1.0)) ? 1 : 0;
    double sfour = ss, qzms24 = qzms2t;
    const double perige = (rp - 1.0) * SGP_RE;
    if (perige < 156.0) {
        sfour = perige - 78.0;
        if (perige < 98.0) sfour = 20.0;
        qzms24 = std::pow((120.0 - sfour) / SGP_RE, 4);
        sfour = sfour / SGP_RE + 1.0;
    }
    const double pinvsq = 1.0 / posq;
    const double tsi = 1.0 / (ao - sfour);
    s.eta = ao * ecco * tsi;
    const double etasq = s.eta * s.eta;
    const double eeta = ecco * s.eta;
    const double psisq = std::fabs(1.0 - etasq);
    const double coef = qzms24 * std::pow(tsi, 4);
    const double coef1 = coef / std::pow(psisq, 3.5);
    const double cc2 = coef1 * s.noUnkozai * (ao * (1.0 + 1.5 * etasq + eeta * (4.0 + etasq))
                     + 0.375 * SGP_J2 * tsi / psisq * s.con41 * (8.0 + 3.0 * etasq * (8.0 + etasq)));
    s.cc1 = s.bstar * cc2;
    double cc3 = 0.0;
    if (ecco > 1.0e-4) cc3 = -2.0 * coef * tsi * SGP_J3OJ2 * s.noUnkozai * sinio / ecco;
    s.x1mth2 = 1.0 - cosio2;
    s.cc4 = 2.0 * s.noUnkozai * coef1 * ao * omeosq *
            (s.eta * (2.0 + 0.5 * etasq) + ecco * (0.5 + 2.0 * etasq)
             - SGP_J2 * tsi / (ao * psisq) *
               (-3.0 * s.con41 * (1.0 - 2.0 * eeta + etasq * (1.5 - 0.5 * eeta))
                + 0.75 * s.x1mth2 * (2.0 * etasq - eeta * (1.0 + etasq)) * std::cos(2.0 * s.argpo)));
    s.cc5 = 2.0 * coef1 * ao * omeosq * (1.0 + 2.75 * (etasq + eeta) + eeta * etasq);
    const double cosio4 = cosio2 * cosio2;
    const double temp1 = 1.5 * SGP_J2 * pinvsq * s.noUnkozai;
    const double temp2 = 0.5 * temp1 * SGP_J2 * pinvsq;
    const double temp3 = -0.46875 * SGP_J4 * pinvsq * pinvsq * s.noUnkozai;
    s.mdot = s.noUnkozai + 0.5 * temp1 * rteosq * s.con41
           + 0.0625 * temp2 * rteosq * (13.0 - 78.0 * cosio2 + 137.0 * cosio4);
    s.argpdot = -0.5 * temp1 * con42 + 0.0625 * temp2 * (7.0 - 114.0 * cosio2 + 395.0 * cosio4)
              + temp3 * (3.0 - 36.0 * cosio2 + 49.0 * cosio4);
    const double xhdot1 = -temp1 * cosio;
    s.nodedot = xhdot1 + (0.5 * temp2 * (4.0 - 19.0 * cosio2) + 2.0 * temp3 * (3.0 - 7.0 * cosio2)) * cosio;
    s.omgcof = s.bstar * cc3 * std::cos(s.argpo);
    s.xmcof = 0.0;
    if (ecco > 1.0e-4) s.xmcof = -x2o3 * coef * s.bstar / eeta;
    s.nodecf = 3.5 * omeosq * xhdot1 * s.cc1;
    s.t2cof = 1.5 * s.cc1;
    if (std::fabs(cosio + 1.0) > 1.5e-12) s.xlcof = -0.25 * SGP_J3OJ2 * sinio * (3.0 + 5.0 * cosio) / (1.0 + cosio);
    else s.xlcof = -0.25 * SGP_J3OJ2 * sinio * (3.0 + 5.0 * cosio) / 1.5e-12;
    s.aycof = -0.5 * SGP_J3OJ2 * sinio;
    const double delmotemp = 1.0 + s.eta * std::cos(s.mo);
    s.delmo = delmotemp * delmotemp * delmotemp;
    s.sinmao = std::sin(s.mo);
    s.x7thm1 = 7.0 * cosio2 - 1.0;

    s.deepSpace = (TWOPI / s.noUnkozai) >= 225.0;
    if (s.deepSpace) s.isimp = 1;

    if (s.isimp != 1) {
        const double cc1sq = s.cc1 * s.cc1;
        s.d2 = 4.0 * ao * tsi * cc1sq;
        const double temp = s.d2 * tsi * s.cc1 / 3.0;
        s.d3 = (17.0 * ao + sfour) * temp;
        s.d4 = 0.5 * temp * ao * tsi * (221.0 * ao + 31.0 * sfour) * s.cc1;
        s.t3cof = s.d2 + 2.0 * cc1sq;
        s.t4cof = 0.25 * (3.0 * s.d3 + s.cc1 * (12.0 * s.d2 + 10.0 * cc1sq));
        s.t5cof = 0.2 * (3.0 * s.d4 + 12.0 * s.cc1 * s.d3 + 6.0 * s.d2 * s.d2 + 15.0 * cc1sq * (2.0 * s.d2 + cc1sq));
    }
}

// Parses one TLE (name may be empty). Returns false for malformed lines.
inline bool parseTle(const std::string& name, const std::string& l1in, const std::string& l2in, Satellite& s) {
    auto trimRight = [](std::string x) {
        while (!x.empty() && (x.back() == '\r' || x.back() == '\n' || x.back() == ' ')) x.pop_back();
        return x;
    };
    std::string l1 = trimRight(l1in), l2 = trimRight(l2in);
    if (l1.size() < 69 || l2.size() < 69 || l1[0] != '1' || l2[0] != '2') return false;
    try {
        s = Satellite();
        s.line1 = l1;
        s.line2 = l2;
        s.norad = std::atoi(l1.substr(2, 5).c_str());
        s.intlDesig = trimRight(l1.substr(9, 8));
        int epy = std::atoi(l1.substr(18, 2).c_str());
        double epd = std::atof(l1.substr(20, 12).c_str());
        int year = epy < 57 ? 2000 + epy : 1900 + epy;
        s.epochJd = julianDate(year, 1, 1, 0, 0, 0.0) - 1.0 + epd;
        s.bstar = tleExpField(l1.substr(53, 8));

        s.incloDeg = std::atof(l2.substr(8, 8).c_str());
        s.nodeoDeg = std::atof(l2.substr(17, 8).c_str());
        s.ecco     = std::atof(("0." + l2.substr(26, 7)).c_str());
        s.argpoDeg = std::atof(l2.substr(34, 8).c_str());
        s.moDeg    = std::atof(l2.substr(43, 8).c_str());
        s.revPerDay = std::atof(l2.substr(52, 11).c_str());
    } catch (...) { return false; }
    if (s.revPerDay <= 0.0 || s.ecco >= 1.0) return false;

    std::string n = trimRight(name);
    if (n.size() > 2 && n[0] == '0' && n[1] == ' ') n = n.substr(2);   // "0 ISS (ZARYA)" (3LE format)
    size_t b = n.find_first_not_of(' ');
    n = (b == std::string::npos) ? std::string() : n.substr(b);
    s.name = n.empty() ? ("NORAD " + std::to_string(s.norad)) : n;

    sgp4Init(s);
    s.valid = true;
    return true;
}

// Propagates to `tsince` minutes after the TLE epoch. r in km, v in km/s (TEME frame).
// Returns false when the orbit is invalid at that time (decayed, eccentricity out of range).
inline bool sgp4(const Satellite& s, double tsince, Vec3& r, Vec3& v) {
    const double xke = sgpXke();
    const double x2o3 = 2.0 / 3.0;
    const double t = tsince;

    const double xmdf = s.mo + s.mdot * t;
    const double argpdf = s.argpo + s.argpdot * t;
    const double nodedf = s.nodeo + s.nodedot * t;
    double argpm = argpdf;
    double mm = xmdf;
    const double t2 = t * t;
    double nodem = nodedf + s.nodecf * t2;
    double tempa = 1.0 - s.cc1 * t;
    double tempe = s.bstar * s.cc4 * t;
    double templ = s.t2cof * t2;

    if (s.isimp != 1) {
        const double delomg = s.omgcof * t;
        const double delmtemp = 1.0 + s.eta * std::cos(xmdf);
        const double delm = s.xmcof * (delmtemp * delmtemp * delmtemp - s.delmo);
        const double temp = delomg + delm;
        mm = xmdf + temp;
        argpm = argpdf - temp;
        const double t3 = t2 * t;
        const double t4 = t3 * t;
        tempa = tempa - s.d2 * t2 - s.d3 * t3 - s.d4 * t4;
        tempe = tempe + s.bstar * s.cc5 * (std::sin(mm) - s.sinmao);
        templ = templ + s.t3cof * t3 + t4 * (s.t4cof + t * s.t5cof);
    }

    double nm = s.noUnkozai;
    double em = s.ecco;
    const double inclm = s.inclo;
    if (nm <= 0.0) return false;
    const double am = std::pow(xke / nm, x2o3) * tempa * tempa;
    nm = xke / std::pow(am, 1.5);
    em = em - tempe;
    if (em >= 1.0 || em < -0.001) return false;
    if (em < 1.0e-6) em = 1.0e-6;
    mm = mm + s.noUnkozai * templ;
    double xlm = mm + argpm + nodem;
    nodem = std::fmod(nodem, TWOPI);
    argpm = std::fmod(argpm, TWOPI);
    xlm = std::fmod(xlm, TWOPI);
    mm = std::fmod(xlm - argpm - nodem, TWOPI);

    const double sinip = std::sin(inclm), cosip = std::cos(inclm);
    const double ep = em, xincp = inclm, argpp = argpm, nodep = nodem, mp = mm;

    // --- long period periodics ---
    const double axnl = ep * std::cos(argpp);
    double temp = 1.0 / (am * (1.0 - ep * ep));
    const double aynl = ep * std::sin(argpp) + temp * s.aycof;
    const double xl = mp + argpp + nodep + temp * s.xlcof * axnl;

    // --- Kepler's equation ---
    const double u = std::fmod(xl - nodep, TWOPI);
    double eo1 = u, tem5 = 9999.9, sineo1 = 0, coseo1 = 0;
    int ktr = 1;
    while (std::fabs(tem5) >= 1.0e-12 && ktr <= 10) {
        sineo1 = std::sin(eo1);
        coseo1 = std::cos(eo1);
        tem5 = 1.0 - coseo1 * axnl - sineo1 * aynl;
        tem5 = (u - aynl * coseo1 + axnl * sineo1 - eo1) / tem5;
        if (std::fabs(tem5) >= 0.95) tem5 = tem5 > 0.0 ? 0.95 : -0.95;
        eo1 += tem5;
        ktr++;
    }

    // --- short period preliminary quantities ---
    const double ecose = axnl * coseo1 + aynl * sineo1;
    const double esine = axnl * sineo1 - aynl * coseo1;
    const double el2 = axnl * axnl + aynl * aynl;
    const double pl = am * (1.0 - el2);
    if (pl < 0.0) return false;
    const double rl = am * (1.0 - ecose);
    const double rdotl = std::sqrt(am) * esine / rl;
    const double rvdotl = std::sqrt(pl) / rl;
    const double betal = std::sqrt(1.0 - el2);
    temp = esine / (1.0 + betal);
    const double sinu = am / rl * (sineo1 - aynl - axnl * temp);
    const double cosu = am / rl * (coseo1 - axnl + aynl * temp);
    double su = std::atan2(sinu, cosu);
    const double sin2u = (cosu + cosu) * sinu;
    const double cos2u = 1.0 - 2.0 * sinu * sinu;
    temp = 1.0 / pl;
    const double temp1 = 0.5 * SGP_J2 * temp;
    const double temp2 = temp1 * temp;

    // --- update for short period periodics ---
    const double mrt = rl * (1.0 - 1.5 * temp2 * betal * s.con41) + 0.5 * temp1 * s.x1mth2 * cos2u;
    su = su - 0.25 * temp2 * s.x7thm1 * sin2u;
    const double xnode = nodep + 1.5 * temp2 * cosip * sin2u;
    const double xinc = xincp + 1.5 * temp2 * cosip * sinip * cos2u;
    const double mvt = rdotl - nm * temp1 * s.x1mth2 * sin2u / xke;
    const double rvdot = rvdotl + nm * temp1 * (s.x1mth2 * cos2u + 1.5 * s.con41) / xke;

    // --- orientation vectors ---
    const double sinsu = std::sin(su), cossu = std::cos(su);
    const double snod = std::sin(xnode), cnod = std::cos(xnode);
    const double sini = std::sin(xinc), cosi = std::cos(xinc);
    const double xmx = -snod * cosi, xmy = cnod * cosi;
    const double ux = xmx * sinsu + cnod * cossu;
    const double uy = xmy * sinsu + snod * cossu;
    const double uz = sini * sinsu;
    const double vx = xmx * cossu - cnod * sinsu;
    const double vy = xmy * cossu - snod * sinsu;
    const double vz = sini * cossu;

    const double mr = mrt * SGP_RE;
    const double vkmps = SGP_RE * xke / 60.0;
    r = Vec3(mr * ux, mr * uy, mr * uz);
    v = Vec3((mvt * ux + rvdot * vx) * vkmps, (mvt * uy + rvdot * vy) * vkmps, (mvt * uz + rvdot * vz) * vkmps);
    return mrt >= 1.0;
}

inline bool propagateJd(const Satellite& s, double jd, Vec3& r, Vec3& v) {
    return sgp4(s, (jd - s.epochJd) * 1440.0, r, v);
}

} // namespace orbit
