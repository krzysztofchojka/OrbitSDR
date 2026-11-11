#pragma once

#include "../Core/SDRModule.h"
#include "IsmChannel.h"
#include "../Core/DetailsPopup.h"
#include "../Core/ModuleLog.h"
#ifdef ENABLE_RTL433
#include "rtl433_bridge.h"
#endif
#include <SFML/Graphics.hpp>
#include <mutex>
#include <deque>
#include <map>
#include <vector>
#include <string>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <fstream>
#include <algorithm>
#include <functional>
#include <atomic>

// =====================================================================
//  ISM band decoder (433.92 / 868 / 315 / 915 MHz) based on rtl_433.
//
//  Weather stations, tyre pressure sensors (TPMS), thermometers, door and
//  window sensors, remotes, utility meters... rtl_433 knows ~300 devices.
//
//  Works on the raw IQ samples, independent of the audio demodulator:
//  the band around the tuned frequency (250 kHz or 1 MHz wide) is cut out,
//  resampled and pushed into the rtl_433 processing chain. The panel lists
//  the devices heard; clicking a row shows all fields of its last message.
// =====================================================================

class ISMDecoder : public SDRModule {
    struct Field { std::string key, label, value; };
    struct Device {
        std::string key;          // model|id|channel
        std::string model, id, channel, summary, rssi;
        std::vector<Field> fields;
        double lastSeen = 0.0;
        std::string lastTime;
        int count = 0;
        bool uncertain = false;   // from a decoder rtl_433 disables by default
    };

    std::mutex mtx;
    std::map<std::string, Device> devices;
    std::deque<std::string> recentEvents;
    ism::Channel channel;
    std::vector<int16_t> cs16;
    double outRate = 250000.0;
    double activeRate = 0.0;
    long long centerHz = 0;
    long long decodeHz = 0;
    int events = 0;
    std::string status = "Waiting for signal...";
    std::string diag;
    bool allDecoders = false;     // + the decoders rtl_433 disables by default
    bool unknownSignals = false;  // + packages nobody decoded (guessed modulation and bits)
    std::atomic<bool> audioInput {false};
    ChannelSet channels;   // the decoded band (see DecoderChannels.h)
    std::string selectedKey;
    bool rateChanged = false;
    std::chrono::steady_clock::time_point startTime = std::chrono::steady_clock::now();
#ifdef ENABLE_RTL433
    ism_bridge* bridge = nullptr;
#endif

    const sf::Font* font = nullptr;
    sf::FloatRect panelRect, tableRect;
    float scrollRows = 0.0f;
    float tableBodyY = 0.0f, tableRowH = 16.0f;
    int tableFirst = 0, tableVisible = 0;
    std::vector<std::string> drawnKeys;   // device keys in table order (for clicks)
    std::shared_ptr<SdrButton> btnClear, btnRate, btnDecoders, btnUnknown, btnLog, btnExport;
    DetailsPopup popup;

    // meta fields are shown in their own columns or not at all
    static bool isMeta(const std::string& k) {
        static const char* meta[] = { "model", "id", "channel", "protocol", "mod", "freq", "freq1", "freq2",
                                      "rssi", "snr", "noise", "mic", "time", "subtype",
                                      "note", "modulation", "timing", "pulses" };
        for (const char* m : meta) if (k == m) return true;
        return false;
    }

    double now() const {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
    }

    static std::string wallTime(bool withDate) {
        std::time_t t = std::time(nullptr);
        std::tm tm = *std::localtime(&t);
        char b[32];
        std::strftime(b, sizeof(b), withDate ? "%Y-%m-%d %H:%M:%S" : "%H:%M:%S", &tm);
        return b;
    }

    sf::Text text(const std::string& s, unsigned size, sf::Color c) const {
        sf::Text t(*font, sf::String::fromUtf8(s.begin(), s.end()), size);
        t.setScale({0.5f, 0.5f});
        t.setFillColor(c);
        return t;
    }

    static std::string fit(const std::string& s, size_t n) {
        return s.size() > n ? s.substr(0, n - 1) + "." : s;
    }

#ifdef ENABLE_RTL433
    static void eventThunk(void* user, int n, const char* const* keys, const char* const* labels, const char* const* values) {
        static_cast<ISMDecoder*>(user)->onDecoded(n, keys, labels, values);
    }

    void destroyBridge() {
        if (bridge) { ism_destroy(bridge); bridge = nullptr; }
        activeRate = 0.0;
    }
#endif

