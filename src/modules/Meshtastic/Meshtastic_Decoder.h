#pragma once

#include "../Core/SDRModule.h"
#include "../Core/ScrollDrag.h"
#include "../Core/PositionBus.h"
#include "../Core/ModuleLog.h"
#include "../Core/EmojiText.h"
#include "../Core/WorkPool.h"
#include "../Analyzer/Pipeline.h"   // pl::LoraRx (the LoRa physical layer)
#include "MeshtasticCore.h"
#include "MeshCoreProto.h"
#include <SFML/Graphics.hpp>
#include <mutex>
#include <deque>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <algorithm>
#include <atomic>
#include <functional>

// =====================================================================
//  LoRa mesh decoder: Meshtastic and MeshCore nodes (EU 869.525 MHz for
//  Meshtastic, 869.618 MHz for MeshCore; 433 / 915 MHz ...).
//
//  Each standard has its own channel (a yellow band) and its own LoRa
//  receiver (pl::LoraRx of the pipelines: preamble search, SF / BW search,
//  header, CRC). The two receivers run at the same time on the WorkPool
//  (the second one on another core). A packet whose sync word belongs to
//  the other standard goes to that one's decoder (a MeshCore mesh on the
//  old 869.525 MHz / 250 kHz settings shares the Meshtastic channel).
//   - Meshtastic: mesh::Decoder (MeshtasticCore.h): header, AES-CTR with
//     the default key, protobufs (names, hardware, positions, texts ...).
//   - MeshCore: mcore::Decoder (MeshCoreProto.h): adverts (name, type,
//     position), texts of the Public channel; direct messages are
//     encrypted per pair of nodes.
//  The panel's check boxes switch either standard off (its channel and
//  receiver go). The table lists the nodes of both, with a badge (short
//  name or emoji on the node's colour); the side column the latest
//  packets. Positions go to the map.
// =====================================================================

class MeshtasticDecoder : public SDRModule {
    ScrollDrag sbTable;

    enum Std { MT = 0, MC = 1 };
    // MeshCore's settings in use (heard: EU/UK narrow 869.618 MHz, 62.5 kHz, SF8, CR 4/8)
    static std::vector<long long> meshCoreFrequencies() { return {869618000}; }
    static constexpr double MC_BW = 62500.0;

    // where the Meshtastic channel goes: the default slot of the preset in every region (auto:
    // LongFast and MediumFast, the presets meshes use; in EU_868 both are 869.525 MHz). Other
    // slots are not offered, or the channel would jump to the one nearest the tuner (LongSlow's
    // 869.4625 lies next to LongFast's).
    static std::vector<long long> meshtasticFrequencies(int sel) {
        std::vector<long long> v;
        auto add = [&](long long f) { if (f > 0 && std::find(v.begin(), v.end(), f) == v.end()) v.push_back(f); };
        for (const auto& g : mesh::regions()) {
            if (sel == 0) { add(mesh::defaultFrequency(g, mesh::presets()[0])); add(mesh::defaultFrequency(g, mesh::presets()[1])); }
            else add(mesh::defaultFrequency(g, mesh::presets()[sel - 1]));
        }
        return v;
    }
    static bool isMeshCoreFreq(long long f) {
        for (long long m : meshCoreFrequencies()) if (std::llabs(m - f) < 3000) return true;
        return false;
    }

    // one standard's receiver: its channel shifted to 0 Hz -> LoRa receiver
    struct Link {
        pl::LoraRx rx;
        pl::Stream in;
        std::complex<double> nco {1.0, 0.0}, ncoStep {1.0, 0.0};
        double ncoFor = 1e18, ncoRate = 0.0;
        bool active = false;
        double offsetHz = 0.0;
        void run(const std::vector<std::complex<double>>& iq, double fs) {
            if (offsetHz != ncoFor || fs != ncoRate) {
                ncoFor = offsetHz; ncoRate = fs;
                ncoStep = std::polar(1.0, -2.0 * M_PI * ncoFor / fs);
            }
            in.type = pl::PT::IQ;
            in.rate = fs;
            in.iq.resize(iq.size());
            for (size_t i = 0; i < iq.size(); i++) {
                const std::complex<double> v = iq[i] * nco;
                nco *= ncoStep;
                in.iq[i] = pl::CF((float)v.real(), (float)v.imag());
            }
            nco /= std::abs(nco);
            rx.process({&in});
        }
    };

