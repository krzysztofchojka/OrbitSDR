#pragma once

#include "../Core/SDRModule.h"
#include "../Core/ModuleLog.h"
#include "../Core/NarrowChannel.h"
#include "CwCore.h"
#include <SFML/Graphics.hpp>
#include <SFML/Window/Clipboard.hpp>
#include <mutex>
#include <atomic>
#include <string>
#include <vector>
#include <cstdio>
#include <algorithm>

// =====================================================================
//  CW (Morse) decoder module.
//
//  The channel follows the tuner by default (drag it onto any CW signal
//  on the waterfall to decode one station while listening to another).
//  Inside the +-300 Hz channel the strongest carrier is found and
//  followed automatically, the speed is learned (5..50 WPM).
//
//  Audio input: the CW tone of a receiver's audio (400..1000 Hz).
// =====================================================================

class CWDecoder : public SDRModule {
    static constexpr double RATE = 2000.0;
    static constexpr double AUDIO_TONE = 700.0;   // audio input: centre of the searched range

    std::mutex mtx;
    ChannelSet channels;
    NarrowChannel ch;
    cw::Decoder dec;
    std::vector<std::complex<float>> tmp;
    long long centerHz = 0;
    long long chanHz = 0;
    double audioRate = 48000.0;
    std::atomic<bool> audioInput {false};
    std::string text;        // everything decoded (shown wrapped)
    std::string logLine;     // the line being collected for the log
    double lastRate = 0.0;

    const sf::Font* font = nullptr;
    sf::FloatRect panelRect, textRect;
    std::shared_ptr<SdrButton> btnClear, btnCopy, btnLog;
    float scrollLines = 0.0f;   // 0 = follow the end

    sf::Text label(const std::string& s, unsigned size, sf::Color c) const {
        sf::Text t(*font, sf::String::fromUtf8(s.begin(), s.end()), size);
        t.setScale({0.5f, 0.5f});
        t.setFillColor(c);
        return t;
    }

    void onText(const std::string& s) {   // mtx held
        text += s;
        if (text.size() > 20000) text.erase(0, text.size() - 16000);
        logLine += s;
        if ((s == " " && logLine.size() > 70) || logLine.size() > 120) {
            while (!logLine.empty() && logLine.back() == ' ') logLine.pop_back();
            if (!logLine.empty()) {
                char b[40];
                if (chanHz > 0) std::snprintf(b, sizeof(b), "%.4f MHz  ", (chanHz + dec.tone()) / 1e6);
                else std::snprintf(b, sizeof(b), "audio  ");
                log.add(std::string(b) + logLine);
            }
            logLine.clear();
        }
    }

    // Wraps the text into lines of at most `cols` characters
    static std::vector<std::string> wrap(const std::string& t, size_t cols) {
        std::vector<std::string> lines;
        std::string cur;
        size_t pos = 0;
        while (pos < t.size()) {
            size_t sp = t.find(' ', pos);
            std::string word = t.substr(pos, sp == std::string::npos ? std::string::npos : sp - pos);
            pos = sp == std::string::npos ? t.size() : sp + 1;
            if (!cur.empty() && cur.size() + 1 + word.size() > cols) { lines.push_back(cur); cur.clear(); }
            while (word.size() > cols) { lines.push_back(word.substr(0, cols)); word.erase(0, cols); }
            if (!cur.empty()) cur += ' ';
            cur += word;
        }
        lines.push_back(cur);
        return lines;
    }

public:
    ModuleLog log;

    CWDecoder() : SDRModule("CW") {
        channels.name = "CW";
        channels.maxChannels = 1;          // no presets: the channel follows the tuner
        channels.defaultWidth = 600.0;     // the tone search range (+-300 Hz)
        channels.listenMode = Mode::CW;
        dec.onText = [this](const std::string& s) { onText(s); };
    }

