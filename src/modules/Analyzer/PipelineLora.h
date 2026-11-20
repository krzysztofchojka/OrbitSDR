#pragma once
// =====================================================================
//  LoRa (Semtech chirp spread spectrum, SX127x / SX126x / LLCC68 and the
//  modules built on them: EByte E22 / E32 / E220, RFM95, Heltec ...).
//  Included from Pipeline.h.
//
//  The physical layer as the chips send it (checked against gr-lora_sdr,
//  which works with real hardware):
//    preamble (up-chirps, value 0) - 2 sync word symbols ((sw >> 4) << 3,
//    (sw & 15) << 3) - 2.25 down-chirps - payload symbols.
//    TX: whitening -> Hamming (4/5 .. 4/8) -> diagonal interleaving ->
//        Gray decoding (binary = prefix XOR) -> +1 -> chirp value.
//    The first 8 symbols (the header block) always carry SF-2 bits per
//    symbol at CR 4/8; with low data rate optimisation every block does.
//    Explicit header: 5 nibbles (length, CR, CRC on/off, checksum).
//    CRC-16/CCITT (init 0) over the payload but its last two bytes, XORed
//    with them; sent little-endian and not whitened.
//
//  "LoRa receiver" finds the preamble on its own (spreading factor and
//  bandwidth can be "auto": every combination is searched at once), works
//  out the frequency and timing offsets from the up- and down-chirps, and
//  gives the payload of each packet as a frame. It shows the parameters it
//  found (SF, BW, CR, sync word, CFO, SNR), so an unknown transmitter such
//  as an EByte module can be measured first and then fixed in the block.
//  "LoRa modulator" makes packets from frames, for tests without a radio.
// =====================================================================