    std::mutex mtx;
    mesh::Decoder dec;
    mcore::Decoder mcDec;
    Link link[2];
    bool useStd[2] = {true, true};
    bool stdChanged = true;
    int presetSel = 0;              // Meshtastic preset: 0 auto, 1.. = mesh::presets()[i - 1]
    long long centerHz = 0;
    std::string channelInfo = "Waiting for signal...";
    std::atomic<bool> audioInput {false};
    ChannelSet channels;
    std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();
    struct Line { std::string s; sf::Color c; };
    std::deque<Line> recent;
    long long otherLora = 0;

    const sf::Font* font = nullptr;
    sf::FloatRect panelRect, tableRect;
    float scrollRows = 0.0f;
    std::shared_ptr<SdrButton> btnClear, btnMap, btnPreset, btnTune;
    bool euInBand = true;           // Meshtastic's / MeshCore's EU frequencies are in the band
    std::shared_ptr<Checkbox> chkStd[2];

    // drops the last UTF-8 character
    static void popChar(std::string& s) {
        unsigned char c;
        do { c = (unsigned char)s.back(); s.pop_back(); } while (!s.empty() && (c & 0xC0) == 0x80);
    }

    double now() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count(); }

    sf::Text text(const std::string& s, unsigned size, sf::Color c) const {
        sf::Text t(*font, sf::String::fromUtf8(s.begin(), s.end()), size);
        t.setScale({0.5f, 0.5f});
        t.setFillColor(c);
        return t;
    }

    std::string presetLabel() const {
        if (presetSel == 0) return "Preset: auto (all)";
        return std::string("Preset: ") + mesh::presets()[presetSel - 1].name;
    }
    double meshtasticWidth() const { return presetSel == 0 ? 300000.0 : mesh::presets()[presetSel - 1].bwHz * 1.2; }
    static void setParam(pl::Block& b, const char* k, double v) { for (auto& p : b.params) if (p.key == k) p.v = v; b.paramVersion++; }
    static int bwChoice(double hz) {
        const auto& bws = pl::lora::bandwidths();
        for (size_t i = 0; i < bws.size(); i++) if (std::fabs(bws[i] - hz) < 1) return (int)i + 1;
        return 0;
    }

    // the channels and receivers for the standards switched on, the Meshtastic preset (mtx held)
    void applySettings() {
        if (presetSel == 0) { setParam(link[MT].rx, "sf", 0); setParam(link[MT].rx, "bw", 0); }
        else {
            const auto& p = mesh::presets()[presetSel - 1];
            setParam(link[MT].rx, "sf", p.sf - 6);
            setParam(link[MT].rx, "bw", bwChoice(p.bwHz));
        }
        setParam(link[MC].rx, "sf", 0);                 // SF7..12 searched
        setParam(link[MC].rx, "bw", bwChoice(MC_BW));
        std::vector<long long> f;
        if (useStd[MT]) f = meshtasticFrequencies(presetSel);
        if (useStd[MC]) for (long long m : meshCoreFrequencies()) f.push_back(m);
        channels.presets = f;
        channels.maxChannels = std::max<size_t>(1, (size_t)useStd[MT] + (size_t)useStd[MC]);
        channels.defaultWidth = useStd[MT] ? meshtasticWidth() : MC_BW * 1.6;
        channels.clear();                                // placed again on the next block
        for (auto& l : link) { l.rx.reset(); l.ncoFor = 1e18; }
        stdChanged = false;
    }

    static std::string clock() {
        char t[16]; const std::time_t n = std::time(nullptr); std::strftime(t, sizeof(t), "%H:%M:%S", std::localtime(&n));
        return t;
    }
    std::string who(uint32_t num) const {
        auto it = dec.nodes.find(num);
        if (it != dec.nodes.end() && !it->second.shortName.empty()) return it->second.shortName;
        return mesh::nodeId(num);
    }
    void addLine(const std::string& s, sf::Color c) {
        recent.push_front({s, c});
        while (recent.size() > 80) recent.pop_back();
    }

    static std::string nodeInfo(const mesh::Node& n) {
        std::string s = n.hw;
        if (!n.role.empty()) s += (s.empty() ? "" : ", ") + n.role;
        char b[48];
        if (n.battery > 100) s += (s.empty() ? "" : ", ") + std::string("powered");
        else if (n.battery >= 0) { std::snprintf(b, sizeof(b), "%sbattery %d %%", s.empty() ? "" : ", ", n.battery); s += b; }
        if (n.hops >= 0) { std::snprintf(b, sizeof(b), "%s%d hop%s away", s.empty() ? "" : ", ", n.hops, n.hops == 1 ? "" : "s"); s += b; }
        return s;
    }

    // a Meshtastic packet decoded (mtx held): the lines, the log, the map
    void eventDone(const mesh::Event& e) {
        const std::string head = clock() + " " + who(e.from) + (e.to == mesh::BROADCAST ? "" : " > " + who(e.to)) + ": ";
        addLine(head + e.text, e.port < 0 ? sf::Color(130, 130, 140) : e.isText ? sf::Color(140, 255, 160) : sf::Color(200, 200, 205));
        char b[64];
        std::snprintf(b, sizeof(b), "  [%s, id %08x, hops %d, SNR %.1f dB]", e.channel.c_str(), e.id, e.hops, e.snr);
        log.add("Meshtastic " + mesh::nodeId(e.from) + " > " + (e.to == mesh::BROADCAST ? std::string("all") : mesh::nodeId(e.to)) + " " +
                (e.isText ? "text: " : ": ") + e.text + b);

        auto it = dec.nodes.find(e.from);
        if (it == dec.nodes.end()) return;
        const mesh::Node& n = it->second;
        const std::string id = mesh::nodeId(n.num);
        const uint32_t rgb = n.num & 0xFFFFFF;
        if (e.port == 3 && n.hasPos) {
            PosReport r;
            r.kind = PosKind::MESH;
            r.id = id; r.label = n.label(); r.info = "Meshtastic  " + nodeInfo(n);
            r.lat = n.lat; r.lon = n.lon;
            if (n.altM > -1e8) r.altM = n.altM;
            if (n.speedKmh >= 0) r.speedKn = n.speedKmh / 1.852;
            if (n.trackDeg >= 0 && n.speedKmh > 1) r.courseDeg = n.trackDeg;
            r.badge = n.badge(); r.badgeRgb = rgb;
            positionBus().report(r);
        } else if (e.port >= 0) positionBus().describe(PosKind::MESH, id, n.label(), "Meshtastic  " + nodeInfo(n), n.badge(), rgb);
    }

    // a MeshCore packet decoded (mtx held)
    void meshCoreDone(const mcore::Event& e) {
        std::string s = clock() + " ";
        if (!e.from.empty()) s += e.from + ": ";
        addLine(s + e.text, e.isText ? sf::Color(140, 255, 160) : e.advert ? sf::Color(200, 200, 205) : sf::Color(130, 130, 140));
        char b[64];
        std::snprintf(b, sizeof(b), "  [hops %d, SNR %.1f dB]", e.hops, e.snr);
        log.add("MeshCore " + std::string(mcore::typeName(e.type)) + " " + (e.from.empty() ? std::string() : e.from + ": ") + e.text + b);
        if (!e.advert) return;
        auto it = mcDec.nodes.find(mcore::hex(e.key.data(), 32));
        if (it == mcDec.nodes.end()) return;
        const mcore::Node& n = it->second;
        const std::string id = "mc:" + n.id();
        std::string info = std::string("MeshCore ") + mcore::nodeTypeName(n.type);
        if (n.hops >= 0) info += ", " + std::to_string(n.hops) + (n.hops == 1 ? " hop" : " hops") + " away";
        if (n.hasPos) {
            PosReport r;
            r.kind = PosKind::MESH;
            r.id = id; r.label = n.label(); r.info = info;
            r.lat = n.lat; r.lon = n.lon;
            r.badge = n.badge(); r.badgeRgb = n.rgb();
            positionBus().report(r);
        } else positionBus().describe(PosKind::MESH, id, n.label(), info, n.badge(), n.rgb());
    }

    // a LoRa frame from either receiver: to the decoder of its sync word (mtx held)
    void frame(const std::vector<uint8_t>& f, const pl::LoraRx::Meta& m, int fromStd, double t) {
        const bool mt = m.sync == 0x2B || (m.sync < 0 && fromStd == MT);
        const bool mc = m.sync == 0x12 || (m.sync < 0 && fromStd == MC);
        bool used = false;
        if (mt && useStd[MT]) {
            used = dec.handle(f, t, m.snrDb);
            for (const auto& e : dec.events) eventDone(e);
            dec.events.clear();
        } else if (mc && useStd[MC]) {
            used = mcDec.handle(f, t, m.snrDb);
            for (const auto& e : mcDec.events) meshCoreDone(e);
            mcDec.events.clear();
        }
        if (used) return;
        otherLora++;
        char b[120];
        std::snprintf(b, sizeof(b), "%s other LoRa: %zu bytes, SF%d / %.4g kHz, sync %s", clock().c_str(), f.size(), m.sf, m.bwHz / 1000.0,
                      m.sync < 0 ? "?" : (std::string("0x") + "0123456789ABCDEF"[m.sync >> 4] + "0123456789ABCDEF"[m.sync & 15]).c_str());
        addLine(b, sf::Color(110, 110, 120));
    }

    // one row of the node table, from either standard
    struct Row {
        int net = MT;
        std::string badge, name, id, hw, role, battery, pos, alt;
        uint32_t rgb = 0;
        int hops = -1, packets = 0;
        double snr = -1000, lastSeen = 0;
    };
    std::vector<Row> rows() const {
        std::vector<Row> v;
        char b[64];
        for (const auto& kv : dec.nodes) {
            const mesh::Node& n = kv.second;
            Row r;
            r.net = MT; r.badge = n.badge(); r.rgb = n.num & 0xFFFFFF;
            r.name = n.longName.empty() ? "?" : n.longName; r.id = mesh::nodeId(n.num);
            r.hw = n.hw.empty() ? "-" : n.hw; r.role = n.role.empty() ? "-" : n.role;
            r.battery = n.battery > 100 ? "power" : n.battery >= 0 ? std::to_string(n.battery) + " %" : "-";
            if (n.hasPos) { std::snprintf(b, sizeof(b), "%.5f, %.5f", n.lat, n.lon); r.pos = b; } else r.pos = "-";
            if (n.hasPos && n.altM > -1e8) { std::snprintf(b, sizeof(b), "%.0f m", n.altM); r.alt = b; } else r.alt = "-";
            r.hops = n.hops; r.snr = n.snr; r.lastSeen = n.lastSeen; r.packets = n.packets;
            v.push_back(r);
        }
        for (const auto& kv : mcDec.nodes) {
            const mcore::Node& n = kv.second;
            Row r;
            r.net = MC; r.badge = n.badge(); r.rgb = n.rgb();
            r.name = n.name.empty() ? "?" : n.name; r.id = n.id();
            r.hw = "-"; r.role = mcore::nodeTypeName(n.type); r.battery = "-";
            if (n.hasPos) { std::snprintf(b, sizeof(b), "%.5f, %.5f", n.lat, n.lon); r.pos = b; } else r.pos = "-";
            r.alt = "-";
            r.hops = n.hops; r.snr = n.snr; r.lastSeen = n.lastSeen; r.packets = n.packets;
            v.push_back(r);
        }
        return v;
    }

