#pragma once

#include "../Core/SDRModule.h"
#include "../Core/NarrowChannel.h"
#include "AnalyzerCore.h"
#include "PipelineHub.h"
#include <SFML/Graphics.hpp>
#include <SFML/Window/Clipboard.hpp>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <string>
#include <vector>
#include <cstdio>
#include <algorithm>

// =====================================================================
//  Signal analyzer module ("what is this signal?").
//
//  An orange channel of any width (drag its edges; on the tuner it takes
//  the tuner's bandwidth). Its baseband goes to an::Analyzer, which finds
//  the transmissions and measures them; the panel shows
//
//   left:   the verdict, the measurements and what it may be,
//   middle: histogram of the frequency (FSK: at the symbol instants) and
//           the eye diagram (FSK / OOK) or the constellation (PSK),
//   right:  the bursts of the last 20 s and the symbols of the last one
//           (preamble grey, part repeated in every burst orange, known
//           sync words green).
//
//  Threads (a wide channel is a lot of samples):
//   - the decoder worker only cuts the channel out and finds the
//     transmissions (cheap, under `mtx`),
//   - the analysis thread classifies them (an::Analyzer::analyzeCapture),
//     at most two waiting - newer ones replace older ones,
//   - the pipelines (tabs of their own, PipelineHub.h) that take this
//     receiver get the channel through pl::Hub, each on its own thread.
//  The channel is cut out only while someone looks: this panel is shown
//  or a pipeline takes it. The UI never waits for an analysis: reports
//  come under `repMtx`.
// =====================================================================

class SignalAnalyzer : public SDRModule {
public:
    static constexpr const char* NAME = "Analyzer";
    static sf::Color color() { return sf::Color(255, 140, 0); }

private:
    std::mutex mtx;
    ChannelSet channels;
    NarrowChannel ch;
    an::Analyzer core;
    std::vector<std::complex<float>> tmp;
    long long centerHz = 0;
    long long chanHz = 0;          // where the analysed channel was (absolute; offset if unknown)
    double chanWidth = 0.0;
    double lastRate = 0.0;
    double tunerBw = 0.0;
    std::atomic<bool> audioInput {false};

    // ---- analysis thread ----
    an::Analyzer cls;                        // its classification state (earlier bursts)
    std::mutex repMtx;                       // the latest report (short holds only)
    an::Report latestRep;
    int repCount = 0;
    std::atomic<unsigned> gen {0};           // bumped when the channel starts over
    std::mutex aMtx;
    std::condition_variable aCv;
    std::deque<std::pair<unsigned, an::Capture>> aQ;
    unsigned workGen = 0;                    // the generation being analysed (analysis thread)

    bool stopping = false;                   // aMtx
    std::atomic<long long> shownMs {0};      // when the panel was last drawn (steady clock, ms)
    static long long nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void blankReport() {                     // a new channel: nothing to show yet
        gen++;
        std::lock_guard<std::mutex> l(repMtx);
        latestRep = an::Report();
        repCount++;
    }

    void analysisLoop() {
        for (;;) {
            std::pair<unsigned, an::Capture> job;
            {
                std::unique_lock<std::mutex> l(aMtx);
                aCv.wait(l, [this] { return stopping || !aQ.empty(); });
                if (stopping) return;
                job = std::move(aQ.front());
                aQ.pop_front();
            }
            if (job.first != gen.load()) continue;                 // an old channel's
            if (job.first != workGen) { cls.resetHistory(); workGen = job.first; }
            cls.analyzeCapture(job.second);                         // -> onReport
        }
    }

    // UI state (UI thread)
    an::Report shown;
    int shownReports = -1;
    bool frozen = false;
    float symScroll = 0.0f;        // lines scrolled up from the start of the symbols
    const sf::Font* font = nullptr;
    sf::FloatRect panelRect, symRect, trigLvlRect;
    std::shared_ptr<SdrButton> btnFreeze, btnCopy, btnCopySym, btnClear, btnTrig, btnArm, btnLvlDown, btnLvlUp, btnPipe;
    float charW = 7.0f;

