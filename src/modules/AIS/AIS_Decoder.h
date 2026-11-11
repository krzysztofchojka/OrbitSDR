#pragma once

#include "../Core/SDRModule.h"
#include "AisCore.h"
#include <SFML/Graphics.hpp>
#include <mutex>
#include <deque>
#include <chrono>
#include <cstdio>
#include <algorithm>
#include <atomic>

// =====================================================================
//  AIS decoder module (ship tracking on VHF 161.975 / 162.025 MHz).
//
//  Works on the raw IQ samples, independent of the audio demodulator.
//  When both AIS channels fit into the received bandwidth they are
//  decoded at the same time; otherwise the tuned frequency is decoded.
//  The panel shows a table of the ships heard and the latest messages.
// =====================================================================

class AISDecoder : public SDRModule {
    static constexpr long long AIS1_HZ = 161975000;
    static constexpr long long AIS2_HZ = 162025000;

    struct Rx {
        ais::Channel channel;
        ais::Decoder decoder;
        bool active = false;
        double offsetHz = 0.0;
        int frames = 0;
    };
    Rx rx[2];
    std::vector<float> disc, power;

    std::mutex mtx;
    std::map<uint32_t, ais::Vessel> vessels;
    std::deque<std::string> recentMessages;
    long long centerHz = 0;
    std::string channelInfo = "Waiting for signal...";
    std::atomic<bool> audioInput {false};
    ChannelSet channels;   // the two AIS channels (or the tuner, see DecoderChannels.h)
    std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();

    const sf::Font* font = nullptr;
    sf::FloatRect panelRect, tableRect;
    float scrollRows = 0.0f;
    std::shared_ptr<SdrButton> btnClear;

    double now() const {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
    }

    sf::Text text(const std::string& s, unsigned size, sf::Color c) const {
        sf::Text t(*font, sf::String::fromUtf8(s.begin(), s.end()), size);
        t.setScale({0.5f, 0.5f});
        t.setFillColor(c);
        return t;
    }

    static std::string fmtCoord(double v, bool lat) {
        if ((lat && v > 90.0) || (!lat && v > 180.0)) return "-";
        char b[24];
        std::snprintf(b, sizeof(b), "%.5f%c", std::abs(v), lat ? (v >= 0 ? 'N' : 'S') : (v >= 0 ? 'E' : 'W'));
        return b;
    }

public:
    AISDecoder() : SDRModule("AIS") {
        channels.name = "AIS";
        channels.presets = { AIS1_HZ, AIS2_HZ };
        channels.maxChannels = 2;
        channels.defaultWidth = 18000.0;   // +-9 kHz channel filter
        channels.listenMode = Mode::NFM;
        channels.shape = [](DecoderChannel& c) {
            c.name = std::llabs(c.freqHz - AIS1_HZ) < 1000 ? "AIS 1" : std::llabs(c.freqHz - AIS2_HZ) < 1000 ? "AIS 2" : "AIS";
        };
        for (int i = 0; i < 2; i++) {
            rx[i].decoder.sharedVessels = &vessels;
            rx[i].decoder.onMessage = [this, i](const ais::Message& m) {
                // Called from processIQ with mtx held
                rx[i].frames++;
                char head[48];
                std::snprintf(head, sizeof(head), "%s %09u  ", i == 0 ? "A" : "B", m.mmsi);
                recentMessages.push_front(head + m.summary);
                if (recentMessages.size() > 30) recentMessages.pop_back();
            };
        }
    }

    void init(const ModuleContext& ctx) override {
        font = &ctx.font;
        btnClear = std::make_shared<SdrButton>(120.0f, 24.0f, "Clear list", *font);
        btnClear->onClick = [this]() {
            std::lock_guard<std::mutex> l(mtx);
            vessels.clear(); recentMessages.clear();
            rx[0].frames = rx[1].frames = 0;
        };
    }

    void reset() override {
        std::lock_guard<std::mutex> l(mtx);
        for (auto& r : rx) r.decoder.reset();
    }

    float panelHeight(float windowHeight) const override {
        return std::clamp(windowHeight * 0.38f, 260.0f, 460.0f);
    }