public:
    ModuleLog log;
    std::function<void()> onOpenMap;   // main.cpp: open the map window
    // set by the panel button, main.cpp retunes the radio: 869.4 MHz puts Meshtastic's 869.525
    // and MeshCore's 869.618 into the band (from 1 MS/s on), both away from the centre's DC
    static constexpr long long EU_TUNE_HZ = 869400000;
    std::atomic<long long> tuneRequest {0};

    MeshtasticDecoder() : SDRModule("LoRa mesh") {
        channels.name = "Mesh";
        channels.presets = meshtasticFrequencies(0);
        channels.maxChannels = 2;
        channels.defaultWidth = 300000.0;
        channels.listenMode = Mode::RAW;
        channels.shape = [](DecoderChannel& c) {
            if (isMeshCoreFreq(c.freqHz)) { c.name = "MeshCore"; c.widthHz = MC_BW * 1.6; return; }
            c.name = "Meshtastic";
            for (const auto& g : mesh::regions()) for (const auto& p : mesh::presets())
                if (std::llabs(mesh::defaultFrequency(g, p) - c.freqHz) < 2000) { c.name = std::string("Mesh ") + p.name; return; }
        };
        for (auto& l : link) for (auto& p : l.rx.params) if (p.key == "sw") p.text = "any";   // the sync word decides (0x2B / 0x12)
    }

    static bool available() { return true; }

    void init(const ModuleContext& ctx) override {
        font = &ctx.font;
        btnClear = std::make_shared<SdrButton>(84.0f, 24.0f, "Clear list", *font);
        btnClear->onClick = [this]() {
            std::lock_guard<std::mutex> l(mtx);
            dec.clear(); mcDec.clear(); recent.clear(); otherLora = 0;
            positionBus().clear(PosKind::MESH);
        };
        btnMap = std::make_shared<SdrButton>(84.0f, 24.0f, "Show map", *font);
        btnMap->onClick = [this]() { if (onOpenMap) onOpenMap(); };
        btnTune = std::make_shared<SdrButton>(150.0f, 24.0f, "Tune to 869.4 MHz (EU)", *font);
        btnTune->onClick = [this]() { tuneRequest = EU_TUNE_HZ; };
        btnPreset = std::make_shared<SdrButton>(140.0f, 24.0f, "Preset: auto (all)", *font);
        btnPreset->onClick = [this]() {
            std::lock_guard<std::mutex> l(mtx);
            presetSel = (presetSel + 1) % (int)(mesh::presets().size() + 1);
            stdChanged = true;
            btnPreset->setText(presetLabel());
        };
        const char* names[2] = {"Meshtastic", "MeshCore"};
        for (int s = 0; s < 2; s++) {
            chkStd[s] = std::make_shared<Checkbox>(names[s], *font, true);
            chkStd[s]->onToggle = [this, s](bool on) {
                std::lock_guard<std::mutex> l(mtx);
                // at least one stays on
                if (!on && !useStd[1 - s]) { chkStd[s]->checked = true; return; }
                useStd[s] = on;
                stdChanged = true;
            };
        }
    }

    void reset() override {
        std::lock_guard<std::mutex> l(mtx);
        for (auto& lk : link) { lk.rx.reset(); lk.ncoFor = 1e18; }
    }

    float panelHeight(float windowHeight) const override { return std::clamp(windowHeight * 0.40f, 260.0f, 480.0f); }
    void setRadioFrequency(long long hz) override { std::lock_guard<std::mutex> l(mtx); centerHz = hz; }
    bool wantsBurstAgc() const override { return true; }   // packets: the gain must not move inside one
    void setAudioInput(bool audio) override { audioInput = audio; }
    ChannelSet* channelSet() override { return &channels; }

    void processIQ(const std::vector<std::complex<double>>& iq, double sampleRate, double tunedOffset) override {
        if (!enabled || iq.empty() || sampleRate <= 0.0) return;
        std::lock_guard<std::mutex> l(mtx);
        if (audioInput) { channelInfo = "LoRa mesh needs I/Q from an SDR: set Input signal to I/Q"; return; }
        if (stdChanged) applySettings();
        auto rc = channels.update(centerHz, sampleRate, tunedOffset);
        // the channels go to the known frequencies nearest the band's centre: two Meshtastic
        // slots of other regions (RU 869.075 ...) could take both places. MeshCore keeps one.
        if (useStd[MC] && useStd[MT] && centerHz > 0) {
            int mtCount = 0, mtLast = 0;
            bool mcHas = false;
            for (const auto& c : rc) { if (isMeshCoreFreq(c.freqHz)) mcHas = true; else { mtCount++; mtLast = c.id; } }
            const long long mf = meshCoreFrequencies()[0];
            const bool fits = std::fabs((double)(mf - centerHz)) + MC_BW < sampleRate / 2.0;
            if (!mcHas && fits && mtCount >= 2) {
                channels.removeChannel(mtLast);
                channels.addChannel(mf);
                rc = channels.update(centerHz, sampleRate, tunedOffset);
            }
        }
        channelInfo = channels.describe();
        // one Meshtastic channel is enough: a second slot (another region's, e.g. India's
        // 865.125 next to 864.875 when tuned to 865 MHz) goes
        if (useStd[MT] && useStd[MC]) {
            int mtSeen = 0;
            bool removed = false;
            for (const auto& c : rc)
                if (!isMeshCoreFreq(c.freqHz) && !c.ch.followVfo && c.ch.autoPlaced && ++mtSeen > 1) { channels.removeChannel(c.id); removed = true; }
            if (removed) { rc = channels.update(centerHz, sampleRate, tunedOffset); channelInfo = channels.describe(); }
        }
        // where the European frequencies are (the panel offers to tune there when they are not in band)
        euInBand = centerHz <= 0 || std::fabs((double)(869525000 - centerHz)) + 150000 < sampleRate / 2.0;
        // each channel to its standard: MeshCore's frequency to MeshCore, the rest to Meshtastic
        // (with only MeshCore on, any channel is MeshCore's: the user may drag it elsewhere)
        for (auto& lk : link) lk.active = false;
        for (const auto& c : rc) {
            if (!c.inBand) continue;
            const int s = !useStd[MT] ? MC : !useStd[MC] ? MT : isMeshCoreFreq(c.freqHz) ? MC : MT;
            if (link[s].active) continue;
            link[s].active = true;
            link[s].offsetHz = c.offsetHz;
        }
        const double needRate = 1.5 * (presetSel ? mesh::presets()[presetSel - 1].bwHz : 250000.0);
        if (link[MT].active && sampleRate < needRate) { link[MT].active = false; channelInfo = "sample rate too low for Meshtastic (1.5 x the bandwidth)"; }
        std::vector<int> jobs;
        if (link[MT].active) jobs.push_back(MT);       // the more expensive one first
        if (link[MC].active) jobs.push_back(MC);
        if (jobs.empty()) return;
        // the two receivers at the same time (only their own state is touched)
        WorkPool::instance().parallelFor(jobs.size(), [&](size_t k) { link[jobs[k]].run(iq, sampleRate); });
        const double t = now();
        for (int s : jobs) {
            const auto& fr = link[s].rx.out[0].frames;
            for (size_t k = 0; k < fr.size() && k < link[s].rx.meta.size(); k++) frame(fr[k], link[s].rx.meta[k], s, t);
        }
    }

    bool handleEvent(const sf::Event& ev, const sf::RenderWindow& win) override {
        if (!enabled) return false;
        if (sbTable.handleEvent(ev, win, scrollRows)) return true;
        for (auto* b : {btnClear.get(), btnMap.get(), btnPreset.get(), btnTune.get()}) if (b && b->handleEvent(ev, win)) return true;
        for (auto& c : chkStd) if (c && c->handleEvent(ev, win)) return true;
        sf::Vector2f m = win.mapPixelToCoords(sf::Mouse::getPosition(win));
        if (const auto* sc = ev.getIf<sf::Event::MouseWheelScrolled>()) {
            if (!panelRect.contains(m)) return false;
            if (tableRect.contains(m)) scrollRows = std::max(0.0f, scrollRows - sc->delta * 3.0f);
            return true;
        }
        if (ev.getIf<sf::Event::MouseButtonPressed>() && panelRect.contains(m)) return true;
        return false;
    }

    void draw(sf::RenderWindow& win, float x, float y, float w, float h) override {
        if (!enabled || !font) return;
        panelRect = sf::FloatRect({x, y}, {w, h});
        std::vector<Row> list;
        std::deque<Line> lines;
        std::string info;
        long long pk, dcr, enc, other;
        bool on[2], eu = true;
        {
            std::lock_guard<std::mutex> l(mtx);
            list = rows();
            lines = recent;
            info = channelInfo;
            pk = dec.packets + mcDec.packets; dcr = dec.decrypted + mcDec.decoded; enc = dec.encrypted + mcDec.encrypted; other = otherLora;
            on[0] = useStd[0]; on[1] = useStd[1];
            eu = euInBand;
        }
        std::sort(list.begin(), list.end(), [](const Row& a, const Row& b) { return a.lastSeen > b.lastSeen; });
        const double t = now();

        sf::RectangleShape bg({w, h});
        bg.setPosition({x, y});
        bg.setFillColor(sf::Color(20, 22, 27, 245));
        bg.setOutlineColor(Theme::Accent);
        bg.setOutlineThickness(1.0f);
        win.draw(bg);

        const float pad = 8.0f, sideW = 360.0f;
        tableRect = sf::FloatRect({x + pad, y + pad}, {w - sideW - 2 * pad, h - 2 * pad});

        struct Col { const char* name; float w; };
        const Col cols[] = {{"", 50}, {"Net", 66}, {"Name", 150}, {"ID", 74}, {"Hardware", 120}, {"Role", 74}, {"Hops", 36}, {"SNR", 44},
                            {"Battery", 52}, {"Position", 150}, {"Alt", 50}, {"Seen", 44}, {"Pkts", 36}};
        const int NC = (int)(sizeof(cols) / sizeof(cols[0]));
        const float rowH = 20.0f;
        float cx = tableRect.position.x;
        for (const Col& c : cols) {
            if (cx + c.w > tableRect.position.x + tableRect.size.x) break;
            sf::Text th = text(c.name, 22, Theme::Accent);
            th.setStyle(sf::Text::Bold);
            th.setPosition({cx, tableRect.position.y});
            win.draw(th);
            cx += c.w;
        }
        const float bodyY = tableRect.position.y + 18.0f;
        const int visibleRows = std::max(1, (int)((tableRect.size.y - 18.0f) / rowH));
        scrollRows = std::min(scrollRows, (float)std::max(0, (int)list.size() - visibleRows));
        const int first = (int)scrollRows;
        if (list.empty()) {
            sf::Text e = text("No mesh nodes heard yet", 26, sf::Color(110, 110, 120));
            e.setPosition({tableRect.position.x, bodyY + 4.0f});
            win.draw(e);
        }
        for (int r = 0; r < visibleRows && first + r < (int)list.size(); r++) {
            const Row& n = list[first + r];
            const float ry = bodyY + r * rowH;
            if (r % 2 == 0) {
                sf::RectangleShape stripe({tableRect.size.x, rowH});
                stripe.setPosition({tableRect.position.x, ry});
                stripe.setFillColor(sf::Color(255, 255, 255, 8));
                win.draw(stripe);
            }
            const int age = (int)std::max(0.0, t - n.lastSeen);
            const bool fresh = age < 900;
            const sf::Color tc = fresh ? sf::Color(225, 225, 230) : sf::Color(130, 130, 140);
            emoji::badge(win, *font, n.badge, {tableRect.position.x + 22.0f, ry + rowH / 2}, 17.0f,
                         sf::Color((n.rgb >> 16) & 0xFF, (n.rgb >> 8) & 0xFF, n.rgb & 0xFF));
            char b[64];
            std::string cells[13];
            cells[1] = n.net == MT ? "Meshtastic" : "MeshCore";
            cells[2] = n.name; cells[3] = n.id; cells[4] = n.hw; cells[5] = n.role;
            cells[6] = n.hops >= 0 ? std::to_string(n.hops) : "-";
            if (n.snr > -999) { std::snprintf(b, sizeof(b), "%.1f", n.snr); cells[7] = b; } else cells[7] = "-";
            cells[8] = n.battery; cells[9] = n.pos; cells[10] = n.alt;
            if (age < 60) std::snprintf(b, sizeof(b), "%ds", age); else if (age < 3600) std::snprintf(b, sizeof(b), "%dm", age / 60); else std::snprintf(b, sizeof(b), "%dh", age / 3600);
            cells[11] = b;
            cells[12] = std::to_string(n.packets);
            float px = tableRect.position.x + cols[0].w;
            for (int c = 1; c < NC; c++) {
                if (px + cols[c].w > tableRect.position.x + tableRect.size.x) break;
                std::string s = cells[c];
                const float maxW = cols[c].w - 6.0f;
                while (s.size() > 1 && emoji::width(*font, s, 11) > maxW) popChar(s);
                if (s != cells[c]) s += ".";
                const sf::Color cc = c == 1 ? (n.net == MT ? sf::Color(103, 234, 148, fresh ? 255 : 140) : sf::Color(120, 190, 255, fresh ? 255 : 140))
                                   : c == 2 ? sf::Color(255, 255, 255, fresh ? 255 : 150) : tc;
                emoji::draw(win, *font, s, px, ry + 3.0f, 11, cc);
                px += cols[c].w;
            }
        }
        sbTable.hide();
        if ((int)list.size() > visibleRows) {
            const float trackH = tableRect.size.y - 18.0f;
            const float thumbH = std::max(16.0f, trackH * visibleRows / (float)list.size());
            sbTable.layout({{tableRect.position.x + tableRect.size.x - 6.0f, bodyY}, {4.0f, trackH}}, thumbH, scrollRows, (float)(list.size() - visibleRows));
            sbTable.draw(win);
        }

        // --- side column ---
        const float sx = x + w - sideW;
        float sy = y + pad;
        sf::Text title = text("LoRa mesh", 30, Theme::Accent);
        title.setStyle(sf::Text::Bold);
        title.setPosition({sx, sy});
        win.draw(title);
        char st[128];
        std::snprintf(st, sizeof(st), "%zu nodes, %lld packets (%lld read, %lld encrypted)", list.size(), pk, dcr, enc);
        sf::Text stt = text(st, 20, sf::Color(220, 220, 225));
        stt.setPosition({sx + 100.0f, sy + 5.0f});
        win.draw(stt);
        sy += 24.0f;
        chkStd[0]->checked = on[0]; chkStd[1]->checked = on[1];
        chkStd[0]->setPosition(sx, sy); chkStd[0]->draw(win);
        chkStd[1]->setPosition(sx + 110.0f, sy); chkStd[1]->draw(win);
        sy += 22.0f;
        std::string ci = info;
        if (other) ci += "   other LoRa: " + std::to_string(other);
        while (ci.size() > 1 && emoji::width(*font, ci, 10) > sideW - 10.0f) popChar(ci);
        sf::Text cit = text(ci, 20, sf::Color(160, 160, 170));
        cit.setPosition({sx, sy});
        win.draw(cit);
        sy += 18.0f;
        btnPreset->setEnabled(on[0]);
        btnPreset->setPosition(sx, sy); btnPreset->draw(win);
        btnClear->setPosition(sx + 146.0f, sy); btnClear->draw(win);
        btnMap->setPosition(sx + 236.0f, sy); btnMap->draw(win);
        sy += 30.0f;
        if (!eu) {   // tuned elsewhere: say where the meshes are (in Europe) and offer to go there
            sf::Text wt = text("EU meshes: Meshtastic 869.525, MeshCore 869.618 MHz (not in band)", 20, sf::Color(255, 190, 90));
            wt.setPosition({sx, sy + 4.0f});
            win.draw(wt);
            sy += 20.0f;
            btnTune->setPosition(sx, sy); btnTune->draw(win);
            sy += 30.0f;
        }
        sf::Text lt = text("Latest packets", 22, Theme::Accent);
        lt.setPosition({sx, sy});
        win.draw(lt);
        sy += 18.0f;
        for (const auto& m : lines) {
            if (sy > y + h - 16.0f) break;
            std::string s = m.s;
            for (size_t cut = s.size(); cut > 90 && !s.empty(); cut--) popChar(s);   // (the width is measured below)
            while (s.size() > 1 && emoji::width(*font, s, 10) > sideW - 14.0f) popChar(s);
            emoji::draw(win, *font, s, sx, sy, 10, m.c);
            sy += 15.0f;
        }
    }
};
