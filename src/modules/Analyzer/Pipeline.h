#pragma once

#include <SFML/Graphics.hpp>
#include <SFML/Window/Clipboard.hpp>
#include <vector>
#include <deque>
#include <complex>
#include <string>
#include <memory>
#include <mutex>
#include <map>
#include <set>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <functional>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <random>
#include <array>
#include <ctime>
#include <cctype>
#include <chrono>
#include <cstring>
#include "../../DSP.h"
#include "../../PlatformUtils.h"     // getExecutableDir (where the preset files are)
#include "../../NativeDialogs.h"      // openWithDefaultApp (the Script block's "Edit code")
#include "../../../deps/lua/lua.h"    // the Script block (PipelineScript.h); Lua itself: LuaImpl.h
#include "../../../deps/lua/lauxlib.h"
#include "../../../deps/lua/lualib.h"
#include <filesystem>
#include <sys/stat.h>
#include "../../orbit/Json.h"
#include "AnalyzerCore.h"

// =====================================================================
//  Signal pipeline: blocks connected by wires, fed with the analyzer
//  channel's baseband.
//
//  A block has typed inputs / outputs (I/Q, real, symbols), parameters,
//  optional buttons and an optional view (scope, spectrum, bits ...).
//  The pipeline runs the blocks in topological order on every block of
//  samples (the analyzer's worker thread); the editor (PipelineEditor.h)
//  changes it on the UI thread - one mutex guards both.
//
//  A stream carries `newBurst`: its first sample starts a new
//  transmission (set by the burst gate, passed on by every block) so
//  clock recovery restarts and the bits view starts a new line.
//
//  Pipelines are saved as JSON (PipelineStore: pipelines.json in the
//  settings folder) and can be exported / imported.
// =====================================================================

namespace pl {

using CF = std::complex<float>;
constexpr double PI_D = 3.14159265358979323846;

enum class PT { IQ, Real, Sym, Frames, Lines, Any, All };   // Any: an input that takes I/Q or real; All: anything (Script)
// Frames: whole frames / packets of bytes. Lines: rows of an image (8-bit grey)
inline const char* ptName(PT t) {
    return t == PT::IQ ? "I/Q" : t == PT::Real ? "real" : t == PT::Sym ? "symbols / bits" : t == PT::Frames ? "frames (bytes)"
         : t == PT::Lines ? "image lines" : t == PT::All ? "anything" : "I/Q or real";
}
inline sf::Color ptColor(PT t) {
    return t == PT::IQ ? sf::Color(90, 200, 255) : t == PT::Real ? sf::Color(120, 230, 120) : t == PT::Sym ? sf::Color(255, 190, 80)
         : t == PT::Frames ? sf::Color(240, 110, 200) : t == PT::Lines ? sf::Color(250, 250, 250) : sf::Color(200, 200, 210);
}

constexpr uint8_t FRAME = 0xFF;   // in a symbol stream: a frame starts here

struct Stream {
    PT type = PT::IQ;
    double rate = 0.0;
    bool newBurst = false;
    int levels = 2;                 // symbols: how many values (2 = bits)
    std::vector<CF> iq;
    std::vector<float> re;
    std::vector<uint8_t> sym;
    std::vector<std::vector<uint8_t>> frames;   // Frames: one entry per frame / packet
    std::vector<std::vector<uint8_t>> lines;    // Lines: one entry per image row
    void clear() { iq.clear(); re.clear(); sym.clear(); frames.clear(); lines.clear(); newBurst = false; }
    size_t size() const {
        return type == PT::IQ ? iq.size() : type == PT::Real ? re.size() : type == PT::Sym ? sym.size()
             : type == PT::Frames ? frames.size() : lines.size();
    }
};

// Set once by main.cpp (native dialogs, folders)
struct EditorHooks {
    std::function<std::string()> storePath;                                            // pipelines.json
    std::function<std::string(const std::string& name)> saveJsonDialog;                // empty = cancelled
    std::function<std::string()> openJsonDialog;
    std::function<void(const std::string& title, const std::string& msg)> popup;
    std::function<std::string()> imageDir;                                             // where pictures are saved
    std::function<std::string()> recordDir;                                            // where files are written (WAV, bytes)
    std::function<std::vector<std::string>()> audioDevices;                            // sound cards ([0] = system default)
    std::function<void(int stream, int device)> audioRoute;                            // a stream to a sound card
    std::function<void(int stream, const std::vector<float>& lr)> audioPush;          // 48 kHz stereo interleaved
    std::function<void(int stream)> audioClose;
    // the receivers a Channel input can take: "RX1  RTL-SDR  101.700" ... (the first word is the key)
    std::function<std::vector<std::string>()> receivers;
};
inline EditorHooks& hooks() { static EditorHooks h; return h; }

struct Param {
    enum Kind { Num, Choice, Text } kind = Num;
    std::string key, label, unit;
    double v = 0.0, lo = 0.0, hi = 1.0, step = 1.0;
    int decimals = 0;
    std::vector<std::string> choices;
    bool dynamic = false;           // choices from the system (sound cards): saved by name too
    // where a dynamic list comes from (refreshed by the editor); fixed[] stay in front of it
    std::function<std::vector<std::string>()> source;
    std::vector<std::string> fixed;
    bool keyFirstWord = false;      // the same entry when the first word matches ("RX1  ..." stays RX1)
    bool hidden = false;            // saved, but not listed on the block (a Script's code: edited outside)
    // shown only while the choice parameter `showKey` has one of the values in showWhen
    std::string showKey;
    std::vector<int> showWhen;
    std::string text;
    std::string show() const {
        if (kind == Choice) return choices.empty() ? "" : choices[std::clamp((int)v, 0, (int)choices.size() - 1)];
        if (kind == Text) return text.empty() ? "(empty)" : text;
        char b[48];
        // a value finer than the shown decimals (0.1 Hz with 0 decimals): its own digits
        const double q = std::pow(10.0, decimals);
        if (v != 0.0 && std::fabs(v * q - std::round(v * q)) > 1e-6 * std::max(1.0, std::fabs(v * q))) std::snprintf(b, sizeof(b), "%.4g", v);
        else std::snprintf(b, sizeof(b), "%.*f", decimals, v);
        return unit.empty() ? std::string(b) : std::string(b) + " " + unit;
    }
};

struct Port { std::string name; PT type; };

// The colours of a multi-input view's channels (the Scope's traces and its input numbers)
inline sf::Color channelColor(int k) {
    static const sf::Color pal[] = {{120, 230, 120}, {255, 200, 70}, {240, 110, 200}, {90, 200, 255},
                                    {255, 140, 60}, {180, 140, 255}, {230, 230, 230}, {255, 90, 90}};
    return pal[((k % 8) + 8) % 8];
}

// Speaker output of the pipeline (48 kHz mono), filled by the Audio block
// 48 kHz stereo (left, right interleaved), at most a second
struct AudioRing {
    std::mutex m;
    std::deque<float> q;
    void push(const std::vector<float>& lr) {
        std::lock_guard<std::mutex> l(m);
        q.insert(q.end(), lr.begin(), lr.end());
        while (q.size() > 2 * 48000) { q.pop_front(); q.pop_front(); }
    }
    size_t mix(float* left, float* right, size_t n) {   // frames added (0: nothing waiting)
        std::lock_guard<std::mutex> l(m);
        if (q.size() < 2) return 0;
        size_t i = 0;
        for (; i < n && q.size() >= 2; i++) {
            left[i] += q.front(); q.pop_front();
            right[i] += q.front(); q.pop_front();
        }
        return i;
    }
};

// Text drawing for the views
struct Draw {
    const sf::Font* font = nullptr;
    sf::RenderTarget* win = nullptr;
    float text(const std::string& s, float x, float y, unsigned size, sf::Color c) const {
        sf::Text t(*font, sf::String::fromUtf8(s.begin(), s.end()), size * 2);
        t.setScale({0.5f, 0.5f});
        t.setFillColor(c);
        t.setPosition({std::round(x), std::round(y)});
        win->draw(t);
        return t.getGlobalBounds().size.x;
    }
    float width(const std::string& s, unsigned size) const {
        sf::Text t(*font, sf::String::fromUtf8(s.begin(), s.end()), size * 2);
        return t.getGlobalBounds().size.x * 0.5f;
    }
    void rect(const sf::FloatRect& r, sf::Color fill, sf::Color outline = sf::Color::Transparent) const {
        sf::RectangleShape s(r.size);
        s.setPosition(r.position);
        s.setFillColor(fill);
        if (outline.a) { s.setOutlineColor(outline); s.setOutlineThickness(1.0f); }
        win->draw(s);
    }
};

// ---------------------------------------------------------------------
//  Block base
// ---------------------------------------------------------------------
class Block {
public:
    std::string type, title, category;
    int id = 0;
    float x = 0.0f, y = 0.0f;
    std::vector<Param> params;
    std::vector<Port> ins, outs;
    std::vector<Stream> out;
    AudioRing* audio = nullptr;
    bool removable = true;
    int paramVersion = 0;          // bumped by the editor on every change
    double tick = 0.0;             // seconds this process() call stands for (generators)
    std::string warning;           // set by the pipeline: an input of the wrong kind (shown red)
    bool wantsSave = false;        // the block changed a parameter itself (a Script's code edited outside): the editor saves
    int maxIns = 0;                // > 1: a free input appears for every wire connected (up to this many)
    bool isDelay = false;          // a Delay: a feedback loop may pass through it (see Pipeline::runLoop)
    // In a feedback loop: the delay hands out `n` samples it has (of kind t) before it gets its
    // input, then takes as many in. loopDelay(): how many it holds (the loop's step).
    virtual size_t loopDelay() const { return 1; }
    virtual void loopEmit(size_t /*n*/, PT /*t*/, double /*rate*/) {}
    virtual void loopAbsorb(const Stream* /*in*/, size_t /*n*/) {}
    bool isInput = false;          // a Channel input (filled by the workspace, not processed)
    bool syncInputs = false;       // its sample inputs are lined up (same length, same rate) before process()
    std::string note;              // set by the pipeline: e.g. an input converted to another rate (shown amber)
    double cpu = 0.0;              // the share of real time its work takes (smoothed; set by the pipeline)
    double cpuAcc = 0.0;           // (seconds of work since the last update)

    // A dynamic choice list (receivers, sound cards) read again; the chosen entry stays chosen
    // by name, and one that went away stays in the list marked "(not available)"
    static constexpr const char* GONE = "  (not available)";
    void refreshChoices() {
        for (auto& p : params) {
            if (p.kind != Param::Choice || !p.source) continue;
            std::vector<std::string> fresh = p.fixed;
            for (auto& c : p.source()) fresh.push_back(c);
            if (fresh == p.choices) continue;
            std::string cur = p.show();
            if (cur.size() > std::strlen(GONE) && cur.compare(cur.size() - std::strlen(GONE), std::string::npos, GONE) == 0) cur.erase(cur.size() - std::strlen(GONE));
            auto key = [&](const std::string& s) { return p.keyFirstWord ? s.substr(0, s.find(' ')) : s; };
            int idx = -1;
            for (size_t i = 0; i < fresh.size() && idx < 0; i++) if (fresh[i] == cur) idx = (int)i;
            for (size_t i = 0; i < fresh.size() && idx < 0; i++) if (key(fresh[i]) == key(cur)) idx = (int)i;
            if (idx < 0 && (int)p.v >= (int)p.fixed.size()) { fresh.push_back(cur + GONE); idx = (int)fresh.size() - 1; }
            p.choices = fresh;
            p.hi = (double)fresh.size() - 1;
            p.v = std::max(0, idx);
            paramVersion++;
        }
    }

    virtual ~Block() {}
    virtual void reset() {}
    virtual void process(const std::vector<const Stream*>& in) = 0;
    virtual std::string status() const { return ""; }
    virtual std::vector<std::string> buttons() const { return {}; }
    virtual void onButton(int /*i*/) {}
    virtual sf::Vector2f viewSize() const { return {0.0f, 0.0f}; }
    virtual void drawView(const Draw& /*d*/, const sf::FloatRect& /*r*/) {}

    Param& addNum(const std::string& key, const std::string& label, double v, double lo, double hi, double step, const std::string& unit, int dec = 0) {
        Param p; p.kind = Param::Num; p.key = key; p.label = label; p.v = v; p.lo = lo; p.hi = hi; p.step = step; p.unit = unit; p.decimals = dec;
        params.push_back(p);
        return params.back();
    }
    Param& addChoice(const std::string& key, const std::string& label, std::vector<std::string> ch, int v = 0) {
        Param p; p.kind = Param::Choice; p.key = key; p.label = label; p.choices = std::move(ch); p.v = v; p.lo = 0; p.hi = (double)p.choices.size() - 1;
        params.push_back(p);
        return params.back();
    }
    Param& addText(const std::string& key, const std::string& label, const std::string& t) {
        Param p; p.kind = Param::Text; p.key = key; p.label = label; p.text = t;
        params.push_back(p);
        return params.back();
    }
    const Param* param(const std::string& k) const { for (const auto& p : params) if (p.key == k) return &p; return nullptr; }
    double num(const std::string& k) const { const Param* p = param(k); return p ? p->v : 0.0; }
    int choice(const std::string& k) const { const Param* p = param(k); return p ? (int)p->v : 0; }
    std::string text(const std::string& k) const { const Param* p = param(k); return p ? p->text : std::string(); }
    // Show parameter `key` only while choice `on` is one of `values` (the others stay hidden)
    void showOnly(const std::string& key, const std::string& on, std::vector<int> values) {
        for (auto& p : params) if (p.key == key) { p.showKey = on; p.showWhen = std::move(values); }
    }
    bool shown(const Param& p) const {
        if (p.hidden) return false;
        if (p.showKey.empty()) return true;
        const int c = choice(p.showKey);
        return std::find(p.showWhen.begin(), p.showWhen.end(), c) != p.showWhen.end();
    }

    void setOuts(std::vector<Port> o) {
        outs = std::move(o);
        out.assign(outs.size(), Stream());
        for (size_t i = 0; i < outs.size(); i++) out[i].type = outs[i].type;
    }

protected:
    // reconfigure when a parameter or the input rate changed
    int seenVersion = -1;
    double seenRate = -1.0;
    bool changed(double rate) {
        if (seenVersion == paramVersion && seenRate == rate) return false;
        seenVersion = paramVersion; seenRate = rate;
        return true;
    }
};

// ---------------------------------------------------------------------
//  DSP helpers
// ---------------------------------------------------------------------
inline std::vector<float> designLowpass(double fs, double cutoff) {
    cutoff = std::clamp(cutoff, fs * 0.001, fs * 0.49);
    const double trans = std::max(cutoff * 0.3, fs * 0.005);
    int taps = (int)std::ceil(5.5 * fs / trans) | 1;
    taps = std::clamp(taps, 15, 401);
    std::vector<float> h(taps);
    const double fc = cutoff / fs;
    double sum = 0.0;
    for (int i = 0; i < taps; i++) {
        const int m = i - taps / 2;
        const double s = m == 0 ? 2.0 * fc : std::sin(2.0 * PI_D * fc * m) / (PI_D * m);
        const double w = 0.42 - 0.5 * std::cos(2.0 * PI_D * i / (taps - 1)) + 0.08 * std::cos(4.0 * PI_D * i / (taps - 1));
        h[i] = (float)(s * w);
        sum += h[i];
    }
    for (auto& v : h) v = (float)(v / sum);
    return h;
}

template <typename T>
struct Fir {
    std::vector<float> h;
    std::vector<T> hist;
    size_t pos = 0;
    void set(std::vector<float> taps) { h = std::move(taps); hist.assign(h.size() * 2, T()); pos = 0; }
    T step(T x) {
        const size_t n = h.size();
        hist[pos] = x; hist[pos + n] = x;
        pos = (pos + 1) % n;
        T y = T();
        const T* p = &hist[pos];
        for (size_t k = 0; k < n; k++) y += p[k] * h[k];
        return y;
    }
};

inline void passOn(const Stream* in, Stream& o, PT type, double rate) {
    o.clear();
    o.type = type;
    o.rate = rate;
    o.newBurst = in && in->newBurst;
}

// =====================================================================
//  Blocks
// =====================================================================

// The signal of a receiver: the baseband of its orange Analyzer channel (the Analyzer is
// switched on by itself on the receiver chosen here). "none": generators only.
class InputBlock : public Block {
public:
    std::string info;   // set by the workspace: frequency, width, rate (or why there is nothing)
    explicit InputBlock(bool first = true) {
        type = first ? "input" : "rxin"; title = "Channel input"; category = "Source"; removable = !first; isInput = true;
        setOuts({{"I/Q", PT::IQ}});
        Param& p = addChoice("src", "Receiver", {"none (sources only)"});
        p.dynamic = true;
        p.fixed = {"none (sources only)"};
        p.keyFirstWord = true;
        p.source = []() { return hooks().receivers ? hooks().receivers() : std::vector<std::string>(); };
        refreshChoices();
    }
    // the receiver's number (RX1 -> 1), -1: none
    int rxId() const {
        const std::string s = param("src") ? param("src")->show() : std::string();
        if (s.size() > 2 && s[0] == 'R' && s[1] == 'X') return std::atoi(s.c_str() + 2);
        return -1;
    }
    void process(const std::vector<const Stream*>&) override {}
    std::string status() const override { return info; }
};

class ShiftBlock : public Block {
    std::complex<double> ph {1, 0}, st {1, 0};
public:
    ShiftBlock() { type = "shift"; title = "Frequency shift"; category = "I/Q"; ins = {{"in", PT::IQ}}; setOuts({{"out", PT::IQ}});
                   addNum("hz", "Shift", 0, -500000, 500000, 100, "Hz"); }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        passOn(in[0], o, PT::IQ, in[0] ? in[0]->rate : 0);
        if (!in[0] || in[0]->rate <= 0) return;
        if (changed(in[0]->rate)) st = std::polar(1.0, -2.0 * PI_D * num("hz") / in[0]->rate);
        o.iq.resize(in[0]->iq.size());
        for (size_t i = 0; i < o.iq.size(); i++) {
            const auto v = std::complex<double>(in[0]->iq[i]) * ph;
            o.iq[i] = CF((float)v.real(), (float)v.imag());
            ph *= st;
        }
        ph /= std::abs(ph);
    }
};

