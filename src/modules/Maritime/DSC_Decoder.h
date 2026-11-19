#pragma once

#include "../Core/SDRModule.h"
#include "../Core/ScrollDrag.h"
#include "../Core/DetailsPopup.h"
#include "../Core/ModuleLog.h"
#include "../Core/PositionBus.h"
#include "../Core/NarrowChannel.h"
#include "MaritimeCore.h"
#include <SFML/Graphics.hpp>
#include <mutex>
#include <deque>
#include <map>
#include <atomic>
#include <ctime>
#include <cstdio>
#include <algorithm>

// =====================================================================
//  DSC decoder module (Digital Selective Calling, GMDSS).
//
//  VHF channel 70 (156.525 MHz): FM, 1200 Bd AFSK - ships and coast
//  stations calling each other, distress / urgency / safety alerts.
//  MF/HF: 2187.5, 4207.5, 6312, 8414.5, 12577, 16804.5 kHz (100 Bd FSK).
//  Every DSC frequency inside the received band is decoded at once.
//  Positions in the calls go on the map (with the ship's AIS track: the
//  same MMSI). Audio input: a scanner's FM audio (VHF DSC).
// =====================================================================

class DSCDecoder : public SDRModule {
    ScrollDrag sbTable;   // the table's scroll bar (draggable)
    struct Entry {
        std::string time;
        long long freqHz = 0;
        maritime::DscMessage msg;
    };
    struct Rx {
        long long freqHz = 0;
        bool vhf = false;
        NarrowChannel ch;
        maritime::DscReceiver rx;
        std::vector<std::complex<float>> tmp;
        int messages = 0;
    };

    static constexpr long long AUDIO_FREQ = -1;
    static bool isVhf(long long hz) { return hz > 30000000; }

    std::mutex mtx;
    std::map<int, std::unique_ptr<Rx>> rxs;
    std::unique_ptr<Rx> audioRx;
    ChannelSet channels;
    std::deque<Entry> entries;
    long long centerHz = 0;
    double lastRate = 0.0;
    std::atomic<bool> audioInput {false};
    std::string channelInfo = "Waiting for signal...";
    bool tunedOnly = false;

    const sf::Font* font = nullptr;
    sf::FloatRect panelRect, tableRect;
    float scrollRows = 0.0f, tableBodyY = 0.0f;
    const float rowH = 16.0f;
    int tableFirst = 0, tableVisible = 0;
    std::shared_ptr<SdrButton> btnClear, btnLog, btnExport;
    DetailsPopup popup;

    static std::string wallTime() {
        std::time_t t = std::time(nullptr);
        std::tm tm = *std::localtime(&t);
        char b[16];
        std::strftime(b, sizeof(b), "%H:%M:%S", &tm);
        return b;
    }
    static std::string freqText(long long hz) {
        if (hz == AUDIO_FREQ) return "audio";
        if (hz == 156525000) return "VHF CH70";
        char b[24];
        if (isVhf(hz)) std::snprintf(b, sizeof(b), "%.3f MHz", hz / 1e6);
        else std::snprintf(b, sizeof(b), "%.1f kHz", hz / 1e3);
        return b;
    }
    static std::string name(const char* s) { return s ? s : "-"; }
    sf::Text text(const std::string& s, unsigned size, sf::Color c) const {
        sf::Text t(*font, sf::String::fromUtf8(s.begin(), s.end()), size);
        t.setScale({0.5f, 0.5f});
        t.setFillColor(c);
        return t;
    }
    static std::string fit(const std::string& s, size_t n) { return s.size() > n ? s.substr(0, n - 1) + "." : s; }

    static std::string summary(const maritime::DscMessage& m) {
        std::string s;
        if (m.nature >= 0) s += name(maritime::dscDistressName(m.nature));
        if (m.tc1 >= 0) s += (s.empty() ? "" : ", ") + name(maritime::dscTelecommand1Name(m.tc1));
        if (!m.distressId.empty()) s += "  vessel " + m.distressId;
        if (!m.freq1.empty()) s += "  " + m.freq1;
        if (m.hasPos) s += "  " + m.position;
        if (!m.number.empty()) s += "  tel " + m.number;
        return s;
    }

