#pragma once

#include <vector>
#include <complex>
#include <cmath>
#include <algorithm>

using Complex = std::complex<double>;
const double PI = 3.14159265358979323846;

// Fast Fourier Transform (Iterative, In-place)
// NOTE: The size of vector 'a' must be a power of 2!
inline void fft(std::vector<Complex>& a) {
    size_t n = a.size();
    if (n <= 1) return;

    // Bit-reversal permutation
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(a[i], a[j]);
        }
    }

    // Iterative FFT computation (Cooley-Tukey algorithm)
    for (size_t len = 2; len <= n; len <<= 1) {
        double angle = -2.0 * PI / len;
        Complex wlen(std::cos(angle), std::sin(angle)); // Compute trigonometry only once per layer
        
        for (size_t i = 0; i < n; i += len) {
            Complex w(1.0, 0.0);
            for (size_t j = 0; j < len / 2; j++) {
                Complex u = a[i + j];
                Complex v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wlen; // Complex multiplication is significantly faster than std::polar
            }
        }
    }
}

// Generate Hanning window
inline std::vector<double> makeWindow(size_t size) {
    std::vector<double> w(size);
    if (size == 0) return w;
    
    double phaseStep = 2.0 * PI / (double)(size - 1); // Precompute constant
    for (size_t i = 0; i < size; i++) {
        w[i] = 0.5 * (1.0 - std::cos(phaseStep * i));
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
    int normCounter = 0;

public:
    void configure(double inputRate, double targetRate) {
        decimFactor = (int)(inputRate / targetRate);
        if (decimFactor < 1) decimFactor = 1;
        decimSum = {0,0};
        decimCount = 0;
        normCounter = 0;
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
        
        // Fast, low-cost NCO normalization to prevent floating-point drift.
        // Performing this periodically (e.g., every 256 samples) drastically improves performance.
        if (++normCounter >= 256) {
            // Use Taylor expansion for fast normalization (1.5 - 0.5 * |z|^2)
            // Avoids highly expensive std::abs() which performs a square root under the hood.
            double mag2 = std::norm(ncoPhase); 
            ncoPhase *= (1.5 - 0.5 * mag2);
            normCounter = 0;
        }
        
        // 2. Decimator
        decimSum += mixed;
        decimCount++;

        if (decimCount >= decimFactor) {
            // Multiplying by the reciprocal (1.0 / decimFactor) is faster than division,
            // though the compiler usually optimizes this automatically for simple types.
            float invDecim = 1.0f / decimFactor;
            out = std::complex<float>((float)decimSum.real() * invDecim, (float)decimSum.imag() * invDecim);
            decimSum = {0,0};
            decimCount = 0;
            return true;
        }
        return false;
    }
};

class DecimatingFIR {
    std::vector<float> taps;
    std::vector<Complex> buffer;
    int bufIdx = 0;

public:
    void configure(float cutoffHz, float sampleRate, int numTaps = 101) {
        if (sampleRate <= 0) return;
        if (numTaps % 2 == 0) numTaps++; // Filtr musi być nieparzysty
        taps.resize(numTaps);
        buffer.assign(numTaps, {0,0});
        bufIdx = 0;

        float fc = cutoffHz / sampleRate;
        float sum = 0.0f;

        // Okno Blackmana + idealny filtr Sinc
        for (int i = 0; i < numTaps; i++) {
            if (i == numTaps / 2) {
                taps[i] = 2.0f * (float)PI * fc;
            } else {
                float n = (float)(i - numTaps / 2);
                taps[i] = std::sin(2.0f * (float)PI * fc * n) / n;
            }
            taps[i] *= 0.42f - 0.5f * std::cos(2.0f * (float)PI * i / (numTaps - 1)) + 0.08f * std::cos(4.0f * (float)PI * i / (numTaps - 1));
            sum += taps[i];
        }
        for (int i = 0; i < numTaps; i++) taps[i] /= sum; // Normalizacja
    }

    inline void push(Complex in) {
        buffer[bufIdx] = in;
        bufIdx = (bufIdx + 1) % buffer.size();
    }

    inline Complex compute() {
        if (taps.empty()) return {0,0};
        Complex out = {0,0};
        int numTaps = taps.size();
        int idx = bufIdx - 1;
        if (idx < 0) idx += numTaps;

        for (int i = 0; i < numTaps; i++) {
            out += buffer[idx] * (double)taps[i];
            idx--;
            if (idx < 0) idx += numTaps;
        }
        return out;
    }

    void reset() {
        std::fill(buffer.begin(), buffer.end(), Complex(0,0));
        bufIdx = 0;
    }
};