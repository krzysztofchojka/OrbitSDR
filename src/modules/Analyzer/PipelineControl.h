#pragma once
// (included from Pipeline.h inside namespace pl, after PipelineLib.h)

// =====================================================================
//  Convolution of two signals, and control engineering blocks:
//
//   Convolution / correlation (a * b)  a convolved with the last N samples of b (or N samples
//                                      held at "Capture"), by FFT (overlap-save)
//   Transfer function G(s)             any proper G(s) = num(s) / den(s), simulated in
//                                      continuous time (state space, RK4); poles, stability,
//                                      DC gain and a Bode plot on the block
//   PID controller                     e -> u, derivative filter, output limits, anti-windup
//   Closed loop (PID + G(s))           r -> e = r - H y -> PID -> u (+ d) -> G(s) -> y, the
//                                      whole loop solved together (the pipeline itself has no
//                                      loops); closed-loop poles on the block
//   Step response (metrics)            rise time, overshoot, peak / settling time, steady-state
//                                      error of y after every step of r
//
//  A square wave from Signal source (real) is the usual test step: e.g. 0.1 Hz, amplitude 0.5,
//  offset 0.5 = steps 0 -> 1 -> 0 every 5 s. Slow systems want a low sample rate (1000 S/s).
// =====================================================================

// ---------------------------------------------------------------------
//  Polynomials (highest power first: {1, 2, 1} = s^2 + 2s + 1)
// ---------------------------------------------------------------------
inline std::vector<double> polyTrim(std::vector<double> p) {
    size_t i = 0;
    while (i + 1 < p.size() && std::fabs(p[i]) < 1e-300) i++;
    p.erase(p.begin(), p.begin() + i);
    return p;
}
inline std::vector<double> polyMul(const std::vector<double>& a, const std::vector<double>& b) {
    if (a.empty() || b.empty()) return {};
    std::vector<double> r(a.size() + b.size() - 1, 0.0);
    for (size_t i = 0; i < a.size(); i++) for (size_t j = 0; j < b.size(); j++) r[i + j] += a[i] * b[j];
    return r;
}
inline std::vector<double> polyAdd(std::vector<double> a, std::vector<double> b) {
    if (a.size() < b.size()) std::swap(a, b);
    const size_t off = a.size() - b.size();
    for (size_t i = 0; i < b.size(); i++) a[off + i] += b[i];
    return a;
}
inline std::complex<double> polyAt(const std::vector<double>& p, std::complex<double> s) {
    std::complex<double> v = 0.0;
    for (double c : p) v = v * s + c;
    return v;
}
// Roots (Durand-Kerner); p trimmed, degree >= 1
inline std::vector<std::complex<double>> polyRoots(std::vector<double> p) {
    p = polyTrim(p);
    std::vector<std::complex<double>> r;
    const int n = (int)p.size() - 1;
    if (n < 1 || p[0] == 0.0) return r;
    // roots at 0 first (trailing zero coefficients)
    int zeros = 0;
    while (n - zeros >= 1 && p[p.size() - 1 - zeros] == 0.0) zeros++;
    for (int i = 0; i < zeros; i++) r.push_back(0.0);
    std::vector<double> q(p.begin(), p.end() - zeros);
    const int m = (int)q.size() - 1;
    if (m < 1) return r;
    for (auto& c : q) c /= q[0];
    double bound = 0.0;
    for (int k = 1; k <= m; k++) bound = std::max(bound, std::pow(std::fabs(q[k]), 1.0 / k));
    bound = std::max(2.0 * bound, 1e-6);
    std::vector<std::complex<double>> z(m);
    for (int i = 0; i < m; i++) z[i] = std::polar(bound * 0.9, 2.0 * PI_D * i / m + 0.4);
    for (int it = 0; it < 500; it++) {
        double change = 0.0;
        for (int i = 0; i < m; i++) {
            std::complex<double> den = 1.0;
            for (int j = 0; j < m; j++) if (j != i) den *= (z[i] - z[j]);
            if (std::abs(den) < 1e-300) den = 1e-300;
            const std::complex<double> dz = polyAt(q, z[i]) / den;
            z[i] -= dz;
            change = std::max(change, std::abs(dz) / std::max(1.0, std::abs(z[i])));
        }
        if (change < 1e-13) break;
    }
    // a multiple root comes out as a small cluster (error ~ eps^(1/k)): one value each
    for (int i = 0; i < m; i++) {
        std::vector<int> grp;
        for (int j = 0; j < m; j++) if (std::abs(z[j] - z[i]) < 1e-3 * std::max(1.0, std::abs(z[i]))) grp.push_back(j);
        if (grp.size() < 2) continue;
        std::complex<double> mean = 0.0;
        for (int j : grp) mean += z[j];
        mean /= (double)grp.size();
        for (int j : grp) z[j] = mean;
    }
    for (auto& v : z) { if (std::fabs(v.imag()) < 1e-4 * std::max(1.0, std::abs(v))) v = v.real(); r.push_back(v); }
    std::sort(r.begin(), r.end(), [](auto a, auto b) { return a.real() != b.real() ? a.real() > b.real() : a.imag() > b.imag(); });
    return r;
}
inline std::string rootsText(const std::vector<std::complex<double>>& r, size_t maxN = 6) {
    std::string s;
    char b[64];
    size_t shownN = 0;
    for (size_t i = 0; i < r.size(); i++) {
        if (r[i].imag() < 0) continue;              // a conjugate pair is shown once, as +-
        if (i > 0 && r[i] == r[i - 1]) continue;    // a multiple root once, with (xk)
        if (shownN++ >= maxN) { s += ", ..."; break; }
        int mult = 0;
        for (auto& v : r) if (v == r[i]) mult++;
        if (r[i].imag() > 0) std::snprintf(b, sizeof(b), "%.3g\xC2\xB1%.3gj", r[i].real(), r[i].imag());
        else std::snprintf(b, sizeof(b), "%.3g", r[i].real());
        s += (s.empty() ? "" : ", ") + std::string(b) + (mult > 1 ? " (x" + std::to_string(mult) + ")" : "");
    }
    return s.empty() ? "none" : s;
}
inline bool stableRoots(const std::vector<std::complex<double>>& r) { for (auto& v : r) if (v.real() >= -1e-12) return false; return true; }