    // Called from processIQ (mtx is held)
    void onDecoded(int n, const char* const* keys, const char* const* labels, const char* const* values) {
        std::vector<Field> fields;
        std::string model, id, chan, rssi, protocol;
        for (int i = 0; i < n; i++) {
            Field f{keys[i] ? keys[i] : "", labels[i] ? labels[i] : "", values[i] ? values[i] : ""};
            if (f.key == "model") model = f.value;
            else if (f.key == "id") id = f.value;
            else if (f.key == "channel") chan = f.value;
            else if (f.key == "rssi") rssi = f.value;
            else if (f.key == "protocol") protocol = f.value;
            fields.push_back(std::move(f));
        }
        if (model.empty()) return;

        std::string summary;
        for (const Field& f : fields) {
            if (isMeta(f.key)) continue;
            if (!summary.empty()) summary += "  ";
            summary += f.label + ": " + f.value;
        }

        const std::string key = model + "|" + id + "|" + chan;
        Device& d = devices[key];
        d.key = key; d.model = model; d.id = id; d.channel = chan; d.rssi = rssi;
        d.summary = summary; d.fields = fields;
        d.uncertain = false;
        for (const Field& f : fields) if (f.key == "note") d.uncertain = true;
        d.lastSeen = now(); d.lastTime = wallTime(false);
        d.count++;
        events++;

        std::string line = d.lastTime + "  " + model + (id.empty() ? "" : " #" + id) + "  " + summary;
        recentEvents.push_front(line);
        if (recentEvents.size() > 40) recentEvents.pop_back();

        std::string logLine = model;
        if (!protocol.empty()) logLine += " [" + protocol + "]";
        for (const Field& f : fields)
            if (f.key != "model" && f.key != "protocol") logLine += "  " + f.key + "=" + f.value;
        log.add(logLine);
    }

public:
    // Log of all received messages (file set by main) and the folder offered for exports
    ModuleLog log;
    std::string exportDir;

    ISMDecoder() : SDRModule("ISM") {
        channels.name = "ISM";
        channels.presets = { 433920000, 868300000, 315000000, 915000000, 345000000 };
        channels.maxChannels = 1;
        channels.defaultWidth = 250000.0 * 0.84;   // +-0.42 of the rtl_433 sample rate
        channels.listenMode = Mode::AM;
    }
    ~ISMDecoder() override {
#ifdef ENABLE_RTL433
        std::lock_guard<std::mutex> l(mtx);
        destroyBridge();
#endif
    }

    static bool available() {
#ifdef ENABLE_RTL433
        return true;
#else
        return false;
#endif
    }

    bool wantsBurstAgc() const override { return true; }
    void setAudioInput(bool audio) override { audioInput = audio; }

    ChannelSet* channelSet() override { return &channels; }

    void init(const ModuleContext& ctx) override {
        font = &ctx.font;
        btnClear = std::make_shared<SdrButton>(110.0f, 24.0f, "Clear list", *font);
        btnClear->onClick = [this]() {
            std::lock_guard<std::mutex> l(mtx);
            devices.clear(); recentEvents.clear(); selectedKey.clear();
            events = 0;
        };
        btnLog = std::make_shared<SdrButton>(110.0f, 24.0f, "Open log", *font);
        btnLog->onClick = [this]() { log.openLogFile(); };
        btnExport = std::make_shared<SdrButton>(110.0f, 24.0f, "Export", *font);
        btnExport->onClick = [this]() { log.exportSession(exportDir, "ISM"); };
        popup.init(*font);
        btnRate = std::make_shared<SdrButton>(150.0f, 24.0f, "Width: 250 kHz", *font);
        btnRate->onClick = [this]() {
            std::lock_guard<std::mutex> l(mtx);
            outRate = (outRate < 500000.0) ? 1000000.0 : 250000.0;
            rateChanged = true;
            btnRate->setText(outRate < 500000.0 ? "Width: 250 kHz" : "Width: 1 MHz");
        };
        btnDecoders = std::make_shared<SdrButton>(150.0f, 24.0f, "Decoders: default", *font);
        btnDecoders->onClick = [this]() {
            std::lock_guard<std::mutex> l(mtx);
            allDecoders = !allDecoders;
            rateChanged = true;   // recreate rtl_433 with the other decoder set
            btnDecoders->setText(allDecoders ? "Decoders: + extra" : "Decoders: default");
        };
        btnUnknown = std::make_shared<SdrButton>(150.0f, 24.0f, "Unknown: hidden", *font);
        btnUnknown->onClick = [this]() {
            std::lock_guard<std::mutex> l(mtx);
            unknownSignals = !unknownSignals;
            rateChanged = true;   // recreate rtl_433 with the analyzer on / off
            btnUnknown->setText(unknownSignals ? "Unknown: shown" : "Unknown: hidden");
        };
    }

