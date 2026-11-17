#pragma once

#include "../Core/SDRModule.h"
#include "../Core/DetailsPopup.h"
#include "../Core/ModuleLog.h"
#include "../Core/PositionBus.h"
#include "../Core/NarrowChannel.h"
#include "Vdl2Core.h"
#include <SFML/Graphics.hpp>
#include <mutex>
#include <deque>
#include <map>
#include <regex>
#include <vector>
#include <string>
#include <ctime>
#include <cstdio>
#include <algorithm>
#include <atomic>

// =====================================================================
//  VDL Mode 2 decoder module (aircraft datalink, 136.6 .. 136.975 MHz).
//
//  Most European ACARS traffic moved from the old 2400 bit/s AM channels
//  (131.525 / 131.725 ...) to VDL2: 31.5 kbit/s D8PSK on 25 kHz channels,
//  the common signalling channel is 136.975 MHz. Every known VDL2
//  channel inside the received band is decoded at once (e.g. tune
//  136.800 MHz with 2 MSps for 136.650 .. 136.975). See Vdl2Core.h.
//
//  Shown: ACARS messages carried in VDL2 (registration, flight, label,
//  text), XID link management (aircraft position + altitude, destination
//  airport, ground station position: all go on the map) and the other
//  frames by type (X.25 / ATN: CPDLC, CM - listed, not decoded).
// =====================================================================

class VDL2Decoder : public SDRModule {
    struct Entry {
        std::string time;
        long long freqHz = 0;
        float levelDb = 0;
        int corrected = 0;
        vdl2::Frame f;
    };
    struct Rx {
        long long freqHz = 0;
        NarrowChannel ch;
        vdl2::Receiver rx;
        std::vector<std::complex<float>> tmp;
        int frames = 0;
        double level = 0.0;
    };

    static const std::vector<long long>& knownChannels() {
        static const std::vector<long long> ch = {
            136975000, 136925000, 136875000, 136825000, 136775000, 136725000, 136700000, 136650000, 136900000, 136800000 };
        return ch;
    }

    std::mutex mtx;
    std::map<int, std::unique_ptr<Rx>> rxs;
    ChannelSet channels;
    std::deque<Entry> entries;   // newest first
    std::map<uint32_t, std::string> regOf;   // ICAO address -> registration (learned from ACARS)
    long long centerHz = 0;
    double lastRate = 0.0;
    int total = 0;
    std::string channelInfo = "Waiting for signal...";
    bool tunedOnly = false;

    const sf::Font* font = nullptr;
    sf::FloatRect panelRect, tableRect;
    float scrollRows = 0.0f, tableBodyY = 0.0f;
    const float rowH = 16.0f;
    int tableFirst = 0, tableVisible = 0;
    std::shared_ptr<SdrButton> btnClear, btnLog, btnExport, btnFilter;
    bool acarsOnly = false;   // hide link management (S / U frames without data, X.25)
    DetailsPopup popup;

    static std::string wallTime() {
        std::time_t t = std::time(nullptr);
        std::tm tm = *std::localtime(&t);
        char b[16];
        std::strftime(b, sizeof(b), "%H:%M:%S", &tm);
        return b;
    }
    static std::string mhz(long long hz) { char b[24]; std::snprintf(b, sizeof(b), "%.3f", hz / 1e6); return b; }
    static std::string hex6(uint32_t a) { char b[12]; std::snprintf(b, sizeof(b), "%06X", a); return b; }

    sf::Text text(const std::string& s, unsigned size, sf::Color c) const {
        sf::Text t(*font, sf::String::fromUtf8(s.begin(), s.end()), size);
        t.setScale({0.5f, 0.5f});
        t.setFillColor(c);
        return t;
    }
    static std::string fit(const std::string& s, size_t n) { return s.size() > n ? s.substr(0, n - 1) + "." : s; }

    std::string stationName(uint32_t addr, int type) const {
        if (type == vdl2::ADDR_AIRCRAFT) {
            auto it = regOf.find(addr);
            return it != regOf.end() ? it->second : hex6(addr);
        }
        if (type == vdl2::ADDR_ALL) return "all";
        return "GS " + hex6(addr);
    }

