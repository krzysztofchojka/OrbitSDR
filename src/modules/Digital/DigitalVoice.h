#pragma once

#include <vector>
#include <string>
#include <mutex>
#include <memory>
#include <deque>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <algorithm>
#include <utility>

#ifdef ENABLE_DSDCC
#include "dsd_decoder.h"
#endif

// =====================================================================
//  Digital voice (DMR, dPMR, D-STAR, YSF / System Fusion, NXDN).
//
//  Input: the FM discriminator at 48 kHz (what an NFM receiver hears
//  without de-emphasis and audio filters). The frames are decoded by
//  DSDcc (deps/dsdcc, GPL v3), the voice by mbevocoder (deps/mbevocoder,
//  GPL v3): the AMBE / IMBE codecs. Builds with ORBITSDR_NO_VOCODER leave it out:
//  everything is decoded and shown, only the speech stays silent.
//
//  "Auto" runs one decoder per symbol rate (4800 Bd: DMR, D-STAR, YSF;
//  2400 Bd: dPMR, NXDN48) until one of them has a confirmed signal, then
//  only that one until the signal has been gone for a while.
//
//  Noise: a sync pattern also matches noise by chance (about once a
//  second). A signal counts only when it is confirmed: speech through the
//  voice gate (DSDcc's DSDMBEDecoder: the frames' FEC error counts),
//  signalling whose CRC checks, or a sync held for seconds. The voice gate
//  also keeps the frames decoded from noise away from the speaker.
//
//  Output: the decoded speech at 48 kHz, time locked to the input (one
//  output sample per input sample), with a buffer that absorbs the
//  codec's 20 ms frames, the TDMA bursts of DMR and the blocks of the
//  decoder thread (it grows when it runs dry in the middle of a call).
//
//  Events: the data a station sends beside its speech (DMR text messages,
//  data headers, CSBK, talker alias, GPS; D-STAR text and position) go
//  to onEvent as log lines.
//
//  Thread safety: process() on one thread (DSP or decoder), status()
//  and the settings from any thread.
// =====================================================================

