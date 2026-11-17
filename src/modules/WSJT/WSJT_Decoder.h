#pragma once

#include "../Core/SDRModule.h"
#include "../Core/DetailsPopup.h"
#include "../Core/ModuleLog.h"
#include "../Core/PositionBus.h"
#include "../Core/NarrowChannel.h"
#include "Ft8Core.h"
#include "WsprCore.h"
#include <SFML/Graphics.hpp>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <deque>
#include <map>
#include <vector>
#include <string>
#include <sstream>
#include <ctime>
#include <cstdio>
#include <algorithm>

// =====================================================================
//  FT8 / FT4 / WSPR decoder module (HF digital weak signal modes).
//
//  Every known dial frequency in the received band gets its own channel
//  (e.g. with 2 MSps around 7.05 MHz: FT8 7.074, FT4 7.0475 and WSPR
//  7.0386 at once). A channel is a USB receiver: 0..3 kHz above the dial
//  frequency, cut out of the I/Q, resampled to 12 kHz (FT8 / FT4) or to
//  375 Hz around 1500 Hz (WSPR) and collected into UTC time slots
//  (15 s / 7.5 s / 2 min). A worker thread decodes each finished slot.
//  The time comes from the stream (wall clock, or the recording's time),
//  so the computer clock must be right (within ~1 s).
//
//  Audio input (USB audio of a transceiver, Input signal switch set to
//  Audio): the audio is decoded in the mode chosen in the panel.
// =====================================================================

namespace wsjt {

enum class Mode { FT8 = 0, FT4 = 1, WSPR = 2 };

inline const char* modeName(Mode m) { return m == Mode::FT8 ? "FT8" : m == Mode::FT4 ? "FT4" : "WSPR"; }
inline double slotSec(Mode m) { return m == Mode::FT8 ? 15.0 : m == Mode::FT4 ? 7.5 : 120.0; }

// Dial frequencies (USB) of the three modes, 160 m .. 2 m (IARU / WSJT-X defaults)
inline const std::vector<std::pair<long long, Mode>>& dialFrequencies() {
    static const std::vector<std::pair<long long, Mode>> f = {
        {1840000, Mode::FT8}, {3573000, Mode::FT8}, {5357000, Mode::FT8}, {7074000, Mode::FT8}, {10136000, Mode::FT8},
        {14074000, Mode::FT8}, {18100000, Mode::FT8}, {21074000, Mode::FT8}, {24915000, Mode::FT8}, {28074000, Mode::FT8},
        {50313000, Mode::FT8}, {70154000, Mode::FT8}, {144174000, Mode::FT8},
        {3575000, Mode::FT4}, {7047500, Mode::FT4}, {10140000, Mode::FT4}, {14080000, Mode::FT4}, {18104000, Mode::FT4},
        {21140000, Mode::FT4}, {24919000, Mode::FT4}, {28180000, Mode::FT4}, {50318000, Mode::FT4}, {144170000, Mode::FT4},
        {474200, Mode::WSPR}, {1836600, Mode::WSPR}, {3568600, Mode::WSPR}, {5287200, Mode::WSPR}, {7038600, Mode::WSPR},
        {10138700, Mode::WSPR}, {14095600, Mode::WSPR}, {18104600, Mode::WSPR}, {21094600, Mode::WSPR},
        {24924600, Mode::WSPR}, {28124600, Mode::WSPR}, {50293000, Mode::WSPR}, {144489000, Mode::WSPR} };
    return f;
}

inline bool modeFromName(const std::string& n, Mode& m) {
    if (n.rfind("FT8", 0) == 0) { m = Mode::FT8; return true; }
    if (n.rfind("FT4", 0) == 0) { m = Mode::FT4; return true; }
    if (n.rfind("WSPR", 0) == 0) { m = Mode::WSPR; return true; }
    return false;
}

// Maidenhead locator (4 or 6 characters) -> centre of the square
inline bool gridToLatLon(const std::string& g, double& lat, double& lon) {
    if (g.size() < 4) return false;
    const char a = (char)std::toupper((unsigned char)g[0]), b = (char)std::toupper((unsigned char)g[1]);
    if (a < 'A' || a > 'R' || b < 'A' || b > 'R' || !std::isdigit((unsigned char)g[2]) || !std::isdigit((unsigned char)g[3])) return false;
    lon = (a - 'A') * 20.0 - 180.0 + (g[2] - '0') * 2.0;
    lat = (b - 'A') * 10.0 - 90.0 + (g[3] - '0') * 1.0;
    if (g.size() >= 6 && std::isalpha((unsigned char)g[4]) && std::isalpha((unsigned char)g[5])) {
        const char c = (char)std::toupper((unsigned char)g[4]), d = (char)std::toupper((unsigned char)g[5]);
        if (c > 'X' || d > 'X') return false;
        lon += (c - 'A') * (2.0 / 24.0) + 1.0 / 24.0;
        lat += (d - 'A') * (1.0 / 24.0) + 0.5 / 24.0;
    } else {
        lon += 1.0;
        lat += 0.5;
    }
    return true;
}

inline bool looksLikeGrid(const std::string& t) {
    if (t.size() != 4 && t.size() != 6) return false;
    if (t == "RR73") return false;
    double la, lo;
    return gridToLatLon(t, la, lo);
}

} // namespace wsjt

