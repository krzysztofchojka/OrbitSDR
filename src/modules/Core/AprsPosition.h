#pragma once

// =====================================================================
//  APRS position parser (APRS 1.0.1 + 1.2): uncompressed, compressed and
//  Mic-E positions, objects and items. Input: "SRC>DEST:payload" as the
//  APRS decoder prints it.
// =====================================================================

#include "PositionBus.h"
#include <string>
#include <cmath>
#include <cctype>

namespace aprspos {

// "DDMM.mmN" / "DDDMM.mmE" (spaces = position ambiguity, read as 0)
inline bool parseAngle(const std::string& s, int degDigits, double& out) {
    if ((int)s.size() < degDigits + 6) return false;
    std::string t = s.substr(0, degDigits + 5);
    for (char& c : t) if (c == ' ') c = '0';
    const char h = s[degDigits + 5];
    if (t[degDigits + 2] != '.') return false;
    for (int i = 0; i < degDigits + 5; i++) if (i != degDigits + 2 && !std::isdigit((unsigned char)t[i])) return false;
    const double deg = std::stod(t.substr(0, degDigits));
    const double min = std::stod(t.substr(degDigits));
    if (min >= 60.0) return false;
    out = deg + min / 60.0;
    if (h == 'S' || h == 'W') out = -out;
    else if (h != 'N' && h != 'E') return false;
    return true;
}

inline double base91(const std::string& s) {
    double v = 0;
    for (char c : s) v = v * 91 + (unsigned char)c - 33;
    return v;
}

// Position at the start of `p` (after the data type / timestamp / name). Sets the comment too.
inline bool parsePosition(const std::string& p, PosReport& r) {
    if (p.size() >= 19 && (std::isdigit((unsigned char)p[0]) || p[0] == ' ')) {
        double lat, lon;
        if (parseAngle(p.substr(0, 8), 2, lat) && parseAngle(p.substr(9, 9), 3, lon)) {
            r.lat = lat; r.lon = lon;
            r.symTable = p[8]; r.symCode = p[18];
            std::string rest = p.substr(19);
            // course/speed extension "CCC/SSS"
            if (rest.size() >= 7 && rest[3] == '/' && std::isdigit((unsigned char)rest[0]) && std::isdigit((unsigned char)rest[4])) {
                const int c = std::atoi(rest.substr(0, 3).c_str()), s = std::atoi(rest.substr(4, 3).c_str());
                if (c > 0 && c <= 360) r.courseDeg = c % 360;
                r.speedKn = s;
                rest = rest.substr(7);
            }
            r.info = rest;
            return true;
        }
    }
    // compressed: T YYYY XXXX $ cs t
    if (p.size() >= 13) {
        const char t = p[0];
        if (t == '/' || t == '\\' || (t >= 'A' && t <= 'Z') || (t >= 'a' && t <= 'j')) {
            for (int i = 1; i < 9; i++) if (p[i] < '!' || p[i] > '{') return false;
            r.lat = 90.0 - base91(p.substr(1, 4)) / 380926.0;
            r.lon = -180.0 + base91(p.substr(5, 4)) / 190463.0;
            r.symTable = t; r.symCode = p[9];
            const char c = p[10], s = p[11], ct = p[12];
            if (c != ' ' && c >= '!' && c <= 'z' && !(((ct - 33) & 0x18) == 0x10)) {   // not altitude
                if (c - 33 <= 89) { r.courseDeg = (c - 33) * 4; r.speedKn = std::pow(1.08, s - 33) - 1.0; }
            }
            r.info = p.substr(13);
            return true;
        }
    }
    return false;
}

inline bool parseMicE(const std::string& dest, const std::string& info, PosReport& r) {
    std::string d = dest.substr(0, dest.find('-'));
    if (d.size() < 6 || info.size() < 9) return false;
    int dig[6];
    for (int i = 0; i < 6; i++) {
        const char c = d[i];
        if (c >= '0' && c <= '9') dig[i] = c - '0';
        else if (c >= 'A' && c <= 'J') dig[i] = c - 'A';
        else if (c >= 'P' && c <= 'Y') dig[i] = c - 'P';
        else if (c == 'K' || c == 'L' || c == 'Z') dig[i] = 0;
        else return false;
    }
    double lat = dig[0] * 10 + dig[1] + (dig[2] * 10 + dig[3] + (dig[4] * 10 + dig[5]) / 100.0) / 60.0;
    const bool north = d[3] >= 'P', lonOff = d[4] >= 'P', west = d[5] >= 'P';
    if (!north) lat = -lat;
    int lonDeg = (unsigned char)info[1] - 28;
    if (lonOff) lonDeg += 100;
    if (lonDeg >= 180 && lonDeg <= 189) lonDeg -= 80;
    else if (lonDeg >= 190 && lonDeg <= 199) lonDeg -= 190;
    int lonMin = (unsigned char)info[2] - 28;
    if (lonMin >= 60) lonMin -= 60;
    const int lonHun = (unsigned char)info[3] - 28;
    double lon = lonDeg + (lonMin + lonHun / 100.0) / 60.0;
    if (west) lon = -lon;
    const int sp = (unsigned char)info[4] - 28, dc = (unsigned char)info[5] - 28, se = (unsigned char)info[6] - 28;
    int speed = sp * 10 + dc / 10;
    if (speed >= 800) speed -= 800;
    int course = (dc % 10) * 100 + se;
    if (course >= 400) course -= 400;
    if (std::fabs(lat) > 90 || std::fabs(lon) > 180) return false;
    r.lat = lat; r.lon = lon;
    r.speedKn = speed;
    if (course > 0 && course <= 360) r.courseDeg = course % 360;
    r.symCode = info[7]; r.symTable = info[8];
    r.info = info.size() > 9 ? info.substr(9) : std::string();
    return true;
}

// "SQ5XYZ-9>APRS:!5214.07N/02100.73E>comment"
inline bool parse(const std::string& line, PosReport& r) {
    const size_t gt = line.find('>'), colon = line.find(':');
    if (gt == std::string::npos || colon == std::string::npos || gt > colon) return false;
    const std::string src = line.substr(0, gt), dest = line.substr(gt + 1, colon - gt - 1);
    std::string body = line.substr(colon + 1);
    if (body.empty()) return false;
    r = PosReport();
    r.kind = PosKind::APRS;
    r.id = src; r.label = src;
    bool ok = false;
    const char dt = body[0];
    switch (dt) {
    case '!': case '=': ok = parsePosition(body.substr(1), r); break;
    case '/': case '@': ok = body.size() > 8 && parsePosition(body.substr(8), r); break;   // 7-char timestamp
    case ';':   // object: 9-char name, '*' live / '_' killed, timestamp
        if (body.size() > 18 && body[10] == '*') {
            std::string name = body.substr(1, 9);
            while (!name.empty() && name.back() == ' ') name.pop_back();
            ok = parsePosition(body.substr(18), r);
            r.id = name; r.label = name + " (" + src + ")";
        }
        break;
    case ')': {  // item: 3-9 char name ended by '!' (live) or '_' (killed)
        const size_t e = body.find_first_of("!_", 1);
        if (e != std::string::npos && e >= 4 && e <= 10 && body[e] == '!') {
            const std::string name = body.substr(1, e - 1);
            ok = parsePosition(body.substr(e + 1), r);
            r.id = name; r.label = name + " (" + src + ")";
        }
        break;
    }
    case '`': case '\'': case 0x1c: case 0x1d:
        ok = parseMicE(dest, body, r);
        break;
    default: break;
    }
    if (!ok) return false;
    // trim the comment for the map
    while (!r.info.empty() && r.info.front() == ' ') r.info.erase(r.info.begin());
    if (r.info.size() > 60) r.info = r.info.substr(0, 57) + "...";
    return true;
}

// A short name for the most common symbols (primary table '/')
inline const char* symbolName(char table, char code) {
    if (table == '/') {
        switch (code) {
        case '>': return "car"; case 'k': return "truck"; case 'v': return "van"; case 'j': return "jeep";
        case '<': return "motorcycle"; case 'b': return "bicycle"; case '[': return "person"; case 'R': return "RV";
        case 'Y': return "boat"; case 's': return "ship"; case '\'': return "aircraft"; case '^': return "aircraft";
        case 'X': return "helicopter"; case 'O': return "balloon"; case '_': return "weather"; case '-': return "house";
        case '#': return "digipeater"; case '&': return "gateway"; case 'r': return "repeater"; case 'a': return "ambulance";
        case 'f': return "fire truck"; case 'u': return "truck"; case 'y': return "house (yagi)"; case 'U': return "bus";
        default: break;
        }
    } else {
        switch (code) {
        case '#': return "digipeater"; case '&': return "gateway"; case '_': return "weather"; case '>': return "car";
        case '^': return "aircraft"; case 'a': return "ARES"; default: break;
        }
    }
    return "";
}

} // namespace aprspos
