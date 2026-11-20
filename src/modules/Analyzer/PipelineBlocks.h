#pragma once
// =====================================================================
//  Building blocks for decoders (included from Pipeline.h, after the
//  basic blocks). Each block does one job, so decoders are built in the
//  editor from parts, e.g.
//
//   LRPT (Meteor):  AGC -> RRC -> Clock recovery -> Costas -> Soft bits
//                   -> Coded sync -> Viterbi -> Frame sync -> Derandomize
//                   -> Pack bytes -> Reed-Solomon -> CCSDS packets
//                   -> MSU-MR image -> Image view
//   AX.25 1200:     FM -> FSK tones -> Clock+slicer -> Symbols to bits
//                   -> NRZI -> HDLC -> AX.25
//   WEFAX:          FM -> Low-pass -> Raster lines -> Image view
// =====================================================================

// ---------------------------------------------------------------------
//  I/Q conditioning
// ---------------------------------------------------------------------

// Automatic gain: the output's average magnitude is 1
class AgcBlock : public Block {
    double level = 0.0;
public:
    AgcBlock() { type = "agc"; title = "AGC"; category = "I/Q"; ins = {{"in", PT::IQ}}; setOuts({{"out", PT::IQ}});
                 addNum("tc", "Time constant", 20, 0.1, 5000, 1, "ms", 1); }
    std::string status() const override { char b[48]; std::snprintf(b, sizeof(b), "input level %.1f dB", 20.0 * std::log10(level + 1e-12)); return b; }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        passOn(in[0], o, PT::IQ, in[0] ? in[0]->rate : 0);
        if (!in[0] || in[0]->rate <= 0) return;
        const double a = 1.0 / std::max(1.0, num("tc") * 0.001 * in[0]->rate);
        o.iq.reserve(in[0]->iq.size());
        for (CF v : in[0]->iq) {
            const double m = std::abs(v);
            if (level <= 0.0) level = m + 1e-9;
            level += a * (m - level);
            o.iq.push_back(v / (float)std::max(level, 1e-12));
        }
    }
};

// Root raised cosine (matched filter of PSK / QAM)
class RrcBlock : public Block {
    Fir<CF> f;
public:
    RrcBlock() { type = "rrc"; title = "RRC filter"; category = "I/Q"; ins = {{"in", PT::IQ}}; setOuts({{"out", PT::IQ}});
                 addNum("baud", "Symbol rate", 72000, 10, 2000000, 100, "Bd");
                 addNum("alpha", "Roll-off", 0.6, 0.05, 1.0, 0.05, "", 2);
                 addNum("span", "Length", 8, 2, 32, 1, "symbols"); }
    static std::vector<float> design(double fs, double baud, double a, int span) {
        const double sps = fs / baud;
        int n = (int)std::ceil(span * sps) | 1;
        n = std::clamp(n, 7, 1023);
        std::vector<float> h(n);
        double sum = 0.0;
        for (int i = 0; i < n; i++) {
            const double t = (i - n / 2) / sps;   // in symbols
            double v;
            if (std::fabs(t) < 1e-9) v = 1.0 - a + 4.0 * a / PI_D;
            else if (std::fabs(std::fabs(t) - 1.0 / (4.0 * a)) < 1e-9)
                v = a / std::sqrt(2.0) * ((1 + 2 / PI_D) * std::sin(PI_D / (4 * a)) + (1 - 2 / PI_D) * std::cos(PI_D / (4 * a)));
            else
                v = (std::sin(PI_D * t * (1 - a)) + 4 * a * t * std::cos(PI_D * t * (1 + a))) / (PI_D * t * (1 - 16 * a * a * t * t));
            h[i] = (float)v;
            sum += v;
        }
        for (auto& v : h) v = (float)(v / sum);
        return h;
    }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        passOn(in[0], o, PT::IQ, in[0] ? in[0]->rate : 0);
        if (!in[0] || in[0]->rate <= 0) return;
        if (changed(in[0]->rate)) f.set(design(in[0]->rate, num("baud"), num("alpha"), (int)num("span")));
        o.iq.reserve(in[0]->iq.size());
        for (CF v : in[0]->iq) o.iq.push_back(f.step(v));
    }
};

// Symbol timing for I/Q (Gardner, works before the carrier is locked): one output per symbol
class ClockIQ : public Block {
    double t = 0.0, sps = 1.0;
    CF h[4] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}}, prevSym {0, 0}, mid {0, 0};
    double power = 1.0, err = 0.0;
    bool have = false;
    long long syms = 0;
public:
    ClockIQ() { type = "clockiq"; title = "Clock recovery (I/Q)"; category = "Symbols"; ins = {{"in", PT::IQ}}; setOuts({{"symbols", PT::IQ}});
                addNum("baud", "Symbol rate", 72000, 10, 2000000, 100, "Bd");
                addNum("gain", "Loop gain", 0.02, 0.001, 0.2, 0.005, "", 3); }
    void reset() override { t = 0.0; have = false; }
    std::string status() const override { char b[64]; std::snprintf(b, sizeof(b), "%.2f samples/symbol, %lld symbols", sps, syms); return b; }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        passOn(in[0], o, PT::IQ, num("baud"));
        if (!in[0] || in[0]->rate <= 0) return;
        if (changed(in[0]->rate)) reset();
        sps = in[0]->rate / std::max(1.0, num("baud"));
        if (sps < 1.5) return;
        if (in[0]->newBurst) reset();
        const double g = num("gain");
        // The strobe and the mid point fall between two samples. Cubic (Catmull-Rom) interpolation
        // over 4 samples: at 2-3 samples per symbol a straight line between two of them is far off
        // the pulse (ISI). The clock runs one sample late so the point lies between h[1] and h[2].
        auto cubic = [&](double mu) {
            const CF p0 = h[0], p1 = h[1], p2 = h[2], p3 = h[3];
            const float m = (float)mu, m2 = m * m, m3 = m2 * m;
            return 0.5f * (2.0f * p1 + (p2 - p0) * m + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * m2 + (3.0f * p1 - p0 - 3.0f * p2 + p3) * m3);
        };
        for (CF y : in[0]->iq) {
            h[0] = h[1]; h[1] = h[2]; h[2] = h[3]; h[3] = y;
            const double tPrev = t;
            t += 1.0;
            // t counts samples up to h[2]; the moment t = x lies (t - x) samples before h[2]
            if (tPrev < sps * 0.5 && t >= sps * 0.5) mid = cubic(1.0 - (t - sps * 0.5));
            if (t < sps) continue;
            t -= sps;
            const CF s = cubic(1.0 - t);
            o.iq.push_back(s);
            syms++;
            power += 0.01 * (std::norm(s) - power);
            if (have) {
                // Gardner: Re{(this - previous) * conj(mid)}
                err = std::real((s - prevSym) * std::conj(mid)) / std::max(power, 1e-12);
                t += std::clamp(g * err, -0.2, 0.2) * sps;
            }
            prevSym = s;
            have = true;
        }
    }
};

// Carrier recovery for BPSK / QPSK (decision directed, 2nd order loop). Run it on symbols
// (after the clock recovery) or on samples
class CostasBlock : public Block {
    double phase = 0.0, freq = 0.0, rate = 0.0;
    double lockAvg = 0.0;
public:
    CostasBlock() { type = "costas"; title = "Costas loop"; category = "Symbols"; ins = {{"in", PT::IQ}}; setOuts({{"out", PT::IQ}});
                    addChoice("ord", "Modulation", {"BPSK", "QPSK"}, 1);
                    addNum("bw", "Loop bandwidth", 0.01, 0.0005, 0.2, 0.001, "", 4);
                    addNum("max", "Max. offset", 5000, 10, 200000, 100, "Hz"); }
    void reset() override { phase = 0.0; freq = 0.0; lockAvg = 0.0; }
    std::string status() const override {
        char b[80];
        std::snprintf(b, sizeof(b), "offset %+.0f Hz, lock %.0f%%", freq * rate / (2 * PI_D), std::clamp(lockAvg, 0.0, 1.0) * 100.0);
        return b;
    }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        passOn(in[0], o, PT::IQ, in[0] ? in[0]->rate : 0);
        if (!in[0] || in[0]->rate <= 0) return;
        rate = in[0]->rate;
        // until locked the loop is 3x wider (pulls in a Doppler offset faster)
        const double bw = num("bw") * (lockAvg < 0.85 ? 3.0 : 1.0), z = 0.707;
        const double th = bw / (z + 0.25 / z), dn = 1 + 2 * z * th + th * th;
        const double alpha = 4 * z * th / dn, beta = 4 * th * th / dn;
        const double fmax = 2 * PI_D * num("max") / rate;
        const bool qpsk = choice("ord") == 1;
        o.iq.reserve(in[0]->iq.size());
        for (CF v : in[0]->iq) {
            const CF r = v * CF((float)std::cos(-phase), (float)std::sin(-phase));
            o.iq.push_back(r);
            double e;
            const double I = r.real(), Q = r.imag(), mag = std::max(1e-12, (double)std::abs(r));
            if (qpsk) e = ((I > 0 ? 1.0 : -1.0) * Q - (Q > 0 ? 1.0 : -1.0) * I) / mag;
            else e = (I > 0 ? 1.0 : -1.0) * Q / mag;
            // how close the symbols are to the ideal points (1 = locked)
            const double lk = qpsk ? (std::fabs(I) * std::fabs(Q) * 2.0 / (mag * mag)) : (I * I - Q * Q) / (mag * mag);
            lockAvg += 0.001 * (lk - lockAvg);
            freq = std::clamp(freq + beta * e, -fmax, fmax);
            phase += freq + alpha * e;
            if (phase > PI_D) phase -= 2 * PI_D; else if (phase < -PI_D) phase += 2 * PI_D;
        }
    }
};