namespace dv {

enum class Want { Auto, DMR, DPMR, DSTAR, YSF, NXDN };

inline const char* wantName(Want w) {
    switch (w) {
    case Want::Auto: return "Auto";
    case Want::DMR: return "DMR";
    case Want::DPMR: return "dPMR";
    case Want::DSTAR: return "D-STAR";
    case Want::YSF: return "YSF";
    case Want::NXDN: return "NXDN";
    }
    return "?";
}

inline bool available() {
#ifdef ENABLE_DSDCC
    return true;
#else
    return false;
#endif
}

inline bool vocoderAvailable() {
#if defined(ENABLE_DSDCC) && !defined(ORBITSDR_NO_VOCODER)
    return true;
#else
    return false;
#endif
}

// One talk path: a DMR time slot, or the whole channel for the FDMA systems
struct SlotView {
    bool voice = false;          // speech right now
    bool data = false;           // data / control right now
    bool encrypted = false;
    std::string encText;         // "AES-256, key 3" ...
    std::string from, to;        // "3124861", "TG 9"
    std::string alias;           // talker alias / callsign text
    std::string activity;        // "Voice", "Idle", "CSBK: Preamble" ...
    std::string extra;           // "EMERGENCY", "GPS 52.1234 21.0123" ...
    double voiceSec = 0.0;       // length of the current call
};

struct Status {
    bool sync = false;               // frames are being decoded right now
    bool everSync = false;
    std::string proto;               // "DMR", "dPMR", "D-STAR", "YSF", "NXDN48", "NXDN96" ("" = nothing found yet)
    std::string station;             // "Repeater (BS)", "Direct / talkaround (MS)"
    std::string system;              // "Tier II conventional", "Capacity Plus" ...
    std::vector<std::pair<std::string, std::string>> fields;   // further facts, for the card
    int nSlots = 1;
    SlotView slot[2];
    int inLevel = 0;                 // discriminator level seen by the decoder (%)
    int quality = 0;                 // symbol sync quality (0..100)
    double sinceSync = 1e9;          // seconds since the last frame
    bool vocoder = vocoderAvailable();
};

// One finished (or still running) call, for the history list
struct Call {
    std::string time;        // local wall clock "HH:MM:SS"
    std::string proto;
    int slot = 0;            // 1 / 2 for DMR, 0 otherwise
    std::string from, to, alias;
    bool encrypted = false;
    std::string encText;
    std::string extra;
    double seconds = 0.0;
    bool open = true;        // still talking
    long long freqHz = 0;    // set by the owner
    int channelId = 0;       // set by the owner
};

inline const char* dmrFidName(int fid) {
    switch (fid) {
    case 0x00: return "standard (ETSI)";
    case 0x04: return "Flyde Micro";
    case 0x05: return "PROD-EL";
    case 0x06: return "Trident (Connect Plus)";
    case 0x07: return "Radiodata";
    case 0x08: case 0x68: return "Hytera";
    case 0x09: return "Aselsan";
    case 0x0A: return "Kirisun";
    case 0x0B: return "DMR Association";
    case 0x10: return "Motorola";
    case 0x13: case 0x1C: return "EMC";
    case 0x20: return "JVC Kenwood";
    case 0x33: case 0x3C: return "Radio Activity";
    case 0x58: return "Tait";
    case 0x77: return "Vertex Standard";
    default: return nullptr;
    }
}

// The algorithm ids of the DMR privacy header as most radios use them
inline std::string dmrAlgName(int alg) {
    switch (alg) {
    case 0x21: return "ARC4 (Motorola Enhanced Privacy)";
    case 0x22: return "DES";
    case 0x24: return "AES-128";
    case 0x25: return "AES-256";
    case 0x02: return "Hytera Enhanced";
    default: { char b[32]; std::snprintf(b, sizeof(b), "algorithm 0x%02X", alg); return b; }
    }
}

inline std::string dmrCsbkName(int fid, int op) {
    if (fid == 0x00) {
        switch (op) {
        case 0x04: return "Unit to unit voice request";
        case 0x05: return "Unit to unit answer";
        case 0x07: return "Channel timing";
        case 0x19: return "Aloha (Tier III)";
        case 0x1C: return "Ahoy (Tier III)";
        case 0x1E: return "Ack response (Tier III)";
        case 0x28: return "Broadcast (Tier III)";
        case 0x30: return "Private voice grant (Tier III)";
        case 0x31: return "Talkgroup voice grant (Tier III)";
        case 0x38: return "BS downlink activate";
        case 0x3D: return "Preamble";
        }
    }
    if (fid == 0x10 && op == 0x3E) return "Capacity Plus channel status";
    if (fid == 0x10 && op == 0x3B) return "Capacity Plus neighbours";
    if (fid == 0x06) return "Connect Plus control";
    char b[48]; std::snprintf(b, sizeof(b), "opcode 0x%02X%s", op, fid ? " (manufacturer)" : "");
    return b;
}

// --- Buffer of decoded speech (48 kHz), with a pre-buffer ---
// The speech is pushed by the decoder thread a block of I/Q at a time and pulled by the DSP
// thread a block at a time, so the pre-buffer must hold at least a block and the lag of the
// decoder thread. It starts at two of the largest pulls seen plus 80 ms, and grows by 60 ms
// (up to 0.5 s) whenever it runs dry in the middle of a call (speech again within half a second).
class VoiceFifo {
    std::deque<float> q;
    bool playing = false;
    size_t maxPull = 0;
    size_t extra = 0;
    long long dryAt = -1;        // pulled samples when it last ran dry while playing
    long long pulled = 0;
    static constexpr size_t MINBUF = 48000 * 3 / 20;   // 150 ms
    static constexpr size_t MAXBUF = 48000 / 2;
    static constexpr size_t MAXLEN = 48000 * 2;
    size_t prebuf() const { return std::min(MAXBUF, std::max(MINBUF, 2 * maxPull + 48000 * 8 / 100) + extra); }
public:
    void push(const float* s, size_t n) {
        if (!playing && dryAt >= 0 && pulled - dryAt < 48000 / 2 && q.empty()) {
            extra = std::min(MAXBUF, extra + 48000 * 6 / 100);   // a gap in a call: more margin
            dryAt = -1;
        }
        for (size_t i = 0; i < n; i++) q.push_back(s[i]);
        while (q.size() > MAXLEN) q.pop_front();
    }
    // Adds up to n samples to out (zeros when nothing is ready). Returns true while it plays.
    bool pull(float* out, size_t n, float gain) {
        maxPull = std::max(maxPull, n);
        pulled += (long long)n;
        if (!playing && q.size() >= prebuf()) playing = true;
        if (!playing) return false;
        const size_t k = std::min(n, q.size());
        for (size_t i = 0; i < k; i++) { out[i] += q.front() * gain; q.pop_front(); }
        if (q.empty()) { playing = false; dryAt = pulled; }
        return k > 0;
    }
    void clear() { q.clear(); playing = false; dryAt = -1; }
    size_t size() const { return q.size(); }
    size_t prebufSamples() const { return prebuf(); }
};

// --- 8 kHz -> 48 kHz (x6), polyphase windowed sinc, 3.6 kHz ---
class Upsample6 {
    static constexpr int L = 6, TPP = 16, N = L * TPP;
    float h[N];
    float hist[TPP] = {0};
    int pos = 0;
public:
    Upsample6() {
        const double PI_D = 3.14159265358979323846;
        const double fc = 3600.0 / 48000.0;
        double sum = 0.0;
        for (int i = 0; i < N; i++) {
            const double m = i - (N - 1) / 2.0;
            const double s = std::fabs(m) < 1e-9 ? 2.0 * fc : std::sin(2.0 * PI_D * fc * m) / (PI_D * m);
            const double w = 0.42 - 0.5 * std::cos(2.0 * PI_D * i / (N - 1)) + 0.08 * std::cos(4.0 * PI_D * i / (N - 1));
            h[i] = (float)(s * w);
            sum += h[i];
        }
        for (int i = 0; i < N; i++) h[i] = (float)(h[i] * L / sum);
    }
    void reset() { std::fill(hist, hist + TPP, 0.0f); pos = 0; }
    void process(const short* in, int n, std::vector<float>& out) {
        for (int i = 0; i < n; i++) {
            hist[pos] = in[i] / 32768.0f;
            pos = (pos + 1) % TPP;
            for (int p = 0; p < L; p++) {
                float y = 0.0f;
                for (int k = 0; k < TPP; k++) y += hist[(pos - 1 - k + 2 * TPP) % TPP] * h[k * L + p];
                out.push_back(y);
            }
        }
    }
};

inline std::string wallClock() {
    std::time_t t = std::time(nullptr);
    std::tm tm = *std::localtime(&t);
    char b[16];
    std::strftime(b, sizeof(b), "%H:%M:%S", &tm);
    return b;
}

class Core {
public:
    std::function<void(const Call&)> onCall;   // a call started (open), changed or ended (open = false)
    std::function<void(const std::string&)> onEvent;   // data sent beside the speech, as a log line

