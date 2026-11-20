#pragma once
// =====================================================================
//  The general block library (included from Pipeline.h, inside namespace
//  pl): one basic function per block, like in GNU Radio.
//
//   Sources      signal / noise generators, random bits, bit pattern, messages
//   Math         add, subtract, multiply, conjugate multiply, constants,
//                magnitude, dB, delay, moving average, integrate, differentiate,
//                clip, I/Q swap / invert, complex <-> real parts
//   Filters      FIR (own taps, convolution), FIR design (LP/HP/BP/BS, RRC,
//                Gaussian, windows), IIR (biquads, Butterworth, own a/b),
//                Hilbert transform. The designers show |H(f)| and h[n].
//   Resampling   upsample, downsample (keep 1 in N), resample (any rate)
//   Sync / demod symbol sync (real), slicer, threshold, phase demodulator
//   Modulation   bits -> levels / PSK / QAM, FM and AM modulators,
//                convolutional encoder, HDLC framer
//   Bits         LFSR scrambler / descrambler, differential, invert, unpack,
//                CRC check / append (frames)
//   Displays     text, number (probe), waterfall, histogram, eye diagram
//   Sinks        WAV file, byte file, audio to a chosen sound card
//
//  Blocks marked "I/Q or real" take either and give the same kind out
//  (the output port's type is Any).
// =====================================================================

// ---------------------------------------------------------------------
//  helpers
// ---------------------------------------------------------------------

// Runs f on the samples of an I/Q or real input, output of the same kind
template <typename FIQ, typename FRE>
inline void mapAny(const Stream* in, Stream& o, FIQ fiq, FRE fre) {
    if (!in) { o.clear(); return; }
    passOn(in, o, in->type, in->rate);
    if (in->type == PT::IQ) { o.iq.reserve(in->iq.size()); for (CF v : in->iq) o.iq.push_back(fiq(v)); }
    else if (in->type == PT::Real) { o.re.reserve(in->re.size()); for (float v : in->re) o.re.push_back(fre(v)); }
}

// Numbers from text: "1, 2.5 -3; 4" (any separators)
inline std::vector<double> parseNumbers(const std::string& s) {
    std::vector<double> v;
    const char* p = s.c_str();
    while (*p) {
        char* end = nullptr;
        const double x = std::strtod(p, &end);
        if (end != p) { v.push_back(x); p = end; } else p++;
    }
    return v;
}

// Windows for FIR design
inline double besselI0(double x) {
    double s = 1.0, t = 1.0;
    for (int k = 1; k < 30; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; }
    return s;
}
inline std::vector<double> makeWindowFn(int n, int type, double beta) {
    std::vector<double> w(n, 1.0);
    for (int i = 0; i < n; i++) {
        const double r = n > 1 ? (double)i / (n - 1) : 0.0;
        switch (type) {
        case 1: w[i] = 0.54 - 0.46 * std::cos(2 * PI_D * r); break;                                       // Hamming
        case 2: w[i] = 0.5 - 0.5 * std::cos(2 * PI_D * r); break;                                         // Hann
        case 3: w[i] = 0.42 - 0.5 * std::cos(2 * PI_D * r) + 0.08 * std::cos(4 * PI_D * r); break;       // Blackman
        case 4: { const double a = 2 * r - 1; w[i] = besselI0(beta * std::sqrt(std::max(0.0, 1 - a * a))) / besselI0(beta); break; }   // Kaiser
        default: break;                                                                                     // rectangular
        }
    }
    return w;
}

// FIR applied to I/Q or real (taps real)
struct AnyFir {
    std::vector<float> h;
    std::vector<CF> hc; size_t pc = 0;
    std::vector<float> hr; size_t pr = 0;
    void set(const std::vector<float>& taps) { h = taps; if (h.empty()) h = {1.0f}; hc.assign(h.size() * 2, CF(0, 0)); hr.assign(h.size() * 2, 0.0f); pc = pr = 0; }
    CF step(CF x) {
        const size_t n = h.size(); hc[pc] = x; hc[pc + n] = x; pc = (pc + 1) % n;
        CF y(0, 0); const CF* p = &hc[pc];
        for (size_t k = 0; k < n; k++) y += p[k] * h[n - 1 - k];
        return y;
    }
    float step(float x) {
        const size_t n = h.size(); hr[pr] = x; hr[pr + n] = x; pr = (pr + 1) % n;
        float y = 0; const float* p = &hr[pr];
        for (size_t k = 0; k < n; k++) y += p[k] * h[n - 1 - k];
        return y;
    }
};

// |H(f)| of taps (or of an IIR b/a) and the impulse response, for the designers' views
inline void drawResponse(const Draw& d, const sf::FloatRect& r, const std::vector<double>& mag, const std::vector<float>& imp, double rate, bool complexAxis) {
    d.rect(r, sf::Color(8, 10, 12), sf::Color(50, 50, 58));
    const float hh = r.size.y * 0.62f;
    // magnitude, 0 .. -80 dB
    for (int db = 0; db >= -80; db -= 20) {
        const float y = r.position.y + 4 + hh * (-db / 80.0f);
        d.rect(sf::FloatRect({r.position.x, y}, {r.size.x, 1}), sf::Color(35, 37, 44));
        d.text(std::to_string(db), r.position.x + 2, y - 1, 9, sf::Color(100, 100, 110));
    }
    if (!mag.empty()) {
        double mx = 1e-30;
        for (double m : mag) mx = std::max(mx, m);
        sf::VertexArray va(sf::PrimitiveType::LineStrip);
        for (size_t i = 0; i < mag.size(); i++) {
            const double db = std::clamp(20 * std::log10(mag[i] / mx + 1e-12), -80.0, 0.0);
            va.append({{r.position.x + r.size.x * i / (mag.size() - 1), r.position.y + 4 + hh * (float)(-db / 80.0)}, sf::Color(255, 170, 60)});
        }
        d.win->draw(va);
    }
    char b[64];
    std::snprintf(b, sizeof(b), complexAxis ? "%+.1f .. %+.1f kHz" : "0 .. %.1f kHz", complexAxis ? -rate / 2000.0 : rate / 2000.0, rate / 2000.0);
    d.text(b, r.position.x + r.size.x - d.width(b, 9) - 3, r.position.y + 2, 9, sf::Color(130, 130, 140));
    // impulse response below
    const float y0 = r.position.y + hh + 10, h2 = r.size.y - hh - 14;
    if (!imp.empty()) {
        float mx = 1e-12f;
        for (float v : imp) mx = std::max(mx, std::fabs(v));
        const float mid = y0 + h2 / 2;
        d.rect(sf::FloatRect({r.position.x, mid}, {r.size.x, 1}), sf::Color(45, 45, 52));
        const size_t n = imp.size();
        for (size_t i = 0; i < n; i++) {
            const float x = r.position.x + 4 + (r.size.x - 8) * (n > 1 ? (float)i / (n - 1) : 0.5f);
            const float v = imp[i] / mx * h2 / 2;
            d.rect(sf::FloatRect({x, std::min(mid, mid - v)}, {1.5f, std::max(1.0f, std::fabs(v))}), sf::Color(120, 230, 120));
        }
        d.text("h[n] (" + std::to_string(n) + ")", r.position.x + 2, y0, 9, sf::Color(130, 130, 140));
    }
}
inline std::vector<double> firMag(const std::vector<float>& h, bool complexAxis, int N = 256) {
    std::vector<double> m(N);
    for (int k = 0; k < N; k++) {
        const double w = complexAxis ? PI_D * (2.0 * k / (N - 1) - 1.0) : PI_D * k / (N - 1);
        std::complex<double> s(0, 0);
        for (size_t i = 0; i < h.size(); i++) s += (double)h[i] * std::polar(1.0, -w * (double)i);
        m[k] = std::abs(s);
    }
    return m;
}

// ---------------------------------------------------------------------
//  Sources (their amount of signal follows the pipeline's clock)
// ---------------------------------------------------------------------
struct GenClock {
    double acc = 0.0;
    size_t take(double tick, double rate) {
        acc += std::min(tick, 0.5) * rate;
        const size_t n = (size_t)acc;
        acc -= (double)n;
        return n;
    }
};

inline double waveform(int w, double ph, double duty) {   // ph in cycles [0,1)
    switch (w) {
    case 1: return ph < duty ? 1.0 : -1.0;                        // square / pulse
    case 2: return ph < 0.5 ? 4 * ph - 1 : 3 - 4 * ph;            // triangle
    case 3: return 2 * ph - 1;                                    // saw
    case 4: return 1.0;                                           // constant
    default: return std::sin(2 * PI_D * ph);                      // sine
    }
}

class SignalSourceIQ : public Block {
    GenClock clk; double ph = 0.0;
public:
    SignalSourceIQ() { type = "srciq"; title = "Signal source (I/Q)"; category = "Sources"; setOuts({{"out", PT::IQ}});
                       addNum("rate", "Sample rate", 48000, 100, 10000000, 1000, "S/s");
                       addChoice("w", "Waveform", {"complex tone e^(j2pi f t)", "square (I and Q)", "triangle (I and Q)", "saw (I and Q)", "constant"});
                       addNum("f", "Frequency", 1000, -5000000, 5000000, 100, "Hz");
                       addNum("a", "Amplitude", 1, 0, 1000, 0.1, "", 2);
                       addNum("off", "Offset (I)", 0, -1000, 1000, 0.1, "", 2); }
    void process(const std::vector<const Stream*>&) override {
        Stream& o = out[0]; o.clear(); o.type = PT::IQ; o.rate = num("rate");
        const size_t n = clk.take(tick, o.rate);
        const double a = num("a"), f = num("f") / o.rate, off = num("off");
        const int w = choice("w");
        for (size_t i = 0; i < n; i++) {
            if (w == 0) o.iq.push_back(CF((float)(a * std::cos(2 * PI_D * ph) + off), (float)(a * std::sin(2 * PI_D * ph))));
            else { const float v = (float)(a * waveform(w, ph, 0.5)); o.iq.push_back(CF(v + (float)off, w == 4 ? 0.0f : (float)(a * waveform(w, std::fmod(ph + 0.25, 1.0), 0.5)))); }
            ph = std::fmod(ph + f + 1.0, 1.0);
        }
    }
};

class SignalSourceReal : public Block {
    GenClock clk; double ph = 0.0;
public:
    SignalSourceReal() { type = "srcreal"; title = "Signal source (real)"; category = "Sources"; setOuts({{"out", PT::Real}});
                         addNum("rate", "Sample rate", 48000, 100, 10000000, 1000, "S/s");
                         addChoice("w", "Waveform", {"sine", "square / pulse", "triangle", "saw", "constant"});
                         addNum("f", "Frequency", 1000, 0, 5000000, 100, "Hz");
                         addNum("a", "Amplitude", 1, 0, 1000, 0.1, "", 2);
                         addNum("off", "Offset", 0, -1000, 1000, 0.1, "", 2);
                         addNum("duty", "Duty (square)", 0.5, 0.01, 0.99, 0.05, "", 2);
                         showOnly("duty", "w", {1}); }
    void process(const std::vector<const Stream*>&) override {
        Stream& o = out[0]; o.clear(); o.type = PT::Real; o.rate = num("rate");
        const size_t n = clk.take(tick, o.rate);
        const double a = num("a"), f = num("f") / o.rate, off = num("off"), duty = num("duty");
        const int w = choice("w");
        for (size_t i = 0; i < n; i++) { o.re.push_back((float)(a * waveform(w, ph, duty) + off)); ph = std::fmod(ph + f, 1.0); }
    }
};