class LowpassIQ : public Block {
    Fir<CF> f;
public:
    LowpassIQ() { type = "lowpass"; title = "Low-pass (I/Q)"; category = "I/Q"; ins = {{"in", PT::IQ}}; setOuts({{"out", PT::IQ}});
                  addNum("cut", "Cutoff (+-)", 5000, 50, 500000, 250, "Hz"); }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        passOn(in[0], o, PT::IQ, in[0] ? in[0]->rate : 0);
        if (!in[0] || in[0]->rate <= 0) return;
        if (changed(in[0]->rate)) f.set(designLowpass(in[0]->rate, num("cut")));
        o.iq.reserve(in[0]->iq.size());
        for (CF v : in[0]->iq) o.iq.push_back(f.step(v));
    }
};

// Averages n samples into one (put a low-pass in front for a clean result)
class DecimateBlock : public Block {
    CF acc {0, 0};
    int n = 0;
public:
    DecimateBlock() { type = "decim"; title = "Decimate"; category = "I/Q"; ins = {{"in", PT::IQ}}; setOuts({{"out", PT::IQ}});
                      addNum("n", "Factor", 4, 1, 256, 1, "x"); }
    void process(const std::vector<const Stream*>& in) override {
        const int f = std::max(1, (int)num("n"));
        if (!in[0]) { out[0].clear(); return; }
        passOn(in[0], out[0], PT::IQ, in[0]->rate / f);
        if (changed(in[0]->rate)) { acc = CF(0, 0); n = 0; }
        for (CF v : in[0]->iq) { acc += v; if (++n >= f) { out[0].iq.push_back(acc / (float)f); acc = CF(0, 0); n = 0; } }
    }
};

// Passes only the transmissions (like an oscilloscope's trigger): Auto = every burst,
// Single = the next burst, then closed until "Arm"
class GateBlock : public Block {
    double fs = 0.0;
    size_t blk = 1, cnt = 0;
    double acc = 0.0, segAcc = 0.0;
    int segN = 0;
    std::deque<double> segs;
    double floorDb = -200.0, lastDb = -200.0;
    bool haveFloor = false, open = false, armed = true;
    int hang = 0, bursts = 0;
    std::vector<CF> cur;
    std::deque<std::vector<CF>> pre;
public:
    GateBlock() { type = "gate"; title = "Burst gate (trigger)"; category = "I/Q"; ins = {{"in", PT::IQ}}; setOuts({{"out", PT::IQ}});
                  addNum("thr", "Trigger above floor", 8, 2, 50, 1, "dB");
                  addChoice("mode", "Mode", {"Auto (every burst)", "Single (one burst)"});
                  addNum("hang", "Hold after", 20, 1, 1000, 5, "ms");
                  addNum("pre", "Before trigger", 2, 0, 200, 1, "ms"); }
    void reset() override { segs.clear(); haveFloor = false; open = false; pre.clear(); cur.clear(); cnt = 0; acc = 0; segAcc = 0; segN = 0; }
    std::vector<std::string> buttons() const override { return {"Arm", "Reset floor"}; }
    void onButton(int i) override { if (i == 0) armed = true; else reset(); }
    std::string status() const override {
        char b[128];
        std::snprintf(b, sizeof(b), "%s  level %.0f / floor %.0f dB  bursts %d%s", open ? "OPEN" : "closed", lastDb, floorDb, bursts,
                      choice("mode") == 1 ? (armed ? "  ARMED" : "  holding (Arm)") : "");
        return b;
    }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        o.clear(); o.type = PT::IQ;
        if (!in[0] || in[0]->rate <= 0) return;
        o.rate = in[0]->rate;
        if (changed(in[0]->rate)) { fs = in[0]->rate; blk = std::max<size_t>(1, (size_t)(fs * 0.001)); reset(); }
        const int hangBlocks = std::max(1, (int)num("hang"));
        const size_t preBlocks = (size_t)std::max(0.0, num("pre"));
        for (CF v : in[0]->iq) {
            cur.push_back(v);
            acc += std::norm(v);
            if (++cnt < blk) continue;
            const double p = acc / cnt;
            acc = 0; cnt = 0;
            lastDb = 10.0 * std::log10(p + 1e-20);
            segAcc += p;
            if (++segN == 50) {
                segs.push_back(10.0 * std::log10(segAcc / 50 + 1e-20));
                if (segs.size() > 100) segs.pop_front();
                floorDb = *std::min_element(segs.begin(), segs.end());
                haveFloor = true; segAcc = 0; segN = 0;
            }
            const bool singleMode = choice("mode") == 1;
            if (!open) {
                if (haveFloor && lastDb > floorDb + num("thr") && (!singleMode || armed)) {
                    open = true; hang = 0; bursts++;
                    if (singleMode) armed = false;
                    if (o.iq.empty()) o.newBurst = true;
                    for (auto& b : pre) o.iq.insert(o.iq.end(), b.begin(), b.end());
                    pre.clear();
                    o.iq.insert(o.iq.end(), cur.begin(), cur.end());
                } else {
                    pre.push_back(cur);
                    while (pre.size() > preBlocks) pre.pop_front();
                }
            } else {
                o.iq.insert(o.iq.end(), cur.begin(), cur.end());
                if (lastDb > floorDb + std::max(1.0, num("thr") - 3.0)) hang = 0;
                else if (++hang >= hangBlocks) open = false;
            }
            cur.clear();
        }
    }
};

class FmDemod : public Block {
    CF prev {1, 0};
public:
    FmDemod() { type = "fm"; title = "FM demodulator"; category = "Demodulate"; ins = {{"in", PT::IQ}}; setOuts({{"Hz", PT::Real}}); }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        passOn(in[0], o, PT::Real, in[0] ? in[0]->rate : 0);
        if (!in[0] || in[0]->rate <= 0) return;
        const float k = (float)(in[0]->rate / (2.0 * PI_D));
        for (CF v : in[0]->iq) { o.re.push_back(std::arg(v * std::conj(prev)) * k); prev = v; }
    }
};

class AmDemod : public Block {
public:
    AmDemod() { type = "am"; title = "AM demodulator"; category = "Demodulate"; ins = {{"in", PT::IQ}}; setOuts({{"envelope", PT::Real}});
                addChoice("out", "Output", {"Envelope", "Power (dB)"}); }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        passOn(in[0], o, PT::Real, in[0] ? in[0]->rate : 0);
        if (!in[0]) return;
        const bool db = choice("out") == 1;
        for (CF v : in[0]->iq) o.re.push_back(db ? 10.0f * std::log10(std::norm(v) + 1e-20f) : std::abs(v));
    }
};

class SsbDemod : public Block {
public:
    SsbDemod() { type = "ssb"; title = "SSB / real part"; category = "Demodulate"; ins = {{"in", PT::IQ}}; setOuts({{"audio", PT::Real}});
                 addChoice("sb", "Sideband", {"USB (real part)", "LSB (conjugate)"}); }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        passOn(in[0], o, PT::Real, in[0] ? in[0]->rate : 0);
        if (!in[0]) return;
        // USB: the signal above 0 Hz; a Hilbert pair would be cleaner, real part is enough when the
        // channel was shifted so the sideband sits just above 0 Hz
        const bool lsb = choice("sb") == 1;
        for (CF v : in[0]->iq) o.re.push_back(lsb ? v.imag() : v.real());
    }
};

class LowpassReal : public Block {
    Fir<float> f;
public:
    LowpassReal() { type = "rlowpass"; title = "Low-pass (real)"; category = "Real"; ins = {{"in", PT::Real}}; setOuts({{"out", PT::Real}});
                    addNum("cut", "Cutoff", 3000, 10, 200000, 100, "Hz"); }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        passOn(in[0], o, PT::Real, in[0] ? in[0]->rate : 0);
        if (!in[0] || in[0]->rate <= 0) return;
        if (changed(in[0]->rate)) f.set(designLowpass(in[0]->rate, num("cut")));
        for (float v : in[0]->re) o.re.push_back(f.step(v));
    }
};

class DcBlock : public Block {
    double mean = 0.0;
public:
    DcBlock() { type = "dc"; title = "DC removal"; category = "Real"; ins = {{"in", PT::Real}}; setOuts({{"out", PT::Real}});
                addNum("tc", "Time constant", 50, 1, 5000, 5, "ms"); }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        passOn(in[0], o, PT::Real, in[0] ? in[0]->rate : 0);
        if (!in[0] || in[0]->rate <= 0) return;
        const double a = 1.0 / std::max(1.0, num("tc") * 0.001 * in[0]->rate);
        if (in[0]->newBurst && !in[0]->re.empty()) mean = in[0]->re[0];
        for (float v : in[0]->re) { mean += a * (v - mean); o.re.push_back((float)(v - mean)); }
    }
};

class GainBlock : public Block {
public:
    GainBlock() { type = "gain"; title = "Gain"; category = "Real"; ins = {{"in", PT::Real}}; setOuts({{"out", PT::Real}});
                  addNum("db", "Gain", 0, -80, 80, 1, "dB"); }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        passOn(in[0], o, PT::Real, in[0] ? in[0]->rate : 0);
        if (!in[0]) return;
        const float g = (float)std::pow(10.0, num("db") / 20.0);
        for (float v : in[0]->re) o.re.push_back(v * g);
    }
};

// Clock recovery (Gardner) + slicer: real -> symbols 0..L-1. The levels come from the 15th / 85th
// percentile of the symbol values around each symbol (128 of them, the decision is made 64
// symbols late): robust against the noise before a burst, and a preamble of only the outer
// symbols gives the same thresholds as data with all levels.
class SlicerBlock : public Block {
    static constexpr size_t WIN = 128, DELAY = 64;
    double fs = 0.0, sps = 1.0, t = 0.0;
    float integ = 0.0f;
    std::deque<float> win;
    std::deque<float> vals;          // symbol values, the newest last
    std::vector<float> tmp;
    double center = 0.0, amp = 1.0;
    float prevSym = 0.0f;
    int haveSym = 0;
    double lastMid = 0.0;
    size_t symbolsOut = 0;
    void levels() {
        // 2 levels: 5th / 95th percentile (a preamble may hold one level only 1/8 of the time, AX.25
        // flags); 4 levels: 15th / 85th (each level ~25 %, more robust against noise spikes)
        const size_t pc = choice("lv") == 1 ? 15 : 5;
        tmp.assign(vals.begin(), vals.end());
        const size_t a = tmp.size() * pc / 100, b = std::min(tmp.size() - 1, tmp.size() * (100 - pc) / 100);


        std::nth_element(tmp.begin(), tmp.begin() + a, tmp.end());
        const float lo = tmp[a];
        std::nth_element(tmp.begin(), tmp.begin() + b, tmp.end());
        const float hi = tmp[b];
        center = (hi + lo) / 2.0;
        amp = std::max(1e-9, (hi - lo) / 2.0);
    }
    int decide(float y, int L) const {
        if (L == 2) return y > center ? 1 : 0;
        const double u = (y - center) / amp;   // levels -1, -1/3, +1/3, +1
        return u < -2.0 / 3.0 ? 0 : u < 0.0 ? 1 : u < 2.0 / 3.0 ? 2 : 3;
    }
    // the symbols still held back (end of a burst)
    void flush(Stream& o, int L) {
        if (vals.empty()) return;
        levels();
        const size_t from = vals.size() > DELAY ? vals.size() - DELAY : 0;
        for (size_t i = from; i < vals.size(); i++) { o.sym.push_back((uint8_t)decide(vals[i], L)); symbolsOut++; }
        vals.clear();
    }
public:
    SlicerBlock() { type = "slicer"; title = "Clock recovery + slicer"; category = "Symbols"; ins = {{"in", PT::Real}}; setOuts({{"symbols", PT::Sym}});
                    addNum("baud", "Symbol rate", 4800, 10, 500000, 100, "Bd");
                    addChoice("lv", "Levels", {"2 (binary)", "4"});
                    addChoice("clk", "Timing", {"Gardner (tracks)", "Free-running"}); }
    void reset() override { t = 0.0; win.clear(); integ = 0.0f; haveSym = 0; center = 0.0; amp = 1.0; vals.clear(); acq.clear(); acquiring = true; }
    std::string status() const override {
        char b[96];
        std::snprintf(b, sizeof(b), "centre %.0f  amplitude %.0f  %zu symbols", center, amp, symbolsOut);
        return b;
    }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        passOn(in[0], o, PT::Sym, 0.0);
        const int L = choice("lv") == 1 ? 4 : 2;
        o.levels = L;
        if (!in[0] || in[0]->rate <= 0) return;
        if (changed(in[0]->rate)) { fs = in[0]->rate; reset(); }
        sps = fs / std::max(1.0, num("baud"));
        o.rate = num("baud");
        if (sps < 1.5) return;
        // a new burst, or the gate closed (no samples): the held-back symbols come out now
        if (in[0]->newBurst || in[0]->re.empty()) flush(o, L);
        if (in[0]->newBurst) {
            win.clear(); integ = 0.0f; haveSym = 0; acq.clear(); acquiring = true;
        }
        const int Li = std::max(1, (int)std::lround(sps * 0.7));
        for (float x : in[0]->re) {
            // integrate over ~0.7 symbol
            win.push_back(x);
            integ += x;
            if ((int)win.size() > Li) { integ -= win.front(); win.pop_front(); }
            const float y = integ / win.size();
            if (acquiring) {
                // the first 24 symbols: the crossings of the mean are the symbol edges (their
                // circular mean modulo one symbol); the eye's centre is half a symbol later.
                // (The point of most energy fails on preambles like +3 +3 -3 -3.)
                // The crossings of the centre are the symbol edges: their circular mean modulo
                // one symbol, the eye's centre half a symbol later. Crossings by noise spikes
                // (the FM discriminator on noise jumps far past the signal's levels) are left out;
                // the others count by their steepness, so the transmitter's slow key-up hardly does.
                // (The phase of most energy, or of the best fit to the levels, both fail on
                // preambles like +3 +3 -3 -3.)
                acq.push_back(y);
                if (acq.size() < (size_t)(48 * sps)) continue;
                std::vector<float> srt(acq.begin(), acq.end());
                std::sort(srt.begin(), srt.end());
                const double lo = srt[srt.size() / 10], hi = srt[srt.size() * 9 / 10];
                const double mid = (lo + hi) / 2.0, span = std::max(1e-9, hi - lo);
                std::complex<double> acc(0.0, 0.0);
                for (size_t i = 1; i < acq.size(); i++) {
                    const double a0 = acq[i - 1] - mid, a1 = acq[i] - mid;
                    if ((a0 < 0) == (a1 < 0)) continue;
                    if (std::fabs(a0) > span || std::fabs(a1) > span || std::fabs(a0 - a1) > span) continue;
                    const double tc = (i - 1) + a0 / (a0 - a1);
                    acc += std::polar(std::fabs(a0 - a1), 2.0 * PI_D * tc / sps);
                }
                double bestPh = std::arg(acc) / (2.0 * PI_D) * sps + sps * 0.5;
                bestPh = std::fmod(bestPh + 2.0 * sps, sps);
                acquiring = false;
                t = sps - 1.0 - bestPh;   // the first strobe falls on sample bestPh
                std::vector<float> replay;
                replay.swap(acq);
                for (float v : replay) strobe(v, o, L);
                continue;
            }
            strobe(y, o, L);
        }
    }