    Core() { configure(); }

    void setWant(Want w) {
        std::lock_guard<std::mutex> l(mtx);
        if (w == want) return;
        want = w;
        needConfigure = true;
    }
    Want getWant() const { std::lock_guard<std::mutex> l(mtx); return want; }
    // 1: slot 1, 2: slot 2, 3: both (DMR); FDMA systems ignore it
    void setSlotMask(int m) { std::lock_guard<std::mutex> l(mtx); slotMask = m; }
    void setMuteEncrypted(bool b) { std::lock_guard<std::mutex> l(mtx); muteEncrypted = b; }
    void setVolume(float g) { std::lock_guard<std::mutex> f(fifoMtx); pullGain = g; }

    void reset() {
        std::lock_guard<std::mutex> l(mtx);
        needConfigure = true;
    }

    Status status() const {
        std::lock_guard<std::mutex> l(mtx);
        return st;
    }

    // Discriminator samples at 48 kHz, scaled to 16 bit (~5 units per Hz of deviation).
    // voice != nullptr: appends exactly n samples of speech to it (48 kHz mono, time locked to
    // the input: the demodulator). nullptr: the speech waits in the buffer for pull() (the
    // decoder channels: the speaker's thread takes it).
    void process(const short* disc, size_t n, std::vector<float>* voice) {
        std::lock_guard<std::mutex> l(mtx);
        if (needConfigure) configure();
#ifdef ENABLE_DSDCC
        for (size_t i = 0; i < n; i++) {
            const short s = disc[i];
            for (size_t k = 0; k < inst.size(); k++) {
                Inst& in = *inst[k];
                if (locked >= 0 && (int)k != locked) continue;
                in.d.run(s);
                takeAudio(in, k);
            }
            sampleCount++;
            if ((sampleCount % 480) == 0) poll();   // every 10 ms
        }
        {
            std::lock_guard<std::mutex> f(fifoMtx);
            pullMask = st.nSlots > 1 ? slotMask : 3;
        }
#else
        (void)disc;
#endif
        if (voice) {
            const size_t base = voice->size();
            voice->resize(base + n, 0.0f);
            pull(voice->data() + base, n);
        }
    }

    // Adds up to n samples of speech (48 kHz) to out; true while there is speech
    bool pull(float* out, size_t n) {
        std::lock_guard<std::mutex> f(fifoMtx);
        bool any = false;
        for (int sl = 0; sl < 2; sl++) {
            if (!(pullMask & (1 << sl))) { fifo[sl].clear(); continue; }
            if (fifo[sl].pull(out, n, pullGain)) any = true;
        }
        return any;
    }
    // Speech is waiting or playing
    bool hasVoice() const {
        std::lock_guard<std::mutex> f(fifoMtx);
        for (int sl = 0; sl < 2; sl++) if ((pullMask & (1 << sl)) && fifo[sl].size() > 0) return true;
        return false;
    }
    void discardVoice() {
        std::lock_guard<std::mutex> f(fifoMtx);
        fifo[0].clear(); fifo[1].clear();
    }

private:
    mutable std::mutex mtx;
    mutable std::mutex fifoMtx;     // the speech buffers (pull() runs on another thread)
    int pullMask = 3;
    float pullGain = 1.0f;
    Want want = Want::Auto;
    bool needConfigure = true;
    int slotMask = 3;
    bool muteEncrypted = true;
    Status st;
    long long sampleCount = 0;
    int locked = -1;                // the instance that has the signal (-1: all run)
    long long lastSyncSample = -1;
    VoiceFifo fifo[2];
    Upsample6 up[2];
    std::vector<float> upBuf;

