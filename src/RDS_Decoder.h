#pragma once

#include <vector>
#include <complex>
#include <string>
#include <mutex>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <unordered_map>

// =====================================================================
//  RDS (Radio Data System, IEC 62106) decoder for the WFM multiplex.
//
//  Input: the FM discriminator output (MPX) at the demodulator input rate.
//
//  Signal chain:
//    1. Mix the 57 kHz subcarrier down to 0 Hz (free-running NCO).
//    2. 3rd-order CIC decimation to ~38 kHz. Its nulls sit exactly on the
//       alias bands, so the 19 kHz pilot and the stereo L-R signal can't
//       fold into the RDS band.
//    3. FIR low-pass (~2.4 kHz) and linear resampling to exactly 19 kHz,
//       i.e. 16 samples per RDS bit (1187.5 bit/s).
//    4. Biphase (Manchester) correlator + early-late gate symbol timing.
//    5. Differential decoding by comparing consecutive complex correlator
//       outputs (DBPSK style), so no carrier phase recovery is needed and
//       small frequency offsets don't matter.
//    6. Block synchronisation with the RDS CRC/offset words, correction
//       of short error bursts, and group decoding.
//
//  Decoded: PI, PTY, TP, TA, M/S, PS name (0A/0B), RadioText (2A/2B),
//  clock time (4A).
// =====================================================================

struct RdsInfo {
    bool synced = false;
    bool hasData = false;
    uint16_t pi = 0;
    int pty = -1;
    bool tp = false;
    bool ta = false;
    bool music = true;
    std::string ps;            // UTF-8, up to 8 characters
    std::string radioText;     // UTF-8, up to 64 characters
    std::string clockTime;     // "YYYY-MM-DD HH:MM (UTC+x)" or empty
    double blockErrorRate = 1.0;
    uint64_t groups = 0;
};

// European RDS programme type names
inline const char* rdsPtyName(int pty) {
    static const char* names[32] = {
        "None", "News", "Current Affairs", "Information", "Sport", "Education", "Drama", "Culture",
        "Science", "Varied", "Pop Music", "Rock Music", "Easy Listening", "Light Classical", "Serious Classical", "Other Music",
        "Weather", "Finance", "Children's", "Social Affairs", "Religion", "Phone-In", "Travel", "Leisure",
        "Jazz Music", "Country Music", "National Music", "Oldies Music", "Folk Music", "Documentary", "Alarm Test", "Alarm"
    };
    if (pty < 0 || pty > 31) return "";
    return names[pty];
}

class RDSDecoder {
    using C = std::complex<double>;

    static constexpr double PI_D = 3.14159265358979323846;
    static constexpr double SUBCARRIER_HZ = 57000.0;
    static constexpr double BIT_RATE = 1187.5;
    static constexpr int SPB = 16;                         // samples per bit after resampling
    static constexpr double OUT_RATE = BIT_RATE * SPB;     // 19000 Hz
    static constexpr double CIC_TARGET_RATE = 38000.0;
    static constexpr int FIR_TAPS = 129;

    // --- configuration ---
    double fs = 0.0;
    int cicFactor = 1;
    double cicRate = 0.0;

    // --- 1. NCO ---
    C nco = C(1.0, 0.0);
    C ncoStep = C(1.0, 0.0);
    int ncoNormCounter = 0;

    // --- 2. CIC (3 integrators at the input rate, 3 combs at the output rate) ---
    // Fixed-point with two's complement wrap-around: the integrators may
    // overflow freely, the combs still give the exact result. Unsigned types
    // are used so the wrap-around is well defined.
    static constexpr double CIC_SCALE = 16777216.0;   // 2^24
    struct CicChannel {
        uint64_t i1 = 0, i2 = 0, i3 = 0, c1 = 0, c2 = 0, c3 = 0;
        void integrate(int64_t x) { i1 += (uint64_t)x; i2 += i1; i3 += i2; }
        int64_t comb() {
            uint64_t d1 = i3 - c1; c1 = i3;
            uint64_t d2 = d1 - c2; c2 = d1;
            uint64_t d3 = d2 - c3; c3 = d2;
            return (int64_t)d3;
        }
        void reset() { *this = CicChannel(); }
    };
    CicChannel cicI, cicQ;
    int cicCounter = 0;
    double cicGain = 1.0;