namespace lora {

inline const std::vector<double>& bandwidths() {
    static const std::vector<double> b = {7812.5, 10416.67, 15625, 20833.33, 31250, 41666.67, 62500, 125000, 250000, 500000};
    return b;
}
inline std::vector<std::string> bwNames(bool withAuto) {
    std::vector<std::string> v;
    if (withAuto) v.push_back("auto (125 / 250 / 500 kHz)");
    for (const char* s : {"7.8 kHz", "10.4 kHz", "15.6 kHz", "20.8 kHz", "31.25 kHz", "41.7 kHz", "62.5 kHz", "125 kHz", "250 kHz", "500 kHz"}) v.push_back(s);
    return v;
}

// binary from Gray (the transmitter's "Gray demap": prefix XOR)
inline int grayToBin(int g) { int b = g; for (int s = 1; s < 16; s++) b ^= g >> s; return b; }
inline int binToGray(int b) { return b ^ (b >> 1); }

inline const std::array<uint8_t, 256>& whitening() {
    static std::array<uint8_t, 256> w = [] {
        std::array<uint8_t, 256> a {};
        uint8_t x = 0xFF;
        for (int i = 0; i < 256; i++) {
            a[i] = x;
            uint8_t t = x & 0xB8, p = 0;
            while (t) { p ^= t & 1; t >>= 1; }
            x = (uint8_t)((x << 1) | p);   // FF FE FC F8 F0 E1 C2 85 0B 17 ...
        }
        return a;
    }();
    return w;
}

// a bit of an n-bit value, counted from the most significant one
inline int bitMsb(int v, int n, int i) { return (v >> (n - 1 - i)) & 1; }

// nibble -> codeword of 4 + cr bits (cr 1..4)
inline int hammingEnc(int nib, int cr) {
    const int d0 = (nib >> 3) & 1, d1 = (nib >> 2) & 1, d2 = (nib >> 1) & 1, d3 = nib & 1;   // data_bin[0..3], MSB first
    if (cr == 1) return d3 << 4 | d2 << 3 | d1 << 2 | d0 << 1 | (d0 ^ d1 ^ d2 ^ d3);
    const int p0 = d3 ^ d2 ^ d1, p1 = d2 ^ d1 ^ d0, p2 = d3 ^ d2 ^ d0, p3 = d3 ^ d1 ^ d0;
    return (d3 << 7 | d2 << 6 | d1 << 5 | d0 << 4 | p0 << 3 | p1 << 2 | p2 << 1 | p3) >> (4 - cr);
}
// codeword -> nibble (CR 4/7 and 4/8 correct one error)
inline int hammingDec(int cwv, int cr, bool* fixedOne = nullptr) {
    const int L = cr + 4;
    int c[8] = {0};
    for (int i = 0; i < L; i++) c[i] = bitMsb(cwv, L, i);
    int dn[4] = {c[3], c[2], c[1], c[0]};   // MSB first
    bool fix = cr == 3;
    if (cr == 4) { int ones = 0; for (int i = 0; i < 8; i++) ones += c[i]; fix = ones & 1; }
    if (fix) {
        const int s0 = c[0] ^ c[1] ^ c[2] ^ c[4], s1 = c[1] ^ c[2] ^ c[3] ^ c[5], s2 = c[0] ^ c[1] ^ c[3] ^ c[6];
        const int syn = s0 | s1 << 1 | s2 << 2;
        const int which = syn == 5 ? 3 : syn == 7 ? 2 : syn == 3 ? 1 : syn == 6 ? 0 : -1;
        if (which >= 0) { dn[which] ^= 1; if (fixedOne) *fixedOne = true; }
    }
    return dn[0] << 3 | dn[1] << 2 | dn[2] << 1 | dn[3];
}

// sfApp codewords of cwLen bits -> cwLen symbols of sfApp bits (diagonal interleaving)
inline std::vector<int> interleave(const std::vector<int>& cw, int cwLen, int sfApp) {
    std::vector<int> s(cwLen, 0);
    for (int i = 0; i < cwLen; i++)
        for (int j = 0; j < sfApp; j++) {
            const int r = ((i - j - 1) % sfApp + sfApp) % sfApp;
            s[i] |= bitMsb(cw[r], cwLen, i) << (sfApp - 1 - j);
        }
    return s;
}
inline std::vector<int> deinterleave(const std::vector<int>& s, int cwLen, int sfApp) {
    std::vector<int> cw(sfApp, 0);
    for (int i = 0; i < cwLen; i++)
        for (int j = 0; j < sfApp; j++) {
            const int r = ((i - j - 1) % sfApp + sfApp) % sfApp;
            cw[r] |= bitMsb(s[i], sfApp, j) << (cwLen - 1 - i);
        }
    return cw;
}

inline int headerChecksum(int n0, int n1, int n2) {
    auto b = [](int v, int k) { return (v >> k) & 1; };
    const int c4 = b(n0, 3) ^ b(n0, 2) ^ b(n0, 1) ^ b(n0, 0);
    const int c3 = b(n0, 3) ^ b(n1, 3) ^ b(n1, 2) ^ b(n1, 1) ^ b(n2, 0);
    const int c2 = b(n0, 2) ^ b(n1, 3) ^ b(n1, 0) ^ b(n2, 3) ^ b(n2, 1);
    const int c1 = b(n0, 1) ^ b(n1, 2) ^ b(n1, 0) ^ b(n2, 2) ^ b(n2, 1) ^ b(n2, 0);
    const int c0 = b(n0, 0) ^ b(n1, 1) ^ b(n2, 3) ^ b(n2, 2) ^ b(n2, 1) ^ b(n2, 0);
    return c4 << 4 | c3 << 3 | c2 << 2 | c1 << 1 | c0;
}

inline uint16_t crc16(const uint8_t* p, size_t n) {
    uint16_t c = 0;
    for (size_t i = 0; i < n; i++) {
        c ^= (uint16_t)(p[i] << 8);
        for (int k = 0; k < 8; k++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
    }
    return c;
}
inline uint16_t payloadCrc(const uint8_t* p, size_t n) {
    if (n < 2) return crc16(p, n);
    return (uint16_t)(crc16(p, n - 2) ^ p[n - 1] ^ (p[n - 2] << 8));
}

inline bool ldroFor(int mode, int sf, double bw) { return mode == 1 || (mode == 0 && (double)(1 << sf) / bw > 0.016); }

// payload -> chirp values (preamble and sync not included)
inline std::vector<int> encodeSymbols(const std::vector<uint8_t>& pay, int sf, int cr, bool crc, bool implicitHdr, bool ldro) {
    const int N = 1 << sf;
    std::vector<int> nib;
    const int len = (int)std::min<size_t>(pay.size(), 255);
    if (!implicitHdr) {
        const int n0 = len >> 4, n1 = len & 15, n2 = cr << 1 | (crc ? 1 : 0), ck = headerChecksum(n0, n1, n2);
        nib = {n0, n1, n2, ck >> 4, ck & 15};
    }
    const auto& w = whitening();
    for (int i = 0; i < len; i++) { const uint8_t b = pay[i] ^ w[i]; nib.push_back(b & 15); nib.push_back(b >> 4); }
    if (crc) {
        const uint16_t c = payloadCrc(pay.data(), len);
        for (uint8_t b : {(uint8_t)(c & 0xFF), (uint8_t)(c >> 8)}) { nib.push_back(b & 15); nib.push_back(b >> 4); }
    }
    std::vector<int> out;
    size_t k = 0;
    bool first = true;
    while (k < nib.size() || first) {
        const int sfApp = (first || ldro) ? sf - 2 : sf, crApp = first ? 4 : cr, cwLen = crApp + 4;
        std::vector<int> cw(sfApp);
        for (int r = 0; r < sfApp; r++) cw[r] = hammingEnc(k < nib.size() ? nib[k++] : 0, crApp);
        // reduced rate (header block, LDRO): the sf - sfApp low bits are 0, as the chips send them
        // (an SX1278 in an EByte E32 measured: every such symbol is 4 m + 1)
        for (int v : interleave(cw, cwLen, sfApp)) out.push_back(((grayToBin(v) << (sf - sfApp)) + 1) % N);
        first = false;
    }
    return out;
}

// A low-pass FIR (Blackman windowed sinc) that is computed only for the samples kept. I and Q
// sit in separate arrays, stored twice so that a window is always contiguous, and the sums run
// four at a time, which the compiler keeps in vector registers.
struct DecimFir {
    std::vector<float> h, re, im;
    size_t pos = 0, n = 0;
    // -6 dB at `cutoff`, the transition `trans` wide around it
    void design(double fs, double cutoff, double trans) {
        int taps = (int)std::ceil(5.5 * fs / trans) | 1;
        taps = std::clamp(taps, 15, 401);
        h.assign(taps, 0.0f);
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
        n = h.size();
        re.assign(2 * n, 0.0f); im.assign(2 * n, 0.0f); pos = 0;
    }
    void push(CF x) {
        re[pos] = re[pos + n] = x.real();
        im[pos] = im[pos + n] = x.imag();
        pos = pos + 1 == n ? 0 : pos + 1;
    }
    CF output() const {
        const float* a = &re[pos];
        const float* b = &im[pos];
        const float* c = h.data();
        float r0 = 0, r1 = 0, r2 = 0, r3 = 0, i0 = 0, i1 = 0, i2 = 0, i3 = 0;
        size_t k = 0;
        for (; k + 4 <= n; k += 4) {
            r0 += a[k] * c[k]; r1 += a[k + 1] * c[k + 1]; r2 += a[k + 2] * c[k + 2]; r3 += a[k + 3] * c[k + 3];
            i0 += b[k] * c[k]; i1 += b[k + 1] * c[k + 1]; i2 += b[k + 2] * c[k + 2]; i3 += b[k + 3] * c[k + 3];
        }
        float R = (r0 + r1) + (r2 + r3), I = (i0 + i1) + (i2 + i3);
        for (; k < n; k++) { R += a[k] * c[k]; I += b[k] * c[k]; }
        return CF(R, I);
    }
};

}  // namespace lora

// ---------------------------------------------------------------------
//  Receiver
// ---------------------------------------------------------------------
class LoraRx : public Block {
    struct Rx {
        int sf = 7, N = 128;
        int st = 0;                  // 0 search, 1 preamble found: wait for the down-chirps, 2 payload
        double pos = 0;              // the next window, in chips (one chip = 1 / BW; the buffer holds 2 samples per chip)
        int run = 0, lastK = -1, kPre = 0, miss = 0;
        double preFirst = 0, preLast = 0;
        double cfo = 0.0;            // bins
        double p0 = 0;               // where the payload starts (chips, with its fraction)
        int v1 = 0, v2 = 0;
        std::vector<int> syms;
        bool hdrDone = false;
        int need = 0, len = 0, cr = 1;
        bool crc = false, ldro = false, ldroAuto = true;
        double snrAcc = 0.0; int snrN = 0;
    };
    struct Chan {
        double bw = 125000;
        lora::DecimFir lp;
        bool useLp = false;
        int D = 1, dcnt = 0;         // decimation before the interpolation
        CF h[4] = {};
        double mu = 0.0, step = 1.0;
        std::vector<CF> buf;         // at 2 x BW (a window can then start between two chips)
        long long base = 0;          // absolute index of buf[0]
        double endChips() const { return (base + (double)buf.size()) / 2.0; }
        std::vector<Rx> rx;
    };
    std::vector<Chan> chans;
    std::vector<Complex> work;
    std::vector<std::vector<CF>> upc;   // base up-chirp per SF (index sf)
    std::deque<std::string> recent;
    long long good = 0, badCrc = 0, hdrErr = 0, syncSeen = 0, wrongSync = 0;
    std::string last, lastParams, searching;
    double inRate = 0.0;