class WSJTDecoder : public SDRModule {
    using Mode = wsjt::Mode;

    struct Entry {
        std::string utc;          // slot start, HHMMSS UTC
        Mode mode = Mode::FT8;
        long long dialHz = 0;     // AUDIO_DIAL: audio input
        float audioHz = 0;        // above the dial frequency
        float snr = 0, dt = 0, drift = 0;
        std::string text;
        std::string call, grid;   // sender and its locator, when the message has one
    };

    // A finished slot waiting for the worker thread
    struct Job {
        Mode mode;
        double slotStart;
        long long dialHz;
        int chanId;
        std::vector<float> audio;                  // FT8 / FT4: 12 kHz real
        std::vector<std::complex<float>> iq;       // WSPR: 375 Hz complex, centred on 1500 Hz
        double phase = 0.0;                        // slot timing offset tried (time not reliable)
        bool searching = false;                    // one of several offsets being tried
    };

    // One receiving channel
    struct Rx {
        Mode mode = Mode::FT8;
        long long dialHz = 0;
        NarrowChannel ch;
        // one slot assembler per timing offset: a single one normally, several while the
        // slot timing of a recording is searched (its start time is only guessed)
        std::vector<SlotAssembler<float>> slotsA;                 // FT8 / FT4
        std::vector<SlotAssembler<std::complex<float>>> slotsC;   // WSPR
        std::vector<double> offsets;
        bool tuner = false;       // the channel on the tuner
        std::vector<std::complex<float>> tmp;
        std::vector<float> tmpReal;
        double ncoPhase = 0.0;    // back to real audio (FT8 / FT4)
        int decodes = 0;          // in the last slot
        int total = 0;
        double level = 0.0;
        double lastSlot = 0.0;
        bool decoding = false;
    };

    static constexpr long long AUDIO_DIAL = -1;
    static constexpr int AUDIO_CHAN = -1;
    static constexpr double FTX_CENTRE = 1600.0;   // FT8 / FT4 channel centre above the dial (audio 200..3100 Hz)
    static constexpr double WSPR_CENTRE = 1500.0;

    std::mutex mtx;
    std::map<int, std::unique_ptr<Rx>> rxs;   // by channel id
    std::unique_ptr<Rx> audioRx;
    ChannelSet channels;
    std::deque<Entry> entries;               // newest first
    long long centerHz = 0;
    double lastRate = 0.0;
    long long lastCentre = -1;
    double streamTime = 0.0;
    bool timeReliable = true;
    // Slot timing found in a recording without a reliable start time (per mode)
    bool phaseLocked[3] = {false, false, false};
    double phaseOffset[3] = {0.0, 0.0, 0.0};
    std::map<std::string, double> recentDecodes;   // de-duplication while searching (key -> slot time)
    std::atomic<bool> audioInput {false};
    std::atomic<int> defaultMode {(int)Mode::FT8};
    double audioRate = 48000.0;
    std::string channelInfo = "Waiting for signal...";
    bool tunedOnly = false;
    int total = 0;

    // worker
    std::thread worker;
    std::mutex jobMtx;
    std::condition_variable jobCv;
    std::deque<Job> jobs;
    bool stopWorker = false;
    wspr::Decoder wsprDecoder;   // worker thread only (keeps the hashed calls)

    // UI
    const sf::Font* font = nullptr;
    sf::FloatRect panelRect, tableRect;
    float scrollRows = 0.0f, tableBodyY = 0.0f;
    const float rowH = 16.0f;
    int tableFirst = 0, tableVisible = 0;
    std::shared_ptr<SdrButton> btnClear, btnLog, btnExport, btnFT8, btnFT4, btnWSPR, btnCQ;
    bool onlyCQ = false;
    DetailsPopup popup;