// OQPSK -> QPSK: in OQPSK the Q channel changes half a symbol after I. Delaying I by half a
// symbol lines the two up, so the usual QPSK clock recovery works after it. Needs the carrier
// removed first (Costas loop on the samples before this block).
class OqpskAlign : public Block {
    std::deque<CF> hist;
public:
    OqpskAlign() { type = "oqpsk"; title = "OQPSK -> QPSK (half-symbol delay)"; category = "Symbols"; ins = {{"in", PT::IQ}}; setOuts({{"out", PT::IQ}});
                   addNum("baud", "Symbol rate", 72000, 10, 2000000, 100, "Bd");
                   addChoice("which", "Delay", {"I (Q comes late)", "Q (I comes late)"}); }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        passOn(in[0], o, PT::IQ, in[0] ? in[0]->rate : 0);
        if (!in[0] || in[0]->rate <= 0) return;
        if (changed(in[0]->rate)) hist.clear();
        const double d = 0.5 * in[0]->rate / std::max(1.0, num("baud"));   // delay in samples
        const int di = (int)std::floor(d);
        const float mu = (float)(d - di);
        const bool delayI = choice("which") == 0;
        for (CF v : in[0]->iq) {
            hist.push_back(v);
            const size_t need = (size_t)di + 3;
            if (hist.size() > need + 1) hist.pop_front();
            if (hist.size() < need + 1) { o.iq.push_back(v); continue; }
            // the value d samples ago (cubic between the samples di and di+1 back)
            const size_t n = hist.size() - 1;
            auto at = [&](int back) { return hist[n - (size_t)std::clamp(back, 0, (int)n)]; };
            const CF p0 = at(di - 1), p1 = at(di), p2 = at(di + 1), p3 = at(di + 2);
            const float m = mu, m2 = m * m, m3 = m2 * m;
            const CF late = 0.5f * (2.0f * p1 + (p2 - p0) * m + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * m2 + (3.0f * p1 - p0 - 3.0f * p2 + p3) * m3);
            o.iq.push_back(delayI ? CF(late.real(), v.imag()) : CF(v.real(), late.imag()));
        }
    }
};

// Differential decoding of soft bits, each channel on its own (I with the previous I, Q with
// the previous Q): the transmitter sent the change, not the bit (Meteor-M2-x LRPT)
class DiffDecode : public Block {
    float prev[2] = {0, 0};
    int k = 0;
public:
    DiffDecode() { type = "diff"; title = "Differential decode"; category = "Bits"; ins = {{"soft", PT::Real}}; setOuts({{"soft", PT::Real}});
                   addChoice("mode", "Channels", {"I and Q separately (pairs)", "one stream"}); }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        passOn(in[0], o, PT::Real, in[0] ? in[0]->rate : 0);
        if (!in[0]) return;
        const bool pairs = choice("mode") == 0;
        for (float v : in[0]->re) {
            const int c = pairs ? (k & 1) : 0;
            // bit = this xor previous; soft: the product (smaller of the two magnitudes), negated
            const float m = std::min(std::fabs(v), std::fabs(prev[c]));
            o.re.push_back(((v > 0) == (prev[c] > 0)) ? -m : m);
            prev[c] = v;
            k++;
        }
    }
};

// Soft decisions: I then Q of every symbol as a real stream (+-127 at the ideal points;
// positive = bit 1)
class SoftBits : public Block {
    double amp = 1.0;
public:
    SoftBits() { type = "softbits"; title = "Soft bits (I, Q)"; category = "Symbols"; ins = {{"symbols", PT::IQ}}; setOuts({{"soft", PT::Real}});
                 addChoice("w", "Bits per symbol", {"2 (I and Q, QPSK)", "1 (I only, BPSK)"}); }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        const bool bpsk = choice("w") == 1;
        passOn(in[0], o, PT::Real, in[0] ? in[0]->rate * (bpsk ? 1 : 2) : 0);
        if (!in[0]) return;
        for (CF v : in[0]->iq) {
            amp += 0.002 * ((std::fabs(v.real()) + (bpsk ? std::fabs(v.real()) : std::fabs(v.imag()))) / 2.0 - amp);
            const float k = (float)(127.0 / std::max(amp, 1e-12));
            o.re.push_back(std::clamp(v.real() * k, -127.0f, 127.0f));
            if (!bpsk) o.re.push_back(std::clamp(v.imag() * k, -127.0f, 127.0f));
        }
    }
};

// ---------------------------------------------------------------------
//  Real signals: rate, arithmetic, FM broadcast stereo
// ---------------------------------------------------------------------

// Any rate -> any rate (low-pass at 0.45 x the lower one, then fractional resampling)
class ResampleReal : public Block {
    Fir<float> f;
    double pos = 0.0;
    float prev = 0.0f;
public:
    ResampleReal() { type = "resample"; title = "Resample (real)"; category = "Real"; ins = {{"in", PT::Real}}; setOuts({{"out", PT::Real}});
                     addNum("rate", "Output rate", 192000, 1000, 4000000, 1000, "S/s"); }
    std::string status() const override { return seenRate > 0 ? std::to_string((int)seenRate) + " -> " + std::to_string((int)num("rate")) + " S/s" : ""; }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        const double outRate = num("rate");
        passOn(in[0], o, PT::Real, outRate);
        if (!in[0] || in[0]->rate <= 0) return;
        if (changed(in[0]->rate)) { f.set(designLowpass(in[0]->rate, 0.45 * std::min(in[0]->rate, outRate))); pos = 0.0; }
        const double step = in[0]->rate / outRate;   // input samples per output sample
        for (float x : in[0]->re) {
            const float y = f.step(x);
            // output samples between prev (0) and y (1)
            while (pos < 1.0) { o.re.push_back(prev + (y - prev) * (float)pos); pos += step; }
            pos -= 1.0;
            prev = y;
        }
    }
};

// Locks to a pilot tone and gives its N-th harmonic in the pilot's phase (sin): FM stereo
// (19 kHz pilot, x2 = the 38 kHz subcarrier), RDS (x3 = 57 kHz)
class PilotPll : public Block {
    double theta = 0.0, freq = 0.0, iAvg = 0.0, qAvg = 0.0, rate = 0.0;
    double level = 0.0, lock = 0.0;
public:
    PilotPll() { type = "pilot"; title = "Pilot PLL"; category = "Real"; ins = {{"signal", PT::Real}}; setOuts({{"carrier", PT::Real}});
                 addNum("hz", "Pilot", 19000, 10, 1000000, 100, "Hz");
                 addNum("mult", "Output harmonic", 2, 1, 8, 1, "x");
                 addNum("bw", "Loop bandwidth", 30, 1, 1000, 5, "Hz");
                 addNum("phase", "Phase correction", 0, -180, 180, 5, "deg"); }
    std::string status() const override {
        char b[80];
        std::snprintf(b, sizeof(b), "%s, %+.1f Hz", lock > 0.8 ? "LOCKED (stereo)" : "no pilot", freq * rate / (2 * PI_D));
        return b;
    }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        passOn(in[0], o, PT::Real, in[0] ? in[0]->rate : 0);
        if (!in[0] || in[0]->rate <= 0) return;
        rate = in[0]->rate;
        if (changed(rate)) { theta = 0.0; freq = 0.0; iAvg = qAvg = 0.0; }
        const double w0 = 2 * PI_D * num("hz") / rate;
        const double wn = 2 * PI_D * num("bw") / rate, z = 0.707;
        const double alpha = 2 * z * wn, beta = wn * wn;
        const double avg = 1.0 - std::exp(-2 * PI_D * num("bw") * 4.0 / rate);   // phase detector smoothing
        const double N = num("mult");
        for (float x : in[0]->re) {
            // lock so that the pilot = cos(theta)
            iAvg += avg * (x * std::cos(theta) - iAvg);
            qAvg += avg * (-x * std::sin(theta) - qAvg);
            const double e = std::atan2(qAvg, std::fabs(iAvg) + 1e-12);
            freq = std::clamp(freq + beta * e, -w0 * 0.01, w0 * 0.01);
            theta += w0 + freq + alpha * e;
            if (theta > 2 * PI_D) theta -= 2 * PI_D;
            // a pilot sent as sin(p) is cos(theta) with p = theta + pi/2: the N-th harmonic in the
            // pilot's phase is sin(N p)
            // (the -pi/2 measured: best L/R separation, 28 dB on a synthetic stereo signal)
            o.re.push_back((float)std::sin(N * (theta + PI_D / 2) - PI_D / 2 + num("phase") * PI_D / 180.0));
        }
        level = std::hypot(iAvg, qAvg);
        lock = std::fabs(iAvg) / (level + 1e-12) * (level > 1e-9 ? 1.0 : 0.0);
    }
};

