#pragma once

#include "../Core/SDRModule.h"
#include "../Core/DetailsPopup.h"
#include "../Core/ModuleLog.h"
#include "../Core/NarrowChannel.h"
#include "DigitalVoice.h"
#include "SignalClassifier.h"
#include "Tetra/TetraPhy.h"
#include "Tetra/TetraCore.h"
#include <SFML/Graphics.hpp>
#include <mutex>
#include <deque>
#include <map>
#include <vector>
#include <string>
#include <memory>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <algorithm>

// =====================================================================
//  Digital voice decoder module: DMR, dPMR, D-STAR, YSF, NXDN on several
//  channels at once.
//
//  Every channel (yellow band on the spectrum) is cut out of the I/Q,
//  FM demodulated and decoded by its own dv::Core, which finds the system
//  by itself (or decodes the one chosen on its card). One channel always
//  follows the tuner; "+ Channel at tuner" pins the tuner's frequency as
//  a further channel, so a port or an event with many channels can be
//  watched at once.
//
//  The panel: a card per channel (system, colour code, both DMR time
//  slots with who talks to whom, encryption, talker alias, GPS ...) and
//  the list of calls. When somebody talks, the speech goes to the speaker
//  (one channel at a time, or all mixed), and the main demodulator is
//  muted meanwhile. Encrypted calls are shown and kept silent.
//
//  A channel no decoder understands is described by the signal
//  classifier: TETRA, analog FM, FSK of an unknown system ...
//
//  Audio input (Input signal switch): the discriminator audio of a
//  receiver or a recording is decoded as one channel.
// =====================================================================

class DigitalVoiceDecoder : public SDRModule {
    using C = std::complex<double>;
    static constexpr int AUDIO_ID = -1;
    static constexpr size_t MAX_CHANNELS = 12;
    static constexpr size_t MAX_CALLS = 500;

    struct ChSettings {
        bool listen = true;
        dv::Want want = dv::Want::Auto;
    };

    struct Rx {
        int id = 0;
        long long freqHz = 0;
        bool follow = false;
        double widthHz = 12500.0;
        NarrowChannel nc;          // the channel -> 48 kHz
        NarrowChannel wide;        // 25 kHz around it, for the classifier
        std::vector<std::complex<float>> bb, bbw;
        std::vector<short> disc;
        std::complex<float> prev {1.0f, 0.0f};
        std::shared_ptr<dv::Core> core = std::make_shared<dv::Core>();
        dv::SignalClassifier cls;
        double levelDb = -150.0;
        int calls = 0;

        // TETRA pi/4-DQPSK front end (demod + burst sync). Runs only while the
        // classifier says the channel is TETRA. Proves the demodulator locks on a
        // real signal before the full tetra-kit decoder stack is wired in.
        NarrowChannel tet;                 // dedicated 72 kHz (4 samples/symbol) baseband
        std::vector<std::complex<float>> bbt;
        tetra::TetraDemod tdemod;
        tetra::TetraSync tsync;
        std::shared_ptr<tetra::Core> tcore = std::make_shared<tetra::Core>();   // ported tetra-kit decoder (+ speech)
        bool   tetReady = false;           // demod configured for the current rate
        int    tetBursts = 0;
        double tetCarrierHz = 0.0;
        double tetEvmDeg = 180.0;
        int    tetVariant = -1;
        double tetSrcRate = 0.0;           // group rate feeding the TETRA channel (diagnostics)
    };

    // TETRA occupies only ~+-12 kHz (Rs*(1+roll-off)/2 = 12.15 kHz), so a 54 kHz baseband
    // (3 samples/symbol) is ample. NarrowChannel resamples from whatever the group rate is,
    // up or down, so the demod runs even when the decoder is fed a narrow IQ stream.
    static constexpr double TETRA_RATE    = 54000.0;   // 3 x 18 ksym/s
    static constexpr double TETRA_MIN_SRC = 40000.0;   // need at least this at the group output

    // what the UI draws of a channel
    struct CardView {
        int id = 0;
        long long freqHz = 0;
        bool follow = false, isAudio = false;
        double levelDb = -150.0;
        dv::Status st;
        std::string verdict;
        bool tetra = false;
        bool tetLocked = false;
        int  tetBursts = 0;
        double tetCarrierHz = 0.0;
        double tetEvmDeg = 180.0;
        int  tetVariant = -1;
        double tetSrcRate = 0.0;
        tetra::Status tstat;               // decoded TETRA metadata (network info, SDS, LIP)
        dv::SignalClassifier cls;
        ChSettings set;
        int calls = 0;
    };

    struct VoiceSrc {
        int id;
        std::shared_ptr<dv::Core> core;
        bool listen;
        bool follow;
        long long freqHz;
        std::shared_ptr<tetra::Core> tcore;   // TETRA speech of the same channel

        // one speech source per channel, whichever system it decodes
        bool pull(float* out, size_t n) {
            bool a = core->pull(out, n);
            if (tcore && tcore->pull(out, n)) a = true;
            return a;
        }
        bool hasVoice() const { return core->hasVoice() || (tcore && tcore->hasVoice()); }
        void discardVoice() { core->discardVoice(); if (tcore) tcore->discardVoice(); }
    };

    std::mutex mtx;
    std::map<int, std::unique_ptr<Rx>> rxs;          // by channel id (AUDIO_ID: the audio input)
    std::map<int, ChSettings> settings;
    ChannelSet channels;
    GroupDecimator group;
    std::deque<dv::Call> calls;                      // newest first
    long long centerHz = 0;
    double lastRate = 0.0;
    double tunerBw = 12500.0;
    std::string info = "Waiting for signal...";
    std::atomic<bool> audioInput {false};
    int totalCalls = 0;

    // speech routing (the DSP thread, see mixVoice)
    std::mutex voiceMtx;
    std::vector<VoiceSrc> voiceList;
    int holder = 0;               // channel id that has the speaker ("one at a time"), 0 = none
    size_t holderIdle = 0;        // samples since the holder last spoke
    std::atomic<int> speakingId {0};   // for the UI
    std::atomic<bool> mixAll {false};
    std::atomic<bool> duckMain {true};
    std::atomic<bool> muteEnc {true};
    // TETRA SDS / LIP content and subscriber numbers are hidden until switched on
    // (network info is infrastructure metadata and always shown). Safe default for
    // a projector: nothing of a third party's traffic appears unless asked for.
    std::atomic<bool> showTetraContent {false};

    // UI
    const sf::Font* font = nullptr;
    sf::FloatRect panelRect, cardsRect, tableRect;
    float cardScroll = 0.0f, tableScroll = 0.0f;
    float tableBodyY = 0.0f;
    const float rowH = 16.0f;
    int tableFirst = 0, tableVisible = 0;
    struct CardHit { int id; sf::FloatRect card, want, listen, close; };
    std::vector<CardHit> cardHits;
    std::vector<CardView> lastCards;
    std::shared_ptr<SdrButton> btnAdd, btnMix, btnDuck, btnEnc, btnTetra, btnClear, btnLog, btnExport;
    DetailsPopup popup;