    void reset() override {
        std::lock_guard<std::mutex> l(mtx);
        channel.reset();
        cs16.clear();
#ifdef ENABLE_RTL433
        if (bridge) ism_reset(bridge);
#endif
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
#ifdef ENABLE_RTL433
        std::lock_guard<std::mutex> l(mtx);

        // rtl_433 needs the raw radio signal; demodulated audio has lost what it decodes
        if (audioInput) {
            status = "ISM needs I/Q from an SDR: set Input signal to I/Q";
            diag = "Use RTL-SDR, SDRplay, RTL-TCP or an I/Q (stereo) recording";
            return;
        }

        double rate = outRate;
        if (sampleRate < rate && rate > 250000.0) rate = 250000.0;   // 1 MHz needs a wider input
        if (sampleRate < rate) {
            status = "Sample rate too low (needs at least 250 kHz)";
            return;
        }

        if (!bridge || rateChanged || rate != activeRate) {
            destroyBridge();
            bridge = ism_create((uint32_t)rate,
                                (allDecoders ? ISM_EXTRA_DECODERS : 0) | (unknownSignals ? ISM_UNKNOWN_SIGNALS : 0),
                                &ISMDecoder::eventThunk, this);
            rateChanged = false;
            if (!bridge) { status = "rtl_433 initialisation failed"; return; }
            activeRate = rate;
            channels.setWidth(rate * 0.84);
            channel.reset();
            cs16.clear();
        }

        const auto rc = channels.update(centerHz, sampleRate, tunedOffset);
        if (rc.empty()) return;
        const double offset = rc[0].offsetHz;
        if (!rc[0].inBand) {
            status = channels.describe();
            return;
        }
        const long long hz = centerHz > 0 ? rc[0].freqHz : 0;
        if (std::llabs(hz - decodeHz) > 5000) {   // retuned: drop the signal in progress
            decodeHz = hz;
            ism_reset(bridge);
            cs16.clear();
        }
        if (hz > 0) ism_set_frequency(bridge, (uint32_t)std::clamp(hz, 0LL, 4294967295LL));

        if (!channel.configure(sampleRate, offset, rate)) return;
        channel.process(iq.data(), iq.size(), cs16);

        // push in blocks of ~65 ms (rtl_433 works on whole buffers)
        const size_t block = (size_t)(rate / 16.0);
        if (cs16.size() / 2 >= block) {
            ism_push_cs16(bridge, cs16.data(), (uint32_t)(cs16.size() / 2));
            cs16.clear();
        }

        char b[160];
        if (hz > 0)
            std::snprintf(b, sizeof(b), "%.3f MHz%s  +-%.0f kHz   %d decoders", hz / 1e6,
                          rc[0].ch.followVfo ? " (tuner)" : "", rate * 0.42 / 1000.0, ism_protocols_enabled(bridge));
        else
            std::snprintf(b, sizeof(b), "At the tuner  +-%.0f kHz   %d decoders",
                          rate * 0.42 / 1000.0, ism_protocols_enabled(bridge));
        status = b;

        // What the pulse detector sees: many signals but few messages means the devices
        // are unknown to rtl_433 (or the signal is too weak / disturbed)
        ism_stats s;
        ism_get_stats(bridge, &s);
        std::snprintf(b, sizeof(b), "Signals: %u (OOK %u, FSK %u), decoded %u   noise %.1f dB",
                      s.ook_packages + s.fsk_packages, s.ook_packages, s.fsk_packages,
                      s.decoded_packages, s.noise_db);
        diag = b;
#else
        (void)tunedOffset;
#endif
    }

    bool modalOpen() const override { return popup.isOpen(); }
    void drawModal(sf::RenderWindow& win) override { popup.draw(win); }