// Sum and difference -> left and right (FM stereo: L = M + k S, R = M - k S)
class StereoMatrix : public Block {
public:
    StereoMatrix() { type = "matrix"; title = "Stereo matrix"; category = "Real"; ins = {{"L+R", PT::Real}, {"L-R", PT::Real}}; syncInputs = true;
                     setOuts({{"left", PT::Real}, {"right", PT::Real}});
                     addNum("k", "L-R gain", 2, 0, 10, 0.1, "", 2); }
    void process(const std::vector<const Stream*>& in) override {
        Stream& L = out[0]; Stream& R = out[1];
        passOn(in[0], L, PT::Real, in[0] ? in[0]->rate : 0);
        passOn(in[0], R, PT::Real, in[0] ? in[0]->rate : 0);
        if (!in[0]) return;
        const float k = (float)num("k");
        for (size_t i = 0; i < in[0]->re.size(); i++) {
            const float m = in[0]->re[i], s = (in[1] && i < in[1]->re.size()) ? in[1]->re[i] * k : 0.0f;
            L.re.push_back(m + s); R.re.push_back(m - s);
        }
    }
};

// FM broadcast de-emphasis (50 us Europe, 75 us Americas)
class Deemphasis : public Block {
    double y = 0.0;
public:
    Deemphasis() { type = "deemph"; title = "De-emphasis"; category = "Real"; ins = {{"in", PT::Real}}; setOuts({{"out", PT::Real}});
                   addChoice("tau", "Time constant", {"50 us (Europe)", "75 us (Americas)"}); }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        passOn(in[0], o, PT::Real, in[0] ? in[0]->rate : 0);
        if (!in[0] || in[0]->rate <= 0) return;
        const double tau = choice("tau") == 0 ? 50e-6 : 75e-6;
        const double a = 1.0 - std::exp(-1.0 / (tau * in[0]->rate));
        for (float x : in[0]->re) { y += a * (x - y); o.re.push_back((float)y); }
    }
};

// ---------------------------------------------------------------------
//  Tones (AFSK)
// ---------------------------------------------------------------------

// Two-tone FSK on audio (AFSK, e.g. Bell 202 1200 / 2200 Hz): +1 mark ... -1 space
class FskTones : public Block {
    double pm = 0, ps = 0;
    std::deque<CF> hm, hs;
    CF sm {0, 0}, ss {0, 0};
public:
    FskTones() { type = "fsktones"; title = "FSK tone detector"; category = "Real"; ins = {{"audio", PT::Real}}; setOuts({{"mark-space", PT::Real}});
                 addNum("mark", "Mark tone", 1200, 10, 100000, 10, "Hz");
                 addNum("space", "Space tone", 2200, 10, 100000, 10, "Hz");
                 addNum("baud", "Symbol rate", 1200, 10, 100000, 50, "Bd"); }
    void reset() override { hm.clear(); hs.clear(); sm = ss = CF(0, 0); }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        passOn(in[0], o, PT::Real, in[0] ? in[0]->rate : 0);
        if (!in[0] || in[0]->rate <= 0) return;
        if (changed(in[0]->rate)) reset();
        const double fs = in[0]->rate;
        const size_t L = std::max<size_t>(2, (size_t)std::lround(fs / num("baud")));
        const double dm = 2 * PI_D * num("mark") / fs, ds = 2 * PI_D * num("space") / fs;
        for (float x : in[0]->re) {
            const CF a(x * (float)std::cos(pm), -x * (float)std::sin(pm)), b(x * (float)std::cos(ps), -x * (float)std::sin(ps));
            pm += dm; ps += ds;
            if (pm > 2 * PI_D) pm -= 2 * PI_D;
            if (ps > 2 * PI_D) ps -= 2 * PI_D;
            hm.push_back(a); hs.push_back(b); sm += a; ss += b;
            if (hm.size() > L) { sm -= hm.front(); hm.pop_front(); ss -= hs.front(); hs.pop_front(); }
            const float m = std::abs(sm), s = std::abs(ss);
            o.re.push_back((m - s) / (m + s + 1e-12f));
        }
    }
};

// ---------------------------------------------------------------------
//  Bits
// ---------------------------------------------------------------------

// G3RUH / K9NG descrambler (x^17 + x^12 + 1), 9600 Bd packet
class G3ruhBlock : public Block {
    uint32_t sr = 0;
public:
    G3ruhBlock() { type = "g3ruh"; title = "G3RUH descrambler"; category = "Bits"; ins = {{"bits", PT::Sym}}; setOuts({{"bits", PT::Sym}}); }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        passOn(in[0], o, PT::Sym, in[0] ? in[0]->rate : 0);
        o.levels = 2;
        if (!in[0]) return;
        for (uint8_t b : in[0]->sym) {
            if (b == FRAME) { o.sym.push_back(FRAME); continue; }
            o.sym.push_back((uint8_t)(b ^ ((sr >> 11) & 1) ^ ((sr >> 16) & 1)));
            sr = (sr << 1) | b;
        }
    }
};

// CCSDS pseudo-random sequence (x^8 + x^7 + x^5 + x^3 + 1, all ones at the start of each frame)
inline const std::vector<uint8_t>& ccsdsPn() {
    static std::vector<uint8_t> pn;
    if (pn.empty()) {
        uint8_t sr = 0xFF;
        for (int i = 0; i < 255 * 8; i++) {
            pn.push_back((uint8_t)((sr >> 7) & 1));
            // (gives FF 48 0E C0 9A 0D 70 BC ...)
            const uint8_t fb = (uint8_t)(((sr >> 7) ^ (sr >> 4) ^ (sr >> 2) ^ sr) & 1);
            sr = (uint8_t)((sr << 1) | fb);
        }
    }
    return pn;
}

class DerandBlock : public Block {
    long long pos = -1;
public:
    DerandBlock() { type = "derand"; title = "Derandomize (CCSDS)"; category = "Bits"; ins = {{"frames (bits)", PT::Sym}}; setOuts({{"bits", PT::Sym}});
                    addNum("skip", "Not after a frame start", 32, 0, 1024, 8, "bits"); }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        passOn(in[0], o, PT::Sym, in[0] ? in[0]->rate : 0);
        o.levels = 2;
        if (!in[0]) return;
        const auto& pn = ccsdsPn();
        const long long skip = (long long)num("skip");
        for (uint8_t b : in[0]->sym) {
            if (b == FRAME) { o.sym.push_back(FRAME); pos = 0; continue; }
            if (pos < 0) { o.sym.push_back(b); continue; }
            const long long k = pos - skip;
            o.sym.push_back(k >= 0 ? (uint8_t)(b ^ pn[(size_t)(k % (long long)pn.size())]) : b);
            pos++;
        }
    }
};

// Bits (frames marked by the frame sync) -> frames of bytes, most significant bit first
class PackBytes : public Block {
    std::vector<uint8_t> cur;
    int nbits = 0, skipLeft = 0;
    uint8_t acc = 0;
    bool in_ = false;
    long long frames = 0;
public:
    PackBytes() { type = "pack"; title = "Bits to bytes (frames)"; category = "Bits"; ins = {{"frames (bits)", PT::Sym}}; setOuts({{"frames", PT::Frames}});
                  addNum("skip", "Leave out after a frame start", 32, 0, 1024, 8, "bits");
                  addNum("len", "Frame length (0: to the next)", 1020, 0, 65536, 1, "bytes"); }
    std::string status() const override { return std::to_string(frames) + " frames"; }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        o.clear(); o.type = PT::Frames;
        if (!in[0]) return;
        o.rate = in[0]->rate / 8;
        const size_t len = (size_t)num("len");
        auto done = [&]() { if (!cur.empty() && (len == 0 || cur.size() == len)) { o.frames.push_back(cur); frames++; } cur.clear(); in_ = false; };
        for (uint8_t b : in[0]->sym) {
            if (b == FRAME) { if (len == 0) done(); cur.clear(); in_ = true; nbits = 0; acc = 0; skipLeft = (int)num("skip"); continue; }
            if (!in_) continue;
            if (skipLeft > 0) { skipLeft--; continue; }
            acc = (uint8_t)((acc << 1) | (b & 1));
            if (++nbits == 8) { cur.push_back(acc); nbits = 0; acc = 0; if (len && cur.size() == len) done(); }
        }
    }
};