    // ---------------------------------------------------------------
    static std::string mhz(long long hz) {
        char b[24];
        std::snprintf(b, sizeof(b), hz % 100 == 0 ? "%.4f" : "%.5f", hz / 1e6);
        return b;
    }

    sf::Text text(const std::string& s, unsigned size, sf::Color c) const {
        sf::Text t(*font, sf::String::fromUtf8(s.begin(), s.end()), size);
        t.setScale({0.5f, 0.5f});
        t.setFillColor(c);
        return t;
    }

    static std::string fit(const std::string& s, size_t n) {
        return s.size() > n ? s.substr(0, n > 1 ? n - 1 : 0) + "." : s;
    }

    static std::string durText(double s) {
        char b[16];
        if (s < 60) std::snprintf(b, sizeof(b), "%.0f s", s);
        else std::snprintf(b, sizeof(b), "%d:%02d", (int)s / 60, (int)s % 60);
        return b;
    }

    // One SDS as text. Hidden: only its kind, no addresses, no payload.
    static std::string sdsText(const tetra::SdsMsg& m, bool show) {
        if (!show) return std::string(tetra::sdsKindName(m.kind)) + " (content hidden)";
        std::string s = (m.from.empty() ? std::string("?") : m.from) + " > " + (m.to.empty() ? std::string("?") : m.to) + "  ";
        switch (m.kind) {
        case tetra::SdsMsg::Kind::Text:   s += "\"" + m.text + "\""; break;
        case tetra::SdsMsg::Kind::Status: s += "status " + m.text; break;
        default:                          s += "[" + m.proto + "] " + m.text; break;
        }
        return s;
    }

    static dv::Want nextWant(dv::Want w) {
        switch (w) {
        case dv::Want::Auto: return dv::Want::DMR;
        case dv::Want::DMR: return dv::Want::DPMR;
        case dv::Want::DPMR: return dv::Want::DSTAR;
        case dv::Want::DSTAR: return dv::Want::YSF;
        case dv::Want::YSF: return dv::Want::NXDN;
        default: return dv::Want::Auto;
        }
    }

    // A call started / changed / ended on a channel (mtx is held: called from process)
    void onCall(Rx* r, dv::Call c) {
        c.freqHz = r->id == AUDIO_ID ? 0 : r->freqHz;
        c.channelId = r->id;
        for (auto& e : calls) {
            if (e.channelId == c.channelId && e.slot == c.slot && e.open) {
                const bool ended = !c.open;
                e = c;
                if (ended) logCall(e);
                return;
            }
        }
        if (!c.open) return;   // an end without a start (the list was cleared meanwhile)
        calls.push_front(c);
        r->calls++;
        totalCalls++;
        if (calls.size() > MAX_CALLS) calls.pop_back();
    }

    void logCall(const dv::Call& c) {
        std::string line = (c.freqHz ? mhz(c.freqHz) + " MHz  " : std::string("audio  ")) + c.proto +
                           (c.slot ? "  TS" + std::to_string(c.slot) : "") + "  " + (c.from.empty() ? "?" : c.from) +
                           " > " + (c.to.empty() ? "?" : c.to) + "  " + durText(c.seconds);
        if (!c.alias.empty()) line += "  alias: " + c.alias;
        if (c.encrypted) line += "  ENCRYPTED" + (c.encText.empty() ? std::string() : " (" + c.encText + ")");
        if (!c.extra.empty()) line += "  " + c.extra;
        log.add(line);
    }

    Rx& rxFor(int id) {
        auto& slot = rxs[id];
        if (!slot) {
            slot = std::make_unique<Rx>();
            slot->id = id;
            Rx* raw = slot.get();
            slot->core->onCall = [this, raw](const dv::Call& c) { onCall(raw, c); };
            slot->core->onEvent = [this, raw](const std::string& e) {   // mtx is held: called from process
                log.add((raw->id == AUDIO_ID ? std::string("audio  ") : mhz(raw->freqHz) + " MHz  ") + e);
            };
            // TETRA front end: demod -> sync. Later (decoder port) tsync.onBit feeds
            // TetraDecoder::rxSymbol(); for now we only track burst lock / quality.
            slot->tdemod.onSymbol = [raw](int q, double, std::complex<float>) { raw->tsync.pushQuadrant(q); };
            slot->tsync.onBurst = [raw](int) {
                raw->tetBursts = raw->tsync.burstCount();
                raw->tetVariant = raw->tsync.conventionVariant();
            };
            // resolved downlink bits -> ported tetra-kit decoder (network info, SDS, LIP)
            slot->tsync.onBit = [raw](uint8_t b) { raw->tcore->rxBit(b); };
        }
        return *slot;
    }

    // mtx is held
    void applySettings(Rx& r) {
        const ChSettings& s = settings[r.id];
        r.core->setWant(s.want);
        r.core->setMuteEncrypted(muteEnc);
        r.tcore->setContentEnabled(showTetraContent);   // TETRA speech only while content is shown
    }

    void publishVoice() {   // mtx is held
        std::vector<VoiceSrc> v;
        for (auto& kv : rxs) {
            const ChSettings& s = settings[kv.first];
            v.push_back({kv.first, kv.second->core, s.listen, kv.second->follow, kv.second->freqHz, kv.second->tcore});
        }
        std::lock_guard<std::mutex> l(voiceMtx);
        voiceList = std::move(v);
    }

