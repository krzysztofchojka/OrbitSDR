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