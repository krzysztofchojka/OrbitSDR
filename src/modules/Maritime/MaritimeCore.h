#pragma once

// =====================================================================
//  Maritime safety signals: NAVTEX (SITOR-B) and DSC (ITU-R M.493).
//
//  Demodulation and decoding follow SDRangel's NAVTEX and DSC decoders
//  by Jon Beniston, M7RCE (GNU GPL v3, the same as OrbitSDR); written
//  again here without Qt. References: ITU-R M.476 / M.625 (SITOR),
//  M.540 (NAVTEX), M.493 (DSC).
//
//   NAVTEX: 518 / 490 / 4209.5 kHz, FSK 100 Bd, +-85 Hz around the
//           frequency (the higher tone = 1), CCIR 476 7-bit characters
//           (4 ones each), every character sent twice (DX / RX).
//   DSC MF/HF: the same FSK (the lower tone = 1); 10-bit symbols:
//           7 information bits LSB first + the number of zeros (3 bits).
//   DSC VHF: channel 70 (156.525 MHz), FM with AFSK 1300 (= 1) /
//           2100 Hz, 1200 Bd.
// =====================================================================

#include <vector>
#include <deque>
#include <string>
#include <complex>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <array>
#include <algorithm>
#include <functional>

namespace maritime {

inline int popcount7(unsigned v) { int n = 0; for (int i = 0; i < 7; i++) n += (v >> i) & 1; return n; }

// ---------------------------------------------------------------------
//  Bit clock: follows the data transitions, samples in mid-bit
// ---------------------------------------------------------------------
class BitClock {
    double spb = 10.0, count = -5.0;
    bool prev = false;

public:
    bool locked = false;   // set by the user of the bits once a frame start was found (lower gain)
    void configure(double samplesPerBit) { spb = samplesPerBit; reset(); }
    void reset() { count = -spb / 2.0; prev = false; locked = false; }
    // Returns true when a bit is to be taken from this sample
    bool step(bool data) {
        if (data != prev) count -= count * (locked ? 0.05 : 0.2);
        prev = data;
        count += 1.0;
        if (count >= spb / 2.0 - 1.0) { count -= spb; return true; }
        return false;
    }
};

// ---------------------------------------------------------------------
//  FSK +-85 Hz, 100 Bd, input 1000 S/s complex centred on the frequency.
//  soft > 0: the higher tone (NAVTEX "1"); DSC MF/HF uses the opposite.
// ---------------------------------------------------------------------
class Fsk170 {
    static constexpr int FS = 1000, SPB = 10;
    std::complex<double> nco = {1.0, 0.0}, step;
    std::array<std::complex<double>, SPB> c1{}, c2{};
    int ci = 0;
    std::deque<double> e1, e2;   // envelopes (moving maximum over 8 bits)
    BitClock clock;

public:
    std::function<void(double soft)> onBit;
    double level = 0.0;

    Fsk170() { step = std::polar(1.0, 2.0 * 3.14159265358979 * 85.0 / FS); clock.configure(SPB); }
    void reset() { clock.reset(); e1.clear(); e2.clear(); c1.fill({}); c2.fill({}); }
    void setLocked(bool l) { clock.locked = l; }

    void process(const std::complex<float>* x, size_t n) {
        for (size_t k = 0; k < n; k++) {
            const std::complex<double> z(x[k].real(), x[k].imag());
            c1[ci] = z * nco;              // the lower tone (-85 Hz) to 0 Hz
            c2[ci] = z * std::conj(nco);   // the higher tone (+85 Hz) to 0 Hz
            nco *= step;
            if ((k & 255) == 0) nco /= std::abs(nco);
            ci = (ci + 1) % SPB;
            std::complex<double> s1, s2;   // matched filter: one bit
            for (int i = 0; i < SPB; i++) { s1 += c1[i]; s2 += c2[i]; }
            const double a1 = std::abs(s1) / SPB, a2 = std::abs(s2) / SPB;
            level = 0.999 * level + 0.001 * (a1 + a2);
            e1.push_back(a1); e2.push_back(a2);
            if (e1.size() > 8 * SPB) { e1.pop_front(); e2.pop_front(); }
            const double m1 = *std::max_element(e1.begin(), e1.end());
            const double m2 = *std::max_element(e2.begin(), e2.end());
            // automatic threshold correction for frequency selective fading (W7AY)
            const double soft = (a2 - 0.5 * m2) - (a1 - 0.5 * m1);
            if (clock.step(soft > 0.0) && onBit) onBit(soft);
        }
    }
};

// ---------------------------------------------------------------------
//  VHF DSC: FM discriminator audio at 48 kHz, AFSK 1300 / 2100 Hz, 1200 Bd.
//  soft > 0: 1300 Hz ("1").
// ---------------------------------------------------------------------
class Afsk1200 {
    static constexpr int FS = 48000, SPB = 40;
    std::array<double, SPB> buf{};
    std::array<std::complex<double>, SPB> eLow, eHigh;
    int idx = 0, filled = 0;
    double dc = 0.0;
    std::complex<double> prev = {1.0, 0.0};
    BitClock clock;

public:
    std::function<void(double soft)> onBit;
    double level = 0.0;