    void setRadioFrequency(long long hz) override {
        std::lock_guard<std::mutex> l(mtx);
        centerHz = hz;
    }

    void processIQ(const std::vector<std::complex<double>>& iq, double sampleRate, double tunedOffset) override {
        if (!enabled || iq.empty() || sampleRate <= 0.0) return;
        std::lock_guard<std::mutex> l(mtx);
        if (audioInput) {   // AIS needs the I/Q of the two channels
            rx[0].active = rx[1].active = false;
            channelInfo = "AIS needs I/Q from an SDR: set Input signal to I/Q";
            return;
        }

        // Both AIS channels when they fit into the band, else the tuner (see ChannelSet)
        const auto rc = channels.update(centerHz, sampleRate, tunedOffset);
        for (int i = 0; i < 2; i++) {
            rx[i].active = i < (int)rc.size() && rc[i].inBand;
            if (i < (int)rc.size()) rx[i].offsetHz = rc[i].offsetHz;
        }
        channelInfo = channels.describe();

        const double t = now();
        for (auto& r : rx) {
            if (!r.active) continue;
            r.channel.configure(sampleRate, r.offsetHz);
            disc.clear(); power.clear();
            r.channel.process(iq.data(), iq.size(), disc, power);
            r.decoder.clockSeconds = t;
            r.decoder.process(disc.data(), power.data(), disc.size());
        }
    }

    void setAudioInput(bool audio) override { audioInput = audio; }

    ChannelSet* channelSet() override { return &channels; }

