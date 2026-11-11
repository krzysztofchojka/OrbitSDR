#pragma once

#include "../Core/SDRModule.h"
#include "../Core/DetailsPopup.h"
#include "../Core/ModuleLog.h"
#include "../../Decimator.h"
#include "AcarsCore.h"
#include <SFML/Graphics.hpp>
#include <mutex>
#include <deque>
#include <map>
#include <vector>
#include <string>
#include <ctime>
#include <cstdio>
#include <algorithm>
#include <atomic>

// =====================================================================
//  ACARS decoder module (aircraft datalink, VHF AM 129-137 MHz).
//
//  Works on the raw IQ samples, independent of the audio demodulator:
//  every known ACARS channel that fits into the received bandwidth is
//  cut out, AM demodulated and decoded at the same time (e.g. tune
//  131.650 MHz with 2 MSps for the European channels 131.525 / 131.725 /
//  131.825). When none fits, the tuned frequency is decoded.
//
//  Audio input (sound card from a radio's audio output, a WAV file with
//  AM-demodulated audio - Input signal switch set to Audio): the audio is decoded directly.
// =====================================================================

namespace acars {

// One channel: NCO mix -> halfband decimation to 24..48 kHz -> channel
// filter (+-7 kHz) -> AM envelope -> MSK demodulator
class ChannelRx {
    using C = std::complex<double>;
    static constexpr double PI_D = 3.14159265358979323846;
    double fs = 0.0, offsetHz = 1e18;
    C nco = C(1.0, 0.0), ncoStep = C(1.0, 0.0);
    IQDecimator decim;
    std::vector<double> chf;
    std::vector<C> chHist;
    size_t chPos = 0;
    std::vector<C> mixed;
    std::vector<float> audio;

public:
    Demod demod;
    double level = 0.0;   // smoothed carrier level (for the UI)

    void configure(double sampleRate, double channelOffsetHz) {
        if (sampleRate != fs) {
            fs = sampleRate;
            int f = 1;
            while (fs / (f * 2) >= 24000.0) f *= 2;
            decim.setFactor(f);
            const double mid = fs / decim.getFactor();
            const int taps = 31;
            chf.assign(taps, 0.0);
            const double fc = 7000.0 / mid;
            double sum = 0.0;
            for (int i = 0; i < taps; i++) {
                int m = i - taps / 2;
                double s = m == 0 ? 2.0 * fc : std::sin(2.0 * PI_D * fc * m) / (PI_D * m);
                double w = 0.42 - 0.5 * std::cos(2.0 * PI_D * i / (taps - 1)) + 0.08 * std::cos(4.0 * PI_D * i / (taps - 1));
                chf[i] = s * w; sum += chf[i];
            }
            for (double& v : chf) v /= sum;
            chHist.assign(taps * 2, C(0.0, 0.0));
            chPos = 0;
            demod.configure(mid);
            offsetHz = 1e18;
        }
        if (channelOffsetHz != offsetHz) {
            offsetHz = channelOffsetHz;
            ncoStep = std::polar(1.0, -2.0 * PI_D * offsetHz / fs);
        }
    }

    void process(const C* iq, size_t n) {
        if (fs <= 0.0 || n == 0) return;
        mixed.resize(n);
        for (size_t i = 0; i < n; i++) { mixed[i] = iq[i] * nco; nco *= ncoStep; }
        nco /= std::abs(nco);
        size_t cnt = 0;
        const C* d = decim.process(mixed.data(), n, cnt);
        audio.resize(cnt);
        const size_t taps = chf.size();
        double sumLevel = 0.0;
        for (size_t i = 0; i < cnt; i++) {
            chHist[chPos] = d[i]; chHist[chPos + taps] = d[i];
            chPos = (chPos + 1) % taps;
            C y(0.0, 0.0);
            const C* h = &chHist[chPos];
            for (size_t k = 0; k < taps; k++) y += h[k] * chf[k];
            const double a = std::abs(y);
            audio[i] = (float)a;
            sumLevel += a;
        }
        if (cnt) level = 0.9 * level + 0.1 * (sumLevel / cnt);
        demod.process(audio.data(), audio.size());
    }
};

} // namespace acars

class ACARSDecoder : public SDRModule {
    struct Entry {
        std::string time;
        long long freqHz = 0;
        acars::Message msg;
    };
    struct Rx {
        long long freqHz = 0;
        acars::ChannelRx rx;
        int messages = 0;
    };

    // Known ACARS channels (Europe, North America, Asia / world)
    static const std::vector<long long>& knownChannels() {
        static const std::vector<long long> ch = {
            129125000, 130025000, 130425000, 130450000, 131125000, 131450000, 131475000,
            131525000, 131550000, 131725000, 131825000, 131850000, 136700000, 136750000,
            136800000, 136850000, 136875000, 136900000, 136925000, 136975000 };
        return ch;
    }
    static constexpr size_t MAX_CHANNELS = 8;