// ---------------------------------------------------------------------
//  A continuous LTI system G(s) in controllable canonical form
// ---------------------------------------------------------------------
struct Lti {
    std::vector<double> a, c;   // den normalised (a[0] = 1), output row
    double d = 0.0;             // feedthrough
    int n = 0;
    double speed = 1.0;         // an upper bound of |poles| (for the RK4 step)
    std::string err;
    std::vector<double> num, den;   // as given (trimmed)

    bool set(std::vector<double> nu, std::vector<double> de) {
        err.clear();
        nu = polyTrim(nu); de = polyTrim(de);
        if (de.empty() || std::fabs(de[0]) < 1e-300) { err = "the denominator is empty"; return false; }
        if (nu.empty()) nu = {0.0};
        if (nu.size() > de.size()) { err = "improper: the numerator's degree is above the denominator's"; return false; }
        num = nu; den = de;
        n = (int)de.size() - 1;
        a.assign(n + 1, 0.0);
        for (int k = 0; k <= n; k++) a[k] = de[k] / de[0];
        std::vector<double> b(n + 1, 0.0);
        for (size_t i = 0; i < nu.size(); i++) b[n + 1 - nu.size() + i] = nu[i] / de[0];
        d = b[0];
        c.assign(n, 0.0);
        for (int k = 1; k <= n; k++) c[n - k] = b[k] - b[0] * a[k];
        speed = 1e-9;
        for (int k = 1; k <= n; k++) speed = std::max(speed, std::pow(std::fabs(a[k]), 1.0 / k));
        speed *= 2.0;
        return true;
    }
    bool strictlyProper() const { return num.size() < den.size(); }
    // x' for input u
    void deriv(const double* x, double u, double* dx) const {
        for (int i = 0; i + 1 < n; i++) dx[i] = x[i + 1];
        if (n > 0) {
            double s = u;
            for (int k = 1; k <= n; k++) s -= a[k] * x[n - k];
            dx[n - 1] = s;
        }
    }
    double out(const double* x, double u) const {
        double y = d * u;
        for (int i = 0; i < n; i++) y += c[i] * x[i];
        return y;
    }
    std::complex<double> at(std::complex<double> s) const { return polyAt(num, s) / polyAt(den, s); }
};

// RK4 on a state vector, f(x, dx)
template <typename F>
inline void rk4(std::vector<double>& x, double h, F f, std::vector<double> (&k)[5]) {
    const size_t n = x.size();
    for (auto& v : k) v.resize(n);
    f(x.data(), k[0].data());
    for (size_t i = 0; i < n; i++) k[4][i] = x[i] + 0.5 * h * k[0][i];
    f(k[4].data(), k[1].data());
    for (size_t i = 0; i < n; i++) k[4][i] = x[i] + 0.5 * h * k[1][i];
    f(k[4].data(), k[2].data());
    for (size_t i = 0; i < n; i++) k[4][i] = x[i] + h * k[2][i];
    f(k[4].data(), k[3].data());
    for (size_t i = 0; i < n; i++) x[i] += h / 6.0 * (k[0][i] + 2 * k[1][i] + 2 * k[2][i] + k[3][i]);
}
// RK4 sub-steps per sample so that h * |fastest pole| stays below 0.5
inline int substeps(double dt, double speed) { return (int)std::clamp(std::ceil(dt * speed / 0.5), 1.0, 4000.0); }