private:
    std::vector<float> acq;
    float prevSample = 0.0f;
    bool acquiring = true;
    // one sample of the integrated signal: advance the clock, decide a symbol on the strobe
    void strobe(float y, Stream& o, int L) {
        const bool track = choice("clk") == 0;
        {
            // the strobe and the mid point fall between two samples: interpolate to the exact
            // moment (taking the next sample is 0.5 sample late on average - a bias for Gardner)
            const float yPrev = prevSample;
            prevSample = y;
            const double tPrev = t;
            t += 1.0;
            // Clock tracking on the crossings of the centre (robust when transitions are rare,
            // e.g. AX.25 flags; Gardner drifted there): a crossing belongs half-way between two
            // strobes. Later than that -> the strobes come early -> hold them back.
            if (track && haveSym > 8 && (yPrev - center) * (y - center) < 0.0 && std::fabs(y - yPrev) > amp * 0.3) {
                const double tc = tPrev + (center - yPrev) / (y - yPrev);
                double e = tc - sps * 0.5;
                if (e > sps * 0.5) e -= sps;
                t -= std::clamp(0.12 * e, -0.2 * sps, 0.2 * sps);
            }
            if (tPrev < sps * 0.5 && t >= sps * 0.5) lastMid = y - (t - sps * 0.5) * (y - yPrev);
            if (t < sps) return;
            t -= sps;
            y = (float)(y - t * (y - yPrev));
            vals.push_back(y);
            if (vals.size() > WIN) vals.pop_front();
            levels();
            // decide the symbol DELAY back, with the levels from both sides of it
            if (vals.size() > DELAY) {
                o.sym.push_back((uint8_t)decide(vals[vals.size() - 1 - DELAY], L));
                symbolsOut++;
            }
            const double a = amp;
            // Gardner: e = mid x (this - previous). The integrator makes y lag the signal, so
            // e > 0 means the strobe comes early: hold the next one back (t smaller).
            if (false && track && haveSym) {   // (Gardner, kept for reference: see the crossings above)
                const double e = (lastMid - center) * (y - prevSym) / (a * a + 1e-12);
                t -= std::clamp(0.03 * e, -0.15, 0.15) * sps;
            }
            prevSym = y;
            haveSym++;
        }
    }
};

class MapBlock : public Block {
public:
    MapBlock() { type = "map"; title = "Symbols to bits"; category = "Symbols"; ins = {{"symbols", PT::Sym}}; setOuts({{"bits", PT::Sym}});
                 addChoice("m", "Mapping", {"2-level: high = 1", "2-level: high = 0", "4-level dibits (+3=01 +1=00 -1=10 -3=11)", "4-level natural (0..3 = 00..11)"}); }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        passOn(in[0], o, PT::Sym, 0.0);
        o.levels = 2;
        if (!in[0]) return;
        const int m = choice("m");
        o.rate = in[0]->rate * (m >= 2 ? 2 : 1);
        for (uint8_t s : in[0]->sym) {
            if (s == FRAME) { o.sym.push_back(FRAME); continue; }
            switch (m) {
            case 0: o.sym.push_back(s >= (in[0]->levels + 1) / 2 ? 1 : 0); break;
            case 1: o.sym.push_back(s >= (in[0]->levels + 1) / 2 ? 0 : 1); break;
            case 2: { static const uint8_t d[4][2] = {{1, 1}, {1, 0}, {0, 0}, {0, 1}}; const int k = std::min<int>(s, 3);
                      o.sym.push_back(d[k][0]); o.sym.push_back(d[k][1]); break; }
            default: { const int k = std::min<int>(s, 3); o.sym.push_back((k >> 1) & 1); o.sym.push_back(k & 1); break; }
            }
        }
    }
};

class NrziBlock : public Block {
    uint8_t last = 0;
public:
    NrziBlock() { type = "nrzi"; title = "NRZI decode"; category = "Bits"; ins = {{"bits", PT::Sym}}; setOuts({{"bits", PT::Sym}});
                  addChoice("m", "Rule", {"no change = 1 (AX.25)", "change = 1"}); }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        passOn(in[0], o, PT::Sym, in[0] ? in[0]->rate : 0);
        if (!in[0]) return;
        const bool changeIsOne = choice("m") == 1;
        for (uint8_t b : in[0]->sym) {
            if (b == FRAME) { o.sym.push_back(FRAME); continue; }
            const bool same = b == last;
            last = b;
            o.sym.push_back((uint8_t)(same != changeIsOne ? 1 : 0));
        }
    }
};

class ManchesterBlock : public Block {
    int have = 0;
    uint8_t first = 0;
    int errors = 0, good = 0;
    size_t decoded = 0;
public:
    ManchesterBlock() { type = "manch"; title = "Manchester decode"; category = "Bits"; ins = {{"bits", PT::Sym}}; setOuts({{"bits", PT::Sym}});
                        addChoice("m", "Convention", {"10 = 1 (G.E. Thomas)", "01 = 1 (IEEE 802.3)"}); }
    std::string status() const override { char b[64]; std::snprintf(b, sizeof(b), "%zu bits, recent errors %d", decoded, errors); return b; }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        passOn(in[0], o, PT::Sym, in[0] ? in[0]->rate / 2 : 0);
        if (!in[0]) return;
        if (in[0]->newBurst) { have = 0; errors = 0; }
        const bool ieee = choice("m") == 1;
        for (uint8_t b : in[0]->sym) {
            if (b == FRAME) { o.sym.push_back(FRAME); have = 0; continue; }
            if (!have) { first = b; have = 1; continue; }
            have = 0;
            if (first == b) {
                // 00 / 11 is not Manchester: probably off by one half-bit, slip
                if (++errors > 3) { first = b; have = 1; errors = 0; }
                continue;
            }
            if (good++ > 32) { errors = std::max(0, errors - 1); good = 0; }
            o.sym.push_back((uint8_t)((first == 1) != ieee ? 1 : 0));
            decoded++;
        }
    }
};

// Looks for a bit pattern; after a hit outputs a frame: FRAME marker, the pattern, `len` bits
inline std::vector<uint8_t> parsePattern(const std::string& s) {
    std::vector<uint8_t> b;
    std::string t;
    for (char c : s) if (c != ' ' && c != '_') t += c;
    if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) {
        for (size_t i = 2; i < t.size(); i++) {
            const char c = (char)std::tolower((unsigned char)t[i]);
            int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (v < 0) return {};
            for (int k = 3; k >= 0; k--) b.push_back((v >> k) & 1);
        }
    } else {
        for (char c : t) { if (c == '0' || c == '1') b.push_back((uint8_t)(c - '0')); else return {}; }
    }
    return b;
}

class SyncBlock : public Block {
    std::vector<uint8_t> pat, histBits;
    int remaining = 0;
    bool inverted = false;
    int frames = 0;
public:
    SyncBlock() { type = "sync"; title = "Frame sync"; category = "Bits"; ins = {{"bits", PT::Sym}}; setOuts({{"frames", PT::Sym}});
                  addText("pat", "Sync word (0x.. or bits)", "0x7CD215D8");
                  addNum("err", "Max. wrong bits", 2, 0, 16, 1, "");
                  addNum("len", "Frame length after it", 512, 0, 10000000, 8, "bits");
                  addChoice("inv", "Inverted word too", {"no", "yes (and invert the frame)"}); }
    std::string status() const override {
        char b[96];
        std::snprintf(b, sizeof(b), pat.empty() ? "invalid pattern" : "%d bits word, %d frames", (int)pat.size(), frames);
        return b;
    }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        passOn(in[0], o, PT::Sym, in[0] ? in[0]->rate : 0);
        o.newBurst = false;
        o.levels = 2;
        if (changed(1.0)) { pat = parsePattern(text("pat")); histBits.clear(); remaining = 0; }
        if (!in[0] || pat.empty()) return;
        const int maxErr = (int)num("err");
        const bool tryInv = choice("inv") == 1;
        for (uint8_t b : in[0]->sym) {
            if (b == FRAME) continue;
            if (remaining > 0) { o.sym.push_back(inverted ? (uint8_t)(b ^ 1) : b); remaining--; }
            histBits.push_back(b);
            if (histBits.size() > pat.size()) histBits.erase(histBits.begin());
            if (histBits.size() < pat.size() || remaining > 0) continue;
            int e = 0, ei = 0;
            for (size_t k = 0; k < pat.size(); k++) { e += histBits[k] != pat[k]; ei += histBits[k] == pat[k]; }
            if (e <= maxErr || (tryInv && ei <= maxErr)) {
                inverted = e > maxErr;
                o.sym.push_back(FRAME);
                for (uint8_t p : pat) o.sym.push_back(p);
                remaining = (int)num("len");
                frames++;
                if (o.sym.size() == pat.size() + 1) o.newBurst = true;
            }
        }
    }
};

// ---------------------------------------------------------------------
//  Views
// ---------------------------------------------------------------------

// Time plot with a trigger: Auto (free running), Normal (only on trigger), Single (once, then hold)
// An oscilloscope with as many channels as wires: every wire connected to it opens another
// input (up to 8). Each channel is real or I/Q (I and Q drawn as two traces), at its own rate;
// one of them triggers, and all show the same moment and the same time span.
class ScopeBlock : public Block {
    struct Ch {
        std::deque<float> a, b;            // real, or I and Q
        std::vector<float> showA, showB;   // the shown capture
        bool iq = false, live = false;
        double rate = 0.0;
        size_t need = 0;
    };
    std::vector<Ch> ch;
    long long sinceTrig = 0, sinceShow = 0;
    int capturing = -1;              // samples still to collect after the trigger (-1: not)
    float prevV = 0.0f;
    double rate = 0.0;               // of the triggering channel
    int trigCh = 0;
    bool singleDone = false;
    // Slow sweeps (a span of half a second or more, the control labs use 20 s) are drawn while
    // they are captured, left to right, like a real scope's slow time base: the capture would
    // otherwise appear only when it is complete, many seconds after the trigger. Before the
    // first trigger the input rolls in. showFrac: how much of the width the shown samples fill
    // (1 = a whole capture).
    float showFrac = 1.0f;
    bool everTriggered = false;