class NoiseSource : public Block {
    GenClock clk; std::mt19937 rng {12345};
    bool iq;
public:
    explicit NoiseSource(bool complexOut) : iq(complexOut) {
        type = iq ? "noiseiq" : "noisereal"; title = iq ? "Noise source (I/Q)" : "Noise source (real)"; category = "Sources";
        setOuts({{"out", iq ? PT::IQ : PT::Real}});
        addNum("rate", "Sample rate", 48000, 100, 10000000, 1000, "S/s");
        addChoice("d", "Distribution", {"Gaussian", "uniform"});
        addNum("a", "RMS level", 0.1, 0, 1000, 0.01, "", 3);
    }
    void process(const std::vector<const Stream*>&) override {
        Stream& o = out[0]; o.clear(); o.type = iq ? PT::IQ : PT::Real; o.rate = num("rate");
        const size_t n = clk.take(tick, o.rate);
        const float a = (float)num("a");
        std::normal_distribution<float> g(0.0f, 1.0f);
        std::uniform_real_distribution<float> u(-1.7320508f, 1.7320508f);   // unit RMS
        auto s = [&]() { return choice("d") == 0 ? g(rng) : u(rng); };
        for (size_t i = 0; i < n; i++) {
            if (iq) o.iq.push_back(CF(s() * a * 0.70710678f, s() * a * 0.70710678f)); else o.re.push_back(s() * a);
        }
    }
};

// Random bits, PRBS (PN9 / PN15 / PN23) or 0101...
class BitSource : public Block {
    GenClock clk; std::mt19937 rng {777};
    uint32_t sr = 0x7FFFFF; int bitPos = 0;
public:
    BitSource() { type = "bitsrc"; title = "Bit source (random / PRBS)"; category = "Sources"; setOuts({{"bits", PT::Sym}});
                  addNum("rate", "Bit rate", 1200, 1, 10000000, 100, "bit/s");
                  addChoice("k", "Bits", {"random", "PN9 (x9+x5+1)", "PN15 (x15+x14+1)", "PN23 (x23+x18+1)", "alternating 0101"}); }
    void process(const std::vector<const Stream*>&) override {
        Stream& o = out[0]; o.clear(); o.type = PT::Sym; o.levels = 2; o.rate = num("rate");
        const size_t n = clk.take(tick, o.rate);
        const int k = choice("k");
        for (size_t i = 0; i < n; i++) {
            uint8_t b;
            switch (k) {
            case 1: { const uint32_t nb = ((sr >> 8) ^ (sr >> 4)) & 1; sr = ((sr << 1) | nb) & 0x1FF; b = (uint8_t)nb; break; }
            case 2: { const uint32_t nb = ((sr >> 14) ^ (sr >> 13)) & 1; sr = ((sr << 1) | nb) & 0x7FFF; b = (uint8_t)nb; break; }
            case 3: { const uint32_t nb = ((sr >> 22) ^ (sr >> 17)) & 1; sr = ((sr << 1) | nb) & 0x7FFFFF; b = (uint8_t)nb; break; }
            case 4: b = (uint8_t)(bitPos++ & 1); break;
            default: b = (uint8_t)(rng() & 1);
            }
            o.sym.push_back(b);
        }
        if (sr == 0) sr = 1;
    }
};

// A fixed bit pattern (bits, 0x.. hex, or "text") sent over and over, each a frame
class PatternSource : public Block {
    GenClock clk; size_t pos = 0; std::vector<uint8_t> bits; int seen = -1;
public:
    PatternSource() { type = "patsrc"; title = "Pattern source"; category = "Sources"; setOuts({{"bits", PT::Sym}});
                      addText("p", "Pattern (bits, 0x.., or \"text\")", "0x7E");
                      addNum("rate", "Bit rate", 1200, 1, 10000000, 100, "bit/s");
                      addChoice("mark", "Frame start marks", {"no", "yes, at every repeat"}); }
    void process(const std::vector<const Stream*>&) override {
        Stream& o = out[0]; o.clear(); o.type = PT::Sym; o.levels = 2; o.rate = num("rate");
        if (seen != paramVersion) {
            seen = paramVersion; pos = 0; bits.clear();
            const std::string t = text("p");
            if (t.size() >= 2 && t.front() == '"' && t.back() == '"') { for (size_t i = 1; i + 1 < t.size(); i++) for (int k = 7; k >= 0; k--) bits.push_back(((uint8_t)t[i] >> k) & 1); }
            else bits = parsePattern(t);
        }
        const size_t n = clk.take(tick, o.rate);
        if (bits.empty()) return;
        for (size_t i = 0; i < n; i++) {
            if (pos == 0 && choice("mark") == 1) o.sym.push_back(FRAME);
            o.sym.push_back(bits[pos]);
            pos = (pos + 1) % bits.size();
        }
    }
};

// A text message as a frame of bytes every few seconds (to feed framers, CRC, ...)
class MessageSource : public Block {
    double t = 0.0; long long count = 0;
public:
    MessageSource() { type = "msgsrc"; title = "Message source"; category = "Sources"; setOuts({{"frames", PT::Frames}});
                      addText("m", "Text (%n = counter)", "Hello from OrbitSDR %n");
                      addNum("every", "Every", 1.0, 0.01, 3600, 0.5, "s", 2); }
    void process(const std::vector<const Stream*>&) override {
        Stream& o = out[0]; o.clear(); o.type = PT::Frames; o.rate = 0;
        t += tick;
        if (t < num("every")) return;
        t = 0.0;
        std::string s = text("m");
        const size_t k = s.find("%n");
        if (k != std::string::npos) s.replace(k, 2, std::to_string(++count));
        o.frames.emplace_back(s.begin(), s.end());
    }
};

// ---------------------------------------------------------------------
//  Math (I/Q or real unless said otherwise)
// ---------------------------------------------------------------------
class ArithBlock : public Block {
    int op;   // 0 add, 1 subtract, 2 multiply, 3 multiply by conj(b)
public:
    explicit ArithBlock(int o) : op(o) {
        static const char* t[] = {"add", "sub", "mul", "mulconj"};
        static const char* ti[] = {"Add  (a + b)", "Subtract  (a - b)", "Multiply  (a x b)", "Multiply conjugate  (a x b*)"};
        type = t[op]; title = ti[op]; category = "Math";
        ins = {{"a", op == 3 ? PT::IQ : PT::Any}, {"b", op == 3 ? PT::IQ : PT::Any}};
        setOuts({{"out", op == 3 ? PT::IQ : PT::Any}});
        addNum("k", "Times", 1, -1e6, 1e6, 0.5, "", 3);
        syncInputs = true;   // a and b line up (same length; b at a's rate)
    }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        if (!in[0]) { o.clear(); return; }
        passOn(in[0], o, in[0]->type, in[0]->rate);
        const float k = (float)num("k");
        const Stream* b = in[1];
        // b missing: a + 0, a x 1
        const CF unit = op >= 2 ? CF(1, 0) : CF(0, 0);
        auto apply = [&](CF a, CF y) { return (op == 0 ? a + y : op == 1 ? a - y : op == 2 ? a * y : a * std::conj(y)) * k; };
        if (in[0]->type == PT::IQ) {
            const auto& A = in[0]->iq;
            o.iq.resize(A.size());
            if (!b) for (size_t i = 0; i < A.size(); i++) o.iq[i] = apply(A[i], unit);
            else if (b->type == PT::IQ) for (size_t i = 0; i < A.size(); i++) o.iq[i] = apply(A[i], i < b->iq.size() ? b->iq[i] : unit);
            else for (size_t i = 0; i < A.size(); i++) o.iq[i] = apply(A[i], i < b->re.size() ? CF(b->re[i], 0) : unit);
        } else {
            if (b && b->type == PT::IQ) warning = "b is I/Q but a is real: put the I/Q signal on a";
            const auto& A = in[0]->re;
            const float u = unit.real();
            const bool haveB = b && b->type == PT::Real;
            o.re.resize(A.size());
            for (size_t i = 0; i < A.size(); i++) {
                const float y = haveB && i < b->re.size() ? b->re[i] : u;
                o.re[i] = (op == 0 ? A[i] + y : op == 1 ? A[i] - y : A[i] * y) * k;
            }
        }
    }
};

class ConstBlock : public Block {
public:
    ConstBlock() { type = "affine"; title = "Multiply / add constant"; category = "Math"; ins = {{"in", PT::Any}}; setOuts({{"out", PT::Any}});
                   addNum("k", "Multiply by", 1, -1e9, 1e9, 0.1, "", 4);
                   addNum("c", "Then add", 0, -1e9, 1e9, 0.1, "", 4); }
    void process(const std::vector<const Stream*>& in) override {
        const float k = (float)num("k"), c = (float)num("c");
        mapAny(in[0], out[0], [&](CF v) { return v * k + CF(c, 0); }, [&](float v) { return v * k + c; });
    }
};

class MagBlock : public Block {
public:
    MagBlock() { type = "mag"; title = "Magnitude / power / dB"; category = "Math"; ins = {{"in", PT::Any}}; setOuts({{"out", PT::Real}});
                 addChoice("m", "Gives", {"|x|", "|x|^2 (power)", "10 log10 |x|^2 (dB)", "20 log10 |x| (dB of a real value)"}); }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        passOn(in[0], o, PT::Real, in[0] ? in[0]->rate : 0);
        if (!in[0]) return;
        const int m = choice("m");
        auto f = [&](double p) { return m == 0 ? std::sqrt(p) : m == 1 ? p : 10 * std::log10(p + 1e-30); };
        if (in[0]->type == PT::IQ) for (CF v : in[0]->iq) o.re.push_back((float)f(std::norm(v)));
        else for (float v : in[0]->re) o.re.push_back((float)f((double)v * v));
    }
};

// I/Q -> Re, Im, |x|, arg
class ComplexToReal : public Block {
public:
    ComplexToReal() { type = "c2r"; title = "Complex to real parts"; category = "Math"; ins = {{"in", PT::IQ}};
                      setOuts({{"I (Re)", PT::Real}, {"Q (Im)", PT::Real}, {"|x|", PT::Real}, {"arg (rad)", PT::Real}}); }
    void process(const std::vector<const Stream*>& in) override {
        for (int k = 0; k < 4; k++) passOn(in[0], out[k], PT::Real, in[0] ? in[0]->rate : 0);
        if (!in[0]) return;
        for (CF v : in[0]->iq) { out[0].re.push_back(v.real()); out[1].re.push_back(v.imag()); out[2].re.push_back(std::abs(v)); out[3].re.push_back(std::arg(v)); }
    }
};