    void openDetails(const CardView& cv) {
        std::vector<std::pair<std::string, std::string>> rows;
        const dv::Status& st = cv.st;
        rows.push_back({"Channel", cv.isAudio ? "audio input" : mhz(cv.freqHz) + " MHz" + (cv.follow ? "  (follows the tuner)" : "")});
        rows.push_back({"Decoding", std::string(dv::wantName(cv.set.want)) + (cv.set.want == dv::Want::Auto ? " (finds the system)" : " only")});
        rows.push_back({"System", st.proto.empty() ? "not found" : st.proto + (st.system.empty() ? "" : "  -  " + st.system)});
        if (!st.station.empty()) rows.push_back({"Station", st.station});
        for (const auto& f : st.fields) rows.push_back(f);
        if (!st.proto.empty()) {
            char b[64];
            std::snprintf(b, sizeof(b), "%d %%   sync quality %d", st.inLevel, st.quality);
            rows.push_back({"Input level", b});
        }
        for (int sl = 0; sl < st.nSlots && sl < 2; sl++) {
            const dv::SlotView& v = st.slot[sl];
            const std::string p = st.nSlots > 1 ? "TS" + std::to_string(sl + 1) + " " : "";
            rows.push_back({p + "activity", v.activity.empty() ? "-" : v.activity});
            if (!v.from.empty()) rows.push_back({p + "from", v.from});
            if (!v.to.empty()) rows.push_back({p + "to", v.to});
            if (!v.alias.empty()) rows.push_back({p + "alias", v.alias});
            if (v.encrypted) rows.push_back({p + "encrypted", v.encText.empty() ? "yes" : v.encText});
            if (!v.extra.empty()) rows.push_back({p + "info", v.extra});
        }
        char b[160];
        std::snprintf(b, sizeof(b), "%.1f dB   envelope %.2f   deviation %.0f Hz", cv.levelDb, cv.cls.envCv, cv.cls.devHz);
        rows.push_back({"Signal", b});
        if (!cv.verdict.empty() && !st.sync) rows.push_back({"Looks like", cv.verdict});
        if (cv.tetra) {
            char tb[96];
            std::snprintf(tb, sizeof(tb), "%s   %d bursts", cv.tetLocked ? "LOCKED" : "searching", cv.tetBursts);
            rows.push_back({"TETRA sync", tb});
            std::snprintf(tb, sizeof(tb), "%+.0f Hz residual   EVM %.0f deg%s", cv.tetCarrierHz, cv.tetEvmDeg,
                          cv.tetVariant >= 0 ? ("   map v" + std::to_string(cv.tetVariant)).c_str() : "");
            rows.push_back({"TETRA demod", tb});
            std::snprintf(tb, sizeof(tb), "%.0f kHz baseband (3 samples/symbol)", cv.tetSrcRate / 1000.0);
            rows.push_back({"TETRA input", tb});

            const tetra::Status& ts = cv.tstat;
            if (ts.net.valid) {
                std::snprintf(tb, sizeof(tb), "MCC %u   MNC %u", ts.net.mcc, ts.net.mnc);
                rows.push_back({"Network", tb});
                std::snprintf(tb, sizeof(tb), "colour code %u   location area %u", ts.net.colourCode, ts.net.locationArea);
                rows.push_back({"Cell", tb});
            }
            if (ts.reports) rows.push_back({"PDUs decoded", std::to_string(ts.reports)});
            const bool show = showTetraContent;
            if (!ts.sds.empty()) rows.push_back({"SDS received", std::to_string(ts.sds.size())});
            int n = 0;
            for (const auto& m : ts.sds) {
                if (n++ >= 6) break;
                // "2026-11-18T20:47:01Z" -> "20:47:01"
                rows.push_back({"SDS " + (m.time.size() >= 19 ? m.time.substr(11, 8) : m.time), sdsText(m, show)});
            }
            if (!ts.lip.empty()) rows.push_back({"LIP positions", std::to_string(ts.lip.size())});
            n = 0;
            for (const auto& p : ts.lip) {
                if (n++ >= 6) break;
                if (show) std::snprintf(tb, sizeof(tb), "%s  %.5f, %.5f  %s", p.from.empty() ? "?" : p.from.c_str(), p.lat, p.lon, p.extra.c_str());
                else std::snprintf(tb, sizeof(tb), "position (content hidden)");
                rows.push_back({"LIP", tb});
            }
            if (!ts.group.empty() || !ts.talker.empty()) {
                rows.push_back({"Call", show ? (ts.talker.empty() ? std::string("(idle)") : ts.talker) + " > " + ts.group +
                                                   (ts.callId.empty() ? "" : "   call id " + ts.callId)
                                             : std::string("(content hidden)")});
            }
            if (!tetra::speechAvailable()) rows.push_back({"Speech", "no codec in this build (scripts/tetracodec.sh)"});
            else if (!show) rows.push_back({"Speech", "off while content is hidden"});
            else if (ts.voiceFrames) {
                std::snprintf(tb, sizeof(tb), "time slot %d   %d / %d frames passed CRC%s", ts.voiceTn, ts.voiceGood,
                              ts.voiceFrames, ts.voice ? "   (speaking)" : "");
                rows.push_back({"Speech", tb});
            }
        }
        std::string note;
        if (!st.vocoder) note = "This build has no voice codec: the calls are shown, the speech stays silent.\n";
        if (cv.tetra)
            note += std::string("TETRA: ") + (tetra::available()
                        ? "the clear-text downlink is decoded: cell identity, call control, unencrypted SDS and LIP, "
                          "and - with the ETSI speech codec - speech of time slots sent without encryption. "
                          "TEA-encrypted traffic stays opaque. Speech, SDS / LIP content and subscriber numbers "
                          "appear only with \"TETRA content: shown\".\n"
                        : "this build has no TETRA decoder library: only the pi/4-DQPSK lock is shown.\n");
        note += "Encrypted calls are never decrypted: OrbitSDR only shows the signalling that every radio "
                "on the channel receives in the clear (addresses, colour code, the encryption algorithm id).";
        popup.show("Digital voice  " + (cv.isAudio ? std::string("audio input") : mhz(cv.freqHz) + " MHz"), rows, note);
    }

    void openCall(const dv::Call& c) {
        std::vector<std::pair<std::string, std::string>> rows;
        rows.push_back({"Time", c.time});
        rows.push_back({"Channel", c.freqHz ? mhz(c.freqHz) + " MHz" : "audio input"});
        rows.push_back({"System", c.proto});
        if (c.slot) rows.push_back({"Time slot", std::to_string(c.slot)});
        rows.push_back({"From", c.from.empty() ? "-" : c.from});
        rows.push_back({"To", c.to.empty() ? "-" : c.to});
        if (!c.alias.empty()) rows.push_back({"Alias", c.alias});
        rows.push_back({"Encrypted", c.encrypted ? (c.encText.empty() ? "yes" : c.encText) : "no"});
        if (!c.extra.empty()) rows.push_back({"Info", c.extra});
        rows.push_back({"Length", durText(c.seconds) + (c.open ? "  (still talking)" : "")});
        popup.show(c.proto + " call  " + (c.from.empty() ? "" : c.from) + (c.to.empty() ? "" : " > " + c.to), rows, "");
    }

public:
    ModuleLog log;
    std::string exportDir;

    static bool available() { return dv::available(); }

    DigitalVoiceDecoder() : SDRModule("Digital Voice") {
        channels.name = "DV";
        channels.maxChannels = MAX_CHANNELS;
        channels.defaultWidth = 12500.0;
        channels.listenMode = Mode::DIGITAL;
        channels.alwaysTuner = true;
        channels.resizable = true;
        channels.shape = [](DecoderChannel& c) {
            if (c.followVfo) { c.name = "DV"; return; }
            char b[24];
            std::snprintf(b, sizeof(b), "%.4f", c.freqHz / 1e6);
            c.name = b;
        };
    }