    bool handleEvent(const sf::Event& ev, const sf::RenderWindow& win) override {
        if (!enabled) return false;
        if (popup.handleEvent(ev, win)) return true;
        if (btnClear && btnClear->handleEvent(ev, win)) return true;
        if (btnLog && btnLog->handleEvent(ev, win)) return true;
        if (btnExport && btnExport->handleEvent(ev, win)) return true;
        if (btnRate && btnRate->handleEvent(ev, win)) return true;
        if (btnDecoders && btnDecoders->handleEvent(ev, win)) return true;
        if (btnUnknown && btnUnknown->handleEvent(ev, win)) return true;
        sf::Vector2f m = win.mapPixelToCoords(sf::Mouse::getPosition(win));
        if (const auto* sc = ev.getIf<sf::Event::MouseWheelScrolled>()) {
            if (!panelRect.contains(m)) return false;
            if (tableRect.contains(m)) scrollRows = std::max(0.0f, scrollRows - sc->delta * 3.0f);
            return true;
        }
        if (const auto* mb = ev.getIf<sf::Event::MouseButtonPressed>()) {
            if (!panelRect.contains(m)) return false;
            if (mb->button == sf::Mouse::Button::Left && tableRect.contains(m) && m.y >= tableBodyY) {
                int row = (int)((m.y - tableBodyY) / tableRowH);
                int idx = tableFirst + row;
                std::string title;
                std::vector<std::pair<std::string, std::string>> rows;
                {
                    std::lock_guard<std::mutex> l(mtx);
                    selectedKey.clear();
                    if (row >= 0 && row < tableVisible && idx < (int)drawnKeys.size()) {
                        auto it = devices.find(drawnKeys[idx]);
                        if (it != devices.end()) {
                            const Device& d = it->second;
                            selectedKey = d.key;
                            title = d.model + (d.id.empty() ? "" : "  #" + d.id);
                            rows.push_back({"Last heard", d.lastTime});
                            rows.push_back({"Messages", std::to_string(d.count)});
                            for (const Field& f : d.fields) rows.push_back({f.label, f.value});
                        }
                    }
                }
                if (!title.empty()) popup.show(title, rows);
            }
            return true;
        }
        return false;
    }

