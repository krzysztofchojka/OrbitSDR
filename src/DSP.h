#pragma once

#include <vector>
#include <complex>
#include <cmath>

using Complex = std::complex<double>;
const double PI = 3.14159265358979323846;

// Fast Fourier Transform (Recursive)
inline void fft(std::vector<Complex>& a) {
    size_t n = a.size();
    if (n <= 1) return;

    std::vector<Complex> even(n / 2);
    std::vector<Complex> odd(n / 2);

    for (size_t i = 0; i < n / 2; i++) { 
        even[i] = a[2 * i]; 
        odd[i] = a[2 * i + 1]; 
    }

    fft(even); 
    fft(odd);

    for (size_t k = 0; k < n / 2; k++) {
        Complex t = std::polar(1.0, -2.0 * PI * k / n) * odd[k];
        a[k] = even[k] + t;
        a[k + n / 2] = even[k] - t;
    }
}

// Generate Hanning window
inline std::vector<double> makeWindow(size_t size) {
    std::vector<double> w(size);
    for (size_t i = 0; i < size; i++) {
        w[i] = 0.5 * (1.0 - std::cos(2.0 * PI * i / (double)(size - 1)));
    }
    return w;
}

// --- CHANNELIZER (Universal DDC) ---
// Extracts a narrow channel from a wide bandwidth
class Channelizer {
    std::complex<double> ncoPhase = {1.0, 0.0};
    std::complex<double> ncoStep = {1.0, 0.0};
    std::complex<double> decimSum = {0.0, 0.0};
    int decimCount = 0;
    int decimFactor = 1;

public:
    void configure(double inputRate, double targetRate) {
        decimFactor = (int)(inputRate / targetRate);
        if (decimFactor < 1) decimFactor = 1;
        decimSum = {0,0};
        decimCount = 0;
    }

    void setCenter(double offsetHz, double sampleRate) {
        double angle = -2.0 * PI * (offsetHz / sampleRate);
        ncoStep = std::polar(1.0, angle);
    }

    // Returns true if a new output sample is available
    inline bool process(const std::complex<double>& in, std::complex<float>& out) {
        // 1. Mixer
        std::complex<double> mixed = in * ncoPhase;
        ncoPhase *= ncoStep;
        
        // Normalize the vector periodically
        if (std::abs(ncoPhase.real()) > 2.0) ncoPhase /= std::abs(ncoPhase); 
        
        // 2. Decimator
        decimSum += mixed;
        decimCount++;

        if (decimCount >= decimFactor) {
            out = std::complex<float>((float)decimSum.real() / decimFactor, (float)decimSum.imag() / decimFactor);
            decimSum = {0,0};
            decimCount = 0;
            return true;
        }
        return false;
    }
};