// Re (+ Im) -> I/Q
class RealToComplex : public Block {
public:
    RealToComplex() { type = "r2c"; title = "Real to complex"; category = "Math"; ins = {{"I (Re)", PT::Real}, {"Q (Im)", PT::Real}}; setOuts({{"out", PT::IQ}}); syncInputs = true;
                      addChoice("m", "From", {"Re and Im", "magnitude and phase (rad)"}); }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        passOn(in[0], o, PT::IQ, in[0] ? in[0]->rate : 0);
        if (!in[0]) return;
        const bool polar = choice("m") == 1;
        for (size_t i = 0; i < in[0]->re.size(); i++) {
            const float a = in[0]->re[i], b = in[1] && i < in[1]->re.size() ? in[1]->re[i] : 0.0f;
            o.iq.push_back(polar ? std::polar(a, b) : CF(a, b));
        }
    }
};

class IqSwap : public Block {
public:
    IqSwap() { type = "iqswap"; title = "I/Q swap / invert"; category = "Math"; ins = {{"in", PT::IQ}}; setOuts({{"out", PT::IQ}});
               addChoice("m", "Do", {"swap I and Q", "invert Q (conjugate: spectrum mirrored)", "invert I", "invert both (180 deg)", "rotate +90 deg (x j)"}); }
    void process(const std::vector<const Stream*>& in) override {
        const int m = choice("m");
        mapAny(in[0], out[0], [&](CF v) {
            switch (m) { case 0: return CF(v.imag(), v.real()); case 1: return std::conj(v); case 2: return CF(-v.real(), v.imag());
                         case 3: return -v; default: return CF(-v.imag(), v.real()); } }, [](float v) { return v; });
    }
};

// Delay by n samples (z^-n). Also what makes a feedback loop possible: a loop must pass through a
// Delay; the loop is then worked out n samples at a time (n = 1: sample by sample, exact).
class DelayBlock : public Block {
    std::deque<CF> dc; std::deque<float> dr;
public:
    DelayBlock() { type = "delay"; title = "Delay  (z^-n, feedback)"; category = "Math"; ins = {{"in", PT::Any}}; setOuts({{"out", PT::Any}});
                   addNum("n", "Delay", 1, 0, 10000000, 1, "samples"); isDelay = true; }
    void reset() override { dc.clear(); dr.clear(); }
    size_t loopDelay() const override { return std::max<size_t>(1, (size_t)num("n")); }
    void loopEmit(size_t n, PT t, double rate) override {
        const size_t D = loopDelay();
        Stream& o = out[0];
        o.clear(); o.type = t; o.rate = rate;
        if (t == PT::IQ) { while (dc.size() < D) dc.push_front(CF(0, 0)); for (size_t i = 0; i < n && !dc.empty(); i++) { o.iq.push_back(dc.front()); dc.pop_front(); } }
        else { o.type = PT::Real; while (dr.size() < D) dr.push_front(0.0f); for (size_t i = 0; i < n && !dr.empty(); i++) { o.re.push_back(dr.front()); dr.pop_front(); } }
    }
    void loopAbsorb(const Stream* in, size_t n) override {
        const size_t D = loopDelay();
        if (in && in->type == PT::IQ) { dc.insert(dc.end(), in->iq.begin(), in->iq.end()); while (dc.size() > D) dc.pop_front(); }
        else if (in && in->type == PT::Real) { dr.insert(dr.end(), in->re.begin(), in->re.end()); while (dr.size() > D) dr.pop_front(); }
        else { for (size_t i = 0; i < n; i++) { dr.push_back(0.0f); dc.push_back(CF(0, 0)); } while (dr.size() > D) dr.pop_front(); while (dc.size() > D) dc.pop_front(); }
    }
    void process(const std::vector<const Stream*>& in) override {
        const size_t n = (size_t)num("n");
        mapAny(in[0], out[0], [&](CF v) { dc.push_back(v); if (dc.size() <= n) return CF(0, 0); const CF y = dc.front(); dc.pop_front(); return y; },
                              [&](float v) { dr.push_back(v); if (dr.size() <= n) return 0.0f; const float y = dr.front(); dr.pop_front(); return y; });
    }
};

class MovingAverage : public Block {
    std::deque<CF> hc; std::deque<float> hr; CF sc {0, 0}; double sr = 0;
public:
    MovingAverage() { type = "movavg"; title = "Moving average"; category = "Math"; ins = {{"in", PT::Any}}; setOuts({{"out", PT::Any}});
                      addNum("n", "Length", 8, 1, 1000000, 1, "samples"); }
    void process(const std::vector<const Stream*>& in) override {
        const size_t n = std::max<size_t>(1, (size_t)num("n"));
        mapAny(in[0], out[0], [&](CF v) { hc.push_back(v); sc += v; while (hc.size() > n) { sc -= hc.front(); hc.pop_front(); } return sc / (float)hc.size(); },
                              [&](float v) { hr.push_back(v); sr += v; while (hr.size() > n) { sr -= hr.front(); hr.pop_front(); } return (float)(sr / hr.size()); });
    }
};

// Sum of N samples, one output per N (matched filter for rectangular pulses)
class IntegrateDump : public Block {
    CF ac {0, 0}; double ar = 0; size_t k = 0;
public:
    IntegrateDump() { type = "intdump"; title = "Integrate and dump"; category = "Math"; ins = {{"in", PT::Any}}; setOuts({{"out", PT::Any}});
                      addNum("n", "Samples", 8, 1, 1000000, 1, ""); }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        const size_t n = std::max<size_t>(1, (size_t)num("n"));
        if (!in[0]) { o.clear(); return; }
        passOn(in[0], o, in[0]->type, in[0]->rate / n);
        if (in[0]->type == PT::IQ) for (CF v : in[0]->iq) { ac += v; if (++k >= n) { o.iq.push_back(ac / (float)n); ac = CF(0, 0); k = 0; } }
        else for (float v : in[0]->re) { ar += v; if (++k >= n) { o.re.push_back((float)(ar / n)); ar = 0; k = 0; } }
    }
};

class Differentiate : public Block {
    CF pc {0, 0}; float pr = 0;
public:
    Differentiate() { type = "diffr"; title = "Differentiate (x[n] - x[n-1])"; category = "Math"; ins = {{"in", PT::Any}}; setOuts({{"out", PT::Any}}); }
    void process(const std::vector<const Stream*>& in) override {
        mapAny(in[0], out[0], [&](CF v) { const CF y = v - pc; pc = v; return y; }, [&](float v) { const float y = v - pr; pr = v; return y; });
    }
};

class Accumulate : public Block {
    CF ac {0, 0}; double ar = 0;
public:
    Accumulate() { type = "accum"; title = "Accumulate (running sum)"; category = "Math"; ins = {{"in", PT::Any}}; setOuts({{"out", PT::Any}});
                   addNum("leak", "Leak per sample", 0, 0, 1, 0.001, "", 4); }
    void process(const std::vector<const Stream*>& in) override {
        const float l = 1.0f - (float)num("leak");
        mapAny(in[0], out[0], [&](CF v) { ac = ac * l + v; return ac; }, [&](float v) { ar = ar * l + v; return (float)ar; });
    }
};

class ClipBlock : public Block {
public:
    ClipBlock() { type = "clip"; title = "Clip / limiter"; category = "Math"; ins = {{"in", PT::Any}}; setOuts({{"out", PT::Any}});
                  addNum("lim", "Limit", 1, 0, 1e9, 0.1, "", 3);
                  addChoice("m", "Mode", {"clip at +-limit", "hard limit (sign x limit)"}); }
    void process(const std::vector<const Stream*>& in) override {
        const float L = (float)num("lim");
        const bool hard = choice("m") == 1;
        mapAny(in[0], out[0], [&](CF v) { const float a = std::abs(v); return hard ? (a > 0 ? v / a * L : v) : (a > L ? v / a * L : v); },
                              [&](float v) { return hard ? (v >= 0 ? L : -L) : std::clamp(v, -L, L); });
    }
};

// ---------------------------------------------------------------------
//  Filters
// ---------------------------------------------------------------------

// FIR with own taps = convolution with h[n] (reverse them: correlation / matched filter)
class FirTaps : public Block {
    AnyFir f; double rate = 48000;
public:
    FirTaps() { type = "fir"; title = "FIR filter (own taps) / convolution"; category = "Filters"; ins = {{"in", PT::Any}}; setOuts({{"out", PT::Any}});
                addText("h", "Taps h[n]", "0.25, 0.5, 0.25");
                addChoice("rev", "Taps", {"as written (convolution)", "reversed (correlation / matched)"}); }
    sf::Vector2f viewSize() const override { return {300.0f, 150.0f}; }
    std::string status() const override { return std::to_string(f.h.size()) + " taps"; }
    void process(const std::vector<const Stream*>& in) override {
        if (in[0] && in[0]->rate > 0) rate = in[0]->rate;
        if (changed(1.0)) {
            const auto v = parseNumbers(text("h"));
            std::vector<float> h(v.begin(), v.end());
            if (choice("rev") == 1) std::reverse(h.begin(), h.end());
            f.set(h);
        }
        mapAny(in[0], out[0], [&](CF v) { return f.step(v); }, [&](float v) { return f.step(v); });
    }
    void drawView(const Draw& d, const sf::FloatRect& r) override { drawResponse(d, r, firMag(f.h, false), f.h, rate, false); }
};