    // the base up-chirp at two samples per chip, no wrap (-BW/2 .. +BW/2 over 2N samples)
    const std::vector<CF>& chirp(int sf) {
        if ((int)upc.size() <= sf) upc.resize(sf + 1);
        auto& u = upc[sf];
        if (u.empty()) {
            const int N = 1 << sf;
            u.resize(2 * N);
            for (int j = 0; j < 2 * N; j++) {
                const double t = j / 2.0;   // chips
                const double ph = PI_D * t * t / N - PI_D * t;
                u[j] = CF((float)std::cos(ph), (float)std::sin(ph));
            }
        }
        return u;
    }
    std::vector<double> fold;
    // Dechirps the N chips from chip time p (up: by conj(up-chirp), else by the up-chirp, for
    // down-chirps), corrects `cfo` bins, FFT (zero padded `pad` times). Peak bin (in padded bins),
    // its power and the total power.
    // At two samples per chip the received chirp's wrap (+BW/2 -> -BW/2) is a real jump, so the
    // dechirped tone sits at k for part of the symbol and at k - N for the rest: the 2N-point
    // spectrum is folded (|X[k]|^2 + |X[k+N]|^2). At one sample per chip both parts would alias
    // onto k but with a phase jump of 2 pi x (the timing's fraction) between them, which splits
    // the peak; folded, no timing offset does.
    int window(Chan& c, Rx& r, double p, bool up, double cfo, int pad, double& P, double& T) {
        const int N = r.N, M = 2 * N * pad, H = N * pad;
        const auto& u = chirp(r.sf);
        work.assign(M, Complex(0, 0));
        const double q = 2.0 * p - (double)c.base;
        const long long qi = (long long)std::floor(q);
        const float mu = (float)(q - (double)qi);
        const bool exact = mu < 1e-4f;
        const long long last = (long long)c.buf.size() - 3;
        const double w = -PI_D * cfo / N;   // per half chip
        Complex rot(1, 0);
        const Complex st(std::cos(w), std::sin(w));
        for (int j = 0; j < 2 * N; j++) {
            const long long i = std::clamp<long long>(qi + j, 1, std::max<long long>(1, last));
            CF xv;
            if (exact) xv = c.buf[(size_t)i];
            else {
                const CF p0 = c.buf[(size_t)i - 1], p1 = c.buf[(size_t)i], p2 = c.buf[(size_t)i + 1], p3 = c.buf[(size_t)i + 2];
                const float m2 = mu * mu, m3 = m2 * mu;
                xv = 0.5f * (2.0f * p1 + (p2 - p0) * mu + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * m2 + (3.0f * p1 - p0 - 3.0f * p2 + p3) * m3);
            }
            const CF ref = up ? std::conj(u[j]) : u[j];
            const CF v = xv * ref;
            work[j] = Complex(v.real(), v.imag()) * rot;
            rot *= st;
        }
        fft(work);
        fold.resize(H);
        int k = 0;
        double best = -1.0, tot = 0.0;
        for (int i = 0; i < H; i++) {
            const double e = std::norm(work[i]) + std::norm(work[i + H]);
            fold[i] = e;
            tot += e;
            if (e > best) { best = e; k = i; }
        }
        P = best; T = tot;
        return k;
    }
    static double par(double P, double T, int N) { return P / std::max(1e-30, (T - P) / std::max(1, N - 1)); }
    static int circ(int a, int b, int N) { int d = ((a - b) % N + N) % N; return std::min(d, N - d); }
    // a fractional peak (8x zero padding), in bins [0, N)
    double fine(Chan& c, Rx& r, double p, bool up) {
        double P, T;
        const int k = window(c, r, p, up, 0.0, 8, P, T);
        const int M = r.N * 8;
        const double a = std::sqrt(fold[(k + M - 1) % M]), b = std::sqrt(fold[k]), d = std::sqrt(fold[(k + 1) % M]);
        const double den = a - 2 * b + d;
        const double off = std::fabs(den) > 1e-30 ? 0.5 * (a - d) / den : 0.0;
        return std::fmod((k + std::clamp(off, -0.5, 0.5)) / 8.0 + r.N, (double)r.N);
    }

