#pragma once

#include "../Core/SDRModule.h"
#include "../Core/DetailsPopup.h"
#include "../Core/ModuleLog.h"
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
//  NAVTEX decoder module: maritime safety information on 518 kHz
//  (international, English), 490 kHz (national language) and 4209.5 kHz.
//  All of them inside the received band are decoded at once (e.g. a
//  direct sampling RTL-SDR or an SDRplay around 500 kHz).
//  Audio input: a receiver in USB, the NAVTEX tones around 1700 Hz.
// =====================================================================

class NAVTEXDecoder : public SDRModule {
    struct Entry {
        std::string time;
        long long freqHz = 0;
        maritime::NavtexMessage msg;
    };
    struct Rx {
        long long freqHz = 0;
        NarrowChannel ch;
        maritime::NavtexReceiver rx;
        std::vector<std::complex<float>> tmp;
        std::string live;   // the text being received
        int messages = 0;
    };

    static constexpr long long AUDIO_FREQ = -1;
    static constexpr double AUDIO_CENTRE = 1700.0;

    std::mutex mtx;
    std::map<int, std::unique_ptr<Rx>> rxs;
    std::unique_ptr<Rx> audioRx;
    ChannelSet channels;
    std::deque<Entry> entries;   // newest first
    long long centerHz = 0;
    double lastRate = 0.0, audioRate = 48000.0;
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
    static std::string khz(long long hz) {
        if (hz == AUDIO_FREQ) return "audio";
        char b[24];
        std::snprintf(b, sizeof(b), "%.1f", hz / 1e3);
        return b;
    }
    sf::Text text(const std::string& s, unsigned size, sf::Color c) const {
        sf::Text t(*font, sf::String::fromUtf8(s.begin(), s.end()), size);
        t.setScale({0.5f, 0.5f});
        t.setFillColor(c);
        return t;
    }
    static std::string fit(const std::string& s, size_t n) { return s.size() > n ? s.substr(0, n - 1) + "." : s; }
    static std::string firstLine(const std::string& s) {
        std::string o;
        for (char c : s) { if (c == '\n') { if (!o.empty()) break; continue; } o += c; }
        return o;
    }

    void wire(Rx* r) {
        r->rx.onMessage = [this, r](const maritime::NavtexMessage& m) {   // mtx held
            r->messages++;
            Entry e;
            e.time = wallTime();
            e.freqHz = r->freqHz;
            e.msg = m;
            entries.push_front(e);
            if (entries.size() > 500) entries.pop_back();
            std::string flat = m.text;
            for (char& c : flat) if (c == '\n') c = ' ';
            char b[64];
            std::snprintf(b, sizeof(b), "%s kHz  %c%c%s  ", khz(r->freqHz).c_str(), m.station, m.type, m.number.c_str());
            log.add(std::string(b) + flat);
        };
        r->rx.onLive = [r](const std::string& t) { r->live = t; };
    }

    void openDetails(const Entry& e) {
        const maritime::NavtexMessage& m = e.msg;
        std::vector<std::pair<std::string, std::string>> rows;
        rows.push_back({"Received", e.time});
        rows.push_back({"Frequency", e.freqHz == AUDIO_FREQ ? std::string("audio input") : khz(e.freqHz) + " kHz"});
        if (m.valid) {
            rows.push_back({"Station", std::string(1, m.station) + "  (transmitter letter of the NAVAREA)"});
            const char* tn = maritime::navtexTypeName(m.type);
            rows.push_back({"Subject", std::string(1, m.type) + (*tn ? std::string("  ") + tn : "")});
            rows.push_back({"Number", m.number});
        }
        rows.push_back({"Character errors", std::to_string(m.errors) + " (corrected with the repeated characters where possible)"});
        popup.show("NAVTEX " + (m.valid ? std::string(1, m.station) + std::string(1, m.type) + m.number : std::string("message")), rows, m.text);
    }

public:
    ModuleLog log;
    std::string exportDir;