    void init(const ModuleContext& ctx) override {
        font = &ctx.font;
        popup.init(*font);
        btnAdd = std::make_shared<SdrButton>(132.0f, 22.0f, "+ Channel at tuner", *font);
        btnAdd->onClick = [this]() { channels.addChannel(0); };
        btnMix = std::make_shared<SdrButton>(122.0f, 22.0f, "Speaker: one at a time", *font);
        btnMix->onClick = [this]() {
            mixAll = !mixAll;
            btnMix->setText(mixAll ? "Speaker: mix all" : "Speaker: one at a time");
        };
        btnDuck = std::make_shared<SdrButton>(128.0f, 22.0f, "Mute main RX: on", *font);
        btnDuck->setActive(true);
        btnDuck->onClick = [this]() {
            duckMain = !duckMain;
            btnDuck->setActive(duckMain);
            btnDuck->setText(duckMain ? "Mute main RX: on" : "Mute main RX: off");
        };
        btnEnc = std::make_shared<SdrButton>(126.0f, 22.0f, "Mute encrypted: on", *font);
        btnEnc->setActive(true);
        btnEnc->onClick = [this]() {
            muteEnc = !muteEnc;
            btnEnc->setActive(muteEnc);
            btnEnc->setText(muteEnc ? "Mute encrypted: on" : "Mute encrypted: off");
            std::lock_guard<std::mutex> l(mtx);
            for (auto& kv : rxs) kv.second->core->setMuteEncrypted(muteEnc);
        };
        btnTetra = std::make_shared<SdrButton>(132.0f, 22.0f, "TETRA content: hidden", *font);
        btnTetra->onClick = [this]() {
            showTetraContent = !showTetraContent;
            btnTetra->setActive(showTetraContent);
            btnTetra->setText(showTetraContent ? "TETRA content: shown" : "TETRA content: hidden");
        };
        btnClear = std::make_shared<SdrButton>(60.0f, 22.0f, "Clear", *font);
        btnClear->onClick = [this]() {
            std::lock_guard<std::mutex> l(mtx);
            calls.clear(); totalCalls = 0;
            for (auto& kv : rxs) kv.second->calls = 0;
        };
        btnLog = std::make_shared<SdrButton>(60.0f, 22.0f, "Log", *font);
        btnLog->onClick = [this]() { log.openLogFile(); };
        btnExport = std::make_shared<SdrButton>(60.0f, 22.0f, "Export", *font);
        btnExport->onClick = [this]() { log.exportSession(exportDir, "DigitalVoice"); };
    }

    void reset() override {
        std::lock_guard<std::mutex> l(mtx);
        rxs.clear();   // rebuilt on the next block
        publishVoice();
    }

    float panelHeight(float windowHeight) const override {
        return std::clamp(windowHeight * 0.40f, 280.0f, 520.0f);
    }

    void setRadioFrequency(long long hz) override { std::lock_guard<std::mutex> l(mtx); centerHz = hz; }
    void setAudioInput(bool audio) override { audioInput = audio; }
    void setTunerBandwidth(double hz) override { std::lock_guard<std::mutex> l(mtx); tunerBw = hz; }
    ChannelSet* channelSet() override { return &channels; }
    bool modalOpen() const override { return popup.isOpen(); }
    void drawModal(sf::RenderWindow& win) override { popup.draw(win); }

    // Audio input: the discriminator audio itself (48 kHz)
    void processAudio(const std::vector<float>& audio) override {
        if (!enabled || !audioInput || audio.empty()) return;
        std::lock_guard<std::mutex> l(mtx);
        const bool fresh = rxs.find(AUDIO_ID) == rxs.end();
        Rx& r = rxFor(AUDIO_ID);
        applySettings(r);
        r.disc.resize(audio.size());
        double p = 0.0;
        for (size_t i = 0; i < audio.size(); i++) {
            r.disc[i] = (short)std::clamp(audio[i] * 32768.0f, -32767.0f, 32767.0f);
            p += (double)audio[i] * audio[i];
        }
        r.levelDb = 10.0 * std::log10(p / audio.size() + 1e-20);
        r.core->process(r.disc.data(), r.disc.size(), nullptr);
        if (fresh) publishVoice();
    }

    void processIQ(const std::vector<std::complex<double>>& iq, double sampleRate, double tunedOffset) override {
        if (!enabled || iq.empty() || sampleRate <= 0.0) return;
        std::lock_guard<std::mutex> l(mtx);
        if (audioInput) {
            bool changed = false;
            for (auto it = rxs.begin(); it != rxs.end();) {
                if (it->first != AUDIO_ID) { it = rxs.erase(it); changed = true; } else ++it;
            }
            if (changed) publishVoice();
            info = "Input signal: Audio (decoding the discriminator audio)";
            return;
        }
        if (rxs.count(AUDIO_ID)) rxs.erase(AUDIO_ID);
        if (sampleRate < 48000.0) { info = "Sample rate too low (48 kHz or more needed)"; return; }

        const auto rc = channels.update(centerHz, sampleRate, tunedOffset, tunerBw);
        bool changed = false;
        if (sampleRate != lastRate) { rxs.clear(); lastRate = sampleRate; changed = true; }
        for (auto it = rxs.begin(); it != rxs.end();) {
            bool keep = false;
            for (const auto& c : rc) if (c.id == it->first) keep = true;
            if (!keep) { it = rxs.erase(it); changed = true; } else ++it;
        }

        // one shared first stage for all channels in band (wide enough for the classifier)
        double lo = 1e18, hi = -1e18;
        for (const auto& c : rc) if (c.inBand) { lo = std::min(lo, c.offsetHz - 16000.0); hi = std::max(hi, c.offsetHz + 16000.0); }
        const C* gin = iq.data();
        size_t gn = iq.size();
        double grate = sampleRate, gcentre = 0.0;
        if (hi > lo) {
            group.configure(sampleRate, (lo + hi) / 2.0, hi - lo);
            gin = group.process(iq.data(), iq.size(), gn);
            grate = group.outputRate();
            gcentre = group.centreHz();
        }

        int inBand = 0;
        for (const auto& c : rc) {
            if (!rxs.count(c.id)) changed = true;
            Rx& r = rxFor(c.id);
            r.freqHz = c.freqHz;
            r.follow = c.ch.followVfo;
            r.widthHz = c.ch.widthHz;
            if (!c.inBand) continue;
            inBand++;
            applySettings(r);
            if (grate < 48000.0) continue;
            const double off = c.offsetHz - gcentre;
            r.nc.configure(grate, off, 48000.0, std::max(2000.0, r.widthHz / 2.0), 1500.0);
            r.bb.clear();
            r.nc.process(gin, gn, r.bb);
            r.disc.resize(r.bb.size());
            double p = 0.0;
            for (size_t i = 0; i < r.bb.size(); i++) {
                const std::complex<float> v = r.bb[i];
                r.disc[i] = discToShort(std::arg(v * std::conj(r.prev)));
                r.prev = v;
                p += std::norm(v);
            }
            if (!r.bb.empty()) r.levelDb = 10.0 * std::log10(p / r.bb.size() + 1e-20);
            r.core->process(r.disc.data(), r.disc.size(), nullptr);
            // what is it, when no decoder understands it
            if (r.core->status().sinceSync > 1.0) {
                r.wide.configure(grate, off, 48000.0, 12500.0, 2500.0, 1.0);
                r.bbw.clear();
                r.wide.process(gin, gn, r.bbw);
                r.cls.process(r.bbw.data(), r.bbw.size());
            } else if (r.cls.windows) r.cls.reset();

            // TETRA pi/4-DQPSK demod: only while the classifier calls it TETRA and the
            // group rate is high enough for 4 samples/symbol. Feeds the burst synchroniser.
            if (r.cls.windows && r.cls.tetra && grate >= TETRA_MIN_SRC) {
                if (!r.tetReady) { r.tdemod.configure(TETRA_RATE); r.tsync.reset(); r.tetReady = true; }
                r.tet.configure(grate, off, TETRA_RATE, 12700.0, 3000.0, 1.0);
                r.bbt.clear();
                r.tet.process(gin, gn, r.bbt);
                r.tdemod.process(r.bbt.data(), r.bbt.size());
                r.tetCarrierHz = r.tdemod.carrierHz;
                r.tetEvmDeg = r.tdemod.evmAcc * 180.0 / 3.14159265358979323846;
                r.tetSrcRate = grate;
            } else if (r.tetReady) {
                r.tetReady = false; r.tsync.reset(); r.tet.reset(); r.tcore->reset();
                r.tetBursts = 0; r.tetVariant = -1; r.tetSrcRate = 0.0;
            }
        }
        if (changed) publishVoice();
        info = std::to_string(rc.size()) + " channel(s), " + std::to_string(inBand) + " in band";
    }