    int wantSf() const { return choice("sf") == 0 ? 0 : choice("sf") + 6; }
    void reconfigure(double fs) {
        chans.clear();
        std::vector<double> bws;
        if (choice("bw") == 0) bws = {125000, 250000, 500000};
        else bws = {lora::bandwidths()[std::clamp(choice("bw") - 1, 0, 9)]};
        for (double bw : bws) {
            if (fs < bw * 1.5) continue;
            Chan c;
            c.bw = bw;
            // The chirp fills -BW/2 .. +BW/2 right to the edge: a filter that cuts there bends the
            // chirp's ends and the peaks move by up to a bin. Flat to 0.55 BW keeps the chirp
            // whole; it only has to be down by BW, where the output at 2 x BW folds (wider than
            // that costs taps: the noise between 0.5 and 0.95 BW that folds back costs about 1 dB).
            // It is computed only for every D-th input (kept at 2.4 x BW or more, which the cubic
            // interpolation to 2 x BW handles): at 2 MS/s and three bandwidths full-rate filters
            // took more than a core, and the pipeline fell behind.
            c.useLp = fs > bw * 2.4;
            c.D = c.useLp ? std::max(1, (int)std::floor(fs / (bw * 2.4))) : 1;
            c.step = fs / c.D / (2.0 * bw);
            if (c.useLp) c.lp.design(fs, bw * 0.75, bw * 0.4);
            for (int sf = 7; sf <= 12; sf++) {
                if (wantSf() && sf != wantSf()) continue;
                Rx r; r.sf = sf; r.N = 1 << sf;
                c.rx.push_back(r);
            }
            chans.push_back(std::move(c));
        }
    }
    void toSearch(Rx& r) { r.st = 0; r.run = 0; r.lastK = -1; r.miss = 0; r.syms.clear(); r.hdrDone = false; }