// HDLC: flags, bit stuffing, CRC-16 (X.25) -> frames without the CRC (AX.25 and others)
class HdlcBlock : public Block {
    std::vector<uint8_t> bits;
    int ones = 0;
    bool inFrame = false;
    long long good = 0, bad = 0;
public:
    HdlcBlock() { type = "hdlc"; title = "HDLC deframer"; category = "Bits"; ins = {{"bits", PT::Sym}}; setOuts({{"frames", PT::Frames}});
                  addChoice("crc", "CRC-16 check", {"drop bad frames", "keep all"});
                  addNum("min", "Shortest frame", 17, 3, 400, 1, "bytes"); }
    std::string status() const override { char b[64]; std::snprintf(b, sizeof(b), "%lld frames OK, %lld bad CRC", good, bad); return b; }
    static uint16_t crc16(const uint8_t* p, size_t n) {
        uint16_t c = 0xFFFF;
        for (size_t i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0x8408 : c >> 1; }
        return (uint16_t)~c;
    }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        o.clear(); o.type = PT::Frames;
        if (!in[0]) return;
        o.rate = in[0]->rate / 8;
        for (uint8_t b : in[0]->sym) {
            if (b == FRAME) continue;
            if (b) {
                if (++ones > 6) { inFrame = false; bits.clear(); }   // abort / idle
                bits.push_back(1);
                continue;
            }
            if (ones == 6) {
                // a flag (0111 1110): what came before it is a frame
                if (inFrame && bits.size() >= 7) {
                    bits.resize(bits.size() - 7);
                    if (bits.size() % 8 == 0 && bits.size() / 8 >= (size_t)num("min")) {
                        std::vector<uint8_t> by(bits.size() / 8, 0);
                        for (size_t i = 0; i < bits.size(); i++) if (bits[i]) by[i / 8] |= (uint8_t)(1 << (i % 8));   // LSB first
                        const size_t n = by.size();
                        const bool ok = crc16(by.data(), n - 2) == (uint16_t)(by[n - 2] | (by[n - 1] << 8));
                        if (ok) good++; else bad++;
                        if (ok || choice("crc") == 1) { by.resize(n - 2); o.frames.push_back(by); }
                    }
                }
                bits.clear(); inFrame = true; ones = 0;
                continue;
            }
            if (ones == 5) { ones = 0; continue; }   // a stuffed zero
            ones = 0;
            bits.push_back(0);
            if (bits.size() > 8 * 4096) { bits.clear(); inFrame = false; }
        }
    }
};

// ---------------------------------------------------------------------
//  Convolutional code (K = 7, rate 1/2: CCSDS, Meteor LRPT, many satellites)
// ---------------------------------------------------------------------
inline int parity8(unsigned v) { v ^= v >> 4; v ^= v >> 2; v ^= v >> 1; return (int)(v & 1); }

struct ConvCode {
    unsigned pa = 0x4F, pb = 0x6D;
    // reg7 = (state << 1) | bit (the newest bit is the lowest)
    int out0(unsigned reg7) const { return parity8(reg7 & pa); }
    int out1(unsigned reg7) const { return parity8(reg7 & pb); }
    std::vector<uint8_t> encode(const std::vector<uint8_t>& bits, unsigned& state) const {
        std::vector<uint8_t> o;
        for (uint8_t b : bits) { const unsigned r = ((state << 1) | b) & 0x7F; o.push_back((uint8_t)out0(r)); o.push_back((uint8_t)out1(r)); state = r & 0x3F; }
        return o;
    }
};
inline std::vector<uint8_t> hexBitsMsb(const std::string& s) { return parsePattern(s); }

// Finds the coded sync word in the soft bits and fixes the QPSK phase ambiguity (which pair
// starts a symbol, I/Q swapped, either inverted): the output is ready for the Viterbi decoder
class CodedSync : public Block {
    std::vector<float> buf;          // soft bits not yet sent
    long long absStart = 0;          // absolute index of buf[0]
    std::vector<uint8_t> patA, patB; // coded sync word (reliable part): first / second bit of each pair
    long long lastHit = -1;
    int parity = -1, swap = 0, flipA = 0, flipB = 0;   // the transform in use (-1: none yet)
    long long hits = 0, misses = 0;
    int lastErr = -1;
    long long outIdx = 0;            // next absolute index to send
public:
    CodedSync() { type = "codedsync"; title = "Coded sync + QPSK phase"; category = "Bits"; ins = {{"soft", PT::Real}}; setOuts({{"soft", PT::Real}});
                  addText("asm", "Sync word", "0x1ACFFC1D");
                  addNum("len", "Coded frame length", 16384, 64, 1000000, 64, "bits");
                  addNum("err", "Max. wrong bits (of 52)", 8, 0, 20, 1, ""); }
    void reset() override { buf.clear(); absStart = 0; lastHit = -1; parity = -1; outIdx = 0; }
    std::string status() const override {
        char b[96];
        if (parity < 0) return "searching for the sync word";
        std::snprintf(b, sizeof(b), "locked: %s%s%s, %lld syncs (last %d wrong)", swap ? "I/Q swapped " : "", flipA ? "I inv. " : "", flipB ? "Q inv. " : "", hits, lastErr);
        return b;
    }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        passOn(in[0], o, PT::Real, in[0] ? in[0]->rate : 0);
        if (changed(1.0)) {
            reset();
            const auto w = parsePattern(text("asm"));
            unsigned st = 0;
            const auto c = ConvCode().encode(w, st);
            patA.clear(); patB.clear();
            // the first 6 pairs depend on the bits before the word: left out
            for (size_t i = 12; i + 1 < c.size(); i += 2) { patA.push_back(c[i]); patB.push_back(c[i + 1]); }
        }
        if (!in[0] || patA.size() < 8) return;
        const size_t P = patA.size(), W = 2 * P + 12;
        const long long frameLen = (long long)num("len");
        const int maxErr = (int)num("err");
        for (float v : in[0]->re) {
            buf.push_back(v);
            const long long n = absStart + (long long)buf.size();
            if ((long long)buf.size() >= (long long)W) {
                // the window starting W bits back: is the coded word there?
                const size_t s0 = buf.size() - W + 12;
                int ea = 0, eb = 0, sa = 0, sb = 0;
                for (size_t k = 0; k < P; k++) {
                    const uint8_t a = buf[s0 + 2 * k] > 0, b = buf[s0 + 2 * k + 1] > 0;
                    ea += a != patA[k]; eb += b != patB[k]; sa += b != patA[k]; sb += a != patB[k];
                }
                const int Pn = (int)P;
                const int eNo = std::min(ea, Pn - ea) + std::min(eb, Pn - eb);
                const int eSw = std::min(sa, Pn - sa) + std::min(sb, Pn - sb);
                const int e = std::min(eNo, eSw);
                if (e <= maxErr) {
                    const long long pos = n - (long long)W;   // start of the coded word (incl. its first 12 bits)
                    const bool expected = lastHit >= 0 && std::llabs(((pos - lastHit) % frameLen + frameLen) % frameLen) <= 4;
                    const bool fresh = parity < 0 || misses >= 3 * frameLen;
                    if (expected || fresh || pos - lastHit > 3 * frameLen) {
                        parity = (int)(pos & 1);
                        swap = eSw < eNo;
                        if (!swap) { flipA = ea > Pn - ea; flipB = eb > Pn - eb; }
                        else { flipA = sa > Pn - sa; flipB = sb > Pn - sb; }
                        lastHit = pos; hits++; lastErr = e; misses = 0;
                    }
                }
            }
            misses++;
            // send the pairs older than the window (so a new lock applies from its word on)
            const long long limit = n - (long long)W;
            if (parity >= 0) {
                if ((outIdx & 1) != parity) outIdx++;   // realign to the symbol pairs
                while (outIdx + 1 < limit + 1 && outIdx + 1 < n) {
                    if (outIdx < absStart) { outIdx = absStart + ((absStart & 1) != parity); continue; }
                    float a = buf[(size_t)(outIdx - absStart)], b = buf[(size_t)(outIdx + 1 - absStart)];
                    if (swap) std::swap(a, b);
                    if (flipA) a = -a;
                    if (flipB) b = -b;
                    o.re.push_back(a); o.re.push_back(b);
                    outIdx += 2;
                }
            } else {
                outIdx = limit;
            }
            // keep the window and what is still to be sent
            const long long keepFrom = std::min(outIdx, n - (long long)W);
            if (keepFrom - absStart > 4096) {
                const long long drop = keepFrom - absStart;
                buf.erase(buf.begin(), buf.begin() + (size_t)drop);
                absStart += drop;
            }
        }
    }
};

