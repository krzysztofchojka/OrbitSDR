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
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <functional>
#include <algorithm>
#include <fstream>
#include <sstream>
#include "../../DSP.h"
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

enum class PT { IQ, Real, Sym, Any };   // Any: an input that takes I/Q or real
inline const char* ptName(PT t) { return t == PT::IQ ? "I/Q" : t == PT::Real ? "real" : t == PT::Sym ? "symbols" : "I/Q or real"; }
inline sf::Color ptColor(PT t) {
    return t == PT::IQ ? sf::Color(90, 200, 255) : t == PT::Real ? sf::Color(120, 230, 120) : t == PT::Sym ? sf::Color(255, 190, 80) : sf::Color(200, 200, 210);
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
    void clear() { iq.clear(); re.clear(); sym.clear(); newBurst = false; }
    size_t size() const { return type == PT::IQ ? iq.size() : type == PT::Real ? re.size() : sym.size(); }
};

struct Param {
    enum Kind { Num, Choice, Text } kind = Num;
    std::string key, label, unit;
    double v = 0.0, lo = 0.0, hi = 1.0, step = 1.0;
    int decimals = 0;
    std::vector<std::string> choices;
    std::string text;
    std::string show() const {
        if (kind == Choice) return choices.empty() ? "" : choices[std::clamp((int)v, 0, (int)choices.size() - 1)];
        if (kind == Text) return text.empty() ? "(empty)" : text;
        char b[48];
        std::snprintf(b, sizeof(b), "%.*f", decimals, v);
        return unit.empty() ? std::string(b) : std::string(b) + " " + unit;
    }
};

struct Port { std::string name; PT type; };