    struct Talk {
        bool active = false;
        long long lastVoice = -1, start = 0;
        Call call;
        std::string lastKey;
    } talk[2];

    // what has been logged as events (per slot), so each item is logged once
    struct Logged {
        unsigned int dataSeq = 0, textSeq = 0;
        std::string alias, aliasSeen;
        long long aliasSince = -1;
        double lat = 1e9, lon = 1e9;
        std::string csbkKey;
        long long csbkAt = -1;
        unsigned int csbkCount = 0;
    } logged[2];
    std::string dstarText, dstarTextSeen, dstarLoc;
    long long dstarTextSince = -1;

#ifdef ENABLE_DSDCC
    struct Inst {
        DSDcc::DSDDecoder d;
        unsigned int dmrSeq[2] = {0, 0};
        long long confirmedAt = -1;   // sampleCount when the signal was last confirmed
        long long crcWindowAt = 0;    // the CRC score at the start of the current second
        unsigned long long crcWindowBase = 0;
        unsigned long long crcLast = 0;
    };
    std::vector<std::unique_ptr<Inst>> inst;

    static void setup(DSDcc::DSDDecoder& d, std::initializer_list<DSDcc::DSDDecoder::DSDDecodeMode> modes,
                      DSDcc::DSDDecoder::DSDRate rate) {
        d.setQuiet();
        d.setLogVerbosity(0);
        d.setDecodeMode(DSDcc::DSDDecoder::DSDDecodeNone, true);
        for (auto m : modes) d.setDecodeMode(m, true);
        d.setDataRate(rate);
        d.setUvQuality(3);
        d.setUpsampling(0);
        d.enableAudioOut(true);
        d.setTDMAStereo(true);   // slot 1 and slot 2 come out separately
        d.setAudioGain(0.0f);    // automatic
#ifdef ORBITSDR_NO_VOCODER
        d.enableMbelib(false);
#else
        d.enableMbelib(true);
#endif
    }
#endif

    void configure() {
        needConfigure = false;
        locked = -1;
        lastSyncSample = -1;
        { std::lock_guard<std::mutex> f(fifoMtx); fifo[0].clear(); fifo[1].clear(); }
        for (int i = 0; i < 2; i++) { up[i].reset(); endTalk(i); talk[i] = Talk(); logged[i] = Logged(); }
        dstarText.clear(); dstarTextSeen.clear(); dstarLoc.clear(); dstarTextSince = -1;
        st = Status();
#ifdef ENABLE_DSDCC
        using D = DSDcc::DSDDecoder;
        inst.clear();
        auto add = [&](std::initializer_list<D::DSDDecodeMode> modes, D::DSDRate rate) {
            inst.push_back(std::make_unique<Inst>());
            setup(inst.back()->d, modes, rate);
        };
        switch (want) {
        case Want::Auto:  add({D::DSDDecodeDMR, D::DSDDecodeDStar, D::DSDDecodeYSF}, D::DSDRate4800);
                          add({D::DSDDecodeDPMR, D::DSDDecodeNXDN48}, D::DSDRate2400); break;
        case Want::DMR:   add({D::DSDDecodeDMR}, D::DSDRate4800); break;
        case Want::DPMR:  add({D::DSDDecodeDPMR}, D::DSDRate2400); break;
        case Want::DSTAR: add({D::DSDDecodeDStar}, D::DSDRate4800); break;
        case Want::YSF:   add({D::DSDDecodeYSF}, D::DSDRate4800); break;
        case Want::NXDN:  add({D::DSDDecodeNXDN96}, D::DSDRate4800);
                          add({D::DSDDecodeNXDN48}, D::DSDRate2400); break;
        }
#endif
    }

    double secs(long long samples) const { return samples / 48000.0; }