// Viterbi decoder (K = 7, rate 1/2, soft input: positive = bit 1)
class ViterbiBlock : public Block {
    static constexpr int NS = 64, TB = 96, CHUNK = 256;
    ConvCode code;
    double pm[NS], nm[NS];
    std::vector<uint64_t> dec;        // decisions, one 64-bit word per step
    float half = 0.0f;
    bool haveHalf = false;
    long long steps = 0;
    double quality = 0.0;
    void init() {
        for (int s = 0; s < NS; s++) pm[s] = s == 0 ? 0.0 : -1e9;
        dec.clear(); haveHalf = false;
    }
    void traceback(Stream& o, size_t outN) {
        // the best state now, back through TB steps, then outN decided bits
        int s = 0;
        for (int k = 1; k < NS; k++) if (pm[k] > pm[s]) s = k;
        const size_t n = dec.size();
        std::vector<uint8_t> bitsRev;
        for (size_t k = n; k-- > 0;) {
            const int x = (int)((dec[k] >> s) & 1);
            if (k < outN) bitsRev.push_back((uint8_t)(s & 1));
            s = (s >> 1) | (x << 5);
        }
        for (size_t i = bitsRev.size(); i-- > 0;) o.sym.push_back(bitsRev[i]);
        dec.erase(dec.begin(), dec.begin() + (long long)outN);
    }
public:
    ViterbiBlock() { type = "viterbi"; title = "Viterbi decoder (K=7, 1/2)"; category = "Bits"; ins = {{"soft", PT::Real}}; setOuts({{"bits", PT::Sym}});
                     addChoice("poly", "Code", {"0x4F, 0x6D (CCSDS / Meteor)", "0x6D, 0x4F (swapped)"}); init(); }
    void reset() override { init(); }
    std::string status() const override {
        char b[64];
        std::snprintf(b, sizeof(b), "%lld bits, path quality %.0f%%", steps, std::clamp(quality, 0.0, 1.0) * 100.0);
        return b;
    }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        passOn(in[0], o, PT::Sym, in[0] ? in[0]->rate / 2 : 0);
        o.levels = 2;
        if (!in[0]) return;
        if (changed(1.0)) { code.pa = choice("poly") == 0 ? 0x4F : 0x6D; code.pb = choice("poly") == 0 ? 0x6D : 0x4F; init(); }
        for (float v : in[0]->re) {
            if (!haveHalf) { half = v; haveHalf = true; continue; }
            haveHalf = false;
            const float a = half, b = v;
            uint64_t d = 0;
            double best = -1e300;
            for (int s2 = 0; s2 < NS; s2++) {
                double m[2];
                for (int x = 0; x < 2; x++) {
                    const unsigned reg = (unsigned)((x << 6) | s2);
                    const double bm = (code.out0(reg) ? a : -a) + (code.out1(reg) ? b : -b);
                    m[x] = pm[(s2 >> 1) | (x << 5)] + bm;
                }
                const int x = m[1] > m[0];
                nm[s2] = m[x];
                if (x) d |= (uint64_t)1 << s2;
                best = std::max(best, nm[s2]);
            }
            // keep the metrics small; how much better the best path is than the average says
            // how clean the input is
            double avg = 0.0;
            for (int s2 = 0; s2 < NS; s2++) { pm[s2] = nm[s2] - best; avg += pm[s2]; }
            avg /= NS;
            quality += 0.0005 * (std::min(1.0, -avg / (2.0 * 127.0 * 4.0)) - quality);
            dec.push_back(d);
            steps++;
            if ((int)dec.size() >= TB + CHUNK) traceback(o, CHUNK);
        }
    }
};

// ---------------------------------------------------------------------
//  Reed-Solomon (255, 223), CCSDS (dual basis), interleaved
// ---------------------------------------------------------------------
class RsCcsds {
    static constexpr int NN = 255, NROOTS = 32, FCR = 112, PRIM = 11, IPRIM = 116, A0 = 255;
    uint8_t alpha[256], index[256], gen[NROOTS + 1], tal[256], tal1[256];
    static int modnn(int x) { while (x >= 255) { x -= 255; x = (x >> 8) + (x & 255); } return x; }
public:
    RsCcsds() {
        int sr = 1;
        for (int i = 0; i < NN; i++) { alpha[i] = (uint8_t)sr; index[sr] = (uint8_t)i; sr <<= 1; if (sr & 256) sr ^= 0x187; }
        alpha[NN] = 0; index[0] = A0;
        gen[0] = 1;
        for (int i = 0, root = FCR * PRIM; i < NROOTS; i++, root += PRIM) {
            gen[i + 1] = 1;
            for (int j = i; j > 0; j--)
                gen[j] = gen[j] ? (uint8_t)(gen[j - 1] ^ alpha[modnn(index[gen[j]] + root)]) : gen[j - 1];
            gen[0] = alpha[modnn(index[gen[0]] + root)];
        }
        for (int i = 0; i <= NROOTS; i++) gen[i] = index[gen[i]];
        static const uint8_t T[8] = {0x8d, 0xef, 0xec, 0x86, 0xfa, 0x99, 0xaf, 0x7b};
        for (int i = 0; i < 256; i++) {
            int v = 0;
            for (int j = 0; j < 8; j++) for (int k = 0; k < 8; k++) if (i & (1 << k)) v ^= T[7 - k] & (1 << j);
            tal[i] = (uint8_t)v; tal1[v] = (uint8_t)i;
        }
    }
    // 223 data bytes -> 32 parity bytes (conventional or dual basis)
    void encode(const uint8_t* data, uint8_t* parity, bool dual) const {
        uint8_t d[223], p[NROOTS] = {0};
        for (int i = 0; i < 223; i++) d[i] = dual ? tal1[data[i]] : data[i];
        for (int i = 0; i < 223; i++) {
            const int fb = index[d[i] ^ p[0]];
            if (fb != A0) for (int j = 1; j < NROOTS; j++) p[j] ^= alpha[modnn(fb + gen[NROOTS - j])];
            std::memmove(p, p + 1, NROOTS - 1);
            p[NROOTS - 1] = fb != A0 ? alpha[modnn(fb + gen[0])] : 0;
        }
        for (int i = 0; i < NROOTS; i++) parity[i] = dual ? tal[p[i]] : p[i];
    }
    // Corrects a 255-byte codeword in place: errors corrected, or -1 (too many)
    int decode(uint8_t* cw, bool dual) const {
        uint8_t data[NN];
        for (int i = 0; i < NN; i++) data[i] = dual ? tal1[cw[i]] : cw[i];
        int s[NROOTS];
        for (int i = 0; i < NROOTS; i++) s[i] = data[0];
        for (int j = 1; j < NN; j++)
            for (int i = 0; i < NROOTS; i++)
                s[i] = s[i] == 0 ? data[j] : data[j] ^ alpha[modnn(index[s[i]] + (FCR + i) * PRIM)];
        bool err = false;
        for (int i = 0; i < NROOTS; i++) { err = err || s[i]; s[i] = index[s[i]]; }
        if (!err) return 0;
        int lambda[NROOTS + 1] = {0}, b[NROOTS + 1], t[NROOTS + 1];
        lambda[0] = 1;
        for (int i = 0; i <= NROOTS; i++) b[i] = index[lambda[i]];
        int el = 0;
        for (int r = 1; r <= NROOTS; r++) {
            int discr = 0;
            for (int i = 0; i < r; i++) if (lambda[i] != 0 && s[r - i - 1] != A0) discr ^= alpha[modnn(index[lambda[i]] + s[r - i - 1])];
            discr = index[discr];
            if (discr == A0) {
                std::memmove(&b[1], b, NROOTS * sizeof(b[0])); b[0] = A0;
            } else {
                t[0] = lambda[0];
                for (int i = 0; i < NROOTS; i++) t[i + 1] = b[i] != A0 ? lambda[i + 1] ^ alpha[modnn(discr + b[i])] : lambda[i + 1];
                if (2 * el <= r - 1) {
                    el = r - el;
                    for (int i = 0; i <= NROOTS; i++) b[i] = lambda[i] == 0 ? A0 : modnn(index[lambda[i]] - discr + NN);
                } else {
                    std::memmove(&b[1], b, NROOTS * sizeof(b[0])); b[0] = A0;
                }
                std::memcpy(lambda, t, sizeof(t));
            }
        }
        int deg = 0;
        for (int i = 0; i <= NROOTS; i++) { lambda[i] = index[lambda[i]]; if (lambda[i] != A0) deg = i; }
        int reg[NROOTS + 1], root[NROOTS], loc[NROOTS], count = 0;
        std::memcpy(&reg[1], &lambda[1], NROOTS * sizeof(reg[0]));
        for (int i = 1, k = IPRIM - 1; i <= NN; i++, k = modnn(k + IPRIM)) {
            int q = 1;
            for (int j = deg; j > 0; j--) if (reg[j] != A0) { reg[j] = modnn(reg[j] + j); q ^= alpha[reg[j]]; }
            if (q != 0) continue;
            root[count] = i; loc[count] = k;
            if (++count == deg) break;
        }
        if (deg != count) return -1;
        int omega[NROOTS + 1];
        const int degOmega = deg - 1;
        for (int i = 0; i <= degOmega; i++) {
            int tmp = 0;
            for (int j = i; j >= 0; j--) if (s[i - j] != A0 && lambda[j] != A0) tmp ^= alpha[modnn(s[i - j] + lambda[j])];
            omega[i] = index[tmp];
        }
        for (int j = count - 1; j >= 0; j--) {
            int num1 = 0;
            for (int i = degOmega; i >= 0; i--) if (omega[i] != A0) num1 ^= alpha[modnn(omega[i] + i * root[j])];
            const int num2 = alpha[modnn(root[j] * (FCR - 1) + NN)];
            int den = 0;
            for (int i = std::min(deg, NROOTS - 1) & ~1; i >= 0; i -= 2) if (lambda[i + 1] != A0) den ^= alpha[modnn(lambda[i + 1] + i * root[j])];
            if (num1 != 0 && den != 0) data[loc[j]] ^= alpha[modnn(index[num1] + index[num2] + NN - index[den])];
        }
        for (int i = 0; i < NN; i++) cw[i] = dual ? tal[data[i]] : data[i];
        return count;
    }
    static const RsCcsds& get() { static RsCcsds r; return r; }
};