// FIR design: windowed sinc (LP / HP / BP / BS), root raised cosine, Gaussian
class FirDesign : public Block {
    AnyFir f; double rate = 0, shownRate = 48000; bool complexIn = false;
public:
    FirDesign() { type = "firdesign"; title = "FIR filter design"; category = "Filters"; ins = {{"in", PT::Any}}; setOuts({{"out", PT::Any}});
                  addChoice("t", "Type", {"low-pass", "high-pass", "band-pass", "band-stop", "root raised cosine", "Gaussian"});
                  addNum("f1", "Cutoff / low edge", 3000, 0, 10000000, 100, "Hz");
                  addNum("f2", "High edge (band)", 6000, 0, 10000000, 100, "Hz");
                  addNum("n", "Taps", 63, 3, 4095, 2, "");
                  addChoice("win", "Window", {"rectangular", "Hamming", "Hann", "Blackman", "Kaiser"}, 1);
                  addNum("beta", "Kaiser beta", 6, 0, 20, 0.5, "", 1);
                  addNum("baud", "Symbol rate (RRC / Gauss)", 4800, 1, 10000000, 100, "Bd");
                  addNum("ab", "Roll-off / BT", 0.35, 0.05, 1.0, 0.05, "", 2);
                  showOnly("f1", "t", {0, 1, 2, 3}); showOnly("f2", "t", {2, 3}); showOnly("win", "t", {0, 1, 2, 3});
                  showOnly("beta", "win", {4}); showOnly("baud", "t", {4, 5}); showOnly("ab", "t", {4, 5}); }
    sf::Vector2f viewSize() const override { return {300.0f, 150.0f}; }
    std::string status() const override { return rate > 0 ? std::to_string(f.h.size()) + " taps at " + std::to_string((int)rate) + " S/s" : "no input yet (shown at 48 kS/s)"; }
    static std::vector<float> design(int t, double fs, double f1, double f2, int n, int win, double beta, double baud, double ab) {
        n |= 1;
        std::vector<double> h(n, 0.0);
        const auto w = makeWindowFn(n, win, beta);
        auto lp = [&](double fc) {   // ideal low-pass, unit DC gain after the window
            std::vector<double> v(n);
            double s = 0;
            for (int i = 0; i < n; i++) { const int m = i - n / 2; const double c = fc / fs;
                v[i] = (m == 0 ? 2 * c : std::sin(2 * PI_D * c * m) / (PI_D * m)) * w[i]; s += v[i]; }
            for (auto& x : v) x /= (s != 0 ? s : 1);
            return v;
        };
        switch (t) {
        case 0: h = lp(f1); break;
        case 1: h = lp(f1); for (auto& x : h) x = -x; h[n / 2] += 1.0; break;
        case 2: { auto a = lp(f2), b = lp(f1); for (int i = 0; i < n; i++) h[i] = a[i] - b[i]; break; }
        case 3: { auto a = lp(f2), b = lp(f1); for (int i = 0; i < n; i++) h[i] = -(a[i] - b[i]); h[n / 2] += 1.0; break; }
        case 4: { const double sps = fs / baud; auto r = RrcBlock::design(fs, baud, ab, std::max(2, (int)std::lround(n / sps)));
                  return r; }
        default: {   // Gaussian (GMSK): sigma = sqrt(ln 2) / (2 pi BT) symbols
            const double sps = fs / baud, sig = std::sqrt(std::log(2.0)) / (2 * PI_D * ab) * sps;
            double s = 0;
            for (int i = 0; i < n; i++) { const double m = i - n / 2; h[i] = std::exp(-m * m / (2 * sig * sig)); s += h[i]; }
            for (auto& x : h) x /= s;
        }
        }
        return std::vector<float>(h.begin(), h.end());
    }
    void process(const std::vector<const Stream*>& in) override {
        const double fs = in[0] && in[0]->rate > 0 ? in[0]->rate : 0.0;
        if (fs > 0) { complexIn = in[0]->type == PT::IQ; shownRate = fs; }
        if (changed(fs) || f.h.empty()) {
            rate = fs;
            f.set(design(choice("t"), fs > 0 ? fs : 48000, num("f1"), num("f2"), (int)num("n"), choice("win"), num("beta"), num("baud"), num("ab")));
        }
        mapAny(in[0], out[0], [&](CF v) { return f.step(v); }, [&](float v) { return f.step(v); });
    }
    void drawView(const Draw& d, const sf::FloatRect& r) override { drawResponse(d, r, firMag(f.h, complexIn), f.h, shownRate, complexIn); }
};

// IIR: biquads (RBJ) / Butterworth of any order / own coefficients; direct form II transposed
struct Biquad { double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0; };
class IirBlock : public Block {
    std::vector<Biquad> sec;
    std::vector<double> bc, ac;       // own coefficients
    std::vector<std::array<CF, 2>> zc; std::vector<std::array<double, 2>> zr;
    std::vector<CF> zcc; std::vector<double> zrc;
    double rate = 0, shownRate = 48000;
    std::vector<double> mag; std::vector<float> imp;
    bool custom() const { return choice("t") == 7; }
    void build(double fs) {
        sec.clear();
        const int t = choice("t");
        const double f0 = std::clamp(num("f"), 1.0, fs * 0.49), Q = num("q"), A = std::pow(10.0, num("g") / 40.0);
        const double w0 = 2 * PI_D * f0 / fs, cw = std::cos(w0), sw = std::sin(w0);
        auto rbj = [&](int kind, double q) {
            const double al = sw / (2 * q);
            double b0, b1, b2, a0, a1, a2;
            switch (kind) {
            case 0: b0 = (1 - cw) / 2; b1 = 1 - cw; b2 = b0; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; break;          // LP
            case 1: b0 = (1 + cw) / 2; b1 = -(1 + cw); b2 = b0; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; break;       // HP
            case 2: b0 = al; b1 = 0; b2 = -al; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; break;                         // BP 0 dB
            case 3: b0 = 1; b1 = -2 * cw; b2 = 1; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; break;                      // notch
            case 4: b0 = 1 + al * A; b1 = -2 * cw; b2 = 1 - al * A; a0 = 1 + al / A; a1 = -2 * cw; a2 = 1 - al / A; break;   // peak
            default: b0 = 1 - al; b1 = -2 * cw; b2 = 1 + al; a0 = 1 + al; a1 = -2 * cw; a2 = 1 - al; break;          // all-pass
            }
            sec.push_back({b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0});
        };
        if (t <= 5) rbj(t, Q);
        else if (t == 6) {
            // Butterworth: pairs of poles as biquads (Q from the pole angles), odd order: one first-order section
            const int N = std::max(1, (int)num("ord"));
            const bool hp = choice("bt") == 1;
            for (int k = 0; k < N / 2; k++) rbj(hp ? 1 : 0, 1.0 / (2.0 * std::sin((2 * k + 1) * PI_D / (2.0 * N))));
            if (N & 1) {
                const double K = std::tan(PI_D * f0 / fs);
                if (!hp) sec.push_back({K / (1 + K), K / (1 + K), 0, (K - 1) / (K + 1), 0});
                else sec.push_back({1 / (1 + K), -1 / (1 + K), 0, (K - 1) / (K + 1), 0});
            }
        } else {
            bc = parseNumbers(text("b")); ac = parseNumbers(text("a"));
            if (bc.empty()) bc = {1};
            if (ac.empty() || ac[0] == 0) ac = {1};
            const double a0 = ac[0];
            for (auto& v : bc) v /= a0;
            for (auto& v : ac) v /= a0;
        }
        zc.assign(sec.size(), {CF(0, 0), CF(0, 0)}); zr.assign(sec.size(), {0, 0});
        const size_t m = std::max(bc.size(), ac.size());
        zcc.assign(m, CF(0, 0)); zrc.assign(m, 0.0);
        // response for the view
        mag.assign(256, 0.0);
        for (int k = 0; k < 256; k++) {
            const std::complex<double> z1 = std::polar(1.0, -PI_D * k / 255.0);
            std::complex<double> H(1, 0);
            if (custom()) {
                std::complex<double> nb(0, 0), na(0, 0), zp(1, 0);
                for (size_t i = 0; i < m; i++) { if (i < bc.size()) nb += bc[i] * zp; if (i < ac.size()) na += ac[i] * zp; zp *= z1; }
                H = nb / na;
            } else for (const auto& s : sec) H *= (s.b0 + s.b1 * z1 + s.b2 * z1 * z1) / (1.0 + s.a1 * z1 + s.a2 * z1 * z1);
            mag[k] = std::abs(H);
        }
        imp.clear();
        std::vector<std::array<double, 2>> z(sec.size(), {0, 0});
        std::vector<double> zz(m, 0.0);
        for (int i = 0; i < 64; i++) { double x = i == 0 ? 1.0 : 0.0; imp.push_back((float)stepReal(x, z, zz)); }
    }
    double stepReal(double x, std::vector<std::array<double, 2>>& z, std::vector<double>& zz) const {
        if (custom()) {
            const size_t m = zz.size();
            const double y = (bc.size() ? bc[0] : 0) * x + (m > 1 ? zz[0] : 0);
            for (size_t i = 0; i + 1 < m; i++) zz[i] = (i + 1 < bc.size() ? bc[i + 1] * x : 0) - (i + 1 < ac.size() ? ac[i + 1] * y : 0) + (i + 1 < m - 1 ? zz[i + 1] : 0);
            return y;
        }
        for (size_t k = 0; k < sec.size(); k++) {
            const Biquad& s = sec[k];
            const double y = s.b0 * x + z[k][0];
            z[k][0] = s.b1 * x - s.a1 * y + z[k][1];
            z[k][1] = s.b2 * x - s.a2 * y;
            x = y;
        }
        return x;
    }
    CF stepCplx(CF x) {
        if (custom()) {
            const size_t m = zcc.size();
            const CF y = (float)(bc.size() ? bc[0] : 0) * x + (m > 1 ? zcc[0] : CF(0, 0));
            for (size_t i = 0; i + 1 < m; i++) zcc[i] = (float)(i + 1 < bc.size() ? bc[i + 1] : 0) * x - (float)(i + 1 < ac.size() ? ac[i + 1] : 0) * y + (i + 1 < m - 1 ? zcc[i + 1] : CF(0, 0));
            return y;
        }
        for (size_t k = 0; k < sec.size(); k++) {
            const Biquad& s = sec[k];
            const CF y = (float)s.b0 * x + zc[k][0];
            zc[k][0] = (float)s.b1 * x - (float)s.a1 * y + zc[k][1];
            zc[k][1] = (float)s.b2 * x - (float)s.a2 * y;
            x = y;
        }
        return x;
    }
public:
    IirBlock() { type = "iir"; title = "IIR filter"; category = "Filters"; ins = {{"in", PT::Any}}; setOuts({{"out", PT::Any}});
                 addChoice("t", "Type", {"low-pass (biquad)", "high-pass (biquad)", "band-pass (biquad)", "notch (biquad)", "peak / EQ (biquad)", "all-pass (biquad)", "Butterworth (order N)", "own coefficients a, b"});
                 addNum("f", "Frequency", 1000, 1, 10000000, 50, "Hz");
                 addNum("q", "Q", 0.707, 0.05, 100, 0.05, "", 3);
                 addNum("g", "Gain (peak)", 6, -40, 40, 1, "dB");
                 addNum("ord", "Butterworth order", 4, 1, 16, 1, "");
                 addChoice("bt", "Butterworth kind", {"low-pass", "high-pass"});
                 addText("b", "b (numerator)", "0.2, 0.2");
                 addText("a", "a (denominator)", "1, -0.6");
                 showOnly("f", "t", {0, 1, 2, 3, 4, 5, 6}); showOnly("q", "t", {0, 1, 2, 3, 4, 5}); showOnly("g", "t", {4});
                 showOnly("ord", "t", {6}); showOnly("bt", "t", {6}); showOnly("b", "t", {7}); showOnly("a", "t", {7}); }
    sf::Vector2f viewSize() const override { return {300.0f, 150.0f}; }
    std::string status() const override { return custom() ? "y[n] = sum b x - sum a y" : std::to_string(sec.size()) + " second-order sections"; }
    void process(const std::vector<const Stream*>& in) override {
        const double fs = in[0] && in[0]->rate > 0 ? in[0]->rate : 0.0;
        if (fs > 0) shownRate = fs;
        if (changed(fs) || mag.empty()) { rate = fs; build(fs > 0 ? fs : 48000); }
        mapAny(in[0], out[0], [&](CF v) { return stepCplx(v); }, [&](float v) { return (float)stepReal(v, zr, zrc); });
    }
    void drawView(const Draw& d, const sf::FloatRect& r) override { drawResponse(d, r, mag, imp, shownRate, false); }
};