    void sync(Chan& c, Rx& r, double pd) {
        const int N = r.N;
        const double pa = std::max(r.preFirst, r.preLast - N);
        // a = e + cfo, b = -e + cfo (mod N): e = how far into a chirp the windows start. Windows
        // that start between two chips see a split peak (the phase jumps at the chirp's wrap), so
        // the estimate is made again with both windows moved to start on a whole chip.
        auto wrap = [&](double v) { v = std::fmod(v + 1.5 * N, (double)N) - 0.5 * N; return v; };
        double sh = 0.0, cfo = 0.0, e = 0.0;
        for (int it = 0; it < 3; it++) {
            const double a = fine(c, r, pa + sh, true), b = fine(c, r, pd + sh, false);
            const double s = std::fmod(a + b + 2.0 * N, (double)N);
            const double d1 = wrap(s / 2), d2 = wrap(s / 2 + N / 2.0);
            cfo = std::fabs(d1) <= std::fabs(d2) ? d1 : d2;
            e = std::fmod(a - cfo + 2.0 * N, (double)N);
            if (it < 2) sh += std::round(e) - e;
        }
        // the window at pd + sh starts e chips into a chirp: a boundary at most a chip after pd
        const double b1 = pd + sh - (e > N - 0.5 ? e - N : e);
        // the two whole down-chirps start at D and D + N; D is b1 - N, b1 or b1 + N (the window
        // picked may hold the end of the second sync symbol)
        double Pu, Tu, Pd, Td;
        double E[4];
        for (int k = 0; k < 4; k++) { window(c, r, b1 + (k - 1) * N, false, cfo, 1, Pd, Td); window(c, r, b1 + (k - 1) * N, true, cfo, 1, Pu, Tu); E[k] = Pd - Pu; }
        int bestK = 0;
        for (int k = 1; k < 3; k++) if (E[k] + E[k + 1] > E[bestK] + E[bestK + 1]) bestK = k;
        const double D = b1 + (bestK - 1) * N;
        const int kd = window(c, r, D, false, cfo, 1, Pd, Td);
        if (circ(kd, 0, N) > 1 || par(Pd, Td, N) < 6.0) { toSearch(r); r.pos = pd + N; return; }
        r.v1 = window(c, r, D - 2 * N, true, cfo, 1, Pu, Tu);
        r.v2 = window(c, r, D - N, true, cfo, 1, Pu, Tu);
        syncSeen++;
        const std::string want = text("sw");
        if (!want.empty() && want != "any") {
            const int sw = (int)std::strtol(want.c_str(), nullptr, 0) & 0xFF;
            if (circ(r.v1, ((sw >> 4) & 15) << 3, N) > 2 || circ(r.v2, (sw & 15) << 3, N) > 2) { wrongSync++; toSearch(r); r.pos = D + 2 * N; return; }
        }
        r.cfo = cfo;
        r.p0 = D + N * 2.25;
        r.pos = r.p0;
        r.st = 2;
        r.syms.clear(); r.hdrDone = false; r.snrAcc = 0.0; r.snrN = 0;
        r.ldro = lora::ldroFor(choice("ldro"), r.sf, c.bw);
        r.ldroAuto = choice("ldro") == 0;
        if (choice("hdr") == 1) {   // implicit: the length, CR and CRC are given
            r.len = (int)num("len"); r.cr = choice("cr") + 1; r.crc = choice("crc") == 0;
        }
    }

    // the nibbles of a block of symbols (raw FFT bins)
    std::vector<int> block(const Rx& r, size_t from, int cwLen, int sfApp, int crApp, int& fixes) const {
        std::vector<int> v;
        const int sh = r.sf - sfApp;
        for (int i = 0; i < cwLen; i++) {
            const int raw = ((r.syms[from + i] - 1) % r.N + r.N) % r.N;
            // reduced rate: the low bits are 0 when sent, so a peak a bin off rounds back
            const int val = sh ? ((raw + (1 << (sh - 1))) >> sh) & ((1 << sfApp) - 1) : raw;
            v.push_back(lora::binToGray(val));
        }
        std::vector<int> nib;
        for (int cw : lora::deinterleave(v, cwLen, sfApp)) { bool f = false; nib.push_back(lora::hammingDec(cw, crApp, &f)); fixes += f; }
        return nib;
    }
    int symbolsFor(const Rx& r, bool ldro) const {
        const int total = 2 * r.len + (r.crc ? 4 : 0);
        const int inHdr = choice("hdr") == 1 ? r.sf - 2 : r.sf - 7;
        const int sfApp = ldro ? r.sf - 2 : r.sf;
        const int rest = std::max(0, total - inHdr);
        return 8 + (rest + sfApp - 1) / sfApp * (r.cr + 4);
    }
    // the payload as sent with or without low data rate optimisation; false: bad CRC
    bool decodeAs(const Rx& r, bool ldro, std::vector<uint8_t>& pay) const {
        int fixes = 0;
        std::vector<int> nib = block(r, 0, 8, r.sf - 2, 4, fixes);
        if (choice("hdr") == 0) nib.erase(nib.begin(), nib.begin() + 5);
        const int sfApp = ldro ? r.sf - 2 : r.sf, n = std::min((int)r.syms.size(), symbolsFor(r, ldro));
        for (int s = 8; s + r.cr + 4 <= n; s += r.cr + 4) {
            auto b = block(r, (size_t)s, r.cr + 4, sfApp, r.cr, fixes);
            nib.insert(nib.end(), b.begin(), b.end());
        }
        pay.clear();
        const auto& w = lora::whitening();
        for (int i = 0; i < r.len && 2 * i + 1 < (int)nib.size(); i++) pay.push_back((uint8_t)(((nib[2 * i + 1] << 4) | nib[2 * i]) ^ w[i]));
        if (!r.crc) return true;
        if ((int)nib.size() < 2 * r.len + 4) return false;
        const int c0 = nib[2 * r.len + 1] << 4 | nib[2 * r.len], c1 = nib[2 * r.len + 3] << 4 | nib[2 * r.len + 2];
        return (uint16_t)(c0 | c1 << 8) == lora::payloadCrc(pay.data(), pay.size());
    }
    // LDRO from the payload symbols: with it their two low bits are 0 (raw = 4 m), without it
    // they are random (a quarter of them 0)
    bool looksLdro(const Rx& r) const {
        const int n = std::min((int)r.syms.size(), symbolsFor(r, false));
        int zero = 0, all = 0;
        for (int s = 8; s < n; s++, all++) zero += ((((r.syms[s] - 1) % r.N + r.N) % r.N) & 3) == 0;
        return all > 0 && zero > 0.6 * all;
    }
    void finish(Chan& c, Rx& r, Stream& o) {
        std::vector<uint8_t> pay;
        bool ok;
        if (r.ldroAuto) {
            // auto: what the symbols say first, the other way if its CRC fails. (The 16 ms rule
            // is Semtech's advice, not what modules do: an EByte E32 sends SF7 / 500 kHz with LDRO.)
            r.ldro = looksLdro(r);
            ok = decodeAs(r, r.ldro, pay);
            std::vector<uint8_t> alt;
            if (!ok && r.crc && decodeAs(r, !r.ldro, alt)) { ok = true; r.ldro = !r.ldro; pay = alt; }
        } else ok = decodeAs(r, r.ldro, pay);
        const int used = std::min((int)r.syms.size(), symbolsFor(r, r.ldro));
        if (ok) good++; else badCrc++;
        const double snr = r.snrN ? 10.0 * std::log10(std::max(1e-12, r.snrAcc / r.snrN)) : 0.0;
        char sw[16] = "?";
        // the sync symbols are nibble x 8 (+-1 left over from the timing)
        const int s1 = ((r.v1 + 4) / 8) & 15, s2 = ((r.v2 + 4) / 8) & 15;
        if (circ(r.v1, s1 * 8, r.N) <= 1 && circ(r.v2, s2 * 8, r.N) <= 1) std::snprintf(sw, sizeof(sw), "0x%X%X", s1, s2);
        char b[200];
        std::snprintf(b, sizeof(b), "SF%d  BW %.4g kHz  CR 4/%d  %s%s  sync %s  CFO %+.2f kHz  SNR %.1f dB%s",
                      r.sf, c.bw / 1000.0, r.cr + 4, choice("hdr") == 0 ? "explicit header" : "implicit", r.crc ? "" : ", no CRC",
                      sw, r.cfo * c.bw / r.N / 1000.0, snr, r.ldro ? "  LDRO" : "");
        lastParams = b;
        std::string line;
        {
            char t[16]; const std::time_t now = std::time(nullptr); std::strftime(t, sizeof(t), "%H:%M:%S ", std::localtime(&now));
            line = t;
            std::snprintf(b, sizeof(b), "%3d B %s  ", (int)pay.size(), !r.crc ? "     " : ok ? "CRC ok" : "BAD CRC");
            line += b;
            for (uint8_t ch : pay) line += (ch >= 32 && ch < 127) ? (char)ch : '.';
        }
        recent.push_back(line);
        while (recent.size() > 40) recent.pop_front();
        if (ok || choice("bad") == 1) { o.frames.push_back(pay); if (o.frames.size() == 1) o.newBurst = true; }
        toSearch(r);
        r.pos = std::floor(r.p0 + (double)used * r.N);
    }