    void wire(Rx* r) {
        r->rx.onMessage = [this, r](const maritime::DscMessage& m) {   // mtx held
            r->messages++;
            Entry e;
            e.time = wallTime();
            e.freqHz = r->freqHz;
            e.msg = m;
            entries.push_front(e);
            if (entries.size() > 1000) entries.pop_back();
            log.add(freqText(r->freqHz) + "  " + name(maritime::dscFormatName(m.format)) + "  " +
                    (m.category >= 0 ? name(maritime::dscCategoryName(m.category)) : std::string("-")) + "  from " + m.selfId +
                    (m.address.empty() ? std::string() : "  to " + m.address) + "  " + summary(m));
            // the ship on the map (AIS uses the same id, the 9 digit MMSI)
            if (m.hasPos && m.selfId.size() >= 9) {
                PosReport p;
                p.kind = PosKind::AIS;
                p.id = (m.format == maritime::DSC_DISTRESS || m.distressId.empty()) ? m.selfId.substr(0, 9) : m.distressId.substr(0, 9);
                p.info = std::string("DSC ") + name(maritime::dscFormatName(m.format)) + (m.nature >= 0 ? std::string(": ") + name(maritime::dscDistressName(m.nature)) : "");
                p.label = p.id; p.lat = m.lat; p.lon = m.lon;
                positionBus().report(p);
            }
        };
    }

    void openDetails(const Entry& e) {
        const maritime::DscMessage& m = e.msg;
        std::vector<std::pair<std::string, std::string>> rows;
        rows.push_back({"Received", e.time});
        rows.push_back({"Frequency", e.freqHz == AUDIO_FREQ ? std::string("audio input") : freqText(e.freqHz)});
        rows.push_back({"Format", name(maritime::dscFormatName(m.format))});
        if (m.category >= 0) rows.push_back({"Category", name(maritime::dscCategoryName(m.category))});
        rows.push_back({"From (MMSI)", m.selfId});
        if (!m.address.empty()) rows.push_back({"To", m.address});
        if (m.tc1 >= 0) rows.push_back({"Telecommand 1", name(maritime::dscTelecommand1Name(m.tc1))});
        if (m.tc2 >= 0) rows.push_back({"Telecommand 2", name(maritime::dscTelecommand2Name(m.tc2))});
        if (!m.distressId.empty()) rows.push_back({"Vessel in distress", m.distressId});
        if (m.nature >= 0) rows.push_back({"Nature of distress", name(maritime::dscDistressName(m.nature))});
        if (!m.position.empty()) rows.push_back({"Position", m.position + (m.hasPos ? "  (on the map)" : "")});
        if (!m.time.empty()) rows.push_back({"Time of position", m.time + " UTC"});
        if (!m.freq1.empty()) rows.push_back({"Working frequency", m.freq1 + (m.freq2.empty() ? "" : " / " + m.freq2)});
        if (!m.number.empty()) rows.push_back({"Telephone number", m.number});
        if (m.subsequent >= 0) rows.push_back({"Subsequent comms", name(maritime::dscTelecommand1Name(m.subsequent))});
        rows.push_back({"End of sequence", name(maritime::dscEosName(m.eos))});
        rows.push_back({"Symbol errors", std::to_string(m.errors) + " (corrected with the repeated symbols)"});
        std::string syms;
        for (size_t i = 0; i < m.symbols.size(); i++) syms += std::to_string(m.symbols[i]) + (i % 16 == 15 ? "\n" : " ");
        popup.show("DSC  " + name(maritime::dscFormatName(m.format)) + "  " + m.selfId, rows, "Symbols: " + syms);
    }

public:
    ModuleLog log;
    std::string exportDir;

    DSCDecoder() : SDRModule("DSC") {
        channels.name = "DSC";
        channels.presets = { 156525000, 2187500, 4207500, 6312000, 8414500, 12577000, 16804500 };
        channels.maxChannels = 7;
        channels.defaultWidth = 25000.0;
        channels.shape = [](DecoderChannel& c) {
            const bool v = isVhf(c.freqHz);
            c.widthHz = v ? 25000.0 : 400.0;
            c.listenMode = v ? Mode::NFM : Mode::CW;
            char b[32];
            if (c.freqHz == 156525000) std::snprintf(b, sizeof(b), "DSC CH70");
            else if (v) std::snprintf(b, sizeof(b), "DSC %.3f", c.freqHz / 1e6);
            else std::snprintf(b, sizeof(b), "DSC %.1f", c.freqHz / 1e3);
            c.name = c.followVfo ? std::string("DSC") : std::string(b);
        };
    }