    // Shows the last `need` samples of every channel (frac < 1: that share of them), ending at
    // sample i (of nT) of the triggering channel's block; the other channels end at the same
    // moment of theirs
    void snapshot(const std::vector<const Stream*>& in, size_t i, size_t nT, double frac = 1.0) {
        frac = std::clamp(frac, 0.0, 1.0);
        showFrac = (float)frac;
        for (size_t k = 0; k < ch.size(); k++) {
            Ch& c = ch[k];
            c.showA.clear(); c.showB.clear();
            if (!c.live) continue;
            size_t drop = 0;
            if ((int)k != trigCh && k < in.size() && in[k] && nT) {
                const size_t nk = in[k]->size();
                const size_t upto = (size_t)std::llround((double)(i + 1) / nT * nk);
                drop = nk - std::min(nk, upto);
            }
            const size_t end = c.a.size() - std::min(drop, c.a.size());
            const size_t n = std::min((size_t)std::llround(c.need * frac), end);
            c.showA.assign(c.a.begin() + (end - n), c.a.begin() + end);
            if (c.iq && c.b.size() == c.a.size()) c.showB.assign(c.b.begin() + (end - n), c.b.begin() + end);
        }
        sinceShow = 0;
    }
    int liveCount() const { int n = 0; for (const auto& c : ch) n += c.live; return n; }
    static sf::Color chColor(int k) { return channelColor(k); }
public:
    ScopeBlock() { type = "scope"; title = "Scope"; category = "View"; ins = {{"in", PT::Any}}; maxIns = 8;
                   addNum("ms", "Time span", 50, 0.1, 120000, 5, "ms", 1);
                   addChoice("mode", "Trigger", {"Auto", "Normal", "Single"});
                   addChoice("src", "Trigger on", {"Level (rising)", "Start of a burst"});
                   addNum("lvl", "Level", 0, -1e6, 1e6, 100, "", 1);
                   addNum("tch", "Trigger channel", 1, 1, 8, 1, "");
                   addChoice("scale", "Vertical", {"Common scale", "Stacked (each own scale)"});
                   showOnly("lvl", "src", {0}); }
    sf::Vector2f viewSize() const override {
        const int n = liveCount();
        return {380.0f, 170.0f + (choice("scale") == 1 && n > 1 ? 55.0f * (n - 1) : 0.0f)};
    }
    std::vector<std::string> buttons() const override { return {"Arm (Single)"}; }
    void onButton(int) override { singleDone = false; capturing = -1; }
    bool slowSweep() const { return num("ms") >= 500.0; }
    const std::vector<float>& trace(size_t k) const { static const std::vector<float> none; return k < ch.size() ? ch[k].showA : none; }
    void reset() override { ch.clear(); capturing = -1; sinceTrig = sinceShow = 0; everTriggered = false; showFrac = 1.0f; }
    std::string status() const override {
        const int m = choice("mode");
        std::string s = m == 2 ? (singleDone ? "Single: captured (Arm for the next)" : "Single: armed, waiting")
                               : m == 1 ? "Normal: shows the last trigger" : "Auto";
        if (ch.size() > 1) s += "   trigger: ch " + std::to_string(trigCh + 1);
        return s;
    }
    void process(const std::vector<const Stream*>& in) override {
        ch.resize(in.size());
        // the triggering channel: the chosen one, else the first with a signal
        trigCh = -1;
        const int want = (int)num("tch") - 1;
        if (want >= 0 && want < (int)in.size() && in[want] && in[want]->rate > 0) trigCh = want;
        for (size_t k = 0; k < in.size() && trigCh < 0; k++) if (in[k] && in[k]->rate > 0) trigCh = (int)k;
        if (trigCh < 0) { for (auto& c : ch) c.live = false; return; }
        const double ms = num("ms") * 0.001;
        // every other channel: its whole block (snapshot() goes back to the trigger's moment)
        for (size_t k = 0; k < in.size(); k++) {
            Ch& c = ch[k];
            if (!in[k] || in[k]->rate <= 0) { c.live = false; c.a.clear(); c.b.clear(); continue; }
            const bool iq = in[k]->type == PT::IQ;
            if (iq != c.iq || !c.live) { c.a.clear(); c.b.clear(); }
            c.iq = iq; c.live = true; c.rate = in[k]->rate;
            c.need = std::max<size_t>(16, (size_t)(ms * c.rate));
            if ((int)k == trigCh) continue;
            const size_t n = in[k]->size();
            for (size_t i = 0; i < n; i++) {
                if (iq) { c.a.push_back(in[k]->iq[i].real()); c.b.push_back(in[k]->iq[i].imag()); }
                else c.a.push_back(in[k]->re[i]);
            }
            while (c.a.size() > c.need * 2 + n) c.a.pop_front();
            while (c.b.size() > c.need * 2 + n) c.b.pop_front();
        }
        // the triggering channel, sample by sample
        const Stream* t = in[trigCh];
        Ch& T = ch[trigCh];
        rate = t->rate;
        const size_t need = T.need, preN = need / 10;
        const int mode = choice("mode");
        const bool burstTrig = choice("src") == 1;
        const float lvl = (float)num("lvl");
        const size_t n = t->size();
        const bool slow = slowSweep();
        const long long liveStep = std::max<long long>(1, (long long)(rate * 0.05));   // a slow sweep is redrawn every 50 ms
        for (size_t i = 0; i < n; i++) {
            float va, vb = 0.0f;
            if (T.iq) { va = t->iq[i].real(); vb = t->iq[i].imag(); } else va = t->re[i];
            T.a.push_back(va); if (T.iq) T.b.push_back(vb);
            while (T.a.size() > need * 2) T.a.pop_front();
            while (T.b.size() > need * 2) T.b.pop_front();
            sinceTrig++; sinceShow++;
            const float lv = T.iq ? std::sqrt(va * va + vb * vb) : va;
            const bool trig = burstTrig ? (i == 0 && t->newBurst) : (prevV < lvl && lv >= lvl);
            prevV = lv;
            if (capturing >= 0) {
                if (--capturing <= 0) { snapshot(in, i, n); capturing = -1; if (mode == 2) singleDone = true; }
                else if (slow && sinceShow >= liveStep) snapshot(in, i, n, (double)(need - (size_t)capturing) / need);
                continue;
            }
            if (trig && !(mode == 2 && singleDone)) { capturing = (int)(need - preN); sinceTrig = 0; everTriggered = true; continue; }
            if (mode == 0 && sinceShow > (long long)need * 2 && sinceTrig > (long long)need * 2) snapshot(in, i, n);
            // nothing captured yet: the input rolls in (the last `need` samples, growing from the left)
            else if (slow && !everTriggered && sinceShow >= liveStep) snapshot(in, i, n, (double)std::min(T.a.size(), need) / need);
        }
    }
    void drawView(const Draw& d, const sf::FloatRect& r) override {
        d.rect(r, sf::Color(8, 10, 12), sf::Color(50, 50, 58));
        std::vector<int> shown;
        for (size_t k = 0; k < ch.size(); k++) if (ch[k].showA.size() >= 2) shown.push_back((int)k);
        if (shown.empty()) { d.text(rate > 0 ? "waiting for the trigger" : "no input", r.position.x + 6, r.position.y + 4, 11, sf::Color(110, 110, 120)); return; }
        const bool single = liveCount() <= 1;
        const bool stacked = choice("scale") == 1 && shown.size() > 1;
        auto range = [&](const std::vector<int>& ks, float& lo, float& hi) {
            lo = 1e30f; hi = -1e30f;
            for (int k : ks) {
                for (float v : ch[k].showA) { lo = std::min(lo, v); hi = std::max(hi, v); }
                for (float v : ch[k].showB) { lo = std::min(lo, v); hi = std::max(hi, v); }
            }
            if (hi - lo < 1e-9f) { hi += 1; lo -= 1; }
            const float pad = (hi - lo) * 0.08f; lo -= pad; hi += pad;
        };
        auto plot = [&](const std::vector<float>& v, sf::Color c, const sf::FloatRect& lane, float lo, float hi) {
            sf::VertexArray va(sf::PrimitiveType::LineStrip);
            const size_t n = v.size();
            const size_t step = std::max<size_t>(1, n / 1500);
            const float w = lane.size.x * showFrac;
            for (size_t i = 0; i < n; i += step)
                va.append({{lane.position.x + w * i / (n - 1), lane.position.y + lane.size.y * (1.0f - (v[i] - lo) / (hi - lo))}, c});
            d.win->draw(va);
        };
        auto colors = [&](int k, sf::Color& ci, sf::Color& cq) {
            if (single) { ci = sf::Color(120, 230, 120); cq = sf::Color(90, 200, 255); return; }   // one channel: I green, Q blue
            ci = chColor(k);
            cq = sf::Color((uint8_t)(ci.r * 0.55f), (uint8_t)(ci.g * 0.55f), (uint8_t)(ci.b * 0.55f));
        };
        d.rect(sf::FloatRect({r.position.x + r.size.x * 0.1f, r.position.y}, {1, r.size.y}), sf::Color(255, 255, 255, 40));
        char b[64];
        const int lanes = stacked ? (int)shown.size() : 1;
        for (int li = 0; li < lanes; li++) {
            const sf::FloatRect lane({r.position.x, r.position.y + r.size.y * li / lanes}, {r.size.x, r.size.y / lanes});
            const std::vector<int> ks = stacked ? std::vector<int>{shown[li]} : shown;
            float lo, hi;
            range(ks, lo, hi);
            if (stacked && li > 0) d.rect(sf::FloatRect({lane.position.x, lane.position.y}, {lane.size.x, 1}), sf::Color(50, 50, 58));
            // the trigger level, in the triggering channel's lane
            if (!burstTrigLine() && std::find(ks.begin(), ks.end(), trigCh) != ks.end()) {
                const float ly = lane.position.y + lane.size.y * (1.0f - ((float)num("lvl") - lo) / (hi - lo));
                if (ly > lane.position.y && ly < lane.position.y + lane.size.y) d.rect(sf::FloatRect({lane.position.x, ly}, {lane.size.x, 1}), sf::Color(255, 140, 0, 90));
            }
            for (int k : ks) {
                sf::Color ci, cq;
                colors(k, ci, cq);
                if (!ch[k].showB.empty()) plot(ch[k].showB, cq, lane, lo, hi);
                plot(ch[k].showA, ci, lane, lo, hi);
            }
            std::snprintf(b, sizeof(b), "%.4g .. %.4g", lo, hi);
            const int k0 = stacked ? shown[li] : 0;
            const std::string lab = stacked ? std::to_string(k0 + 1) + (ch[k0].iq ? " I/Q" : " real") + (k0 == trigCh ? " (trigger)" : "") + "   " + b : std::string(b);
            d.rect(sf::FloatRect({lane.position.x + 1, lane.position.y + 1}, {d.width(lab, 10) + 8, 14}), sf::Color(8, 10, 12, 200));
            d.text(lab, lane.position.x + 4, lane.position.y + 2, 10, stacked ? chColor(k0) : sf::Color(140, 140, 150));
        }
        // legend: channel and kind; * marks the triggering one
        if (!single && !stacked) {
            float x = r.position.x + r.size.x - 4;
            for (auto it = shown.rbegin(); it != shown.rend(); ++it) {
                const int k = *it;
                const std::string t = std::to_string(k + 1) + (ch[k].iq ? " I/Q" : " real") + (k == trigCh ? "*" : "");
                x -= d.width(t, 10) + 8;
                d.rect(sf::FloatRect({x - 3, r.position.y + 1}, {d.width(t, 10) + 6, 14}), sf::Color(8, 10, 12, 200));
                d.text(t, x, r.position.y + 2, 10, chColor(k));
            }
        }
        std::snprintf(b, sizeof(b), "%.1f ms", num("ms"));
        d.text(b, r.position.x + r.size.x - d.width(b, 10) - 4, r.position.y + r.size.y - 14, 10, sf::Color(140, 140, 150));
    }
    bool burstTrigLine() const { return choice("src") == 1; }
};

class SpectrumBlock : public Block {
    std::vector<double> avg;
    std::vector<Complex> buf;
    bool isIQ = true;
    double rate = 0.0;
    static constexpr size_t N = 1024;
public:
    SpectrumBlock() { type = "spectrum"; title = "Spectrum"; category = "View"; ins = {{"in", PT::Any}};
                      addNum("avg", "Averaging", 0.3, 0.01, 1.0, 0.05, "", 2); }
    sf::Vector2f viewSize() const override { return {380.0f, 150.0f}; }
    void process(const std::vector<const Stream*>& in) override {
        if (!in[0] || in[0]->rate <= 0) return;
        isIQ = in[0]->type == PT::IQ;
        rate = in[0]->rate;
        const size_t n = in[0]->size();
        for (size_t i = 0; i < n; i++) {
            buf.emplace_back(isIQ ? Complex(in[0]->iq[i].real(), in[0]->iq[i].imag()) : Complex(in[0]->re[i], 0.0));
            if (buf.size() < N) continue;
            for (size_t k = 0; k < N; k++) buf[k] *= 0.5 - 0.5 * std::cos(2.0 * PI_D * k / (N - 1));
            fft(buf);
            if (avg.size() != N) avg.assign(N, 0.0);
            const double al = num("avg");
            for (size_t k = 0; k < N; k++) avg[k] += al * (std::norm(buf[(k + N / 2) % N]) - avg[k]);
            buf.clear();
        }
    }
    void drawView(const Draw& d, const sf::FloatRect& r) override {
        d.rect(r, sf::Color(8, 10, 12), sf::Color(50, 50, 58));
        if (avg.empty()) { d.text("no input", r.position.x + 6, r.position.y + 4, 11, sf::Color(110, 110, 120)); return; }
        const size_t a = isIQ ? 0 : N / 2;
        std::vector<float> db;
        for (size_t k = a; k < N; k++) db.push_back((float)(10.0 * std::log10(avg[k] + 1e-20)));
        const float hi = *std::max_element(db.begin(), db.end()) + 3.0f, lo = hi - 70.0f;
        sf::VertexArray va(sf::PrimitiveType::LineStrip);
        for (size_t k = 0; k < db.size(); k++)
            va.append({{r.position.x + r.size.x * k / (db.size() - 1), r.position.y + r.size.y * (1.0f - std::clamp((db[k] - lo) / (hi - lo), 0.0f, 1.0f))}, sf::Color(255, 160, 50)});
        d.win->draw(va);
        char b[64];
        std::snprintf(b, sizeof(b), isIQ ? "%+.1f kHz" : "0 Hz", isIQ ? -rate / 2000.0 : 0.0);
        d.text(b, r.position.x + 3, r.position.y + r.size.y - 14, 10, sf::Color(140, 140, 150));
        std::snprintf(b, sizeof(b), "%+.1f kHz", rate / 2000.0);
        d.text(b, r.position.x + r.size.x - d.width(b, 10) - 3, r.position.y + r.size.y - 14, 10, sf::Color(140, 140, 150));
        d.text("70 dB range", r.position.x + 3, r.position.y + 2, 10, sf::Color(140, 140, 150));
    }
};

class ConstelBlock : public Block {
    std::deque<CF> pts;
    size_t skip = 0;
public:
    ConstelBlock() { type = "constel"; title = "Constellation / XY"; category = "View"; ins = {{"in", PT::IQ}};
                     addNum("dec", "Show every n-th sample", 1, 1, 1000, 1, ""); }
    sf::Vector2f viewSize() const override { return {200.0f, 200.0f}; }
    void process(const std::vector<const Stream*>& in) override {
        if (!in[0]) return;
        const size_t dn = std::max<size_t>(1, (size_t)num("dec"));
        for (CF v : in[0]->iq) { if (++skip >= dn) { skip = 0; pts.push_back(v); } }
        while (pts.size() > 3000) pts.pop_front();
    }
    void drawView(const Draw& d, const sf::FloatRect& r) override {
        d.rect(r, sf::Color(8, 10, 12), sf::Color(50, 50, 58));
        if (pts.empty()) return;
        double rms = 0.0;
        for (CF v : pts) rms += std::norm(v);
        rms = std::sqrt(rms / pts.size());
        const float cx = r.position.x + r.size.x / 2, cy = r.position.y + r.size.y / 2, sc = r.size.x / 4.5f / (float)std::max(rms, 1e-12);
        d.rect(sf::FloatRect({r.position.x, cy}, {r.size.x, 1}), sf::Color(45, 45, 52));
        d.rect(sf::FloatRect({cx, r.position.y}, {1, r.size.y}), sf::Color(45, 45, 52));
        sf::VertexArray va(sf::PrimitiveType::Points);
        for (CF v : pts) {
            const float x = cx + v.real() * sc, y = cy - v.imag() * sc;
            if (r.contains({x, y})) va.append({{x, y}, sf::Color(255, 170, 60, 160)});
        }
        d.win->draw(va);
    }
};

class BitsView : public Block {
    std::deque<std::vector<uint8_t>> lines;
    std::vector<uint8_t> curLine;
    int levels = 2;
    std::string fmtLine(const std::vector<uint8_t>& l) const {
        const int f = choice("fmt");
        std::string s;
        if (f == 1 && levels == 2) {   // hex
            for (size_t i = 0; i + 8 <= l.size() || (i < l.size() && i % 8 == 0 && l.size() - i >= 4); i += 8) {
                int v = 0, nb = 0;
                for (size_t k = i; k < std::min(l.size(), i + 8); k++, nb++) v = (v << 1) | l[k];
                v <<= (8 - nb);
                char b[4]; std::snprintf(b, sizeof(b), "%02X", v);
                s += b; s += ' ';
            }
        } else if (f == 2 && levels == 2) {   // ASCII (8 bit, MSB first)
            for (size_t i = 0; i + 8 <= l.size(); i += 8) {
                int v = 0;
                for (size_t k = i; k < i + 8; k++) v = (v << 1) | l[k];
                s += (v >= 32 && v < 127) ? (char)v : '.';
            }
        } else {
            for (size_t i = 0; i < l.size(); i++) { s += (char)('0' + l[i]); if (i % 8 == 7) s += ' '; }
        }
        return s;
    }
    void endLine() {
        if (!curLine.empty()) lines.push_back(curLine);
        curLine.clear();
        while (lines.size() > 300) lines.pop_front();
    }
public:
    BitsView() { type = "bits"; title = "Bits / symbols view"; category = "View"; ins = {{"in", PT::Sym}};
                 addChoice("fmt", "Format", {"Bits / symbols", "Hex", "ASCII (8 bit)"});
                 addNum("len", "Line length (no frames)", 64, 8, 4096, 8, "symbols"); }
    sf::Vector2f viewSize() const override { return {460.0f, 210.0f}; }
    std::vector<std::string> buttons() const override { return {"Copy", "Clear"}; }
    std::string allText() const {
        std::string t;
        for (const auto& l : lines) t += fmtLine(l) + "\n";
        if (!curLine.empty()) t += fmtLine(curLine) + "\n";
        return t;
    }
    void onButton(int i) override {
        if (i == 1) { lines.clear(); curLine.clear(); return; }
        sf::Clipboard::setString(allText());
    }
    std::string status() const override { return std::to_string(lines.size()) + " lines"; }
    void process(const std::vector<const Stream*>& in) override {
        if (!in[0]) return;
        levels = in[0]->levels;
        if (in[0]->newBurst) endLine();
        const size_t len = std::max<size_t>(8, (size_t)num("len"));
        for (uint8_t s : in[0]->sym) {
            if (s == FRAME) { endLine(); continue; }
            curLine.push_back(s);
            if (curLine.size() >= len) endLine();
        }
    }
    void drawView(const Draw& d, const sf::FloatRect& r) override {
        d.rect(r, sf::Color(8, 10, 12), sf::Color(50, 50, 58));
        const float lh = 13.0f;
        const int rows = std::max(1, (int)((r.size.y - 4) / lh));
        std::vector<std::string> show;
        if (!curLine.empty()) show.push_back(fmtLine(curLine));
        for (auto it = lines.rbegin(); it != lines.rend() && (int)show.size() < rows; ++it) show.push_back(fmtLine(*it));
        std::reverse(show.begin(), show.end());
        float y = r.position.y + 2;
        for (const auto& s : show) {
            std::string t = s;
            while (!t.empty() && d.width(t, 10) > r.size.x - 8) t.pop_back();
            d.text(t, r.position.x + 4, y, 10, sf::Color(215, 215, 220));
            y += lh;
        }
    }
};

// The speaker: "left / mono" alone plays on both sides; with "right" connected it is stereo
class AudioOut : public Block {
    double pos = 0.0, rate = 0.0;
    float prevL = 0.0f, prevR = 0.0f, peak = 1e-3f;
    std::vector<float> o48;
    int stream = 0, routedTo = -1;   // own stream when a sound card is chosen
public:
    AudioOut() {
        type = "audio"; title = "Audio out (speaker)"; category = "Sinks"; ins = {{"left / mono", PT::Real}, {"right", PT::Real}};
        o48.reserve(8192);
        addNum("vol", "Volume", 0, -60, 30, 1, "dB");
        addChoice("agc", "Level", {"Automatic", "Fixed"});
        // which sound card ([0] the system's default output); its own stream in the speaker mixer
        Param& p = addChoice("dev", "Output", {"System Default Output"});
        p.dynamic = true;
        p.source = []() { return hooks().audioDevices ? hooks().audioDevices() : std::vector<std::string>{"System Default Output"}; };
        refreshChoices();
        syncInputs = true;   // left and right line up
        static int nextStream = 100000;
        stream = nextStream++;
    }
    ~AudioOut() override { if (routedTo >= 0 && hooks().audioClose) hooks().audioClose(stream); }
    std::string status() const override { return rate > 0 ? std::string(stereo ? "stereo" : "mono") + ", from " + std::to_string((int)rate) + " S/s" : ""; }
    bool stereo = false;
    void process(const std::vector<const Stream*>& in) override {
        const int dev = choice("dev");
        const bool own = hooks().audioPush && hooks().audioRoute;   // the app: a stream of its own; tests: the ring
        if (own && dev != routedTo) { hooks().audioRoute(stream, dev); routedTo = dev; }
        if (!in[0] || in[0]->rate <= 0 || (!own && !audio)) return;
        rate = in[0]->rate;
        stereo = in[1] && in[1]->re.size() == in[0]->re.size();
        const double step = rate / 48000.0;
        const bool agc = choice("agc") == 0;
        const float g = (float)std::pow(10.0, num("vol") / 20.0);
        // the automatic level follows the loudest side, falling back over ~3 s
        const float decay = (float)std::exp(-1.0 / (rate * 3.0));
        o48.clear();
        const auto& L = in[0]->re;
        o48.reserve((size_t)(L.size() / step) * 2 + 8);
        for (size_t i = 0; i < L.size(); i++) {
            const float xl = L[i], xr = stereo ? in[1]->re[i] : xl;
            if (agc) peak = std::max({std::fabs(xl), std::fabs(xr), peak * decay});
            const float k = (agc ? 0.3f / std::max(peak, 1e-9f) : 1.0f) * g;
            while (pos <= 1.0) {
                const float vl = prevL + (xl - prevL) * (float)pos, vr = prevR + (xr - prevR) * (float)pos;
                o48.push_back(std::clamp(vl * k, -1.0f, 1.0f));
                o48.push_back(std::clamp(vr * k, -1.0f, 1.0f));
                pos += step;
            }
            pos -= 1.0;
            prevL = xl; prevR = xr;
        }
        if (own) hooks().audioPush(stream, o48);
        else audio->push(o48);
    }
};

