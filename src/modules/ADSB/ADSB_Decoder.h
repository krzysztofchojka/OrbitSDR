#pragma once

#include "../Core/SDRModule.h"
#include "../Core/PositionBus.h"
#include "AdsbCore.h"
#include <SFML/Graphics.hpp>
#include <mutex>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <algorithm>
#include <functional>

// =====================================================================
//  ADS-B decoder (aircraft on 1090 MHz) - what dump1090 does, built in.
//
//  Needs I/Q from an SDR tuned to 1090 MHz at 2.0 or 2.4 MS/s (2.048 works
//  poorly, below 2 MS/s not at all). Decodes the whole received band
//  (see AdsbCore.h), so the channel is only shown for orientation.
//  The panel lists the aircraft; positions go to the satellite map.
// =====================================================================

class ADSBDecoder : public SDRModule {
public:
    static constexpr long long ADSB_HZ = 1090000000;

    std::function<void()> onOpenMap;        // main.cpp: open the map window
    std::atomic<long long> tuneRequest {0}; // set by the panel button, main.cpp retunes the radio

    ADSBDecoder() : SDRModule("ADS-B") {
        channels.name = "ADS-B";
        channels.presets = { ADSB_HZ };
        channels.maxChannels = 1;
        channels.defaultWidth = 2000000.0;
        channels.listenMode = Mode::AM;
        channels.shape = [](DecoderChannel& c) { c.name = "ADS-B"; };
        dec.onFrame = [this](const adsb::Frame& f, const adsb::Aircraft& a) { frameDone(f, a); };   // mtx held
    }

    // main.cpp: the receiver (ground station) for distances and surface positions
    void setReceiver(double lat, double lon) {
        std::lock_guard<std::mutex> l(mtx);
        dec.refLat = lat; dec.refLon = lon; dec.refValid = !(lat == 0 && lon == 0);
    }

    void init(const ModuleContext& ctx) override {
        font = &ctx.font;
        btnClear = std::make_shared<SdrButton>(96.0f, 24.0f, "Clear list", *font);
        btnClear->onClick = [this]() {
            std::lock_guard<std::mutex> l(mtx);
            dec.aircraft.clear();
            positionBus().clear(PosKind::ADSB);
        };
        btnTune = std::make_shared<SdrButton>(130.0f, 24.0f, "Tune to 1090 MHz", *font);
        btnTune->onClick = [this]() { tuneRequest = ADSB_HZ; };
        btnMap = std::make_shared<SdrButton>(96.0f, 24.0f, "Show map", *font);
        btnMap->onClick = [this]() { if (onOpenMap) onOpenMap(); };
    }

    void reset() override {
        std::lock_guard<std::mutex> l(mtx);
        dec.reset();
    }

    float panelHeight(float windowHeight) const override { return std::clamp(windowHeight * 0.40f, 260.0f, 480.0f); }

    void setRadioFrequency(long long hz) override { std::lock_guard<std::mutex> l(mtx); centerHz = hz; }
    // short bursts on a quiet band: the software AGC must not change the gain inside a frame
    bool wantsBurstAgc() const override { return true; }
    void setAudioInput(bool audio) override { audioInput = audio; }
    ChannelSet* channelSet() override { return &channels; }