    Afsk1200() {
        for (int i = 0; i < SPB; i++) {
            eLow[i] = std::polar(1.0, 2.0 * 3.14159265358979 * 1300.0 * i / FS);
            eHigh[i] = std::polar(1.0, 2.0 * 3.14159265358979 * 2100.0 * i / FS);
        }
        clock.configure(SPB);
    }
    void reset() { clock.reset(); buf.fill(0.0); filled = 0; dc = 0.0; }
    void setLocked(bool l) { clock.locked = l; }

    // x: complex baseband of the FM channel at 48 kS/s
    void process(const std::complex<float>* x, size_t n) {
        for (size_t k = 0; k < n; k++) {
            const std::complex<double> z(x[k].real(), x[k].imag());
            level = 0.9995 * level + 0.0005 * std::abs(z);
            const double fm = std::arg(z * std::conj(prev));
            prev = z;
            sample(fm);
        }
    }
    // FM-demodulated audio at 48 kHz (a scanner's audio output)
    void processAudio(const float* a, size_t n) {
        for (size_t k = 0; k < n; k++) { level = 0.9995 * level + 0.0005 * std::fabs(a[k]); sample(a[k]); }
    }

private:
    void sample(double fm) {
        {
            dc += 0.001 * (fm - dc);   // the radio's tuning error
            buf[idx] = fm - dc;
            if (filled < SPB) filled++;
            double lo = 0.0, hi = 0.0;
            if (filled == SPB) {
                std::complex<double> cl, ch;
                for (int i = 0; i < SPB; i++) {
                    int j = idx - i;
                    if (j < 0) j += SPB;
                    cl += eLow[i] * buf[j];
                    ch += eHigh[i] * buf[j];
                }
                // 6 dB/octave pre-emphasis: weigh each tone by its frequency
                lo = std::abs(cl) / 1300.0;
                hi = std::abs(ch) / 2100.0;
            }
            idx = (idx + 1) % SPB;
            const double soft = lo - hi;
            if (clock.step(soft > 0.0) && onBit) onBit(soft);
        }
    }
};

// =====================================================================
//  NAVTEX (SITOR-B)
// =====================================================================

// CCIR 476: 7-bit code -> character (letters / figures); -1 = invalid code.
// 0x0e = figures shift, 0x0f = letters shift, '<' / '>' = phasing 1 / 2.
inline const signed char* ccir476Letters() {
    static const signed char t[128] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,0x0d,
        -1,-1,-1,-1,-1,-1,-1,'T',-1,-1,-1,0x0a,-1,' ','V',-1,
        -1,-1,-1,-1,-1,-1,-1,'B',-1,-1,-1,-1,-1,0x0f,'X',-1,
        -1,-1,-1,'>',-1,'E',0x0e,-1,-1,'U','Q',-1,'K',-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,'O',-1,-1,-1,'H',-1,'N','M',-1,
        -1,-1,-1,'L',-1,'R','G',-1,-1,'I','P',-1,'C',-1,-1,-1,
        -1,-1,-1,'Z',-1,'D',-1,-1,-1,'S','Y',-1,'F',-1,-1,-1,
        -1,'A','W',-1,'J',-1,-1,-1,'<',-1,-1,-1,-1,-1,-1,-1 };
    return t;
}
inline const signed char* ccir476Figures() {
    static const signed char t[128] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,0x0d,
        -1,-1,-1,-1,-1,-1,-1,'5',-1,-1,-1,0x0a,-1,' ','=',-1,
        -1,-1,-1,-1,-1,-1,-1,'?',-1,-1,-1,-1,-1,0x0f,'/',-1,
        -1,-1,-1,'>',-1,'3',0x0e,-1,-1,'7','1',-1,'(',-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,'9',-1,-1,-1,'#',-1,',','.',-1,
        -1,-1,-1,')',-1,'4','&',-1,-1,'8','0',-1,':',-1,-1,-1,
        -1,-1,-1,'+',-1,0x05,-1,-1,-1,'\'','6',-1,'!',-1,-1,-1,
        -1,'-','2',-1,0x07,-1,-1,-1,'<',-1,-1,-1,-1,-1,-1,-1 };
    return t;
}

class SitorB {
    static constexpr signed char PHASING_1 = 0x78, PHASING_2 = 0x33;
    static constexpr int BUF = 3;
    enum class St { PHASING, FILL_DX, FILL_RX, DX, RX } st = St::PHASING;
    signed char buf[BUF] = {0, 0, 0};
    int idx = 0;
    bool figures = false;

    signed char dec(signed char c) const { return c < 0 ? (signed char)-1 : (figures ? ccir476Figures() : ccir476Letters())[(int)c]; }

public:
    int errors = 0;
    void init() { st = St::PHASING; idx = 0; figures = false; errors = 0; }

