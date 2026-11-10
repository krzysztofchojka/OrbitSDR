#pragma once

#include <vector>
#include <complex>
#include <string>
#include <map>
#include <deque>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <functional>

// =====================================================================
//  AIS decoder core (no UI code, so it can be tested on its own).
//
//  Channel: takes the raw IQ samples, shifts the AIS channel to 0 Hz,
//  filters it (+-9 kHz), resamples to exactly 48 kHz and FM-demodulates it
//  (scaled so that the +-2400 Hz deviation is +-1). This front end is
//  independent of the audio demodulator settings.
//
//  Decoder: AIS is GMSK at 9600 bit/s, so one bit is exactly 5 samples
//  at 48 kHz.
//
//  - The signal is averaged over one bit (matched filter for NRZ data).
//  - A frequency offset shows up as DC. The signal power tells when a
//    burst starts; during the first ~20 bits (the balanced 0101 preamble)
//    the DC is estimated quickly, then it is almost frozen for the rest of
//    the burst, so long runs of equal bits can't pull it. A second set of
//    receivers slices without any DC correction (best for well tuned
//    signals); duplicates are dropped.
//  - Five decoders run in parallel, one for each sampling phase, so no
//    clock recovery loop is needed: the phase that hits the bit centres
//    produces frames with a valid CRC. Duplicates are dropped.
//  - Each decoder does NRZI decoding, HDLC framing (0x7E flags, bit
//    de-stuffing) and the CRC-16 (X.25) check.
//  - AIS sends every byte LSB first; the bits are put back in field
//    order before the message is parsed.
// =====================================================================