    void processIQ(const std::vector<std::complex<double>>& iq, double sampleRate, double tunedOffset) override {
        std::lock_guard<std::mutex> l(mtx);
        rate = sampleRate;
        if (audioInput) { info = "ADS-B needs I/Q from an SDR: set Input signal to I/Q"; return; }
        channels.update(centerHz, sampleRate, tunedOffset);
        if (sampleRate < 1.9e6) {
            info = "Set the sample rate to 2.4 or 2.0 MS/s (now " + mss(sampleRate) + ")";
            return;
        }
        if (centerHz > 0 && std::llabs(centerHz - ADSB_HZ) > (long long)(sampleRate * 0.3)) {
            info = "Tune the radio to 1090 MHz (button below)";
        } else if (sampleRate > 2.01e6 && sampleRate < 2.35e6) {
            info = mss(sampleRate) + ": few positions, set 2.4 MS/s or more";
        } else info = "Decoding at " + mss(sampleRate);
        dec.clock = now();
        dec.magnitude(reinterpret_cast<const double*>(iq.data()), iq.size(), mag);
        dec.process(mag.data(), mag.size(), sampleRate);
        // aircraft not heard for 5 minutes leave the table
        if (dec.clock - lastPurge > 10.0) {
            lastPurge = dec.clock;
            for (auto it = dec.aircraft.begin(); it != dec.aircraft.end();) {
                if (dec.clock - it->second.lastSeen > 300.0) it = dec.aircraft.erase(it); else ++it;
            }
        }
    }