    void init(const ModuleContext& ctx) override {
        font = &ctx.font;
        audioRate = ctx.sampleRate > 0.0f ? ctx.sampleRate : 48000.0;
        btnClear = std::make_shared<SdrButton>(90.0f, 24.0f, "Clear", *font);
        btnClear->onClick = [this]() { std::lock_guard<std::mutex> l(mtx); text.clear(); scrollLines = 0; };
        btnCopy = std::make_shared<SdrButton>(90.0f, 24.0f, "Copy text", *font);
        btnCopy->onClick = [this]() { std::string t; { std::lock_guard<std::mutex> l(mtx); t = text; } sf::Clipboard::setString(t); };
        btnLog = std::make_shared<SdrButton>(90.0f, 24.0f, "Open log", *font);
        btnLog->onClick = [this]() { log.openLogFile(); };
    }

    void reset() override { std::lock_guard<std::mutex> l(mtx); ch.reset(); dec.reset(); }
    float panelHeight(float windowHeight) const override { return std::clamp(windowHeight * 0.28f, 200.0f, 340.0f); }
    void setRadioFrequency(long long hz) override { std::lock_guard<std::mutex> l(mtx); centerHz = hz; }
    void setAudioInput(bool a) override { audioInput = a; }
    ChannelSet* channelSet() override { return &channels; }

    void processAudio(const std::vector<float>& audio) override {
        if (!enabled || !audioInput || audio.empty()) return;
        std::lock_guard<std::mutex> l(mtx);
        std::vector<std::complex<double>> c(audio.size());
        for (size_t i = 0; i < audio.size(); i++) c[i] = std::complex<double>(audio[i], 0.0);
        ch.configure(audioRate, AUDIO_TONE, RATE, 400.0, 300.0);
        tmp.clear();
        ch.process(c.data(), c.size(), tmp);
        chanHz = 0;
        dec.process(tmp.data(), tmp.size());
    }

    void processIQ(const std::vector<std::complex<double>>& iq, double sampleRate, double tunedOffset) override {
        if (!enabled || iq.empty() || sampleRate <= 0.0 || audioInput) return;
        std::lock_guard<std::mutex> l(mtx);
        const auto rc = channels.update(centerHz, sampleRate, tunedOffset);
        if (rc.empty() || !rc[0].inBand) return;
        if (sampleRate != lastRate) { ch.reset(); lastRate = sampleRate; }
        ch.configure(sampleRate, rc[0].offsetHz, RATE, 400.0, 300.0);
        if (std::llabs(rc[0].freqHz - chanHz) > 50) dec.reset();   // another signal
        chanHz = rc[0].freqHz;
        tmp.clear();
        ch.process(iq.data(), iq.size(), tmp);
        dec.process(tmp.data(), tmp.size());
    }

    bool handleEvent(const sf::Event& ev, const sf::RenderWindow& win) override {
        if (!enabled) return false;
        for (auto* b : {&btnClear, &btnCopy, &btnLog}) if (*b && (*b)->handleEvent(ev, win)) return true;
        const sf::Vector2f m = win.mapPixelToCoords(sf::Mouse::getPosition(win));
        if (const auto* sc = ev.getIf<sf::Event::MouseWheelScrolled>()) {
            if (!panelRect.contains(m)) return false;
            if (textRect.contains(m)) scrollLines = std::max(0.0f, scrollLines + sc->delta * 2.0f);
            return true;
        }
        if (ev.is<sf::Event::MouseButtonPressed>() && panelRect.contains(m)) return true;
        return false;
    }