    static std::string utcHms(double unix) {
        std::time_t t = (std::time_t)std::floor(unix + 0.5);
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &t);
#else
        gmtime_r(&t, &tm);
#endif
        char b[16];
        std::strftime(b, sizeof(b), "%H%M%S", &tm);
        return b;
    }

    static std::string dialText(long long hz) {
        if (hz == AUDIO_DIAL) return "audio";
        char b[24];
        std::snprintf(b, sizeof(b), "%.4f", hz / 1e6);
        return b;
    }

    sf::Text text(const std::string& s, unsigned size, sf::Color c) const {
        sf::Text t(*font, sf::String::fromUtf8(s.begin(), s.end()), size);
        t.setScale({0.5f, 0.5f});
        t.setFillColor(c);
        return t;
    }
    static std::string fit(const std::string& s, size_t n) { return s.size() > n ? s.substr(0, n - 1) + "." : s; }

    Mode modeOfChannel(const DecoderChannel& c) const {
        Mode m;
        if (wsjt::modeFromName(c.name, m)) return m;
        return (Mode)defaultMode.load();
    }

    // FT8 / FT4: who sent it and from where ("CQ SP5ABC KO02", "K1JT SP5ABC KO02")
    static void senderAndGrid(const std::string& textIn, std::string& call, std::string& grid) {
        std::istringstream ss(textIn);
        std::vector<std::string> tok;
        for (std::string t; ss >> t;) tok.push_back(t);
        call.clear(); grid.clear();
        if (tok.size() < 2) return;
        if (tok[0] == "CQ") {
            // CQ CALL [GRID]  or  CQ DX|POTA|... CALL [GRID]
            const size_t n = tok.size();
            size_t i = 1;
            if (n >= 4 || (n == 3 && !wsjt::looksLikeGrid(tok[2]))) i = 2;
            call = tok[i];
            if (i + 1 < n && wsjt::looksLikeGrid(tok[i + 1])) grid = tok[i + 1];
            return;
        }
        if (tok.size() >= 2) call = tok[1];
        if (tok.size() >= 3 && wsjt::looksLikeGrid(tok[2])) grid = tok[2];
        if (!call.empty() && call.front() == '<') call = call.substr(1, call.size() > 2 ? call.size() - 2 : 0);
    }

    void reportPosition(const Entry& e) {
        if (e.call.empty() || e.grid.empty() || e.call.find("...") != std::string::npos) return;
        double lat, lon;
        if (!wsjt::gridToLatLon(e.grid, lat, lon)) return;
        PosReport r;
        r.kind = PosKind::HF;
        r.id = e.call;
        r.label = e.call;
        char b[96];
        if (e.dialHz == AUDIO_DIAL)
            std::snprintf(b, sizeof(b), "%s %s, %+.0f dB", wsjt::modeName(e.mode), e.grid.c_str(), e.snr);
        else
            std::snprintf(b, sizeof(b), "%s %.3f MHz %s, %+.0f dB", wsjt::modeName(e.mode), (e.dialHz + e.audioHz) / 1e6, e.grid.c_str(), e.snr);
        r.info = b;
        r.lat = lat; r.lon = lon;
        positionBus().report(r);
    }

    // --- worker thread ---
    void workerLoop() {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> l(jobMtx);
                jobCv.wait(l, [this] { return stopWorker || !jobs.empty(); });
                if (stopWorker) return;
                job = std::move(jobs.front());
                jobs.pop_front();
            }
            std::vector<Entry> found;
            // the real stream time of the slot; "~" when the recording's time is only guessed
            const std::string utc = utcHms(job.slotStart - job.phase) + (job.phase != 0.0 || job.searching ? "~" : "");
            if (job.mode == Mode::WSPR) {
                for (const wspr::Spot& s : wsprDecoder.decode(job.iq)) {
                    Entry e;
                    e.utc = utc; e.mode = Mode::WSPR; e.dialHz = job.dialHz;
                    e.audioHz = (float)WSPR_CENTRE + s.freq;
                    e.snr = s.snr; e.dt = s.dt; e.drift = s.drift;
                    e.text = s.message;
                    e.call = s.call;
                    if (!e.call.empty() && e.call.front() == '<') e.call.clear();   // hashed: not known for sure
                    e.grid = s.grid;
                    found.push_back(e);
                }
            } else {
                const ftx::Proto p = job.mode == Mode::FT8 ? ftx::Proto::FT8 : ftx::Proto::FT4;
                for (const ftx::Decode& d : ftx::decodeSlot(p, job.audio)) {
                    Entry e;
                    e.utc = utc; e.mode = job.mode; e.dialHz = job.dialHz;
                    e.audioHz = d.freq;
                    e.snr = d.snr - 2.0f;   // the estimate reads ~2 dB high against WSJT-X
                    e.dt = d.dt - 0.5f;     // relative to the nominal start (0.5 s into the slot)
                    e.text = d.text;
                    senderAndGrid(d.text, e.call, e.grid);
                    found.push_back(e);
                }
                std::sort(found.begin(), found.end(), [](const Entry& a, const Entry& b) { return a.audioHz < b.audioHz; });
            }
            std::lock_guard<std::mutex> l(mtx);
            if (job.searching && !found.empty() && !phaseLocked[(int)job.mode]) {
                // this timing offset works: keep it
                phaseLocked[(int)job.mode] = true;
                phaseOffset[(int)job.mode] = job.phase;
            }
            if (job.searching || job.phase != 0.0) {
                // several offsets may decode the same transmission: show it once
                const double slotReal = job.slotStart - job.phase;
                const double window = wsjt::slotSec(job.mode) * 0.9;
                for (auto it = recentDecodes.begin(); it != recentDecodes.end();)
                    it = std::fabs(it->second - slotReal) > 600.0 ? recentDecodes.erase(it) : std::next(it);
                std::vector<Entry> unique;
                for (const Entry& e : found) {
                    const std::string key = std::to_string((int)e.mode) + "|" + std::to_string(e.dialHz) + "|" + e.text;
                    auto it = recentDecodes.find(key);
                    if (it != recentDecodes.end() && std::fabs(it->second - slotReal) < window) continue;
                    recentDecodes[key] = slotReal;
                    unique.push_back(e);
                }
                found.swap(unique);
            }
            Rx* rx = nullptr;
            if (job.chanId == AUDIO_CHAN) rx = audioRx.get();
            else if (auto it = rxs.find(job.chanId); it != rxs.end()) rx = it->second.get();
            if (rx) { rx->decodes = (int)found.size(); rx->total += (int)found.size(); rx->decoding = false; }
            for (auto it = found.rbegin(); it != found.rend(); ++it) {
                entries.push_front(*it);
                total++;
                reportPosition(*it);
                char b[160];
                std::snprintf(b, sizeof(b), "%s %-4s %s MHz %+5.0f Hz  %+3.0f dB  dt %+.1f  %s", it->utc.c_str(), wsjt::modeName(it->mode),
                              dialText(it->dialHz).c_str(), it->audioHz, it->snr, it->dt, it->text.c_str());
                log.add(b);
            }
            while (entries.size() > 3000) entries.pop_back();
        }
    }

    void queueJob(Job&& j) {
        {
            std::lock_guard<std::mutex> l(jobMtx);
            if (jobs.size() > 64) jobs.pop_front();   // the decoder can't keep up: drop the oldest
            jobs.push_back(std::move(j));
        }
        jobCv.notify_one();
    }

    // Feeds one channel's complex samples (input at `fs`, the channel centre already applied by `offset`)
    void runRx(Rx& r, int chanId, const std::complex<double>* in, size_t n, double fs, double centreOffsetHz, double t0) {
        const bool wsprMode = r.mode == Mode::WSPR;
        const double outRate = wsprMode ? wspr::SAMPLE_RATE : 12000.0;
        if (wsprMode) r.ch.configure(fs, centreOffsetHz, outRate, 180.0, 100.0);
        else r.ch.configure(fs, centreOffsetHz, outRate, FTX_CENTRE, 600.0);
        r.tmp.clear();
        r.ch.process(in, n, r.tmp);
        if (r.tmp.empty()) return;
        double lv = 0.0;
        for (const auto& v : r.tmp) lv += std::norm(v);
        r.level = 0.9 * r.level + 0.1 * (lv / r.tmp.size());

        const long long dial = r.dialHz;
        const Mode mode = r.mode;
        // timing offsets: one (0 s, or the one found), or a set to search while the
        // recording's start time is only a guess
        const double period = wsjt::slotSec(mode);
        const bool search = !timeReliable && !phaseLocked[(int)mode];
        std::vector<double> offs;
        if (search) {
            const double step = mode == Mode::WSPR ? 10.0 : mode == Mode::FT8 ? 2.5 : 1.25;
            for (double o = 0.0; o < period - 1e-6; o += step) offs.push_back(o);
        } else offs.push_back(timeReliable ? 0.0 : phaseOffset[(int)mode]);
        if (offs != r.offsets) {
            r.offsets = offs;
            r.slotsA.assign(wsprMode ? 0 : offs.size(), SlotAssembler<float>());
            r.slotsC.assign(wsprMode ? offs.size() : 0, SlotAssembler<std::complex<float>>());
        }
        if (wsprMode) {
            for (size_t k = 0; k < offs.size(); k++) {
                r.slotsC[k].configure(120.0, outRate, 3.0);
                r.slotsC[k].push(r.tmp.data(), r.tmp.size(), t0 + offs[k], [&](double slot, std::vector<std::complex<float>>& buf) {
                    Job j{mode, slot, dial, chanId, {}, std::move(buf), offs[k], search};
                    r.decoding = true;
                    r.lastSlot = slot;
                    queueJob(std::move(j));
                });
            }
        } else {
            // complex around FTX_CENTRE -> real USB audio 0..3 kHz
            r.tmpReal.resize(r.tmp.size());
            const double w = 2.0 * 3.14159265358979323846 * FTX_CENTRE / outRate;
            for (size_t i = 0; i < r.tmp.size(); i++) {
                r.tmpReal[i] = (float)(r.tmp[i].real() * std::cos(r.ncoPhase) - r.tmp[i].imag() * std::sin(r.ncoPhase));
                r.ncoPhase += w;
                if (r.ncoPhase > 6.283185307179586) r.ncoPhase -= 6.283185307179586;
            }
            for (size_t k = 0; k < offs.size(); k++) {
                r.slotsA[k].configure(period, outRate, mode == Mode::FT8 ? 1.5 : 0.8);
                r.slotsA[k].push(r.tmpReal.data(), r.tmpReal.size(), t0 + offs[k], [&](double slot, std::vector<float>& buf) {
                    Job j{mode, slot, dial, chanId, std::move(buf), {}, offs[k], search};
                    r.decoding = true;
                    r.lastSlot = slot;
                    queueJob(std::move(j));
                });
            }
        }
    }

    void openDetails(const Entry& e) {
        std::vector<std::pair<std::string, std::string>> rows;
        rows.push_back({"Time (UTC)", e.utc.substr(0, 2) + ":" + e.utc.substr(2, 2) + ":" + e.utc.substr(4, 2)});
        rows.push_back({"Mode", wsjt::modeName(e.mode)});
        if (e.dialHz == AUDIO_DIAL) rows.push_back({"Frequency", "audio " + std::to_string((int)std::lround(e.audioHz)) + " Hz"});
        else {
            char b[64];
            std::snprintf(b, sizeof(b), "%.6f MHz (dial %s + %.0f Hz)", (e.dialHz + e.audioHz) / 1e6, dialText(e.dialHz).c_str(), e.audioHz);
            rows.push_back({"Frequency", b});
        }
        char b[48];
        std::snprintf(b, sizeof(b), "%+.0f dB (in 2500 Hz)", e.snr);
        rows.push_back({"SNR", b});
        std::snprintf(b, sizeof(b), "%+.2f s", e.dt);
        rows.push_back({"Time offset", b});
        if (e.mode == Mode::WSPR) { std::snprintf(b, sizeof(b), "%.0f Hz", e.drift); rows.push_back({"Drift", b}); }
        if (!e.call.empty()) rows.push_back({"Station", e.call});
        if (!e.grid.empty()) {
            double la, lo;
            if (wsjt::gridToLatLon(e.grid, la, lo)) {
                std::snprintf(b, sizeof(b), "%s  (%.2f, %.2f) - on the map", e.grid.c_str(), la, lo);
                rows.push_back({"Locator", b});
            }
        }
        popup.show(std::string(wsjt::modeName(e.mode)) + "  " + e.text, rows, e.text);
    }

    void setDefaultMode(Mode m) {
        defaultMode = (int)m;
        if (btnFT8) { btnFT8->setActive(m == Mode::FT8); btnFT4->setActive(m == Mode::FT4); btnWSPR->setActive(m == Mode::WSPR); }
        channels.clear();   // placed again: the channel on the tuner takes the new mode
        std::lock_guard<std::mutex> l(mtx);
        audioRx.reset();
    }