    bool handleEvent(const sf::Event& ev, const sf::RenderWindow& win) override {
        if (!enabled) return false;
        if (btnClear && btnClear->handleEvent(ev, win)) return true;
        if (btnTune && btnTune->handleEvent(ev, win)) return true;
        if (btnMap && btnMap->handleEvent(ev, win)) return true;
        const sf::Vector2f m = win.mapPixelToCoords(sf::Mouse::getPosition(win));
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
        std::vector<adsb::Aircraft> list;
        std::string inf;
        long long frames = 0, fixed = 0;
        double refLat = 0, refLon = 0; bool refOk = false;
        const double t = now();
        {
            std::lock_guard<std::mutex> l(mtx);
            for (auto& kv : dec.aircraft) list.push_back(kv.second);
            inf = info;
            frames = dec.goodFrames; fixed = dec.fixedFrames;
            refLat = dec.refLat; refLon = dec.refLon; refOk = dec.refValid;
        }
        // messages per second (smoothed)
        if (t - rateT > 1.0) {
            const double r = (frames - rateFrames) / (t - rateT);
            msgRate = msgRate < 0 ? r : msgRate * 0.6 + r * 0.4;
            rateFrames = frames; rateT = t;
        }
        // fixed order: as first heard (new aircraft join at the bottom, rows stay where they are;
        // an aircraft not heard for 5 minutes leaves the list)
        std::sort(list.begin(), list.end(), [](const adsb::Aircraft& a, const adsb::Aircraft& b) {
            if (a.firstSeen != b.firstSeen) return a.firstSeen < b.firstSeen;
            return a.icao < b.icao;
        });

        sf::RectangleShape bg({w, h});
        bg.setPosition({x, y});
        bg.setFillColor(sf::Color(20, 22, 27, 245));
        bg.setOutlineColor(Theme::Accent);
        bg.setOutlineThickness(1.0f);
        win.draw(bg);

        const float pad = 8.0f, sideW = 300.0f;
        tableRect = sf::FloatRect({x + pad, y + pad}, {w - sideW - 2 * pad, h - 2 * pad});
        struct Col { const char* name; float w; };
        const Col cols[] = { {"ICAO", 58}, {"Flight", 74}, {"Squawk", 54}, {"Alt (ft)", 64}, {"Speed", 50}, {"Track", 46},
                             {"V/S", 56}, {"Latitude", 76}, {"Longitude", 80}, {"Dist km", 56}, {"dB", 40}, {"Msgs", 44}, {"Seen", 40} };
        constexpr int NC = 13;
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
            sf::Text e = text("No aircraft received yet", 26, sf::Color(110, 110, 120));
            e.setPosition({tableRect.position.x, bodyY + 4.0f});
            win.draw(e);
        }
        for (int r = 0; r < visibleRows && first + r < (int)list.size(); r++) {
            const adsb::Aircraft& a = list[first + r];
            const float ry = bodyY + r * rowH;
            if (r % 2 == 0) {
                sf::RectangleShape stripe({tableRect.size.x, rowH});
                stripe.setPosition({tableRect.position.x, ry});
                stripe.setFillColor(sf::Color(255, 255, 255, 8));
                win.draw(stripe);
            }
            char b[32];
            std::string c[NC];
            std::snprintf(b, sizeof(b), "%06X", a.icao); c[0] = b;
            c[1] = a.callsign.empty() ? "-" : a.callsign;
            c[2] = a.squawk.empty() ? "-" : a.squawk;
            if (a.onGround) c[3] = "ground";
            else if (a.altFt != adsb::NO_ALT) { std::snprintf(b, sizeof(b), "%d%s", a.altFt, a.altGnss ? "g" : ""); c[3] = b; }
            else c[3] = "-";
            if (a.speedKn >= 0) { std::snprintf(b, sizeof(b), "%.0f", a.speedKn); c[4] = b; } else c[4] = "-";
            if (a.trackDeg >= 0) { std::snprintf(b, sizeof(b), "%.0f", a.trackDeg); c[5] = b; } else c[5] = "-";
            if (a.vrateValid) { std::snprintf(b, sizeof(b), "%+d", a.vrateFpm); c[6] = b; } else c[6] = "-";
            if (a.posValid) {
                std::snprintf(b, sizeof(b), "%.4f", a.lat); c[7] = b;
                std::snprintf(b, sizeof(b), "%.4f", a.lon); c[8] = b;
                if (refOk) { std::snprintf(b, sizeof(b), "%.0f", adsb::distanceKm(refLat, refLon, a.lat, a.lon)); c[9] = b; } else c[9] = "-";
            } else { c[7] = c[8] = c[9] = "-"; }
            std::snprintf(b, sizeof(b), "%.0f", a.signalDb); c[10] = b;
            c[11] = std::to_string(a.messages);
            const int age = (int)std::max(0.0, t - a.lastSeen);
            std::snprintf(b, sizeof(b), "%ds", age); c[12] = b;
            const bool fresh = age < 30;
            const sf::Color col = !fresh ? sf::Color(130, 130, 140) : a.squawk == "7500" || a.squawk == "7600" || a.squawk == "7700"
                                  ? sf::Color(255, 90, 80) : sf::Color(225, 225, 230);
            float px = tableRect.position.x;
            for (int k = 0; k < NC; k++) {
                if (px + cols[k].w > tableRect.position.x + tableRect.size.x) break;
                sf::Text td = text(c[k], 22, col);
                td.setPosition({px, ry});
                win.draw(td);
                px += cols[k].w;
            }
        }
        if ((int)list.size() > visibleRows) {
            const float trackH = tableRect.size.y - rowH - 4.0f;
            const float thumbH = std::max(16.0f, trackH * visibleRows / (float)list.size());
            const float maxFirst = (float)(list.size() - visibleRows);
            sf::RectangleShape thumb({4.0f, thumbH});
            thumb.setPosition({tableRect.position.x + tableRect.size.x - 6.0f, bodyY + (trackH - thumbH) * (first / maxFirst)});
            thumb.setFillColor(sf::Color(200, 200, 200, 140));
            win.draw(thumb);
        }