class RsBlock : public Block {
    long long ok = 0, fixed = 0, failed = 0;
public:
    RsBlock() { type = "rs"; title = "Reed-Solomon (255,223)"; category = "Frames"; ins = {{"frames", PT::Frames}}; setOuts({{"frames", PT::Frames}});
                addNum("depth", "Interleave", 4, 1, 8, 1, "");
                addChoice("dual", "Basis", {"dual (CCSDS)", "conventional"});
                addChoice("bad", "Uncorrectable", {"drop", "pass on"});
                addChoice("out", "Output", {"data only (no parity)", "whole frame"}); }
    std::string status() const override { char b[80]; std::snprintf(b, sizeof(b), "%lld clean, %lld corrected, %lld failed", ok, fixed, failed); return b; }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        o.clear(); o.type = PT::Frames;
        if (!in[0]) return;
        o.rate = in[0]->rate;
        const int I = (int)num("depth");
        const bool dual = choice("dual") == 0;
        for (const auto& f : in[0]->frames) {
            if ((int)f.size() < 255 * I) continue;
            std::vector<uint8_t> fr = f;
            bool good = true;
            int corr = 0;
            for (int k = 0; k < I; k++) {
                uint8_t cw[255];
                for (int i = 0; i < 255; i++) cw[i] = fr[k + I * i];
                const int r = RsCcsds::get().decode(cw, dual);
                if (r < 0) { good = false; continue; }
                corr += r;
                for (int i = 0; i < 255; i++) fr[k + I * i] = cw[i];
            }
            if (!good) failed++; else if (corr) fixed++; else ok++;
            if (!good && choice("bad") == 0) continue;
            if (choice("out") == 0) fr.resize(223 * I);
            o.frames.push_back(std::move(fr));
        }
    }
};

// VCDU (CCSDS transfer frames, M_PDU) -> source packets
class CcsdsPackets : public Block {
    std::vector<uint8_t> pk;          // the packet being collected
    bool have = false;
    int lastCount = -1;
    long long packets = 0, frames = 0, gaps = 0;
    int lastVcid = -1;
    void feed(const uint8_t* p, size_t n, Stream& o) {
        for (size_t i = 0; i < n; i++) {
            pk.push_back(p[i]);
            if (pk.size() >= 6) {
                const size_t len = (size_t)((pk[4] << 8) | pk[5]) + 7;
                if (pk.size() == len) {
                    const int apid = ((pk[0] & 7) << 8) | pk[1];
                    if (apid != 2047) { o.frames.push_back(pk); packets++; }
                    pk.clear();
                }
            }
        }
    }
public:
    CcsdsPackets() { type = "ccsds"; title = "CCSDS packets (from VCDU)"; category = "Frames"; ins = {{"VCDU", PT::Frames}}; setOuts({{"packets", PT::Frames}});
                     addNum("vcid", "Virtual channel (-1: all)", -1, -1, 63, 1, "");
                     addNum("hdr", "M_PDU header at byte", 8, 6, 64, 1, ""); }
    std::string status() const override {
        char b[96];
        std::snprintf(b, sizeof(b), "%lld frames (VCID %d), %lld packets, %lld gaps", frames, lastVcid, packets, gaps);
        return b;
    }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        o.clear(); o.type = PT::Frames;
        if (!in[0]) return;
        o.rate = in[0]->rate;
        const int hdr = (int)num("hdr");
        for (const auto& f : in[0]->frames) {
            if ((int)f.size() < hdr + 3) continue;
            const int vcid = f[1] & 0x3F;
            if ((int)num("vcid") >= 0 && vcid != (int)num("vcid")) continue;
            if (vcid == 63) continue;   // fill frames
            lastVcid = vcid;
            frames++;
            const int count = (f[2] << 16) | (f[3] << 8) | f[4];
            if (lastCount >= 0 && count != ((lastCount + 1) & 0xFFFFFF)) { gaps++; pk.clear(); have = false; }
            lastCount = count;
            const int fhp = ((f[hdr] & 7) << 8) | f[hdr + 1];
            const uint8_t* d = f.data() + hdr + 2;
            const size_t n = f.size() - hdr - 2;
            if (fhp == 0x7FF) { if (have) feed(d, n, o); continue; }
            if ((size_t)fhp > n) continue;
            if (have) feed(d, fhp, o);     // the end of the packet from the earlier frames
            pk.clear(); have = true;
            feed(d + fhp, n - fhp, o);
        }
    }
};