namespace ais {

struct Message {
    int type = 0;
    uint32_t mmsi = 0;
    std::vector<uint8_t> bits;     // payload, field order (MSB first)
    std::string summary;           // short human readable text
};

struct Vessel {
    uint32_t mmsi = 0;
    std::string name, callsign, shipType;
    double lat = 91.0, lon = 181.0;   // 91 / 181 = not available
    double sog = -1.0, cog = -1.0;
    int heading = -1;
    int lastType = 0;
    int messages = 0;
    double lastSeen = 0.0;            // seconds of audio processed when last heard
};

inline uint32_t getBits(const std::vector<uint8_t>& b, int start, int len) {
    uint32_t r = 0;
    for (int i = 0; i < len; i++) {
        int idx = start + i;
        r = (r << 1) | ((idx < (int)b.size()) ? b[idx] : 0);
    }
    return r;
}

inline int32_t getSigned(const std::vector<uint8_t>& b, int start, int len) {
    uint32_t v = getBits(b, start, len);
    if (v & (1u << (len - 1))) return (int32_t)(v | (~0u << len));
    return (int32_t)v;
}

inline std::string getText(const std::vector<uint8_t>& b, int start, int len) {
    static const char* table = "@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_ !\"#$%&'()*+,-./0123456789:;<=>?";
    std::string s;
    for (int i = 0; i + 6 <= len; i += 6) {
        if (start + i + 6 > (int)b.size()) break;
        s += table[getBits(b, start + i, 6)];
    }
    // '@' is padding; trailing spaces are padding too
    size_t at = s.find('@');
    if (at != std::string::npos) s.resize(at);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

inline const char* shipTypeName(int t) {
    if (t >= 20 && t <= 29) return "Wing in ground";
    if (t == 30) return "Fishing";
    if (t == 31 || t == 32) return "Towing";
    if (t == 33) return "Dredging";
    if (t == 34) return "Diving ops";
    if (t == 35) return "Military";
    if (t == 36) return "Sailing";
    if (t == 37) return "Pleasure craft";
    if (t >= 40 && t <= 49) return "High speed craft";
    if (t == 50) return "Pilot vessel";
    if (t == 51) return "Search and rescue";
    if (t == 52) return "Tug";
    if (t == 53) return "Port tender";
    if (t == 55) return "Law enforcement";
    if (t == 58) return "Medical transport";
    if (t >= 60 && t <= 69) return "Passenger";
    if (t >= 70 && t <= 79) return "Cargo";
    if (t >= 80 && t <= 89) return "Tanker";
    if (t >= 90 && t <= 99) return "Other";
    return "";
}

// CRC-16 X.25 residue check on a bit-serial frame (bits in transmission order, LSB first)
inline bool checkCrc(const std::vector<uint8_t>& bits) {
    if (bits.size() < 32 || bits.size() % 8 != 0) return false;
    uint16_t crc = 0xFFFF;
    for (uint8_t bit : bits) {
        bool x = ((crc ^ bit) & 1) != 0;
        crc >>= 1;
        if (x) crc ^= 0x8408;
    }
    return crc == 0xF0B8;
}

// One HDLC/NRZI receiver for one sampling phase
class PhaseReceiver {
    bool lastLevel = false;
    uint8_t rawReg = 0;
    bool collecting = false;
    int ones = 0;
    std::vector<uint8_t> frame;
public:
    // Returns true when a frame with a valid CRC ended; 'out' gets the raw frame bits
    bool push(bool level, std::vector<uint8_t>& out, int& crcFailures) {
        const uint8_t bit = (level == lastLevel) ? 1 : 0;   // NRZI: no change = 1
        lastLevel = level;
        rawReg = (uint8_t)((rawReg << 1) | bit);

        if (rawReg == 0x7E) {   // flag
            bool ok = false;
            if (collecting && frame.size() >= 7 + 56) {
                frame.resize(frame.size() - 7);   // the flag's first 7 bits were collected already
                if (frame.size() % 8 == 0 && frame.size() >= 56) {
                    if (checkCrc(frame)) { out = frame; ok = true; }
                    else crcFailures++;
                }
            }
            collecting = true; ones = 0; frame.clear();
            return ok;
        }
        if (!collecting) return false;
        if (bit) {
            if (++ones > 6) { collecting = false; return false; }   // abort sequence / noise
            frame.push_back(1);
        } else {
            if (ones == 5) { ones = 0; return false; }              // stuffed zero
            ones = 0;
            frame.push_back(0);
        }
        if (frame.size() > 1100) collecting = false;
        return false;
    }
};

class Channel {
    using C = std::complex<double>;
    static constexpr double PI_D = 3.14159265358979323846;
    static constexpr int TAPS = 63;
    double fs = 0.0;
    int decim = 1;
    double midRate = 0.0;
    C nco = C(1.0, 0.0), ncoStep = C(1.0, 0.0);
    int ncoNorm = 0;
    C boxSum = C(0.0, 0.0);
    int boxCount = 0;
    std::vector<double> taps;
    std::vector<C> hist;
    int pos = 0;
    double resPos = 0.0, resStep = 1.0;
    C resPrev = C(0.0, 0.0);
    bool havePrev = false;
    C lastOut = C(1.0, 0.0);
    double offsetHz = 0.0;

public:
    static constexpr double OUT_RATE = 48000.0;

    void configure(double sampleRate, double channelOffsetHz) {
        if (sampleRate != fs) {
            fs = sampleRate;
            decim = std::max(1, (int)std::floor(fs / 96000.0));   // boxcar down to >= 96 kHz
            midRate = fs / decim;
            taps.assign(TAPS, 0.0);
            const double fc = 9000.0 / midRate;
            double sum = 0.0;
            for (int n = 0; n < TAPS; n++) {
                int m = n - TAPS / 2;
                double sn = (m == 0) ? 2.0 * fc : std::sin(2.0 * PI_D * fc * m) / (PI_D * m);
                double w = 0.42 - 0.5 * std::cos(2.0 * PI_D * n / (TAPS - 1)) + 0.08 * std::cos(4.0 * PI_D * n / (TAPS - 1));
                taps[n] = sn * w; sum += taps[n];
            }
            for (auto& t : taps) t /= sum;
            hist.assign(TAPS * 2, C(0.0, 0.0));
            pos = 0; boxSum = C(0.0, 0.0); boxCount = 0;
            resStep = midRate / OUT_RATE; resPos = 0.0; havePrev = false;
            offsetHz = 1e18;   // force the NCO update below
        }
        if (channelOffsetHz != offsetHz) {
            offsetHz = channelOffsetHz;
            ncoStep = std::polar(1.0, -2.0 * PI_D * offsetHz / fs);
        }
    }

    // Appends FM discriminator samples at 48 kHz (+-1 = +-2400 Hz) and their signal power
    void process(const std::complex<double>* iq, size_t n, std::vector<float>& disc, std::vector<float>& power) {
        if (fs <= 0.0) return;
        const double scale = OUT_RATE / (2.0 * PI_D * 2400.0);
        for (size_t i = 0; i < n; i++) {
            boxSum += iq[i] * nco;
            nco *= ncoStep;
            if (++ncoNorm >= 4096) { nco /= std::abs(nco); ncoNorm = 0; }
            if (++boxCount < decim) continue;
            C x = boxSum / (double)decim;
            boxSum = C(0.0, 0.0); boxCount = 0;

            hist[pos] = x; hist[pos + TAPS] = x;
            pos = (pos + 1) % TAPS;
            C y(0.0, 0.0);
            const C* h = &hist[pos];
            for (int k = 0; k < TAPS; k++) y += h[k] * taps[k];

            if (!havePrev) { resPrev = y; havePrev = true; continue; }
            while (resPos < 1.0) {
                C z = resPrev + (y - resPrev) * resPos;
                double d = std::arg(z * std::conj(lastOut));
                lastOut = z;
                disc.push_back((float)(d * scale));
                power.push_back((float)std::norm(z));
                resPos += resStep;
            }
            resPos -= 1.0;
            resPrev = y;
        }
    }
};

class Decoder {
public:
    static constexpr double RATE = 48000.0;
    static constexpr int SPB = 5;   // samples per bit at 48 kHz

    std::function<void(const Message&)> onMessage;
    std::map<uint32_t, Vessel> vessels;
    std::map<uint32_t, Vessel>* sharedVessels = nullptr;   // set to merge several channels into one table
    double clockSeconds = -1.0;                              // time stamp for Vessel::lastSeen (default: audio time)
    int framesOk = 0;
    int crcFailures = 0;

    double secondsProcessed() const { return sampleCount / RATE; }

    void reset() {
        for (auto& r : rxPlain) r = PhaseReceiver();
        for (auto& r : rxDc) r = PhaseReceiver();
        std::fill(std::begin(avgRing), std::end(avgRing), 0.0f);
        std::fill(std::begin(powRing), std::end(powRing), 0.0f);
        avgSum = 0.0f; powSum = 0.0f; avgPos = 0;
        noiseFloor = -1.0f; inBurst = false; burstSamples = 0; dc = 0.0f;
        recent.clear();
    }

    // disc: FM discriminator (+-1 = +-2400 Hz), power: signal power per sample (from Channel)
    void process(const float* disc, const float* power, size_t n) {
        std::vector<uint8_t> frameBits;
        for (size_t i = 0; i < n; i++) {
            // One bit moving averages of the signal and of its power
            avgSum += disc[i] - avgRing[avgPos];
            avgRing[avgPos] = disc[i];
            powSum += power[i] - powRing[avgPos];
            powRing[avgPos] = power[i];
            avgPos = (avgPos + 1) % SPB;
            const float x = avgSum / SPB;
            const float p = powSum / SPB;

            // Burst detection: power well above the noise floor
            if (noiseFloor < 0.0f) noiseFloor = p;
            if (!inBurst) {
                noiseFloor += 0.002f * (p - noiseFloor);
                if (p > 6.0f * noiseFloor && p > 1e-12f) { inBurst = true; burstSamples = 0; }
            } else if (p < 2.0f * noiseFloor) {
                inBurst = false;
            }
            if (inBurst) {
                const float alpha = (burstSamples < 20 * SPB) ? 0.05f : 0.002f;
                dc += alpha * (x - dc);
                burstSamples++;
            }

            const int phase = (int)(sampleCount % SPB);
            if (rxPlain[phase].push(x > 0.0f, frameBits, crcFailures)) handleFrame(frameBits);
            if (rxDc[phase].push(x > dc, frameBits, crcFailures)) handleFrame(frameBits);
            sampleCount++;
        }
    }

private:
    PhaseReceiver rxPlain[SPB];    // slicing at 0
    PhaseReceiver rxDc[SPB];       // slicing at the burst DC estimate
    float avgRing[SPB] = {};
    float powRing[SPB] = {};
    float avgSum = 0.0f, powSum = 0.0f;
    int avgPos = 0;
    float noiseFloor = -1.0f;
    bool inBurst = false;
    int burstSamples = 0;
    float dc = 0.0f;
    long long sampleCount = 0;
    std::deque<std::pair<long long, std::vector<uint8_t>>> recent;   // for removing duplicates

    void handleFrame(const std::vector<uint8_t>& raw) {
        // Neighbouring sampling phases usually decode the same frame
        while (!recent.empty() && recent.front().first < sampleCount - (long long)(0.1 * RATE)) recent.pop_front();
        for (const auto& r : recent) if (r.second == raw) return;
        recent.push_back({sampleCount, raw});

        // Drop the CRC and restore the bit order inside each byte (sent LSB first)
        Message m;
        const size_t dataBits = raw.size() - 16;
        m.bits.resize(dataBits);
        for (size_t byte = 0; byte < dataBits / 8; byte++)
            for (int k = 0; k < 8; k++) m.bits[byte * 8 + k] = raw[byte * 8 + (7 - k)];

        m.type = (int)getBits(m.bits, 0, 6);
        m.mmsi = getBits(m.bits, 8, 30);
        framesOk++;
        parse(m);
        if (onMessage) onMessage(m);
    }

    static double coord(int32_t v, double unavailable) {
        double d = v / 600000.0;
        return (std::abs(d - unavailable) < 1e-6) ? unavailable : d;
    }

    void parse(Message& m) {
        const auto& b = m.bits;
        Vessel& v = (sharedVessels ? *sharedVessels : vessels)[m.mmsi];
        v.mmsi = m.mmsi;
        v.lastType = m.type;
        v.messages++;
        v.lastSeen = clockSeconds >= 0.0 ? clockSeconds : secondsProcessed();
        char buf[160];

        auto setPos = [&](int lonBit, int latBit) {
            double lon = coord(getSigned(b, lonBit, 28), 181.0);
            double lat = coord(getSigned(b, latBit, 27), 91.0);
            if (std::abs(lon) <= 180.0 && std::abs(lat) <= 90.0) { v.lon = lon; v.lat = lat; }
        };
        auto setSogCog = [&](int sogBit, int cogBit, int hdgBit) {
            uint32_t sog = getBits(b, sogBit, 10), cog = getBits(b, cogBit, 12), hdg = getBits(b, hdgBit, 9);
            v.sog = (sog == 1023) ? -1.0 : sog / 10.0;
            v.cog = (cog >= 3600) ? -1.0 : cog / 10.0;
            v.heading = (hdg >= 360) ? -1 : (int)hdg;
        };

        switch (m.type) {
        case 1: case 2: case 3:
            if (b.size() < 168) break;
            setSogCog(50, 116, 128);
            setPos(61, 89);
            std::snprintf(buf, sizeof(buf), "Position %.5f, %.5f  SOG %.1f kn", v.lat, v.lon, v.sog);
            m.summary = buf;
            break;
        case 4:
            if (b.size() < 168) break;
            setPos(79, 107);
            v.shipType = "Base station";
            std::snprintf(buf, sizeof(buf), "Base station %.5f, %.5f", v.lat, v.lon);
            m.summary = buf;
            break;
        case 5:
            if (b.size() < 424) break;
            v.callsign = getText(b, 70, 42);
            v.name = getText(b, 112, 120);
            v.shipType = shipTypeName((int)getBits(b, 232, 8));
            m.summary = "Static data: " + v.name + " (" + v.callsign + ") to " + getText(b, 302, 120);
            break;
        case 18:
            if (b.size() < 168) break;
            setSogCog(46, 112, 124);
            setPos(57, 85);
            std::snprintf(buf, sizeof(buf), "Class B position %.5f, %.5f  SOG %.1f kn", v.lat, v.lon, v.sog);
            m.summary = buf;
            break;
        case 19:
            if (b.size() < 312) break;
            setSogCog(46, 112, 124);
            setPos(57, 85);
            v.name = getText(b, 143, 120);
            v.shipType = shipTypeName((int)getBits(b, 263, 8));
            m.summary = "Class B extended: " + v.name;
            break;
        case 21:
            if (b.size() < 272) break;
            v.name = getText(b, 43, 120);
            setPos(164, 192);
            v.shipType = "Aid to navigation";
            m.summary = "Aid to navigation: " + v.name;
            break;
        case 24:
            if (b.size() < 160) break;
            if (getBits(b, 38, 2) == 0) {
                v.name = getText(b, 40, 120);
                m.summary = "Static data A: " + v.name;
            } else {
                v.shipType = shipTypeName((int)getBits(b, 40, 8));
                v.callsign = getText(b, 90, 42);
                m.summary = "Static data B: " + v.callsign;
            }
            break;
        default:
            std::snprintf(buf, sizeof(buf), "Message type %d", m.type);
            m.summary = buf;
            break;
        }
        if (m.summary.empty()) m.summary = "Message type " + std::to_string(m.type) + " (short)";
    }
};

} // namespace ais