// A Bode plot: |G(jw)| in dB (orange) and phase in degrees (blue), w log-spaced
inline void drawBode(const Draw& d, const sf::FloatRect& r, const std::function<std::complex<double>(double)>& G, double w0, double w1) {
    d.rect(r, sf::Color(8, 10, 12), sf::Color(50, 50, 58));
    const int N = 200;
    std::vector<double> db(N), ph(N);
    double prev = 0.0, unwrap = 0.0;
    for (int i = 0; i < N; i++) {
        const double w = w0 * std::pow(w1 / w0, (double)i / (N - 1));
        const std::complex<double> g = G(w);
        db[i] = 20.0 * std::log10(std::abs(g) + 1e-300);
        double p = std::arg(g) * 180.0 / PI_D;
        if (i > 0) { while (p + unwrap - prev > 180.0) unwrap -= 360.0; while (p + unwrap - prev < -180.0) unwrap += 360.0; }
        ph[i] = p + unwrap;
        prev = ph[i];
    }
    double hi = -1e300, lo = 1e300;
    for (double v : db) if (std::isfinite(v)) { hi = std::max(hi, v); lo = std::min(lo, v); }
    if (!(hi > lo)) { hi = lo + 1; }
    hi = std::ceil(hi / 20.0) * 20.0 + 5; lo = std::max(std::floor(lo / 20.0) * 20.0, hi - 125.0);
    double pmin = 1e300, pmax = -1e300;
    for (double v : ph) { pmin = std::min(pmin, v); pmax = std::max(pmax, v); }
    pmin = std::floor(pmin / 90.0) * 90.0; pmax = std::ceil(pmax / 90.0) * 90.0;
    if (pmax - pmin < 90) { pmax += 45; pmin -= 45; }
    const float H = r.size.y - 14, X0 = r.position.x + 30, W = r.size.x - 36;
    // decades
    for (double w = std::pow(10.0, std::floor(std::log10(w0))); w <= w1 * 1.001; w *= 10.0) {
        if (w < w0 * 0.999) continue;
        const float x = X0 + W * (float)(std::log10(w / w0) / std::log10(w1 / w0));
        d.rect(sf::FloatRect({x, r.position.y + 2}, {1, H}), sf::Color(35, 37, 44));
        char b[24]; std::snprintf(b, sizeof(b), "%g", w);
        d.text(b, x - d.width(b, 9) / 2, r.position.y + r.size.y - 12, 9, sf::Color(110, 110, 120));
    }
    for (double v = hi - 5; v >= lo; v -= 20) {
        const float y = r.position.y + 2 + H * (float)((hi - v) / (hi - lo));
        d.rect(sf::FloatRect({X0, y}, {W, 1}), sf::Color(35, 37, 44));
        char b[16]; std::snprintf(b, sizeof(b), "%.0f", v);
        d.text(b, r.position.x + 2, y - 6, 9, sf::Color(220, 150, 60));
    }
    auto line = [&](const std::vector<double>& v, double a, double b, sf::Color c) {
        sf::VertexArray va(sf::PrimitiveType::LineStrip);
        for (int i = 0; i < N; i++) {
            if (!std::isfinite(v[i])) continue;
            const double t = std::clamp((v[i] - a) / (b - a), 0.0, 1.0);
            va.append({{X0 + W * i / (N - 1), r.position.y + 2 + H * (float)(1.0 - t)}, c});
        }
        d.win->draw(va);
    };
    line(ph, pmin, pmax, sf::Color(90, 170, 255));
    line(db, lo, hi, sf::Color(255, 170, 60));
    char b[64];
    std::snprintf(b, sizeof(b), "|G| dB   phase %.0f..%.0f deg   w rad/s", pmin, pmax);
    d.text(b, X0 + 4, r.position.y + 3, 9, sf::Color(150, 150, 160));
}
// A frequency range for a Bode plot from the poles and zeros
inline void bodeRange(const std::vector<std::complex<double>>& pz, double& w0, double& w1) {
    double lo = 1e300, hi = 0.0;
    for (auto& v : pz) { const double m = std::abs(v); if (m > 1e-9) { lo = std::min(lo, m); hi = std::max(hi, m); } }
    if (hi == 0.0) { lo = hi = 1.0; }
    w0 = std::pow(10.0, std::floor(std::log10(lo)) - 1);
    w1 = std::pow(10.0, std::ceil(std::log10(hi)) + 1);
    if (w1 / w0 < 1e3) w1 = w0 * 1e3;
}

