#pragma once
// (included from Pipeline.h inside namespace pl, after PipelineControl.h)

// =====================================================================
//  Picture building blocks for weather satellites (and any line-scan or
//  framed image):
//
//   Line sync (pattern) -> lines    an analog line-scan signal (APT, ...): cut into lines
//                                   that start where a sync pattern correlates best;
//                                   follows the slant (clock drift) line by line
//   Frame -> image line              a digital frame (bits after a Frame sync, or bytes of
//                                   a packet): N-bit samples from a given position, every
//                                   k-th (interleaved channels), as one image line
//                                   (NOAA HRPT AVHRR, Meteor HRPT, MetOp AHRPT, ...)
// =====================================================================

// Brightness of N-bit (or any) values to 0..255: fixed, or stretched between the 1st and
// 99th percentile of what came recently (slowly adapting, so lines do not flicker)
struct Stretch {
    double lo = 0.0, hi = 1.0;
    bool have = false;
    void learn(const std::vector<double>& v) {
        if (v.empty()) return;
        std::vector<double> s(v);
        std::sort(s.begin(), s.end());
        const double a = s[s.size() / 100], b = s[s.size() - 1 - s.size() / 100];
        if (!have) { lo = a; hi = b; have = true; }
        else { lo += 0.1 * (a - lo); hi += 0.1 * (b - hi); }
    }
    uint8_t px(double v) const {
        const double t = hi > lo ? (v - lo) / (hi - lo) : 0.5;
        return (uint8_t)std::clamp(std::lround(t * 255.0), 0L, 255L);
    }
};

// ---------------------------------------------------------------------
//  Analog lines aligned to a sync pattern
// ---------------------------------------------------------------------
class LineSyncBlock : public Block {
    std::deque<float> px;          // the signal at the pixel rate
    double binT = 0.0, binSum = 0.0;
    int binN = 0;
    float lastPx = 0.0f;
    std::vector<double> pat;       // the pattern, zero mean
    std::string patText;
    long long lines = 0;
    bool locked = false;
    double lastCorr = 0.0;
    int missed = 0;
    Stretch st;
    double rate = 0.0;
public:
    LineSyncBlock() {
        type = "linesync"; title = "Line sync (pattern) -> lines"; category = "Image";
        ins = {{"signal", PT::Real}}; setOuts({{"lines", PT::Lines}});
        addNum("width", "Pixels per line", 2080, 16, 16384, 1, "");
        addNum("lps", "Lines per second", 2, 0.01, 1000, 0.5, "", 3);
        addText("pat", "Sync pattern (0/1 per pixel)", "000011001100110011001100110011000000000");   // APT sync A
        addNum("min", "Lock above correlation", 0.5, 0.05, 1, 0.05, "", 2);
        addChoice("lev", "Brightness", {"automatic (1..99 %)", "given below"});
        addNum("black", "Black at", 0, -1e9, 1e9, 0.1, "", 3);
        addNum("white", "White at", 1, -1e9, 1e9, 0.1, "", 3);
        showOnly("black", "lev", {1}); showOnly("white", "lev", {1});
    }
    void reset() override { px.clear(); binT = 0; binSum = 0; binN = 0; locked = false; missed = 0; }
    std::string status() const override {
        char b[96];
        std::snprintf(b, sizeof(b), "%s, sync %.2f, %lld lines", locked ? "locked" : "searching", lastCorr, lines);
        return b;
    }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        passOn(in[0], o, PT::Lines, num("lps"));
        if (!in[0] || in[0]->rate <= 0) return;
        if (text("pat") != patText || in[0]->rate != rate) {
            patText = text("pat");
            rate = in[0]->rate;
            pat.clear();
            for (char c : patText) if (c == '0' || c == '1') pat.push_back(c == '1' ? 1.0 : 0.0);
            double m = 0; for (double v : pat) m += v;
            if (!pat.empty()) m /= pat.size();
            double e = 0; for (double& v : pat) { v -= m; e += v * v; }
            if (e > 0) for (double& v : pat) v /= std::sqrt(e);
            reset();
        }
        const size_t W = (size_t)num("width");
        const double P = W * num("lps");   // pixels per second
        // to the pixel rate: each pixel the mean of the input samples in it (repeat when none)
        const double step = P / in[0]->rate;
        for (float v : in[0]->re) {
            binSum += v; binN++;
            binT += step;
            while (binT >= 1.0) {
                binT -= 1.0;
                lastPx = binN ? (float)(binSum / binN) : lastPx;
                px.push_back(lastPx);
                binSum = 0; binN = 0;
            }
        }
        if (pat.empty()) { warning = "the sync pattern has no 0 / 1"; return; }
        const size_t M = pat.size();
        // the line starts where the pattern fits best: anywhere in a line while searching,
        // a few pixels around the expected place when locked
        const size_t SL = std::max<size_t>(4, W / 200);   // the window while locked
        for (;;) {
            const size_t S = locked ? SL : W;
            if (px.size() < 2 * S + W + M + 1) break;
            size_t best = 0; double bestC = -2.0;
            for (size_t k = 0; k <= 2 * S; k++) {
                double sum = 0, sum2 = 0, c = 0;
                for (size_t j = 0; j < M; j++) { const double v = px[k + j]; sum += v; sum2 += v * v; c += pat[j] * v; }
                const double var = sum2 - sum * sum / M;
                const double nc = var > 1e-30 ? c / std::sqrt(var) : 0.0;
                if (nc > bestC) { bestC = nc; best = k; }
            }
            lastCorr = bestC;
            size_t start;
            if (bestC >= num("min")) { start = best; locked = true; missed = 0; }
            else if (locked && ++missed < 20) start = S;          // keep the line's place (noise)
            else { locked = false; start = best; }
            // the line, to 0..255
            std::vector<double> v(px.begin() + start, px.begin() + start + W);
            std::vector<uint8_t> line(W);
            if (choice("lev") == 0) {
                st.learn(v);
                for (size_t i = 0; i < W; i++) line[i] = st.px(v[i]);
            } else {
                const double b = num("black"), w = num("white");
                for (size_t i = 0; i < W; i++) line[i] = (uint8_t)std::clamp(std::lround((v[i] - b) / (w - b + 1e-30) * 255.0), 0L, 255L);
            }
            o.lines.push_back(std::move(line));
            lines++;
            // the next line is expected one line later: locked, its window is centred there
            const size_t next = start + W;
            const size_t drop = std::max<size_t>(1, locked ? (next > SL ? next - SL : 1) : next);
            px.erase(px.begin(), px.begin() + std::min(drop, px.size()));
        }
        if (px.size() > 20 * W) px.erase(px.begin(), px.end() - 4 * W);   // (never: safety)
    }
};