public:
    ModuleLog log;
    std::string exportDir;

    WSJTDecoder() : SDRModule("FT8/WSPR") {
        channels.name = "FT8";
        for (const auto& p : wsjt::dialFrequencies()) channels.presets.push_back(p.first);
        channels.maxChannels = 6;
        channels.alwaysTuner = true;   // decode what the tuner is on as well as the dial frequencies
        channels.defaultWidth = 3000.0;
        channels.sideband = +1;
        channels.listenMode = ::Mode::USB;
        channels.shape = [this](DecoderChannel& c) {
            // the mode of a known dial frequency; elsewhere the channel keeps its mode
            // (or takes the one chosen in the panel)
            Mode m = (Mode)defaultMode.load();
            bool known = false;
            if (!c.followVfo)
                for (const auto& p : wsjt::dialFrequencies())
                    if (std::llabs(p.first - c.freqHz) < 300) { m = p.second; known = true; break; }
            if (!known && !c.followVfo) wsjt::modeFromName(c.name, m);
            c.name = std::string(wsjt::modeName(m)) + (c.followVfo ? " tuner" : "");
            c.widthHz = 3000.0;
        };
        worker = std::thread([this] { workerLoop(); });
    }

    ~WSJTDecoder() override {
        {
            std::lock_guard<std::mutex> l(jobMtx);
            stopWorker = true;
        }
        jobCv.notify_all();
        if (worker.joinable()) worker.join();
    }

    static bool ft8Available() { return ftx::available(); }

    void init(const ModuleContext& ctx) override {
        font = &ctx.font;
        audioRate = ctx.sampleRate > 0.0f ? ctx.sampleRate : 48000.0;
        popup.init(*font);
        btnClear = std::make_shared<SdrButton>(100.0f, 24.0f, "Clear list", *font);
        btnClear->onClick = [this]() {
            std::lock_guard<std::mutex> l(mtx);
            entries.clear(); total = 0;
            for (auto& kv : rxs) { kv.second->total = 0; kv.second->decodes = 0; }
            if (audioRx) { audioRx->total = 0; audioRx->decodes = 0; }
            positionBus().clear(PosKind::HF);
        };
        btnLog = std::make_shared<SdrButton>(100.0f, 24.0f, "Open log", *font);
        btnLog->onClick = [this]() { log.openLogFile(); };
        btnExport = std::make_shared<SdrButton>(100.0f, 24.0f, "Export", *font);
        btnExport->onClick = [this]() { log.exportSession(exportDir, "FT8_WSPR"); };
        btnFT8 = std::make_shared<SdrButton>(50.0f, 22.0f, "FT8", *font);
        btnFT4 = std::make_shared<SdrButton>(50.0f, 22.0f, "FT4", *font);
        btnWSPR = std::make_shared<SdrButton>(56.0f, 22.0f, "WSPR", *font);
        btnCQ = std::make_shared<SdrButton>(80.0f, 22.0f, "Only CQ", *font);
        btnFT8->onClick = [this]() { setDefaultMode(Mode::FT8); };
        btnFT4->onClick = [this]() { setDefaultMode(Mode::FT4); };
        btnWSPR->onClick = [this]() { setDefaultMode(Mode::WSPR); };
        btnCQ->onClick = [this]() { onlyCQ = !onlyCQ; btnCQ->setActive(onlyCQ); scrollRows = 0; };
        btnFT8->setActive(true);
    }

    void reset() override {
        std::lock_guard<std::mutex> l(mtx);
        rxs.clear();
        audioRx.reset();
    }

    float panelHeight(float windowHeight) const override { return std::clamp(windowHeight * 0.38f, 260.0f, 460.0f); }
    void setRadioFrequency(long long hz) override { std::lock_guard<std::mutex> l(mtx); centerHz = hz; }
    void setAudioInput(bool audio) override { audioInput = audio; }
    void setStreamTime(double t, bool reliable) override {
        std::lock_guard<std::mutex> l(mtx);
        streamTime = t;
        if (reliable != timeReliable) { timeReliable = reliable; resetTiming(); }
    }

    // (mtx held) forget the slot timing found in a recording
    void resetTiming() {
        for (int i = 0; i < 3; i++) { phaseLocked[i] = false; phaseOffset[i] = 0.0; }
        recentDecodes.clear();
    }
    ChannelSet* channelSet() override { return &channels; }
    bool modalOpen() const override { return popup.isOpen(); }
    void drawModal(sf::RenderWindow& win) override { popup.draw(win); }

    // Audio input: the USB audio of a receiver (0..3 kHz), decoded in the chosen mode
    void processAudio(const std::vector<float>& audio) override {
        if (!enabled || !audioInput || audio.empty()) return;
        std::lock_guard<std::mutex> l(mtx);
        const Mode m = (Mode)defaultMode.load();
        if (!audioRx || audioRx->mode != m) { audioRx = std::make_unique<Rx>(); audioRx->mode = m; audioRx->dialHz = AUDIO_DIAL; }
        std::vector<std::complex<double>> c(audio.size());
        for (size_t i = 0; i < audio.size(); i++) c[i] = std::complex<double>(audio[i], 0.0);
        const double centre = m == Mode::WSPR ? WSPR_CENTRE : FTX_CENTRE;
        runRx(*audioRx, AUDIO_CHAN, c.data(), c.size(), audioRate, centre, streamTime);
        channelInfo = std::string("Input signal: Audio (") + wsjt::modeName(m) + ")";
    }

    void processIQ(const std::vector<std::complex<double>>& iq, double sampleRate, double tunedOffset) override {
        if (!enabled || iq.empty() || sampleRate <= 0.0) return;
        std::lock_guard<std::mutex> l(mtx);
        if (audioInput) { rxs.clear(); return; }
        if (sampleRate < 24000.0) { channelInfo = "Sample rate too low"; return; }
        audioRx.reset();
        const auto rc = channels.update(centerHz, sampleRate, tunedOffset);
        tunedOnly = true;
        for (const auto& c : rc) if (!c.ch.followVfo) tunedOnly = false;
        if (sampleRate != lastRate || centerHz != lastCentre) {
            // another file / band: the slot timing found in a recording no longer applies
            if (sampleRate != lastRate) rxs.clear();
            if (std::llabs(centerHz - lastCentre) > 100000) resetTiming();
            lastRate = sampleRate;
            lastCentre = centerHz;
        }
        for (auto it = rxs.begin(); it != rxs.end();) {
            bool keep = false;
            for (const auto& c : rc) if (c.id == it->first) keep = true;
            if (!keep) it = rxs.erase(it); else ++it;
        }
        for (const auto& c : rc) {
            auto& slot = rxs[c.id];
            const Mode m = modeOfChannel(c.ch);
            if (!slot || slot->mode != m || slot->dialHz != c.freqHz) {
                // new channel, other mode or retuned: start the slots again
                const int keepTotal = slot ? slot->total : 0;
                slot = std::make_unique<Rx>();
                slot->mode = m;
                slot->total = keepTotal;
            }
            slot->dialHz = c.freqHz;
            slot->tuner = c.ch.followVfo;
            if (!c.inBand) continue;
            // the tuner sitting on a known dial frequency: that channel decodes it already
            bool dup = false;
            if (c.ch.followVfo)
                for (const auto& o : rc)
                    if (!o.ch.followVfo && std::llabs(o.freqHz - c.freqHz) < 500 && modeOfChannel(o.ch) == m) dup = true;
            if (dup) continue;
            const double centre = m == Mode::WSPR ? WSPR_CENTRE : FTX_CENTRE;
            runRx(*slot, c.id, iq.data(), iq.size(), sampleRate, c.offsetHz + centre, streamTime);
        }
        channelInfo = tunedOnly ? "No FT8/FT4/WSPR dial frequency in band: only the tuner channel"
                                : std::to_string(rxs.size()) + " channel(s), incl. the tuner";
        if (!timeReliable) {
            bool anyLocked = false;
            for (bool b : phaseLocked) anyLocked = anyLocked || b;
            channelInfo += anyLocked ? "  |  slot timing found in the recording"
                                     : "  |  recording time unknown: searching the slot timing";
        }
    }

    bool handleEvent(const sf::Event& ev, const sf::RenderWindow& win) override {
        if (!enabled) return false;
        if (popup.handleEvent(ev, win)) return true;
        for (auto* b : {&btnClear, &btnLog, &btnExport, &btnFT8, &btnFT4, &btnWSPR, &btnCQ})
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
                    const auto list = visibleEntries();
                    const int idx = tableFirst + row;
                    if (row >= 0 && row < tableVisible && idx < (int)list.size()) { e = *list[idx]; found = true; }
                }
                if (found) openDetails(e);
            }
            return true;
        }
        return false;
    }

    // (mtx held)
    std::vector<const Entry*> visibleEntries() const {
        std::vector<const Entry*> v;
        for (const Entry& e : entries)
            if (!onlyCQ || e.text.rfind("CQ ", 0) == 0) v.push_back(&e);
        return v;
    }

    void draw(sf::RenderWindow& win, float x, float y, float w, float h) override {
        if (!enabled || !font) return;
        panelRect = sf::FloatRect({x, y}, {w, h});

        std::vector<Entry> list;
        struct ChanLine { std::string s; sf::Color c; };
        std::vector<ChanLine> chans;
        std::string info;
        int count = 0;
        bool tuned = false;
        {
            std::lock_guard<std::mutex> l(mtx);
            const auto vis = visibleEntries();
            const int first = (int)scrollRows;
            for (int i = first; i < (int)vis.size() && i < first + 60; i++) list.push_back(*vis[i]);
            tableFirst = first;
            count = (int)vis.size();
            auto line = [&](const Rx& r) {
                char b[128];
                const double period = wsjt::slotSec(r.mode);
                const double into = r.mode == Mode::WSPR ? (r.slotsC.empty() ? -1.0 : r.slotsC[0].secondsIntoSlot())
                                                         : (r.slotsA.empty() ? -1.0 : r.slotsA[0].secondsIntoSlot());
                const double db = r.level > 1e-15 ? 10.0 * std::log10(r.level) : -150.0;
                std::string st = r.decoding ? "decoding..." : into < 0 ? "waiting" : "next in " + std::to_string((int)std::max(0.0, period - into)) + " s";
                std::snprintf(b, sizeof(b), "%-4s %s%s  last %2d  total %4d  %5.1f dB  %s", wsjt::modeName(r.mode),
                              r.dialHz == AUDIO_DIAL ? "audio   " : (dialText(r.dialHz) + " MHz").c_str(), r.tuner ? " (tuner)" : "",
                              r.decodes, r.total, db, st.c_str());
                chans.push_back({b, sf::Color(200, 200, 205)});
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

        const float pad = 8.0f;
        const float sideW = 360.0f;
        tableRect = sf::FloatRect({x + pad, y + pad}, {w - sideW - 2 * pad, h - 2 * pad});

        struct Col { const char* name; float w; };
        const float fixedW = 52 + 40 + 64 + 46 + 38 + 38;
        const float textW = std::max(80.0f, tableRect.size.x - fixedW);
        const Col cols[] = { {"UTC", 52}, {"Mode", 40}, {"Dial MHz", 64}, {"Hz", 46}, {"dB", 38}, {"DT", 38}, {"Message", textW} };
        const int nCols = 7;
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
            const std::string hint = !ftx::available() ? "FT8 / FT4 are not in this build (ft8_lib missing); WSPR works."
                                   : tuned ? "The tuner channel is USB: put the tuner just below the signals (decoded 200..3100 Hz above it), or tune near 7.074 / 14.074 MHz."
                                           : "Waiting for the end of the slot (FT8: every 15 s, FT4: 7.5 s, WSPR: 2 min)...";
            sf::Text e = text(hint, 24, sf::Color(110, 110, 120));
            e.setPosition({tableRect.position.x, tableBodyY + 4.0f});
            win.draw(e);
        }
        const float charW = 5.6f;
        for (int r = 0; r < visibleRows && r < (int)list.size(); r++) {
            const Entry& en = list[r];
            const float ry = tableBodyY + r * rowH;
            if (r % 2 == 0) {
                sf::RectangleShape stripe({tableRect.size.x, rowH});
                stripe.setPosition({tableRect.position.x, ry});
                stripe.setFillColor(sf::Color(255, 255, 255, 8));
                win.draw(stripe);
            }
            char hz[16], db[16], dt[16];
            std::snprintf(hz, sizeof(hz), "%.0f", en.audioHz);
            std::snprintf(db, sizeof(db), "%+.0f", en.snr);
            std::snprintf(dt, sizeof(dt), "%+.1f", en.dt);
            const std::string cells[7] = { en.utc, wsjt::modeName(en.mode), dialText(en.dialHz), hz, db, dt, en.text };
            const bool cq = en.text.rfind("CQ ", 0) == 0;
            float px = tableRect.position.x;
            for (int c = 0; c < nCols; c++) {
                if (px + 20.0f > tableRect.position.x + tableRect.size.x) break;
                const size_t maxChars = (size_t)std::max(3.0f, (cols[c].w - 6.0f) / charW);
                sf::Color col = sf::Color(225, 225, 230);
                if (c == 6 && cq) col = sf::Color(120, 235, 140);
                if (c == 1) col = en.mode == Mode::WSPR ? sf::Color(255, 200, 120) : en.mode == Mode::FT4 ? sf::Color(120, 200, 255) : sf::Color(200, 200, 210);
                sf::Text td = text(fit(cells[c], maxChars), 22, col);
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

        // --- side column ---
        const float sx = x + w - sideW;
        float sy = y + pad;
        sf::Text title = text("FT8 / FT4 / WSPR", 30, Theme::Accent);
        title.setStyle(sf::Text::Bold);
        title.setPosition({sx, sy});
        win.draw(title);
        sf::Text st = text(std::to_string(count) + " decodes", 22, sf::Color(220, 220, 225));
        st.setPosition({sx + 160.0f, sy + 4.0f});
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
        sf::Text ml = text("Tuner channel:", 20, sf::Color(160, 160, 170));
        ml.setPosition({sx, sy + 4.0f});
        win.draw(ml);
        btnFT8->setPosition(sx + 70.0f, sy); btnFT8->draw(win);
        btnFT4->setPosition(sx + 124.0f, sy); btnFT4->draw(win);
        btnWSPR->setPosition(sx + 178.0f, sy); btnWSPR->draw(win);
        btnCQ->setPosition(sx + 250.0f, sy); btnCQ->draw(win);
        sy += 30.0f;
        sf::Text lt = text("Channels (click a table row for details)", 22, Theme::Accent);
        lt.setPosition({sx, sy});
        win.draw(lt);
        sy += 18.0f;
        for (const auto& c : chans) {
            if (sy > y + h - 16.0f) break;
            sf::Text ct = text(c.s, 20, c.c);
            ct.setPosition({sx, sy});
            win.draw(ct);
            sy += 15.0f;
        }
    }
};