// ---------------------------------------------------------------------
//  Convolution / correlation of two signals
// ---------------------------------------------------------------------
class ConvolveBlock : public Block {
    std::vector<std::complex<double>> win;     // the last N samples of b (oldest first)
    std::vector<std::complex<double>> held;    // the kernel source while "held"
    std::vector<std::complex<double>> histA;   // the last N-1 samples of a
    std::vector<Complex> fa, fh;
    std::vector<float> shown;                  // the kernel, for the view
    bool anyComplex = false, wantCapture = false;
    size_t N = 0;
public:
    ConvolveBlock() {
        type = "conv"; title = "Convolution / correlation (a * b)"; category = "Filters";
        ins = {{"a", PT::Any}, {"b", PT::Any}}; setOuts({{"out", PT::Any}});
        syncInputs = true;
        addNum("n", "Length of b (kernel)", 64, 1, 65536, 1, "samples");
        addChoice("src", "Kernel", {"the last N samples of b (moving)", "N samples of b held (Capture)"});
        addChoice("op", "Operation", {"convolution  y = a * b", "correlation  y = a (x) b (b reversed, conj.)"});
        addChoice("sc", "Scale", {"sum", "divide by N (average)"});
    }
    std::vector<std::string> buttons() const override { return {"Capture b"}; }
    void onButton(int) override { wantCapture = true; }
    void reset() override { win.clear(); held.clear(); histA.clear(); }
    sf::Vector2f viewSize() const override { return {300.0f, 80.0f}; }
    std::string status() const override {
        return "kernel " + std::to_string(N) + " samples" + (choice("src") == 1 ? (held.empty() ? ", press Capture" : ", held") : ", moving");
    }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        if (!in[0]) { o.clear(); return; }
        const bool cplx = in[0]->type == PT::IQ || (in[1] && in[1]->type == PT::IQ);
        passOn(in[0], o, cplx ? PT::IQ : PT::Real, in[0]->rate);
        N = (size_t)std::max(1.0, num("n"));
        // b's window
        if (in[1]) {
            const size_t nb = in[1]->size();
            for (size_t i = 0; i < nb; i++)
                win.push_back(in[1]->type == PT::IQ ? std::complex<double>(in[1]->iq[i].real(), in[1]->iq[i].imag()) : std::complex<double>(in[1]->re[i], 0.0));
            if (win.size() > N) win.erase(win.begin(), win.end() - N);
        }
        if (wantCapture || (choice("src") == 1 && held.empty() && win.size() == N)) { held = win; wantCapture = false; }
        const auto& bw = choice("src") == 1 ? held : win;
        // the kernel h[k] (length N; missing samples of b are 0)
        std::vector<std::complex<double>> h(N, 0.0);
        const size_t have = std::min(N, bw.size());
        for (size_t k = 0; k < have; k++) h[N - have + k] = bw[bw.size() - have + k];   // oldest first
        if (choice("op") == 1) { std::reverse(h.begin(), h.end()); for (auto& v : h) v = std::conj(v); }
        if (choice("sc") == 1) for (auto& v : h) v /= (double)N;
        shown.resize(std::min<size_t>(N, 512));
        for (size_t i = 0; i < shown.size(); i++) shown[i] = (float)h[i * N / shown.size()].real();
        // a, with N-1 samples of its past, through the kernel (overlap-save)
        const size_t L = in[0]->size();
        if (histA.size() != N - 1) histA.assign(N - 1, 0.0);
        if (L == 0) return;
        size_t M = 1;
        while (M < N - 1 + L) M <<= 1;
        fa.assign(M, 0.0);
        for (size_t i = 0; i < N - 1; i++) fa[i] = histA[i];
        for (size_t i = 0; i < L; i++)
            fa[N - 1 + i] = in[0]->type == PT::IQ ? Complex(in[0]->iq[i].real(), in[0]->iq[i].imag()) : Complex(in[0]->re[i], 0.0);
        // the next call's history
        for (size_t i = 0; i < N - 1; i++) histA[i] = fa[L + i];
        fh.assign(M, 0.0);
        for (size_t k = 0; k < N; k++) fh[k] = h[k];
        fft(fa); fft(fh);
        for (size_t i = 0; i < M; i++) fa[i] = std::conj(fa[i] * fh[i]);
        fft(fa);   // conj(fft(conj(X))) / M = ifft(X)
        if (cplx) { o.iq.resize(L); for (size_t i = 0; i < L; i++) { const Complex v = std::conj(fa[N - 1 + i]) / (double)M; o.iq[i] = CF((float)v.real(), (float)v.imag()); } }
        else { o.re.resize(L); for (size_t i = 0; i < L; i++) o.re[i] = (float)(fa[N - 1 + i].real() / M); }
    }
    void drawView(const Draw& d, const sf::FloatRect& r) override {
        d.rect(r, sf::Color(8, 10, 12), sf::Color(50, 50, 58));
        d.text(choice("op") == 1 ? "kernel: b reversed (real part)" : "kernel h[k] = b (real part)", r.position.x + 3, r.position.y + 2, 9, sf::Color(130, 130, 140));
        if (shown.empty()) return;
        float mx = 1e-12f;
        for (float v : shown) mx = std::max(mx, std::fabs(v));
        const float mid = r.position.y + r.size.y / 2 + 5, hh = r.size.y / 2 - 8;
        d.rect(sf::FloatRect({r.position.x, mid}, {r.size.x, 1}), sf::Color(45, 45, 52));
        sf::VertexArray va(sf::PrimitiveType::LineStrip);
        for (size_t i = 0; i < shown.size(); i++)
            va.append({{r.position.x + 3 + (r.size.x - 6) * (shown.size() > 1 ? (float)i / (shown.size() - 1) : 0.5f), mid - shown[i] / mx * hh}, sf::Color(120, 230, 120)});
        d.win->draw(va);
    }
};