// Hilbert transform: real -> analytic I/Q (only the positive frequencies)
class HilbertBlock : public Block {
    std::vector<float> h; std::deque<float> hist;
public:
    HilbertBlock() { type = "hilbert"; title = "Hilbert (real -> analytic I/Q)"; category = "Filters"; ins = {{"in", PT::Real}}; setOuts({{"out", PT::IQ}});
                     addNum("n", "Taps", 65, 7, 1023, 2, ""); }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        passOn(in[0], o, PT::IQ, in[0] ? in[0]->rate : 0);
        if (changed(1.0)) {
            const int n = (int)num("n") | 1;
            h.assign(n, 0.0f); hist.assign(n, 0.0f);
            const auto w = makeWindowFn(n, 1, 0);
            for (int i = 0; i < n; i++) { const int m = i - n / 2; h[i] = (m % 2) ? (float)(2.0 / (PI_D * m) * w[i]) : 0.0f; }
        }
        if (!in[0]) return;
        const size_t n = h.size();
        for (float x : in[0]->re) {
            hist.push_back(x); hist.pop_front();
            float q = 0;
            for (size_t k = 0; k < n; k++) q += hist[k] * h[n - 1 - k];
            o.iq.push_back(CF(hist[n / 2], q));   // I delayed to the filter's middle
        }
    }
};

// ---------------------------------------------------------------------
//  Resampling
// ---------------------------------------------------------------------
class Upsample : public Block {
public:
    Upsample() { type = "upsample"; title = "Upsample (x N)"; category = "Resampling"; ins = {{"in", PT::Any}}; setOuts({{"out", PT::Any}});
                 addNum("n", "Factor", 4, 1, 1024, 1, "");
                 addChoice("m", "New samples", {"zeros (then filter)", "repeat (hold)"}); }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        const int n = std::max(1, (int)num("n"));
        if (!in[0]) { o.clear(); return; }
        passOn(in[0], o, in[0]->type, in[0]->rate * n);
        const bool hold = choice("m") == 1;
        if (in[0]->type == PT::IQ) for (CF v : in[0]->iq) for (int k = 0; k < n; k++) o.iq.push_back(k == 0 || hold ? v : CF(0, 0));
        else for (float v : in[0]->re) for (int k = 0; k < n; k++) o.re.push_back(k == 0 || hold ? v : 0.0f);
    }
};

class Downsample : public Block {
    long long k = 0;
public:
    Downsample() { type = "downsample"; title = "Downsample (keep 1 in N)"; category = "Resampling"; ins = {{"in", PT::Any}}; setOuts({{"out", PT::Any}});
                   addNum("n", "Factor", 4, 1, 100000, 1, "");
                   addNum("ph", "Phase", 0, 0, 99999, 1, ""); }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        const long long n = std::max(1LL, (long long)num("n")), ph = (long long)num("ph") % n;
        if (!in[0]) { o.clear(); return; }
        passOn(in[0], o, in[0]->type, in[0]->rate / n);
        if (in[0]->type == PT::IQ) for (CF v : in[0]->iq) { if (k % n == ph) o.iq.push_back(v); k++; }
        else for (float v : in[0]->re) { if (k % n == ph) o.re.push_back(v); k++; }
    }
};

// ---------------------------------------------------------------------
//  Synchronisation / decisions
// ---------------------------------------------------------------------

// Symbol timing for a real signal (crossings of its centre): one value per symbol
class SymbolSyncReal : public Block {
    double t = 0, sps = 1, center = 0, hi = 0, lo = 0;
    float prev = 0; bool have = false;
public:
    SymbolSyncReal() { type = "symsync"; title = "Symbol sync (real)"; category = "Sync"; ins = {{"in", PT::Real}}; setOuts({{"symbols", PT::Real}});
                       addNum("baud", "Symbol rate", 1200, 1, 10000000, 100, "Bd");
                       addNum("gain", "Loop gain", 0.1, 0, 1, 0.01, "", 3); }
    std::string status() const override { char b[48]; std::snprintf(b, sizeof(b), "%.2f samples/symbol", sps); return b; }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        passOn(in[0], o, PT::Real, num("baud"));
        if (!in[0] || in[0]->rate <= 0) return;
        sps = in[0]->rate / std::max(1.0, num("baud"));
        if (sps < 1.5) { warning = "fewer than 1.5 samples per symbol"; return; }
        const double g = num("gain");
        for (float y : in[0]->re) {
            if (!have) { hi = lo = center = y; have = true; }
            hi += (y > hi ? 0.1 : 0.0005) * (y - hi); lo += (y < lo ? 0.1 : 0.0005) * (y - lo); center = (hi + lo) / 2;
            const double tPrev = t;
            t += 1.0;
            if ((prev - center) * (y - center) < 0 && std::fabs(y - prev) > (hi - lo) * 0.1) {
                double e = tPrev + (center - prev) / (y - prev) - sps * 0.5;
                if (e > sps * 0.5) e -= sps;
                t -= std::clamp(g * e, -0.2 * sps, 0.2 * sps);
            }
            if (t >= sps) { t -= sps; o.re.push_back((float)(y - t * (y - prev))); }
            prev = y;
        }
    }
};

// Decision: real values -> symbols 0..L-1 (thresholds automatic or given)
class SlicerOnly : public Block {
    std::deque<float> win;
public:
    SlicerOnly() { type = "decide"; title = "Slicer (decision)"; category = "Sync"; ins = {{"in", PT::Real}}; setOuts({{"symbols", PT::Sym}});
                   addNum("lv", "Levels", 2, 2, 16, 1, "");
                   addChoice("th", "Thresholds", {"automatic (from the signal)", "given below"});
                   addText("t", "Thresholds (L-1 values)", "0");
                   showOnly("t", "th", {1}); }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        const int L = (int)num("lv");
        passOn(in[0], o, PT::Sym, in[0] ? in[0]->rate : 0);
        o.levels = L;
        if (!in[0]) return;
        std::vector<double> th = parseNumbers(text("t"));
        const bool autoTh = choice("th") == 0;
        for (float v : in[0]->re) {
            if (autoTh) {
                win.push_back(v); if (win.size() > 256) win.pop_front();
                std::vector<float> s(win.begin(), win.end());
                std::sort(s.begin(), s.end());
                const double lo = s[s.size() * 5 / 100], hi = s[std::min(s.size() - 1, s.size() * 95 / 100)];
                th.clear();
                for (int k = 1; k < L; k++) th.push_back(lo + (hi - lo) * k / L);
            }
            int sym = 0;
            for (double t : th) if (v > t) sym++;
            o.sym.push_back((uint8_t)std::min(sym, L - 1));
        }
    }
};

// Comparator with hysteresis: real -> bits
class Threshold : public Block {
    uint8_t state = 0;
public:
    Threshold() { type = "threshold"; title = "Threshold (with hysteresis)"; category = "Sync"; ins = {{"in", PT::Real}}; setOuts({{"bits", PT::Sym}});
                  addNum("hi", "On above", 0.5, -1e9, 1e9, 0.1, "", 3);
                  addNum("lo", "Off below", 0.3, -1e9, 1e9, 0.1, "", 3); }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        passOn(in[0], o, PT::Sym, in[0] ? in[0]->rate : 0);
        o.levels = 2;
        if (!in[0]) return;
        const float h = (float)num("hi"), l = (float)num("lo");
        for (float v : in[0]->re) { if (v > h) state = 1; else if (v < l) state = 0; o.sym.push_back(state); }
    }
};

class PhaseDemod : public Block {
    float prev = 0, offset = 0;
public:
    PhaseDemod() { type = "phase"; title = "Phase demodulator"; category = "Demodulate"; ins = {{"in", PT::IQ}}; setOuts({{"phase (rad)", PT::Real}});
                   addChoice("u", "Phase", {"wrapped (-pi .. pi)", "unwrapped"}); }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        passOn(in[0], o, PT::Real, in[0] ? in[0]->rate : 0);
        if (!in[0]) return;
        const bool unwrap = choice("u") == 1;
        for (CF v : in[0]->iq) {
            float p = std::arg(v);
            if (unwrap) { const float d = p - prev; if (d > PI_D) offset -= 2 * PI_D; else if (d < -PI_D) offset += 2 * PI_D; prev = p; p += offset; }
            o.re.push_back(p);
        }
    }
};

// ---------------------------------------------------------------------
//  Modulation (the transmitter's side: build a signal, test a receiver)
// ---------------------------------------------------------------------

// Bits -> levels: NRZ (0 -> -1, 1 -> +1) or groups of bits -> 2^k levels (4FSK ...)
class BitsToLevels : public Block {
    std::vector<uint8_t> acc;
public:
    BitsToLevels() { type = "levels"; title = "Bits to levels (NRZ, 4-level ...)"; category = "Modulate"; ins = {{"bits", PT::Sym}}; setOuts({{"levels", PT::Real}});
                     addNum("k", "Bits per symbol", 1, 1, 4, 1, "");
                     addChoice("g", "Order", {"natural binary", "Gray"});
                     addChoice("inv", "Polarity", {"1 = high", "1 = low"}); }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        const int k = (int)num("k");
        passOn(in[0], o, PT::Real, in[0] ? in[0]->rate / k : 0);
        if (!in[0]) return;
        const int M = 1 << k;
        for (uint8_t b : in[0]->sym) {
            if (b == FRAME) continue;
            acc.push_back(b);
            if ((int)acc.size() < k) continue;
            int v = 0;
            for (uint8_t x : acc) v = (v << 1) | x;
            acc.clear();
            if (choice("g") == 1) { int g = v, bit = v >> 1; while (bit) { g ^= bit; bit >>= 1; } v = g; }   // Gray -> index
            float lvl = (float)(2 * v - (M - 1));   // -(M-1) .. +(M-1)
            if (choice("inv") == 1) lvl = -lvl;
            o.re.push_back(lvl);
        }
    }
};

// Bits -> constellation points (BPSK, QPSK, 8PSK, 16QAM), Gray mapped
class BitsToSymbols : public Block {
    std::vector<uint8_t> acc;
public:
    BitsToSymbols() { type = "psk"; title = "Bits to PSK / QAM symbols"; category = "Modulate"; ins = {{"bits", PT::Sym}}; setOuts({{"symbols", PT::IQ}});
                      addChoice("m", "Constellation", {"BPSK", "QPSK", "8PSK", "16QAM"}, 1); }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        const int m = choice("m"), k = m == 0 ? 1 : m == 1 ? 2 : m == 2 ? 3 : 4;
        passOn(in[0], o, PT::IQ, in[0] ? in[0]->rate / k : 0);
        if (!in[0]) return;
        static const int gray8[8] = {0, 1, 3, 2, 7, 6, 4, 5};
        for (uint8_t b : in[0]->sym) {
            if (b == FRAME) continue;
            acc.push_back(b);
            if ((int)acc.size() < k) continue;
            CF s;
            if (m == 0) s = CF(acc[0] ? 1.f : -1.f, 0);
            else if (m == 1) s = CF(acc[0] ? 0.7071f : -0.7071f, acc[1] ? 0.7071f : -0.7071f);
            else if (m == 2) { const int v = acc[0] * 4 + acc[1] * 2 + acc[2]; int p = 0; for (int i = 0; i < 8; i++) if (gray8[i] == v) p = i; s = std::polar(1.0f, (float)(2 * PI_D * p / 8)); }
            else { auto lv = [](int a, int b) { const int g = a * 2 + b; return (float)(g == 0 ? -3 : g == 1 ? -1 : g == 3 ? 1 : 3) / 3.162f; };
                   s = CF(lv(acc[0], acc[1]), lv(acc[2], acc[3])); }
            o.iq.push_back(s);
            acc.clear();
        }
    }
};

