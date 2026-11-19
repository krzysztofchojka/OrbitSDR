#pragma once

#include <string>
#include <deque>
#include <mutex>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <functional>

#include "TetraVoice.h"
#include "../../Core/PositionBus.h"

// =====================================================================
//  tetra::Core - OrbitSDR's in-process wrapper around the ported tetra-kit
//  decoder (deps/tetra-kit/decoder). It takes the hard bits recovered by
//  the pi/4-DQPSK front end (TetraPhy.h: TetraDemod + TetraSync) and turns
//  them into what every radio on the channel receives in the clear: the
//  cell identity (MCC / MNC / colour code / location area), unencrypted
//  SDS (text, status, data), LIP positions, call control (who talks to
//  which group) and - with the speech codec (TetraVoice.h) - clear speech.
//
//  It never decrypts anything: speech is decoded only from time slots the
//  MAC marks as unencrypted (encryption mode 0); TEA traffic stays opaque.
//  Speech and message content are only produced while the UI allows it
//  (setContentEnabled) - see the "TETRA content" switch.
//
//  Built only when the TETRA decoder library is present (ENABLE_TETRA,
//  set by scripts/tetra.sh). Without it, Core is an inert stub so the
//  Digital Voice module still compiles and the pi/4-DQPSK lock indicator
//  keeps working.
// =====================================================================

#ifdef ENABLE_TETRA
#include "decoder.h"      // Tetra::TetraDecoder, Tetra::ReportData, Tetra::TetraCell
#endif

namespace tetra {

inline bool available() {
#ifdef ENABLE_TETRA
    return true;
#else
    return false;
#endif
}

struct NetInfo {
    bool     valid = false;
    uint32_t mcc = 0, mnc = 0, colourCode = 0, locationArea = 0;

    // air interface security of the cell (SYSINFO, EN 300 392-7 A.8.77)
    bool aieKnown = false;           // D-MLE-SYSINFO seen
    bool aieAvailable = false;       // "Air interface encryption service" available
    int  securityInfo = -1;          // 8-bit Security information element, -1 = not broadcast yet
};

// KSG number (EN 300 392-7 table 6.2): the air interface encryption algorithm.
inline std::string ksgName(int ksg) {
    if (ksg < 0) return std::string();
    if (ksg <= 3) return "TEA" + std::to_string(ksg + 1);
    if (ksg <= 7) return "reserved KSG " + std::to_string(ksg);
    return "proprietary KSG " + std::to_string(ksg);
}

// One line on the cell's air interface encryption, "" while unknown.
//   Security information bits (MSB first): authentication, security class 1 MS
//   supported, class 2 (0) / class 3 (1); then SCKN (5) for class 2, or DCK / GCK
//   flags for class 3. They mean something only when the BS offers encryption.
inline std::string airSecurityText(const NetInfo& n, int ksg) {
    if (!n.aieKnown) return std::string();
    const int si = n.securityInfo;
    const bool auth = si >= 0 && (si & 0x80);
    std::string s;
    if (!n.aieAvailable) {
        s = "clear (security class 1)";
    } else if (si < 0) {
        s = "encrypted (class 2 or 3)";
    } else {
        const bool class3 = si & 0x20;
        s = class3 ? "encrypted, security class 3" : "encrypted, security class 2, SCKN " + std::to_string(si & 0x1F);
        if (si & 0x40) s += " (class 1 radios allowed)";
    }
    if (auth) s += ", authentication";
    if (n.aieAvailable) s += ksg >= 0 ? ", " + ksgName(ksg) : ", algorithm not seen yet";
    return s;
}

struct SdsMsg {
    enum class Kind { Text, Data, Status };
    Kind        kind = Kind::Data;
    std::string time;
    std::string from;        // calling party SSI
    std::string to;          // addressed SSI / group (MAC address of the PDU)
    std::string proto;       // "simple text messaging", "user-defined data 1" ...
    std::string text;        // Text: the message; Data: hex payload; Status: status number
};

inline const char* sdsKindName(SdsMsg::Kind k) {
    switch (k) {
    case SdsMsg::Kind::Text:   return "text";
    case SdsMsg::Kind::Status: return "status";
    default:                   return "data";
    }
}

struct LipPos {
    std::string time;
    std::string from;        // SSI
    double      lat = 0.0, lon = 0.0;
    std::string extra;       // direction / velocity / error
};

struct Status {
    bool     active = false;        // decoder is receiving synchronised bursts
    NetInfo  net;
    uint64_t reports = 0;
    uint64_t encryptedPdus = 0;     // signalling PDUs the MAC marks as air-interface encrypted (not parsed)
    int      ksg = -1;              // KSG number from MM (location update / OTAR), -1 = not seen
    std::deque<SdsMsg> sds;        // newest first, bounded
    std::deque<LipPos> lip;         // newest first, bounded