    void draw(sf::RenderWindow& win, float x, float y, float w, float h) override {
        if (!enabled || !font) return;
        panelRect = sf::FloatRect({x, y}, {w, h});

        std::vector<Device> list;
        std::deque<std::string> recent;
        std::string info, selKey, diagText;
        int evCount = 0;
        {
            std::lock_guard<std::mutex> l(mtx);
            for (auto& kv : devices) list.push_back(kv.second);
            recent = recentEvents;
            info = status;
            diagText = diag;
            evCount = events;
            selKey = selectedKey;
        }
        std::sort(list.begin(), list.end(), [](const Device& a, const Device& b) { return a.lastSeen > b.lastSeen; });
        const double t = now();

        sf::RectangleShape bg({w, h});
        bg.setPosition({x, y});
        bg.setFillColor(sf::Color(20, 22, 27, 245));
        bg.setOutlineColor(Theme::Accent);
        bg.setOutlineThickness(1.0f);
        win.draw(bg);

        const float pad = 8.0f;
        const float sideW = 340.0f;
        tableRect = sf::FloatRect({x + pad, y + pad}, {w - sideW - 2 * pad, h - 2 * pad});

        // --- device table ---
        struct Col { const char* name; float w; };
        const float fixedW = 160 + 70 + 34 + 62 + 50 + 44;
        const float dataW = std::max(80.0f, tableRect.size.x - fixedW);
        const Col cols[] = { {"Model", 160}, {"ID", 70}, {"Ch", 34}, {"Data", dataW}, {"RSSI", 62}, {"Seen", 50}, {"Msgs", 44} };
        const int nCols = 7;
        float cx = tableRect.position.x;
        const float rowH = tableRowH;
        for (const Col& c : cols) {
            sf::Text th = text(c.name, 22, Theme::Accent);
            th.setStyle(sf::Text::Bold);
            th.setPosition({cx, tableRect.position.y});
            if (cx + 20.0f <= tableRect.position.x + tableRect.size.x) win.draw(th);
            cx += c.w;
        }
        tableBodyY = tableRect.position.y + rowH + 4.0f;
        const int visibleRows = std::max(1, (int)((tableRect.size.y - rowH - 4.0f) / rowH));
        scrollRows = std::min(scrollRows, (float)std::max(0, (int)list.size() - visibleRows));
        const int first = (int)scrollRows;
        tableFirst = first;
        tableVisible = visibleRows;
        {
            std::vector<std::string> keys;
            for (const Device& d : list) keys.push_back(d.key);
            std::lock_guard<std::mutex> l(mtx);
            drawnKeys = std::move(keys);
        }

        if (!available()) {
            sf::Text e = text("This build of OrbitSDR has no rtl_433 library (it could not be downloaded or compiled).", 24, sf::Color(220, 120, 120));
            e.setPosition({tableRect.position.x, tableBodyY + 4.0f});
            win.draw(e);
            sf::Text e2 = text("Rebuild with network access: the build scripts fetch rtl_433 into .cache/ automatically.", 22, sf::Color(150, 150, 160));
            e2.setPosition({tableRect.position.x, tableBodyY + 22.0f});
            win.draw(e2);
        } else if (list.empty()) {
            sf::Text e = text("No devices received yet. Tune to 433.92 MHz (EU), 868.3 MHz, 315 MHz or 915 MHz (US).", 24, sf::Color(110, 110, 120));
            e.setPosition({tableRect.position.x, tableBodyY + 4.0f});
            win.draw(e);
        }
        const float charW = 5.6f;   // approx. width of one character at this text size
        for (int r = 0; r < visibleRows && first + r < (int)list.size(); r++) {
            const Device& d = list[first + r];
            const float ry = tableBodyY + r * rowH;
            const bool sel = d.key == selKey;
            if (sel || r % 2 == 0) {
                sf::RectangleShape stripe({tableRect.size.x, rowH});
                stripe.setPosition({tableRect.position.x, ry});
                stripe.setFillColor(sel ? sf::Color(Theme::Accent.r, Theme::Accent.g, Theme::Accent.b, 60) : sf::Color(255, 255, 255, 8));
                win.draw(stripe);
            }
            char b[16];
            std::string cells[7];
            cells[0] = d.uncertain ? d.model + " (?)" : d.model;
            cells[1] = d.id.empty() ? "-" : d.id;
            cells[2] = d.channel.empty() ? "-" : d.channel;
            cells[3] = d.summary;
            cells[4] = d.rssi.empty() ? "-" : d.rssi;
            int age = (int)std::max(0.0, t - d.lastSeen);
            if (age < 60) std::snprintf(b, sizeof(b), "%ds", age);
            else if (age < 3600) std::snprintf(b, sizeof(b), "%dm", age / 60);
            else std::snprintf(b, sizeof(b), "%dh", age / 3600);
            cells[5] = b;
            cells[6] = std::to_string(d.count);
            const bool fresh = age < 300;
            float px = tableRect.position.x;
            for (int c = 0; c < nCols; c++) {
                if (px + 20.0f > tableRect.position.x + tableRect.size.x) break;
                const size_t maxChars = (size_t)std::max(3.0f, (cols[c].w - 6.0f) / charW);
                sf::Text td = text(fit(cells[c], maxChars), 22, fresh ? sf::Color(225, 225, 230) : sf::Color(130, 130, 140));
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
            thumb.setPosition({tableRect.position.x + tableRect.size.x - 6.0f, tableBodyY + (trackH - thumbH) * (first / maxFirst)});
            thumb.setFillColor(sf::Color(200, 200, 200, 140));
            win.draw(thumb);
        }

        // --- side column: status, buttons, details of the selected device or the latest messages ---
        const float sx = x + w - sideW;
        float sy = y + pad;
        sf::Text title = text("ISM (rtl_433)", 30, Theme::Accent);
        title.setStyle(sf::Text::Bold);
        title.setPosition({sx, sy});
        win.draw(title);
        char stats[96];
        std::snprintf(stats, sizeof(stats), "%zu devices, %d messages", list.size(), evCount);
        sf::Text st = text(stats, 22, sf::Color(220, 220, 225));
        st.setPosition({sx + 125.0f, sy + 4.0f});
        win.draw(st);
        sy += 24.0f;
        sf::Text ci = text(info, 20, sf::Color(160, 160, 170));
        ci.setPosition({sx, sy});
        win.draw(ci);
        sy += 15.0f;
        sf::Text dg = text(diagText, 20, sf::Color(160, 160, 170));
        dg.setPosition({sx, sy});
        win.draw(dg);
        sy += 18.0f;
        btnClear->setPosition(sx, sy);
        btnClear->draw(win);
        btnRate->setPosition(sx + 115.0f, sy);
        btnRate->draw(win);
        sy += 28.0f;
        btnLog->setPosition(sx, sy);
        btnLog->draw(win);
        btnDecoders->setPosition(sx + 115.0f, sy);
        btnDecoders->draw(win);
        sy += 28.0f;
        btnExport->setPosition(sx, sy);
        btnExport->draw(win);
        btnUnknown->setPosition(sx + 115.0f, sy);
        btnUnknown->draw(win);
        sy += 30.0f;

        const size_t sideChars = 58;
        sf::Text lt = text("Latest messages (click a table row for details)", 22, Theme::Accent);
        lt.setPosition({sx, sy});
        win.draw(lt);
        sy += 18.0f;
        for (const auto& m : recent) {
            if (sy > y + h - 16.0f) break;
            sf::Text mt = text(fit(m, sideChars), 20, sf::Color(200, 200, 205));
            mt.setPosition({sx, sy});
            win.draw(mt);
            sy += 15.0f;
        }
    }
};