class AnalyzerView : public Block {
    an::Analyzer core;
    an::Report shown;
    int seen = 0;
public:
    AnalyzerView() { type = "analyzer"; title = "Analyzer"; category = "View"; ins = {{"in", PT::IQ}};
                     addNum("trig", "Trigger above floor", 8, 2, 50, 1, "dB"); }
    sf::Vector2f viewSize() const override { return {380.0f, 150.0f}; }
    void process(const std::vector<const Stream*>& in) override {
        if (!in[0] || in[0]->rate <= 0) return;
        if (changed(in[0]->rate)) { core.configure(in[0]->rate); core.reset(); }
        core.triggerDb = num("trig");
        core.push(in[0]->iq.data(), in[0]->iq.size());
        if (core.reports != seen) { seen = core.reports; shown = core.latest; }
    }
    void drawView(const Draw& d, const sf::FloatRect& r) override {
        d.rect(r, sf::Color(8, 10, 12), sf::Color(50, 50, 58));
        if (!shown.valid) { d.text("waiting for a transmission", r.position.x + 6, r.position.y + 4, 11, sf::Color(110, 110, 120)); return; }
        float y = r.position.y + 3;
        const float maxW = r.size.x - 10;
        auto fit = [&](std::string s, unsigned size) {
            if (d.width(s, size) <= maxW) return s;
            while (s.size() > 3 && d.width(s + "...", size) > maxW) s.pop_back();
            return s + "...";
        };
        d.text(fit(shown.modulation, 13), r.position.x + 5, y, 13, sf::Color(255, 255, 255)); y += 17;
        for (const auto& g : shown.guesses) { if (y > r.position.y + r.size.y - 13) break; d.text(fit("> " + g, 10), r.position.x + 5, y, 10, sf::Color(255, 190, 110)); y += 12; }
        for (const auto& n : shown.notes) { if (y > r.position.y + r.size.y - 13) break; d.text(fit(n, 10), r.position.x + 5, y, 10, sf::Color(200, 200, 210)); y += 12; }
    }
};

// (inside namespace pl)
#include "PipelineBlocks.h"   // the decoder building blocks (AGC, RRC, Costas, Viterbi, RS, ...)

#include "PipelineLib.h"      // the general library (sources, math, filters, modulators, displays, sinks)

#include "PipelineControl.h"  // convolution of two signals; control: G(s), PID, closed loop, step metrics

#include "PipelineSat.h"      // pictures: analog lines by a sync pattern, image lines from frames

#include "PipelineLora.h"     // LoRa (chirp spread spectrum): receiver and modulator
#include "PipelineScript.h"   // Script (Lua): a protocol's logic written into the pipeline
#include "PipelineCrypto.h"   // Crypto teaching blocks (Key source, Cipher, Brute force)

#include "PipelineTelemetry.h" // packets by a length field, telemetry fields of a frame

// ---------------------------------------------------------------------
//  Registry: the palette lists the blocks in this order, grouped by category
// ---------------------------------------------------------------------
struct BlockInfo { const char* type; const char* title; const char* category; };
inline const std::vector<BlockInfo>& blockList() {
    static const std::vector<BlockInfo> l = {
        // signal sources
        {"rxin", "Channel input (another receiver)", "Sources"},
        {"srciq", "Signal source (I/Q)", "Sources"}, {"srcreal", "Signal source (real)", "Sources"},
        {"noiseiq", "Noise source (I/Q)", "Sources"}, {"noisereal", "Noise source (real)", "Sources"},
        {"bitsrc", "Bit source (random / PRBS)", "Sources"}, {"patsrc", "Pattern source", "Sources"}, {"msgsrc", "Message source", "Sources"},
        // arithmetic and types
        {"add", "Add  (a + b)", "Math"}, {"sub", "Subtract  (a - b)", "Math"}, {"mul", "Multiply  (a x b)", "Math"},
        {"mulconj", "Multiply conjugate  (a x b*)", "Math"}, {"affine", "Multiply / add constant", "Math"}, {"gain", "Gain (dB)", "Math"},
        {"mag", "Magnitude / power / dB", "Math"}, {"c2r", "Complex to real parts", "Math"}, {"r2c", "Real to complex", "Math"},
        {"iqswap", "I/Q swap / invert", "Math"}, {"delay", "Delay  (z^-n, feedback)", "Math"}, {"movavg", "Moving average", "Math"},
        {"intdump", "Integrate and dump", "Math"}, {"diffr", "Differentiate", "Math"}, {"accum", "Accumulate", "Math"}, {"clip", "Clip / limiter", "Math"},
        // filters
        {"fir", "FIR filter (own taps) / convolution", "Filters"}, {"conv", "Convolution / correlation (a * b)", "Filters"}, {"firdesign", "FIR filter design", "Filters"}, {"iir", "IIR filter", "Filters"},
        {"hilbert", "Hilbert (real -> analytic I/Q)", "Filters"}, {"lowpass", "Low-pass (I/Q)", "Filters"}, {"rlowpass", "Low-pass (real)", "Filters"},
        {"rrc", "RRC filter", "Filters"}, {"dc", "DC removal", "Filters"}, {"deemph", "De-emphasis", "Filters"},
        // rates
        {"upsample", "Upsample (x N)", "Resampling"}, {"downsample", "Downsample (keep 1 in N)", "Resampling"},
        {"decim", "Decimate (average N, I/Q)", "Resampling"}, {"resample", "Resample (real, any rate)", "Resampling"},
        // the channel
        {"shift", "Frequency shift", "Channel"}, {"agc", "AGC", "Channel"}, {"gate", "Burst gate (trigger)", "Channel"},
        // demodulators
        {"fm", "FM demodulator", "Demodulate"}, {"am", "AM demodulator", "Demodulate"}, {"ssb", "SSB / real part", "Demodulate"},
        {"phase", "Phase demodulator", "Demodulate"}, {"fsktones", "FSK tone detector", "Demodulate"}, {"matrix", "Stereo matrix", "Demodulate"},
        {"lora", "LoRa receiver", "Demodulate"},
        // synchronisation and decisions
        {"symsync", "Symbol sync (real)", "Sync"}, {"clockiq", "Clock recovery (I/Q)", "Sync"}, {"decide", "Slicer (decision)", "Sync"},
        {"threshold", "Threshold (with hysteresis)", "Sync"}, {"slicer", "Clock recovery + slicer", "Sync"}, {"costas", "Costas loop", "Sync"},
        {"pilot", "Pilot PLL", "Sync"}, {"oqpsk", "OQPSK -> QPSK (half-symbol delay)", "Sync"}, {"softbits", "Soft bits (I, Q)", "Sync"},
        // the transmitter's side
        {"levels", "Bits to levels (NRZ, 4-level ...)", "Modulate"}, {"psk", "Bits to PSK / QAM symbols", "Modulate"},
        {"fmmod", "FM modulator", "Modulate"}, {"ammod", "AM modulator", "Modulate"},
        {"convenc", "Convolutional encoder (K=7, 1/2)", "Modulate"}, {"hdlcframer", "HDLC framer", "Modulate"},
        {"loramod", "LoRa modulator", "Modulate"},
        // bits
        {"map", "Symbols to bits", "Bits"}, {"invbits", "Invert bits", "Bits"}, {"nrzi", "NRZI decode", "Bits"}, {"manch", "Manchester decode", "Bits"},
        {"diffbits", "Differential (bits)", "Bits"}, {"diff", "Differential decode (soft)", "Bits"}, {"lfsr", "Scrambler / descrambler (LFSR)", "Bits"},
        {"g3ruh", "G3RUH descrambler", "Bits"}, {"derand", "Derandomize (CCSDS)", "Bits"}, {"sync", "Frame sync", "Bits"},
        {"codedsync", "Coded sync + QPSK phase", "Bits"}, {"viterbi", "Viterbi decoder (K=7, 1/2)", "Bits"},
        {"pack", "Bits to bytes (frames)", "Bits"}, {"unpack", "Bytes to bits (frames)", "Bits"}, {"hdlc", "HDLC deframer", "Bits"},
        // frames and packets
        {"crc", "CRC check / append", "Frames"}, {"slice", "Bytes of a frame (from, count)", "Frames"}, {"xorbytes", "XOR bytes with a key", "Frames"}, {"rs", "Reed-Solomon (255,223)", "Frames"},
        {"ccsds", "CCSDS packets (from VCDU)", "Frames"}, {"msumr", "Meteor MSU-MR image", "Frames"},
        {"lenpkt", "Packets by a length field", "Frames"}, {"fields", "Telemetry fields (decode a frame)", "Frames"},
        // pictures
        {"raster", "Raster lines", "Image"}, {"linesync", "Line sync (pattern) -> lines", "Image"},
        {"framelines", "Frame -> image line (N-bit samples)", "Image"},
        // control engineering
        {"tf", "Transfer function G(s)", "Control"}, {"pid", "PID controller", "Control"},
        {"loop", "Closed loop (PID + G(s))", "Control"}, {"stepinfo", "Step response (metrics)", "Control"},
        // displays
        {"scope", "Scope", "Displays"}, {"spectrum", "Spectrum", "Displays"}, {"waterfall", "Waterfall", "Displays"},
        {"constel", "Constellation / XY", "Displays"}, {"histogram", "Histogram", "Displays"}, {"eye", "Eye diagram", "Displays"},
        {"probe", "Number display (probe)", "Displays"}, {"text", "Text display", "Displays"}, {"bits", "Bits / symbols view", "Displays"},
        {"framesview", "Frames view (hex)", "Displays"}, {"ax25", "AX.25 packets", "Displays"}, {"image", "Image view", "Displays"},
        {"analyzer", "Analyzer", "Displays"},
        // code
        {"script", "Script (Lua)", "Code"},
        // crypto (teaching labs)
        {"keysrc", "Key source", "Crypto"}, {"cipher", "Cipher (AES)", "Crypto"}, {"xorbrute", "Brute force (XOR key)", "Crypto"},
        // outputs
        {"audio", "Audio out (speaker / sound card)", "Sinks"}, {"wavsink", "WAV file (record)", "Sinks"}, {"bytesink", "Byte file (record)", "Sinks"},
    };
    return l;
}