    // 7-bit code in; returns a character, 0x02 at the end of the text, '*' for an unreadable
    // character or -1 when there is nothing yet
    signed char decode(signed char c) {
        signed char ret = -1;
        switch (st) {
        case St::PHASING:
            if (c != PHASING_1 && c != PHASING_2 && dec(c) != -1) { buf[idx++] = c; st = St::FILL_RX; }
            break;
        case St::FILL_DX:
            buf[idx++] = c;
            if (idx == BUF) { st = St::RX; idx = 0; } else st = St::FILL_RX;
            break;
        case St::FILL_RX:
            if (c != PHASING_1) errors++;
            st = St::FILL_DX;
            break;
        case St::RX: {
            const signed char dx = dec(buf[idx]), rx = dec(c);
            signed char a;
            if (dx == '<' && rx == '<') a = 0x02;   // idle in both: end of the text
            else if (dx != -1) { a = dx; if (dx != rx && !((dx == '<' && rx == '>') || (dx == '>' && rx == '<'))) errors++; }
            else if (rx != -1) { a = rx; errors++; }
            else { a = '*'; errors += 2; }
            if (a == 0x0f) figures = false;
            else if (a == 0x0e) figures = true;
            else ret = a;
            st = St::DX;
            break;
        }
        case St::DX:
            buf[idx] = c;
            idx = (idx + 1) % BUF;
            st = St::RX;
            break;
        }
        return ret;
    }
};

struct NavtexMessage {
    bool valid = false;        // ZCZC B1B2B3B4 ... NNNN found
    char station = '?';        // B1: transmitter
    char type = '?';           // B2: subject
    std::string number;        // B3B4
    std::string text;          // the message (without ZCZC / NNNN)
    int errors = 0;
};

inline const char* navtexTypeName(char t) {
    switch (t) {
    case 'A': return "Navigational warning"; case 'B': return "Meteorological warning"; case 'C': return "Ice report";
    case 'D': return "Search and rescue / piracy"; case 'E': return "Meteorological forecast"; case 'F': return "Pilot service";
    case 'G': return "AIS"; case 'H': return "LORAN"; case 'J': return "SATNAV"; case 'K': return "Other navaids";
    case 'L': return "Navigational warning (extra)"; case 'T': return "Test"; case 'V': case 'W': case 'X': case 'Y': return "Special service";
    case 'Z': return "No message on hand"; default: return "";
    }
}

class NavtexReceiver {
    Fsk170 fsk;
    SitorB sitor;
    unsigned bits = 0;
    int bitCount = 0;
    bool gotSOP = false;
    int errorCount = 0, consecutive = 0;
    std::string buffer;

    void restart() {
        gotSOP = false; bitCount = 0; bits = 0; errorCount = 0; consecutive = 0;
        fsk.setLocked(false);
        buffer.clear();
        if (onLive) onLive("");
    }

    void flushMessage() {
        // the text between ZCZC and NNNN ('*' may stand for any unreadable character)
        const auto is = [&](size_t i, char c) { return buffer[i] == c || buffer[i] == '*'; };
        for (size_t s = 0; s + 4 <= buffer.size(); s++) {
            if (!(is(s, 'Z') && is(s + 1, 'C') && is(s + 2, 'Z') && is(s + 3, 'C'))) continue;
            for (size_t e = s + 4; e + 4 <= buffer.size(); e++) {
                if (!(is(e, 'N') && is(e + 1, 'N') && is(e + 2, 'N') && is(e + 3, 'N'))) continue;
                NavtexMessage m;
                std::string body = buffer.substr(s + 4, e - s - 4);
                size_t p = 0;
                while (p < body.size() && body[p] == ' ') p++;
                if (p + 4 <= body.size() && std::isalpha((unsigned char)body[p]) && std::isalpha((unsigned char)body[p + 1]) &&
                    std::isdigit((unsigned char)body[p + 2]) && std::isdigit((unsigned char)body[p + 3])) {
                    m.station = body[p]; m.type = body[p + 1]; m.number = body.substr(p + 2, 2);
                    body = body.substr(p + 4);
                    m.valid = true;
                }
                // tidy the line ends
                std::string t;
                for (char c : body) { if (c == '\r') continue; t += c; }
                while (!t.empty() && (t.front() == '\n' || t.front() == ' ')) t.erase(t.begin());
                while (!t.empty() && (t.back() == '\n' || t.back() == ' ')) t.pop_back();
                m.text = t;
                m.errors = sitor.errors;
                if (onMessage) onMessage(m);
                buffer.erase(0, e + 4);
                return;
            }
        }
    }