    // Aircraft / ground station positions on the map (mtx held)
    void reportPositions(const vdl2::Frame& f) {
        const bool fromAc = f.srcType == vdl2::ADDR_AIRCRAFT;
        const uint32_t ac = fromAc ? f.src : f.dst;
        if (f.hasAcPos && fromAc) {
            PosReport r;
            r.kind = PosKind::ACARS;
            r.id = hex6(ac);
            r.label = stationName(ac, vdl2::ADDR_AIRCRAFT);
            r.info = "VDL2 XID" + (f.dstAirport.empty() ? std::string() : ", to " + f.dstAirport);
            r.lat = f.acLat; r.lon = f.acLon;
            r.altM = f.acAltFt * 0.3048;
            positionBus().report(r);
        }
        if (f.hasGsPos) {
            const uint32_t gs = f.srcType != vdl2::ADDR_AIRCRAFT ? f.src : f.dst;
            PosReport r;
            r.kind = PosKind::ACARS;
            r.id = "GS" + hex6(gs);
            r.label = "VDL2 ground station " + hex6(gs);
            r.info = "VDL2 ground station";
            r.lat = f.gsLat; r.lon = f.gsLon;
            positionBus().report(r);
        }
        if (f.hasAcars && f.acars.downlink && !f.acars.text.empty()) {
            // "POSN52142E021022" (degrees + minutes x10) or "N 52.142,E 21.022"
            static const std::regex posMin(R"(([NS])\s?(\d{2})(\d{2})(\d)\s?[,/ ]?\s?([EW])\s?(\d{3})(\d{2})(\d))");
            static const std::regex posDec(R"(([NS])\s?(\d{1,2}\.\d{2,4})\s?[, ]\s?([EW])\s?(\d{1,3}\.\d{2,4}))");
            std::smatch s;
            double lat = 0, lon = 0;
            const std::string& t = f.acars.text;
            if (std::regex_search(t, s, posDec)) {
                lat = std::stod(s[2]); lon = std::stod(s[4]);
                if (s[1] == "S") lat = -lat;
                if (s[3] == "W") lon = -lon;
            } else if (std::regex_search(t, s, posMin)) {
                lat = std::stoi(s[2]) + (std::stoi(s[3]) + std::stoi(s[4]) / 10.0) / 60.0;
                lon = std::stoi(s[6]) + (std::stoi(s[7]) + std::stoi(s[8]) / 10.0) / 60.0;
                if (s[1] == "S") lat = -lat;
                if (s[5] == "W") lon = -lon;
            } else return;
            if (std::fabs(lat) > 90 || std::fabs(lon) > 180) return;
            PosReport r;
            r.kind = PosKind::ACARS;
            r.id = hex6(ac);
            r.label = f.acars.flight.empty() ? stationName(ac, vdl2::ADDR_AIRCRAFT)
                                             : f.acars.flight + " (" + stationName(ac, vdl2::ADDR_AIRCRAFT) + ")";
            r.info = "VDL2 ACARS label " + f.acars.label;
            r.lat = lat; r.lon = lon;
            positionBus().report(r);
        }
    }

    void onFrame(long long freqHz, const vdl2::Frame& f, float levelDb, int corrected) {
        if (f.hasAcars && !f.acars.reg.empty()) {
            const uint32_t ac = f.srcType == vdl2::ADDR_AIRCRAFT ? f.src : f.dst;
            regOf[ac] = f.acars.reg;
        }
        Entry e;
        e.time = wallTime();
        e.freqHz = freqHz;
        e.levelDb = levelDb;
        e.corrected = corrected;
        e.f = f;
        entries.push_front(e);
        if (entries.size() > 2000) entries.pop_back();
        total++;
        reportPositions(f);
        log.add(mhz(freqHz) + " MHz  " + stationName(f.src, f.srcType) + " -> " + stationName(f.dst, f.dstType) +
                "  " + f.kind + "  " + f.summary);
    }