    void init(const ModuleContext& ctx) override {
        font = &ctx.font;
        popup.init(*font);
        btnClear = std::make_shared<SdrButton>(100.0f, 24.0f, "Clear list", *font);
        btnClear->onClick = [this]() { std::lock_guard<std::mutex> l(mtx); entries.clear(); for (auto& kv : rxs) kv.second->messages = 0; };
        btnLog = std::make_shared<SdrButton>(100.0f, 24.0f, "Open log", *font);
        btnLog->onClick = [this]() { log.openLogFile(); };
        btnExport = std::make_shared<SdrButton>(100.0f, 24.0f, "Export", *font);
        btnExport->onClick = [this]() { log.exportSession(exportDir, "DSC"); };
    }

    void reset() override { std::lock_guard<std::mutex> l(mtx); rxs.clear(); audioRx.reset(); }
    float panelHeight(float windowHeight) const override { return std::clamp(windowHeight * 0.36f, 240.0f, 440.0f); }
    void setRadioFrequency(long long hz) override { std::lock_guard<std::mutex> l(mtx); centerHz = hz; }
    void setAudioInput(bool a) override { audioInput = a; }
    ChannelSet* channelSet() override { return &channels; }
    bool modalOpen() const override { return popup.isOpen(); }
    void drawModal(sf::RenderWindow& win) override { popup.draw(win); }

    // Audio input: a scanner's FM audio on channel 70
    void processAudio(const std::vector<float>& audio) override {
        if (!enabled || !audioInput || audio.empty()) return;
        std::lock_guard<std::mutex> l(mtx);
        if (!audioRx) { audioRx = std::make_unique<Rx>(); audioRx->freqHz = AUDIO_FREQ; audioRx->rx.setVhf(true); wire(audioRx.get()); }
        audioRx->rx.processAudio(audio.data(), audio.size());
        channelInfo = "Input signal: Audio (VHF channel 70, FM audio)";
    }