    void bit(double soft) {
        bits = (bits << 1) | (soft > 0.0 ? 1u : 0u);
        bitCount++;
        if (!gotSOP) {
            if (bitCount >= 14) {
                bitCount = 14;
                if ((bits & 0x3fff) == 0x19f8) {   // phasing 2 followed by phasing 1
                    gotSOP = true; bitCount = 0; sitor.init(); fsk.setLocked(true);
                }
            }
            return;
        }
        if (bitCount < 7) return;
        bitCount = 0;
        const signed char c = sitor.decode((signed char)(bits & 0x7f));
        if (c == -1) return;
        if (c == '*') { errorCount++; consecutive++; } else consecutive = 0;
        if (c != '<' && c != '>' && c != 0x02) {
            buffer += (char)c;
            if (buffer.size() > 20000) buffer.erase(0, 10000);
            if (onLive) onLive(buffer);
            flushMessage();
        } else {
            flushMessage();
            if (c == 0x02) { restart(); return; }
        }
        // back to stand-by when it is mostly noise (M.476: after a share of mutilated characters)
        if (c == '*') {
            if (buffer.size() >= 12) {
                if (errorCount / (float)(buffer.size() + errorCount) >= 0.2f) restart();
            } else if (errorCount >= 3) restart();
            if (consecutive >= 5) restart();
        }
    }

public:
    std::function<void(const NavtexMessage&)> onMessage;
    std::function<void(const std::string&)> onLive;   // the text being received (empty = idle)

    NavtexReceiver() { fsk.onBit = [this](double s) { bit(s); }; }
    bool receiving() const { return gotSOP; }
    double level() const { return fsk.level; }
    void reset() { fsk.reset(); restart(); }
    void process(const std::complex<float>* x, size_t n) { fsk.process(x, n); }
};

// =====================================================================
//  DSC (ITU-R M.493)
// =====================================================================
enum DscFormat { DSC_GEOGRAPHIC = 102, DSC_DISTRESS = 112, DSC_GROUP = 114, DSC_ALL_SHIPS = 116, DSC_SELECTIVE = 120, DSC_AUTOMATIC = 123 };
enum DscCategory { DSC_ROUTINE = 100, DSC_SAFETY = 108, DSC_URGENCY = 110, DSC_CAT_DISTRESS = 112 };

inline const char* dscFormatName(int f) {
    switch (f) {
    case 102: return "Geographic area call"; case 112: return "Distress alert"; case 114: return "Group call";
    case 116: return "All ships"; case 120: return "Selective call"; case 123: return "Automatic call"; default: return nullptr;
    }
}
inline const char* dscCategoryName(int c) {
    switch (c) { case 100: return "Routine"; case 108: return "Safety"; case 110: return "Urgency"; case 112: return "Distress"; default: return nullptr; }
}
inline const char* dscTelecommand1Name(int t) {
    switch (t) {
    case 100: return "FM telephony (all modes)"; case 101: return "FM telephony, duplex"; case 103: return "Polling";
    case 104: return "Unable to comply"; case 105: return "End of call"; case 106: return "Data"; case 109: return "SSB telephony";
    case 110: return "Distress acknowledgement"; case 112: return "Distress alert relay"; case 113: return "TTY FEC";
    case 115: return "TTY ARQ"; case 118: return "Test"; case 121: return "Position update"; case 126: return "No information";
    default: return nullptr;
    }
}
inline const char* dscTelecommand2Name(int t) {
    switch (t) {
    case 100: return "No reason given"; case 101: return "Congestion"; case 102: return "Busy"; case 103: return "Queue indication";
    case 104: return "Station barred"; case 105: return "No operator"; case 106: return "Operator unavailable"; case 107: return "Equipment disabled";
    case 108: return "Unable to use proposed channel"; case 109: return "Unable to use proposed mode"; case 110: return "Not party to a conflict";
    case 111: return "Medical transport"; case 112: return "Pay-phone"; case 113: return "Facsimile"; case 126: return "No information";
    default: return nullptr;
    }
}
inline const char* dscDistressName(int n) {
    switch (n) {
    case 100: return "Fire, explosion"; case 101: return "Flooding"; case 102: return "Collision"; case 103: return "Grounding";
    case 104: return "Listing, capsizing"; case 105: return "Sinking"; case 106: return "Disabled and adrift"; case 107: return "Undesignated";
    case 108: return "Abandoning ship"; case 109: return "Piracy / armed attack"; case 110: return "Man overboard"; case 112: return "EPIRB emission";
    default: return nullptr;
    }
}
inline const char* dscEosName(int e) { switch (e) { case 117: return "Ack requested"; case 122: return "Acknowledgement"; case 127: return "End of sequence"; default: return nullptr; } }

struct DscMessage {
    bool valid = false;
    int format = -1, category = -1, tc1 = -1, tc2 = -1, nature = -1, subsequent = -1, eos = -1;
    std::string address;       // MMSI of the called station, or the area / group
    std::string selfId;        // MMSI of the caller
    std::string distressId;    // the vessel in distress (relays, acknowledgements)
    std::string position;      // "54°21'N 018°40'E"
    bool hasPos = false;
    double lat = 0, lon = 0;
    std::string freq1, freq2;  // "156.800 MHz" / "CH16"
    std::string number;        // automatic call: telephone number
    std::string time;          // UTC "HH:MM"
    int errors = 0;
    std::vector<int> symbols;
};