    bool visible(const Entry& e) const {
        if (!acarsOnly) return true;
        return e.f.hasAcars || e.f.hasAcPos || e.f.hasGsPos || !e.f.dstAirport.empty();
    }

    void openDetails(const Entry& e) {
        const vdl2::Frame& f = e.f;
        std::vector<std::pair<std::string, std::string>> rows;
        rows.push_back({"Time", e.time});
        rows.push_back({"Frequency", mhz(e.freqHz) + " MHz"});
        char b[96];
        std::snprintf(b, sizeof(b), "%.1f dBFS%s", e.levelDb, e.corrected > 0 ? (", " + std::to_string(e.corrected) + " octets corrected (RS)").c_str() : "");
        rows.push_back({"Signal", b});
        rows.push_back({"From", hex6(f.src) + " (" + vdl2::addrTypeName(f.srcType) + ")" +
                                (f.srcType == vdl2::ADDR_AIRCRAFT && regOf.count(f.src) ? "  " + regOf.at(f.src) : "")});
        rows.push_back({"To", hex6(f.dst) + " (" + vdl2::addrTypeName(f.dstType) + ")" +
                              (f.dstType == vdl2::ADDR_AIRCRAFT && regOf.count(f.dst) ? "  " + regOf.at(f.dst) : "")});
        rows.push_back({"Aircraft", f.onGround ? "on ground" : "airborne"});
        rows.push_back({"AVLC frame", f.kind + (f.response ? " (response)" : " (command)")});
        rows.push_back({"Contents", f.proto});
        std::string body;
        if (f.hasAcars) {
            const acars::Message& m = f.acars;
            rows.push_back({"Registration", m.reg.empty() ? "-" : m.reg});
            if (!m.flight.empty()) rows.push_back({"Flight", m.flight});
            const char* ld = acars::labelDescription(m.label);
            rows.push_back({"Label", m.label + (*ld ? std::string("  (") + ld + ")" : "")});
            rows.push_back({"Mode / block", std::string(1, m.mode) + " / " + std::string(1, m.blockId)});
            if (!m.msn.empty()) rows.push_back({"Message no.", m.msn});
            rows.push_back({"ACARS CRC", f.acarsCrcOk ? "OK" : "wrong"});
            if (m.more) rows.push_back({"Blocks", "more blocks follow (ETB)"});
            body = m.text.empty() ? "(no text)" : m.text;
        }
        if (f.hasAcPos) { std::snprintf(b, sizeof(b), "%.1f, %.1f  %d ft", f.acLat, f.acLon, f.acAltFt); rows.push_back({"Aircraft position", b}); }
        if (!f.dstAirport.empty()) rows.push_back({"Destination", f.dstAirport});
        if (f.hasGsPos) { std::snprintf(b, sizeof(b), "%.1f, %.1f", f.gsLat, f.gsLon); rows.push_back({"Ground station", b}); }
        if (body.empty()) {
            // hex dump of the AVLC information field
            std::string hx;
            for (size_t i = 9; i < f.raw.size(); i++) {
                char h[4];
                std::snprintf(h, sizeof(h), "%02X ", f.raw[i]);
                hx += h;
                if ((i - 9) % 24 == 23) hx += "\n";
            }
            body = hx.empty() ? "(no information field)" : hx;
        }
        popup.show("VDL2  " + stationName(f.src, f.srcType) + " -> " + stationName(f.dst, f.dstType), rows, body);
    }

public:
    ModuleLog log;
    std::string exportDir;

    VDL2Decoder() : SDRModule("VDL2") {
        channels.name = "VDL2";
        channels.presets = knownChannels();
        channels.maxChannels = 10;   // all of them: 2 MSps around 136.8 MHz covers every channel
        channels.defaultWidth = 25000.0;
        channels.listenMode = Mode::AM;
        channels.shape = [](DecoderChannel& c) {
            char b[24];
            std::snprintf(b, sizeof(b), "%.3f", c.freqHz / 1e6);
            c.name = c.followVfo ? std::string("VDL2") : std::string(b);
        };
    }