// Hidden search keywords for each block: related ideas, what it is used for, other names
// (English and Polish without diacritics, the search box takes ASCII only)
inline const char* blockTags(const std::string& t) {
    static const std::map<std::string, const char*> m = {
        {"rxin", "receiver radio input channel baseband iq second another sdr rx odbiornik wejscie kanal drugi"},
        {"srciq", "generator oscillator nco tone carrier sine cosine complex exponential test signal generator sygnalu nosna ton sinus oscylator"},
        {"srcreal", "generator oscillator tone sine square triangle sawtooth ramp dc constant function generator sinus prostokat trojkat pila stala sygnal testowy"},
        {"noiseiq", "awgn gaussian white noise channel model snr test szum bialy gaussa kanal"},
        {"noisereal", "awgn gaussian uniform white noise dither test szum bialy gaussa rownomierny"},
        {"bitsrc", "prbs pn9 pn15 pn23 pseudo random sequence ber test data bity losowe generator danych"},
        {"patsrc", "pattern repeat preamble sync word bits test wzorzec preambula powtarzanie"},
        {"msgsrc", "message text string ascii packet payload counter beacon wiadomosc tekst ramka nadajnik"},
        {"add", "sum adder mix combine plus superposition suma dodawanie sumator mieszanie"},
        {"sub", "difference minus subtract compare error roznica odejmowanie"},
        {"mul", "multiply mixer product modulate ring heterodyne mnozenie mieszacz iloczyn"},
        {"mulconj", "conjugate correlation cross phase difference mixer sprzezenie korelacja roznica faz"},
        {"affine", "scale offset constant gain bias multiply add skala przesuniecie stala"},
        {"gain", "amplify attenuate volume level db wzmocnienie tlumienie glosnosc"},
        {"mag", "magnitude abs power envelope rssi db log modul moc obwiednia amplituda decybele"},
        {"c2r", "complex to real split i q real imaginary magnitude phase arg angle rozdziel czesc rzeczywista urojona faza"},
        {"r2c", "real to complex combine i q make complex zloz zespolony"},
        {"iqswap", "swap i q invert conjugate spectrum flip mirror negative frequency j rotate zamiana odwrocenie widma lustro"},
        {"delay", "delay z-1 unit delay feedback loop sprzezenie zwrotne petla shift samples buffer align opoznienie przesuniecie probki"},
        {"movavg", "moving average boxcar smoothing mean running sum lowpass srednia kroczaca wygladzanie"},
        {"intdump", "integrate and dump matched filter rectangular pulse symbol energy calkowanie filtr dopasowany"},
        {"diffr", "derivative differentiate difference slope highpass pochodna rozniczkowanie"},
        {"accum", "accumulate integrator cumulative sum running total calka akumulator suma"},
        {"clip", "clip limiter saturate hard limit clamp ogranicznik obcinanie nasycenie"},
        {"fir", "fir convolution correlation matched filter taps coefficients impulse response kernel splot korelacja wspolczynniki odpowiedz impulsowa filtr dopasowany"},
        {"conv", "convolution correlation two signals a b kernel cross correlation matched filter splot korelacja dwoch sygnalow wzajemna"},
        {"tf", "transfer function laplace s domain plant model system lti bode poles zeros stability step response inertia oscillator transmitancja uklad inercyjny obiekt bieguny zera stabilnosc automatyka"},
        {"pid", "pid controller regulator proportional integral derivative feedback control kp ki kd anti windup regulator automatyka sterowanie"},
        {"loop", "closed loop feedback control system pid plant regulation set point disturbance uklad zamkniety sprzezenie zwrotne regulacja automatyka uchyb"},
        {"stepinfo", "step response metrics rise time overshoot settling time steady state error peak odpowiedz skokowa czas narastania przeregulowanie czas regulacji uchyb ustalony"},
        {"firdesign", "fir design windowed sinc lowpass highpass bandpass bandstop notch rrc gaussian kaiser hamming hann blackman window projekt filtra dolnoprzepustowy gornoprzepustowy pasmowy zaporowy okno"},
        {"iir", "iir recursive biquad butterworth lowpass highpass bandpass notch peaking shelf poles zeros transfer function rekursywny bieguny zera transmitancja"},
        {"hilbert", "hilbert analytic signal 90 degree phase shift ssb real to complex sygnal analityczny przesuniecie fazy"},
        {"lowpass", "lowpass channel filter bandwidth anti alias selectivity dolnoprzepustowy filtr kanalu pasmo"},
        {"rlowpass", "lowpass audio filter bandwidth smoothing anti alias dolnoprzepustowy filtr audio"},
        {"rrc", "root raised cosine matched filter pulse shaping nyquist isi rolloff alpha filtr dopasowany ksztaltowanie impulsu"},
        {"dc", "dc removal offset highpass blocker coupling skladowa stala usuwanie offset"},
        {"deemph", "deemphasis emphasis 50us 75us fm broadcast audio tone deemfaza preemfaza"},
        {"upsample", "upsample interpolate zero stuffing expander increase rate interpolacja nadprobkowanie"},
        {"downsample", "downsample decimate keep every nth reduce rate decymacja"},
        {"decim", "decimate average reduce sample rate decymacja zmniejszenie czestotliwosci probkowania"},
        {"resample", "resample rational rate conversion interpolate audio rate zmiana czestotliwosci probkowania"},
        {"shift", "frequency shift translate mixer nco tune offset doppler przesuniecie czestotliwosci strojenie mieszacz"},
        {"agc", "automatic gain control normalize level amplitude automatyczna regulacja wzmocnienia normalizacja"},
        {"gate", "burst trigger squelch gate detect threshold blokada szumow wyzwalanie paczka"},
        {"fm", "fm demodulator frequency discriminator quadrature nbfm wbfm fsk demodulator czestotliwosci dyskryminator"},
        {"am", "am demodulator envelope detector amplitude demodulator amplitudy detektor obwiedni"},
        {"ssb", "ssb usb lsb real part product detector cw sideband wstega boczna"},
        {"phase", "phase demodulator pm atan2 angle unwrap demodulator fazy kat"},
        {"fsktones", "fsk tone detector goertzel mark space afsk dtmf wykrywanie tonow"},
        {"lora", "lora chirp css spread spectrum semtech sx1276 sx1278 sx1262 llcc68 ebyte e22 e32 e220 rfm95 lorawan meshtastic ttn spreading factor iot telemetria modul radiowy"},
        {"loramod", "lora modulator transmitter chirp css test loopback semtech nadajnik"},
        {"lenpkt", "packet length field byte cc1101 sx1276 fsk rfm69 si4463 variable length frame sync payload pakiet dlugosc ramki telemetria"},
        {"fields", "telemetry fields decode bytes integer uint16 int16 float little big endian scale parse sensor value temperature voltage telemetria pola dekodowanie czujnik"},
        {"matrix", "stereo matrix l r left right mpx sum difference wfm broadcast radio stereo lewy prawy"},
        {"symsync", "symbol sync timing recovery clock gardner mueller muller baud synchronizacja symboli zegar odzyskiwanie"},
        {"clockiq", "clock recovery timing gardner symbol sync psk qpsk complex odzyskiwanie zegara synchronizacja"},
        {"decide", "slicer decision hard decision quantize levels comparator decyzja kwantyzacja komparator"},
        {"threshold", "threshold comparator schmitt trigger hysteresis binary prog histereza komparator"},
        {"slicer", "clock recovery slicer bits fsk symbols demod synchronizacja decyzja"},
        {"costas", "costas loop carrier recovery pll bpsk qpsk phase lock frequency offset odzyskiwanie nosnej petla fazowa"},
        {"pilot", "pll pilot tone 19 khz stereo phase locked loop subcarrier petla fazowa pilot podnosna"},
        {"oqpsk", "oqpsk offset qpsk staggered half symbol delay meteor align"},
        {"softbits", "soft bits llr soft decision viterbi input bity miekkie decyzje miekkie"},
        {"levels", "nrz bits to levels 4fsk pam mapping line code modulator mapowanie poziomy"},
        {"psk", "bpsk qpsk 8psk 16qam qam constellation mapper modulator symbols mapowanie konstelacja"},
        {"fmmod", "fm modulator vco frequency modulation transmitter afsk fsk modulator czestotliwosci nadajnik"},
        {"ammod", "am modulator amplitude modulation transmitter dsb modulator amplitudy nadajnik"},
        {"convenc", "convolutional encoder fec k7 rate 1/2 forward error correction transmitter koder splotowy korekcja bledow"},
        {"hdlcframer", "hdlc framer flags bit stuffing ax25 packet transmitter ramkowanie nadajnik"},
        {"map", "symbols to bits demapper gray decode bity demapowanie"},
        {"invbits", "invert bits not polarity flip negate negacja odwrocenie polaryzacji"},
        {"nrzi", "nrzi decode transition differential ax25 line code dekodowanie"},
        {"manch", "manchester biphase decode line code dekodowanie"},
        {"diffbits", "differential encode decode bits xor dbpsk transition kodowanie roznicowe"},
        {"diff", "differential decode soft dqpsk phase ambiguity meteor dekodowanie roznicowe"},
        {"lfsr", "lfsr scrambler descrambler whitening randomizer polynomial pn additive multiplicative self synchronizing skrambler wybielanie wielomian"},
        {"g3ruh", "g3ruh descrambler 9600 packet ax25 skrambler"},
        {"derand", "derandomize ccsds pn whitening descrambler satellite derandomizacja satelita"},
        {"sync", "frame sync word correlator preamble access code asm find header synchronizacja ramki slowo synchronizacyjne korelator"},
        {"codedsync", "coded sync asm viterbi qpsk phase ambiguity ccsds lrpt satellite synchronizacja"},
        {"viterbi", "viterbi decoder convolutional fec k7 error correction trellis dekoder korekcja bledow"},
        {"pack", "pack bits to bytes octets frame bajty pakowanie"},
        {"unpack", "unpack bytes to bits serialize bajty bity"},
        {"hdlc", "hdlc deframer flags bit stuffing ax25 packet fcs crc aprs ramki"},
        {"crc", "crc checksum fcs crc16 crc32 x25 ccitt error detection suma kontrolna wykrywanie bledow"},
        {"slice", "slice bytes field extract header payload substring wycinanie pole naglowek"},
        {"xorbytes", "xor key bytes scramble descramble unscramble obfuscation ebyte e32 klucz szyfrowanie odszyfrowanie"},
        {"rs", "reed solomon fec error correction ccsds 255 223 korekcja bledow satellite satelita"},
        {"ccsds", "ccsds vcdu mpdu packets space protocol satellite telemetry pakiety satelita telemetria"},
        {"msumr", "meteor msu mr image jpeg lrpt satellite picture weather obraz satelita pogoda"},
        {"linesync", "line sync pattern apt noaa analog image lines correlate sync a slant weather satellite obraz synchronizacja linii satelita pogodowy"},
        {"framelines", "frame to image line pixels words 10 bit samples hrpt avhrr noaa meteor metop ahrpt fengyun channel interleave obraz ramka piksele kanal"},
        {"raster", "raster image lines wefax apt fax picture scan obraz linie"},
        {"scope", "oscilloscope scope time domain waveform plot oscyloskop przebieg czas"},
        {"spectrum", "spectrum fft frequency domain psd analyzer widmo czestotliwosc analizator"},
        {"waterfall", "waterfall spectrogram fft time frequency wodospad spektrogram"},
        {"constel", "constellation xy iq plot scatter qpsk psk qam konstelacja"},
        {"histogram", "histogram distribution statistics levels pdf rozklad statystyka"},
        {"eye", "eye diagram isi timing symbol quality diagram oczkowy"},
        {"probe", "probe number value meter rms mean power level display sonda wartosc miernik srednia"},
        {"text", "text display console terminal ascii log print wyswietlacz tekst tekstu konsola"},
        {"bits", "bits symbols view stream binary podglad bitow"},
        {"framesview", "frames hex dump bytes packets view podglad ramek szesnastkowo"},
        {"ax25", "ax25 aprs packet radio callsign decoder znak wywolawczy"},
        {"image", "image view picture png save display obraz zdjecie zapis"},
        {"analyzer", "analyzer classify modulation identify what is this signal analizator rozpoznawanie modulacji"},
        {"script", "script lua code program custom protocol logic syndrome checksum parser state machine kod skrypt wlasny protokol"},
        {"keysrc", "key source bank loop counter random encryption klucz bank petla losowy szyfrowanie"},
        {"cipher", "cipher aes encrypt decrypt ctr ecb crypto szyfr szyfrowanie odszyfrowanie klucz"},
        {"xorbrute", "brute force xor crack break key attack guess bruteforce lamanie klucz atak slabe szyfrowanie"},
        {"audio", "audio speaker sound card output headphones play dzwiek glosnik karta dzwiekowa sluchawki"},
        {"wavsink", "wav file record save output iq recording zapis nagrywanie plik"},
        {"bytesink", "file record save bytes binary output dump zapis plik binarny"},
    };
    const auto it = m.find(t);
    return it == m.end() ? "" : it->second;
}

inline std::unique_ptr<Block> makeBlockRaw(const std::string& t) {
    if (t == "input") return std::make_unique<InputBlock>(true);
    if (t == "rxin") return std::make_unique<InputBlock>(false);
    if (t == "shift") return std::make_unique<ShiftBlock>();
    if (t == "lowpass") return std::make_unique<LowpassIQ>();
    if (t == "decim") return std::make_unique<DecimateBlock>();
    if (t == "gate") return std::make_unique<GateBlock>();
    if (t == "fm") return std::make_unique<FmDemod>();
    if (t == "am") return std::make_unique<AmDemod>();
    if (t == "ssb") return std::make_unique<SsbDemod>();
    if (t == "rlowpass") return std::make_unique<LowpassReal>();
    if (t == "dc") return std::make_unique<DcBlock>();
    if (t == "gain") return std::make_unique<GainBlock>();
    if (t == "slicer") return std::make_unique<SlicerBlock>();
    if (t == "map") return std::make_unique<MapBlock>();
    if (t == "nrzi") return std::make_unique<NrziBlock>();
    if (t == "manch") return std::make_unique<ManchesterBlock>();
    if (t == "sync") return std::make_unique<SyncBlock>();
    if (t == "scope") return std::make_unique<ScopeBlock>();
    if (t == "spectrum") return std::make_unique<SpectrumBlock>();
    if (t == "constel") return std::make_unique<ConstelBlock>();
    if (t == "bits") return std::make_unique<BitsView>();
    if (t == "audio") return std::make_unique<AudioOut>();
    if (t == "analyzer") return std::make_unique<AnalyzerView>();
    if (t == "agc") return std::make_unique<AgcBlock>();
    if (t == "rrc") return std::make_unique<RrcBlock>();
    if (t == "fsktones") return std::make_unique<FskTones>();
    if (t == "clockiq") return std::make_unique<ClockIQ>();
    if (t == "costas") return std::make_unique<CostasBlock>();
    if (t == "softbits") return std::make_unique<SoftBits>();
    if (t == "g3ruh") return std::make_unique<G3ruhBlock>();
    if (t == "codedsync") return std::make_unique<CodedSync>();
    if (t == "viterbi") return std::make_unique<ViterbiBlock>();
    if (t == "derand") return std::make_unique<DerandBlock>();
    if (t == "pack") return std::make_unique<PackBytes>();
    if (t == "hdlc") return std::make_unique<HdlcBlock>();
    if (t == "rs") return std::make_unique<RsBlock>();
    if (t == "ccsds") return std::make_unique<CcsdsPackets>();
    if (t == "msumr") return std::make_unique<MsuMrBlock>();
    if (t == "raster") return std::make_unique<RasterBlock>();
    if (t == "linesync") return std::make_unique<LineSyncBlock>();
    if (t == "framelines") return std::make_unique<FrameLinesBlock>();
    if (t == "image") return std::make_unique<ImageView>();
    if (t == "ax25") return std::make_unique<Ax25View>();
    if (t == "framesview") return std::make_unique<FramesView>();
    if (t == "oqpsk") return std::make_unique<OqpskAlign>();
    if (t == "diff") return std::make_unique<DiffDecode>();
    if (t == "resample") return std::make_unique<ResampleReal>();
    if (t == "pilot") return std::make_unique<PilotPll>();
    if (t == "matrix") return std::make_unique<StereoMatrix>();
    if (t == "deemph") return std::make_unique<Deemphasis>();
    // the general library
    if (t == "srciq") return std::make_unique<SignalSourceIQ>();
    if (t == "srcreal") return std::make_unique<SignalSourceReal>();
    if (t == "noiseiq") return std::make_unique<NoiseSource>(true);
    if (t == "noisereal") return std::make_unique<NoiseSource>(false);
    if (t == "bitsrc") return std::make_unique<BitSource>();
    if (t == "patsrc") return std::make_unique<PatternSource>();
    if (t == "msgsrc") return std::make_unique<MessageSource>();
    if (t == "add") return std::make_unique<ArithBlock>(0);
    if (t == "sub") return std::make_unique<ArithBlock>(1);
    if (t == "mul") return std::make_unique<ArithBlock>(2);
    if (t == "mulconj") return std::make_unique<ArithBlock>(3);
    if (t == "affine") return std::make_unique<ConstBlock>();
    if (t == "mag") return std::make_unique<MagBlock>();
    if (t == "c2r") return std::make_unique<ComplexToReal>();
    if (t == "r2c") return std::make_unique<RealToComplex>();
    if (t == "iqswap") return std::make_unique<IqSwap>();
    if (t == "delay") return std::make_unique<DelayBlock>();
    if (t == "movavg") return std::make_unique<MovingAverage>();
    if (t == "intdump") return std::make_unique<IntegrateDump>();
    if (t == "diffr") return std::make_unique<Differentiate>();
    if (t == "accum") return std::make_unique<Accumulate>();
    if (t == "clip") return std::make_unique<ClipBlock>();
    if (t == "fir") return std::make_unique<FirTaps>();
    if (t == "conv") return std::make_unique<ConvolveBlock>();
    if (t == "tf") return std::make_unique<TfBlock>();
    if (t == "pid") return std::make_unique<PidBlock>();
    if (t == "loop") return std::make_unique<ClosedLoopBlock>();
    if (t == "stepinfo") return std::make_unique<StepInfoBlock>();
    if (t == "firdesign") return std::make_unique<FirDesign>();
    if (t == "iir") return std::make_unique<IirBlock>();
    if (t == "hilbert") return std::make_unique<HilbertBlock>();
    if (t == "upsample") return std::make_unique<Upsample>();
    if (t == "downsample") return std::make_unique<Downsample>();
    if (t == "symsync") return std::make_unique<SymbolSyncReal>();
    if (t == "decide") return std::make_unique<SlicerOnly>();
    if (t == "threshold") return std::make_unique<Threshold>();
    if (t == "phase") return std::make_unique<PhaseDemod>();
    if (t == "levels") return std::make_unique<BitsToLevels>();
    if (t == "psk") return std::make_unique<BitsToSymbols>();
    if (t == "fmmod") return std::make_unique<FmMod>();
    if (t == "ammod") return std::make_unique<AmMod>();
    if (t == "convenc") return std::make_unique<ConvEncoder>();
    if (t == "hdlcframer") return std::make_unique<HdlcFramer>();
    if (t == "lfsr") return std::make_unique<LfsrBlock>();
    if (t == "diffbits") return std::make_unique<DiffBits>();
    if (t == "invbits") return std::make_unique<InvertBits>();
    if (t == "unpack") return std::make_unique<UnpackBytes>();
    if (t == "crc") return std::make_unique<CrcBlock>();
    if (t == "slice") return std::make_unique<SliceFrames>();
    if (t == "xorbytes") return std::make_unique<XorBytes>();
    if (t == "script") return std::make_unique<ScriptBlock>();
    if (t == "keysrc") return std::make_unique<KeySource>();
    if (t == "cipher") return std::make_unique<CipherBlock>();
    if (t == "xorbrute") return std::make_unique<XorBrute>();
    if (t == "text") return std::make_unique<TextDisplay>();
    if (t == "probe") return std::make_unique<ProbeBlock>();
    if (t == "waterfall") return std::make_unique<WaterfallBlock>();
    if (t == "histogram") return std::make_unique<HistogramBlock>();
    if (t == "eye") return std::make_unique<EyeBlock>();
    if (t == "wavsink") return std::make_unique<WavSink>();
    if (t == "bytesink") return std::make_unique<ByteSink>();
    if (t == "lora") return std::make_unique<LoraRx>();
    if (t == "loramod") return std::make_unique<LoraMod>();
    if (t == "lenpkt") return std::make_unique<LengthPackets>();
    if (t == "fields") return std::make_unique<FieldsBlock>();
    return nullptr;
}

// A block with its palette title and category (the colour of its title bar)
inline std::unique_ptr<Block> makeBlock(const std::string& t) {
    auto b = makeBlockRaw(t);
    if (b) for (const auto& bi : blockList()) if (t == bi.type) { b->category = bi.category; b->title = bi.title; }
    return b;
}

inline sf::Color categoryColor(const std::string& c) {
    if (c == "Source") return sf::Color(255, 140, 0);              // the channel input
    if (c == "Sources") return sf::Color(230, 120, 40);
    if (c == "Math") return sf::Color(90, 160, 160);
    if (c == "Filters") return sf::Color(70, 150, 220);
    if (c == "Resampling") return sf::Color(100, 120, 220);
    if (c == "Channel") return sf::Color(60, 170, 200);
    if (c == "Demodulate") return sf::Color(160, 110, 230);
    if (c == "Sync") return sf::Color(220, 160, 50);
    if (c == "Modulate") return sf::Color(230, 90, 120);
    if (c == "Bits") return sf::Color(200, 120, 60);
    if (c == "Frames") return sf::Color(200, 90, 170);
    if (c == "Image") return sf::Color(180, 180, 190);
    if (c == "Sinks") return sf::Color(150, 200, 90);
    if (c == "Control") return sf::Color(80, 200, 150);
    if (c == "Code") return sf::Color(240, 220, 90);
    if (c == "Crypto") return sf::Color(210, 130, 230);
    return sf::Color(120, 125, 140);   // Displays
}