    void endTalk(int sl) {
        Talk& t = talk[sl];
        if (!t.active) return;
        t.active = false;
        t.call.open = false;
        t.call.seconds = secs(t.lastVoice - t.start);
        if (onCall) onCall(t.call);
    }

#ifdef ENABLE_DSDCC
    // Speech frames of one instance: into the slot buffers (only from the instance with the signal)
    void takeAudio(Inst& in, size_t k) {
        for (int sl = 0; sl < 2; sl++) {
            int cnt = 0;
            short* a = sl == 0 ? in.d.getAudio1(cnt) : in.d.getAudio2(cnt);
            if (cnt <= 0) continue;
            const bool mine = locked < 0 || (int)k == locked;
            if (mine) {
                talk[sl].lastVoice = sampleCount;
                if (!talk[sl].active) { talk[sl].active = true; talk[sl].start = sampleCount; talk[sl].lastKey.clear(); }
                const bool mute = muteEncrypted && st.slot[sl].encrypted;
                if (!mute) {
                    upBuf.clear();
                    up[sl].process(a, cnt, upBuf);
                    // soft limiter: the peaks of the speech (and the overshoot of the
                    // interpolation) round off above 3/4 of full scale instead of clipping
                    for (float& v : upBuf) {
                        const float m = std::fabs(v);
                        if (m > 0.75f) v = std::copysign(0.75f + 0.25f * std::tanh((m - 0.75f) * 4.0f), v);
                    }
                    std::lock_guard<std::mutex> f(fifoMtx);
                    fifo[sl].push(upBuf.data(), upBuf.size());
                }
            }
            if (sl == 0) in.d.resetAudio1(); else in.d.resetAudio2();
        }
    }

    static std::string idStr(unsigned v) { return std::to_string(v); }
    static std::string trim(std::string s) {
        while (!s.empty() && (s.back() == ' ' || s.back() == 0)) s.pop_back();
        size_t i = 0;
        while (i < s.size() && s[i] == ' ') i++;
        return s.substr(i);
    }

    // Is the signal of an instance real? Speech through the voice gate, or signalling whose CRCs
    // check: a score of 4 within a second (one CRC of 12 bits or more, or several weak ones).
    // A sync pattern alone proves nothing: the short ones (dPMR, NXDN) match noise all the time.
    void confirm(Inst& in) {
        using D = DSDcc::DSDDecoder;
        D& d = in.d;
        bool ok = d.voiceGateOpen(0) || d.voiceGateOpen(1);
        const unsigned long long score = d.getCrcScore();
        if (secs(sampleCount - in.crcWindowAt) > 1.0) { in.crcWindowAt = sampleCount; in.crcWindowBase = in.crcLast; }
        if (score != in.crcLast && score - in.crcWindowBase >= 4) ok = true;
        in.crcLast = score;
        if (ok && d.getSyncType() != D::DSDSyncNone) in.confirmedAt = sampleCount;
    }

    // Every 10 ms: which instance has the signal, and what it says
    void poll() {
        using D = DSDcc::DSDDecoder;
        for (size_t k = 0; k < inst.size(); k++) {
            if (locked >= 0 && (int)k != locked) continue;
            confirm(*inst[k]);
        }
        auto recent = [&](const Inst& in, double s) { return in.confirmedAt >= 0 && secs(sampleCount - in.confirmedAt) < s; };
        // the signal is gone: listen with all decoders again
        if (locked >= 0 && !recent(*inst[locked], 2.0)) locked = -1;
        int best = locked;
        if (best < 0)
            for (size_t k = 0; k < inst.size(); k++)
                if (recent(*inst[k], 0.5)) { best = (int)k; break; }
        if (best >= 0) {
            if (inst[best]->d.getSyncType() != D::DSDSyncNone) lastSyncSample = sampleCount;
            if (locked < 0 && inst.size() > 1) {
                locked = best;
                for (size_t k = 0; k < inst.size(); k++) if ((int)k != best) inst[k]->d.resetAudio1(), inst[k]->d.resetAudio2();
            }
        }
        const double since = lastSyncSample < 0 ? 1e9 : secs(sampleCount - lastSyncSample);

        // end of calls: no speech for 0.6 s
        for (int sl = 0; sl < 2; sl++)
            if (talk[sl].active && secs(sampleCount - talk[sl].lastVoice) > 0.6) endTalk(sl);

        st.sinceSync = since;
        st.sync = since < 0.3;
        const int src = best;
        if (src < 0) {
            if (!st.sync) for (int sl = 0; sl < 2; sl++) { st.slot[sl].voice = false; st.slot[sl].data = false; }
            return;
        }
        st.everSync = true;
        fill(*inst[src]);
        for (int sl = 0; sl < 2; sl++) {
            Talk& t = talk[sl];
            SlotView& v = st.slot[sl];
            v.voice = t.active && secs(sampleCount - t.lastVoice) < 0.3;
            v.voiceSec = t.active ? secs(t.lastVoice - t.start) : 0.0;
            if (!t.active) continue;
            // the call record follows what the frames say
            const std::string key = v.from + "|" + v.to + "|" + v.alias + "|" + (v.encrypted ? "E" : "") + v.encText;
            if (key != t.lastKey) {
                t.lastKey = key;
                Call& c = t.call;
                if (!c.open || c.time.empty()) { c = Call(); c.time = wallClock(); }   // a new call
                c.open = true;
                c.proto = st.proto;
                c.slot = st.nSlots > 1 ? sl + 1 : 0;
                c.from = v.from; c.to = v.to; c.alias = v.alias;
                c.encrypted = v.encrypted; c.encText = v.encText;
                c.extra = v.extra;
                c.seconds = v.voiceSec;
                if (onCall) onCall(c);
            }
            t.call.seconds = v.voiceSec;
        }
    }