    void draw(sf::RenderWindow& win, float x, float y, float w, float h) override {
        if (!enabled || !font) return;
        panelRect = sf::FloatRect({x, y}, {w, h});
        std::string t;
        double wpm, tone, snr, sig, noise, env;
        bool key, sq;
        long long f;
        {
            std::lock_guard<std::mutex> l(mtx);
            t = text;
            wpm = dec.wpm(); tone = dec.tone(); snr = dec.toneSnrDb;
            sig = dec.signalDb(); noise = dec.noiseDb(); env = dec.envelopeDb();
            key = dec.key(); sq = dec.squelchOpen;
            f = chanHz;
        }
        sf::RectangleShape bg({w, h});
        bg.setPosition({x, y});
        bg.setFillColor(sf::Color(20, 22, 27, 245));
        bg.setOutlineColor(Theme::Accent);
        bg.setOutlineThickness(1.0f);
        win.draw(bg);

        const float pad = 8.0f, sideW = 300.0f;
        textRect = sf::FloatRect({x + pad, y + pad}, {w - sideW - 2 * pad, h - 2 * pad});
        const float lineH = 20.0f, charW = 8.4f;
        const size_t cols = (size_t)std::max(10.0f, textRect.size.x / charW);
        const auto lines = wrap(t, cols);
        const int visible = std::max(1, (int)(textRect.size.y / lineH));
        const int maxScroll = std::max(0, (int)lines.size() - visible);
        scrollLines = std::min(scrollLines, (float)maxScroll);
        const int last = (int)lines.size() - (int)scrollLines;
        const int first = std::max(0, last - visible);
        if (t.empty()) {
            sf::Text e = label("Tune to a CW signal (or drag the yellow channel onto it). The tone and the speed are found automatically.",
                               24, sf::Color(110, 110, 120));
            e.setPosition({textRect.position.x, textRect.position.y});
            win.draw(e);
        }
        for (int i = first, r = 0; i < last; i++, r++) {
            sf::Text tl = label(lines[i], 32, sf::Color(230, 230, 235));
            tl.setPosition({textRect.position.x, textRect.position.y + r * lineH});
            win.draw(tl);
        }

        const float sx = x + w - sideW;
        float sy = y + pad;
        sf::Text title = label("CW (Morse)", 30, Theme::Accent);
        title.setStyle(sf::Text::Bold);
        title.setPosition({sx, sy});
        win.draw(title);
        // key indicator
        sf::CircleShape led(6.0f);
        led.setPosition({sx + 120.0f, sy + 3.0f});
        led.setFillColor(key ? sf::Color(120, 255, 140) : sq ? sf::Color(60, 90, 60) : sf::Color(60, 60, 64));
        win.draw(led);
        sy += 26.0f;
        char b[96];
        std::snprintf(b, sizeof(b), "Speed: %.0f WPM", wpm);
        sf::Text t1 = label(sq ? b : "Speed: -", 24, sf::Color(220, 220, 225));
        t1.setPosition({sx, sy}); win.draw(t1);
        sy += 18.0f;
        if (f > 0) std::snprintf(b, sizeof(b), "Tone: %.4f MHz (%+.0f Hz)", (f + tone) / 1e6, tone);
        else std::snprintf(b, sizeof(b), "Tone: %.0f Hz (audio)", AUDIO_TONE + tone);
        sf::Text t2 = label(b, 22, sf::Color(200, 200, 205));
        t2.setPosition({sx, sy}); win.draw(t2);
        sy += 17.0f;
        std::snprintf(b, sizeof(b), "Tone SNR %.0f dB   level %.0f / noise %.0f dB", snr, sig, noise);
        sf::Text t3 = label(sq ? b : "No clear tone in the channel", 22, sq ? sf::Color(200, 200, 205) : sf::Color(150, 150, 160));
        t3.setPosition({sx, sy}); win.draw(t3);
        sy += 20.0f;
        // envelope bar
        {
            const float bw = sideW - 20.0f;
            sf::RectangleShape bar({bw, 8.0f});
            bar.setPosition({sx, sy});
            bar.setFillColor(sf::Color(40, 40, 46));
            win.draw(bar);
            const float span = (float)std::max(6.0, sig - noise + 6.0);
            const float v = (float)std::clamp((env - noise + 3.0) / span, 0.0, 1.0);
            sf::RectangleShape fill({bw * v, 8.0f});
            fill.setPosition({sx, sy});
            fill.setFillColor(key ? sf::Color(120, 255, 140) : sf::Color(120, 140, 160));
            win.draw(fill);
        }
        sy += 18.0f;
        btnClear->setPosition(sx, sy); btnClear->draw(win);
        btnCopy->setPosition(sx + 96.0f, sy); btnCopy->draw(win);
        btnLog->setPosition(sx + 192.0f, sy); btnLog->draw(win);
        sy += 32.0f;
        sf::Text hint = label("Listen in CW mode. Mouse wheel: scroll the text.", 20, sf::Color(140, 140, 150));
        hint.setPosition({sx, sy}); win.draw(hint);
    }
};