inline std::string dscDigits(const std::vector<int>& d, size_t idx, size_t n) {
    std::string s;
    for (size_t i = 0; i < n && idx + i < d.size(); i++) {
        char b[4];
        std::snprintf(b, sizeof(b), "%02d", std::clamp(d[idx + i], 0, 99));
        s += b;
    }
    return s;
}
inline std::string dscMmsi(const std::string& a) {   // 10 digits: MMSI + 0 (or an equipment digit)
    if (a.size() < 10) return a;
    return a.back() == '0' ? a.substr(0, 9) : a.substr(0, 9) + "-" + a.substr(9, 1);
}

// Position: quadrant, lat deg (2), min (2), lon deg (3), min (2)
inline bool dscPosition(const std::string& c, DscMessage& m) {
    if (c.size() < 10 || c == "9999999999") { m.position = "not available"; return false; }
    const int q = c[0] - '0';
    const int la = std::atoi(c.substr(1, 2).c_str()), lam = std::atoi(c.substr(3, 2).c_str());
    const int lo = std::atoi(c.substr(5, 3).c_str()), lom = std::atoi(c.substr(8, 2).c_str());
    if (q < 0 || q > 3 || la > 90 || lam > 59 || lo > 180 || lom > 59) { m.position = c; return false; }
    m.lat = (la + lam / 60.0) * ((q == 2 || q == 3) ? -1 : 1);
    m.lon = (lo + lom / 60.0) * ((q == 1 || q == 3) ? -1 : 1);
    char b[64];
    std::snprintf(b, sizeof(b), "%02d\xC2\xB0%02d'%c %03d\xC2\xB0%02d'%c", la, lam, (q == 2 || q == 3) ? 'S' : 'N', lo, lom, (q == 1 || q == 3) ? 'W' : 'E');
    m.position = b;
    m.hasPos = true;
    return true;
}

inline void dscFrequency(const std::vector<int>& d, size_t& idx, std::string& out) {
    if (idx + 3 > d.size()) return;
    if (d[idx] == 126 && d[idx + 1] == 126 && d[idx + 2] == 126) { idx += 3; return; }
    std::string s = dscDigits(d, idx, 3);
    idx += 3;
    if (s[0] == '4') { s += dscDigits(d, idx, 1); idx++; }
    char b[48];
    if (s[0] == '0' || s[0] == '1' || s[0] == '2') { std::snprintf(b, sizeof(b), "%.1f kHz", std::atoi(s.c_str()) * 0.1); out = b; }
    else if (s[0] == '3') out = "MF/HF CH" + s.substr(1);
    else if (s[0] == '4') { std::snprintf(b, sizeof(b), "%.2f kHz", std::atoi(s.substr(1).c_str()) * 0.01); out = b; }
    else if (s[0] == '9') { std::string ch = s.substr(2); while (ch.size() > 1 && ch[0] == '0') ch.erase(0, 1); out = "VHF CH" + ch; }
}