// ---------------------------------------------------------------------
//  JSON writing (the reader is orbit::parseJson)
// ---------------------------------------------------------------------
inline std::string jsonStr(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c == '\n') o += "\\n";
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o + "\"";
}
inline std::string jsonNum(double v) {
    char b[40];
    std::snprintf(b, sizeof(b), "%.10g", v);
    return b;
}

// =====================================================================
//  The pipeline
// =====================================================================
struct Link { int from = 0, out = 0, to = 0, in = 0; };

class Pipeline {
public:
    // a block, or a feedback loop (blocks that reach each other) worked out together
    struct Unit { std::vector<Block*> members, inner; bool loop = false; };
    std::mutex mtx;   // the worker (process) and the editor (UI)
    std::string name = "Untitled";
    std::vector<std::unique_ptr<Block>> nodes;
    std::vector<Link> links;
    AudioRing audio;
    int nextId = 1;
    long long processed = 0;

    Pipeline() { clear(); }

    // mtx held by the caller for all of these
    void clear() {
        nodes.clear(); links.clear(); nextId = 1; orderDirty = true;
        fifo.clear(); lined.clear();
        auto in = makeBlock("input");
        in->id = nextId++; in->x = 40; in->y = 60;
        nodes.push_back(std::move(in));
    }
    Block* find(int id) { for (auto& n : nodes) if (n->id == id) return n.get(); return nullptr; }
    InputBlock* input() { for (auto& n : nodes) if (n->type == "input") return static_cast<InputBlock*>(n.get()); return nullptr; }

    Block* add(const std::string& type, float x, float y) {
        auto b = makeBlock(type);
        if (!b || type == "input") return nullptr;
        b->id = nextId++; b->x = x; b->y = y; b->audio = &audio;
        nodes.push_back(std::move(b));
        orderDirty = true;
        return nodes.back().get();
    }
    void remove(int id) {
        Block* b = find(id);
        if (!b || !b->removable) return;
        std::vector<int> fed;   // blocks this one fed (their spare inputs shrink)
        for (const auto& l : links) if (l.from == id) fed.push_back(l.to);
        links.erase(std::remove_if(links.begin(), links.end(), [&](const Link& l) { return l.from == id || l.to == id; }), links.end());
        for (int t : fed) fitIns(find(t));
        forgetFifos(id);
        nodes.erase(std::remove_if(nodes.begin(), nodes.end(), [&](const std::unique_ptr<Block>& n) { return n->id == id; }), nodes.end());
        orderDirty = true;
    }
    // Any on an output: the same kind as the block's input (checked again while running)
    static bool compatible(PT out, PT in) {
        const auto signal = [](PT t) { return t == PT::IQ || t == PT::Real || t == PT::Any; };
        return out == in || in == PT::All || (in == PT::Any && signal(out)) || (out == PT::Any && signal(in));
    }
    bool reaches(int from, int target) const {
        if (from == target) return true;
        for (const auto& l : links) if (l.from == from && reaches(l.to, target)) return true;
        return false;
    }
    // Is there a path from `from` to `target` that does not go through a Delay? (a Delay's
    // outputs do not count: a loop through one is allowed)
    bool reachesPastDelays(int from, int target) const {
        std::vector<int> stack {from};
        std::set<int> seen;
        while (!stack.empty()) {
            const int id = stack.back(); stack.pop_back();
            if (id == target) return true;
            if (!seen.insert(id).second) continue;
            const Block* b = const_cast<Pipeline*>(this)->find(id);
            if (!b || b->isDelay) continue;
            for (const auto& l : links) if (l.from == id) stack.push_back(l.to);
        }
        return false;
    }
    // A block with growing inputs: one free input after the last connected one
    void fitIns(Block* b) {
        if (!b || b->maxIns <= 1 || b->ins.empty()) return;
        int last = -1;
        for (const auto& l : links) if (l.to == b->id) last = std::max(last, l.in);
        const size_t want = (size_t)std::clamp(last + 2, 1, b->maxIns);
        const Port first = b->ins[0];
        while (b->ins.size() < want) b->ins.push_back({first.name + " " + std::to_string(b->ins.size() + 1), first.type});
        if (b->ins.size() > want) b->ins.resize(want);
    }
    // Returns an error text, empty when connected
    std::string connect(int from, int out, int to, int in) {
        Block* a = find(from); Block* b = find(to);
        if (b && b->maxIns > 1 && in >= 0 && in < b->maxIns)
            while ((int)b->ins.size() <= in) b->ins.push_back({b->ins[0].name + " " + std::to_string(b->ins.size() + 1), b->ins[0].type});
        const std::string e = connectPort(a, from, out, b, to, in);
        fitIns(b);
        return e;
    }
    std::string connectPort(Block* a, int from, int out, Block* b, int to, int in) {
        if (!a || !b || out >= (int)a->outs.size() || in >= (int)b->ins.size()) return "no such port";
        if (!compatible(a->outs[out].type, b->ins[in].type))
            return std::string("cannot connect ") + ptName(a->outs[out].type) + " to " + ptName(b->ins[in].type);
        if (!a->isDelay && reachesPastDelays(to, from)) return "a feedback loop needs a Delay in it (Math > Delay, z^-1)";
        links.erase(std::remove_if(links.begin(), links.end(), [&](const Link& l) { return l.to == to && l.in == in; }), links.end());
        links.push_back({from, out, to, in});
        orderDirty = true;
        b->reset();
        forgetFifos(to);
        return "";
    }
    void disconnect(int to, int in) {
        links.erase(std::remove_if(links.begin(), links.end(), [&](const Link& l) { return l.to == to && l.in == in; }), links.end());
        orderDirty = true;
        fitIns(find(to));
    }
    const Link* linkInto(int to, int in) const { for (const auto& l : links) if (l.to == to && l.in == in) return &l; return nullptr; }

    // Worker thread: one block of the channel's baseband (locks mtx). `seconds`: how much time
    // this step stands for (generators make that much signal); < 0: n / rate. With no input
    // (x == nullptr, n == 0) the module still calls it now and then, so generators run alone.
    void process(const CF* x, size_t n, double rate, const std::string& inputInfo, double seconds = -1.0) {
        std::lock_guard<std::mutex> l(mtx);
        InputBlock* inb = input();
        if (!inb) return;
        inb->info = inputInfo;
        Stream& s = inb->out[0];
        s.clear(); s.type = PT::IQ; s.rate = rate;
        if (x && n) s.iq.assign(x, x + n);
        for (auto& b : nodes) if (b->isInput && b.get() != inb) b->out[0].clear();
        run(seconds >= 0.0 ? seconds : (rate > 0 ? n / rate : 0.0));
        processed += (long long)n;
    }

    // mtx held: every block but the inputs (filled by the caller), once. `tick`: the seconds
    // this step stands for (generators make that much signal)
    void run(double tick) {
        if (orderDirty) computeOrder();
        std::vector<const Stream*> ins;
        for (Unit& u : units) {
            if (u.loop) { runLoop(u, tick); continue; }
            Block* b = u.members[0];
            if (b->isInput) continue;
            b->tick = tick;
            b->warning.clear();
            gather(b, ins, [&](const Link& lk) -> const Stream* {
                Block* src = find(lk.from);
                return src && lk.out < (int)src->out.size() ? &src->out[lk.out] : nullptr;
            });
            const auto t0 = std::chrono::steady_clock::now();
            if (b->syncInputs) lineUp(b, ins);
            b->process(ins);
            b->cpuAcc += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        }
        // the blocks' share of real time, about twice a second
        cpuSignal += tick;
        if (cpuSignal >= 0.5) {
            for (auto& n : nodes) { n->cpu = n->cpu * 0.5 + 0.5 * n->cpuAcc / cpuSignal; n->cpuAcc = 0.0; }
            cpuSignal = 0.0;
        }
    }
    double cpuSignal = 0.0;   // seconds of signal since the CPU shares were updated

    // The inputs of b, checked for their kind; `source` gives the stream behind a wire
    template <typename F>
    void gather(Block* b, std::vector<const Stream*>& ins, F source) {
        ins.assign(b->ins.size(), nullptr);
        for (size_t i = 0; i < b->ins.size(); i++) {
            const Link* lk = linkInto(b->id, (int)i);
            if (!lk) continue;
            const Stream* st = source(*lk);
            if (!st) continue;
            // a "same as the input" output may carry the wrong kind for this input
            const PT want = b->ins[i].type;
            if (want == PT::All) { ins[i] = st; continue; }   // (a Script takes every kind)
            if (want != PT::Any && st->type != want && !(st->size() == 0)) {
                b->warning = std::string("input ") + b->ins[i].name + ": got " + ptName(st->type) + ", needs " + ptName(want);
                continue;
            }
            if (want == PT::Any && st->type != PT::IQ && st->type != PT::Real && st->size()) {
                b->warning = std::string("input ") + b->ins[i].name + ": needs I/Q or real";
                continue;
            }
            ins[i] = st;
        }
    }

    // ---- feedback loops ----
    // A loop (blocks that reach each other) must pass through a Delay of D samples. Its blocks
    // are worked out D samples at a time (the smallest D of its Delays): first each Delay hands
    // out what it holds, then the other blocks run in order, then the Delays take their inputs.
    // The loop runs at the pace of the first signal that comes into it from outside.
    static void sliceStream(const Stream& s, size_t a, size_t b, size_t L, Stream& o) {
        const size_t M = s.size();
        const size_t i0 = L ? (size_t)((double)a * M / L + 0.5) : 0, i1 = L ? (size_t)((double)b * M / L + 0.5) : M;
        o.clear();
        o.type = s.type; o.rate = s.rate; o.levels = s.levels; o.newBurst = s.newBurst && a == 0;
        switch (s.type) {
            case PT::IQ: o.iq.assign(s.iq.begin() + std::min(i0, M), s.iq.begin() + std::min(i1, M)); break;
            case PT::Real: o.re.assign(s.re.begin() + std::min(i0, M), s.re.begin() + std::min(i1, M)); break;
            case PT::Sym: o.sym.assign(s.sym.begin() + std::min(i0, M), s.sym.begin() + std::min(i1, M)); break;
            case PT::Frames: o.frames.assign(s.frames.begin() + std::min(i0, M), s.frames.begin() + std::min(i1, M)); break;
            default: o.lines.assign(s.lines.begin() + std::min(i0, M), s.lines.begin() + std::min(i1, M)); break;
        }
    }
    static void appendStream(Stream& d, const Stream& s, bool first) {
        if (first) { d.clear(); d.type = s.type; d.rate = s.rate; d.levels = s.levels; }
        d.newBurst = d.newBurst || s.newBurst;
        d.iq.insert(d.iq.end(), s.iq.begin(), s.iq.end());
        d.re.insert(d.re.end(), s.re.begin(), s.re.end());
        d.sym.insert(d.sym.end(), s.sym.begin(), s.sym.end());
        d.frames.insert(d.frames.end(), s.frames.begin(), s.frames.end());
        d.lines.insert(d.lines.end(), s.lines.begin(), s.lines.end());
    }
    void runLoop(Unit& u, double tick) {
        std::set<int> mem;
        for (Block* b : u.members) { mem.insert(b->id); b->warning.clear(); }
        // the pace: the first signal that comes in from outside
        const Stream* ref = nullptr;
        for (Block* b : u.members) for (const auto& l : links) {
            if (ref || l.to != b->id || mem.count(l.from)) continue;
            Block* src = find(l.from);
            if (!src || l.out >= (int)src->out.size()) continue;
            const Stream& s = src->out[l.out];
            if ((s.type == PT::IQ || s.type == PT::Real) && s.rate > 0 && s.size() > 0) ref = &s;
        }
        if (!ref) {
            for (Block* b : u.members) for (auto& o : b->out) o.clear();
            u.members[0]->warning = "feedback loop: no signal comes into it (it runs at the pace of its input)";
            return;
        }
        (void)tick;
        const size_t L = ref->size();
        const double R = ref->rate;
        size_t S = SIZE_MAX;
        std::vector<Block*> delays, others;
        for (Block* b : u.inner) (b->isDelay ? delays : others).push_back(b);
        for (Block* d : delays) S = std::min(S, d->loopDelay());
        if (delays.empty() || S == SIZE_MAX) S = L;
        // what each Delay hands out: the kind of what goes into it
        std::vector<PT> dType;
        for (Block* d : delays) {
            PT t = PT::Real;
            if (const Link* lk = linkInto(d->id, 0)) if (Block* src = find(lk->from)) {
                if (src->outs[lk->out].type == PT::IQ || src->outs[lk->out].type == PT::Real) t = src->outs[lk->out].type;
                else if (src->out[lk->out].rate > 0 && (src->out[lk->out].type == PT::IQ || src->out[lk->out].type == PT::Real)) t = src->out[lk->out].type;
            }
            dType.push_back(t);
        }
        std::map<std::pair<int, int>, Stream> acc, slices;
        auto source = [&](Block* b, const Link& lk, size_t pos, size_t len) -> const Stream* {
            Block* src = find(lk.from);
            if (!src || lk.out >= (int)src->out.size()) return nullptr;
            if (mem.count(lk.from)) return &src->out[lk.out];
            Stream& sl = slices[{b->id, lk.in}];
            sliceStream(src->out[lk.out], pos, pos + len, L, sl);
            return &sl;
        };
        std::vector<const Stream*> ins;
        for (size_t pos = 0; pos < L; pos += S) {
            const size_t len = std::min(S, L - pos);
            for (size_t k = 0; k < delays.size(); k++) { delays[k]->tick = len / R; delays[k]->loopEmit(len, dType[k], R); }
            for (Block* b : others) {
                const auto t0 = std::chrono::steady_clock::now();
                b->tick = len / R;
                gather(b, ins, [&](const Link& lk) { return source(b, lk, pos, len); });
                if (b->syncInputs) lineUp(b, ins);
                b->process(ins);
                b->cpuAcc += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            }
            for (Block* d : delays) {
                const Link* lk = linkInto(d->id, 0);
                d->loopAbsorb(lk ? source(d, *lk, pos, len) : nullptr, len);
            }
            for (Block* b : u.members)
                for (size_t k = 0; k < b->out.size(); k++) appendStream(acc[{b->id, (int)k}], b->out[k], pos == 0);
        }
        for (Block* b : u.members)
            for (size_t k = 0; k < b->out.size(); k++) b->out[k] = std::move(acc[{b->id, (int)k}]);
    }

    // ---- lining up the inputs of a block that combines samples (Add, Multiply, Real to complex,
    //      Audio left / right ...) ----
    // Each input waits in a FIFO; the block gets the same number of samples on every input, all
    // at the rate of the first one (others are converted, linear interpolation - shown as a
    // note). An input that brings nothing while the first has 0.3 s waiting is left out (0).
    struct SyncIn {
        PT type = PT::IQ;
        double srcRate = 0.0, rate = 0.0, t = 0.0;
        std::vector<CF> iq;
        std::vector<float> re;
        CF pc {0, 0};
        float pr = 0.0f;
        bool primed = false;
        bool silent = false;   // brought nothing for a while: left out until it brings samples
        size_t size() const { return type == PT::IQ ? iq.size() : re.size(); }
    };
    std::map<std::pair<int, int>, SyncIn> fifo;
    std::map<std::pair<int, int>, Stream> lined;

