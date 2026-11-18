#pragma once

#include <vector>
#include <complex>
#include <string>
#include <cmath>
#include <algorithm>

// =====================================================================
//  What kind of signal is on a channel that no digital voice decoder
//  understands. Works on the channel's complex baseband at 48 kHz (25 kHz
//  wide, so a TETRA carrier fits) over half a second at a time:
//
//   - level against the noise floor (slowly tracked minimum),
//   - envelope: FM / FSK have a constant envelope, the linear modulations
//     (TETRA pi/4-DQPSK, P25 phase 2 ...) fluctuate strongly,
//   - TETRA: the squared envelope has a spectral line at the symbol rate
//     of 18 kHz,
//   - FSK: the transitions of the FM discriminator show a line at the
//     symbol rate (2400 / 4800 / 9600 Bd); analog voice shows none.
// =====================================================================

namespace dv {

class SignalClassifier {
    static constexpr double FS = 48000.0;
    static constexpr double PI_D = 3.14159265358979323846;
    static constexpr size_t WIN = 24000;   // 0.5 s

    // Goertzel accumulators over one window
    struct Goertzel {
        double f = 0, c = 0, s1 = 0, s2 = 0;
        void setup(double hz) { f = hz; c = 2.0 * std::cos(2.0 * PI_D * hz / FS); reset(); }
        void reset() { s1 = s2 = 0; }
        void push(double x) { const double s = x + c * s1 - s2; s2 = s1; s1 = s; }
        double power() const { return s1 * s1 + s2 * s2 - c * s1 * s2; }
    };

    Goertzel env18, env16, env20;          // squared envelope: TETRA line + neighbours
    Goertzel fsk[3], fskLo[3], fskHi[3];   // discriminator transitions at 2400 / 4800 / 9600
    size_t count = 0;
    double sumP = 0, sumA = 0, sumA2 = 0, sumD2 = 0;
    std::complex<double> prev {1.0, 0.0};
    double prevD = 0;

public:
    // Results of the last window
    double levelDb = -150.0;
    double envCv = 0.0;          // std / mean of the envelope
    double tetraRatio = 0.0;     // 18 kHz line against its neighbours
    double fskRatio[3] = {0, 0, 0};
    double devHz = 0.0;          // rms deviation
    std::string verdict;         // empty: nothing worth saying
    bool tetra = false;
    int windows = 0;

    SignalClassifier() {
        env18.setup(18000.0); env16.setup(16500.0); env20.setup(19500.0);
        const double r[3] = {2400.0, 4800.0, 9600.0};
        for (int i = 0; i < 3; i++) { fsk[i].setup(r[i]); fskLo[i].setup(r[i] * 0.87); fskHi[i].setup(r[i] * 1.13); }
    }

    void reset() { count = 0; sumP = sumA = sumA2 = sumD2 = 0; verdict.clear(); tetra = false; windows = 0; }

    void process(const std::complex<float>* x, size_t n) {
        for (size_t i = 0; i < n; i++) {
            const std::complex<double> v(x[i].real(), x[i].imag());
            const double p = std::norm(v), a = std::sqrt(p);
            sumP += p; sumA += a; sumA2 += p;
            env18.push(p); env16.push(p); env20.push(p);
            const double d = std::arg(v * std::conj(prev)) * FS / (2.0 * PI_D);   // Hz
            prev = v;
            sumD2 += d * d;
            const double t = (d - prevD) * (d - prevD);
            prevD = d;
            for (int k = 0; k < 3; k++) { fsk[k].push(t); fskLo[k].push(t); fskHi[k].push(t); }
            if (++count >= WIN) finish();
        }
    }

private:
    static double ratio(const Goertzel& g, const Goertzel& a, const Goertzel& b) {
        const double side = 0.5 * (a.power() + b.power());
        return side > 0 ? g.power() / side : 0.0;
    }

    void finish() {
        const double meanP = sumP / count, meanA = sumA / count;
        levelDb = 10.0 * std::log10(meanP + 1e-20);
        const double var = std::max(0.0, sumA2 / count - meanA * meanA);
        envCv = meanA > 0 ? std::sqrt(var) / meanA : 0.0;
        tetraRatio = ratio(env18, env16, env20);
        for (int k = 0; k < 3; k++) fskRatio[k] = ratio(fsk[k], fskLo[k], fskHi[k]);
        devHz = std::sqrt(sumD2 / count);
        windows++;

        // Noise has a Rayleigh envelope (cv ~0.52) and a wild discriminator (rms ~14 kHz at
        // 48 kHz); FM / FSK carriers a flat envelope and a few kHz; TETRA the 18 kHz line.
        tetra = false;
        if (envCv > 0.15 && tetraRatio > 8.0) {
            tetra = true;
            verdict = "TETRA (pi/4-DQPSK, 18 ksym/s)";
        } else if (envCv < 0.3 && devHz < 6000.0) {
            // the lowest rate with a clear line (the transitions also show its harmonics)
            int best = -1;
            for (int k = 0; k < 3 && best < 0; k++) if (fskRatio[k] > 20.0) best = k;
            static const char* rates[3] = {"2400", "4800", "9600"};
            if (best == 1) verdict = "FSK 4800 Bd without a known sync (P25, or a system not supported)";
            else if (best >= 0) verdict = std::string("FSK ") + rates[best] + " Bd without a known sync (data or other system)";
            else verdict = "Analog FM (voice) or a carrier";
        } else verdict = "No signal";

        count = 0; sumP = sumA = sumA2 = sumD2 = 0;
        env18.reset(); env16.reset(); env20.reset();
        for (int k = 0; k < 3; k++) { fsk[k].reset(); fskLo[k].reset(); fskHi[k].reset(); }
    }
};

} // namespace dv