// ---------------------------------------------------------------------
//  G(s)
// ---------------------------------------------------------------------
class TfBlock : public Block {
    Lti g;
    std::vector<double> x;
    std::vector<double> k[5];
    std::vector<std::complex<double>> poles, zeros;
    std::string sNum, sDen, info;
    double rate = 0.0;
    void rebuild() {
        sNum = text("num"); sDen = text("den");
        x.clear();
        if (!g.set(parseNumbers(sNum), parseNumbers(sDen))) { info = g.err; poles.clear(); zeros.clear(); return; }
        x.assign(g.n, 0.0);
        poles = polyRoots(g.den);
        zeros = g.num.size() > 1 ? polyRoots(g.num) : std::vector<std::complex<double>>();
        const std::complex<double> g0 = g.at(0.0);
        char b[64];
        info = "poles " + rootsText(poles, 4) + (stableRoots(poles) ? "  stable" : "  NOT stable");
        if (std::isfinite(g0.real()) && std::abs(polyAt(g.den, 0.0)) > 1e-300) { std::snprintf(b, sizeof(b), ", DC gain %.4g", g0.real()); info += b; }
    }
public:
    TfBlock() {
        type = "tf"; title = "Transfer function G(s)"; category = "Control";
        ins = {{"u", PT::Real}}; setOuts({{"y", PT::Real}});
        addText("num", "Numerator  (s^m ... s^0)", "1");
        addText("den", "Denominator (s^n ... s^0)", "1, 2, 1");
        rebuild();
    }
    std::string status() const override { return info; }
    std::vector<std::string> buttons() const override { return {"Reset state"}; }
    void onButton(int) override { std::fill(x.begin(), x.end(), 0.0); }
    sf::Vector2f viewSize() const override { return {320.0f, 130.0f}; }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        passOn(in[0], o, PT::Real, in[0] ? in[0]->rate : 0);
        if (text("num") != sNum || text("den") != sDen) rebuild();
        if (!in[0] || in[0]->rate <= 0 || !g.err.empty()) return;
        rate = in[0]->rate;
        const double dt = 1.0 / rate;
        const int ns = substeps(dt, g.speed);
        const double h = dt / ns;
        o.re.resize(in[0]->re.size());
        for (size_t i = 0; i < in[0]->re.size(); i++) {
            const double u = in[0]->re[i];
            o.re[i] = (float)g.out(x.data(), u);
            for (int s = 0; s < ns && g.n > 0; s++) rk4(x, h, [&](const double* xs, double* dx) { g.deriv(xs, u, dx); }, k);
        }
        for (double& v : x) if (!std::isfinite(v)) v = 0.0;   // blew up (unstable): start again
    }
    void drawView(const Draw& d, const sf::FloatRect& r) override {
        if (!g.err.empty()) { d.rect(r, sf::Color(8, 10, 12), sf::Color(50, 50, 58)); return; }
        std::vector<std::complex<double>> pz = poles;
        pz.insert(pz.end(), zeros.begin(), zeros.end());
        double w0, w1;
        bodeRange(pz, w0, w1);
        drawBode(d, r, [&](double w) { return g.at({0.0, w}); }, w0, w1);
    }
};

// ---------------------------------------------------------------------
//  PID: u = Kp e + Ki int e + Kd N s/(s+N) e, limited to [min, max]
// ---------------------------------------------------------------------
struct Pid {
    double kp = 1, ki = 0, kd = 0, nf = 100, lo = -1e9, hi = 1e9;
    double integ = 0.0, xd = 0.0;
    // u for e (the states not moved)
    double u(double e, bool* sat = nullptr, double* raw = nullptr) const {
        const double r = kp * e + ki * integ + kd * nf * (e - xd);
        const double v = std::clamp(r, lo, hi);
        if (sat) *sat = v != r;
        if (raw) *raw = r;
        return v;
    }
};

class PidBlock : public Block {
    Pid p;
public:
    PidBlock() {
        type = "pid"; title = "PID controller"; category = "Control";
        ins = {{"e", PT::Real}}; setOuts({{"u", PT::Real}});
        addNum("kp", "Kp", 1, -1e9, 1e9, 0.1, "", 3);
        addNum("ki", "Ki", 0, -1e12, 1e12, 0.1, "1/s", 3);
        addNum("kd", "Kd", 0, -1e9, 1e9, 0.01, "s", 3);
        addNum("nf", "Derivative filter N", 100, 0.1, 1e6, 10, "rad/s", 1);
        addNum("lo", "Output min", -1e6, -1e9, 1e9, 1, "", 2);
        addNum("hi", "Output max", 1e6, -1e9, 1e9, 1, "", 2);
    }
    std::vector<std::string> buttons() const override { return {"Reset"}; }
    void onButton(int) override { p.integ = 0; p.xd = 0; }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        passOn(in[0], o, PT::Real, in[0] ? in[0]->rate : 0);
        if (!in[0] || in[0]->rate <= 0) return;
        p.kp = num("kp"); p.ki = num("ki"); p.kd = num("kd"); p.nf = num("nf"); p.lo = num("lo"); p.hi = std::max(num("hi"), p.lo);
        const double dt = 1.0 / in[0]->rate, a = 1.0 - std::exp(-p.nf * dt);
        o.re.resize(in[0]->re.size());
        for (size_t i = 0; i < in[0]->re.size(); i++) {
            const double e = in[0]->re[i];
            bool sat; double raw;
            o.re[i] = (float)p.u(e, &sat, &raw);
            // anti-windup: the integrator stops while the output is limited and e pushes further
            if (!(sat && ((raw > p.hi && e > 0) || (raw < p.lo && e < 0)))) p.integ += e * dt;
            p.xd += a * (e - p.xd);
        }
    }
};