    void processIQ(const std::vector<std::complex<double>>& iq, double sampleRate, double tunedOffset) override {
        if (!enabled || iq.empty() || sampleRate <= 0.0) return;
        std::lock_guard<std::mutex> l(mtx);
        if (audioInput) { rxs.clear(); return; }
        audioRx.reset();
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
            if (!slot) { slot = std::make_unique<Rx>(); wire(slot.get()); }
            Rx& r = *slot;
            const bool v = isVhf(c.freqHz);
            if (r.freqHz != c.freqHz || r.vhf != v) { r.vhf = v; r.rx.setVhf(v); r.rx.reset(); r.freqHz = c.freqHz; }
            if (!c.inBand) continue;
            if (v) r.ch.configure(sampleRate, c.offsetHz, 48000.0, 8000.0, 4000.0, 1.0);
            else r.ch.configure(sampleRate, c.offsetHz, 1000.0, 250.0, 150.0);
            if (v && sampleRate < 50000.0) continue;
            r.tmp.clear();
            r.ch.process(iq.data(), iq.size(), r.tmp);
            r.rx.process(r.tmp.data(), r.tmp.size());
        }
        channelInfo = tunedOnly ? "No DSC frequency in band: decoding at the tuner" : std::to_string(rxs.size()) + " channel(s)";
    }

    bool handleEvent(const sf::Event& ev, const sf::RenderWindow& win) override {
        if (!enabled) return false;
        if (popup.handleEvent(ev, win)) return true;
        if (sbTable.handleEvent(ev, win, scrollRows)) return true;   // the table's scroll bar
        for (auto* b : {&btnClear, &btnLog, &btnExport}) if (*b && (*b)->handleEvent(ev, win)) return true;
        const sf::Vector2f m = win.mapPixelToCoords(sf::Mouse::getPosition(win));
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
                    const int idx = tableFirst + row;
                    if (row >= 0 && row < tableVisible && idx < (int)entries.size()) { e = entries[idx]; found = true; }
                }
                if (found) openDetails(e);
            }
            return true;
        }
        return false;
    }

    void draw(sf::RenderWindow& win, float x, float y, float w, float h) override {
        if (!enabled || !font) return;
        panelRect = sf::FloatRect({x, y}, {w, h});
        std::vector<Entry> list;
        std::vector<std::string> chans;
        std::string info;
        int count = 0;
        bool tuned = false;
        {
            std::lock_guard<std::mutex> l(mtx);
            const int first = (int)scrollRows;
            for (int i = first; i < (int)entries.size() && i < first + 60; i++) list.push_back(entries[i]);
            tableFirst = first;
            count = (int)entries.size();
            auto line = [&](const Rx& r) {
                char b[128];
                std::snprintf(b, sizeof(b), "%-12s  %3d calls   %s", freqText(r.freqHz).c_str(), r.messages, r.rx.receiving() ? "receiving..." : "");
                chans.push_back(b);
            };
            for (auto& kv : rxs) line(*kv.second);
            if (audioRx) line(*audioRx);
            info = channelInfo;
            tuned = tunedOnly;
        }
        sf::RectangleShape bg({w, h});
        bg.setPosition({x, y});
        bg.setFillColor(sf::Color(20, 22, 27, 245));
        bg.setOutlineColor(Theme::Accent);
        bg.setOutlineThickness(1.0f);
        win.draw(bg);

        const float pad = 8.0f, sideW = 320.0f;
        tableRect = sf::FloatRect({x + pad, y + pad}, {w - sideW - 2 * pad, h - 2 * pad});
        struct Col { const char* name; float w; };
        const float fixedW = 58 + 70 + 92 + 62 + 70 + 70;
        const Col cols[] = { {"Time", 58}, {"Freq", 70}, {"Format", 92}, {"Category", 62}, {"From", 70}, {"To", 70},
                             {"Details", std::max(80.0f, tableRect.size.x - fixedW)} };
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
        { std::lock_guard<std::mutex> l(mtx); scrollRows = std::min(scrollRows, (float)std::max(0, count - visibleRows)); }
        if (list.empty()) {
            sf::Text e = text(tuned ? "Tune near 156.525 MHz (VHF channel 70) or 2187.5 kHz, or drag the channel there."
                                    : "No call yet. Coast stations and ships call now and then; near a port more often.", 24, sf::Color(110, 110, 120));
            e.setPosition({tableRect.position.x, tableBodyY + 4.0f});
            win.draw(e);
        }
        const float charW = 5.6f;
        for (int r = 0; r < visibleRows && r < (int)list.size(); r++) {
            const Entry& en = list[r];
            const maritime::DscMessage& m = en.msg;
            const float ry = tableBodyY + r * rowH;
            const bool distress = m.format == maritime::DSC_DISTRESS || m.category == maritime::DSC_CAT_DISTRESS;
            const bool urgent = m.category == maritime::DSC_URGENCY;
            if (distress || urgent || r % 2 == 0) {
                sf::RectangleShape stripe({tableRect.size.x, rowH});
                stripe.setPosition({tableRect.position.x, ry});
                stripe.setFillColor(distress ? sf::Color(170, 30, 30, 110) : urgent ? sf::Color(170, 110, 20, 90) : sf::Color(255, 255, 255, 8));
                win.draw(stripe);
            }
            const std::string cells[7] = { en.time, freqText(en.freqHz), name(maritime::dscFormatName(m.format)),
                                           m.category >= 0 ? name(maritime::dscCategoryName(m.category)) : "-", m.selfId,
                                           m.address.empty() ? "-" : m.address, summary(m) };
            float px = tableRect.position.x;
            for (int c = 0; c < 7; c++) {
                if (px + 20.0f > tableRect.position.x + tableRect.size.x) break;
                const size_t maxChars = (size_t)std::max(3.0f, (cols[c].w - 6.0f) / charW);
                sf::Text td = text(fit(cells[c], maxChars), 22, m.valid ? sf::Color(225, 225, 230) : sf::Color(140, 140, 150));
                td.setPosition({px, ry});
                win.draw(td);
                px += cols[c].w;
            }
        }
        sbTable.hide();   // laid out again below when there is a bar
        if (count > visibleRows) {
            const float trackH = tableRect.size.y - rowH - 4.0f;
            const float thumbH = std::max(16.0f, trackH * visibleRows / (float)count);
            sbTable.layout({{tableRect.position.x + tableRect.size.x - 6.0f, tableBodyY}, {4.0f, trackH}}, thumbH, scrollRows, (float)(count - visibleRows));
            sbTable.draw(win);
        }
        const float sx = x + w - sideW;
        float sy = y + pad;
        sf::Text title = text("DSC", 30, Theme::Accent);
        title.setStyle(sf::Text::Bold);
        title.setPosition({sx, sy});
        win.draw(title);
        sf::Text st = text(std::to_string(count) + " calls", 22, sf::Color(220, 220, 225));
        st.setPosition({sx + 50.0f, sy + 4.0f});
        win.draw(st);
        sy += 24.0f;
        sf::Text ci = text(info, 20, sf::Color(160, 160, 170));
        ci.setPosition({sx, sy});
        win.draw(ci);
        sy += 18.0f;
        btnClear->setPosition(sx, sy); btnClear->draw(win);
        btnLog->setPosition(sx + 106.0f, sy); btnLog->draw(win);
        btnExport->setPosition(sx + 212.0f, sy); btnExport->draw(win);
        sy += 32.0f;
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