    void init(const ModuleContext& ctx) override {
        font = &ctx.font;
        popup.init(*font);
        btnClear = std::make_shared<SdrButton>(100.0f, 24.0f, "Clear list", *font);
        btnClear->onClick = [this]() {
            std::lock_guard<std::mutex> l(mtx);
            entries.clear(); total = 0;
            for (auto& kv : rxs) kv.second->frames = 0;
        };
        btnLog = std::make_shared<SdrButton>(100.0f, 24.0f, "Open log", *font);
        btnLog->onClick = [this]() { log.openLogFile(); };
        btnExport = std::make_shared<SdrButton>(100.0f, 24.0f, "Export", *font);
        btnExport->onClick = [this]() { log.exportSession(exportDir, "VDL2"); };
        btnFilter = std::make_shared<SdrButton>(150.0f, 22.0f, "Only ACARS / positions", *font);
        btnFilter->onClick = [this]() { acarsOnly = !acarsOnly; btnFilter->setActive(acarsOnly); scrollRows = 0; };
    }

    void reset() override { std::lock_guard<std::mutex> l(mtx); rxs.clear(); }
    float panelHeight(float windowHeight) const override { return std::clamp(windowHeight * 0.38f, 260.0f, 460.0f); }
    void setRadioFrequency(long long hz) override { std::lock_guard<std::mutex> l(mtx); centerHz = hz; }
    ChannelSet* channelSet() override { return &channels; }
    bool modalOpen() const override { return popup.isOpen(); }
    void drawModal(sf::RenderWindow& win) override { popup.draw(win); }

    void processIQ(const std::vector<std::complex<double>>& iq, double sampleRate, double tunedOffset) override {
        if (!enabled || iq.empty() || sampleRate <= 0.0) return;
        std::lock_guard<std::mutex> l(mtx);
        if (sampleRate < 120000.0) { channelInfo = "Sample rate too low (VDL2 needs at least 250 kS/s)"; return; }
        const auto rc = channels.update(centerHz, sampleRate, tunedOffset);
        tunedOnly = rc.size() == 1 && rc[0].ch.fallback;
        if (sampleRate != lastRate) { rxs.clear(); lastRate = sampleRate; }
        for (auto it = rxs.begin(); it != rxs.end();) {
            bool keep = false;
            for (const auto& c : rc) if (c.id == it->first) keep = true;
            if (!keep) it = rxs.erase(it); else ++it;
        }
        for (const auto& c : rc) {
            auto& slot = rxs[c.id];
            if (!slot) {
                slot = std::make_unique<Rx>();
                Rx* raw = slot.get();
                slot->rx.onFrame = [this, raw](const vdl2::Frame& f, float lv, int corr) {   // mtx held
                    raw->frames++;
                    onFrame(raw->freqHz, f, lv, corr);
                };
            }
            Rx& r = *slot;
            r.freqHz = c.freqHz;
            if (!c.inBand) continue;
            r.ch.configure(sampleRate, c.offsetHz, vdl2::SAMPLE_RATE, 9000.0, 6000.0, 1.0);
            r.tmp.clear();
            r.ch.process(iq.data(), iq.size(), r.tmp);
            if (r.tmp.empty()) continue;
            double lv = 0.0;
            for (const auto& v : r.tmp) lv += std::norm(v);
            r.level = 0.9 * r.level + 0.1 * (lv / r.tmp.size());
            r.rx.process(r.tmp.data(), r.tmp.size());
        }
        channelInfo = tunedOnly ? "No VDL2 channel in band: decoding at the tuner"
                                : std::to_string(rxs.size()) + " channel(s)";
    }