// ---------------------------------------------------------------------
//  Meteor MSU-MR: the JPEG-like coded image packets -> image lines
// ---------------------------------------------------------------------
class MsuMrBlock : public Block {
    static constexpr int W = 1568;   // 196 blocks of 8 pixels, 14 packets x 14 blocks
    std::vector<uint8_t> strip;      // 8 lines being filled
    long long firstPck = -1, lastPck = -1, curStrip = -1;
    long long packets = 0, strips = 0;
    // Huffman tables (the standard JPEG luminance ones)
    struct Huff { std::vector<int> codes, lens, vals; };
    Huff dc, ac;
    static Huff build(const uint8_t* bits, const uint8_t* vals, int nv) {
        Huff h;
        int code = 0, k = 0;
        for (int len = 1; len <= 16; len++) {
            for (int i = 0; i < bits[len - 1]; i++) { h.codes.push_back(code); h.lens.push_back(len); h.vals.push_back(vals[k++]); code++; }
            code <<= 1;
        }
        (void)nv;
        return h;
    }
    struct BitReader {
        const uint8_t* p; size_t n, pos = 0;
        int bit() { if (pos >= n * 8) return -1; const int b = (p[pos / 8] >> (7 - pos % 8)) & 1; pos++; return b; }
        int bits(int k) { int v = 0; for (int i = 0; i < k; i++) { const int b = bit(); if (b < 0) return -100000; v = (v << 1) | b; } return v; }
    };
    static int decodeHuff(BitReader& br, const Huff& h) {
        int code = 0;
        for (int len = 1; len <= 16; len++) {
            const int b = br.bit();
            if (b < 0) return -1;
            code = (code << 1) | b;
            for (size_t i = 0; i < h.codes.size(); i++) if (h.lens[i] == len && h.codes[i] == code) return h.vals[i];
        }
        return -1;
    }
    static int extend(int v, int s) { return v < (1 << (s - 1)) ? v - (1 << s) + 1 : v; }
    void flushStrip(Stream& o) {
        if (curStrip < 0) return;
        for (int r = 0; r < 8; r++) o.lines.emplace_back(strip.begin() + r * W, strip.begin() + (r + 1) * W);
        strips++;
        std::fill(strip.begin(), strip.end(), 0);
    }
public:
    MsuMrBlock() {
        type = "msumr"; title = "Meteor MSU-MR image"; category = "Frames"; ins = {{"packets", PT::Frames}}; setOuts({{"lines", PT::Lines}});
        addNum("apid", "Channel (APID)", 64, 64, 69, 1, "");
        static const uint8_t dcBits[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
        static const uint8_t dcVals[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
        static const uint8_t acBits[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
        static const uint8_t acVals[162] = {
            0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
            0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
            0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
            0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
            0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
            0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
            0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};
        dc = build(dcBits, dcVals, 12);
        ac = build(acBits, acVals, 162);
        strip.assign(W * 8, 0);
    }
    void reset() override { firstPck = -1; lastPck = -1; curStrip = -1; std::fill(strip.begin(), strip.end(), 0); }
    std::string status() const override { char b[64]; std::snprintf(b, sizeof(b), "%lld packets, %lld lines", packets, strips * 8); return b; }

    // The standard tables, the order of the coefficients
    static const int* zigzag() {
        static const int z[64] = {0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
                                  35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
        return z;
    }
    static const int* stdQ() {
        static const int q[64] = {16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62,
                                  18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92, 49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
        return q;
    }
    // one 8x8 block: coefficients (natural order) -> pixels
    static void idct(const double* c, uint8_t* out) {
        static double cs[8][8];
        static bool init = false;
        if (!init) { for (int x = 0; x < 8; x++) for (int u = 0; u < 8; u++) cs[x][u] = (u ? 1.0 : 1.0 / std::sqrt(2.0)) * std::cos((2 * x + 1) * u * PI_D / 16.0); init = true; }
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                double s = 0.0;
                for (int v = 0; v < 8; v++) for (int u = 0; u < 8; u++) s += cs[x][u] * cs[y][v] * c[v * 8 + u];
                out[y * 8 + x] = (uint8_t)std::clamp((int)std::lround(s / 4.0 + 128.0), 0, 255);
            }
    }

    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        o.clear(); o.type = PT::Lines;
        if (!in[0]) return;
        const int want = (int)num("apid");
        for (const auto& p : in[0]->frames) {
            if (p.size() < 21) continue;
            const int apid = ((p[0] & 7) << 8) | p[1];
            if (apid != want) continue;
            const long long pck = ((p[2] & 0x3F) << 8) | p[3];
            const uint8_t* d = p.data() + 14;   // after the primary header (6) and the time (8)
            const size_t n = p.size() - 14;
            const int mcu = d[0];
            const int q = d[5];
            if (firstPck < 0) { if (mcu != 0) continue; firstPck = pck; lastPck = pck; }
            if (pck < lastPck - 8000) firstPck -= 16384;   // the counter wrapped
            lastPck = pck;
            const long long s = (pck - firstPck) / 43;     // 43 packets per strip (3 channels x 14 + telemetry)
            if (s != curStrip) {
                if (curStrip >= 0 && s > curStrip) {
                    flushStrip(o);
                    for (long long k = curStrip + 1; k < s && k < curStrip + 50; k++) flushStrip(o);   // missing strips: black
                }
                curStrip = s;
            }
            // quantisation table for this packet's quality
            double qt[64];
            const double f = (q > 20 && q < 50) ? 5000.0 / q : 200.0 - 2.0 * q;
            for (int i = 0; i < 64; i++) qt[i] = std::max(1.0, std::round(f / 100.0 * stdQ()[i]));
            BitReader br {d + 6, n - 6};
            int prevDc = 0;
            for (int m = 0; m < 14; m++) {
                double c[64] = {0};
                const int cat = decodeHuff(br, dc);
                if (cat < 0) break;
                int diff = 0;
                if (cat) { const int v = br.bits(cat); if (v < -1000) break; diff = extend(v, cat); }
                prevDc += diff;
                c[0] = prevDc * qt[0];
                bool bad = false;
                for (int k = 1; k < 64;) {
                    const int rs = decodeHuff(br, ac);
                    if (rs < 0) { bad = true; break; }
                    if (rs == 0) break;
                    const int r = rs >> 4, sz = rs & 15;
                    if (sz == 0) { if (r == 15) { k += 16; continue; } break; }
                    k += r;
                    if (k > 63) { bad = true; break; }
                    const int v = br.bits(sz);
                    if (v < -1000) { bad = true; break; }
                    c[zigzag()[k]] = extend(v, sz) * qt[k];
                    k++;
                }
                if (bad) break;
                uint8_t px[64];
                idct(c, px);
                const int x0 = (mcu + m) * 8;
                if (x0 + 8 > W) break;
                for (int y = 0; y < 8; y++) std::memcpy(&strip[y * W + x0], &px[y * 8], 8);
            }
            packets++;
        }
    }
};

// ---------------------------------------------------------------------
//  Raster: a signal cut into image lines (WEFAX, and others drawn line by line)
// ---------------------------------------------------------------------
class RasterBlock : public Block {
    double pos = 0.0;                 // samples into the current line
    std::vector<double> sum;
    std::vector<int> cnt;
    std::deque<std::vector<uint8_t>> recent;   // for the alignment
    long long lines = 0;
    // start / stop tones: crossings of the middle per second
    double mean = 0.0;
    int crossings = 0;
    double toneT = 0.0, lastV = 0.0;
    int startSec = 0, stopSec = 0;
    bool running = false;
    double alignPx = 0.0;
public:
    RasterBlock() { type = "raster"; title = "Raster lines"; category = "Image"; ins = {{"signal", PT::Real}}; setOuts({{"lines", PT::Lines}});
                    addNum("lpm", "Lines per minute", 120, 1, 6000, 1, "");
                    addNum("width", "Pixels per line", 1809, 16, 8192, 1, "");
                    addNum("black", "Black at", -400, -1e6, 1e6, 50, "", 1);
                    addNum("white", "White at", 400, -1e6, 1e6, 50, "", 1);
                    addChoice("start", "Start", {"right away", "on start tone, stop on stop tone"});
                    addNum("tstart", "Start tone", 300, 1, 5000, 5, "Hz");
                    addNum("tstop", "Stop tone", 450, 1, 5000, 5, "Hz");
                    addNum("slant", "Slant", 0, -5000, 5000, 1, "ppm");
                    addNum("shift", "Shift", 0, -8192, 8192, 10, "px");
                    showOnly("tstart", "start", {1}); showOnly("tstop", "start", {1}); }
    std::vector<std::string> buttons() const override { return {"Start", "Stop", "Align"}; }
    void onButton(int i) override {
        if (i == 0) { running = true; pos = 0; }
        else if (i == 1) running = false;
        else {
            // phasing / any vertical edge: the column where the brightness jumps up the most,
            // over the last lines, becomes the line start
            if (recent.size() < 4) return;
            const size_t w = recent.back().size();
            std::vector<double> col(w, 0.0);
            for (const auto& l : recent) if (l.size() == w) for (size_t x = 0; x < w; x++) col[x] += l[x];
            size_t best = 0; double bestD = -1e300;
            for (size_t x = 0; x < w; x++) { const double d = col[x] - col[(x + w - 4) % w]; if (d > bestD) { bestD = d; best = x; } }
            alignPx += (double)best;
        }
    }
    std::string status() const override {
        char b[96];
        std::snprintf(b, sizeof(b), "%s, %lld lines", running ? "receiving" : choice("start") == 1 ? "waiting for the start tone" : "stopped", lines);
        return b;
    }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        o.clear(); o.type = PT::Lines;
        if (!in[0] || in[0]->rate <= 0) return;
        const double fs = in[0]->rate;
        const int W = (int)num("width");
        if ((int)sum.size() != W) { sum.assign(W, 0.0); cnt.assign(W, 0); }
        if (choice("start") == 0 && seenVersion != paramVersion) running = true;
        changed(fs);
        const double spl = 60.0 / std::max(1.0, num("lpm")) * fs * (1.0 + num("slant") * 1e-6);
        const double lo = num("black"), hi = num("white");
        const double shift = num("shift") + alignPx;
        for (float v : in[0]->re) {
            // the start / stop tones: the picture signal alternating at a fixed rate
            mean += 0.0005 * (v - mean);
            if ((v - mean) * (lastV - mean) < 0) crossings++;
            lastV = v;
            toneT += 1.0 / fs;
            if (toneT >= 1.0) {
                const double hz = crossings / 2.0 / toneT;
                const auto nearHz = [&](double f) { return std::fabs(hz - f) < f * 0.1; };
                startSec = nearHz(num("tstart")) ? startSec + 1 : 0;
                stopSec = nearHz(num("tstop")) ? stopSec + 1 : 0;
                if (choice("start") == 1) {
                    if (!running && startSec >= 3) { running = true; pos = 0; }
                    if (running && stopSec >= 3) running = false;
                }
                crossings = 0; toneT = 0.0;
            }
            if (!running) continue;
            double px = (pos / spl * W) - shift;
            px = std::fmod(std::fmod(px, (double)W) + W, (double)W);
            const int x = std::clamp((int)px, 0, W - 1);
            sum[x] += v; cnt[x]++;
            pos += 1.0;
            if (pos >= spl) {
                pos -= spl;
                std::vector<uint8_t> line(W);
                double last = lo;
                for (int i = 0; i < W; i++) {
                    const double val = cnt[i] ? sum[i] / cnt[i] : last;
                    last = val;
                    line[i] = (uint8_t)std::clamp((val - lo) / (hi - lo) * 255.0, 0.0, 255.0);
                }
                std::fill(sum.begin(), sum.end(), 0.0);
                std::fill(cnt.begin(), cnt.end(), 0);
                recent.push_back(line);
                while (recent.size() > 40) recent.pop_front();
                o.lines.push_back(std::move(line));
                lines++;
            }
        }
    }
};

// Shows image lines; Save writes a PNG
class ImageView : public Block {
    std::vector<uint8_t> px;          // width * rows
    int width = 0, rows = 0;
    static constexpr int MAX_ROWS = 6000;
    sf::Texture tex;
    int texRows = -1, texFrom = 0;
    std::string saved;
public:
    ImageView() { type = "image"; title = "Image view"; category = "View"; ins = {{"lines", PT::Lines}};
                  addChoice("show", "Show", {"newest lines", "whole picture"}); }
    sf::Vector2f viewSize() const override { return {460.0f, 300.0f}; }
    std::vector<std::string> buttons() const override { return {"Save PNG", "Clear"}; }
    std::string status() const override {
        if (!saved.empty()) return saved;
        return std::to_string(width) + " x " + std::to_string(rows);
    }
    void onButton(int i) override {
        if (i == 1) { px.clear(); rows = 0; texRows = -1; saved.clear(); return; }
        if (!rows || !width) { saved = "nothing to save yet"; return; }
        sf::Image img({(unsigned)width, (unsigned)rows});
        for (int y = 0; y < rows; y++)
            for (int x = 0; x < width; x++) { const uint8_t g = px[(size_t)y * width + x]; img.setPixel({(unsigned)x, (unsigned)y}, sf::Color(g, g, g)); }
        std::string dir = hooks().imageDir ? hooks().imageDir() : std::string(".");
        char name[64];
        const std::time_t now = std::time(nullptr);
        std::strftime(name, sizeof(name), "pipeline_%Y%m%d_%H%M%S.png", std::localtime(&now));
#ifdef _WIN32
        const std::string base = dir + "\\" + std::string(name, std::strlen(name) - 4);
#else
        const std::string base = dir + "/" + std::string(name, std::strlen(name) - 4);
#endif
        // never overwrite (two views saved in the same second)
        std::string path = base + ".png";
        for (int k = 2; std::ifstream(path).good() && k < 100; k++) path = base + "_" + std::to_string(k) + ".png";
        saved = img.saveToFile(path) ? "saved " + path : "could not write " + path;
    }
    void process(const std::vector<const Stream*>& in) override {
        if (!in[0]) return;
        for (const auto& l : in[0]->lines) {
            if (l.empty()) continue;
            if ((int)l.size() != width) { width = (int)l.size(); px.clear(); rows = 0; texRows = -1; }
            px.insert(px.end(), l.begin(), l.end());
            rows++;
            if (rows > MAX_ROWS) { px.erase(px.begin(), px.begin() + width); rows--; }
        }
    }
    void drawView(const Draw& d, const sf::FloatRect& r) override {
        d.rect(r, sf::Color(8, 10, 12), sf::Color(50, 50, 58));
        if (!rows || !width) { d.text("no picture yet", r.position.x + 6, r.position.y + 4, 11, sf::Color(110, 110, 120)); return; }
        const bool whole = choice("show") == 1;
        const float scale = whole ? std::min(r.size.x / width, r.size.y / rows) : r.size.x / width;
        const int vis = whole ? rows : std::min(rows, (int)(r.size.y / scale));
        const int from = rows - vis;
        if (texRows != rows || texFrom != from) {
            // grey -> RGBA for the rows shown (at most a few hundred thousand pixels)
            std::vector<uint8_t> rgba((size_t)width * vis * 4);
            for (size_t i = 0; i < (size_t)width * vis; i++) {
                const uint8_t g = px[(size_t)from * width + i];
                rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = g; rgba[i * 4 + 3] = 255;
            }
            if (tex.resize({(unsigned)width, (unsigned)vis})) tex.update(rgba.data());
            tex.setSmooth(true);
            texRows = rows; texFrom = from;
        }
        sf::Sprite sp(tex);
        sp.setScale({scale, scale});
        sp.setPosition({r.position.x, r.position.y + (whole ? 0.0f : r.size.y - vis * scale)});
        d.win->draw(sp);
    }
};

// AX.25: the frames from the HDLC deframer as text (SRC>DEST,PATH: info)
class Ax25View : public Block {
    std::deque<std::string> lines;
    long long n = 0;
public:
    Ax25View() { type = "ax25"; title = "AX.25 packets"; category = "View"; ins = {{"frames", PT::Frames}}; }
    sf::Vector2f viewSize() const override { return {460.0f, 200.0f}; }
    std::vector<std::string> buttons() const override { return {"Copy", "Clear"}; }
    std::string status() const override { return std::to_string(n) + " packets"; }
    void onButton(int i) override {
        if (i == 1) { lines.clear(); return; }
        std::string t;
        for (const auto& l : lines) t += l + "\n";
        sf::Clipboard::setString(sf::String::fromUtf8(t.begin(), t.end()));
    }
    static std::string call(const uint8_t* a) {
        std::string s;
        for (int i = 0; i < 6; i++) { const char c = (char)(a[i] >> 1); if (c != ' ') s += c; }
        const int ssid = (a[6] >> 1) & 15;
        if (ssid) s += "-" + std::to_string(ssid);
        return s;
    }
    static std::string parse(const std::vector<uint8_t>& f) {
        if (f.size() < 16) return "";
        size_t i = 0;
        std::vector<std::string> addr;
        std::vector<bool> used;
        while (i + 7 <= f.size()) {
            addr.push_back(call(&f[i]));
            used.push_back((f[i + 6] & 0x80) != 0);
            const bool last = f[i + 6] & 1;
            i += 7;
            if (last || addr.size() >= 10) break;
        }
        if (addr.size() < 2 || i >= f.size()) return "";
        std::string s = addr[1] + ">" + addr[0];
        for (size_t k = 2; k < addr.size(); k++) s += "," + addr[k] + (used[k] ? "*" : "");
        const uint8_t ctl = f[i++];
        if ((ctl & 0x01) == 0 || ctl == 0x03) i++;   // I / UI frame: PID
        s += ": ";
        for (; i < f.size(); i++) { const uint8_t c = f[i]; s += (c >= 32 && c < 127) ? (char)c : (c == '\r' ? ' ' : '.'); }
        return s;
    }
    void process(const std::vector<const Stream*>& in) override {
        if (!in[0]) return;
        for (const auto& f : in[0]->frames) {
            const std::string s = parse(f);
            if (s.empty()) continue;
            char t[16];
            const std::time_t now = std::time(nullptr);
            std::strftime(t, sizeof(t), "%H:%M:%S ", std::localtime(&now));
            lines.push_back(t + s);
            while (lines.size() > 500) lines.pop_front();
            n++;
        }
    }
    void drawView(const Draw& d, const sf::FloatRect& r) override {
        d.rect(r, sf::Color(8, 10, 12), sf::Color(50, 50, 58));
        const float lh = 13.0f;
        const int rows = std::max(1, (int)((r.size.y - 4) / lh));
        float y = r.position.y + 2;
        const size_t from = lines.size() > (size_t)rows ? lines.size() - rows : 0;
        for (size_t k = from; k < lines.size(); k++) {
            std::string t = lines[k];
            while (!t.empty() && d.width(t, 10) > r.size.x - 8) t.pop_back();
            d.text(t, r.position.x + 4, y, 10, sf::Color(215, 230, 215));
            y += lh;
        }
        if (lines.empty()) d.text("no packets yet", r.position.x + 6, r.position.y + 4, 11, sf::Color(110, 110, 120));
    }
};

// Frames as hex lines (any frame / packet stream)
class FramesView : public Block {
    std::deque<std::string> lines;
    long long n = 0;
public:
    FramesView() { type = "framesview"; title = "Frames view (hex)"; category = "View"; ins = {{"frames", PT::Frames}};
                   addNum("max", "Bytes per line shown", 32, 4, 4096, 4, ""); }
    sf::Vector2f viewSize() const override { return {460.0f, 180.0f}; }
    std::vector<std::string> buttons() const override { return {"Copy", "Clear"}; }
    std::string status() const override { return std::to_string(n) + " frames"; }
    void onButton(int i) override {
        if (i == 1) { lines.clear(); return; }
        std::string t;
        for (const auto& l : lines) t += l + "\n";
        sf::Clipboard::setString(t);
    }
    void process(const std::vector<const Stream*>& in) override {
        if (!in[0]) return;
        for (const auto& f : in[0]->frames) {
            std::string s = std::to_string(f.size()) + "B: ";
            char b[4];
            for (size_t i = 0; i < f.size() && i < (size_t)num("max"); i++) { std::snprintf(b, sizeof(b), "%02X ", f[i]); s += b; }
            lines.push_back(s);
            while (lines.size() > 300) lines.pop_front();
            n++;
        }
    }
    void drawView(const Draw& d, const sf::FloatRect& r) override {
        d.rect(r, sf::Color(8, 10, 12), sf::Color(50, 50, 58));
        const float lh = 13.0f;
        const int rows = std::max(1, (int)((r.size.y - 4) / lh));
        float y = r.position.y + 2;
        const size_t from = lines.size() > (size_t)rows ? lines.size() - rows : 0;
        for (size_t k = from; k < lines.size(); k++) {
            std::string t = lines[k];
            while (!t.empty() && d.width(t, 10) > r.size.x - 8) t.pop_back();
            d.text(t, r.position.x + 4, y, 10, sf::Color(215, 215, 220));
            y += lh;
        }
    }
};