    // --- 3. FIR + resampler ---
    std::vector<double> firTaps;
    std::vector<C> firHist;
    int firPos = 0;
    double resampleStep = 1.0;   // input samples per output sample
    double resamplePos = 0.0;
    C resamplePrev = C(0.0, 0.0);
    bool resampleHavePrev = false;

    // --- 4. Biphase correlator + timing ---
    C histBuf[SPB * 2] = {};      // last 32 resampled samples (ring)
    int histPos = 0;
    std::vector<C> mfHist;         // recent correlator outputs (ring of 8)
    int mfPos = 0;
    double bitPhase = 0.0;
    C prevSymbol = C(0.0, 0.0);
    bool havePrevSymbol = false;
    long long sampleCount = 0;

    // --- 6. Block sync ---
    uint32_t shiftReg = 0;
    long long bitCounter = 0;
    bool synced = false;
    int blockBitCount = 0;
    int expectedBlock = 0;         // 0=A 1=B 2=C/C' 3=D
    struct Hit { long long pos; int idx; };
    std::vector<Hit> hits;
    int badBlockWindow[50] = {};
    int badWindowPos = 0;
    int badWindowSum = 0;
    int blocksInWindow = 0;

    uint16_t groupData[4] = {};
    bool groupValid[4] = {};
    bool groupIsCPrime = false;

    std::unordered_map<uint16_t, uint32_t> burstTable;

    // --- decoded data ---
    uint16_t piCandidate = 0;
    int piCandidateCount = 0;
    uint16_t pi = 0;
    bool hasPi = false;
    int pty = -1;
    bool tp = false, ta = false, music = true;
    char psBuf[8];
    char psShown[8];
    int psMask = 0;
    bool psEverComplete = false;
    char rtBuf[64];
    int rtAB = -1;
    std::string clockTime;
    uint64_t groupCount = 0;

    mutable std::mutex infoMtx;
    RdsInfo info;

    // Offset words A, B, C, C', D
    static constexpr uint16_t OFFSETS[5] = { 0x0FC, 0x198, 0x168, 0x350, 0x1B4 };

    // Remainder of a 26-bit block divided by g(x) = x^10+x^8+x^7+x^5+x^4+x^3+1.
    // For an error-free block this equals the offset word that was added.
    static uint16_t syndrome(uint32_t block) {
        uint32_t reg = 0;
        for (int i = 25; i >= 0; i--) {
            reg = (reg << 1) | ((block >> i) & 1u);
            if (reg & 0x400u) reg ^= 0x5B9u;
        }
        return (uint16_t)(reg & 0x3FFu);
    }

    // Offset index for a syndrome: 0=A 1=B 2=C 3=C' 4=D, -1 = none
    static int offsetIndex(uint16_t s) {
        for (int i = 0; i < 5; i++) if (OFFSETS[i] == s) return i;
        return -1;
    }
    static int blockSlot(int offsetIdx) { return offsetIdx <= 1 ? offsetIdx : (offsetIdx == 4 ? 3 : 2); }

    void buildBurstTable() {
        // Bursts of length 1..2 anywhere in the 26-bit block. Longer bursts
        // could be corrected too, but they increase the risk of miscorrection.
        burstTable.clear();
        const uint32_t patterns[] = { 0x1, 0x3 };
        for (uint32_t p : patterns) {
            for (int shift = 0; shift < 26; shift++) {
                uint32_t e = (p << shift) & 0x3FFFFFFu;
                if ((e >> shift) != p) continue;
                burstTable.emplace(syndrome(e), e);
            }
        }
    }