class FmMod : public Block {
    double ph = 0;
public:
    FmMod() { type = "fmmod"; title = "FM modulator"; category = "Modulate"; ins = {{"in", PT::Real}}; setOuts({{"out", PT::IQ}});
              addNum("dev", "Deviation per unit", 1000, 0, 1e7, 100, "Hz"); }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        passOn(in[0], o, PT::IQ, in[0] ? in[0]->rate : 0);
        if (!in[0] || in[0]->rate <= 0) return;
        const double k = 2 * PI_D * num("dev") / in[0]->rate;
        o.iq.reserve(in[0]->re.size());
        for (float x : in[0]->re) {
            ph += k * x;
            if (ph > PI_D || ph < -PI_D) ph = std::remainder(ph, 2 * PI_D);   // (any step size: also above half a turn)
            o.iq.push_back(std::polar(1.0f, (float)ph));
        }
    }
};

class AmMod : public Block {
public:
    AmMod() { type = "ammod"; title = "AM modulator"; category = "Modulate"; ins = {{"in", PT::Real}}; setOuts({{"out", PT::IQ}});
              addNum("m", "Depth", 0.8, 0, 10, 0.1, "", 2);
              addChoice("c", "Carrier", {"with carrier (1 + m x)", "suppressed (DSB: m x)"}); }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        passOn(in[0], o, PT::IQ, in[0] ? in[0]->rate : 0);
        if (!in[0]) return;
        const float m = (float)num("m"); const bool c = choice("c") == 0;
        for (float x : in[0]->re) o.iq.push_back(CF((c ? 1.0f : 0.0f) + m * x, 0));
    }
};

class ConvEncoder : public Block {
    unsigned st = 0;
public:
    ConvEncoder() { type = "convenc"; title = "Convolutional encoder (K=7, 1/2)"; category = "Modulate"; ins = {{"bits", PT::Sym}}; setOuts({{"coded bits", PT::Sym}});
                    addChoice("poly", "Code", {"0x4F, 0x6D (CCSDS / Meteor)", "0x6D, 0x4F (swapped)"}); }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        passOn(in[0], o, PT::Sym, in[0] ? in[0]->rate * 2 : 0); o.levels = 2;
        if (!in[0]) return;
        ConvCode c; if (choice("poly") == 1) { c.pa = 0x6D; c.pb = 0x4F; }
        for (uint8_t b : in[0]->sym) {
            if (b == FRAME) { o.sym.push_back(FRAME); continue; }
            const unsigned r = ((st << 1) | b) & 0x7F; st = r & 0x3F;
            o.sym.push_back((uint8_t)c.out0(r)); o.sym.push_back((uint8_t)c.out1(r));
        }
    }
};

// Frames -> HDLC bits: flags, CRC-16 (X.25) appended, bit stuffing, LSB first
class HdlcFramer : public Block {
public:
    HdlcFramer() { type = "hdlcframer"; title = "HDLC framer"; category = "Modulate"; ins = {{"frames", PT::Frames}}; setOuts({{"bits", PT::Sym}});
                   addNum("pre", "Flags before", 30, 1, 1000, 1, "");
                   addNum("post", "Flags after", 3, 1, 100, 1, "");
                   addChoice("crc", "CRC-16", {"append", "none (already in the frame)"});
                   addNum("rate", "Bit rate", 1200, 1, 1e7, 100, "bit/s"); }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        o.clear(); o.type = PT::Sym; o.levels = 2; o.rate = num("rate");
        if (!in[0]) return;
        for (auto f : in[0]->frames) {
            if (choice("crc") == 0) { const uint16_t c = HdlcBlock::crc16(f.data(), f.size()); f.push_back((uint8_t)(c & 0xFF)); f.push_back((uint8_t)(c >> 8)); }
            auto flag = [&]() { for (int k = 0; k < 8; k++) o.sym.push_back((0x7E >> k) & 1); };
            for (int i = 0; i < (int)num("pre"); i++) flag();
            int ones = 0;
            for (uint8_t b : f) for (int k = 0; k < 8; k++) {
                const uint8_t v = (b >> k) & 1;
                o.sym.push_back(v);
                if (v) { if (++ones == 5) { o.sym.push_back(0); ones = 0; } } else ones = 0;
            }
            for (int i = 0; i < (int)num("post"); i++) flag();
        }
    }
};

// ---------------------------------------------------------------------
//  Bits
// ---------------------------------------------------------------------

// LFSR scrambler / descrambler, any polynomial: additive (XOR with the sequence, CCSDS style)
// or multiplicative (self-synchronising, G3RUH style)
class LfsrBlock : public Block {
    uint64_t sr = 0; int seen = -1;
public:
    LfsrBlock() { type = "lfsr"; title = "Scrambler / descrambler (LFSR)"; category = "Bits"; ins = {{"bits", PT::Sym}}; setOuts({{"bits", PT::Sym}});
                  addText("poly", "Taps (exponents, e.g. 17 12)", "17 12");
                  addChoice("m", "Kind", {"multiplicative descrambler", "multiplicative scrambler", "additive (XOR sequence)"});
                  addText("seed", "Seed (additive, hex)", "0x1FF");
                  addChoice("rst", "Restart at a frame start", {"no", "yes"});
                  showOnly("seed", "m", {2}); }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        passOn(in[0], o, PT::Sym, in[0] ? in[0]->rate : 0); o.levels = 2;
        const auto taps = parseNumbers(text("poly"));
        int deg = 1; for (double t : taps) deg = std::max(deg, (int)t);
        const uint64_t seed = std::strtoull(text("seed").c_str(), nullptr, 0);
        if (seen != paramVersion) { seen = paramVersion; sr = choice("m") == 2 ? seed : 0; }
        if (!in[0]) return;
        const int m = choice("m");
        auto fb = [&]() { uint64_t x = 0; for (double t : taps) x ^= (sr >> ((int)t - 1)) & 1; return (uint8_t)x; };
        for (uint8_t b : in[0]->sym) {
            if (b == FRAME) { o.sym.push_back(FRAME); if (choice("rst") == 1) sr = m == 2 ? seed : 0; continue; }
            uint8_t y;
            if (m == 0) { y = b ^ fb(); sr = (sr << 1) | b; }            // descramble: register holds received bits
            else if (m == 1) { y = b ^ fb(); sr = (sr << 1) | y; }       // scramble: register holds sent bits
            else { const uint8_t p = fb(); y = b ^ (uint8_t)((sr >> (deg - 1)) & 1); sr = (sr << 1) | p; }
            sr &= (deg >= 64) ? ~0ULL : ((1ULL << deg) - 1);
            o.sym.push_back(y);
        }
    }
};

class DiffBits : public Block {
    uint8_t last = 0;
public:
    DiffBits() { type = "diffbits"; title = "Differential (bits)"; category = "Bits"; ins = {{"bits", PT::Sym}}; setOuts({{"bits", PT::Sym}});
                 addChoice("m", "Do", {"decode (y = x xor previous x)", "encode (y = x xor previous y)"}); }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        passOn(in[0], o, PT::Sym, in[0] ? in[0]->rate : 0); o.levels = 2;
        if (!in[0]) return;
        const bool enc = choice("m") == 1;
        for (uint8_t b : in[0]->sym) {
            if (b == FRAME) { o.sym.push_back(FRAME); continue; }
            const uint8_t y = b ^ last;
            last = enc ? y : b;
            o.sym.push_back(y);
        }
    }
};

class InvertBits : public Block {
public:
    InvertBits() { type = "invbits"; title = "Invert bits"; category = "Bits"; ins = {{"bits", PT::Sym}}; setOuts({{"bits", PT::Sym}}); }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        passOn(in[0], o, PT::Sym, in[0] ? in[0]->rate : 0); o.levels = in[0] ? in[0]->levels : 2;
        if (!in[0]) return;
        for (uint8_t b : in[0]->sym) o.sym.push_back(b == FRAME ? FRAME : (uint8_t)(o.levels - 1 - b));
    }
};

// Frames of bytes -> bits (each frame marked), most significant bit first
class UnpackBytes : public Block {
public:
    UnpackBytes() { type = "unpack"; title = "Bytes to bits (frames)"; category = "Bits"; ins = {{"frames", PT::Frames}}; setOuts({{"bits", PT::Sym}});
                    addChoice("o", "Bit order", {"MSB first", "LSB first"});
                    addNum("rate", "Bit rate", 1200, 1, 1e7, 100, "bit/s"); }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0]; o.clear(); o.type = PT::Sym; o.levels = 2; o.rate = num("rate");
        if (!in[0]) return;
        const bool lsb = choice("o") == 1;
        for (const auto& f : in[0]->frames) { o.sym.push_back(FRAME); for (uint8_t b : f) for (int k = 0; k < 8; k++) o.sym.push_back((b >> (lsb ? k : 7 - k)) & 1); }
    }
};