// ---------------------------------------------------------------------
//  The closed loop: r -> (+) -> PID -> (+ d) -> G(s) -> y, feedback H y (negative)
// ---------------------------------------------------------------------
class ClosedLoopBlock : public Block {
    Lti g;
    Pid c;
    std::vector<double> x;     // plant states, then the integrator, then the derivative filter
    std::vector<double> k[5];
    std::vector<std::complex<double>> clPoles;
    std::string key, info;
    double speed = 1.0, yPrev = 0.0;
    std::vector<double> pidNum, pidDen;
    void rebuild() {
        key.clear();
        for (const auto& p : params) key += (p.kind == Param::Text ? p.text : std::to_string(p.v)) + "|";
        c.kp = num("kp"); c.ki = num("ki"); c.kd = num("kd"); c.nf = num("nf"); c.lo = num("lo"); c.hi = std::max(num("hi"), c.lo);
        if (!g.set(parseNumbers(text("num")), parseNumbers(text("den")))) { info = "G(s): " + g.err; x.clear(); clPoles.clear(); return; }
        x.assign(g.n + 2, 0.0);
        // C(s) as one fraction (no cancelled factors)
        const double kp = c.kp, ki = c.ki, kd = c.kd, nf = c.nf;
        if (kd == 0 && ki == 0) { pidNum = {kp}; pidDen = {1}; }
        else if (kd == 0) { pidNum = {kp, ki}; pidDen = {1, 0}; }
        else if (ki == 0) { pidNum = {kp + kd * nf, kp * nf}; pidDen = {1, nf}; }
        else { pidNum = {kp + kd * nf, kp * nf + ki, ki * nf}; pidDen = {1, nf, 0}; }
        // 1 + H C G = 0  ->  denC denG + H numC numG
        const double H = num("h");
        auto ch = polyAdd(polyMul(pidDen, g.den), polyMul({H}, polyMul(pidNum, g.num)));
        clPoles = polyRoots(ch);
        speed = std::max(g.speed, nf);
        for (auto& v : clPoles) speed = std::max(speed, 2.0 * std::abs(v));
        info = "closed loop: poles " + rootsText(clPoles, 4) + (stableRoots(clPoles) ? "  stable" : "  NOT stable");
        if (!g.strictlyProper()) info += "  (G biproper: y fed back one sample late)";
    }
public:
    ClosedLoopBlock() {
        type = "loop"; title = "Closed loop (PID + G(s))"; category = "Control";
        ins = {{"r (set point)", PT::Real}, {"d (disturbance)", PT::Real}};
        setOuts({{"y", PT::Real}, {"u", PT::Real}, {"e", PT::Real}});
        syncInputs = true;
        addText("num", "G(s) numerator", "1");
        addText("den", "G(s) denominator", "1, 3, 3, 1");
        addNum("kp", "Kp", 2, -1e6, 1e6, 0.1, "", 3);
        addNum("ki", "Ki", 1, -1e6, 1e6, 0.1, "1/s", 3);
        addNum("kd", "Kd", 0.5, -1e6, 1e6, 0.05, "s", 3);
        addNum("nf", "Derivative filter N", 100, 0.1, 1e6, 10, "rad/s", 1);
        addNum("lo", "u min", -1e6, -1e9, 1e9, 1, "", 2);
        addNum("hi", "u max", 1e6, -1e9, 1e9, 1, "", 2);
        addNum("h", "Feedback gain H", 1, -1e6, 1e6, 0.1, "", 3);
        rebuild();
    }
    std::string status() const override { return info; }
    std::vector<std::string> buttons() const override { return {"Reset state"}; }
    void onButton(int) override { std::fill(x.begin(), x.end(), 0.0); yPrev = 0.0; }
    sf::Vector2f viewSize() const override { return {320.0f, 130.0f}; }
    void process(const std::vector<const Stream*>& in) override {
        for (auto& s : out) passOn(in[0], s, PT::Real, in[0] ? in[0]->rate : 0);
        std::string kNow;
        for (const auto& p : params) kNow += (p.kind == Param::Text ? p.text : std::to_string(p.v)) + "|";
        if (kNow != key) {
            const size_t n0 = x.size();
            std::vector<double> keep = x;
            rebuild();
            if (x.size() == n0) x = keep;   // only gains changed: the loop goes on from where it is
        }
        if (!in[0] || in[0]->rate <= 0 || !g.err.empty()) return;
        const double dt = 1.0 / in[0]->rate, H = num("h");
        const int ns = substeps(dt, speed);
        const double h = dt / ns;
        const int n = g.n;
        const bool late = !g.strictlyProper();
        const size_t L = in[0]->re.size();
        out[0].re.resize(L); out[1].re.resize(L); out[2].re.resize(L);
        for (size_t i = 0; i < L; i++) {
            const double r = in[0]->re[i], dist = in[1] && i < in[1]->re.size() ? in[1]->re[i] : 0.0;
            // the loop at this instant
            auto signals = [&](const double* s, double& y, double& e, double& u, bool& sat, double& raw) {
                Pid cc = c; cc.integ = s[n]; cc.xd = s[n + 1];
                const double yp = late ? yPrev : g.out(s, 0.0);
                e = r - H * yp;
                u = cc.u(e, &sat, &raw);
                y = late ? g.out(s, u + dist) : yp;
            };
            double y, e, u, raw; bool sat;
            signals(x.data(), y, e, u, sat, raw);
            out[0].re[i] = (float)y; out[1].re[i] = (float)u; out[2].re[i] = (float)e;
            for (int st = 0; st < ns; st++) {
                rk4(x, h, [&](const double* s, double* dx) {
                    double yy, ee, uu, rr; bool ss;
                    signals(s, yy, ee, uu, ss, rr);
                    g.deriv(s, uu + dist, dx);
                    const bool hold = ss && ((rr > c.hi && ee > 0) || (rr < c.lo && ee < 0));
                    dx[n] = hold ? 0.0 : ee;
                    dx[n + 1] = c.nf * (ee - s[n + 1]);
                }, k);
            }
            yPrev = y;
            bool bad = false;
            for (double& v : x) if (!std::isfinite(v) || std::fabs(v) > 1e30) bad = true;
            if (bad) { std::fill(x.begin(), x.end(), 0.0); warning = "the loop blew up (unstable): started again"; }
        }
    }
    void drawView(const Draw& d, const sf::FloatRect& r) override {
        if (!g.err.empty()) { d.rect(r, sf::Color(8, 10, 12), sf::Color(50, 50, 58)); return; }
        // the open loop L(s) = H C(s) G(s): gain and phase margins are read here
        std::vector<std::complex<double>> pz = polyRoots(g.den);
        auto zz = g.num.size() > 1 ? polyRoots(g.num) : std::vector<std::complex<double>>();
        pz.insert(pz.end(), zz.begin(), zz.end());
        if (c.nf > 0 && c.kd != 0) pz.push_back(-c.nf);
        double w0, w1;
        bodeRange(pz, w0, w1);
        const double H = num("h");
        drawBode(d, r, [&](double w) {
            const std::complex<double> s(0.0, w);
            return H * polyAt(pidNum, s) / polyAt(pidDen, s) * g.at(s);
        }, w0, w1);
        d.text("open loop H C G", r.position.x + r.size.x - d.width("open loop H C G", 9) - 4, r.position.y + 14, 9, sf::Color(150, 150, 160));
    }
};