    // one window of one receiver
    void stepRx(Chan& c, Rx& r, Stream& o) {
        const int N = r.N;
        double P, T;
        if (r.st == 0) {
            const int k = window(c, r, r.pos, true, 0.0, 1, P, T);
            const bool valid = par(P, T, N) > 8.0;
            if (valid && r.lastK >= 0 && circ(k, r.lastK, N) <= 1) r.run++;
            else { r.run = valid ? 1 : 0; r.preFirst = r.pos; }
            r.lastK = valid ? k : -1;
            if (valid) r.preLast = r.pos;
            r.pos += N;
            if (r.run >= 4) { r.st = 1; r.kPre = k; r.miss = 0; }
            return;
        }
        if (r.st == 1) {
            double Pd, Td;
            const int ku = window(c, r, r.pos, true, 0.0, 1, P, T);
            window(c, r, r.pos, false, 0.0, 1, Pd, Td);
            if (Pd > 2.0 * P && par(Pd, Td, N) > 8.0) {
                double Pd2, Td2;
                window(c, r, r.pos + N, false, 0.0, 1, Pd2, Td2);
                sync(c, r, Pd2 > Pd ? r.pos + N : r.pos);
                return;
            }
            if (par(P, T, N) > 8.0 && circ(ku, r.kPre, N) <= 1) { r.preLast = r.pos; r.miss = 0; }
            else if (++r.miss > 4) toSearch(r);
            r.pos += N;
            return;
        }
        // payload
        const int k = window(c, r, r.pos, true, r.cfo, 1, P, T);
        const double noise = (T - P) / std::max(1, N - 1);
        r.snrAcc += std::max(0.0, P - noise) / std::max(1e-30, noise * N);
        r.snrN++;
        r.syms.push_back(k);
        r.pos += N;
        if (!r.hdrDone && r.syms.size() == 8) {
            r.hdrDone = true;
            if (choice("hdr") == 0) {
                int fixes = 0;
                const auto nib = block(r, 0, 8, r.sf - 2, 4, fixes);
                r.len = nib[0] << 4 | nib[1];
                r.crc = nib[2] & 1;
                r.cr = nib[2] >> 1;
                if (lora::headerChecksum(nib[0], nib[1], nib[2]) != ((nib[3] & 1) << 4 | nib[4]) || r.len == 0 || r.cr < 1 || r.cr > 4) {
                    // (with SF / BW on auto, other combinations with the same chirp slope see the
                    // same preamble: their failed headers are no errors)
                    if (choice("sf") != 0 && choice("bw") != 0) hdrErr++;
                    toSearch(r); r.pos = std::floor(r.p0);
                    return;
                }
            }
            // auto LDRO: enough symbols for the longer of the two, decided in finish()
            r.need = r.ldroAuto ? std::max(symbolsFor(r, false), symbolsFor(r, true)) : symbolsFor(r, r.ldro);
        }
        if (r.hdrDone && (int)r.syms.size() >= r.need) finish(c, r, o);
    }

public:
    LoraRx() {
        type = "lora"; title = "LoRa receiver"; category = "Demodulate"; ins = {{"in", PT::IQ}}; setOuts({{"frames", PT::Frames}});
        addChoice("sf", "Spreading factor", {"auto (7..12)", "SF7", "SF8", "SF9", "SF10", "SF11", "SF12"});
        addChoice("bw", "Bandwidth", lora::bwNames(true), 0);
        addText("sw", "Sync word (hex, or any)", "any");
        addChoice("hdr", "Header", {"explicit (normal)", "implicit (fixed length)"});
        addNum("len", "Implicit: length", 16, 1, 255, 1, "bytes");
        addChoice("cr", "Implicit: coding rate", {"4/5", "4/6", "4/7", "4/8"});
        addChoice("crc", "Implicit: CRC", {"on", "off"});
        addChoice("ldro", "Low data rate optim.", {"auto (detected)", "on", "off"});
        addChoice("bad", "Bad CRC", {"drop", "pass on"});
        for (const char* k : {"len", "cr", "crc"}) showOnly(k, "hdr", {1});
    }
    void reset() override { chans.clear(); inRate = 0.0; seenVersion = -1; }
    const std::string& lastPacketParams() const { return lastParams; }
    std::vector<std::string> buttons() const override { return {"Copy", "Clear"}; }
    void onButton(int i) override {
        if (i == 1) { recent.clear(); good = badCrc = hdrErr = syncSeen = wrongSync = 0; lastParams.clear(); return; }
        std::string t = lastParams + "\n";
        for (const auto& l : recent) t += l + "\n";
        sf::Clipboard::setString(sf::String::fromUtf8(t.begin(), t.end()));
    }
    std::string status() const override {
        char b[160];
        std::snprintf(b, sizeof(b), "%lld packets, %lld bad CRC, %lld header errors%s", good, badCrc, hdrErr, searching.c_str());
        return b;
    }
    sf::Vector2f viewSize() const override { return {520.0f, 190.0f}; }
    void drawView(const Draw& d, const sf::FloatRect& r) override {
        d.rect(r, sf::Color(6, 10, 14), sf::Color(50, 60, 75));
        float y = r.position.y + 3;
        auto line = [&](std::string s, sf::Color col) {
            while (!s.empty() && d.width(s, 10) > r.size.x - 8) s.pop_back();
            d.text(s, r.position.x + 4, y, 10, col);
            y += 13.0f;
        };
        if (inRate > 0 && chans.empty()) line("input at " + std::to_string((int)inRate) + " S/s: needs 1.5 x the bandwidth or more (widen the channel)", sf::Color(255, 120, 110));
        line(lastParams.empty() ? "no packet yet (the parameters found are shown here)" : "last: " + lastParams, sf::Color(255, 200, 110));
        char b[96];
        std::snprintf(b, sizeof(b), "preambles with sync %lld, wrong sync word %lld", syncSeen, wrongSync);
        line(b, sf::Color(130, 140, 160));
        const int rows = std::max(1, (int)((r.position.y + r.size.y - y - 2) / 13.0f));
        const size_t from = recent.size() > (size_t)rows ? recent.size() - rows : 0;
        for (size_t k = from; k < recent.size(); k++) line(recent[k], recent[k].find("BAD") != std::string::npos ? sf::Color(240, 130, 120) : sf::Color(140, 255, 160));
    }
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0];
        o.clear(); o.type = PT::Frames;
        if (!in[0] || in[0]->rate <= 0) return;
        o.rate = in[0]->rate;
        if (changed(in[0]->rate)) { inRate = in[0]->rate; reconfigure(inRate); }
        for (Chan& c : chans) {
            // to twice the bandwidth: low-pass, then cubic interpolation
            for (CF x : in[0]->iq) {
                CF y = x;
                if (c.useLp) {
                    c.lp.push(x);
                    if (++c.dcnt < c.D) continue;
                    c.dcnt = 0;
                    y = c.lp.output();
                }
                c.h[0] = c.h[1]; c.h[1] = c.h[2]; c.h[2] = c.h[3]; c.h[3] = y;
                while (c.mu < 1.0) {
                    const float m = (float)c.mu, m2 = m * m, m3 = m2 * m;
                    const CF p0 = c.h[0], p1 = c.h[1], p2 = c.h[2], p3 = c.h[3];
                    c.buf.push_back(0.5f * (2.0f * p1 + (p2 - p0) * m + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * m2 + (3.0f * p1 - p0 - 3.0f * p2 + p3) * m3));
                    c.mu += c.step;
                }
                c.mu -= 1.0;
            }
            const double end = c.endChips();
            double keep = end;
            for (Rx& r : c.rx) {
                // a receiver works while there is room for four windows ahead (the down-chirp check)
                while (r.pos + 4.0 * r.N + 2.0 <= end) {
                    const double first = c.base / 2.0 + 8.0 * r.N;   // look-back room
                    if (r.pos < first) { r.pos = std::ceil(first); continue; }
                    stepRx(c, r, o);
                }
                keep = std::min(keep, std::min(r.pos, r.st == 2 ? r.p0 : r.pos) - 8.0 * r.N);
            }
            // samples no receiver will look at again
            const long long keepIdx = (long long)std::floor(2.0 * keep) - 4;
            if (keepIdx - c.base > 65536) {
                const long long drop = std::min<long long>(keepIdx - c.base, (long long)c.buf.size());
                c.buf.erase(c.buf.begin(), c.buf.begin() + (size_t)drop);
                c.base += drop;
            }
            // too far behind (a very slow machine): skip ahead
            if (c.buf.size() > (size_t)(c.bw * 8)) {
                const long long drop = (long long)c.buf.size() - (long long)(c.bw * 4);
                c.buf.erase(c.buf.begin(), c.buf.begin() + (size_t)drop);
                c.base += drop;
                for (Rx& r : c.rx) { toSearch(r); r.pos = std::max(r.pos, std::ceil(c.base / 2.0 + 8.0 * r.N)); }
            }
        }
        searching.clear();
        for (const Chan& c : chans) for (const Rx& r : c.rx) if (r.st == 2) {
            char b[48]; std::snprintf(b, sizeof(b), "  receiving SF%d / %.4g kHz", r.sf, c.bw / 1000.0); searching = b;
        }
    }
};