    void designFir() {
        firTaps.assign(FIR_TAPS, 0.0);
        const double cutoff = 2400.0 / cicRate;   // normalised to the CIC output rate
        const int c = FIR_TAPS / 2;
        double sum = 0.0;
        for (int n = 0; n < FIR_TAPS; n++) {
            int m = n - c;
            double s = (m == 0) ? 2.0 * cutoff : std::sin(2.0 * PI_D * cutoff * m) / (PI_D * m);
            double w = 0.42 - 0.5 * std::cos(2.0 * PI_D * n / (FIR_TAPS - 1))
                            + 0.08 * std::cos(4.0 * PI_D * n / (FIR_TAPS - 1));
            firTaps[n] = s * w;
            sum += firTaps[n];
        }
        for (auto& t : firTaps) t /= sum;
        firHist.assign(FIR_TAPS * 2, C(0.0, 0.0));   // doubled buffer avoids modulo in the inner loop
        firPos = 0;
    }

    static std::string rdsCharToUtf8(uint8_t c) {
        // EBU Latin-based RDS character set (IEC 62106 Annex E)
        static const char* hi[128] = {
            "á","à","é","è","í","ì","ó","ò","ú","ù","Ñ","Ç","Ş","β","¡","Ĳ",
            "â","ä","ê","ë","î","ï","ô","ö","û","ü","ñ","ç","ş","ğ","ı","ĳ",
            "ª","α","©","‰","Ǧ","ě","ň","ő","π","€","£","$","←","↑","→","↓",
            "º","¹","²","³","±","İ","ń","ű","µ","¿","÷","°","¼","½","¾","§",
            "Á","À","É","È","Í","Ì","Ó","Ò","Ú","Ù","Ř","Č","Š","Ž","Đ","Ŀ",
            "Â","Ä","Ê","Ë","Î","Ï","Ô","Ö","Û","Ü","ř","č","š","ž","đ","ŀ",
            "Ã","Å","Æ","Œ","ŷ","Ý","Õ","Ø","Þ","Ŋ","Ŕ","Ć","Ś","Ź","Ŧ","ð",
            "ã","å","æ","œ","ŵ","ý","õ","ø","þ","ŋ","ŕ","ć","ś","ź","ŧ"," "
        };
        if (c >= 0x20 && c < 0x7F) return std::string(1, (char)c);
        if (c >= 0x80) return hi[c - 0x80];
        return " ";
    }

    static std::string bufToUtf8(const char* buf, int len) {
        std::string out;
        for (int i = 0; i < len; i++) out += rdsCharToUtf8((uint8_t)buf[i]);
        return out;
    }

    static std::string trimRight(std::string s) {
        while (!s.empty() && s.back() == ' ') s.pop_back();
        return s;
    }

    void clearStationData() {
        hasPi = false; pi = 0; piCandidate = 0; piCandidateCount = 0;
        pty = -1; tp = ta = false; music = true;
        std::fill(std::begin(psBuf), std::end(psBuf), ' ');
        std::fill(std::begin(psShown), std::end(psShown), ' ');
        psMask = 0; psEverComplete = false;
        std::fill(std::begin(rtBuf), std::end(rtBuf), ' ');
        rtAB = -1;
        clockTime.clear();
        groupCount = 0;
    }

    void publish() {
        std::lock_guard<std::mutex> l(infoMtx);
        info.synced = synced;
        info.hasData = hasPi;
        info.pi = pi;
        info.pty = pty;
        info.tp = tp; info.ta = ta; info.music = music;
        info.ps = psEverComplete ? bufToUtf8(psShown, 8) : bufToUtf8(psBuf, 8);
        int rtLen = 64;
        for (int i = 0; i < 64; i++) if (rtBuf[i] == 0x0D) { rtLen = i; break; }
        info.radioText = trimRight(bufToUtf8(rtBuf, rtLen));
        info.clockTime = clockTime;
        info.blockErrorRate = blocksInWindow > 0 ? (double)badWindowSum / blocksInWindow : 1.0;
        info.groups = groupCount;
    }

    // ---------------- group decoding ----------------
    void handlePi(uint16_t p) {
        if (p == piCandidate) {
            if (piCandidateCount < 10) piCandidateCount++;
        } else {
            piCandidate = p; piCandidateCount = 1;
        }
        if (piCandidateCount >= 2 && (!hasPi || pi != piCandidate)) {
            if (hasPi && pi != piCandidate) {
                // A different station: drop everything decoded so far
                clearStationData();
                piCandidate = p; piCandidateCount = 2;
            }
            pi = piCandidate; hasPi = true;
        }
    }