    // DSP thread: the speech of the channels for the speaker
    bool mixVoice(float* out, size_t n, bool& duck) override {
        if (!enabled) return false;
        std::vector<VoiceSrc> list;
        {
            std::lock_guard<std::mutex> l(voiceMtx);
            list = voiceList;
        }
        // the channel on the tuner repeats a pinned channel at the same frequency: not twice
        auto duplicate = [&](const VoiceSrc& v) {
            if (!v.follow) return false;
            for (const auto& o : list) if (!o.follow && std::llabs(o.freqHz - v.freqHz) < 3000) return true;
            return false;
        };
        bool any = false;
        int spoke = 0;
        if (mixAll) {
            for (auto& v : list) {
                if (!v.listen || duplicate(v)) { v.discardVoice(); continue; }
                if (v.pull(out, n)) { any = true; if (!spoke) spoke = v.id; }
            }
        } else {
            bool holderThere = false;
            for (auto& v : list) if (v.id == holder && v.listen) holderThere = true;
            if (!holderThere) holder = 0;
            if (holder) {
                for (auto& v : list) if (v.id == holder) {
                    if (v.pull(out, n)) { any = true; holderIdle = 0; spoke = holder; }
                    else holderIdle += n;
                }
                if (holderIdle > 48000 / 2) holder = 0;   // half a second of silence: the next one may talk
            }
            if (!holder) {
                // the first channel with speech waiting gets the speaker (pinned channels first)
                for (int pass = 0; pass < 2 && !holder; pass++)
                    for (auto& v : list) {
                        if (!v.listen || duplicate(v) || (pass == 0) == v.follow) continue;
                        if (v.hasVoice()) { holder = v.id; holderIdle = 0; break; }
                    }
                if (holder) for (auto& v : list) if (v.id == holder && v.pull(out, n)) { any = true; spoke = holder; }
            }
            for (auto& v : list) if (v.id != holder) v.discardVoice();
        }
        speakingId = spoke;
        duck = any && duckMain;
        return any;
    }

    // ---------------------------------------------------------------
    bool handleEvent(const sf::Event& ev, const sf::RenderWindow& win) override {
        if (!enabled) return false;
        if (popup.handleEvent(ev, win)) return true;
        for (auto* b : {btnAdd.get(), btnMix.get(), btnDuck.get(), btnEnc.get(), btnTetra.get(), btnClear.get(), btnLog.get(), btnExport.get()})
            if (b && b->handleEvent(ev, win)) return true;
        const sf::Vector2f m = win.mapPixelToCoords(sf::Mouse::getPosition(win));
        if (const auto* sc = ev.getIf<sf::Event::MouseWheelScrolled>()) {
            if (!panelRect.contains(m)) return false;
            if (tableRect.contains(m)) tableScroll = std::max(0.0f, tableScroll - sc->delta * 3.0f);
            else if (cardsRect.contains(m)) cardScroll = std::max(0.0f, cardScroll - sc->delta * 40.0f);
            return true;
        }
        if (const auto* mb = ev.getIf<sf::Event::MouseButtonPressed>()) {
            if (!panelRect.contains(m)) return false;
            if (mb->button != sf::Mouse::Button::Left) return true;
            for (const auto& h : cardHits) {
                if (!h.card.contains(m)) continue;
                if (h.close.size.x > 0 && h.close.contains(m)) { channels.removeChannel(h.id); return true; }
                if (h.want.contains(m)) {
                    std::lock_guard<std::mutex> l(mtx);
                    settings[h.id].want = nextWant(settings[h.id].want);
                    return true;
                }
                if (h.listen.contains(m)) {
                    std::lock_guard<std::mutex> l(mtx);
                    settings[h.id].listen = !settings[h.id].listen;
                    publishVoice();
                    return true;
                }
                for (const auto& cv : lastCards) if (cv.id == h.id) { openDetails(cv); break; }
                return true;
            }
            if (tableRect.contains(m) && m.y >= tableBodyY) {
                const int row = (int)((m.y - tableBodyY) / rowH);
                const int idx = tableFirst + row;
                dv::Call c;
                bool found = false;
                {
                    std::lock_guard<std::mutex> l(mtx);
                    if (row >= 0 && row < tableVisible && idx < (int)calls.size()) { c = calls[idx]; found = true; }
                }
                if (found) openCall(c);
            }
            return true;
        }
        return false;
    }