// CRC (8 / 16 / 32, any polynomial): check (and cut off) or append
class CrcBlock : public Block {
    long long ok = 0, bad = 0;
    static uint32_t refl(uint32_t v, int w) { uint32_t r = 0; for (int i = 0; i < w; i++) if (v & (1u << i)) r |= 1u << (w - 1 - i); return r; }
public:
    CrcBlock() { type = "crc"; title = "CRC check / append"; category = "Frames"; ins = {{"frames", PT::Frames}}; setOuts({{"frames", PT::Frames}});
                 addChoice("p", "CRC", {"CRC-16/X.25 (AX.25, HDLC)", "CRC-16/CCITT-FALSE", "CRC-16/IBM (ARC)", "CRC-32 (Ethernet, zip)", "CRC-8 (0x07)", "own (below)"});
                 addChoice("m", "Do", {"check, drop bad, cut it off", "check, keep all", "append"});
                 addNum("w", "Own: width", 16, 8, 32, 8, "bits");
                 addText("poly", "Own: polynomial (hex)", "0x1021");
                 addText("init", "Own: init (hex)", "0xFFFF");
                 addText("xo", "Own: final XOR (hex)", "0x0000");
                 addChoice("rf", "Own: reflected", {"no", "yes"});
                 for (const char* k : {"w", "poly", "init", "xo", "rf"}) showOnly(k, "p", {5}); }
    std::string status() const override { char b[64]; std::snprintf(b, sizeof(b), "%lld OK, %lld bad", ok, bad); return b; }
    void params(int& w, uint32_t& poly, uint32_t& init, uint32_t& xo, bool& rf) const {
        switch (choice("p")) {
        case 0: w = 16; poly = 0x1021; init = 0xFFFF; xo = 0xFFFF; rf = true; break;
        case 1: w = 16; poly = 0x1021; init = 0xFFFF; xo = 0; rf = false; break;
        case 2: w = 16; poly = 0x8005; init = 0; xo = 0; rf = true; break;
        case 3: w = 32; poly = 0x04C11DB7; init = 0xFFFFFFFF; xo = 0xFFFFFFFF; rf = true; break;
        case 4: w = 8; poly = 0x07; init = 0; xo = 0; rf = false; break;
        default: w = (int)num("w"); poly = (uint32_t)std::strtoul(text("poly").c_str(), nullptr, 0);
                 init = (uint32_t)std::strtoul(text("init").c_str(), nullptr, 0); xo = (uint32_t)std::strtoul(text("xo").c_str(), nullptr, 0); rf = choice("rf") == 1;
        }
    }
    uint32_t compute(const uint8_t* p, size_t n) const {
        int w; uint32_t poly, init, xo; bool rf; params(w, poly, init, xo, rf);
        const uint32_t top = 1u << (w - 1), mask = w == 32 ? 0xFFFFFFFFu : (1u << w) - 1;
        uint32_t c = init;
        for (size_t i = 0; i < n; i++) {
            const uint32_t b = rf ? refl(p[i], 8) : p[i];
            c ^= b << (w - 8);
            for (int k = 0; k < 8; k++) c = (c & top) ? ((c << 1) ^ poly) & mask : (c << 1) & mask;
        }
        if (rf) c = refl(c, w);
        return (c ^ xo) & mask;
    }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0]; o.clear(); o.type = PT::Frames;
        if (!in[0]) return;
        o.rate = in[0]->rate;
        int w; uint32_t poly, init, xo; bool rf; params(w, poly, init, xo, rf);
        const int nb = w / 8;
        for (auto f : in[0]->frames) {
            if (choice("m") == 2) {
                const uint32_t c = compute(f.data(), f.size());
                for (int k = 0; k < nb; k++) f.push_back(rf ? (uint8_t)(c >> (8 * k)) : (uint8_t)(c >> (8 * (nb - 1 - k))));
                o.frames.push_back(f); continue;
            }
            if ((int)f.size() <= nb) { bad++; continue; }
            const uint32_t c = compute(f.data(), f.size() - nb);
            uint32_t got = 0;
            for (int k = 0; k < nb; k++) got |= rf ? (uint32_t)f[f.size() - nb + k] << (8 * k) : (uint32_t)f[f.size() - nb + k] << (8 * (nb - 1 - k));
            const bool good = c == got;
            if (good) ok++; else bad++;
            if (choice("m") == 0) { if (!good) continue; f.resize(f.size() - nb); }
            o.frames.push_back(f);
        }
    }
};

// Part of every frame: bytes from .. to
class SliceFrames : public Block {
public:
    SliceFrames() { type = "slice"; title = "Bytes of a frame (from, count)"; category = "Frames"; ins = {{"frames", PT::Frames}}; setOuts({{"frames", PT::Frames}});
                    addNum("from", "From byte", 0, 0, 1e6, 1, "");
                    addNum("n", "Count (0: to the end)", 0, 0, 1e6, 1, ""); }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0]; o.clear(); o.type = PT::Frames;
        if (!in[0]) return;
        o.rate = in[0]->rate;
        const size_t a = (size_t)num("from"), n = (size_t)num("n");
        for (const auto& f : in[0]->frames) {
            if (a >= f.size()) continue;
            const size_t e = n ? std::min(f.size(), a + n) : f.size();
            o.frames.emplace_back(f.begin() + a, f.begin() + e);
        }
    }
};

// Every byte of a frame XORed with a key (one byte or more, repeated): simple scrambling such as
// an EByte E32's, which XORs its payload with 0x12
class XorBytes : public Block {
public:
    XorBytes() { type = "xorbytes"; title = "XOR bytes with a key"; category = "Frames"; ins = {{"frames", PT::Frames}}; setOuts({{"frames", PT::Frames}});
                 addText("key", "Key (hex bytes)", "0x12"); }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0]; o.clear(); o.type = PT::Frames;
        if (!in[0]) return;
        o.rate = in[0]->rate; o.newBurst = in[0]->newBurst;
        // "0x12", "12", "12 34", "0x1234": the hex digits, two per byte
        std::vector<uint8_t> key;
        std::string t = text("key"), hex;
        if (t.size() >= 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) t = t.substr(2);
        for (char ch : t) if (std::isxdigit((unsigned char)ch)) hex += ch;
        if (hex.size() % 2) hex = "0" + hex;
        for (size_t i = 0; i + 1 < hex.size(); i += 2) key.push_back((uint8_t)std::strtol(hex.substr(i, 2).c_str(), nullptr, 16));
        for (const auto& f : in[0]->frames) {
            std::vector<uint8_t> g = f;
            if (!key.empty()) for (size_t i = 0; i < g.size(); i++) g[i] ^= key[i % key.size()];
            o.frames.push_back(std::move(g));
        }
    }
};

// ---------------------------------------------------------------------
//  Displays
// ---------------------------------------------------------------------

// Frames as text: ASCII, hex, or both (like a terminal)
class TextDisplay : public Block {
    std::deque<std::string> lines; long long n = 0;
public:
    TextDisplay() { type = "text"; title = "Text display"; category = "Displays"; ins = {{"frames", PT::Frames}};
                    addChoice("f", "Show", {"text (ASCII)", "hex", "hex + text"});
                    addChoice("ts", "Time stamps", {"yes", "no"}); }
    sf::Vector2f viewSize() const override { return {480.0f, 220.0f}; }
    std::vector<std::string> buttons() const override { return {"Copy", "Clear"}; }
    std::string status() const override { return std::to_string(n) + " messages"; }
    void onButton(int i) override {
        if (i == 1) { lines.clear(); return; }
        std::string t; for (const auto& l : lines) t += l + "\n";
        sf::Clipboard::setString(sf::String::fromUtf8(t.begin(), t.end()));
    }
    void process(const std::vector<const Stream*>& in) override {
        if (!in[0]) return;
        for (const auto& f : in[0]->frames) {
            std::string s;
            if (choice("ts") == 0) { char t[16]; const std::time_t now = std::time(nullptr); std::strftime(t, sizeof(t), "%H:%M:%S  ", std::localtime(&now)); s = t; }
            const int m = choice("f");
            if (m >= 1) { char b[4]; for (uint8_t c : f) { std::snprintf(b, sizeof(b), "%02X ", c); s += b; } }
            if (m == 2) s += " | ";
            if (m != 1) for (uint8_t c : f) s += (c >= 32 && c < 127) ? (char)c : '.';
            lines.push_back(s);
            while (lines.size() > 1000) lines.pop_front();
            n++;
        }
    }
    void drawView(const Draw& d, const sf::FloatRect& r) override {
        d.rect(r, sf::Color(6, 12, 8), sf::Color(50, 70, 55));
        const float lh = 13.0f;
        const int rows = std::max(1, (int)((r.size.y - 4) / lh));
        float y = r.position.y + 2;
        const size_t from = lines.size() > (size_t)rows ? lines.size() - rows : 0;
        for (size_t k = from; k < lines.size(); k++) {
            std::string t = lines[k];
            while (!t.empty() && d.width(t, 10) > r.size.x - 8) t.pop_back();
            d.text(t, r.position.x + 4, y, 10, sf::Color(140, 255, 160));
            y += lh;
        }
        if (lines.empty()) d.text("waiting for frames", r.position.x + 6, r.position.y + 4, 11, sf::Color(90, 120, 95));
    }
};

// A number: mean, RMS, peak, power in dB, frequency of the samples ...
class ProbeBlock : public Block {
    double val = 0, rate = 0; long long count = 0; double acc = 0, acc2 = 0, pk = 0; size_t n = 0; double tAcc = 0;
public:
    ProbeBlock() { type = "probe"; title = "Number display (probe)"; category = "Displays"; ins = {{"in", PT::Any}};
                   addChoice("m", "Show", {"mean", "RMS", "peak |x|", "power (dB)", "sample rate", "samples per second"});
                   addNum("avg", "Average over", 0.5, 0.01, 60, 0.1, "s", 2); }
    sf::Vector2f viewSize() const override { return {220.0f, 54.0f}; }
    void process(const std::vector<const Stream*>& in) override {
        if (!in[0]) return;
        rate = in[0]->rate;
        const size_t sz = in[0]->size();
        for (size_t i = 0; i < sz; i++) {
            const double v = in[0]->type == PT::IQ ? std::abs(in[0]->iq[i]) : (in[0]->type == PT::Real ? in[0]->re[i] : 0.0);
            acc += v; acc2 += v * v; pk = std::max(pk, std::fabs(v)); n++;
        }
        count += (long long)sz;
        tAcc += tick;
        if (tAcc < num("avg")) return;
        const int m = choice("m");
        val = n == 0 ? 0 : m == 0 ? acc / n : m == 1 ? std::sqrt(acc2 / n) : m == 2 ? pk : m == 3 ? 10 * std::log10(acc2 / n + 1e-30) : m == 4 ? rate : count / tAcc;
        acc = acc2 = pk = 0; n = 0; count = 0; tAcc = 0;
    }
    void drawView(const Draw& d, const sf::FloatRect& r) override {
        d.rect(r, sf::Color(8, 10, 12), sf::Color(50, 50, 58));
        char b[48];
        std::snprintf(b, sizeof(b), std::fabs(val) >= 1e5 || (std::fabs(val) < 1e-3 && val != 0) ? "%.4g" : "%.4f", val);
        d.text(b, r.position.x + 8, r.position.y + 6, 26, sf::Color(255, 200, 90));
    }
};