    void decodeGroup() {
        if (groupValid[0]) handlePi(groupData[0]);
        if (!groupValid[1]) return;

        const uint16_t b = groupData[1];
        const int groupType = (b >> 12) & 0xF;
        const bool versionB = (b >> 11) & 1;
        tp = (b >> 10) & 1;
        pty = (b >> 5) & 0x1F;
        if (versionB && groupValid[2] && groupIsCPrime) handlePi(groupData[2]);
        groupCount++;

        if (groupType == 0) {
            // 0A / 0B: basic tuning and switching information (PS name)
            ta = (b >> 4) & 1;
            music = (b >> 3) & 1;
            if (groupValid[3]) {
                int seg = b & 0x3;
                psBuf[seg * 2]     = (char)(groupData[3] >> 8);
                psBuf[seg * 2 + 1] = (char)(groupData[3] & 0xFF);
                psMask |= (1 << seg);
                if (psMask == 0xF) {
                    std::copy(std::begin(psBuf), std::end(psBuf), std::begin(psShown));
                    psEverComplete = true;
                    psMask = 0;
                }
            }
        } else if (groupType == 2) {
            // 2A / 2B: RadioText
            int ab = (b >> 4) & 1;
            if (rtAB != -1 && ab != rtAB) std::fill(std::begin(rtBuf), std::end(rtBuf), ' ');
            rtAB = ab;
            int seg = b & 0xF;
            if (!versionB) {
                if (groupValid[2]) {
                    rtBuf[seg * 4]     = (char)(groupData[2] >> 8);
                    rtBuf[seg * 4 + 1] = (char)(groupData[2] & 0xFF);
                }
                if (groupValid[3]) {
                    rtBuf[seg * 4 + 2] = (char)(groupData[3] >> 8);
                    rtBuf[seg * 4 + 3] = (char)(groupData[3] & 0xFF);
                }
            } else if (groupValid[3]) {
                rtBuf[seg * 2]     = (char)(groupData[3] >> 8);
                rtBuf[seg * 2 + 1] = (char)(groupData[3] & 0xFF);
            }
        } else if (groupType == 4 && !versionB && groupValid[2] && groupValid[3]) {
            // 4A: clock time and date
            uint32_t mjd = ((uint32_t)(b & 0x3) << 15) | (groupData[2] >> 1);
            int hour = ((groupData[2] & 1) << 4) | (groupData[3] >> 12);
            int minute = (groupData[3] >> 6) & 0x3F;
            int offHalfHours = groupData[3] & 0x1F;
            bool offNeg = (groupData[3] >> 5) & 1;
            if (mjd > 15079 && hour < 24 && minute < 60) {
                // MJD -> calendar date (IEC 62106 Annex G)
                int yp = (int)((mjd - 15078.2) / 365.25);
                int mp = (int)((mjd - 14956.1 - (int)(yp * 365.25)) / 30.6001);
                int day = (int)mjd - 14956 - (int)(yp * 365.25) - (int)(mp * 30.6001);
                int k = (mp == 14 || mp == 15) ? 1 : 0;
                int year = 1900 + yp + k;
                int month = mp - 1 - k * 12;
                // Convert UTC to local time using the transmitted offset
                int localMin = hour * 60 + minute + (offNeg ? -1 : 1) * offHalfHours * 30;
                localMin = ((localMin % 1440) + 1440) % 1440;
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d (UTC%c%d%s)",
                              year, month, day, localMin / 60, localMin % 60,
                              offNeg ? '-' : '+', offHalfHours / 2, (offHalfHours & 1) ? ".5" : "");
                clockTime = buf;
            }
        }
    }

    // ---------------- block layer ----------------
    void recordBlockQuality(bool bad) {
        badWindowSum -= badBlockWindow[badWindowPos];
        badBlockWindow[badWindowPos] = bad ? 1 : 0;
        badWindowSum += badBlockWindow[badWindowPos];
        badWindowPos = (badWindowPos + 1) % 50;
        if (blocksInWindow < 50) blocksInWindow++;
    }

    void loseSync() {
        synced = false;
        hits.clear();
        std::fill(std::begin(badBlockWindow), std::end(badBlockWindow), 0);
        badWindowSum = 0;
        blocksInWindow = 0;
    }

    void processBit(int bit) {
        shiftReg = ((shiftReg << 1) | (uint32_t)bit) & 0x3FFFFFFu;
        bitCounter++;

        if (!synced) {
            int idx = offsetIndex(syndrome(shiftReg));
            if (idx < 0) return;
            int slot = blockSlot(idx);
            for (const Hit& h : hits) {
                long long dist = bitCounter - h.pos;
                if (dist <= 0 || dist % 26 != 0 || dist > 26 * 6) continue;
                if ((h.idx + dist / 26) % 4 == slot) {
                    // Two blocks in the right order at the right distance: locked
                    synced = true;
                    blockBitCount = 0;
                    expectedBlock = (slot + 1) % 4;
                    std::fill(std::begin(groupValid), std::end(groupValid), false);
                    // Keep the block we just found if it starts a group
                    if (slot == 0) { groupData[0] = (uint16_t)(shiftReg >> 10); groupValid[0] = true; }
                    break;
                }
            }
            if (!synced) {
                hits.push_back({bitCounter, slot});
                if (hits.size() > 16) hits.erase(hits.begin());
            }
            return;
        }

        if (++blockBitCount < 26) return;
        blockBitCount = 0;

        uint32_t block = shiftReg;
        uint16_t s = syndrome(block);
        int slot = expectedBlock;
        bool ok = false;
        bool cPrime = false;

        auto matches = [&](uint16_t syn) -> bool {
            if (slot == 2) {
                if (syn == OFFSETS[2]) { cPrime = false; return true; }
                if (syn == OFFSETS[3]) { cPrime = true; return true; }
                return false;
            }
            const int offIdx = (slot == 3) ? 4 : slot;
            return syn == OFFSETS[offIdx];
        };

        if (matches(s)) {
            ok = true;
        } else {
            // Try to correct a short burst: syndrome of the error = s XOR offset
            const int candidates[2] = { slot == 3 ? 4 : slot, slot == 2 ? 3 : -1 };
            for (int ci : candidates) {
                if (ci < 0) continue;
                auto it = burstTable.find((uint16_t)(s ^ OFFSETS[ci]));
                if (it != burstTable.end()) {
                    block ^= it->second;
                    cPrime = (ci == 3);
                    ok = true;
                    break;
                }
            }
        }

        recordBlockQuality(!ok);
        groupData[slot] = (uint16_t)(block >> 10);
        groupValid[slot] = ok;
        if (slot == 2) groupIsCPrime = cPrime;

        if (slot == 3) {
            decodeGroup();
            std::fill(std::begin(groupValid), std::end(groupValid), false);
            publish();
        }
        expectedBlock = (slot + 1) % 4;

        if (badWindowSum > 35) { loseSync(); publish(); }
    }

    // ---------------- symbol layer ----------------
    void processSymbolSample(C x) {
        histBuf[histPos] = x;
        histPos = (histPos + 1) % (SPB * 2);

        // Biphase correlator over the last 16 samples: +first half, -second half
        C first(0.0, 0.0), second(0.0, 0.0);
        for (int k = 0; k < SPB; k++) {
            int idx = (histPos - SPB + k + SPB * 2) % (SPB * 2);
            if (k < SPB / 2) first += histBuf[idx]; else second += histBuf[idx];
        }
        C mf = first - second;
        mfHist[mfPos] = mf;
        mfPos = (mfPos + 1) % (int)mfHist.size();
        sampleCount++;
        if (sampleCount < SPB * 2) return;

        bitPhase += 1.0;
        if (bitPhase < SPB) return;
        bitPhase -= SPB;

        // Evaluate the strobe 2 samples in the past so "late" is available
        const int n = (int)mfHist.size();
        C late   = mfHist[(mfPos - 1 + n) % n];
        C center = mfHist[(mfPos - 3 + n) % n];
        C early  = mfHist[(mfPos - 5 + n) % n];

        // Early-late gate: steer the strobe towards the correlation peak
        double el = std::norm(late) - std::norm(early);
        double norm = std::norm(late) + std::norm(early) + 1e-20;
        double err = el / norm;
        bitPhase -= 0.2 * err;

        if (havePrevSymbol) {
            // Differential decoding: a phase reversal between bits means "1"
            double d = (center * std::conj(prevSymbol)).real();
            processBit(d < 0.0 ? 1 : 0);
        }
        prevSymbol = center;
        havePrevSymbol = true;
    }