// ---------------------------------------------------------------------
//  A frame's samples -> one image line
// ---------------------------------------------------------------------
class FrameLinesBlock : public Block {
    std::vector<uint8_t> bits;     // the current frame (from its sync word on)
    bool inFrame = false, done = false;
    long long lines = 0, frames = 0;
    Stretch st;
    size_t need() const {
        const size_t nb = (size_t)num("nb"), first = (size_t)num("first"), cnt = (size_t)num("count"), stride = (size_t)std::max(1.0, num("stride"));
        return (first + (cnt - 1) * stride + 1) * nb;
    }
    void emit(Stream& o, const std::vector<uint8_t>& b) {
        const size_t nb = (size_t)num("nb"), first = (size_t)num("first"), cnt = (size_t)num("count"), stride = (size_t)std::max(1.0, num("stride"));
        std::vector<double> v(cnt);
        for (size_t i = 0; i < cnt; i++) {
            const size_t at = (first + i * stride) * nb;
            uint32_t x = 0;
            for (size_t k = 0; k < nb; k++) x = (x << 1) | (at + k < b.size() ? (b[at + k] & 1) : 0);
            v[i] = x;
        }
        std::vector<uint8_t> line(cnt);
        if (choice("lev") == 0) { st.learn(v); for (size_t i = 0; i < cnt; i++) line[i] = st.px(v[i]); }
        else { const int sh = std::max(0, (int)nb - 8); for (size_t i = 0; i < cnt; i++) line[i] = (uint8_t)std::min(255.0, (double)((uint32_t)v[i] >> sh)); }
        if (choice("flip") == 1) std::reverse(line.begin(), line.end());
        o.lines.push_back(std::move(line));
        lines++;
    }
public:
    FrameLinesBlock() {
        type = "framelines"; title = "Frame -> image line (N-bit samples)"; category = "Image";
        ins = {{"bits (after Frame sync)", PT::Sym}, {"frames (bytes)", PT::Frames}}; setOuts({{"lines", PT::Lines}});
        addNum("nb", "Bits per sample", 10, 1, 16, 1, "");
        addNum("first", "First sample (from the frame start)", 750, 0, 1e7, 1, "");
        addNum("stride", "Every k-th sample", 5, 1, 1024, 1, "");
        addNum("count", "Pixels", 2048, 1, 65536, 1, "");
        addChoice("lev", "Brightness", {"automatic (1..99 %)", "top 8 bits"});
        addChoice("flip", "Direction", {"as sent", "mirrored"});
    }
    std::string status() const override { char b[64]; std::snprintf(b, sizeof(b), "%lld frames, %lld lines", frames, lines); return b; }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        passOn(in[0] ? in[0] : in[1], o, PT::Lines, 0);
        const size_t n = need();
        // bits: a frame starts at each FRAME mark (the Frame sync puts its word after it)
        if (in[0]) for (uint8_t b : in[0]->sym) {
            if (b == FRAME) { bits.clear(); inFrame = true; done = false; frames++; continue; }
            if (!inFrame || done) continue;
            bits.push_back(b);
            if (bits.size() >= n) { emit(o, bits); done = true; }
        }
        // bytes: each frame (packet) on its own, most significant bit first
        if (in[1]) for (const auto& f : in[1]->frames) {
            frames++;
            std::vector<uint8_t> b;
            b.reserve(f.size() * 8);
            for (uint8_t c : f) for (int k = 7; k >= 0; k--) b.push_back((c >> k) & 1);
            if (b.size() >= n) emit(o, b);
        }
    }
};