    void lineUp(Block* b, std::vector<const Stream*>& ins) {
        b->note.clear();
        auto signal = [](const Stream* s) { return s && (s->type == PT::IQ || s->type == PT::Real) && s->rate > 0; };
        int ref = -1;
        for (size_t i = 0; i < ins.size() && ref < 0; i++) if (signal(ins[i])) ref = (int)i;
        if (ref < 0) return;
        const double R = ins[ref]->rate;
        std::vector<int> used;
        for (size_t i = 0; i < ins.size(); i++) {
            if (!signal(ins[i])) continue;
            const Stream& s = *ins[i];
            SyncIn& f = fifo[{b->id, (int)i}];
            if (f.type != s.type || f.srcRate != s.rate || f.rate != R) { f = SyncIn(); f.type = s.type; f.srcRate = s.rate; f.rate = R; }
            if (s.rate == R) {
                if (s.type == PT::IQ) f.iq.insert(f.iq.end(), s.iq.begin(), s.iq.end());
                else f.re.insert(f.re.end(), s.re.begin(), s.re.end());
            } else {
                // to the first input's rate
                const double step = s.rate / R;
                if (s.type == PT::IQ) {
                    for (CF x : s.iq) {
                        if (!f.primed) { f.pc = x; f.primed = true; continue; }
                        while (f.t < 1.0) { f.iq.push_back(f.pc + (x - f.pc) * (float)f.t); f.t += step; }
                        f.t -= 1.0; f.pc = x;
                    }
                } else {
                    for (float x : s.re) {
                        if (!f.primed) { f.pr = x; f.primed = true; continue; }
                        while (f.t < 1.0) { f.re.push_back(f.pr + (x - f.pr) * (float)f.t); f.t += step; }
                        f.t -= 1.0; f.pr = x;
                    }
                }
                char t[160];
                std::snprintf(t, sizeof(t), "%s%s %.4g -> %.4g kS/s", b->note.empty() ? "" : ", ", b->ins[i].name.c_str(), s.rate / 1000.0, R / 1000.0);
                b->note += t;
            }
            used.push_back((int)i);
        }
        const size_t refHave = fifo[{b->id, ref}].size();
        size_t n = refHave;
        for (int i : used) {
            SyncIn& f = fifo[{b->id, i}];
            if (f.size() > 0) f.silent = false;
            if (i != ref && f.size() == 0 && (f.silent || refHave > (size_t)(0.3 * R))) {
                f.silent = true;
                ins[i] = nullptr;   // brings nothing: left out
                b->note += std::string(b->note.empty() ? "" : ", ") + b->ins[i].name + ": no samples";
                continue;
            }
            n = std::min(n, f.size());
        }
        for (int i : used) {
            if (!ins[i]) continue;
            SyncIn& f = fifo[{b->id, i}];
            Stream& o = lined[{b->id, i}];
            o.clear();
            o.type = f.type; o.rate = R; o.newBurst = ins[i]->newBurst; o.levels = ins[i]->levels;
            if (f.type == PT::IQ) { o.iq.assign(f.iq.begin(), f.iq.begin() + n); f.iq.erase(f.iq.begin(), f.iq.begin() + n); }
            else { o.re.assign(f.re.begin(), f.re.begin() + n); f.re.erase(f.re.begin(), f.re.begin() + n); }
            // never more than 2 s waiting (an input far ahead of the others)
            const size_t cap = (size_t)(2.0 * R) + 1;
            if (f.iq.size() > cap) f.iq.erase(f.iq.begin(), f.iq.end() - cap);
            if (f.re.size() > cap) f.re.erase(f.re.begin(), f.re.end() - cap);
            ins[i] = &o;
        }
    }
    void forgetFifos(int id) {
        for (auto it = fifo.begin(); it != fifo.end();) it = it->first.first == id ? fifo.erase(it) : std::next(it);
        for (auto it = lined.begin(); it != lined.end();) it = it->first.first == id ? lined.erase(it) : std::next(it);
    }
    // the dynamic choice lists (receivers, sound cards) read again - UI thread, mtx held
    void refreshChoices() { for (auto& b : nodes) b->refreshChoices(); }

    // ---- JSON ----
    std::string toJson(int indent = 2) const {
        const std::string p1(indent, ' '), p2(indent * 2, ' '), p3(indent * 3, ' ');
        std::string o = "{\n" + p1 + "\"name\": " + jsonStr(name) + ",\n" + p1 + "\"version\": 1,\n" + p1 + "\"nodes\": [\n";
        for (size_t i = 0; i < nodes.size(); i++) {
            const Block& b = *nodes[i];
            o += p2 + "{\"id\": " + std::to_string(b.id) + ", \"type\": " + jsonStr(b.type) + ", \"x\": " + jsonNum(std::round(b.x)) +
                 ", \"y\": " + jsonNum(std::round(b.y)) + ", \"params\": {";
            for (size_t k = 0; k < b.params.size(); k++) {
                const Param& p = b.params[k];
                o += (k ? ", " : "") + jsonStr(p.key) + ": " + (p.kind == Param::Text ? jsonStr(p.text) : jsonNum(p.v));
                // a choice from a list that may change (sound cards): its name too
                if (p.kind == Param::Choice && p.dynamic) {
                    std::string nm = p.show();
                    const size_t g = nm.find(Block::GONE);
                    if (g != std::string::npos) nm.erase(g);
                    o += ", " + jsonStr(p.key + "__name") + ": " + jsonStr(nm);
                }
            }
            o += "}}" + std::string(i + 1 < nodes.size() ? "," : "") + "\n";
        }
        o += p1 + "],\n" + p1 + "\"links\": [\n";
        for (size_t i = 0; i < links.size(); i++) {
            const Link& l = links[i];
            o += p2 + "{\"from\": " + std::to_string(l.from) + ", \"out\": " + std::to_string(l.out) + ", \"to\": " + std::to_string(l.to) +
                 ", \"in\": " + std::to_string(l.in) + "}" + (i + 1 < links.size() ? "," : "") + "\n";
        }
        (void)p3;
        return o + p1 + "]\n}";
    }

    // Replaces this pipeline with the JSON one; false (and unchanged) if it is not a pipeline
    bool fromJson(const orbit::JsonValue& v, std::string& err) {
        const orbit::JsonValue* ns = v.get("nodes");
        if (!ns || ns->type != orbit::JsonValue::Array) { err = "no \"nodes\" array"; return false; }
        std::vector<std::unique_ptr<Block>> nn;
        int maxId = 0;
        bool haveInput = false;
        for (const auto& jn : ns->arr) {
            auto b = makeBlock(jn.getString("type"));
            if (!b) continue;   // unknown block (newer version): skipped
            b->id = (int)jn.getNumber("id", 0);
            b->x = (float)jn.getNumber("x", 40); b->y = (float)jn.getNumber("y", 60);
            b->audio = &audio;
            if (const orbit::JsonValue* ps = jn.get("params")) {
                for (auto& p : b->params) {
                    const orbit::JsonValue* pv = ps->get(p.key);
                    if (!pv) continue;
                    if (p.kind == Param::Text && pv->type == orbit::JsonValue::String) p.text = pv->str;
                    else if (pv->type == orbit::JsonValue::Number) p.v = std::clamp(pv->num, p.lo, p.hi);
                    if (p.kind == Param::Choice && p.dynamic) {
                        const std::string nm = ps->getString(p.key + "__name");
                        bool found = false;
                        for (size_t c = 0; c < p.choices.size(); c++) if (!nm.empty() && p.choices[c] == nm) { p.v = (double)c; found = true; }
                        if (!found && !nm.empty() && nm != "receiver's speaker") {
                            p.choices.push_back(nm + Block::GONE);   // not here now: refreshChoices() looks again
                            p.hi = (double)p.choices.size() - 1;
                            p.v = p.hi;
                        } else if (!found) p.v = 0;
                    }
                }
            }
            if (b->type == "input") { if (haveInput) continue; haveInput = true; }
            b->refreshChoices();
            maxId = std::max(maxId, b->id);
            nn.push_back(std::move(b));
        }
        if (!haveInput) {
            auto in = makeBlock("input");
            in->id = ++maxId; in->x = 40; in->y = 60;
            nn.insert(nn.begin(), std::move(in));
        }
        nodes = std::move(nn);
        links.clear();
        fifo.clear(); lined.clear();
        nextId = maxId + 1;
        name = v.getString("name", name);
        if (const orbit::JsonValue* ls = v.get("links")) {
            for (const auto& jl : ls->arr)
                connect((int)jl.getNumber("from"), (int)jl.getNumber("out"), (int)jl.getNumber("to"), (int)jl.getNumber("in"));
        }
        orderDirty = true;
        return true;
    }

private:
    std::vector<Unit> units;
    std::vector<Block*> order;
    bool orderDirty = true;
    // The order of work: every group of blocks that reach each other (a feedback loop) is one
    // unit; the units in the order of their wires; inside a loop, the order with the Delays'
    // outputs left out (they are known before the step)
    void computeOrder() {
        order.clear();
        units.clear();
        // Tarjan's strongly connected components
        std::map<int, int> index, low, comp;
        std::vector<int> stack;
        std::set<int> onStack;
        int counter = 0, ncomp = 0;
        std::function<void(int)> visit = [&](int v) {
            index[v] = low[v] = counter++;
            stack.push_back(v); onStack.insert(v);
            for (const auto& l : links) {
                if (l.from != v) continue;
                const int w = l.to;
                if (!index.count(w)) { visit(w); low[v] = std::min(low[v], low[w]); }
                else if (onStack.count(w)) low[v] = std::min(low[v], index[w]);
            }
            if (low[v] == index[v]) {
                for (;;) { const int w = stack.back(); stack.pop_back(); onStack.erase(w); comp[w] = ncomp; if (w == v) break; }
                ncomp++;
            }
        };
        for (auto& n : nodes) if (!index.count(n->id)) visit(n->id);
        // the components in the order of the wires between them
        std::vector<std::vector<Block*>> members(ncomp);
        for (auto& n : nodes) members[comp[n->id]].push_back(n.get());
        std::vector<int> indeg(ncomp, 0);
        std::set<std::pair<int, int>> edges;
        for (const auto& l : links) {
            const int a = comp[l.from], b = comp[l.to];
            if (a != b && edges.insert({a, b}).second) indeg[b]++;
        }
        std::deque<int> q;
        for (int c = 0; c < ncomp; c++) if (indeg[c] == 0) q.push_back(c);
        while (!q.empty()) {
            const int c = q.front(); q.pop_front();
            Unit u;
            u.members = members[c];
            bool self = false;
            for (const auto& l : links) if (l.from == l.to && comp[l.from] == c) self = true;
            u.loop = u.members.size() > 1 || self;
            if (u.loop) {
                // inside: wires out of a Delay do not count
                std::map<int, int> in2;
                for (Block* b : u.members) in2[b->id] = 0;
                for (const auto& l : links) if (comp[l.from] == c && comp[l.to] == c && !find(l.from)->isDelay) in2[l.to]++;
                std::deque<int> q2;
                for (Block* b : u.members) if (in2[b->id] == 0) q2.push_back(b->id);
                std::set<int> placed;
                while (!q2.empty()) {
                    const int id = q2.front(); q2.pop_front();
                    u.inner.push_back(find(id)); placed.insert(id);
                    if (find(id)->isDelay) continue;
                    for (const auto& l : links) if (l.from == id && comp[l.to] == c && --in2[l.to] == 0) q2.push_back(l.to);
                }
                for (Block* b : u.members) if (!placed.count(b->id)) u.inner.push_back(b);   // (a loop without a Delay: not allowed by connect)
            } else {
                u.inner = u.members;
            }
            for (Block* b : u.inner) order.push_back(b);
            units.push_back(std::move(u));
            for (const auto& e : edges) if (e.first == c && --indeg[e.second] == 0) q.push_back(e.second);
        }
        orderDirty = false;
    }
};

// ---------------------------------------------------------------------
//  Presets: ready-made chains, one JSON file each.
//   - the ones that come with OrbitSDR: assets/pipelines/ in the repository, next to the
//     program after installing (pipelines/ beside the .exe, in the app bundle's Resources,
//     in the Linux bundle's folder);
//   - the user's own: the "presets" folder next to pipelines.json in the settings folder.
//     A chain dropped there (e.g. written by an AI assistant from docs/pipeline.md) is in
//     the Presets menu the next time it opens.
//  A preset file is a chain (section 11 of docs/pipeline.md) with two more fields: "preset",
//  its name in the menu (else "name" is used), and "description". The files are listed by
//  file name (the built-in ones start with a number for their order).
// ---------------------------------------------------------------------
struct Preset { std::string name, json, description, file; };

// the folder of the built-in presets, "" when there is none
inline std::string builtinPresetDir() {
    const std::string exe = getExecutableDir();
    for (const std::string& c : {exe + "/pipelines", exe + "/../Resources/pipelines", std::string("./assets/pipelines"),
                                 exe + "/../assets/pipelines", exe + "/assets/pipelines", std::string("../Resources/pipelines")}) {
        std::error_code ec;
        if (std::filesystem::is_directory(c, ec)) return c;
    }
    return "";
}
inline std::string userPresetDir() {
    if (!hooks().storePath) return "";
    return (std::filesystem::path(hooks().storePath()).parent_path() / "presets").string();
}

inline std::vector<Preset> loadPresetDir(const std::string& dir) {
    std::vector<Preset> v;
    if (dir.empty()) return v;
    std::error_code ec;
    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec))
        if (e.is_regular_file(ec) && e.path().extension() == ".json") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
        std::ifstream in(f, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        orbit::JsonValue j;
        if (!orbit::parseJson(ss.str(), j) || !j.get("nodes")) continue;   // not a chain
        Preset p;
        p.json = ss.str();
        p.name = j.getString("preset", j.getString("name", f.stem().string()));
        p.description = j.getString("description", "");
        p.file = f.string();
        v.push_back(std::move(p));
    }
    return v;
}

// The presets, read again by reloadPresets() (the editor does when its Presets menu opens)
inline std::vector<Preset>& presetList() { static std::vector<Preset> p; return p; }
inline void reloadPresets() {
    auto& p = presetList();
    p = loadPresetDir(builtinPresetDir());
    for (auto& u : loadPresetDir(userPresetDir())) { u.name = "(mine) " + u.name; p.push_back(std::move(u)); }
}
inline const std::vector<Preset>& presets() {
    static bool loaded = false;
    if (!loaded) { loaded = true; reloadPresets(); }
    return presetList();
}

// =====================================================================
//  Saved pipelines (pipelines.json in the settings folder)
// =====================================================================
class PipelineStore {
public:
    struct Entry { std::string name, json; };
    std::vector<Entry> items;
    std::string path;
    bool loaded = false;

    static PipelineStore& get() { static PipelineStore s; return s; }

    void load(const std::string& p) {
        path = p;
        loaded = true;
        items.clear();
        std::ifstream f(p, std::ios::binary);
        if (!f) return;
        std::stringstream ss; ss << f.rdbuf();
        orbit::JsonValue v;
        if (!orbit::parseJson(ss.str(), v)) return;
        importValue(v, false);
    }
    bool save() const {
        if (path.empty()) return false;
        std::string o = "{\n  \"orbitsdr_pipelines\": 1,\n  \"pipelines\": [\n";
        for (size_t i = 0; i < items.size(); i++) o += indent(items[i].json) + (i + 1 < items.size() ? ",\n" : "\n");
        o += "  ]\n}\n";
        const std::string tmp = path + ".tmp";
        { std::ofstream f(tmp, std::ios::binary); if (!f) return false; f << o; }
        std::remove(path.c_str());
        return std::rename(tmp.c_str(), path.c_str()) == 0;
    }
    int indexOf(const std::string& n) const { for (size_t i = 0; i < items.size(); i++) if (items[i].name == n) return (int)i; return -1; }
    std::string uniqueName(const std::string& base) const {
        if (indexOf(base) < 0) return base;
        for (int k = 2;; k++) { const std::string n = base + " (" + std::to_string(k) + ")"; if (indexOf(n) < 0) return n; }
    }
    void put(const std::string& name, const std::string& json) {
        const int i = indexOf(name);
        if (i >= 0) items[i].json = json; else items.push_back({name, json});
    }
    void erase(const std::string& name) { const int i = indexOf(name); if (i >= 0) items.erase(items.begin() + i); }

    // A file with one pipeline or a whole store; returns how many were added (renamed on a clash)
    int importValue(const orbit::JsonValue& v, bool rename = true) {
        int n = 0;
        auto one = [&](const orbit::JsonValue& p) {
            Pipeline tmp;
            std::string err;
            if (!tmp.fromJson(p, err)) return;
            tmp.name = rename ? uniqueName(p.getString("name", "Imported")) : p.getString("name", "Pipeline " + std::to_string(items.size() + 1));
            put(tmp.name, tmp.toJson());
            n++;
        };
        if (const orbit::JsonValue* arr = v.get("pipelines")) { for (const auto& p : arr->arr) one(p); }
        else one(v);
        return n;
    }

private:
    static std::string indent(const std::string& j) {
        std::string o = "    ";
        for (char c : j) { o += c; if (c == '\n') o += "    "; }
        return o;
    }
};

} // namespace pl
