#pragma once

// =====================================================================
//  Morse (CW) decoder core.
//
//  Input: complex baseband around the channel frequency (2000 S/s).
//   1. tone search: every 0.5 s an FFT of the last second finds the
//      strongest carrier within +-300 Hz; the decoder follows it
//      (so the channel doesn't have to be tuned to the Hz),
//   2. a narrow filter (+-60 Hz) around it and the envelope, averaged
//      to 5 ms steps,
//   3. key up / down from an adaptive threshold between the noise floor
//      and the signal peak (with hysteresis); a squelch needs ~8 dB,
//   4. timing: the dot length is learned from the marks (a dash is
//      3 dots), so any speed from ~5 to ~50 WPM decodes; gaps of
//      ~3 dots end a character, ~7 dots a word.
// =====================================================================

#include <vector>
#include <complex>
#include <string>
#include <cmath>
#include <algorithm>
#include <functional>
#include "../../DSP.h"   // fft()

namespace cw {

inline const char* morseToChar(const std::string& code) {
    struct M { const char* c; const char* s; };
    static const M table[] = {
        {".-", "A"}, {"-...", "B"}, {"-.-.", "C"}, {"-..", "D"}, {".", "E"}, {"..-.", "F"}, {"--.", "G"}, {"....", "H"},
        {"..", "I"}, {".---", "J"}, {"-.-", "K"}, {".-..", "L"}, {"--", "M"}, {"-.", "N"}, {"---", "O"}, {".--.", "P"},
        {"--.-", "Q"}, {".-.", "R"}, {"...", "S"}, {"-", "T"}, {"..-", "U"}, {"...-", "V"}, {".--", "W"}, {"-..-", "X"},
        {"-.--", "Y"}, {"--..", "Z"},
        {"-----", "0"}, {".----", "1"}, {"..---", "2"}, {"...--", "3"}, {"....-", "4"}, {".....", "5"}, {"-....", "6"},
        {"--...", "7"}, {"---..", "8"}, {"----.", "9"},
        {".-.-.-", "."}, {"--..--", ","}, {"..--..", "?"}, {"-..-.", "/"}, {"-...-", "="}, {".-.-.", "+"}, {"-....-", "-"},
        {"---...", ":"}, {".----.", "'"}, {"-.--.", "("}, {"-.--.-", ")"}, {".-..-.", "\""}, {".--.-.", "@"}, {"-.-.--", "!"},
        {"...-.-", "<SK>"}, {"-.-.-", "<KA>"}, {".-...", "<AS>"}, {"...-.", "<SN>"}, {"........", "<HH>"},
        {".-.-", "<AA>"}, {"---.", "<OE>"}, {"..--", "<UE>"}, {".-.-..", "<AE>"} };
    for (const M& m : table) if (code == m.c) return m.s;
    return nullptr;
}

class Decoder {
    static constexpr double FS = 2000.0;
    static constexpr int STEP = 10;            // 5 ms envelope steps
    static constexpr double STEP_SEC = STEP / FS;
    static constexpr int NFFT = 2048;          // ~1 s, ~1 Hz bins

    // tone search
    std::vector<std::complex<float>> hist = std::vector<std::complex<float>>(NFFT);
    size_t histPos = 0, sinceSearch = 0;
    double toneHz = 0.0;     // offset of the followed carrier from the channel centre
    std::complex<double> nco = {1.0, 0.0};
    // narrow filter: 2 cascaded 2-pole complex resonators would ring; use a moving average
    // over the envelope window plus a 4th order low-pass on I/Q
    struct Bq { double b0, b1, b2, a1, a2, z1 = 0, z2 = 0;
        double run(double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; } };
    Bq lpI[2], lpQ[2];
    double envAcc = 0.0;
    int envCnt = 0;

    // levels
    double sigLevel = 0.0, noiseLevel = 1e-9, env = 0.0;
    bool keyDown = false;
    int runSteps = 0;        // length of the current mark / space in steps
    double dotSteps = 12.0;  // learned dot length (60 ms = 20 WPM)
    std::string code;        // dots and dashes of the current character
    bool wordSpaceSent = true;
    bool levelsInit = false;
    int squelchHold = 0;     // tone searches (0.5 s each) the squelch stays open

    static Bq lowpass(double fc, double fs, double q) {
        const double w0 = 2.0 * 3.14159265358979 * fc / fs, alpha = std::sin(w0) / (2.0 * q), cs = std::cos(w0), n = 1.0 / (1.0 + alpha);
        Bq b;
        b.b0 = (1.0 - cs) / 2.0 * n; b.b1 = (1.0 - cs) * n; b.b2 = (1.0 - cs) / 2.0 * n;
        b.a1 = -2.0 * cs * n; b.a2 = (1.0 - alpha) * n;
        return b;
    }

    void emitText(const std::string& s) { if (onText) onText(s); }

    void endChar() {
        if (code.empty()) return;
        const char* c = morseToChar(code);
        emitText(c ? c : "*");
        code.clear();
        wordSpaceSent = false;
    }