    bool handleEvent(const sf::Event& ev, const sf::RenderWindow& win) override {
        if (!enabled) return false;
        if (popup.handleEvent(ev, win)) return true;
        for (auto* b : {&btnClear, &btnLog, &btnExport, &btnFilter})
            if (*b && (*b)->handleEvent(ev, win)) return true;
        sf::Vector2f m = win.mapPixelToCoords(sf::Mouse::getPosition(win));
        if (const auto* sc = ev.getIf<sf::Event::MouseWheelScrolled>()) {
            if (!panelRect.contains(m)) return false;
            if (tableRect.contains(m)) scrollRows = std::max(0.0f, scrollRows - sc->delta * 3.0f);
            return true;
        }
        if (const auto* mb = ev.getIf<sf::Event::MouseButtonPressed>()) {
            if (!panelRect.contains(m)) return false;
            if (mb->button == sf::Mouse::Button::Left && tableRect.contains(m) && m.y >= tableBodyY) {
                const int row = (int)((m.y - tableBodyY) / rowH);
                Entry e;
                bool found = false;
                {
                    std::lock_guard<std::mutex> l(mtx);
                    int seen = 0;
                    const int want = tableFirst + row;
                    if (row >= 0 && row < tableVisible)
                        for (const Entry& x : entries) {
                            if (!visible(x)) continue;
                            if (seen++ == want) { e = x; found = true; break; }
                        }
                }
                if (found) {
                    std::lock_guard<std::mutex> l(mtx);
                    openDetails(e);
                }
            }
            return true;
        }
        return false;
    }