    // trigger, like an oscilloscope's: Auto (bursts + steady signals), Normal (bursts only),
    // Single (the next burst, then hold until Arm)
    int trigMode = 0;
    bool armed = false;
    int armedAt = 0;               // core.reports when armed
    double trigDb = 8.0;
    void applyTrigger() {          // mtx held
        core.triggerDb = trigDb;
        core.steadyWindows = trigMode == 0;
        core.live = trigMode == 3;
    }
    void setTrigMode(int m) {
        trigMode = m;
        static const char* names[] = {"Trigger: Auto", "Trigger: Normal", "Trigger: Single", "Live (continuous)"};
        btnTrig->setText(names[m]);
        btnArm->setEnabled(m == 2);
        { std::lock_guard<std::mutex> l(mtx); applyTrigger(); }
        if (m == 2) arm(); else { armed = false; frozen = false; btnFreeze->setActive(false); }
    }
    void arm() {
        std::lock_guard<std::mutex> l(mtx);
        armed = true; frozen = true;
        { std::lock_guard<std::mutex> r(repMtx); armedAt = repCount; }
        btnFreeze->setActive(false);
    }

    std::string inputInfo;           // frequency, width, rate of the channel (mtx)
    std::thread aTh;                 // analysis (started in the constructor)

    sf::Text label(const std::string& s, unsigned size, sf::Color c) const {
        sf::Text t(*font, sf::String::fromUtf8(s.begin(), s.end()), size);
        t.setScale({0.5f, 0.5f});
        t.setFillColor(c);
        return t;
    }

    // the channel's rate: twice its width, but never above the input's (no up-sampling of a wide channel)
    static double outRate(double width, double inRate) { return std::max(4000.0, std::min(width * 2.0, inRate)); }

    // Draws s word-wrapped to maxW; returns the y below it (stops at maxY)
    float drawWrapped(sf::RenderWindow& win, const std::string& s, float x, float y, float maxW, float maxY,
                      unsigned size, sf::Color c, float lineH, const std::string& indent = "") const {
        std::string line;
        size_t pos = 0;
        auto flush = [&](const std::string& l) {
            if (y > maxY - lineH) return;
            sf::Text t = label(l, size, c);
            t.setPosition({x, y});
            win.draw(t);
            y += lineH;
        };
        while (pos < s.size()) {
            size_t sp = s.find(' ', pos);
            const std::string word = s.substr(pos, sp == std::string::npos ? std::string::npos : sp - pos);
            pos = sp == std::string::npos ? s.size() : sp + 1;
            const std::string cand = line.empty() ? word : line + " " + word;
            if (!line.empty() && label(cand, size, c).getGlobalBounds().size.x > maxW) {
                flush(line);
                line = indent + word;
            } else {
                line = cand;
            }
        }
        if (!line.empty()) flush(line);
        return y;
    }

    std::string reportText(const an::Report& r, long long fHz) const {
        std::string t;
        char b[128];
        if (fHz > 0) { std::snprintf(b, sizeof(b), "%.4f MHz  ", (fHz + r.offsetHz) / 1e6); t += b; }
        t += r.modulation + "\n";
        for (const auto& n : r.notes) t += "  " + n + "\n";
        for (const auto& g : r.guesses) t += "  ? " + g + "\n";
        if (!r.symbols.empty()) {
            t += "  symbols: ";
            for (uint8_t s : r.symbols) t += (char)('0' + s);
            t += "\n";
        }
        return t;
    }

public:
    SignalAnalyzer() : SDRModule(NAME) {
        channels.name = "Analyzer";
        channels.maxChannels = 1;           // no presets: follows the tuner until dragged
        channels.defaultWidth = 12500.0;
        channels.listenMode = Mode::NFM;
        channels.resizable = true;          // the user sets the width to the signal
        channels.followTunerWidth = true;   // on the tuner: the tuner's bandwidth
        // the detector hands its captures to the analysis thread (two waiting at most:
        // a newer one replaces the oldest, the panel wants the latest)
        core.onCapture = [this](an::Capture&& c) {
            {
                std::lock_guard<std::mutex> l(aMtx);
                aQ.emplace_back(gen.load(), std::move(c));
                while (aQ.size() > 2) aQ.pop_front();
            }
            aCv.notify_one();
        };
        cls.onReport = [this](an::Report&& r) {
            if (workGen != gen.load()) return;   // the channel moved meanwhile
            std::lock_guard<std::mutex> l(repMtx);
            latestRep = std::move(r);
            repCount++;
        };
        aTh = std::thread([this] { analysisLoop(); });
    }