    // call control (D-SETUP / D-TX GRANTED / D-RELEASE)
    std::string group;              // called group / party of the last call
    std::string talker;             // who has the transmission right now ("" = nobody)
    std::string callId;

    // speech (this channel owns the codec and decodes a clear time slot)
    bool voice = false;
    int  voiceTn = -1;
    int  voiceGood = 0, voiceFrames = 0;   // speech frames passing the CRC / decoded
};

class Core {
public:
    static constexpr size_t MAX_SDS = 200;
    static constexpr size_t MAX_LIP = 200;

    // For the calls table: a transmission starts / ends, an SDS arrives. Called on the
    // thread that feeds rxBit(), while that call runs. Call control is repeated many times
    // on the air (late entry): the receiver of these events drops the repeats.
    struct Event {
        enum class Type { TxStart, TxEnd, Sds };
        Type        type = Type::TxStart;
        std::string callId;
        std::string from, to;       // TxStart: talker > group
        bool        encrypted = false;    // end-to-end encrypted (CMCE "encryption control" /
                                          // basic service "encryption flag"), not TEA
        bool        setup = false;   // TxStart from D-SETUP: it names the CALLER, and is
                                      // repeated during the whole call, also while others talk
        bool        release = false;  // TxEnd from D-RELEASE: the whole call is over
        SdsMsg      sds;            // Sds
    };
    std::function<void(const Event&)> onEvent;

    // When a traffic (TCH/S) slot was last seen on the channel, clear or not, decoded or
    // not (steady clock, ms; 0 = never). Tells an over that is still going on.
    long long lastTrafficMs() const { return m_lastTrafficMs.load(); }

    Core() {
#ifdef ENABLE_TETRA
        m_dec = new Tetra::TetraDecoder(
            [this](const Tetra::ReportData& r) { onReport(r); },
            true,                           // remove fill bits
            Tetra::LogLevel::NONE,
            false);                         // no Wireshark output
#endif
    }

    ~Core() {
        releaseCodec();
#ifdef ENABLE_TETRA
        delete m_dec;
#endif
    }

    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;

    // Feed one recovered downlink bit (from TetraSync::onBit). DSP thread.
    void rxBit(uint8_t bit) {
#ifdef ENABLE_TETRA
        m_dec->rxSymbol(bit);
        // refresh the cell identity cheaply (it only changes on SYSINFO)
        const Tetra::TetraCell* c = m_dec->cell();
        if (c && (c->mcc() || c->mnc())) {
            std::lock_guard<std::mutex> l(m_mtx);
            m_st.net.valid = true;
            m_st.net.mcc = c->mcc();
            m_st.net.mnc = c->mnc();
            m_st.net.colourCode = c->colorCode();
            m_st.net.locationArea = c->locationArea();
            m_st.net.aieKnown = c->aieKnown();
            m_st.net.aieAvailable = c->aieAvailable();
            m_st.net.securityInfo = c->securityInfo();
            m_st.active = m_dec->isSynchronized();
        }
#else
        (void)bit;
#endif
    }