inline DscMessage parseDsc(const std::vector<int>& d) {
    DscMessage m;
    m.symbols = d;
    if (d.size() < 8) return m;
    for (int v : d) if (v < 0) return m;   // an unreadable symbol
    size_t idx = 0;
    m.format = d[idx++];
    const bool fmtMatch = d[idx++] == m.format;
    bool hasCat = false, hasTc1 = false, hasTc2 = false, hasNature = false;
    auto need = [&](size_t n) { return idx + n <= d.size(); };
    if (m.format != DSC_DISTRESS) {
        if (m.format != DSC_ALL_SHIPS) {
            if (!need(5)) return m;
            const std::string a = dscDigits(d, idx, 5);
            idx += 5;
            if (m.format == DSC_GEOGRAPHIC && a.size() == 10) {
                // NW corner + size: quadrant, lat (2), lon (3), dlat (2), dlon (2)
                const char q = a[0];
                const char* ns = (q == '2' || q == '3') ? "S" : "N";
                const char* ew = (q == '1' || q == '3') ? "W" : "E";
                m.address = "area " + a.substr(1, 2) + ns + " " + a.substr(3, 3) + ew + ", " + a.substr(6, 2) + "x" + a.substr(8, 2) + " deg";
            } else m.address = dscMmsi(a);
        }
        if (!need(1)) return m;
        m.category = d[idx++];
        hasCat = true;
    } else {
        m.category = DSC_CAT_DISTRESS;
    }
    if (!need(5)) return m;
    m.selfId = dscMmsi(dscDigits(d, idx, 5));
    idx += 5;
    if (m.format != DSC_DISTRESS) {
        if (!need(1)) return m;
        m.tc1 = d[idx++]; hasTc1 = true;
        if (m.category != DSC_CAT_DISTRESS) { if (!need(1)) return m; m.tc2 = d[idx++]; hasTc2 = true; }
    }
    if (hasCat && m.category == DSC_CAT_DISTRESS) {
        if (!need(5)) return m;
        m.distressId = dscMmsi(dscDigits(d, idx, 5));
        idx += 5;
    }
    if (m.format == DSC_DISTRESS || (hasCat && m.category == DSC_CAT_DISTRESS && (m.tc1 == 110 || m.tc1 == 112))) {
        if (!need(6)) return m;
        m.nature = d[idx++]; hasNature = true;
        dscPosition(dscDigits(d, idx, 5), m);
        idx += 5;
    } else if (hasCat) {
        if (!need(1)) return m;
        if (d[idx] == 55) {   // position follows
            if (!need(6)) return m;
            idx++;
            dscPosition(dscDigits(d, idx, 5), m);
            idx += 5;
        } else {
            dscFrequency(d, idx, m.freq1);
            if (m.format != DSC_AUTOMATIC) dscFrequency(d, idx, m.freq2);
        }
    }
    if (m.format == DSC_AUTOMATIC && need(3)) {
        const int oddEven = d[idx++];
        const size_t len = d.size() - idx - 2;
        m.number = dscDigits(d, idx, len);
        idx += len;
        if (oddEven == 105 && !m.number.empty()) m.number.erase(0, 1);
    }
    if (m.format == DSC_DISTRESS || (hasCat && m.category == DSC_CAT_DISTRESS)) {
        if (need(2)) {
            const std::string t = dscDigits(d, idx, 2);
            idx += 2;
            if (t != "8888") m.time = t.substr(0, 2) + ":" + t.substr(2, 2);
        }
        if (need(1)) m.subsequent = d[idx++];
    }
    if (!need(2)) return m;
    m.eos = d[idx++];
    const int ecc = d[idx++];
    int calc = 0;
    for (size_t i = 1; i + 1 < d.size(); i++) calc ^= d[i];   // one format specifier, up to the EOS
    m.valid = fmtMatch && dscFormatName(m.format) && (!hasCat || dscCategoryName(m.category)) &&
              (!hasTc1 || dscTelecommand1Name(m.tc1)) && (!hasTc2 || dscTelecommand2Name(m.tc2)) &&
              (!hasNature || dscDistressName(m.nature)) && dscEosName(m.eos) && calc == ecc;
    return m;
}

// Time diversity + soft decisions (SDRangel's DSCDecoder)
class DscDecoder {
public:
    using Soft = std::array<float, 10>;

private:
    static constexpr int BUF = 3, MAX_BYTES = 40;
    enum class St { PHASING, FILL_DX, FILL_RX, DX, RX, DX_EOS, RX_EOS, DONE } st = St::PHASING;
    int buf[BUF] = {0, 0, 0};
    Soft softBuf[BUF];
    Soft cur{};
    int idx = 0, phaseIdx = 0;
    std::vector<int> bytes, hard;
    std::vector<float> margins;

    static int toSymbol(unsigned bits) {
        // 7 information bits LSB first (bits 9..3), check = number of zeros (bits 2..0)
        int data = 0;
        for (int i = 0; i < 7; i++) data |= ((bits >> (9 - i)) & 1) << i;
        return (7 - popcount7(data)) == (int)(bits & 7) ? data : -1;
    }
    static float metric(int s, const Soft& soft) {
        const int zeros = 7 - popcount7(s);
        float m = 0;
        for (int i = 0; i < 7; i++) m += ((s >> i) & 1) ? soft[i] : -soft[i];
        for (int i = 0; i < 3; i++) m += ((zeros >> (2 - i)) & 1) ? soft[7 + i] : -soft[7 + i];
        return m;
    }
    static int best(const Soft& soft, float& bm, float& nm) {
        int b = 0; bm = nm = -1e30f;
        for (int s = 0; s < 128; s++) {
            const float m = metric(s, soft);
            if (m > bm) { nm = bm; bm = m; b = s; } else if (m > nm) nm = m;
        }
        return b;
    }
    int select(int dx, int rx, const Soft& dxs, const Soft& rxs) {
        int s;
        if (dx != -1) { s = dx; if (dx != rx) errors++; }
        else if (rx != -1) { s = rx; errors++; }
        else { s = -1; errors += 2; }
        hard.push_back(s);
        Soft comb;
        float scale = 0;
        for (int i = 0; i < 10; i++) { comb[i] = dxs[i] + rxs[i]; scale += std::fabs(comb[i]); }
        float bm, nm;
        const int b = best(comb, bm, nm);
        if (dx == -1 || dx != rx) s = b;
        float margin = scale <= 0 ? 0.0f : (s == b ? (bm - nm) / scale : (metric(s, comb) - bm) / scale);
        bytes.push_back(s);
        margins.push_back(margin);
        return s;
    }
    void eraseWeak() {
        int weak = 0;
        for (float m : margins) weak += m < 0.1f;
        if (weak > 1) for (size_t i = 0; i < bytes.size(); i++) if (margins[i] < 0.1f) bytes[i] = -1;
    }

public:
    int errors = 0;