// Speaker output of the pipeline (48 kHz mono), filled by the Audio block
struct AudioRing {
    std::mutex m;
    std::deque<float> q;
    void push(const std::vector<float>& x) {
        std::lock_guard<std::mutex> l(m);
        q.insert(q.end(), x.begin(), x.end());
        while (q.size() > 48000) q.pop_front();
    }
    bool mix(float* out, size_t n) {
        std::lock_guard<std::mutex> l(m);
        if (q.empty()) return false;
        for (size_t i = 0; i < n && !q.empty(); i++) { out[i] += q.front(); q.pop_front(); }
        return true;
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

class InputBlock : public Block {
public:
    std::string info;   // set by the analyzer: frequency, width, rate
    InputBlock() { type = "input"; title = "Channel input"; category = "Source"; removable = false; setOuts({{"I/Q", PT::IQ}}); }
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
        tmp.assign(vals.begin(), vals.end());
        const size_t a = tmp.size() * 15 / 100, b = std::min(tmp.size() - 1, tmp.size() * 85 / 100);
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
            if (track && haveSym) {
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
                  addNum("len", "Frame length after it", 512, 0, 100000, 8, "bits");
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
class ScopeBlock : public Block {
    std::deque<float> a, b;          // real, or I and Q
    std::vector<float> showA, showB;
    bool isIQ = false;
    bool waiting = true;             // armed, looking for the trigger
    long long sinceTrig = 0, sinceShow = 0;
    size_t need = 0, preN = 0;
    int capturing = -1;              // samples still to collect after the trigger (-1: not)
    float prevV = 0.0f;
    double rate = 0.0;
    bool singleDone = false;
    void snapshot() {
        const size_t n = std::min(need, a.size());
        showA.assign(a.end() - n, a.end());
        showB.clear();
        if (isIQ) showB.assign(b.end() - std::min(n, b.size()), b.end());
        sinceShow = 0;
    }
public:
    ScopeBlock() { type = "scope"; title = "Scope"; category = "View"; ins = {{"in", PT::Any}};
                   addNum("ms", "Time span", 50, 0.1, 5000, 5, "ms", 1);
                   addChoice("mode", "Trigger", {"Auto", "Normal", "Single"});
                   addChoice("src", "Trigger on", {"Level (rising)", "Start of a burst"});
                   addNum("lvl", "Level", 0, -1e6, 1e6, 100, "", 1); }
    sf::Vector2f viewSize() const override { return {380.0f, 170.0f}; }
    std::vector<std::string> buttons() const override { return {"Arm (Single)"}; }
    void onButton(int) override { singleDone = false; waiting = true; capturing = -1; }
    std::string status() const override {
        const int m = choice("mode");
        if (m == 2) return singleDone ? "Single: captured (Arm for the next)" : "Single: armed, waiting";
        return m == 1 ? "Normal: shows the last trigger" : "Auto";
    }
    void process(const std::vector<const Stream*>& in) override {
        if (!in[0] || in[0]->rate <= 0) return;
        isIQ = in[0]->type == PT::IQ;
        rate = in[0]->rate;
        need = std::max<size_t>(16, (size_t)(num("ms") * 0.001 * rate));
        preN = need / 10;
        const int mode = choice("mode");
        const bool burstTrig = choice("src") == 1;
        const size_t n = in[0]->size();
        for (size_t i = 0; i < n; i++) {
            float va, vb = 0.0f;
            if (isIQ) { va = in[0]->iq[i].real(); vb = in[0]->iq[i].imag(); } else va = in[0]->re[i];
            a.push_back(va); if (isIQ) b.push_back(vb);
            while (a.size() > need * 2) a.pop_front();
            while (b.size() > need * 2) b.pop_front();
            sinceTrig++; sinceShow++;
            const float lv = isIQ ? std::sqrt(va * va + vb * vb) : va;
            const bool trig = burstTrig ? (i == 0 && in[0]->newBurst) : (prevV < num("lvl") && lv >= num("lvl"));
            prevV = lv;
            if (capturing >= 0) {
                if (--capturing <= 0) { snapshot(); capturing = -1; if (mode == 2) singleDone = true; }
                continue;
            }
            if (trig && !(mode == 2 && singleDone)) { capturing = (int)(need - preN); sinceTrig = 0; continue; }
            if (mode == 0 && sinceShow > (long long)need * 2 && sinceTrig > (long long)need * 2) snapshot();
        }
    }
    void drawView(const Draw& d, const sf::FloatRect& r) override {
        d.rect(r, sf::Color(8, 10, 12), sf::Color(50, 50, 58));
        if (showA.size() < 2) { d.text(rate > 0 ? "waiting for the trigger" : "no input", r.position.x + 6, r.position.y + 4, 11, sf::Color(110, 110, 120)); return; }
        float lo = 1e30f, hi = -1e30f;
        for (float v : showA) { lo = std::min(lo, v); hi = std::max(hi, v); }
        for (float v : showB) { lo = std::min(lo, v); hi = std::max(hi, v); }
        if (hi - lo < 1e-9f) { hi += 1; lo -= 1; }
        const float pad = (hi - lo) * 0.08f; lo -= pad; hi += pad;
        auto plot = [&](const std::vector<float>& v, sf::Color c) {
            sf::VertexArray va(sf::PrimitiveType::LineStrip);
            const size_t n = v.size();
            const size_t step = std::max<size_t>(1, n / 1500);
            for (size_t i = 0; i < n; i += step)
                va.append({{r.position.x + r.size.x * i / (n - 1), r.position.y + r.size.y * (1.0f - (v[i] - lo) / (hi - lo))}, c});
            d.win->draw(va);
        };
        if (!burstTrigLine()) {
            const float ly = r.position.y + r.size.y * (1.0f - ((float)num("lvl") - lo) / (hi - lo));
            if (ly > r.position.y && ly < r.position.y + r.size.y) d.rect(sf::FloatRect({r.position.x, ly}, {r.size.x, 1}), sf::Color(255, 140, 0, 90));
        }
        d.rect(sf::FloatRect({r.position.x + r.size.x * 0.1f, r.position.y}, {1, r.size.y}), sf::Color(255, 255, 255, 40));
        plot(showA, sf::Color(120, 230, 120));
        if (!showB.empty()) plot(showB, sf::Color(90, 200, 255));
        char b[64];
        std::snprintf(b, sizeof(b), "%.4g .. %.4g", lo, hi);
        d.text(b, r.position.x + 4, r.position.y + 2, 10, sf::Color(140, 140, 150));
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

class AudioOut : public Block {
    double pos = 0.0, rate = 0.0;
    float prev = 0.0f, peak = 1e-3f;
    std::vector<float> o48;
public:
    AudioOut() { type = "audio"; title = "Audio out (speaker)"; category = "View"; ins = {{"in", PT::Real}};
                 addNum("vol", "Volume", 0, -60, 30, 1, "dB");
                 addChoice("agc", "Level", {"Automatic", "Fixed"}); }
    void process(const std::vector<const Stream*>& in) override {
        if (!in[0] || in[0]->rate <= 0 || !audio) return;
        rate = in[0]->rate;
        const double step = rate / 48000.0;
        const bool agc = choice("agc") == 0;
        const float g = (float)std::pow(10.0, num("vol") / 20.0);
        o48.clear();
        for (float x : in[0]->re) {
            if (agc) { peak = std::max(std::fabs(x), peak * 0.9999f); }
            while (pos <= 1.0) {
                const float v = prev + (x - prev) * (float)pos;
                o48.push_back(std::clamp((agc ? v / std::max(peak, 1e-9f) * 0.3f : v) * g, -1.0f, 1.0f));
                pos += step;
            }
            pos -= 1.0;
            prev = x;
        }
        audio->push(o48);
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

// ---------------------------------------------------------------------
//  Registry
// ---------------------------------------------------------------------
struct BlockInfo { const char* type; const char* title; const char* category; };
inline const std::vector<BlockInfo>& blockList() {
    static const std::vector<BlockInfo> l = {
        {"shift", "Frequency shift", "I/Q"}, {"lowpass", "Low-pass (I/Q)", "I/Q"}, {"decim", "Decimate", "I/Q"},
        {"gate", "Burst gate (trigger)", "I/Q"},
        {"fm", "FM demodulator", "Demodulate"}, {"am", "AM demodulator", "Demodulate"}, {"ssb", "SSB / real part", "Demodulate"},
        {"rlowpass", "Low-pass (real)", "Real"}, {"dc", "DC removal", "Real"}, {"gain", "Gain", "Real"},
        {"slicer", "Clock recovery + slicer", "Symbols"}, {"map", "Symbols to bits", "Symbols"},
        {"nrzi", "NRZI decode", "Bits"}, {"manch", "Manchester decode", "Bits"}, {"sync", "Frame sync", "Bits"},
        {"scope", "Scope", "View"}, {"spectrum", "Spectrum", "View"}, {"constel", "Constellation / XY", "View"},
        {"bits", "Bits / symbols view", "View"}, {"analyzer", "Analyzer", "View"}, {"audio", "Audio out (speaker)", "View"},
    };
    return l;
}

inline std::unique_ptr<Block> makeBlock(const std::string& t) {
    if (t == "input") return std::make_unique<InputBlock>();
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
    return nullptr;
}

inline sf::Color categoryColor(const std::string& c) {
    if (c == "Source") return sf::Color(255, 140, 0);
    if (c == "I/Q") return sf::Color(70, 150, 220);
    if (c == "Demodulate") return sf::Color(160, 110, 230);
    if (c == "Real") return sf::Color(80, 180, 100);
    if (c == "Symbols") return sf::Color(220, 160, 50);
    if (c == "Bits") return sf::Color(200, 120, 60);
    return sf::Color(120, 125, 140);   // View
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
        links.erase(std::remove_if(links.begin(), links.end(), [&](const Link& l) { return l.from == id || l.to == id; }), links.end());
        nodes.erase(std::remove_if(nodes.begin(), nodes.end(), [&](const std::unique_ptr<Block>& n) { return n->id == id; }), nodes.end());
        orderDirty = true;
    }
    static bool compatible(PT out, PT in) { return out == in || (in == PT::Any && (out == PT::IQ || out == PT::Real)); }
    bool reaches(int from, int target) const {
        if (from == target) return true;
        for (const auto& l : links) if (l.from == from && reaches(l.to, target)) return true;
        return false;
    }
    // Returns an error text, empty when connected
    std::string connect(int from, int out, int to, int in) {
        Block* a = find(from); Block* b = find(to);
        if (!a || !b || out >= (int)a->outs.size() || in >= (int)b->ins.size()) return "no such port";
        if (!compatible(a->outs[out].type, b->ins[in].type))
            return std::string("cannot connect ") + ptName(a->outs[out].type) + " to " + ptName(b->ins[in].type);
        if (reaches(to, from)) return "that would make a loop";
        disconnect(to, in);
        links.push_back({from, out, to, in});
        orderDirty = true;
        b->reset();
        return "";
    }
    void disconnect(int to, int in) {
        links.erase(std::remove_if(links.begin(), links.end(), [&](const Link& l) { return l.to == to && l.in == in; }), links.end());
        orderDirty = true;
    }
    const Link* linkInto(int to, int in) const { for (const auto& l : links) if (l.to == to && l.in == in) return &l; return nullptr; }

    // Worker thread: one block of the channel's baseband (locks mtx)
    void process(const CF* x, size_t n, double rate, const std::string& inputInfo) {
        std::lock_guard<std::mutex> l(mtx);
        InputBlock* inb = input();
        if (!inb) return;
        inb->info = inputInfo;
        Stream& s = inb->out[0];
        s.clear(); s.type = PT::IQ; s.rate = rate;
        s.iq.assign(x, x + n);
        if (orderDirty) computeOrder();
        std::vector<const Stream*> ins;
        for (Block* b : order) {
            if (b == inb) continue;
            ins.assign(b->ins.size(), nullptr);
            for (size_t i = 0; i < b->ins.size(); i++) {
                const Link* lk = linkInto(b->id, (int)i);
                if (!lk) continue;
                Block* src = find(lk->from);
                if (src && lk->out < (int)src->out.size()) ins[i] = &src->out[lk->out];
            }
            b->process(ins);
        }
        processed += (long long)n;
    }

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
                    else if (pv->type == orbit::JsonValue::Number) p.v = p.kind == Param::Num ? std::clamp(pv->num, p.lo, p.hi) : std::clamp(pv->num, p.lo, p.hi);
                }
            }
            if (b->type == "input") { if (haveInput) continue; haveInput = true; }
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
    std::vector<Block*> order;
    bool orderDirty = true;
    void computeOrder() {
        order.clear();
        std::map<int, int> indeg;
        for (auto& n : nodes) indeg[n->id] = 0;
        for (const auto& l : links) indeg[l.to]++;
        std::deque<int> q;
        for (auto& n : nodes) if (indeg[n->id] == 0) q.push_back(n->id);
        while (!q.empty()) {
            const int id = q.front(); q.pop_front();
            if (Block* b = find(id)) order.push_back(b);
            for (const auto& l : links) if (l.from == id && --indeg[l.to] == 0) q.push_back(l.to);
        }
        orderDirty = false;
    }
};

// ---------------------------------------------------------------------
//  Built-in starting points
// ---------------------------------------------------------------------
struct Preset { const char* name; const char* json; };
inline const std::vector<Preset>& presets() {
    static const std::vector<Preset> p = {
        {"FSK data -> bits (4FSK 4800)", R"({"name":"FSK data -> bits","nodes":[
            {"id":1,"type":"input","x":30,"y":40},
            {"id":2,"type":"gate","x":250,"y":40,"params":{"thr":8,"mode":0}},
            {"id":3,"type":"fm","x":520,"y":40},
            {"id":4,"type":"slicer","x":760,"y":40,"params":{"baud":4800,"lv":1}},
            {"id":5,"type":"map","x":1000,"y":40,"params":{"m":2}},
            {"id":6,"type":"bits","x":760,"y":420,"params":{"fmt":1,"len":256}},
            {"id":7,"type":"scope","x":330,"y":200,"params":{"ms":20,"mode":1,"src":1}},
            {"id":8,"type":"analyzer","x":30,"y":540}],
            "links":[{"from":1,"out":0,"to":2,"in":0},{"from":2,"out":0,"to":3,"in":0},{"from":3,"out":0,"to":4,"in":0},
                     {"from":4,"out":0,"to":5,"in":0},{"from":5,"out":0,"to":6,"in":0},{"from":3,"out":0,"to":7,"in":0},
                     {"from":1,"out":0,"to":8,"in":0}]})"},
        {"POCSAG pager (2FSK 1200)", R"({"name":"POCSAG","nodes":[
            {"id":1,"type":"input","x":30,"y":40},
            {"id":2,"type":"fm","x":250,"y":40},
            {"id":3,"type":"slicer","x":480,"y":40,"params":{"baud":1200,"lv":0}},
            {"id":4,"type":"map","x":740,"y":40,"params":{"m":1}},
            {"id":5,"type":"sync","x":740,"y":170,"params":{"pat":"0x7CD215D8","err":2,"len":512,"inv":1}},
            {"id":6,"type":"bits","x":1000,"y":40,"params":{"fmt":1}},
            {"id":7,"type":"scope","x":250,"y":200,"params":{"ms":30,"mode":0}}],
            "links":[{"from":1,"out":0,"to":2,"in":0},{"from":2,"out":0,"to":3,"in":0},{"from":3,"out":0,"to":4,"in":0},
                     {"from":4,"out":0,"to":5,"in":0},{"from":5,"out":0,"to":6,"in":0},{"from":2,"out":0,"to":7,"in":0}]})"},
        {"FM audio", R"({"name":"FM audio","nodes":[
            {"id":1,"type":"input","x":30,"y":40},
            {"id":2,"type":"fm","x":250,"y":40},
            {"id":3,"type":"rlowpass","x":480,"y":40,"params":{"cut":3500}},
            {"id":4,"type":"audio","x":720,"y":40},
            {"id":5,"type":"spectrum","x":250,"y":180}],
            "links":[{"from":1,"out":0,"to":2,"in":0},{"from":2,"out":0,"to":3,"in":0},{"from":3,"out":0,"to":4,"in":0},{"from":1,"out":0,"to":5,"in":0}]})"},
        {"AM audio", R"({"name":"AM audio","nodes":[
            {"id":1,"type":"input","x":30,"y":40},
            {"id":2,"type":"am","x":250,"y":40},
            {"id":3,"type":"dc","x":480,"y":40},
            {"id":4,"type":"audio","x":720,"y":40}],
            "links":[{"from":1,"out":0,"to":2,"in":0},{"from":2,"out":0,"to":3,"in":0},{"from":3,"out":0,"to":4,"in":0}]})"},
        {"OOK -> bits (remotes, sensors)", R"({"name":"OOK -> bits","nodes":[
            {"id":1,"type":"input","x":30,"y":40},
            {"id":2,"type":"gate","x":250,"y":40,"params":{"thr":10}},
            {"id":3,"type":"am","x":520,"y":40},
            {"id":4,"type":"slicer","x":750,"y":40,"params":{"baud":2000,"lv":0,"clk":1}},
            {"id":5,"type":"map","x":1000,"y":40,"params":{"m":0}},
            {"id":6,"type":"bits","x":1000,"y":180},
            {"id":7,"type":"scope","x":520,"y":200,"params":{"ms":50,"mode":1,"src":1}}],
            "links":[{"from":1,"out":0,"to":2,"in":0},{"from":2,"out":0,"to":3,"in":0},{"from":3,"out":0,"to":4,"in":0},
                     {"from":4,"out":0,"to":5,"in":0},{"from":5,"out":0,"to":6,"in":0},{"from":3,"out":0,"to":7,"in":0}]})"},
        {"Single-shot capture (scope + spectrum)", R"({"name":"Single-shot capture","nodes":[
            {"id":1,"type":"input","x":30,"y":40},
            {"id":2,"type":"gate","x":250,"y":40,"params":{"mode":1}},
            {"id":3,"type":"fm","x":520,"y":40},
            {"id":4,"type":"scope","x":760,"y":40,"params":{"ms":100,"mode":2,"src":1}},
            {"id":5,"type":"spectrum","x":250,"y":260},
            {"id":6,"type":"constel","x":760,"y":330}],
            "links":[{"from":1,"out":0,"to":2,"in":0},{"from":2,"out":0,"to":3,"in":0},{"from":3,"out":0,"to":4,"in":0},
                     {"from":2,"out":0,"to":5,"in":0},{"from":2,"out":0,"to":6,"in":0}]})"},
    };
    return p;
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