    NAVTEXDecoder() : SDRModule("NAVTEX") {
        channels.name = "NAVTEX";
        channels.presets = { 518000, 490000, 4209500 };
        channels.maxChannels = 3;
        channels.defaultWidth = 400.0;   // +-85 Hz tones and some margin
        channels.listenMode = Mode::CW;  // a beat note on each tone (615 / 785 Hz)
        channels.shape = [](DecoderChannel& c) {
            char b[32];
            std::snprintf(b, sizeof(b), "NAVTEX %.1f", c.freqHz / 1e3);
            c.name = c.followVfo ? std::string("NAVTEX") : std::string(b);
        };
    }

    void init(const ModuleContext& ctx) override {
        font = &ctx.font;
        audioRate = ctx.sampleRate > 0.0f ? ctx.sampleRate : 48000.0;
        popup.init(*font);
        btnClear = std::make_shared<SdrButton>(100.0f, 24.0f, "Clear list", *font);
        btnClear->onClick = [this]() { std::lock_guard<std::mutex> l(mtx); entries.clear(); for (auto& kv : rxs) kv.second->messages = 0; };
        btnLog = std::make_shared<SdrButton>(100.0f, 24.0f, "Open log", *font);
        btnLog->onClick = [this]() { log.openLogFile(); };
        btnExport = std::make_shared<SdrButton>(100.0f, 24.0f, "Export", *font);
        btnExport->onClick = [this]() { log.exportSession(exportDir, "NAVTEX"); };
    }

    void reset() override { std::lock_guard<std::mutex> l(mtx); rxs.clear(); audioRx.reset(); }
    float panelHeight(float windowHeight) const override { return std::clamp(windowHeight * 0.36f, 240.0f, 440.0f); }
    void setRadioFrequency(long long hz) override { std::lock_guard<std::mutex> l(mtx); centerHz = hz; }
    void setAudioInput(bool a) override { audioInput = a; }
    ChannelSet* channelSet() override { return &channels; }
    bool modalOpen() const override { return popup.isOpen(); }
    void drawModal(sf::RenderWindow& win) override { popup.draw(win); }