public:
    RDSDecoder() {
        mfHist.assign(8, C(0.0, 0.0));
        buildBurstTable();
        clearStationData();
    }

    void setSampleRate(double sampleRate) {
        if (sampleRate == fs) return;
        fs = sampleRate;
        cicFactor = std::max(1, (int)std::lround(fs / CIC_TARGET_RATE));
        cicRate = fs / cicFactor;
        cicGain = 1.0 / (std::pow((double)cicFactor, 3.0) * CIC_SCALE);
        ncoStep = std::polar(1.0, -2.0 * PI_D * SUBCARRIER_HZ / fs);
        resampleStep = cicRate / OUT_RATE;
        designFir();
        reset();
    }

    // Can RDS be decoded at this sample rate at all?
    static bool rateSupported(double sampleRate) { return sampleRate >= 125000.0; }

    // Full reset: DSP state and decoded station data (call when retuning)
    void reset() {
        nco = C(1.0, 0.0); ncoNormCounter = 0;
        cicI.reset(); cicQ.reset();
        cicCounter = 0;
        std::fill(firHist.begin(), firHist.end(), C(0.0, 0.0));
        firPos = 0;
        resamplePos = 0.0; resampleHavePrev = false; resamplePrev = C(0.0, 0.0);
        std::fill(std::begin(histBuf), std::end(histBuf), C(0.0, 0.0));
        histPos = 0;
        std::fill(mfHist.begin(), mfHist.end(), C(0.0, 0.0));
        mfPos = 0; bitPhase = 0.0; havePrevSymbol = false; sampleCount = 0;
        shiftReg = 0; bitCounter = 0; blockBitCount = 0; expectedBlock = 0;
        loseSync();
        clearStationData();
        publish();
    }

    void process(const float* mpx, size_t n) {
        if (fs <= 0.0 || !rateSupported(fs)) return;
        for (size_t i = 0; i < n; i++) {
            // 1. Mix 57 kHz down to baseband
            C x = nco * (double)mpx[i];
            nco *= ncoStep;
            if (++ncoNormCounter >= 4096) { nco /= std::abs(nco); ncoNormCounter = 0; }

            // 2. CIC decimation
            cicI.integrate((int64_t)std::llround(x.real() * CIC_SCALE));
            cicQ.integrate((int64_t)std::llround(x.imag() * CIC_SCALE));
            if (++cicCounter < cicFactor) continue;
            cicCounter = 0;
            C y((double)cicI.comb() * cicGain, (double)cicQ.comb() * cicGain);

            // 3a. FIR low-pass at the CIC output rate
            firHist[firPos] = y;
            firHist[firPos + FIR_TAPS] = y;
            firPos = (firPos + 1) % FIR_TAPS;
            C acc(0.0, 0.0);
            const C* h = &firHist[firPos];
            for (int k = 0; k < FIR_TAPS; k++) acc += h[k] * firTaps[k];

            // 3b. Linear resampling to exactly 16 samples per bit
            if (!resampleHavePrev) { resamplePrev = acc; resampleHavePrev = true; continue; }
            while (resamplePos < 1.0) {
                C out = resamplePrev + (acc - resamplePrev) * resamplePos;
                processSymbolSample(out);
                resamplePos += resampleStep;
            }
            resamplePos -= 1.0;
            resamplePrev = acc;
        }
    }

    RdsInfo getInfo() const {
        std::lock_guard<std::mutex> l(infoMtx);
        return info;
    }
};