// ---------------------------------------------------------------------
//  Modulator (frames -> I/Q), for tests and loopback labs
// ---------------------------------------------------------------------
class LoraMod : public Block {
    GenClock clk;
    std::deque<CF> q;
    double phase = 0.0;
    long long sent = 0;
    void chirp(double fs, double bw, int N, int value, bool up, double chips) {
        const double os = fs / bw;
        const long long n = (long long)std::llround(chips * os);
        for (long long m = 0; m < n; m++) {
            const double t = (m + 0.5) / os;   // in chips (middle of the sample)
            double f = std::fmod((value + t) / N, 1.0) - 0.5;
            if (!up) f = -(std::fmod(t / N, 1.0) - 0.5);
            phase += 2.0 * PI_D * f * bw / fs;
            q.push_back(CF((float)std::cos(phase), (float)std::sin(phase)) * (float)num("a"));
        }
        phase = std::fmod(phase, 2.0 * PI_D);
    }
public:
    LoraMod() {
        type = "loramod"; title = "LoRa modulator"; category = "Modulate"; ins = {{"frames", PT::Frames}}; setOuts({{"out", PT::IQ}});
        addChoice("sf", "Spreading factor", {"SF7", "SF8", "SF9", "SF10", "SF11", "SF12"}, 2);
        addChoice("bw", "Bandwidth", lora::bwNames(false), 7);
        addChoice("cr", "Coding rate", {"4/5", "4/6", "4/7", "4/8"});
        addChoice("hdr", "Header", {"explicit (normal)", "implicit (fixed length)"});
        addChoice("crc", "CRC", {"on", "off"});
        addChoice("ldro", "Low data rate optim.", {"auto (symbol > 16 ms)", "on", "off"});
        addText("sw", "Sync word (hex)", "0x12");
        addNum("pre", "Preamble", 8, 6, 65535, 1, "symbols");
        addNum("os", "Samples per chip", 4, 1, 32, 1, "x");
        addNum("a", "Amplitude", 1, 0, 1000, 0.1, "", 2);
    }
    std::string status() const override {
        const int sf = choice("sf") + 7;
        const double bw = lora::bandwidths()[std::clamp(choice("bw"), 0, 9)];
        char b[96];
        std::snprintf(b, sizeof(b), "%lld packets, %.0f bit/s, %.4g S/s", sent, sf * bw / (1 << sf) * 4.0 / (choice("cr") + 5), bw * num("os"));
        return b;
    }
    void process(const std::vector<const Stream*>& in) override {
        const int sf = choice("sf") + 7, N = 1 << sf;
        const double bw = lora::bandwidths()[std::clamp(choice("bw"), 0, 9)], fs = bw * num("os");
        Stream& o = out[0]; o.clear(); o.type = PT::IQ; o.rate = fs;
        if (in[0] && q.size() < (size_t)(fs * 20)) {
            for (const auto& f : in[0]->frames) {
                const bool ldro = lora::ldroFor(choice("ldro"), sf, bw);
                const int sw = (int)std::strtol(text("sw").c_str(), nullptr, 0) & 0xFF;
                for (int i = 0; i < (int)num("pre"); i++) chirp(fs, bw, N, 0, true, N);
                chirp(fs, bw, N, ((sw >> 4) & 15) << 3, true, N);
                chirp(fs, bw, N, (sw & 15) << 3, true, N);
                chirp(fs, bw, N, 0, false, N);
                chirp(fs, bw, N, 0, false, N);
                chirp(fs, bw, N, 0, false, N / 4.0);
                for (int v : lora::encodeSymbols(f, sf, choice("cr") + 1, choice("crc") == 0, choice("hdr") == 1, ldro)) chirp(fs, bw, N, v, true, N);
                for (int i = 0; i < 4 * N * (int)num("os"); i++) q.push_back(CF(0, 0));   // a gap after it
                sent++;
            }
        }
        const size_t n = clk.take(tick, fs);
        for (size_t i = 0; i < n; i++) {
            if (q.empty()) { o.iq.push_back(CF(0, 0)); continue; }
            o.iq.push_back(q.front()); q.pop_front();
        }
    }
};