class WaterfallBlock : public Block {
    std::vector<Complex> buf; std::vector<std::vector<float>> rows; bool iq = true; double rate = 0;
    sf::Texture tex; bool dirty = true; static constexpr size_t N = 512, ROWS = 160;
public:
    WaterfallBlock() { type = "waterfall"; title = "Waterfall"; category = "Displays"; ins = {{"in", PT::Any}};
                       addNum("range", "Range", 60, 10, 140, 5, "dB"); }
    sf::Vector2f viewSize() const override { return {380.0f, 180.0f}; }
    void process(const std::vector<const Stream*>& in) override {
        if (!in[0] || in[0]->rate <= 0) return;
        iq = in[0]->type == PT::IQ; rate = in[0]->rate;
        for (size_t i = 0; i < in[0]->size(); i++) {
            buf.emplace_back(iq ? Complex(in[0]->iq[i].real(), in[0]->iq[i].imag()) : Complex(in[0]->re[i], 0));
            if (buf.size() < N) continue;
            for (size_t k = 0; k < N; k++) buf[k] *= 0.5 - 0.5 * std::cos(2 * PI_D * k / (N - 1));
            fft(buf);
            std::vector<float> row;
            for (size_t k = iq ? 0 : N / 2; k < N; k++) row.push_back((float)(10 * std::log10(std::norm(buf[(k + N / 2) % N]) + 1e-20)));
            rows.push_back(row); if (rows.size() > ROWS) rows.erase(rows.begin());
            buf.clear(); dirty = true;
        }
    }
    void drawView(const Draw& d, const sf::FloatRect& r) override {
        d.rect(r, sf::Color(0, 0, 0), sf::Color(50, 50, 58));
        if (rows.empty()) return;
        if (dirty) {
            const size_t W = rows.back().size();
            float hi = -1e30f; for (const auto& rw : rows) for (float v : rw) hi = std::max(hi, v);
            const float lo = hi - (float)num("range");
            std::vector<uint8_t> px(W * ROWS * 4, 0);
            for (size_t y = 0; y < rows.size(); y++) for (size_t x = 0; x < W && x < rows[y].size(); x++) {
                const float t = std::clamp((rows[y][x] - lo) / (hi - lo), 0.0f, 1.0f);
                uint8_t* p = &px[((ROWS - rows.size() + y) * W + x) * 4];
                p[0] = (uint8_t)(255 * std::clamp(t * 3 - 1.5f, 0.0f, 1.0f)); p[1] = (uint8_t)(255 * std::clamp(t * 3 - 0.8f, 0.0f, 1.0f) * 0.9f);
                p[2] = (uint8_t)(255 * std::clamp(t * 2.0f, 0.0f, 1.0f) * (1 - std::clamp(t * 3 - 2.0f, 0.0f, 1.0f))); p[3] = 255;
            }
            if (tex.resize({(unsigned)W, (unsigned)ROWS})) tex.update(px.data());
            dirty = false;
        }
        sf::Sprite sp(tex);
        sp.setScale({r.size.x / tex.getSize().x, r.size.y / tex.getSize().y});
        sp.setPosition(r.position);
        d.win->draw(sp);
    }
};

class HistogramBlock : public Block {
    std::vector<float> vals;
public:
    HistogramBlock() { type = "histogram"; title = "Histogram"; category = "Displays"; ins = {{"in", PT::Real}};
                       addNum("bins", "Bins", 64, 8, 256, 8, ""); }
    sf::Vector2f viewSize() const override { return {300.0f, 140.0f}; }
    void process(const std::vector<const Stream*>& in) override {
        if (!in[0]) return;
        vals.insert(vals.end(), in[0]->re.begin(), in[0]->re.end());
        if (vals.size() > 20000) vals.erase(vals.begin(), vals.end() - 20000);
    }
    void drawView(const Draw& d, const sf::FloatRect& r) override {
        d.rect(r, sf::Color(8, 10, 12), sf::Color(50, 50, 58));
        if (vals.size() < 10) return;
        std::vector<float> s = vals; std::sort(s.begin(), s.end());
        const float lo = s[s.size() / 200], hi = s[s.size() - 1 - s.size() / 200], span = std::max(1e-9f, hi - lo);
        const int B = (int)num("bins");
        std::vector<int> h(B, 0);
        for (float v : vals) { const int k = (int)((v - lo) / span * B); if (k >= 0 && k < B) h[k]++; }
        const int mx = std::max(1, *std::max_element(h.begin(), h.end()));
        for (int k = 0; k < B; k++) {
            const float bh = (r.size.y - 16) * h[k] / mx;
            d.rect(sf::FloatRect({r.position.x + r.size.x * k / B, r.position.y + r.size.y - 12 - bh}, {std::max(1.0f, r.size.x / B - 1), bh}), sf::Color(255, 150, 40));
        }
        char b[64]; std::snprintf(b, sizeof(b), "%.4g", lo); d.text(b, r.position.x + 2, r.position.y + r.size.y - 12, 9, sf::Color(130, 130, 140));
        std::snprintf(b, sizeof(b), "%.4g", hi); d.text(b, r.position.x + r.size.x - d.width(b, 9) - 2, r.position.y + r.size.y - 12, 9, sf::Color(130, 130, 140));
    }
};

// Eye diagram of a real signal: traces two symbols long
class EyeBlock : public Block {
    std::deque<float> hist; std::vector<std::vector<float>> traces; double pos = 0, sps = 1;
public:
    EyeBlock() { type = "eye"; title = "Eye diagram"; category = "Displays"; ins = {{"in", PT::Real}};
                 addNum("baud", "Symbol rate", 1200, 1, 1e7, 100, "Bd"); }
    sf::Vector2f viewSize() const override { return {300.0f, 150.0f}; }
    void process(const std::vector<const Stream*>& in) override {
        if (!in[0] || in[0]->rate <= 0) return;
        sps = in[0]->rate / std::max(1.0, num("baud"));
        const size_t L = (size_t)std::ceil(2 * sps) + 1;
        for (float v : in[0]->re) {
            hist.push_back(v); if (hist.size() > L) hist.pop_front();
            pos += 1.0;
            if (pos >= sps && hist.size() == L) {
                pos -= sps;
                traces.emplace_back(hist.begin(), hist.end());
                if (traces.size() > 150) traces.erase(traces.begin());
            }
        }
    }
    void drawView(const Draw& d, const sf::FloatRect& r) override {
        d.rect(r, sf::Color(8, 10, 12), sf::Color(50, 50, 58));
        if (traces.empty()) return;
        float lo = 1e30f, hi = -1e30f;
        for (const auto& t : traces) for (float v : t) { lo = std::min(lo, v); hi = std::max(hi, v); }
        const float span = std::max(1e-9f, hi - lo);
        sf::VertexArray va(sf::PrimitiveType::Lines);
        for (const auto& t : traces) for (size_t i = 1; i < t.size(); i++) {
            auto P = [&](size_t k) { return sf::Vector2f(r.position.x + r.size.x * k / (t.size() - 1), r.position.y + 4 + (r.size.y - 8) * (1 - (t[k] - lo) / span)); };
            va.append({P(i - 1), sf::Color(120, 230, 120, 50)}); va.append({P(i), sf::Color(120, 230, 120, 50)});
        }
        d.win->draw(va);
    }
};

// ---------------------------------------------------------------------
//  Sinks: files
// ---------------------------------------------------------------------
inline std::string outPath(const std::string& prefix, const std::string& ext) {
    std::string dir = hooks().recordDir ? hooks().recordDir() : (hooks().imageDir ? hooks().imageDir() : std::string("."));
    char name[64]; const std::time_t now = std::time(nullptr);
    std::strftime(name, sizeof(name), "_%Y%m%d_%H%M%S", std::localtime(&now));
#ifdef _WIN32
    return dir + "\\" + prefix + name + ext;
#else
    return dir + "/" + prefix + name + ext;
#endif
}

// WAV (float 32 bit): real = mono, I/Q = 2 channels (I left, Q right) - opens in OrbitSDR as a recording
class WavSink : public Block {
    std::ofstream f; uint64_t frames = 0; int ch = 0; uint32_t rate = 0; std::string path, msg;
    void header() {
        const uint32_t data = (uint32_t)std::min<uint64_t>(frames * ch * 4, 0xFFFFFFF0ULL);
        f.seekp(0);
        auto w32 = [&](uint32_t v) { f.write((const char*)&v, 4); };
        auto w16 = [&](uint16_t v) { f.write((const char*)&v, 2); };
        f.write("RIFF", 4); w32(36 + data); f.write("WAVEfmt ", 8); w32(16); w16(3); w16((uint16_t)ch); w32(rate);
        w32(rate * ch * 4); w16((uint16_t)(ch * 4)); w16(32); f.write("data", 4); w32(data);
        f.seekp(0, std::ios::end);
    }
    void stop() { if (f.is_open()) { header(); f.close(); msg = "saved " + path; } }
public:
    WavSink() { type = "wavsink"; title = "WAV file (record)"; category = "Sinks"; ins = {{"in", PT::Any}}; }
    ~WavSink() override { stop(); }
    std::vector<std::string> buttons() const override { return {f.is_open() ? "Stop" : "Record"}; }
    std::string status() const override {
        if (f.is_open()) { char b[96]; std::snprintf(b, sizeof(b), "recording %.1f s", rate ? (double)frames / rate : 0.0); return b; }
        return msg.empty() ? "press Record" : msg;
    }
    void onButton(int) override { if (f.is_open()) stop(); else { path.clear(); frames = 0; ch = 0; msg = "waiting for samples"; wantStart = true; } }
    bool wantStart = false;
    void process(const std::vector<const Stream*>& in) override {
        if (!in[0] || in[0]->rate <= 0) return;
        if (wantStart) {
            wantStart = false;
            ch = in[0]->type == PT::IQ ? 2 : 1; rate = (uint32_t)std::lround(in[0]->rate);
            path = outPath(ch == 2 ? "pipeline_iq" : "pipeline_audio", ".wav");
            f.open(path, std::ios::binary);
            if (!f) { msg = "could not write " + path; return; }
            header();
        }
        if (!f.is_open()) return;
        if (in[0]->type == PT::IQ && ch == 2) { f.write((const char*)in[0]->iq.data(), in[0]->iq.size() * 8); frames += in[0]->iq.size(); }
        else if (in[0]->type == PT::Real && ch == 1) { f.write((const char*)in[0]->re.data(), in[0]->re.size() * 4); frames += in[0]->re.size(); }
    }
};

// Bytes to a file: frames as they are, or bits packed 8 to a byte
class ByteSink : public Block {
    std::ofstream f; uint64_t bytes = 0; std::string path, msg; uint8_t acc = 0; int nb = 0;
public:
    ByteSink() { type = "bytesink"; title = "Byte file (record)"; category = "Sinks"; ins = {{"frames", PT::Frames}, {"bits", PT::Sym}}; }
    ~ByteSink() override { if (f.is_open()) f.close(); }
    std::vector<std::string> buttons() const override { return {f.is_open() ? "Stop" : "Record"}; }
    std::string status() const override { return f.is_open() ? "recording, " + std::to_string(bytes) + " bytes" : (msg.empty() ? "press Record" : msg); }
    void onButton(int) override {
        if (f.is_open()) { f.close(); msg = "saved " + path; return; }
        path = outPath("pipeline_bytes", ".bin"); f.open(path, std::ios::binary); bytes = 0;
        if (!f) msg = "could not write " + path;
    }
    void process(const std::vector<const Stream*>& in) override {
        if (!f.is_open()) return;
        if (in[0]) for (const auto& fr : in[0]->frames) { f.write((const char*)fr.data(), fr.size()); bytes += fr.size(); }
        if (in[1]) for (uint8_t b : in[1]->sym) {
            if (b == FRAME) continue;
            acc = (uint8_t)((acc << 1) | (b & 1));
            if (++nb == 8) { f.put((char)acc); bytes++; nb = 0; acc = 0; }
        }
    }
};