    ~SignalAnalyzer() override {
        { std::lock_guard<std::mutex> a(aMtx); stopping = true; }
        aCv.notify_all();
        if (aTh.joinable()) aTh.join();
    }

    void init(const ModuleContext& ctx) override {
        font = &ctx.font;
        btnFreeze = std::make_shared<SdrButton>(70.0f, 22.0f, "Freeze", *font);
        btnFreeze->onClick = [this]() { frozen = !frozen; btnFreeze->setActive(frozen); };
        btnCopy = std::make_shared<SdrButton>(84.0f, 22.0f, "Copy report", *font);
        btnCopy->onClick = [this]() {
            std::string t;
            long long f;
            { std::lock_guard<std::mutex> l(mtx); f = chanHz; }
            t = reportText(shown, f);
            sf::Clipboard::setString(sf::String::fromUtf8(t.begin(), t.end()));
        };
        btnCopySym = std::make_shared<SdrButton>(90.0f, 22.0f, "Copy symbols", *font);
        btnCopySym->onClick = [this]() {
            std::string t;
            for (uint8_t s : shown.symbols) t += (char)('0' + s);
            sf::Clipboard::setString(t);
        };
        btnClear = std::make_shared<SdrButton>(60.0f, 22.0f, "Reset", *font);
        btnClear->onClick = [this]() {
            { std::lock_guard<std::mutex> l(mtx); core.reset(); ch.reset(); }
            blankReport();
            shown = an::Report(); symScroll = 0.0f;
            { std::lock_guard<std::mutex> r(repMtx); shownReports = repCount; armedAt = repCount; }
        };
        btnTrig = std::make_shared<SdrButton>(112.0f, 22.0f, "Trigger: Auto", *font);
        btnTrig->onClick = [this]() { setTrigMode((trigMode + 1) % 4); };
        btnArm = std::make_shared<SdrButton>(46.0f, 22.0f, "Arm", *font);
        btnArm->onClick = [this]() { arm(); };
        btnArm->setEnabled(false);
        btnLvlDown = std::make_shared<SdrButton>(22.0f, 22.0f, "-", *font);
        btnLvlDown->onClick = [this]() { std::lock_guard<std::mutex> l(mtx); trigDb = std::max(2.0, trigDb - 1.0); applyTrigger(); };
        btnLvlUp = std::make_shared<SdrButton>(22.0f, 22.0f, "+", *font);
        btnLvlUp->onClick = [this]() { std::lock_guard<std::mutex> l(mtx); trigDb = std::min(50.0, trigDb + 1.0); applyTrigger(); };
        btnPipe = std::make_shared<SdrButton>(92.0f, 22.0f, "Pipeline  >", *font);
        btnPipe->onClick = [this]() { if (onOpenPipeline) onOpenPipeline(); };
        sf::Text probe(*font, "0123456789", 22);
        probe.setScale({0.5f, 0.5f});
        charW = probe.getGlobalBounds().size.x / 10.0f;
    }

    // main.cpp: the receiver's number (its channel goes to the pipelines as "RX<n>") and
    // "Pipeline >": a pipeline tab taking this receiver
    int rxId = 0;
    std::function<void()> onOpenPipeline;

    void reset() override {
        { std::lock_guard<std::mutex> l(mtx); ch.reset(); core.reset(); }
        blankReport();
    }
    float panelHeight(float windowHeight) const override { return std::clamp(windowHeight * 0.32f, 230.0f, 380.0f); }
    void setRadioFrequency(long long hz) override { std::lock_guard<std::mutex> l(mtx); centerHz = hz; }
    void setAudioInput(bool a) override { audioInput = a; }
    void setTunerBandwidth(double hz) override { std::lock_guard<std::mutex> l(mtx); tunerBw = hz; }
    ChannelSet* channelSet() override { return &channels; }