    bool handleEvent(const sf::Event& ev, const sf::RenderWindow& win) override {
        if (!enabled) return false;
        if (btnClear && btnClear->handleEvent(ev, win)) return true;
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

        std::vector<ais::Vessel> list;
        std::deque<std::string> msgs;
        std::string info;
        int frames = 0;
        {
            std::lock_guard<std::mutex> l(mtx);
            for (auto& kv : vessels) list.push_back(kv.second);
            msgs = recentMessages;
            info = channelInfo;
            frames = rx[0].frames + rx[1].frames;
        }
        std::sort(list.begin(), list.end(), [](const ais::Vessel& a, const ais::Vessel& b) { return a.lastSeen > b.lastSeen; });
        const double t = now();

        sf::RectangleShape bg({w, h});
        bg.setPosition({x, y});
        bg.setFillColor(sf::Color(20, 22, 27, 245));
        bg.setOutlineColor(Theme::Accent);
        bg.setOutlineThickness(1.0f);
        win.draw(bg);

        const float pad = 8.0f;
        const float sideW = 330.0f;
        tableRect = sf::FloatRect({x + pad, y + pad}, {w - sideW - 2 * pad, h - 2 * pad});

        // --- ship table ---
        struct Col { const char* name; float w; };
        const Col cols[] = { {"MMSI", 78}, {"Name", 150}, {"Call", 62}, {"Type", 100}, {"Latitude", 82},
                             {"Longitude", 86}, {"SOG", 46}, {"COG", 46}, {"Seen", 50}, {"Msgs", 40} };
        float cx = tableRect.position.x;
        const float rowH = 16.0f;
        for (const Col& c : cols) {
            sf::Text th = text(c.name, 22, Theme::Accent);
            th.setStyle(sf::Text::Bold);
            th.setPosition({cx, tableRect.position.y});
            if (cx + c.w <= tableRect.position.x + tableRect.size.x) win.draw(th);
            cx += c.w;
        }
        const float bodyY = tableRect.position.y + rowH + 4.0f;
        const int visibleRows = std::max(1, (int)((tableRect.size.y - rowH - 4.0f) / rowH));
        scrollRows = std::min(scrollRows, (float)std::max(0, (int)list.size() - visibleRows));
        const int first = (int)scrollRows;

        if (list.empty()) {
            sf::Text e = text("No ships received yet", 26, sf::Color(110, 110, 120));
            e.setPosition({tableRect.position.x, bodyY + 4.0f});
            win.draw(e);
        }
        for (int r = 0; r < visibleRows && first + r < (int)list.size(); r++) {
            const ais::Vessel& v = list[first + r];
            const float ry = bodyY + r * rowH;
            if (r % 2 == 0) {
                sf::RectangleShape stripe({tableRect.size.x, rowH});
                stripe.setPosition({tableRect.position.x, ry});
                stripe.setFillColor(sf::Color(255, 255, 255, 8));
                win.draw(stripe);
            }
            char b[16];
            std::string cells[10];
            std::snprintf(b, sizeof(b), "%09u", v.mmsi); cells[0] = b;
            cells[1] = v.name.empty() ? "-" : v.name;
            cells[2] = v.callsign.empty() ? "-" : v.callsign;
            cells[3] = v.shipType.empty() ? "-" : v.shipType;
            cells[4] = fmtCoord(v.lat, true);
            cells[5] = fmtCoord(v.lon, false);
            if (v.sog >= 0) { std::snprintf(b, sizeof(b), "%.1f", v.sog); cells[6] = b; } else cells[6] = "-";
            if (v.cog >= 0) { std::snprintf(b, sizeof(b), "%.0f", v.cog); cells[7] = b; } else cells[7] = "-";
            int age = (int)std::max(0.0, t - v.lastSeen);
            if (age < 60) std::snprintf(b, sizeof(b), "%ds", age); else std::snprintf(b, sizeof(b), "%dm", age / 60);
            cells[8] = b;
            cells[9] = std::to_string(v.messages);
            const bool fresh = age < 120;
            float px = tableRect.position.x;
            for (int c = 0; c < 10; c++) {
                if (px + cols[c].w > tableRect.position.x + tableRect.size.x) break;
                std::string s = cells[c];
                if (c == 1 && s.size() > 20) s = s.substr(0, 19) + ".";
                if (c == 3 && s.size() > 14) s = s.substr(0, 13) + ".";
                sf::Text td = text(s, 22, fresh ? sf::Color(225, 225, 230) : sf::Color(130, 130, 140));
                td.setPosition({px, ry});
                win.draw(td);
                px += cols[c].w;
            }
        }
        if ((int)list.size() > visibleRows) {
            float trackH = tableRect.size.y - rowH - 4.0f;
            float thumbH = std::max(16.0f, trackH * visibleRows / (float)list.size());
            float maxFirst = (float)(list.size() - visibleRows);
            sf::RectangleShape thumb({4.0f, thumbH});
            thumb.setPosition({tableRect.position.x + tableRect.size.x - 6.0f, bodyY + (trackH - thumbH) * (first / maxFirst)});
            thumb.setFillColor(sf::Color(200, 200, 200, 140));
            win.draw(thumb);
        }

        // --- side column: status and latest messages ---
        const float sx = x + w - sideW;
        float sy = y + pad;
        sf::Text title = text("AIS", 30, Theme::Accent);
        title.setStyle(sf::Text::Bold);
        title.setPosition({sx, sy});
        win.draw(title);
        char stats[96];
        std::snprintf(stats, sizeof(stats), "%zu ships, %d messages", list.size(), frames);
        sf::Text st = text(stats, 22, sf::Color(220, 220, 225));
        st.setPosition({sx + 50.0f, sy + 4.0f});
        win.draw(st);
        sy += 24.0f;
        sf::Text ci = text(info, 20, sf::Color(160, 160, 170));
        ci.setPosition({sx, sy});
        win.draw(ci);
        sy += 18.0f;
        btnClear->setPosition(sx, sy);
        btnClear->draw(win);
        sy += 30.0f;
        sf::Text lt = text("Latest messages", 22, Theme::Accent);
        lt.setPosition({sx, sy});
        win.draw(lt);
        sy += 18.0f;
        for (const auto& m : msgs) {
            if (sy > y + h - 16.0f) break;
            std::string s = m.size() > 52 ? m.substr(0, 51) + "." : m;
            sf::Text mt = text(s, 20, sf::Color(200, 200, 205));
            mt.setPosition({sx, sy});
            win.draw(mt);
            sy += 15.0f;
        }
    }
};