    // offset: phasing symbols still to come (0 = the message follows)
    void init(int offset) {
        if (offset == 0) st = St::FILL_DX; else { phaseIdx = offset; st = St::PHASING; }
        idx = 0; errors = 0; bytes.clear(); hard.clear(); margins.clear();
    }
    bool isPhasing() const { return st == St::PHASING; }

    // Returns true when the message is complete
    bool feed(const Soft& soft) {
        static const int expected[] = {110, 125, 109, 125, 108, 125, 107, 125, 106};
        unsigned bits = 0;
        for (int i = 0; i < 10; i++) bits = (bits << 1) | (soft[i] > 0.0f ? 1u : 0u);
        const int symbol = toSymbol(bits);
        cur = soft;
        switch (st) {
        case St::PHASING:
            if (symbol != expected[9 - phaseIdx]) errors++;
            if (--phaseIdx == 0) st = St::FILL_DX;
            return false;
        case St::FILL_DX:
            softBuf[idx] = cur; buf[idx++] = symbol;
            if (idx == BUF) { st = St::RX; idx = 0; } else st = St::FILL_RX;
            return false;
        case St::FILL_RX:
            if ((idx == 1 && symbol != 105) || (idx == 2 && symbol != 104)) errors++;
            st = St::FILL_DX;
            return false;
        case St::RX: {
            const int a = select(buf[idx], symbol, softBuf[idx], cur);
            st = dscEosName(a) ? St::DX_EOS : St::DX;
            if ((int)bytes.size() > MAX_BYTES) { st = St::DONE; eraseWeak(); return true; }
            return false;
        }
        case St::DX:
            softBuf[idx] = cur; buf[idx] = symbol; idx = (idx + 1) % BUF;
            st = St::RX;
            return false;
        case St::DX_EOS:
            softBuf[idx] = cur; buf[idx] = symbol; idx = (idx + 1) % BUF;
            st = St::RX_EOS;
            return false;
        case St::RX_EOS:
            select(buf[idx], symbol, softBuf[idx], cur);
            st = St::DONE;
            eraseWeak();
            return true;
        case St::DONE:
            return false;
        }
        return false;
    }

    // The hard-decision message when it is valid, otherwise the soft one
    std::vector<int> message() const {
        if (parseDsc(hard).valid || !parseDsc(bytes).valid) return hard;
        return bytes;
    }
};

class DscReceiver {
    Fsk170 fsk;      // MF/HF
    Afsk1200 afsk;   // VHF
    bool vhf = false;
    DscDecoder decoder;
    unsigned bits = 0;
    int bitCount = 0, phasingErrors = 0;
    bool gotSOP = false;
    DscDecoder::Soft softBits{};

    struct Pattern { unsigned bits; int offset; };
    // 10-bit code word of a symbol as received (first bit = MSB): 7 information bits LSB
    // first, then the number of zeros MSB first
    static unsigned codeword(int s) {
        unsigned w = 0;
        for (int i = 0; i < 7; i++) w = (w << 1) | ((s >> i) & 1);
        const int zeros = 7 - popcount7(s);
        return (w << 3) | (unsigned)zeros;
    }
    // Three successive phasing symbols (DX 125 / RX 111..106) and how many follow them
    static const std::vector<Pattern>& patterns() {
        static const std::vector<Pattern> p = [] {
            const int t[10][4] = { {125, 111, 125, 9}, {111, 125, 110, 8}, {125, 110, 125, 7}, {110, 125, 109, 6},
                                   {125, 109, 125, 5}, {109, 125, 108, 4}, {125, 108, 125, 3}, {108, 125, 107, 2},
                                   {125, 107, 125, 1}, {107, 125, 106, 0} };
            std::vector<Pattern> v;
            for (const auto& x : t) v.push_back({(codeword(x[0]) << 20) | (codeword(x[1]) << 10) | codeword(x[2]), x[3]});
            return v;
        }();
        return p;
    }
    static int popcount(unsigned v) { int n = 0; while (v) { n += v & 1; v >>= 1; } return n; }

    void setLocked(bool l) { fsk.setLocked(l); afsk.setLocked(l); }
    void restart() { gotSOP = false; bitCount = 0; bits = 0; phasingErrors = 0; setLocked(false); }