    void processIQ(const std::vector<std::complex<double>>& iq, double sampleRate, double tunedOffset) override {
        if (!enabled || iq.empty() || sampleRate <= 0.0 || audioInput) return;
        // nobody looking (panel hidden, no pipeline takes this receiver): nothing to do
        const bool look = nowMs() - shownMs.load() < 1000;
        const bool feed = pl::Hub::get().wants(rxId);
        if (!look && !feed) return;
        std::lock_guard<std::mutex> l(mtx);
        const auto rc = channels.update(centerHz, sampleRate, tunedOffset, tunerBw);
        if (rc.empty() || !rc[0].inBand) return;
        const double w = std::clamp(rc[0].ch.widthHz, 500.0, sampleRate * 0.8);
        // another signal (moved, resized, other sample rate): start over
        if (sampleRate != lastRate || std::fabs(w - chanWidth) > 1.0 || std::llabs(rc[0].freqHz - chanHz) > 100) {
            ch.reset();
            core.configure(outRate(w, sampleRate));
            core.reset();
            applyTrigger();
            blankReport();   // (lock order everywhere: mtx before repMtx)
            lastRate = sampleRate;
            chanWidth = w;
            chanHz = rc[0].freqHz;
            char b[96];
            if (chanHz > 1000000) std::snprintf(b, sizeof(b), "%.4f MHz, %.1f kHz wide, %.0f kS/s", chanHz / 1e6, w / 1000.0, outRate(w, sampleRate) / 1000.0);
            else std::snprintf(b, sizeof(b), "%+.1f kHz, %.1f kHz wide, %.0f kS/s", chanHz / 1000.0, w / 1000.0, outRate(w, sampleRate) / 1000.0);
            inputInfo = b;
        }
        ch.configure(sampleRate, rc[0].offsetHz, outRate(w, sampleRate), w / 2.0, std::max(200.0, w * 0.08), 2.0);
        tmp.clear();
        ch.process(iq.data(), iq.size(), tmp);
        if (look) core.push(tmp.data(), tmp.size());   // captures go to the analysis thread
        if (feed) pl::Hub::get().publish(rxId, tmp.data(), tmp.size(), outRate(w, sampleRate), inputInfo);
    }

    bool handleEvent(const sf::Event& ev, const sf::RenderWindow& win) override {
        if (!enabled) return false;
        for (auto* b : {&btnFreeze, &btnCopy, &btnCopySym, &btnClear, &btnTrig, &btnArm, &btnLvlDown, &btnLvlUp, &btnPipe})
            if (*b && (*b)->handleEvent(ev, win)) return true;
        const sf::Vector2f m = win.mapPixelToCoords(sf::Mouse::getPosition(win));
        if (const auto* sc = ev.getIf<sf::Event::MouseWheelScrolled>()) {
            if (!panelRect.contains(m)) return false;
            if (symRect.contains(m)) symScroll = std::max(0.0f, symScroll - sc->delta * 2.0f);
            return true;
        }
        if (ev.is<sf::Event::MouseButtonPressed>() && panelRect.contains(m)) return true;
        return false;
    }

private:
    // ---------------- drawing helpers ----------------
    void frame(sf::RenderWindow& win, const sf::FloatRect& r, const std::string& title) const {
        sf::RectangleShape bg(r.size);
        bg.setPosition(r.position);
        bg.setFillColor(sf::Color(12, 12, 15));
        bg.setOutlineColor(sf::Color(55, 55, 62));
        bg.setOutlineThickness(1.0f);
        win.draw(bg);
        sf::Text t = label(title, 20, sf::Color(140, 140, 150));
        t.setPosition({r.position.x + 4.0f, r.position.y + 1.0f});
        win.draw(t);
    }