    std::mutex mtx;
    std::map<int, std::unique_ptr<Rx>> rxs;         // by channel id
    ChannelSet channels;                            // see DecoderChannels.h
    std::deque<Entry> entries;                      // newest first
    long long centerHz = 0;
    double lastRate = 0.0;
    int total = 0;
    std::string channelInfo = "Waiting for signal...";
    bool tunedOnly = false;
    std::atomic<bool> audioInput {false};
    acars::Demod audioDemod;   // audio input path
    double audioRate = 48000.0;
    int audioMessages = 0;
    static constexpr long long AUDIO_FREQ = -1;   // "frequency" of messages from the audio path

    const sf::Font* font = nullptr;
    sf::FloatRect panelRect, tableRect;
    float scrollRows = 0.0f;
    float tableBodyY = 0.0f;
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

    static std::string mhz(long long hz) {
        if (hz == AUDIO_FREQ) return "audio";
        char b[24];
        std::snprintf(b, sizeof(b), "%.3f", hz / 1e6);
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

    static std::string oneLine(const std::string& s) {
        std::string o = s;
        for (char& c : o) if (c == '\n' || c == '\t') c = ' ';
        return o;
    }

    // Called from processIQ (mtx is held)
    void onMessage(long long freqHz, const acars::Message& m) {
        Entry e;
        e.time = wallTime();
        e.freqHz = freqHz;
        e.msg = m;
        entries.push_front(e);
        if (entries.size() > 1000) entries.pop_back();
        total++;

        std::string line = mhz(freqHz) + (freqHz == AUDIO_FREQ ? "  " : " MHz  ") + (m.reg.empty() ? "-" : m.reg) +
                           (m.flight.empty() ? "" : "  " + m.flight) +
                           "  mode " + std::string(1, m.mode) + "  label " + m.label +
                           "  block " + std::string(1, m.blockId) +
                           (m.msn.empty() ? "" : "  msn " + m.msn) +
                           (m.ack == '!' ? "" : "  ack " + std::string(1, m.ack)) +
                           (m.downlink ? "  (down)" : "  (up)") +
                           (m.more ? "  [more]" : "");
        if (!m.text.empty()) line += "  | " + oneLine(m.text);
        log.add(line);
    }

    void openDetails(const Entry& e) {
        const acars::Message& m = e.msg;
        std::vector<std::pair<std::string, std::string>> rows;
        rows.push_back({"Time", e.time});
        rows.push_back({"Frequency", e.freqHz == AUDIO_FREQ ? "audio input" : mhz(e.freqHz) + " MHz"});
        rows.push_back({"Direction", m.downlink ? "Downlink (aircraft -> ground)" : "Uplink (ground -> aircraft)"});
        rows.push_back({"Aircraft", m.reg.empty() ? "-" : m.reg});
        if (!m.flight.empty()) rows.push_back({"Flight", m.flight});
        const char* ld = acars::labelDescription(m.label);
        rows.push_back({"Label", m.label + (*ld ? std::string("  (") + ld + ")" : "")});
        rows.push_back({"Mode", std::string(1, m.mode)});
        rows.push_back({"Block ID", std::string(1, m.blockId)});
        if (!m.msn.empty()) rows.push_back({"Message no.", m.msn});
        rows.push_back({"ACK", m.ack == '!' ? "NAK" : std::string(1, m.ack)});
        if (m.more) rows.push_back({"Blocks", "more blocks follow (ETB)"});
        if (m.fixedBits) rows.push_back({"Corrected", std::to_string(m.fixedBits) + " bit (parity + CRC)"});
        popup.show("ACARS  " + (m.reg.empty() ? std::string("message") : m.reg) +
                   (m.flight.empty() ? "" : "  " + m.flight), rows, m.text.empty() ? "(no text)" : m.text);
    }

public:
    ModuleLog log;
    std::string exportDir;

    ACARSDecoder() : SDRModule("ACARS") {
        channels.name = "ACARS";
        channels.presets = knownChannels();
        channels.maxChannels = MAX_CHANNELS;
        channels.defaultWidth = 14000.0;   // +-7 kHz channel filter
        channels.listenMode = Mode::AM;
        channels.shape = [](DecoderChannel& c) {
            char b[24];
            std::snprintf(b, sizeof(b), "%.3f", c.freqHz / 1e6);
            c.name = c.followVfo ? std::string("ACARS") : std::string(b);
        };
    }

    void init(const ModuleContext& ctx) override {
        font = &ctx.font;
        audioRate = ctx.sampleRate > 0.0f ? ctx.sampleRate : 48000.0;
        audioDemod.onMessage = [this](const acars::Message& m) {   // mtx is held (processAudio)
            audioMessages++;
            onMessage(AUDIO_FREQ, m);
        };
        popup.init(*font);
        btnClear = std::make_shared<SdrButton>(100.0f, 24.0f, "Clear list", *font);
        btnClear->onClick = [this]() {
            std::lock_guard<std::mutex> l(mtx);
            entries.clear(); total = 0; audioMessages = 0;
            for (auto& kv : rxs) kv.second->messages = 0;
        };
        btnLog = std::make_shared<SdrButton>(100.0f, 24.0f, "Open log", *font);
        btnLog->onClick = [this]() { log.openLogFile(); };
        btnExport = std::make_shared<SdrButton>(100.0f, 24.0f, "Export", *font);
        btnExport->onClick = [this]() { log.exportSession(exportDir, "ACARS"); };
    }

    void reset() override {
        std::lock_guard<std::mutex> l(mtx);
        rxs.clear();   // rebuilt on the next block
    }

    float panelHeight(float windowHeight) const override {
        return std::clamp(windowHeight * 0.38f, 260.0f, 460.0f);
    }

    void setRadioFrequency(long long hz) override {
        std::lock_guard<std::mutex> l(mtx);
        centerHz = hz;
    }

    void setAudioInput(bool audio) override { audioInput = audio; }

    // Audio input: the AM-demodulated audio is the ACARS signal itself
    void processAudio(const std::vector<float>& audio) override {
        if (!enabled || !audioInput || audio.empty()) return;
        std::lock_guard<std::mutex> l(mtx);
        audioDemod.configure(audioRate);
        audioDemod.process(audio.data(), audio.size());
    }

    ChannelSet* channelSet() override { return &channels; }

    bool modalOpen() const override { return popup.isOpen(); }
    void drawModal(sf::RenderWindow& win) override { popup.draw(win); }

    void processIQ(const std::vector<std::complex<double>>& iq, double sampleRate, double tunedOffset) override {
        if (!enabled || iq.empty() || sampleRate <= 0.0) return;
        std::lock_guard<std::mutex> l(mtx);
        if (audioInput) {   // decoded in processAudio
            if (!rxs.empty()) rxs.clear();
            channelInfo = "Input signal: Audio (decoding the demodulated audio)";
            return;
        }
        if (sampleRate < 48000.0) { channelInfo = "Sample rate too low"; return; }

        // The known ACARS channels in band, or the tuner (see ChannelSet)
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
                slot->rx.demod.onMessage = [this, raw](const acars::Message& m) {
                    raw->messages++;
                    onMessage(raw->freqHz, m);
                };
            }
            Rx& r = *slot;
            r.freqHz = c.freqHz;
            if (!c.inBand) continue;
            r.rx.configure(sampleRate, c.offsetHz);
            r.rx.process(iq.data(), iq.size());
        }
        channelInfo = tunedOnly ? "No ACARS channel in band: decoding at the tuner"
                                : std::to_string(rxs.size()) + " channel(s)";
    }

    bool handleEvent(const sf::Event& ev, const sf::RenderWindow& win) override {
        if (!enabled) return false;
        if (popup.handleEvent(ev, win)) return true;
        if (btnClear && btnClear->handleEvent(ev, win)) return true;
        if (btnLog && btnLog->handleEvent(ev, win)) return true;
        if (btnExport && btnExport->handleEvent(ev, win)) return true;
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
                const int idx = tableFirst + row;
                Entry e;
                bool found = false;
                {
                    std::lock_guard<std::mutex> l(mtx);
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
        std::vector<std::pair<long long, std::pair<int, double>>> chans;
        std::string info;
        int count = 0;
        bool tuned = false;
        {
            std::lock_guard<std::mutex> l(mtx);
            const int first = (int)scrollRows;
            for (int i = first; i < (int)entries.size() && i < first + 60; i++) list.push_back(entries[i]);
            tableFirst = first;
            count = (int)entries.size();
            for (auto& kv : rxs) chans.push_back({kv.second->freqHz, {kv.second->messages, kv.second->rx.level}});
            if (audioInput) chans.push_back({AUDIO_FREQ, {audioMessages, -1.0}});
            info = channelInfo;
            tuned = tunedOnly;
        }

        sf::RectangleShape bg({w, h});
        bg.setPosition({x, y});
        bg.setFillColor(sf::Color(20, 22, 27, 245));
        bg.setOutlineColor(Theme::Accent);
        bg.setOutlineThickness(1.0f);
        win.draw(bg);

        const float pad = 8.0f;
        const float sideW = 330.0f;
        tableRect = sf::FloatRect({x + pad, y + pad}, {w - sideW - 2 * pad, h - 2 * pad});

        struct Col { const char* name; float w; };
        const float fixedW = 58 + 60 + 70 + 64 + 40 + 30 + 36;
        const float textW = std::max(80.0f, tableRect.size.x - fixedW);
        const Col cols[] = { {"Time", 58}, {"Freq", 60}, {"Aircraft", 70}, {"Flight", 64}, {"Label", 40},
                             {"Blk", 30}, {"Dir", 36}, {"Text", textW} };
        const int nCols = 8;
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
            sf::Text e = text(tuned ? "No messages yet. Tune near 131.650 MHz (Europe) or 131.550 / 130.025 MHz to cover several ACARS channels."
                                    : "No messages yet. ACARS bursts are short: wait for aircraft nearby.", 24, sf::Color(110, 110, 120));
            e.setPosition({tableRect.position.x, tableBodyY + 4.0f});
            win.draw(e);
        }
        const float charW = 5.6f;
        for (int r = 0; r < visibleRows && r < (int)list.size(); r++) {
            const Entry& en = list[r];
            const acars::Message& m = en.msg;
            const float ry = tableBodyY + r * rowH;
            if (r % 2 == 0) {
                sf::RectangleShape stripe({tableRect.size.x, rowH});
                stripe.setPosition({tableRect.position.x, ry});
                stripe.setFillColor(sf::Color(255, 255, 255, 8));
                win.draw(stripe);
            }
            std::string cells[8] = {
                en.time, mhz(en.freqHz), m.reg.empty() ? "-" : m.reg, m.flight.empty() ? "-" : m.flight,
                m.label, std::string(1, m.blockId), m.downlink ? "down" : "up",
                m.text.empty() ? (*acars::labelDescription(m.label) ? std::string("(") + acars::labelDescription(m.label) + ")" : "")
                               : oneLine(m.text) };
            float px = tableRect.position.x;
            for (int c = 0; c < nCols; c++) {
                if (px + 20.0f > tableRect.position.x + tableRect.size.x) break;
                const size_t maxChars = (size_t)std::max(3.0f, (cols[c].w - 6.0f) / charW);
                sf::Color col = (c == 7 && m.text.empty()) ? sf::Color(130, 130, 140) : sf::Color(225, 225, 230);
                if (c == 6) col = m.downlink ? sf::Color(120, 200, 255) : sf::Color(255, 200, 120);
                sf::Text td = text(fit(cells[c], maxChars), 22, col);
                td.setPosition({px, ry});
                win.draw(td);
                px += cols[c].w;
            }
        }
        if (count > visibleRows) {
            float trackH = tableRect.size.y - rowH - 4.0f;
            float thumbH = std::max(16.0f, trackH * visibleRows / (float)count);
            float maxFirst = (float)(count - visibleRows);
            sf::RectangleShape thumb({4.0f, thumbH});
            thumb.setPosition({tableRect.position.x + tableRect.size.x - 6.0f,
                               tableBodyY + (trackH - thumbH) * (tableFirst / maxFirst)});
            thumb.setFillColor(sf::Color(200, 200, 200, 140));
            win.draw(thumb);
        }

        // --- side column ---
        const float sx = x + w - sideW;
        float sy = y + pad;
        sf::Text title = text("ACARS", 30, Theme::Accent);
        title.setStyle(sf::Text::Bold);
        title.setPosition({sx, sy});
        win.draw(title);
        sf::Text st = text(std::to_string(count) + " messages", 22, sf::Color(220, 220, 225));
        st.setPosition({sx + 70.0f, sy + 4.0f});
        win.draw(st);
        sy += 24.0f;
        sf::Text ci = text(info, 20, sf::Color(160, 160, 170));
        ci.setPosition({sx, sy});
        win.draw(ci);
        sy += 18.0f;
        btnClear->setPosition(sx, sy);
        btnClear->draw(win);
        btnLog->setPosition(sx + 106.0f, sy);
        btnLog->draw(win);
        btnExport->setPosition(sx + 212.0f, sy);
        btnExport->draw(win);
        sy += 32.0f;
        sf::Text lt = text("Channels (click a table row for details)", 22, Theme::Accent);
        lt.setPosition({sx, sy});
        win.draw(lt);
        sy += 18.0f;
        for (const auto& c : chans) {
            if (sy > y + h - 16.0f) break;
            const double lvl = c.second.second;
            const double db = lvl > 1e-9 ? 20.0 * std::log10(lvl) : -120.0;
            char b[96];
            if (c.first == AUDIO_FREQ)
                std::snprintf(b, sizeof(b), "Audio input   %3d msgs", c.second.first);
            else
                std::snprintf(b, sizeof(b), "%s MHz   %3d msgs   level %5.1f dB", mhz(c.first).c_str(), c.second.first, db);
            sf::Text ct = text(b, 20, sf::Color(200, 200, 205));
            ct.setPosition({sx, sy});
            win.draw(ct);
            sy += 15.0f;
        }
    }
};