// ---------------------------------------------------------------------
//  Step response metrics: after every step of r, how y follows it
// ---------------------------------------------------------------------
class StepInfoBlock : public Block {
    struct Seg { double r0 = 0, r1 = 0, y0 = 0; std::vector<float> y; };
    Seg cur, done;
    bool haveDone = false, started = false;
    double rate = 0.0, lastR = 0.0;
    size_t sinceMeasure = 0;
    struct M { bool ok = false, settled = true; double tr = 0, tp = 0, os = 0, ts = 0, fin = 0, ess = 0, essPct = 0, len = 0; };
    M mDone, mCur;
    M measure(const Seg& s) const {
        M m;
        const double D = s.r1 - s.r0;
        if (s.y.size() < 4 || std::fabs(D) < 1e-12 || rate <= 0) return m;
        const size_t n = s.y.size(), tail = std::max<size_t>(1, n / 20);
        double fin = 0;
        for (size_t i = n - tail; i < n; i++) fin += (s.y[i] - s.y0) / D;
        fin /= tail;
        m.fin = s.y0 + fin * D;
        m.len = n / rate;
        double peak = -1e300; size_t ip = 0;
        long i10 = -1, i90 = -1;
        for (size_t i = 0; i < n; i++) {
            const double p = (s.y[i] - s.y0) / D;
            if (p > peak) { peak = p; ip = i; }
            if (fin > 0) {
                if (i10 < 0 && p >= 0.1 * fin) i10 = (long)i;
                if (i90 < 0 && p >= 0.9 * fin) i90 = (long)i;
            }
        }
        m.tr = i10 >= 0 && i90 >= 0 ? (i90 - i10) / rate : -1;
        m.tp = ip / rate;
        m.os = fin > 0 ? std::max(0.0, (peak - fin) / fin * 100.0) : 0.0;
        long last = -1;
        for (size_t i = 0; i < n; i++) if (std::fabs((s.y[i] - s.y0) / D - fin) > 0.02 * std::fabs(fin)) last = (long)i;
        m.ts = last < 0 ? 0.0 : (last + 1) / rate;
        m.settled = last < (long)(n - tail);   // still moving in the last 5 %: not settled yet
        m.ess = s.r1 - m.fin;
        m.essPct = m.ess / D * 100.0;
        m.ok = true;
        return m;
    }
public:
    StepInfoBlock() {
        type = "stepinfo"; title = "Step response (metrics)"; category = "Control";
        ins = {{"r (input step)", PT::Real}, {"y (response)", PT::Real}};
        syncInputs = true;
        addNum("thr", "Step: a jump above", 0.001, 1e-9, 1e9, 0.001, "", 4);
    }
    void reset() override { started = false; haveDone = false; cur = Seg(); }
    sf::Vector2f viewSize() const override { return {300.0f, 128.0f}; }
    void process(const std::vector<const Stream*>& in) override {
        if (!in[0] || !in[1] || in[0]->rate <= 0) return;
        rate = in[0]->rate;
        const double thr = num("thr");
        const size_t n = std::min(in[0]->re.size(), in[1]->re.size());
        const size_t cap = (size_t)std::min(rate * 600.0, 2e6);   // 10 minutes of one step (2 M samples) at most
        for (size_t i = 0; i < n; i++) {
            const double r = in[0]->re[i], y = in[1]->re[i];
            if (!started) { started = true; lastR = r; }
            if (std::fabs(r - lastR) > thr) {
                // a step: the one before is complete
                if (!cur.y.empty() && std::fabs(cur.r1 - cur.r0) > thr) { done = std::move(cur); haveDone = true; mDone = measure(done); }
                cur = Seg(); cur.r0 = lastR; cur.r1 = r; cur.y0 = y;
            }
            lastR = r;
            if (cur.y.size() < cap) cur.y.push_back((float)y);
        }
        // the running step: measured ~4 times a second (it may be long)
        sinceMeasure += n;
        if (sinceMeasure > rate * 0.25 || cur.y.size() < rate * 0.25) { mCur = measure(cur); sinceMeasure = 0; }
    }
    void drawView(const Draw& d, const sf::FloatRect& r) override {
        d.rect(r, sf::Color(8, 10, 12), sf::Color(50, 50, 58));
        // the last complete step; the running one until there is one (or when it is longer)
        const bool useCur = !haveDone || (mCur.ok && mCur.len > mDone.len);
        const M& m = useCur ? mCur : mDone;
        const Seg& s = useCur ? cur : done;
        float y = r.position.y + 4;
        const float x = r.position.x + 6;
        auto row = [&](const std::string& a, const std::string& b, sf::Color c = sf::Color(225, 225, 232)) {
            d.text(a, x, y, 10, sf::Color(150, 150, 162));
            d.text(b, x + 130, y, 10, c);
            y += 13;
        };
        if (!m.ok) { d.text("waiting for a step of r (e.g. a square wave)", x, y, 10, sf::Color(120, 120, 130)); return; }
        char b[96];
        std::snprintf(b, sizeof(b), "%.4g -> %.4g%s", s.r0, s.r1, useCur ? "  (measuring)" : "  (last step)");
        row("step of r", b, useCur ? sf::Color(240, 190, 90) : sf::Color(140, 220, 140));
        if (m.tr >= 0) { std::snprintf(b, sizeof(b), "%.4g s", m.tr); row("rise time 10-90 %", b); } else row("rise time 10-90 %", "-");
        std::snprintf(b, sizeof(b), "%.1f %%   (peak at %.4g s)", m.os, m.tp); row("overshoot", b);
        const sf::Color warn(240, 150, 80), norm(225, 225, 232);
        if (m.settled) { std::snprintf(b, sizeof(b), "%.4g s", m.ts); row("settling time 2 %", b); }
        else { std::snprintf(b, sizeof(b), "not settled in %.4g s", m.len); row("settling time 2 %", b, warn); }
        std::snprintf(b, sizeof(b), "%.5g%s", m.fin, m.settled ? "" : "  (not final yet)"); row("final value", b, m.settled ? norm : warn);
        std::snprintf(b, sizeof(b), "%.4g  (%.2f %% of the step)", m.ess, m.essPct); row("steady-state error", b, m.settled ? norm : warn);
        std::snprintf(b, sizeof(b), "%.4g s", m.len); row("time since the step", b);
        if (!m.settled) d.text(useCur ? "(still moving)" : "(the step was too short: make r slower)", x, y, 9, sf::Color(150, 120, 100));
    }
};