    void draw(sf::RenderWindow& win, float x, float y, float w, float h) override {
        if (!enabled || !font) return;
        panelRect = sf::FloatRect({x, y}, {w, h});

        // --- snapshot ---
        std::vector<CardView> cards;
        std::vector<dv::Call> list;
        std::string inf;
        int count = 0;
        {
            std::lock_guard<std::mutex> l(mtx);
            for (auto& kv : rxs) {
                CardView cv;
                cv.id = kv.first;
                cv.isAudio = kv.first == AUDIO_ID;
                cv.freqHz = kv.second->freqHz;
                cv.follow = kv.second->follow;
                cv.levelDb = kv.second->levelDb;
                cv.st = kv.second->core->status();
                cv.cls = kv.second->cls;
                cv.verdict = kv.second->cls.windows ? kv.second->cls.verdict : "";
                cv.tetra = kv.second->cls.windows && kv.second->cls.tetra;
                cv.tetLocked = kv.second->tsync.locked();
                cv.tetBursts = kv.second->tetBursts;
                cv.tetCarrierHz = kv.second->tetCarrierHz;
                cv.tetEvmDeg = kv.second->tetEvmDeg;
                cv.tetVariant = kv.second->tetVariant;
                cv.tetSrcRate = kv.second->tetSrcRate;
                if (cv.tetra) cv.tstat = kv.second->tcore->status();
                cv.set = settings[kv.first];
                cv.calls = kv.second->calls;
                cards.push_back(cv);
            }
            const int first = (int)tableScroll;
            for (int i = first; i < (int)calls.size() && i < first + 80; i++) list.push_back(calls[i]);
            tableFirst = first;
            count = (int)calls.size();
            inf = info;
        }
        // the channel on the tuner first, then by frequency
        std::sort(cards.begin(), cards.end(), [](const CardView& a, const CardView& b) {
            if (a.follow != b.follow) return a.follow;
            return a.freqHz < b.freqHz;
        });
        lastCards = cards;
        const int speaking = speakingId;

        sf::RectangleShape bg({w, h});
        bg.setPosition({x, y});
        bg.setFillColor(sf::Color(20, 22, 27, 245));
        bg.setOutlineColor(Theme::Accent);
        bg.setOutlineThickness(1.0f);
        win.draw(bg);

        const float pad = 8.0f;
        // --- top bar ---
        sf::Text title = text("DIGITAL VOICE", 28, Theme::Accent);
        title.setStyle(sf::Text::Bold);
        title.setPosition({x + pad, y + pad});
        win.draw(title);
        sf::Text sub = text("DMR  dPMR  D-STAR  YSF  NXDN     " + inf + "     " + std::to_string(count) + " calls", 20, sf::Color(150, 150, 160));
        sub.setPosition({x + pad + 112.0f, y + pad + 3.0f});
        win.draw(sub);
        float bx = x + w - pad;
        for (auto* b : {btnExport.get(), btnLog.get(), btnClear.get(), btnTetra.get(), btnEnc.get(), btnDuck.get(), btnMix.get(), btnAdd.get()}) {
            bx -= b->shape.getSize().x;
            b->setPosition(bx, y + pad - 2.0f);
            bx -= 4.0f;
        }
        const bool narrowTop = bx < x + pad + 112.0f + 260.0f;   // the buttons cover the subtitle: they get a row
        float topY = y + pad + 26.0f;
        if (narrowTop) {
            float bx2 = x + w - pad;
            for (auto* b : {btnExport.get(), btnLog.get(), btnClear.get(), btnTetra.get(), btnEnc.get(), btnDuck.get(), btnMix.get(), btnAdd.get()}) {
                bx2 -= b->shape.getSize().x;
                b->setPosition(bx2, y + pad + 20.0f);
                bx2 -= 4.0f;
            }
            topY += 22.0f;
        }
        for (auto* b : {btnAdd.get(), btnMix.get(), btnDuck.get(), btnEnc.get(), btnTetra.get(), btnClear.get(), btnLog.get(), btnExport.get()}) b->draw(win);
        if (!dv::vocoderAvailable()) {
            sf::Text nv = text("This build has no voice codec: calls are shown without speech", 20, sf::Color(230, 150, 120));
            nv.setPosition({x + pad, topY - 4.0f});
            win.draw(nv);
            topY += 12.0f;
        }

        // --- cards (left) and calls (right) ---
        const float bodyH = y + h - pad - topY;
        const float tableW = std::clamp(w * 0.42f, 300.0f, 620.0f);
        cardsRect = sf::FloatRect({x + pad, topY}, {w - tableW - 3 * pad, bodyH});
        tableRect = sf::FloatRect({x + w - pad - tableW, topY}, {tableW, bodyH});
        drawCards(win, cards, speaking);
        drawCalls(win, list, count);
    }

private:
    void drawCards(sf::RenderWindow& win, const std::vector<CardView>& cards, int speaking) {
        cardHits.clear();
        const float cw = 312.0f, ch = 112.0f, gap = 6.0f;
        const int cols = std::max(1, (int)((cardsRect.size.x + gap) / (cw + gap)));
        const float realW = (cardsRect.size.x - gap * (cols - 1)) / cols;
        const int rows = ((int)cards.size() + cols - 1) / cols;
        const float maxScroll = std::max(0.0f, rows * (ch + gap) - gap - cardsRect.size.y);
        cardScroll = std::min(cardScroll, maxScroll);

        if (cards.empty()) {
            sf::Text e = text("Waiting for the first block of samples...", 22, sf::Color(120, 120, 130));
            e.setPosition({cardsRect.position.x, cardsRect.position.y + 4.0f});
            win.draw(e);
            return;
        }
        for (size_t i = 0; i < cards.size(); i++) {
            const CardView& cv = cards[i];
            const float cx = cardsRect.position.x + (i % cols) * (realW + gap);
            const float cy = cardsRect.position.y + (i / cols) * (ch + gap) - cardScroll;
            if (cy + ch < cardsRect.position.y || cy > cardsRect.position.y + cardsRect.size.y) continue;
            if (cy < cardsRect.position.y - 1.0f || cy + ch > cardsRect.position.y + cardsRect.size.y + 1.0f) continue;   // only whole cards
            drawCard(win, cv, cx, cy, realW, ch, speaking == cv.id);
        }
        if (maxScroll > 0) {
            const float trackH = cardsRect.size.y;
            const float thumbH = std::max(20.0f, trackH * cardsRect.size.y / (cardsRect.size.y + maxScroll));
            sf::RectangleShape thumb({4.0f, thumbH});
            thumb.setPosition({cardsRect.position.x + cardsRect.size.x + 2.0f, cardsRect.position.y + (trackH - thumbH) * (cardScroll / maxScroll)});
            thumb.setFillColor(sf::Color(200, 200, 200, 140));
            win.draw(thumb);
        }
    }

