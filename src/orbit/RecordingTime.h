#pragma once

// =====================================================================
//  When was a recording made? Used to track satellites in old baseband
//  files: the map time = recording start + playback position.
//
//  File name patterns (first match wins):
//   - OrbitSDR      rec_20261110_173142_99900048Hz_IQ.wav      local time
//   - HDSDR         HDSDR_20261110_173142Z_137100kHz_RF.wav    'Z' = UTC
//   - SDRuno etc.   SDRuno_20261110_173142_137100000Hz.wav     local time
//   - SDR++         baseband_137100000Hz_17-31-42_10-11-2026.wav  local time
//   - ISO-like      2026-11-10_17-31-42 / 2026-11-10T17:31:42Z
//  Fallback: the file's modification time minus its length (the moment
//  the recording was finished), marked as a guess.
// =====================================================================

#include <string>
#include <regex>
#include <ctime>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <cstring>

namespace orbit {

struct RecordingStart {
    double unix = 0;      // UTC seconds, 0 = unknown
    std::string how;      // shown to the user
    bool guess = false;   // from the file time, not the name
};

// Days since 1970-01-01 for a civil date (proleptic Gregorian)
inline long long daysFromCivil(int y, int m, int d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}

// Civil date/time -> unix seconds; `utc` false = the computer's local time zone
inline double civilToUnix(int y, int mo, int d, int h, int mi, double s, bool utc) {
    if (y < 1970 || y > 2200 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s >= 61) return 0;
    if (utc) return (double)daysFromCivil(y, mo, d) * 86400.0 + h * 3600.0 + mi * 60.0 + s;
    std::tm tm{};
    tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
    tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = (int)s;
    tm.tm_isdst = -1;
    const std::time_t t = std::mktime(&tm);
    return t == (std::time_t)-1 ? 0 : (double)t + (s - (int)s);
}

// "2026-11-10 17:31:42" (seconds optional, '-', '.', '/' or ':' separators, 'T' allowed)
inline double parseDateTime(const std::string& text, bool utc) {
    static const std::regex re(R"(^\s*(\d{4})[-./](\d{1,2})[-./](\d{1,2})[ T_]+(\d{1,2})[:.\-](\d{2})(?:[:.\-](\d{2}(?:\.\d+)?))?\s*(Z|UTC)?\s*$)",
                               std::regex::icase);
    std::smatch m;
    if (!std::regex_match(text, m, re)) return 0;
    const bool z = m[7].matched;
    return civilToUnix(std::stoi(m[1]), std::stoi(m[2]), std::stoi(m[3]), std::stoi(m[4]), std::stoi(m[5]),
                       m[6].matched ? std::stod(m[6]) : 0.0, utc || z);
}

// The 'auxi' chunk of a WAV file (SpectraVue format, written by SDRangel, HDSDR, SDR Console ...)
// starts with a Windows SYSTEMTIME: the start of the recording. 0 = none.
inline double wavAuxiStart(const std::string& path) {
    std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
    if (!f.is_open()) return 0;
    char h[8192];
    f.read(h, sizeof(h));
    const size_t n = (size_t)f.gcount();
    if (n < 64 || std::memcmp(h, "RIFF", 4) != 0) return 0;
    for (size_t i = 12; i + 8 + 16 <= n; i++) {
        if (std::memcmp(h + i, "auxi", 4) != 0) continue;
        const unsigned char* s = (const unsigned char*)h + i + 8;
        auto u16 = [&](int k) { return (int)(s[2 * k] | (s[2 * k + 1] << 8)); };
        // wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds
        return civilToUnix(u16(0), u16(1), u16(3), u16(4), u16(5), u16(6) + u16(7) / 1000.0, true);
    }
    return 0;
}

inline RecordingStart recordingStart(const std::string& path, double durationSec) {
    RecordingStart r;
    if (const double a = wavAuxiStart(path); a > 0) {
        r.unix = a;
        r.how = "from the WAV header (UTC)";
        return r;
    }
    const std::string name = std::filesystem::path(path).filename().string();
    std::smatch m;
    // YYYYMMDD_HHMMSS[Z]  (OrbitSDR, HDSDR, SDRuno, many others)
    static const std::regex compact(R"((?:^|[^0-9])((?:19|20|21)\d{2})(\d{2})(\d{2})[_\-T ]?(\d{2})(\d{2})(\d{2})(Z?))");
    // SDR++: HH-MM-SS_DD-MM-YYYY
    static const std::regex sdrpp(R"((\d{2})-(\d{2})-(\d{2})_(\d{2})-(\d{2})-(\d{4}))");
    // YYYY-MM-DD[_T ]HH-MM-SS[Z]
    static const std::regex iso(R"(((?:19|20|21)\d{2})-(\d{2})-(\d{2})[_T ](\d{2})[-:._](\d{2})[-:._](\d{2})(Z?))");
    if (std::regex_search(name, m, iso) || std::regex_search(name, m, compact)) {
        const bool z = m[7].length() > 0;
        r.unix = civilToUnix(std::stoi(m[1]), std::stoi(m[2]), std::stoi(m[3]), std::stoi(m[4]), std::stoi(m[5]), std::stoi(m[6]), z);
        r.how = z ? "from the file name (UTC)" : "from the file name (local time)";
    } else if (std::regex_search(name, m, sdrpp)) {
        r.unix = civilToUnix(std::stoi(m[6]), std::stoi(m[5]), std::stoi(m[4]), std::stoi(m[1]), std::stoi(m[2]), std::stoi(m[3]), false);
        r.how = "from the file name (SDR++, local time)";
    }
    if (r.unix > 0) return r;

    // the file was last written when the recording stopped
    std::error_code ec;
    const auto ft = std::filesystem::last_write_time(path, ec);
    if (ec) return r;
    using namespace std::chrono;
    const auto sys = time_point_cast<system_clock::duration>(ft - std::filesystem::file_time_type::clock::now() + system_clock::now());
    r.unix = (double)duration_cast<milliseconds>(sys.time_since_epoch()).count() / 1000.0 - durationSec;
    r.how = "guessed from the file time - check it";
    r.guess = true;
    return r;
}

} // namespace orbit