    void event(const std::string& line) { if (onEvent) onEvent(line); }

    static const char* dpfName(int dpf) {
        switch (dpf) {
        case 0: return "UDT";
        case 1: return "response";
        case 2: return "unconfirmed";
        case 3: return "confirmed";
        case 13: return "short data (defined)";
        case 14: return "short data (raw / status)";
        case 15: return "proprietary";
        default: return "format ?";
        }
    }
    static const char* sapName(int sap) {
        switch (sap) {
        case 0: return "UDT";
        case 2: return "TCP/IP (compressed)";
        case 3: return "UDP/IP (compressed)";
        case 4: return "IP";
        case 5: return "ARP";
        case 9: return "proprietary";
        case 10: return "short data";
        default: return "SAP ?";
        }
    }
    static const char* udtFormatName(int f) {
        switch (f) {
        case 0: return "binary";
        case 1: return "addresses";
        case 2: return "BCD";
        case 3: return "7-bit text";
        case 4: return "8-bit text";
        case 5: return "NMEA position";
        case 6: return "IP address";
        case 7: return "UTF-16 text";
        case 8: return "manufacturer";
        default: return "mixed";
        }
    }

    // The DMR data of a slot that has not been logged yet
    void dmrEvents(int sl, const DSDcc::DSDDMR::SlotInfo& si, int cc, bool ms) {
        Logged& lg = logged[sl];
        char b[96];
        std::snprintf(b, sizeof(b), "DMR%s CC%d  ", ms ? "" : (sl ? " TS2" : " TS1"), cc);
        const std::string pre = b;
        auto addr = [](unsigned int src, unsigned int dst, bool group) {
            return std::to_string(src) + " > " + (group ? "TG " : "ID ") + std::to_string(dst);
        };
        if (si.textSeq != lg.textSeq) {
            lg.textSeq = si.textSeq;
            event(pre + "SMS  " + addr(si.textSrc, si.textDst, si.textGroup) + "  \"" + si.text + "\"  (" + si.textKind + ")");
        }
        if (si.dataSeq != lg.dataSeq) {
            lg.dataSeq = si.dataSeq;
            std::string d = pre + "DATA  " + addr(si.dataSrc, si.dataDst, si.dataGroup) + "  " + dpfName(si.dataDpf);
            if (si.dataDpf == 0) d += std::string(", ") + udtFormatName(si.dataUdtFormat);
            else if (si.dataDpf >= 2 && si.dataDpf <= 3) d += std::string(", ") + sapName(si.dataSap);
            if (si.dataBlocks > 0) d += ", " + std::to_string(si.dataBlocks) + (si.dataBlocks == 1 ? " block" : " blocks");
            event(d);
        }
        // CSBK: once per kind and addresses (repeated control blocks only every 30 s)
        if (si.csbkOpcode >= 0 && si.csbkCount != lg.csbkCount) {
            lg.csbkCount = si.csbkCount;
            const bool addressed = si.csbkFid == 0 && (si.csbkOpcode == 0x3D || si.csbkOpcode == 0x04 || si.csbkOpcode == 0x05);
            const std::string key = std::to_string(si.csbkFid) + "/" + std::to_string(si.csbkOpcode) +
                                    (addressed ? "/" + std::to_string(si.csbkSrc) + "/" + std::to_string(si.csbkDst) : "");
            if (key != lg.csbkKey || secs(sampleCount - lg.csbkAt) > 30.0) {
                std::string c = pre + "CSBK  " + dmrCsbkName(si.csbkFid, si.csbkOpcode);
                if (addressed) c += "  " + std::to_string(si.csbkSrc) + " > " + std::to_string(si.csbkDst);
                event(c);
                lg.csbkKey = key;
                lg.csbkAt = sampleCount;
            }
        }
        // talker alias: when it has stopped growing for a second
        const std::string alias = trim(si.alias);
        if (alias != lg.aliasSeen) { lg.aliasSeen = alias; lg.aliasSince = sampleCount; }
        else if (!alias.empty() && alias != lg.alias && secs(sampleCount - lg.aliasSince) > 1.0) {
            lg.alias = alias;
            event(pre + "ALIAS  " + std::to_string(si.src) + "  \"" + alias + "\"");
        }
        if (si.haveGps && (std::fabs(si.lat - lg.lat) > 1e-5 || std::fabs(si.lon - lg.lon) > 1e-5)) {
            lg.lat = si.lat; lg.lon = si.lon;
            static const char* errs[8] = {"< 2 m", "< 20 m", "< 200 m", "< 2 km", "< 20 km", "< 200 km", "> 200 km", "?"};
            std::snprintf(b, sizeof(b), "GPS  %u  %.5f %.5f  (%s)", si.src, si.lat, si.lon, errs[si.posErr & 7]);
            event(pre + b);
        }
    }