    void drawCard(sf::RenderWindow& win, const CardView& cv, float x, float y, float w, float h, bool speaking) {
        const dv::Status& st = cv.st;
        bool voice = false, enc = false;
        for (int sl = 0; sl < st.nSlots && sl < 2; sl++) {
            voice = voice || st.slot[sl].voice;
            enc = enc || (st.slot[sl].voice && st.slot[sl].encrypted);
        }
        const bool found = !st.proto.empty() && st.sinceSync < 3.0;
        const sf::Color border = enc ? sf::Color(255, 120, 90) : voice ? sf::Color(90, 220, 120)
                               : found ? Theme::Accent : sf::Color(70, 70, 80);
        sf::RectangleShape box({w, h});
        box.setPosition({x, y});
        box.setFillColor(speaking ? sf::Color(30, 48, 36) : sf::Color(28, 30, 36));
        box.setOutlineColor(border);
        box.setOutlineThickness(voice ? 2.0f : 1.0f);
        win.draw(box);

        CardHit hit;
        hit.id = cv.id;
        hit.card = sf::FloatRect({x, y}, {w, h});
        // header: frequency, the system, level, the buttons
        const std::string head = cv.isAudio ? "Audio input" : (cv.follow ? "Tuner  " : "") + mhz(cv.freqHz) + " MHz";
        sf::Text t = text(head, 24, sf::Color(235, 235, 240));
        t.setStyle(sf::Text::Bold);
        t.setPosition({x + 6.0f, y + 4.0f});
        win.draw(t);
        float hx = x + 6.0f + t.getGlobalBounds().size.x + 10.0f;
        const bool tetraHead = !found && cv.tetra;
        const std::string sys = found ? st.proto : tetraHead ? "TETRA" : (st.everSync ? "lost" : "searching");
        sf::Text ts = text(sys, 24, found ? (enc ? sf::Color(255, 140, 110) : sf::Color(120, 220, 255))
                                    : tetraHead ? (cv.tetLocked ? sf::Color(120, 220, 140) : sf::Color(255, 190, 100))
                                    : sf::Color(130, 130, 140));
        ts.setStyle(sf::Text::Bold);
        ts.setPosition({hx, y + 4.0f});
        win.draw(ts);
        hx += ts.getGlobalBounds().size.x + 10.0f;
        char lv[32];
        std::snprintf(lv, sizeof(lv), "%.0f dB", cv.levelDb);
        sf::Text tl = text(lv, 20, sf::Color(140, 140, 150));
        tl.setPosition({hx, y + 6.0f});
        win.draw(tl);

        // buttons on the card: system choice, speaker, remove
        float bx = x + w - 6.0f;
        auto smallBtn = [&](const std::string& label, float bw, bool on, sf::FloatRect& r) {
            bx -= bw;
            r = sf::FloatRect({bx, y + 4.0f}, {bw, 16.0f});
            sf::RectangleShape b({bw, 16.0f});
            b.setPosition({bx, y + 4.0f});
            b.setFillColor(on ? Theme::AccentDim : sf::Color(40, 42, 50));
            b.setOutlineColor(sf::Color(80, 80, 90));
            b.setOutlineThickness(1.0f);
            win.draw(b);
            sf::Text bt = text(label, 18, sf::Color(230, 230, 235));
            bt.setPosition({bx + (bw - bt.getGlobalBounds().size.x) / 2.0f, y + 5.0f});
            win.draw(bt);
            bx -= 4.0f;
        };
        if (!cv.follow && !cv.isAudio) smallBtn("x", 16.0f, false, hit.close);
        smallBtn(cv.set.listen ? "Spk on" : "Spk off", 44.0f, cv.set.listen, hit.listen);
        smallBtn(dv::wantName(cv.set.want), 46.0f, cv.set.want != dv::Want::Auto, hit.want);
        cardHits.push_back(hit);

        // body
        const size_t maxChars = (size_t)std::max(10.0f, (w - 12.0f) / 5.6f);
        float ly = y + 26.0f;
        auto line = [&](const std::string& s, sf::Color c) {
            sf::Text tx = text(fit(s, maxChars), 21, c);
            tx.setPosition({x + 6.0f, ly});
            win.draw(tx);
            ly += 16.0f;
        };
        if (!found) {
            if (!cv.tetra) {
                if (!cv.verdict.empty() && cv.verdict != "No signal") line(cv.verdict, sf::Color(200, 200, 205));
                else line(st.everSync ? "Signal lost" : "No digital voice found yet", sf::Color(130, 130, 140));
                line(cv.set.want == dv::Want::Auto ? "Looking for DMR, dPMR, D-STAR, YSF, NXDN" : std::string("Looking for ") + dv::wantName(cv.set.want) + " only",
                     sf::Color(110, 110, 120));
            }
            if (cv.tetra) {
                char tb[96];
                if (cv.tetLocked) {
                    std::snprintf(tb, sizeof(tb), "TETRA sync LOCKED   %d bursts   %+.0f Hz   EVM %.0f deg",
                                  cv.tetBursts, cv.tetCarrierHz, cv.tetEvmDeg);
                    line(tb, sf::Color(120, 220, 140));
                } else {
                    std::snprintf(tb, sizeof(tb), "TETRA demod: searching sync   %+.0f Hz   EVM %.0f deg",
                                  cv.tetCarrierHz, cv.tetEvmDeg);
                    line(tb, sf::Color(255, 190, 100));
                }
                const tetra::Status& ts = cv.tstat;
                if (ts.net.valid) {
                    std::snprintf(tb, sizeof(tb), "Network  MCC %u  MNC %u  CC %u  LA %u",
                                  ts.net.mcc, ts.net.mnc, ts.net.colourCode, ts.net.locationArea);
                    line(tb, sf::Color(130, 210, 255));
                }
                const bool show = showTetraContent;
                if (ts.voice || !ts.talker.empty()) {
                    std::string s = ts.voice ? "VOICE TS" + std::to_string(ts.voiceTn) : std::string("CALL");
                    s += show ? "  " + (ts.talker.empty() ? std::string("?") : ts.talker) + " > " + (ts.group.empty() ? std::string("?") : ts.group)
                              : std::string("  (content hidden)");
                    if (ts.voice && ts.voiceFrames)
                        s += "  CRC " + std::to_string(100 * ts.voiceGood / ts.voiceFrames) + "%";
                    else if (!tetra::speechAvailable()) s += "  (no speech codec in this build)";
                    line(s, ts.voice ? sf::Color(140, 235, 150) : sf::Color(200, 200, 205));
                }
                if (!ts.sds.empty())
                    line("SDS (" + std::to_string(ts.sds.size()) + ")  " + sdsText(ts.sds.front(), show), sf::Color(220, 210, 140));
                if (!ts.lip.empty()) {
                    if (show) std::snprintf(tb, sizeof(tb), "GPS (%zu)  %.5f, %.5f  %s", ts.lip.size(), ts.lip.front().lat,
                                            ts.lip.front().lon, ts.lip.front().from.c_str());
                    else std::snprintf(tb, sizeof(tb), "GPS (%zu)  positions (content hidden)", ts.lip.size());
                    line(tb, sf::Color(160, 220, 160));
                }
                if (ts.reports && !ts.net.valid && ts.sds.empty() && ts.lip.empty())
                    line(std::to_string(ts.reports) + " PDUs decoded (awaiting SYSINFO)", sf::Color(150, 150, 160));
            }
            if (cv.calls) line(std::to_string(cv.calls) + " calls heard on this channel", sf::Color(110, 110, 120));
            return;
        }
        std::string l1;
        for (const auto& f : st.fields) {
            if (!l1.empty()) l1 += "   ";
            l1 += (f.first == "Color code" || f.first == "Colour code") ? "CC " + f.second : f.first + " " + f.second;
        }
        if (!st.station.empty()) l1 += (l1.empty() ? "" : "   ") + st.station;
        if (!st.system.empty() && st.nSlots > 1) l1 += "   " + st.system;
        line(l1.empty() ? st.system : l1, sf::Color(170, 170, 180));
        for (int sl = 0; sl < st.nSlots && sl < 2; sl++) {
            const dv::SlotView& v = st.slot[sl];
            std::string s = st.nSlots > 1 ? "TS" + std::to_string(sl + 1) + "  " : "";
            if (v.voice) s += v.encrypted ? "ENCRYPTED  " : "VOICE  ";
            else s += (v.activity.empty() ? "-" : v.activity) + "   ";
            if (!v.from.empty() || !v.to.empty()) s += (v.voice ? "" : "last ") + v.from + (v.to.empty() ? "" : " > " + v.to);
            if (v.voice && v.voiceSec >= 1.0) s += "   " + durText(v.voiceSec);
            if (!v.alias.empty()) s += "   \"" + v.alias + "\"";
            const sf::Color c = v.voice ? (v.encrypted ? sf::Color(255, 140, 110) : sf::Color(140, 235, 150)) : sf::Color(200, 200, 205);
            line(s, c);
        }
        // encryption / extras of the slot that talks (else of any slot)
        std::string ex;
        for (int pass = 0; pass < 2 && ex.empty(); pass++)
            for (int sl = 0; sl < st.nSlots && sl < 2 && ex.empty(); sl++) {
                const dv::SlotView& v = st.slot[sl];
                if (pass == 0 && !v.voice) continue;
                if (v.encrypted) ex = "Encrypted: " + (v.encText.empty() ? std::string("yes") : v.encText);
                else if (!v.extra.empty()) ex = v.extra;
            }
        if (!ex.empty()) line(ex, ex.rfind("Encrypted", 0) == 0 ? sf::Color(255, 160, 120) : sf::Color(220, 200, 140));
        if (st.nSlots == 1 && !st.system.empty()) line(st.system, sf::Color(130, 130, 140));
        if (!st.vocoder && voice) line("(no voice codec in this build)", sf::Color(230, 150, 120));
    }