    void processAudio(const std::vector<float>& audio) override {
        if (!enabled || !audioInput || audio.empty()) return;
        std::lock_guard<std::mutex> l(mtx);
        if (!audioRx) { audioRx = std::make_unique<Rx>(); audioRx->freqHz = AUDIO_FREQ; wire(audioRx.get()); }
        std::vector<std::complex<double>> c(audio.size());
        for (size_t i = 0; i < audio.size(); i++) c[i] = std::complex<double>(audio[i], 0.0);
        audioRx->ch.configure(audioRate, AUDIO_CENTRE, 1000.0, 250.0, 150.0);
        audioRx->tmp.clear();
        audioRx->ch.process(c.data(), c.size(), audioRx->tmp);
        audioRx->rx.process(audioRx->tmp.data(), audioRx->tmp.size());
        channelInfo = "Input signal: Audio (USB, tones around 1700 Hz)";
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
            if (r.freqHz != c.freqHz) { r.rx.reset(); r.freqHz = c.freqHz; }
            if (!c.inBand) continue;
            r.ch.configure(sampleRate, c.offsetHz, 1000.0, 250.0, 150.0);
            r.tmp.clear();
            r.ch.process(iq.data(), iq.size(), r.tmp);
            r.rx.process(r.tmp.data(), r.tmp.size());
        }
        channelInfo = tunedOnly ? "No NAVTEX frequency in band: decoding at the tuner" : std::to_string(rxs.size()) + " channel(s)";
    }

    bool handleEvent(const sf::Event& ev, const sf::RenderWindow& win) override {
        if (!enabled) return false;
        if (popup.handleEvent(ev, win)) return true;
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
        std::string info, live;
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
                std::snprintf(b, sizeof(b), "%s%s   %3d msgs   %s", khz(r.freqHz).c_str(), r.freqHz == AUDIO_FREQ ? "" : " kHz", r.messages,
                              r.rx.receiving() ? "receiving..." : "idle");
                chans.push_back(b);
                if (r.rx.receiving() && !r.live.empty()) live = r.live;
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

        const float pad = 8.0f, sideW = 320.0f, liveH = 34.0f;
        tableRect = sf::FloatRect({x + pad, y + pad}, {w - sideW - 2 * pad, h - 2 * pad - liveH});
        struct Col { const char* name; float w; };
        const float fixedW = 58 + 52 + 34 + 150 + 30;
        const Col cols[] = { {"Time", 58}, {"kHz", 52}, {"Stn", 34}, {"Subject", 150}, {"Nr", 30}, {"Message", std::max(80.0f, tableRect.size.x - fixedW)} };
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
            sf::Text e = text(tuned ? "Tune near 518 kHz (direct sampling / HF antenna) or drag the channel there. Broadcasts every 4 h per station."
                                    : "No message yet: each station broadcasts in its time slot (every 4 hours, plus urgent warnings).", 24, sf::Color(110, 110, 120));
            e.setPosition({tableRect.position.x, tableBodyY + 4.0f});
            win.draw(e);
        }
        const float charW = 5.6f;
        for (int r = 0; r < visibleRows && r < (int)list.size(); r++) {
            const Entry& en = list[r];
            const maritime::NavtexMessage& m = en.msg;
            const float ry = tableBodyY + r * rowH;
            if (r % 2 == 0) {
                sf::RectangleShape stripe({tableRect.size.x, rowH});
                stripe.setPosition({tableRect.position.x, ry});
                stripe.setFillColor(sf::Color(255, 255, 255, 8));
                win.draw(stripe);
            }
            const std::string cells[6] = { en.time, khz(en.freqHz), m.valid ? std::string(1, m.station) : "?",
                                           m.valid ? std::string(1, m.type) + " " + maritime::navtexTypeName(m.type) : "?",
                                           m.number, firstLine(m.text) };
            const bool warn = m.type == 'A' || m.type == 'B' || m.type == 'D' || m.type == 'L';
            float px = tableRect.position.x;
            for (int c = 0; c < 6; c++) {
                if (px + 20.0f > tableRect.position.x + tableRect.size.x) break;
                const size_t maxChars = (size_t)std::max(3.0f, (cols[c].w - 6.0f) / charW);
                sf::Color col = (c == 3 && warn) ? sf::Color(255, 190, 110) : sf::Color(225, 225, 230);
                sf::Text td = text(fit(cells[c], maxChars), 22, col);
                td.setPosition({px, ry});
                win.draw(td);
                px += cols[c].w;
            }
        }
        // the characters being received right now
        {
            std::string t = live;
            for (char& c : t) if (c == '\n' || c == '\r') c = ' ';
            const size_t maxChars = (size_t)std::max(20.0f, tableRect.size.x / 6.2f);
            if (t.size() > maxChars) t = t.substr(t.size() - maxChars);
            sf::Text lt = text(t.empty() ? "(nothing being received)" : t, 24, t.empty() ? sf::Color(100, 100, 110) : sf::Color(120, 235, 140));
            lt.setPosition({tableRect.position.x, y + h - pad - liveH + 10.0f});
            win.draw(lt);
        }
        const float sx = x + w - sideW;
        float sy = y + pad;
        sf::Text title = text("NAVTEX", 30, Theme::Accent);
        title.setStyle(sf::Text::Bold);
        title.setPosition({sx, sy});
        win.draw(title);
        sf::Text st = text(std::to_string(count) + " messages", 22, sf::Color(220, 220, 225));
        st.setPosition({sx + 80.0f, sy + 4.0f});
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
        sf::Text lt = text("Channels (click a table row for the full text)", 22, Theme::Accent);
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