    // Speech is decoded only while allowed (the UI's content switch).
    void setContentEnabled(bool on) {
        if (m_contentOn.exchange(on) && !on) { releaseCodec(); discardVoice(); }
    }

    void reset() {
        releaseCodec();
        discardVoice();
        std::lock_guard<std::mutex> l(m_mtx);
        m_st = Status();
    }

    // Thread-safe snapshot for the UI.
    Status status() {
        Status s;
        {
            std::lock_guard<std::mutex> l(m_mtx);
            s = m_st;
        }
        CodecOwner& o = owner();
        std::lock_guard<std::mutex> l(o.m);
        s.voice = o.core == this && Clock::now() - o.last < std::chrono::milliseconds(700);
        s.voiceTn = o.core == this ? o.tn : -1;
        return s;
    }

    // --- speech for the speaker (48 kHz), called by the DSP thread -------
    bool pull(float* out, size_t n) {
        std::lock_guard<std::mutex> l(m_vmtx);
        if (!m_playing && m_vq.size() >= PREBUF) m_playing = true;
        if (!m_playing) return false;
        const size_t k = std::min(n, m_vq.size());
        for (size_t i = 0; i < k; i++) { out[i] += m_vq.front(); m_vq.pop_front(); }
        if (m_vq.empty()) m_playing = false;
        return k > 0;
    }
    bool hasVoice() const {
        std::lock_guard<std::mutex> l(m_vmtx);
        return m_playing || m_vq.size() >= PREBUF;
    }
    void discardVoice() {
        std::lock_guard<std::mutex> l(m_vmtx);
        m_vq.clear();
        m_playing = false;
    }

private:
    using Clock = std::chrono::steady_clock;
    static constexpr size_t PREBUF = 48000 / 4;          // 250 ms: absorbs the TDMA gaps and block jitter
    static constexpr size_t MAX_BUFFERED = 48000 * 2;    // never lag more than 2 s behind

    // The reference codec has global state: one channel + time slot owns it.
    struct CodecOwner {
        std::mutex m;
        const Core* core = nullptr;
        int tn = -1;
        Clock::time_point last;
    };
    static CodecOwner& owner() { static CodecOwner o; return o; }

    void releaseCodec() {
        CodecOwner& o = owner();
        std::lock_guard<std::mutex> l(o.m);
        if (o.core == this) { o.core = nullptr; o.tn = -1; }
    }

#ifdef ENABLE_TETRA
    Tetra::TetraDecoder* m_dec = nullptr;

    // One clear TCH/S time slot -> 60 ms of speech, if this channel / slot
    // owns the codec (or takes it: the first one whose frame passes the CRC).
    void onSpeech(const Tetra::ReportData& r) {
        if (!speechAvailable() || !m_contentOn) return;
        if (r.blob.size() != 2 * 690 || r.get("encryption mode") != "0") return;
        const int tn = std::atoi(r.get("tn").c_str());
        const Clock::time_point now = Clock::now();

        CodecOwner& o = owner();
        bool restart = false;
        {
            std::lock_guard<std::mutex> l(o.m);
            if (o.core && now - o.last > std::chrono::seconds(1)) o.core = nullptr;   // owner went quiet
            if (o.core && !(o.core == this && o.tn == tn)) return;                     // someone else talks
            restart = o.core == nullptr;
        }

        // the 432 soft bits, where UPlane put them (the reference test-frame layout)
        int16_t w[690];
        std::memcpy(w, r.blob.data(), sizeof(w));
        int16_t soft[SpeechCodec::SLOT_BITS], pcm[SpeechCodec::SLOT_SAMPLES];
        int k = 0;
        for (int i = 0; i < 114; i++) soft[k++] = w[1 + i];
        for (int i = 0; i < 114; i++) soft[k++] = w[116 + i];
        for (int i = 0; i < 114; i++) soft[k++] = w[231 + i];
        for (int i = 0; i < 90;  i++) soft[k++] = w[346 + i];
        const int good = SpeechCodec::decodeSlot(soft, pcm, restart);
        if (good < 0) return;

        {
            std::lock_guard<std::mutex> l(o.m);
            if (restart) {
                if (good == 0 || o.core) return;      // garbage slot, or taken meanwhile
                o.core = this;
                o.tn = tn;
            }
            o.last = now;
        }
        {
            std::lock_guard<std::mutex> l(m_mtx);
            if (restart) { m_st.voiceGood = 0; m_st.voiceFrames = 0; }
            m_st.voiceGood += good;
            m_st.voiceFrames += 2;
        }

        // 8 kHz -> 48 kHz (x6, linear; the codec output ends at 4 kHz)
        std::lock_guard<std::mutex> l(m_vmtx);
        for (int i = 0; i < SpeechCodec::SLOT_SAMPLES; i++) {
            const float s = pcm[i] / 32768.0f;
            for (int j = 1; j <= 6; j++) m_vq.push_back(m_prev + (s - m_prev) * (j / 6.0f));
            m_prev = s;
        }
        while (m_vq.size() > MAX_BUFFERED) m_vq.pop_front();
    }