    void drawCalls(sf::RenderWindow& win, const std::vector<dv::Call>& list, int count) {
        struct Col { const char* name; float w; };
        const float fixedW = 54 + 64 + 26 + 46 + 62 + 66 + 36;
        const float restW = std::max(40.0f, tableRect.size.x - fixedW);
        const Col cols[] = { {"Time", 54}, {"MHz", 64}, {"TS", 26}, {"Sys", 46}, {"From", 62}, {"To", 66}, {"Len", 36}, {"Info", restW} };
        float cx = tableRect.position.x;
        for (const Col& c : cols) {
            sf::Text th = text(c.name, 22, Theme::Accent);
            th.setStyle(sf::Text::Bold);
            th.setPosition({cx, tableRect.position.y});
            win.draw(th);
            cx += c.w;
        }
        tableBodyY = tableRect.position.y + rowH + 4.0f;
        const int visibleRows = std::max(1, (int)((tableRect.size.y - rowH - 4.0f) / rowH));
        tableVisible = visibleRows;
        tableScroll = std::min(tableScroll, (float)std::max(0, count - visibleRows));
        if (list.empty()) {
            sf::Text e = text("No calls yet. Click a card for all details.", 22, sf::Color(110, 110, 120));
            e.setPosition({tableRect.position.x, tableBodyY + 4.0f});
            win.draw(e);
        }
        const float charW = 5.6f;
        for (int r = 0; r < visibleRows && r < (int)list.size(); r++) {
            const dv::Call& c = list[r];
            const float ry = tableBodyY + r * rowH;
            if (r % 2 == 0) {
                sf::RectangleShape stripe({tableRect.size.x, rowH});
                stripe.setPosition({tableRect.position.x, ry});
                stripe.setFillColor(sf::Color(255, 255, 255, 8));
                win.draw(stripe);
            }
            std::string infoTxt;
            if (c.encrypted) infoTxt = "ENCRYPTED" + (c.encText.empty() ? std::string() : ": " + c.encText);
            if (!c.alias.empty()) infoTxt = (infoTxt.empty() ? "" : infoTxt + "  ") + "\"" + c.alias + "\"";
            if (!c.extra.empty()) infoTxt = (infoTxt.empty() ? "" : infoTxt + "  ") + c.extra;
            const std::string cells[8] = {
                c.time, c.freqHz ? mhz(c.freqHz) : "audio", c.slot ? std::to_string(c.slot) : "-", c.proto,
                c.from.empty() ? "?" : c.from, c.to.empty() ? "?" : c.to, c.open ? "live" : durText(c.seconds), infoTxt };
            float px = tableRect.position.x;
            for (int k = 0; k < 8; k++) {
                const size_t maxChars = (size_t)std::max(3.0f, (cols[k].w - 6.0f) / charW);
                sf::Color col = sf::Color(225, 225, 230);
                if (c.open) col = sf::Color(140, 235, 150);
                if (c.encrypted && (k == 7 || c.open)) col = sf::Color(255, 140, 110);
                sf::Text td = text(fit(cells[k], maxChars), 22, col);
                td.setPosition({px, ry});
                win.draw(td);
                px += cols[k].w;
            }
        }
        if (count > visibleRows) {
            const float trackH = tableRect.size.y - rowH - 4.0f;
            const float thumbH = std::max(16.0f, trackH * visibleRows / (float)count);
            const float maxFirst = (float)(count - visibleRows);
            sf::RectangleShape thumb({4.0f, thumbH});
            thumb.setPosition({tableRect.position.x + tableRect.size.x - 6.0f, tableBodyY + (trackH - thumbH) * (tableFirst / maxFirst)});
            thumb.setFillColor(sf::Color(200, 200, 200, 140));
            win.draw(thumb);
        }
    }
};