    void draw(sf::RenderWindow& win, float x, float y, float w, float h) override {
        if (!enabled || !font) return;
        panelRect = sf::FloatRect({x, y}, {w, h});

        struct Row { std::string cells[7]; sf::Color dirCol; bool hasText; };
        std::vector<Row> list;
        std::vector<std::string> chans;
        std::string info;
        int count = 0;
        bool tuned = false;
        {
            std::lock_guard<std::mutex> l(mtx);
            const int first = (int)scrollRows;
            int idx = 0;
            for (const Entry& e : entries) {
                if (!visible(e)) continue;
                if (idx >= first && idx < first + 60) {
                    const vdl2::Frame& f = e.f;
                    Row r;
                    const bool down = f.srcType == vdl2::ADDR_AIRCRAFT;
                    r.cells[0] = e.time;
                    r.cells[1] = mhz(e.freqHz);
                    r.cells[2] = stationName(f.src, f.srcType);
                    r.cells[3] = stationName(f.dst, f.dstType);
                    r.cells[4] = f.hasAcars ? "ACARS" : f.proto == "-" ? f.kind : f.proto;
                    r.cells[5] = f.hasAcars ? f.acars.label : "";
                    std::string t = f.hasAcars ? (f.acars.flight.empty() ? "" : f.acars.flight + "  ") + f.acars.text : f.summary;
                    for (char& c : t) if (c == '\n' || c == '\r' || c == '\t') c = ' ';
                    if (f.hasAcars && f.acars.text.empty() && *acars::labelDescription(f.acars.label))
                        t += std::string("(") + acars::labelDescription(f.acars.label) + ")";
                    r.cells[6] = t;
                    r.dirCol = down ? sf::Color(120, 200, 255) : sf::Color(255, 200, 120);
                    r.hasText = f.hasAcars || f.hasAcPos || f.hasGsPos;
                    list.push_back(r);
                }
                idx++;
            }
            tableFirst = first;
            count = idx;
            for (auto& kv : rxs) {
                const double db = kv.second->level > 1e-15 ? 10.0 * std::log10(kv.second->level) : -150.0;
                char b[96];
                std::snprintf(b, sizeof(b), "%s MHz   %4d frames   bursts %4d   %5.1f dB", mhz(kv.second->freqHz).c_str(),
                              kv.second->frames, kv.second->rx.stats.bursts, db);
                chans.push_back(b);
            }
            info = channelInfo;
            tuned = tunedOnly;
        }

        sf::RectangleShape bg({w, h});
        bg.setPosition({x, y});
        bg.setFillColor(sf::Color(20, 22, 27, 245));
        bg.setOutlineColor(Theme::Accent);
        bg.setOutlineThickness(1.0f);
        win.draw(bg);

        const float pad = 8.0f, sideW = 340.0f;
        tableRect = sf::FloatRect({x + pad, y + pad}, {w - sideW - 2 * pad, h - 2 * pad});
        struct Col { const char* name; float w; };
        const float fixedW = 58 + 56 + 76 + 76 + 50 + 34;
        const float textW = std::max(80.0f, tableRect.size.x - fixedW);
        const Col cols[] = { {"Time", 58}, {"Freq", 56}, {"From", 76}, {"To", 76}, {"Type", 50}, {"Lbl", 34}, {"Text", textW} };
        float cx = tableRect.position.x;
        for (const Col& c : cols) {
            sf::Text th = text(c.name, 22, Theme::Accent);
            th.setStyle(sf::Text::Bold);
            th.setPosition({cx, tableRect.position.y});
            if (cx + 20.0f <= tableRect.position.x + tableRect.size.x) win.draw(th);
            cx += c.w;
        }
        tableBodyY = tableRect.position.y + rowH + 4.0f;
        const int visibleRows = std::max(1, (int)((tableRect.size.y - rowH - 4.0f) / rowH));
        tableVisible = visibleRows;
        {
            std::lock_guard<std::mutex> l(mtx);
            scrollRows = std::min(scrollRows, (float)std::max(0, count - visibleRows));
        }
        if (list.empty()) {
            sf::Text e = text(tuned ? "Tune near 136.800 MHz (2 MSps covers 136.650 .. 136.975) or drag the channel to 136.975 MHz."
                                    : "No frames yet. Ground stations talk every few seconds when aircraft are near.", 24, sf::Color(110, 110, 120));
            e.setPosition({tableRect.position.x, tableBodyY + 4.0f});
            win.draw(e);
        }
        const float charW = 5.6f;
        for (int r = 0; r < visibleRows && r < (int)list.size(); r++) {
            const Row& row = list[r];
            const float ry = tableBodyY + r * rowH;
            if (r % 2 == 0) {
                sf::RectangleShape stripe({tableRect.size.x, rowH});
                stripe.setPosition({tableRect.position.x, ry});
                stripe.setFillColor(sf::Color(255, 255, 255, 8));
                win.draw(stripe);
            }
            float px = tableRect.position.x;
            for (int c = 0; c < 7; c++) {
                if (px + 20.0f > tableRect.position.x + tableRect.size.x) break;
                const size_t maxChars = (size_t)std::max(3.0f, (cols[c].w - 6.0f) / charW);
                sf::Color col = sf::Color(225, 225, 230);
                if (c == 2) col = row.dirCol;
                if (c == 6 && !row.hasText) col = sf::Color(130, 130, 140);
                sf::Text td = text(fit(row.cells[c], maxChars), 22, col);
                td.setPosition({px, ry});
                win.draw(td);
                px += cols[c].w;
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

        const float sx = x + w - sideW;
        float sy = y + pad;
        sf::Text title = text("VDL Mode 2", 30, Theme::Accent);
        title.setStyle(sf::Text::Bold);
        title.setPosition({sx, sy});
        win.draw(title);
        sf::Text st = text(std::to_string(count) + " frames", 22, sf::Color(220, 220, 225));
        st.setPosition({sx + 110.0f, sy + 4.0f});
        win.draw(st);
        sy += 24.0f;
        sf::Text ci = text(info, 20, sf::Color(160, 160, 170));
        ci.setPosition({sx, sy});
        win.draw(ci);
        sy += 18.0f;
        btnClear->setPosition(sx, sy); btnClear->draw(win);
        btnLog->setPosition(sx + 106.0f, sy); btnLog->draw(win);
        btnExport->setPosition(sx + 212.0f, sy); btnExport->draw(win);
        sy += 30.0f;
        btnFilter->setPosition(sx, sy); btnFilter->draw(win);
        sy += 30.0f;
        sf::Text lt = text("Channels (click a table row for details)", 22, Theme::Accent);
        lt.setPosition({sx, sy});
        win.draw(lt);
        sy += 18.0f;
        for (const auto& c : chans) {
            if (sy > y + h - 16.0f) break;
            sf::Text ct = text(c, 20, sf::Color(200, 200, 205));
            ct.setPosition({sx, sy});
            win.draw(ct);
            sy += 15.0f;
        }
    }
};