    // Called from rxBit's thread when a PDU is decoded.
    void emit(const Event& e) { if (onEvent) onEvent(e); }

    void onReport(const Tetra::ReportData& r) {
        if (r.service == "UPLANE") {
            m_lastTrafficMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());
            onSpeech(r);
            return;
        }

        std::lock_guard<std::mutex> l(m_mtx);
        m_st.active = true;

        // Signalling the MAC marks as encrypted (EN 300 392-7 A.8.20: 10 / 11 = encrypted
        // with SCK / CCK) is TEA ciphertext: tetra-kit would parse it as garbage, so it is
        // only counted. The cell's SYSINFO tells which security class it runs.
        const std::string encMode = r.get("encryption mode");
        if (!encMode.empty() && encMode != "0") { m_st.encryptedPdus++; return; }
        m_st.reports++;

        // the air interface algorithm: the KSG number of the ciphering parameters
        // (D-LOCATION UPDATE COMMAND / REJECT) or of an OTAR key delivery
        if (r.has("KSG number")) m_st.ksg = std::atoi(r.get("KSG number").c_str()) & 0xF;

        // call control: who talks to whom
        if (r.pdu == "D-SETUP" || r.pdu == "D-CONNECT") {
            m_st.group  = r.has("actual ssi") ? r.get("actual ssi") : r.get("ssi");
            m_st.callId = r.get("call identifier");
            if (r.has("calling party ssi")) {
                m_st.talker = r.get("calling party ssi");
                Event e;
                e.type = Event::Type::TxStart;
                e.callId = m_st.callId; e.from = m_st.talker; e.to = m_st.group;
                e.setup = true;
                // basic service information (14.8.2): circuit mode type (3), encryption
                // flag (1, end-to-end), communication type (2), slots / speech (2)
                if (r.has("basic service information"))
                    e.encrypted = (std::atoi(r.get("basic service information").c_str()) >> 4) & 1;
                emit(e);
            }
        } else if (r.pdu == "D-TX GRANTED") {
            m_st.group  = r.has("actual ssi") ? r.get("actual ssi") : r.get("ssi");
            m_st.callId = r.get("call identifier");
            m_st.talker = r.get("transmitting party ssi");
            Event e;
            e.type = Event::Type::TxStart;
            e.callId = m_st.callId; e.from = m_st.talker; e.to = m_st.group;
            e.encrypted = r.get("encryption control") == "1";
            emit(e);
        } else if (r.pdu == "D-TX CEASED" || r.pdu == "D-RELEASE") {
            m_st.talker.clear();
            Event e;
            e.type = Event::Type::TxEnd;
            e.callId = r.get("call identifier");
            e.release = r.pdu == "D-RELEASE";
            emit(e);
        }