    void drawHistogram(sf::RenderWindow& win, const sf::FloatRect& r, const an::Report& rep) const {
        const bool ook = rep.family == an::Family::OOK;
        frame(win, r, ook ? "Envelope histogram" : rep.family == an::Family::FSK ? "Frequency at the symbol instants" : "Frequency histogram");
        if (rep.hist.empty()) return;
        const float maxV = std::max(1.0f, *std::max_element(rep.hist.begin(), rep.hist.end()));
        const float x0 = r.position.x + 4.0f, w = r.size.x - 8.0f, yb = r.position.y + r.size.y - 16.0f, h = r.size.y - 34.0f;
        const float bw = w / rep.hist.size();
        sf::VertexArray va(sf::PrimitiveType::Triangles);
        for (size_t i = 0; i < rep.hist.size(); i++) {
            const float bh = h * rep.hist[i] / maxV;
            const float x = x0 + i * bw;
            const sf::Color c = color();
            va.append({{x, yb}, c}); va.append({{x + bw - 1, yb}, c}); va.append({{x, yb - bh}, c});
            va.append({{x + bw - 1, yb}, c}); va.append({{x + bw - 1, yb - bh}, c}); va.append({{x, yb - bh}, c});
        }
        win.draw(va);
        // level markers (FSK)
        for (double lv : rep.levelHz) {
            const float x = x0 + w * (float)((lv - rep.histLo) / (rep.histHi - rep.histLo));
            sf::RectangleShape ln({1.0f, h});
            ln.setPosition({x, yb - h});
            ln.setFillColor(sf::Color(255, 255, 255, 70));
            win.draw(ln);
        }
        if (!ook) {
            char b[64];
            std::snprintf(b, sizeof(b), "%+.1f", rep.histLo / 1000.0);
            sf::Text a = label(std::string(b) + " kHz", 18, sf::Color(120, 120, 130));
            a.setPosition({x0, yb + 1.0f}); win.draw(a);
            std::snprintf(b, sizeof(b), "%+.1f kHz", rep.histHi / 1000.0);
            sf::Text c = label(b, 18, sf::Color(120, 120, 130));
            c.setPosition({x0 + w - c.getGlobalBounds().size.x, yb + 1.0f}); win.draw(c);
        }
    }

    void drawEye(sf::RenderWindow& win, const sf::FloatRect& r, const an::Report& rep) const {
        if (!rep.constel.empty()) {
            frame(win, r, "Constellation (symbol instants)");
            const float cx = r.position.x + r.size.x / 2, cy = r.position.y + r.size.y / 2 + 6.0f;
            const float sc = std::min(r.size.x, r.size.y - 20.0f) / 4.2f;
            sf::RectangleShape ax({r.size.x - 8.0f, 1.0f});
            ax.setPosition({r.position.x + 4.0f, cy}); ax.setFillColor(sf::Color(45, 45, 52)); win.draw(ax);
            sf::RectangleShape ay({1.0f, r.size.y - 22.0f});
            ay.setPosition({cx, r.position.y + 18.0f}); ay.setFillColor(sf::Color(45, 45, 52)); win.draw(ay);
            sf::VertexArray pts(sf::PrimitiveType::Triangles);
            for (const auto& p : rep.constel) {
                const float x = cx + p.real() * sc, y = cy - p.imag() * sc;
                if (!r.contains({x, y})) continue;
                const sf::Color c(255, 170, 60, 170);
                pts.append({{x - 1, y - 1}, c}); pts.append({{x + 1, y - 1}, c}); pts.append({{x - 1, y + 1}, c});
                pts.append({{x + 1, y - 1}, c}); pts.append({{x + 1, y + 1}, c}); pts.append({{x - 1, y + 1}, c});
            }
            win.draw(pts);
            return;
        }
        frame(win, r, rep.family == an::Family::OOK ? "Eye (envelope, 2 symbols)" : "Eye (frequency, 2 symbols)");
        if (rep.eye.empty() || rep.eyeLen < 2) return;
        const auto mm = std::minmax_element(rep.eye.begin(), rep.eye.end());
        const float lo = *mm.first, hi = *mm.second, span = std::max(1e-6f, hi - lo);
        const float x0 = r.position.x + 4.0f, w = r.size.x - 8.0f, y0 = r.position.y + 18.0f, h = r.size.y - 24.0f;
        sf::VertexArray va(sf::PrimitiveType::Lines);
        const sf::Color c(255, 150, 40, 45);
        const size_t traces = rep.eye.size() / rep.eyeLen;
        for (size_t t = 0; t < traces; t++) {
            const float* e = &rep.eye[t * rep.eyeLen];
            for (int k = 1; k < rep.eyeLen; k++) {
                const float xa = x0 + w * (k - 1) / (rep.eyeLen - 1), xb = x0 + w * k / (rep.eyeLen - 1);
                const float ya = y0 + h * (1.0f - (e[k - 1] - lo) / span), yb = y0 + h * (1.0f - (e[k] - lo) / span);
                va.append({{xa, ya}, c}); va.append({{xb, yb}, c});
            }
        }
        win.draw(va);
        // the sampling instant
        sf::RectangleShape mid({1.0f, h});
        mid.setPosition({x0 + w / 2, y0}); mid.setFillColor(sf::Color(255, 255, 255, 60));
        win.draw(mid);
    }