        // --- side column ---
        const float sx = x + w - sideW;
        float sy = y + pad;
        sf::Text title = text("ADS-B", 30, Theme::Accent);
        title.setStyle(sf::Text::Bold);
        title.setPosition({sx, sy});
        win.draw(title);
        int withPos = 0;
        for (auto& a : list) if (a.posValid) withPos++;
        char st[128];
        std::snprintf(st, sizeof(st), "%zu aircraft (%d with position)", list.size(), withPos);
        sf::Text s1 = text(st, 22, sf::Color(220, 220, 225));
        s1.setPosition({sx + 70.0f, sy + 4.0f});
        win.draw(s1);
        sy += 26.0f;
        std::snprintf(st, sizeof(st), "%.1f messages/s   %lld total (%lld repaired)", std::max(0.0, msgRate), frames, fixed);
        sf::Text s2 = text(st, 20, sf::Color(170, 170, 180));
        s2.setPosition({sx, sy});
        win.draw(s2);
        sy += 18.0f;
        sf::Text s3 = text(inf, 20, inf.rfind("Decoding", 0) == 0 ? sf::Color(120, 200, 140) : sf::Color(255, 170, 90));
        s3.setPosition({sx, sy});
        win.draw(s3);
        sy += 22.0f;
        btnTune->setPosition(sx, sy);
        btnTune->draw(win);
        btnMap->setPosition(sx + 136.0f, sy);
        btnMap->draw(win);
        sy += 30.0f;
        btnClear->setPosition(sx, sy);
        btnClear->draw(win);
        sy += 34.0f;
        const char* help[] = {
            "Antenna: a 1090 MHz ground plane or collinear,",
            "high gain (AGC off), 2.4 MS/s. Range grows with height.",
            "Positions need the CPR pair: a few seconds per aircraft.",
            "Red: emergency squawk (7500 / 7600 / 7700).",
        };
        for (const char* hl : help) {
            if (sy > y + h - 16.0f) break;
            sf::Text ht = text(hl, 20, sf::Color(130, 130, 140));
            ht.setPosition({sx, sy});
            win.draw(ht);
            sy += 15.0f;
        }
    }

private:
    std::mutex mtx;
    adsb::Decoder dec;
    std::vector<float> mag;
    ChannelSet channels;
    long long centerHz = 0;
    double rate = 0;
    std::string info = "Waiting for signal...";
    std::atomic<bool> audioInput {false};
    double lastPurge = 0;
    std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();

    const sf::Font* font = nullptr;
    sf::FloatRect panelRect, tableRect;
    float scrollRows = 0.0f;
    std::shared_ptr<SdrButton> btnClear, btnTune, btnMap;
    long long rateFrames = 0;
    double rateT = 0, msgRate = -1;

    double now() const { return std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count(); }
    static std::string mss(double r) { char b[32]; std::snprintf(b, sizeof(b), "%.3g MS/s", r / 1e6); return b; }

    sf::Text text(const std::string& s, unsigned size, sf::Color c) const {
        sf::Text t(*font, sf::String::fromUtf8(s.begin(), s.end()), size);
        t.setScale({0.5f, 0.5f});
        t.setFillColor(c);
        return t;
    }

    // New position (or new details) -> the satellite map
    void frameDone(const adsb::Frame&, const adsb::Aircraft& a) {
        char id[8];
        std::snprintf(id, sizeof(id), "%06X", a.icao);
        std::string label = a.callsign.empty() ? std::string(id) : a.callsign;
        std::string inf;
        if (a.onGround) inf = "on ground";
        else if (a.altFt != adsb::NO_ALT) inf = std::to_string(a.altFt) + " ft";
        if (a.vrateValid && std::abs(a.vrateFpm) >= 300) inf += (a.vrateFpm > 0 ? "  climbing " : "  descending ") + std::to_string(std::abs(a.vrateFpm)) + " fpm";
        if (!a.squawk.empty()) inf += "  squawk " + a.squawk;
        if (!a.category.empty()) inf += "  " + a.category;
        if (a.posValid && a.posTime == dec.clock) {
            PosReport r;
            r.kind = PosKind::ADSB;
            r.id = id; r.label = label; r.info = inf.empty() ? std::string("ICAO ") + id : inf;
            r.lat = a.lat; r.lon = a.lon;
            r.speedKn = a.speedKn;
            r.courseDeg = a.trackDeg;
            if (a.altFt != adsb::NO_ALT) r.altM = a.altFt * 0.3048;
            positionBus().report(r);
        } else if (a.posValid) {
            positionBus().describe(PosKind::ADSB, id, label, inf);
        }
    }
};