        // SDS. tetra-kit overloads "infos": decoded text only when a text coding
        // scheme came with it, otherwise a hex dump of user-defined data.
        // Pre-coded status (D-STATUS) is a separate PDU with its own field.
        const bool isStatus = r.has("pre-coded status");
        if (r.has("infos") || isStatus) {
            SdsMsg m;
            m.time = r.get("time");
            m.from = r.get("calling party ssi");
            m.to   = r.get("ssi");
            if (isStatus) {
                m.kind  = SdsMsg::Kind::Status;
                m.proto = "pre-coded status";
                m.text  = r.get("pre-coded status");
            } else if (r.has("text coding scheme")) {
                m.kind  = SdsMsg::Kind::Text;
                m.proto = r.get("protocol info");
                m.text  = r.get("infos");
            } else {
                m.kind = SdsMsg::Kind::Data;
                static const char* udd[4] = {"user-defined data 1 (16 bit)", "user-defined data 2 (32 bit)",
                                             "user-defined data 3 (64 bit)", "user-defined data 4"};
                const std::string sdti = r.get("sds type identifier");
                const int t = sdti.empty() ? -1 : std::atoi(sdti.c_str());
                m.proto = r.has("protocol info") ? r.get("protocol info") : (t >= 0 && t < 4 ? udd[t] : "data");
                m.text  = r.get("infos");
            }
            m_st.sds.push_front(m);
            if (m_st.sds.size() > MAX_SDS) m_st.sds.pop_back();
            Event e;
            e.type = Event::Type::Sds;
            e.sds = m;
            emit(e);
        }

        // LIP position report
        if (r.has("latitude") && r.has("longitude")) {
            LipPos p;
            p.time = r.get("time");
            p.from = r.has("calling party ssi") ? r.get("calling party ssi") : r.get("ssi");
            p.lat  = std::atof(r.get("latitude").c_str());
            p.lon  = std::atof(r.get("longitude").c_str());
            if (p.lat == 0.0 && p.lon == 0.0) return;   // no fix: nothing to place
            const std::string dir = r.get("direction of travel");       // "22.5 NNE"
            const std::string vel = r.get("horizontal_velocity");       // km/h, -1 = unknown
            const std::string err = r.get("position error");            // "< 20 m"
            const double kmh = vel.empty() ? -1.0 : std::atof(vel.c_str());
            if (!dir.empty()) p.extra = "heading " + dir;
            if (kmh >= 0.0) p.extra += (p.extra.empty() ? "" : "  ") + std::to_string((int)(kmh + 0.5)) + " km/h";
            if (!err.empty() && err != "unknown") p.extra += (p.extra.empty() ? "" : "  ") + std::string("error ") + err;
            m_st.lip.push_front(p);
            if (m_st.lip.size() > MAX_LIP) m_st.lip.pop_back();

            // on the map (a radio's position is content: only while content is shown)
            if (m_contentOn) {
                PosReport pr;
                pr.kind = PosKind::TETRA;
                pr.id = p.from.empty() ? std::string("?") : p.from;
                pr.label = "SSI " + pr.id;
                pr.info = (m_st.net.valid ? "MCC " + std::to_string(m_st.net.mcc) + " MNC " + std::to_string(m_st.net.mnc) + "  " : std::string()) + p.extra;
                pr.lat = p.lat;
                pr.lon = p.lon;
                if (!dir.empty()) pr.courseDeg = std::atof(dir.c_str());
                if (kmh >= 0.0) pr.speedKn = kmh / 1.852;
                positionBus().report(pr);
            }
        }
    }
#endif

    std::mutex m_mtx;
    Status     m_st;
    std::atomic<bool> m_contentOn {false};
    std::atomic<long long> m_lastTrafficMs {0};

    mutable std::mutex m_vmtx;      // speech buffer (pull() runs on the DSP thread)
    std::deque<float>  m_vq;
    bool  m_playing = false;
    float m_prev = 0.0f;
};

} // namespace tetra