    void drawTimeline(sf::RenderWindow& win, const sf::FloatRect& r, const std::vector<an::BurstMark>& marks, double now, bool inBurst) const {
        frame(win, r, "Transmissions, last 20 s");
        const double span = 20.0;
        const float x0 = r.position.x + 4.0f, w = r.size.x - 8.0f, y = r.position.y + 17.0f, h = r.size.y - 21.0f;
        for (const auto& m : marks) {
            if (m.t + m.dur < now - span) continue;
            const double a = std::max(m.t, now - span), b = m.t + std::max(m.dur, 0.004);
            const float xa = x0 + w * (float)((a - (now - span)) / span), xb = x0 + w * (float)((b - (now - span)) / span);
            sf::RectangleShape bar({std::max(1.5f, xb - xa), h});
            bar.setPosition({xa, y});
            bar.setFillColor(color());
            win.draw(bar);
        }
        if (inBurst) {
            sf::RectangleShape now_({3.0f, h});
            now_.setPosition({x0 + w - 3.0f, y});
            now_.setFillColor(sf::Color(255, 220, 120));
            win.draw(now_);
        }
    }

    // Symbols, wrapped; preamble grey, the repeated part orange, sync words green
    void drawSymbols(sf::RenderWindow& win, const sf::FloatRect& r, const an::Report& rep) {
        frame(win, r, rep.symbols.empty() ? "Symbols" : "Symbols (" + std::to_string(rep.symbols.size()) + ", 0 = lowest " +
                                                         (rep.family == an::Family::OOK ? "level)" : "frequency)"));
        symRect = r;
        if (rep.symbols.empty()) {
            sf::Text t = label(rep.family == an::Family::FSK || rep.family == an::Family::OOK ? "" :
                               "Symbols are shown for FSK and OOK signals.", 20, sf::Color(110, 110, 120));
            t.setPosition({r.position.x + 6.0f, r.position.y + 20.0f});
            win.draw(t);
            return;
        }
        const size_t n = rep.symbols.size();
        std::vector<uint8_t> cls(n, 0);
        for (int i = rep.preambleStart; i < rep.preambleStart + rep.preambleLen && i < (int)n; i++) cls[i] = 1;
        if (rep.fixedLen > 0) for (int i = rep.fixedPos; i < rep.fixedPos + rep.fixedLen && i < (int)n; i++) cls[i] = 2;
        for (const auto& h : rep.syncs) {
            const an::KnownSync* ks = nullptr;
            for (const auto& k : an::KNOWN_SYNCS) if (h.name == k.name) ks = &k;
            if (!ks || h.pos < 0) continue;
            const int len = (int)std::strlen(ks->pattern);
            for (int i = h.pos; i < h.pos + len && i < (int)n; i++) cls[i] = 3;
        }
        const sf::Color cols[4] = {sf::Color(215, 215, 220), sf::Color(110, 110, 120), color(), sf::Color(110, 230, 120)};
        const float lineH = 15.0f;
        const size_t perLine = (size_t)std::max(8.0f, (r.size.x - 12.0f) / charW) / 8 * 8;   // whole groups of 8
        const size_t lines = (n + perLine - 1) / perLine;
        const int visible = std::max(1, (int)((r.size.y - 22.0f) / lineH));
        const float maxScroll = (float)std::max(0, (int)lines - visible);
        symScroll = std::min(symScroll, maxScroll);
        const size_t first = (size_t)symScroll;
        for (size_t li = first; li < lines && (int)(li - first) < visible; li++) {
            const size_t a = li * perLine, b = std::min(n, a + perLine);
            float x = r.position.x + 6.0f;
            const float y = r.position.y + 18.0f + (li - first) * lineH;
            size_t i = a;
            while (i < b) {
                size_t j = i;
                std::string seg;
                while (j < b && cls[j] == cls[i]) { seg += (char)('0' + rep.symbols[j]); j++; }
                sf::Text t = label(seg, 22, cols[cls[i]]);
                t.setPosition({x, y});
                win.draw(t);
                x = t.findCharacterPos(seg.size()).x;
                i = j;
            }
        }
        if (lines > (size_t)visible) {
            char b[48];
            std::snprintf(b, sizeof(b), "lines %zu-%zu of %zu (wheel)", first + 1, std::min(lines, first + visible), lines);
            sf::Text t = label(b, 18, sf::Color(120, 120, 130));
            t.setPosition({r.position.x + r.size.x - t.getGlobalBounds().size.x - 6.0f, r.position.y + 1.0f});
            win.draw(t);
        }
    }

public:
    void draw(sf::RenderWindow& win, float x, float y, float w, float h) override {
        if (!enabled || !font) return;
        shownMs = nowMs();
        panelRect = sf::FloatRect({x, y}, {w, h});
        std::vector<an::BurstMark> marks;
        double now, floorDb, levelDb, period = 0.0;
        bool inBurst;
        long long f;
        double width;
        {
            // the latest report (copied only when there is a new one)
            std::lock_guard<std::mutex> r(repMtx);
            if (trigMode == 2) {
                // Single: the first transmission after Arm, then hold
                if (armed && repCount > armedAt && latestRep.valid) {
                    shown = latestRep; shownReports = repCount; symScroll = 0.0f;
                    armed = false;
                }
            } else if (!frozen && repCount != shownReports) {
                if (latestRep.seq != shown.seq) symScroll = 0.0f;
                shown = latestRep;
                shownReports = repCount;
            }
        }
        {
            std::lock_guard<std::mutex> l(mtx);
            marks.assign(core.marks.begin(), core.marks.end());
            now = core.now(); floorDb = core.floorDb(); levelDb = core.levelDb(); inBurst = core.inBurst();
            period = core.period();
            f = chanHz; width = chanWidth;
        }
        const an::Report& rep = shown;

        sf::RectangleShape bg({w, h});
        bg.setPosition({x, y});
        bg.setFillColor(sf::Color(20, 22, 27, 245));
        bg.setOutlineColor(color());
        bg.setOutlineThickness(1.0f);
        win.draw(bg);

        const float pad = 8.0f;
        const float leftW = std::clamp(w * 0.34f, 300.0f, 470.0f);
        const float plotW = std::clamp((h - 2 * pad) * 1.05f, 150.0f, 260.0f);
        float sx = x + pad, sy = y + pad;

        // ---- left: verdict and measurements ----
        sf::Text title = label("Signal analyzer", 26, color());
        title.setStyle(sf::Text::Bold);
        title.setPosition({sx, sy});
        win.draw(title);
        {
            sf::CircleShape led(5.0f);
            led.setPosition({sx + title.getGlobalBounds().size.x + 10.0f, sy + 4.0f});
            led.setFillColor(inBurst ? sf::Color(255, 190, 60) : sf::Color(60, 60, 64));
            win.draw(led);
            char b[128];
            if (f != 0) std::snprintf(b, sizeof(b), "%.4f MHz, %.1f kHz wide   level %.0f / floor %.0f dB", f / 1e6, width / 1000.0, levelDb, floorDb);
            else std::snprintf(b, sizeof(b), "-");
            sf::Text t = label(b, 20, sf::Color(150, 150, 160));
            t.setPosition({sx + title.getGlobalBounds().size.x + 24.0f, sy + 2.0f});
            win.draw(t);
        }
        sy += 22.0f;
        if (trigMode == 2 && armed) {
            sf::Text t = label("ARMED - waiting for the next transmission", 24, sf::Color(255, 200, 80));
            t.setPosition({sx, sy}); win.draw(t);
            sy += 18.0f;
        } else if (trigMode == 2) {
            sf::Text t = label("TRIGGERED - holding (Arm for the next one)", 24, sf::Color(120, 230, 120));
            t.setPosition({sx, sy}); win.draw(t);
            sy += 18.0f;
        }
        if (!rep.valid) {
            sf::Text t = label("Drag the orange channel onto a signal and set its edges to the signal's width.\n"
                               "The analysis appears after the first transmission (or 3 s of a steady signal).", 22, sf::Color(130, 130, 140));
            t.setPosition({sx, sy + 4.0f});
            win.draw(t);
        } else {
            sf::Text v = label(rep.modulation, 34, sf::Color(255, 255, 255));
            v.setStyle(sf::Text::Bold);
            v.setPosition({sx, sy});
            win.draw(v);
            sy += 22.0f;
            char b[128];
            std::snprintf(b, sizeof(b), "%s %.0f ms%s%s", trigMode == 3 ? "Live, last" : rep.continuous ? "Steady signal, window" : "Transmission", rep.durMs,
                          rep.truncated ? " (part of a longer one)" : "", frozen ? "   [frozen]" : "");
            sf::Text d = label(b, 20, sf::Color(150, 150, 160));
            d.setPosition({sx, sy}); win.draw(d);
            sy += 15.0f;
            const float bottom = y + h - pad - 52.0f;
            const float colW = leftW - 2 * pad;
            // what it may be first (that is what the user wants to know), then the numbers
            for (const auto& g : rep.guesses) sy = drawWrapped(win, "> " + g, sx, sy, colW, bottom, 21, sf::Color(255, 190, 110), 14.0f, "   ");
            sy += 3.0f;
            for (const auto& n : rep.notes) sy = drawWrapped(win, n, sx, sy, colW, bottom, 21, sf::Color(205, 205, 212), 14.0f, "   ");
        }
        // buttons at the bottom of the left column: trigger + pipeline, then the report
        {
            const float ty = y + h - pad - 48.0f;
            btnTrig->setPosition(sx, ty); btnTrig->draw(win);
            btnArm->setPosition(sx + 116.0f, ty); btnArm->draw(win);
            btnLvlDown->setPosition(sx + 168.0f, ty); btnLvlDown->draw(win);
            double lv;
            { std::lock_guard<std::mutex> l(mtx); lv = trigDb; }
            char b[32];
            std::snprintf(b, sizeof(b), "%.0f dB", lv);
            sf::Text tl = label(b, 22, sf::Color(220, 220, 225));
            tl.setPosition({sx + 194.0f, ty + 3.0f}); win.draw(tl);
            btnLvlUp->setPosition(sx + 236.0f, ty); btnLvlUp->draw(win);
            btnPipe->setPosition(sx + 266.0f, ty); btnPipe->draw(win);
            const float by = y + h - pad - 22.0f;
            btnFreeze->setPosition(sx, by); btnFreeze->draw(win);
            btnCopy->setPosition(sx + 76.0f, by); btnCopy->draw(win);
            btnCopySym->setPosition(sx + 166.0f, by); btnCopySym->draw(win);
            btnClear->setPosition(sx + 262.0f, by); btnClear->draw(win);
        }

        // ---- middle: plots ----
        const float px = x + pad + leftW;
        const float ph = h - 2 * pad;
        drawHistogram(win, sf::FloatRect({px, y + pad}, {plotW, ph}), rep);
        drawEye(win, sf::FloatRect({px + plotW + pad, y + pad}, {plotW, ph}), rep);

        // ---- right: timeline and symbols ----
        const float rx = px + 2 * (plotW + pad);
        const float rw = x + w - pad - rx;
        if (rw > 120.0f) {
            drawTimeline(win, sf::FloatRect({rx, y + pad}, {rw, 42.0f}), marks, now, inBurst);
            if (period > 0.0) {
                char b[64];
                std::snprintf(b, sizeof(b), "period %.3f s", period);
                sf::Text t = label(b, 18, color());
                t.setPosition({rx + rw - t.getGlobalBounds().size.x - 6.0f, y + pad + 1.0f});
                win.draw(t);
            }
            drawSymbols(win, sf::FloatRect({rx, y + pad + 48.0f}, {rw, h - 2 * pad - 48.0f}), rep);
        }
    }
};