    void fill(Inst& in) {
        using D = DSDcc::DSDDecoder;
        D& d = in.d;
        st.inLevel = d.getInLevel();
        st.quality = d.getSymbolSyncQuality();
        st.fields.clear();
        const D::DSDSyncType sy = d.getSyncType();
        const auto stationType = d.getStationType();
        st.station = stationType == D::DSDBaseStation ? "Repeater (BS)" : stationType == D::DSDMobileStation ? "Direct / talkaround (MS)" : "";
        if (sy == D::DSDSyncDMRDataP || sy == D::DSDSyncDMRDataMS || sy == D::DSDSyncDMRVoiceP || sy == D::DSDSyncDMRVoiceMS) {
            st.proto = "DMR";
            const auto& dmr = d.getDMRDecoder();
            st.nSlots = stationType == D::DSDMobileStation ? 1 : 2;
            char b[64];
            std::snprintf(b, sizeof(b), "%d", (int)dmr.getSlotTypeColorCode());
            st.fields.push_back({"Color code", b});
            std::string sys = stationType == D::DSDMobileStation ? "Direct mode (no repeater)" : "Tier II conventional";
            for (int sl = 0; sl < 2; sl++) {
                const auto& si = dmr.getSlotInfo(sl);
                SlotView& v = st.slot[sl];
                if (si.csbkFid == 0x10 && (si.csbkOpcode == 0x3E || si.csbkOpcode == 0x3B)) sys = "Capacity Plus (Motorola trunking)";
                else if (si.csbkFid == 0x06) sys = "Connect Plus (Motorola trunking)";
                else if (si.csbkFid == 0x00 && (si.csbkOpcode == 0x19 || si.csbkOpcode == 0x28 || si.csbkOpcode == 0x1C)) sys = "Tier III trunking";
                if (si.haveLC) {
                    v.from = idStr(si.src);
                    v.to = (si.group ? "TG " : "ID ") + idStr(si.dst);
                } else { v.from.clear(); v.to.clear(); }
                v.alias = si.alias;
                v.encrypted = si.privacy || si.havePI;
                if (si.havePI) {
                    v.encText = dmrAlgName(si.algId) + ", key " + std::to_string(si.keyId);
                } else if (si.privacy) {
                    v.encText = "privacy bit set (Basic or Enhanced Privacy)";
                } else v.encText.clear();
                std::string ex;
                if (si.emergency) ex += "EMERGENCY  ";
                if (si.broadcast) ex += "broadcast  ";
                if (si.priority) ex += "priority " + std::to_string(si.priority) + "  ";
                if (si.haveGps) {
                    std::snprintf(b, sizeof(b), "GPS %.5f %.5f  ", si.lat, si.lon);
                    ex += b;
                }
                if (si.fid > 0) { const char* fn = dmrFidName(si.fid); ex += std::string("features: ") + (fn ? fn : "manufacturer"); }
                v.extra = trim(ex);
                const bool voiceNow = talk[sl].active && secs(sampleCount - talk[sl].lastVoice) < 0.3;
                dmrEvents(sl, si, (int)dmr.getSlotTypeColorCode(), stationType == D::DSDMobileStation);
                v.data = !voiceNow && si.lastDataType >= 0 && si.lastDataType != 9;
                if (voiceNow) v.activity = v.encrypted ? "Voice (encrypted)" : "Voice";
                else if (si.lastDataType == 9) v.activity = "Idle";
                else if (si.lastDataType == 2) v.activity = "Call ended (hang time)";
                else if (si.lastDataType == 3 && si.csbkOpcode >= 0) v.activity = "CSBK: " + dmrCsbkName(si.csbkFid, si.csbkOpcode);
                else if (si.lastDataType == 6) v.activity = "Data " + idStr(si.dataSrc) + " > " + idStr(si.dataDst);
                else if (si.lastDataType >= 7 && si.lastDataType <= 10) v.activity = "Data blocks";
                else if (si.lastDataType == 0) v.activity = "Privacy header";
                else if (si.lastDataType == 1) v.activity = "Call setup";
                else v.activity = "-";
            }
            st.system = sys;
        } else if (sy == D::DSDSyncDPMR || sy == D::DSDSyncDPMRPacket || sy == D::DSDSyncDPMRPayload || sy == D::DSDSyncDPMREnd) {
            st.proto = "dPMR";
            st.nSlots = 1;
            st.system = "";
            const auto& p = d.getDPMRDecoder();
            char b[32];
            std::snprintf(b, sizeof(b), "%d", p.getColorCode());
            st.fields.push_back({"Colour code", b});
            SlotView& v = st.slot[0];
            v.from = p.getOwnId() ? idStr(p.getOwnId()) : "";
            v.to = p.getCalledId() ? "ID " + idStr(p.getCalledId()) : "";
            v.encrypted = false; v.encText.clear();
            v.alias.clear(); v.extra.clear();
            v.activity = talk[0].active ? "Voice" : "Signalling";
        } else if (sy == D::DSDSyncDStarP || sy == D::DSDSyncDStarN || sy == D::DSDSyncDStarHeaderP || sy == D::DSDSyncDStarHeaderN) {
            st.proto = "D-STAR";
            st.nSlots = 1;
            st.system = "";
            const auto& ds = d.getDStarDecoder();
            SlotView& v = st.slot[0];
            v.from = trim(ds.getMySign());
            v.to = trim(ds.getYourSign());
            v.alias = trim(ds.getInfoText());
            v.encrypted = false; v.encText.clear();
            v.extra.clear();
            const std::string r1 = trim(ds.getRpt1()), r2 = trim(ds.getRpt2());
            if (!r1.empty()) st.fields.push_back({"RPT1", r1});
            if (!r2.empty()) st.fields.push_back({"RPT2", r2});
            const std::string loc = trim(ds.getLocator());
            if (!loc.empty()) st.fields.push_back({"Locator", loc});
            // events: the slow data text and the position, once each
            const std::string who = v.from.empty() ? "?" : v.from;
            if (v.alias != dstarTextSeen) { dstarTextSeen = v.alias; dstarTextSince = sampleCount; }
            else if (!v.alias.empty() && v.alias != dstarText && secs(sampleCount - dstarTextSince) > 1.0) {
                dstarText = v.alias;
                event("D-STAR  TEXT  " + who + (v.to.empty() ? "" : " > " + v.to) + "  \"" + v.alias + "\"");
            }
            if (!loc.empty() && loc != dstarLoc) {
                dstarLoc = loc;
                event("D-STAR  POSITION  " + who + "  " + loc);
            }
            v.activity = talk[0].active ? "Voice" : "Header / data";
        } else if (sy == D::DSDSyncYSF) {
            st.proto = "YSF";
            st.nSlots = 1;
            st.system = "Yaesu System Fusion (C4FM)";
            const auto& y = d.getYSFDecoder();
            SlotView& v = st.slot[0];
            v.from = trim(y.getSrc());
            v.to = trim(y.getDest());
            v.encrypted = false; v.encText.clear();
            v.alias.clear(); v.extra.clear();
            const std::string ul = trim(y.getUplink()), dl = trim(y.getDownlink());
            if (!ul.empty()) st.fields.push_back({"Uplink", ul});
            if (!dl.empty()) st.fields.push_back({"Downlink", dl});
            const int dt = (int)y.getFICH().getDataType();
            if (dt >= 0 && dt < 4) st.fields.push_back({"Mode", DSDcc::DSDYSF::ysfDataTypeText[dt]});
            v.activity = talk[0].active ? "Voice" : "Data";
        } else if (sy == D::DSDSyncNXDNP || sy == D::DSDSyncNXDNN || sy == D::DSDSyncNXDNDataP || sy == D::DSDSyncNXDNDataN) {
            const auto& nx = d.getNXDNDecoder();
            st.proto = d.getDataRate() == D::DSDRate2400 ? "NXDN48" : "NXDN96";
            st.nSlots = 1;
            st.system = "";
            char b[32];
            std::snprintf(b, sizeof(b), "%d", nx.getRAN());
            st.fields.push_back({"RAN", b});
            if (nx.getLocationId()) { std::snprintf(b, sizeof(b), "%u", nx.getLocationId()); st.fields.push_back({"Location", b}); }
            SlotView& v = st.slot[0];
            v.from = nx.getSourceId() ? idStr(nx.getSourceId()) : "";
            v.to = nx.getDestinationId() ? (nx.isGroupCall() ? "TG " : "ID ") + idStr(nx.getDestinationId()) : "";
            const int ct = nx.getCipherType();
            v.encrypted = ct != 0;
            static const char* ciphers[4] = {"", "scrambler", "DES", "AES"};
            v.encText = ct ? std::string(ciphers[ct & 3]) + ", key " + std::to_string(nx.getKeyId()) : "";
            v.alias.clear(); v.extra.clear();
            v.activity = talk[0].active ? (v.encrypted ? "Voice (encrypted)" : "Voice") : "Control / data";
        }
        if (!st.station.empty() && st.proto != "DMR") st.station.clear();
    }
#endif
};

} // namespace dv