    void bit(double soft) {
        // MF/HF: the lower tone is "1", Fsk170 reports the higher one as positive
        const float s = (float)(vhf ? soft : -soft);
        bits = (bits << 1) | (s > 0.0f ? 1u : 0u);
        if (gotSOP && bitCount < 10) softBits[bitCount] = s;
        bitCount++;
        // a phasing sequence inside a message: it was no real call, start again on the new one
        if (gotSOP && (!decoder.isPhasing() || phasingErrors > 0)) {
            const unsigned pat = bits & 0x3fffffffu;
            for (const auto& p : patterns())
                if (pat == p.bits) { decoder.init(p.offset); phasingErrors = 0; bitCount = 0; return; }
        }
        if (!gotSOP) {
            if (bitCount == 30) {
                bitCount--;
                const unsigned pat = bits & 0x3fffffffu;
                int bi = -1, be = 3;   // up to 2 bit errors in the 30 bits
                for (size_t i = 0; i < patterns().size(); i++) {
                    const int e = popcount(pat ^ patterns()[i].bits);
                    if (e < be) { be = e; bi = (int)i; }
                }
                if (bi >= 0) {
                    decoder.init(patterns()[bi].offset);
                    phasingErrors = be;
                    gotSOP = true;
                    bitCount = 0;
                    setLocked(true);
                }
            }
            return;
        }
        if (bitCount == 10) {
            if (decoder.feed(softBits)) {
                DscMessage m = parseDsc(decoder.message());
                m.errors = decoder.errors;
                if (onMessage && (m.valid || reportInvalid)) onMessage(m);
                restart();
                return;
            }
            bitCount = 0;
        }
    }

public:
    std::function<void(const DscMessage&)> onMessage;
    bool reportInvalid = false;

    DscReceiver() {
        fsk.onBit = [this](double s) { bit(s); };
        afsk.onBit = [this](double s) { bit(s); };
    }
    void setVhf(bool v) { if (v != vhf) { vhf = v; reset(); } }
    bool isVhf() const { return vhf; }
    double level() const { return vhf ? afsk.level : fsk.level; }
    bool receiving() const { return gotSOP; }
    void reset() { fsk.reset(); afsk.reset(); restart(); }
    // MF/HF: 1000 S/s centred on the frequency; VHF: 48 kS/s FM channel
    void process(const std::complex<float>* x, size_t n) { if (vhf) afsk.process(x, n); else fsk.process(x, n); }
    // VHF: FM-demodulated audio at 48 kHz
    void processAudio(const float* a, size_t n) { afsk.processAudio(a, n); }
};

// =====================================================================
//  Encoders (test signals)
// =====================================================================

// CCIR 476 code of a character (letters or figures set); -1 = none
inline int ccir476Encode(char c, bool figures) {
    const signed char* t = figures ? ccir476Figures() : ccir476Letters();
    for (int i = 0; i < 128; i++) if (t[i] == c) return i;
    return -1;
}

// NAVTEX bit stream (1 = the higher tone)
inline std::vector<int> navtexBits(const std::string& text) {
    std::vector<int> codes;
    bool figs = false;
    for (char ch : text) {
        char c = (char)std::toupper((unsigned char)ch);
        if (c == '\n') c = 0x0a;
        if (c == '\r') c = 0x0d;
        int l = ccir476Encode(c, false), f = ccir476Encode(c, true);
        if (c == ' ' || c == 0x0a || c == 0x0d) { codes.push_back(l); continue; }
        if (l >= 0 && (!figs || f < 0)) { if (figs) { codes.push_back(0x2D); figs = false; } codes.push_back(l); }
        else if (f >= 0) { if (!figs) { codes.push_back(0x36); figs = true; } codes.push_back(f); }
    }
    std::vector<int> out;
    auto put = [&](int code) { for (int i = 6; i >= 0; i--) out.push_back((code >> i) & 1); };
    for (int i = 0; i < 20; i++) { put(0x33); put(0x78); }   // phasing: RX = phasing 2, DX = phasing 1
    // DX / RX: the RX copy of a character comes 2 DX characters later (phasing 1 until then)
    const size_t n = codes.size();
    for (size_t k = 0; k < n + 2; k++) {
        put(k < n ? codes[k] : 0x78);
        put(k >= 2 ? codes[k - 2] : 0x78);
    }
    for (int i = 0; i < 8; i++) put(0x78);   // idle: end of the text
    return out;
}

// DSC bit stream (1 = "Y": the lower tone / 1300 Hz). `msg`: format .. EOS (no ECC)
inline std::vector<int> dscBits(std::vector<int> msg, int dotBits) {
    int ecc = 0;
    for (size_t i = 1; i < msg.size(); i++) ecc ^= msg[i];
    msg.push_back(ecc);
    const int eos = msg[msg.size() - 2];
    // DX: the message (+ ECC), then the EOS twice more
    std::vector<int> dx = msg;
    dx.push_back(eos); dx.push_back(eos);
    std::vector<int> out;
    auto put = [&](int s) {
        for (int i = 0; i < 7; i++) out.push_back((s >> i) & 1);
        const int zeros = 7 - popcount7(s);
        for (int i = 2; i >= 0; i--) out.push_back((zeros >> i) & 1);
    };
    for (int i = 0; i < dotBits; i++) out.push_back(i & 1 ? 0 : 1);
    static const int rxPh[] = {111, 110, 109, 108, 107, 106};
    for (int i = 0; i < 6; i++) { put(125); put(rxPh[i]); }
    const int rxFill[] = {105, 104};
    for (size_t k = 0; k < dx.size(); k++) {
        put(dx[k]);
        if (k < 2) put(rxFill[k]); else if (k - 2 < msg.size()) put(msg[k - 2]); else put(eos);
    }
    return out;
}

} // namespace maritime