    void searchTone() {
        std::vector<Complex> buf(NFFT);
        for (int i = 0; i < NFFT; i++) {
            const double w = 0.5 - 0.5 * std::cos(2.0 * 3.14159265358979 * i / (NFFT - 1));
            const auto v = hist[(histPos + i) % NFFT];
            buf[i] = Complex(v.real() * w, v.imag() * w);
        }
        fft(buf);
        const int maxBin = (int)(300.0 / (FS / NFFT));
        std::vector<double> p;
        int best = 0;
        double bestP = 0.0;
        for (int k = -maxBin; k <= maxBin; k++) {
            const double v = std::norm(buf[(k + NFFT) % NFFT]);
            p.push_back(v);
            if (v > bestP) { bestP = v; best = k; }
        }
        std::vector<double> s = p;
        std::nth_element(s.begin(), s.begin() + s.size() / 2, s.end());
        const double median = std::max(1e-30, s[s.size() / 2]);
        toneSnrDb = 10.0 * std::log10(bestP / median);
        // follow a clear carrier; keep the old one while it is quiet (between words)
        if (toneSnrDb > 11.0) squelchHold = 3; else if (squelchHold > 0) squelchHold--;
        if (toneSnrDb > 12.0) {
            const double f = best * (FS / NFFT);
            if (std::fabs(f - toneHz) > 8.0) toneHz = f;
            else toneHz += 0.3 * (f - toneHz);
        }
    }

    void step(double e) {
        env = e;
        if (!levelsInit) { noiseLevel = e; sigLevel = 3.0 * e; levelsInit = true; }
        const double span = std::max(0.0, sigLevel - noiseLevel);
        const double hi = noiseLevel + 0.55 * span, lo = noiseLevel + 0.40 * span;
        // a clear tone in the spectrum (see searchTone) opens the squelch; pure noise never does
        squelchOpen = squelchHold > 0 && sigLevel > noiseLevel * 2.0;
        bool down = keyDown ? e > lo : e > hi;
        if (squelchHold <= 0) down = false;
        // levels: the signal from the marks (fast attack), the noise from the spaces
        if (down) sigLevel += (e > sigLevel ? 0.3 : 0.05) * (e - sigLevel);
        else {
            noiseLevel += 0.02 * (std::min(e, hi) - noiseLevel);
            sigLevel += 0.0005 * (noiseLevel * 3.0 - sigLevel);   // slowly forget a gone signal
            if (e > sigLevel) sigLevel += 0.3 * (e - sigLevel);
        }
        if (!squelchOpen) down = false;

        if (down == keyDown) {
            runSteps++;
            if (!keyDown) {
                // a long gap ends the character / word without waiting for the next mark
                if (!code.empty() && runSteps > 2.0 * dotSteps) endChar();
                if (!wordSpaceSent && runSteps > 5.0 * dotSteps) { emitText(" "); wordSpaceSent = true; }
            }
            return;
        }
        // transition
        if (!keyDown && runSteps < 2.0 * dotSteps && runSteps > 0.5 * dotSteps && !code.empty())
            dotSteps = std::clamp(0.9 * dotSteps + 0.1 * runSteps, 2.0, 60.0);   // a gap inside a character: 1 dot
        if (keyDown) {
            // mark ended: dot or dash (glitches shorter than a third of a dot are ignored)
            const double L = runSteps;
            if (L >= std::max(1.0, dotSteps * 0.3)) {
                if (L < 2.0 * dotSteps) {
                    code += '.';
                    dotSteps = std::clamp(0.85 * dotSteps + 0.15 * L, 2.0, 60.0);
                } else {
                    code += '-';
                    dotSteps = std::clamp(0.85 * dotSteps + 0.15 * (L / 3.0), 2.0, 60.0);
                }
                if (code.size() > 9) code.clear();   // noise, not Morse
            }
        }
        keyDown = down;
        runSteps = 1;
    }

public:
    std::function<void(const std::string&)> onText;
    double toneSnrDb = 0.0;
    bool squelchOpen = false;

    Decoder() {
        lpI[0] = lowpass(60.0, FS, 0.5412); lpI[1] = lowpass(60.0, FS, 1.3066);
        lpQ[0] = lpI[0]; lpQ[1] = lpI[1];
    }

    double wpm() const { return 1200.0 / (dotSteps * STEP_SEC * 1000.0); }
    double tone() const { return toneHz; }
    bool key() const { return keyDown; }
    double envelopeDb() const { return 20.0 * std::log10(env + 1e-12); }
    double noiseDb() const { return 20.0 * std::log10(noiseLevel + 1e-12); }
    double signalDb() const { return 20.0 * std::log10(sigLevel + 1e-12); }

    void reset() {
        code.clear(); keyDown = false; runSteps = 0; wordSpaceSent = true;
        sigLevel = 0.0; noiseLevel = 1e-9; levelsInit = false; squelchHold = 0;
    }

    // x: complex baseband at 2000 S/s centred on the channel
    void process(const std::complex<float>* x, size_t n) {
        const double PI2 = 2.0 * 3.14159265358979;
        for (size_t i = 0; i < n; i++) {
            hist[histPos] = x[i];
            histPos = (histPos + 1) % NFFT;
            if (++sinceSearch >= NFFT / 2) { sinceSearch = 0; searchTone(); }
            // shift the followed tone to 0 Hz, narrow low-pass, envelope
            nco *= std::polar(1.0, -PI2 * toneHz / FS);
            const std::complex<double> v = std::complex<double>(x[i].real(), x[i].imag()) * nco;
            double a = v.real(), b = v.imag();
            for (auto& f : lpI) a = f.run(a);
            for (auto& f : lpQ) b = f.run(b);
            envAcc += std::sqrt(a * a + b * b);
            if (++envCnt == STEP) { step(envAcc / STEP); envAcc = 0.0; envCnt = 0; }
            if ((i & 255) == 0) nco /= std::abs(nco);
        }
    }
};

} // namespace cw
