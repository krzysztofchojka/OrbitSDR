#pragma once
#include <vector>
#include <complex>
#include "DSP.h"
#include "Globals.h"
#include "Utils.h"

class InspectorDSP {
    Complex ncoPhase = {1.0, 0.0};
    Complex ncoStep = {1.0, 0.0};
    std::vector<Complex> buffer;
    std::vector<double> windowFunc;
    Complex decimSum = {0,0};
    int decimCount = 0;
    const int INSPECTOR_FFT_SIZE = 512;

public:
    InspectorDSP() {
        windowFunc = makeWindow(INSPECTOR_FFT_SIZE);
        buffer.reserve(INSPECTOR_FFT_SIZE);
    }

    void process(const std::vector<Complex>& rawIQ, double sampleRate, double targetFreqOffset, double bandwidth, SharedData& shared) {
        if (bandwidth < 100.0) bandwidth = 100.0;
        double targetRate = bandwidth * 1.2;
        int decimation = (int)(sampleRate / targetRate);
        if (decimation < 1) decimation = 1;

        double angle = -2.0 * PI * (targetFreqOffset / sampleRate);
        ncoStep = std::polar(1.0, angle);

        for (const auto& s : rawIQ) {
            Complex mixed = s * ncoPhase;
            ncoPhase *= ncoStep;
            if (std::abs(ncoPhase.real()) > 2.0) ncoPhase /= std::abs(ncoPhase);
            decimSum += mixed; decimCount++;

            if (decimCount >= decimation) {
                Complex outSample = decimSum / (double)decimCount;
                decimSum = {0,0}; decimCount = 0;
                buffer.push_back(outSample);

                if (buffer.size() >= INSPECTOR_FFT_SIZE) {
                    performFFT(shared);
                    buffer.clear();
                    double m = std::abs(ncoPhase);
                    ncoPhase /= m;
                }
            }
        }
    }

private:
    void performFFT(SharedData& shared) {
        std::vector<Complex> fftData = buffer;
        for(size_t i=0; i<INSPECTOR_FFT_SIZE; i++) fftData[i] *= windowFunc[i];
        fft(fftData);

        std::vector<double> spectrum(INSPECTOR_FFT_SIZE);
        for(size_t i=0; i<INSPECTOR_FFT_SIZE; i++) {
            int shiftIdx = (i + INSPECTOR_FFT_SIZE/2) % INSPECTOR_FFT_SIZE;
            double mag = std::abs(fftData[shiftIdx]) / INSPECTOR_FFT_SIZE;
            spectrum[i] = 20.0 * std::log10(mag + 1e-12);
        }

        int width = 280; std::vector<uint8_t> row(width * 4);
        float minDb, maxDb; int theme;
        {
            std::lock_guard<std::mutex> l(shared.mtx);
            minDb = shared.minDb; maxDb = shared.maxDb; theme = shared.waterfallTheme;
        }

        for(int x=0; x<width; x++) {
            int fftIdx = (x * INSPECTOR_FFT_SIZE) / width;
            if (fftIdx >= INSPECTOR_FFT_SIZE) fftIdx = INSPECTOR_FFT_SIZE - 1;
            float val = spectrum[fftIdx];
            float norm = (val - minDb) / (maxDb - minDb);
            sf::Color c = getHeatmap(norm, theme);
            row[x*4+0] = c.r; row[x*4+1] = c.g; row[x*4+2] = c.b; row[x*4+3] = 255;
        }

        {
            std::lock_guard<std::mutex> l(shared.mtx);
            shared.inspectorSpectrum = spectrum;
            shared.inspectorWaterfallRow = row;
            shared.newInspectorData = true;
        }
    }
};