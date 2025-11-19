#pragma once

#include "DSP.h"
#include <vector>
#include <cmath>
#include <algorithm>

enum class Mode { AM, NFM, WFM, LSB, USB, OFF };

class Demodulator {
public:
    double sampleRateIn;
    double sampleRateOut;
    double currentPhase = 0;
    
    // Audio filter states
    float audioLpfState = 0.0f;
    float deemphState = 0.0f;
    float wfmDcState = 0.0f;
    
    // IQ Filter state (Bandwidth control)
    Complex iqLpfState = Complex(0.0, 0.0);

    // FM discriminator state
    Complex lastSample = Complex(1.0, 0.0); 

    // Buffers
    float wfmSum = 0.0f;
    int wfmCount = 0;

    Demodulator(double srIn, double srOut) : sampleRateIn(srIn), sampleRateOut(srOut) {}

    std::vector<float> process(const std::vector<Complex>& rawIQ, double freqOffset, double bandwidthHz, Mode mode) {
        std::vector<float> audioOut;
        
        size_t estimatedOut = rawIQ.size() * sampleRateOut / sampleRateIn + 10;
        audioOut.reserve(estimatedOut);

        int decimation = static_cast<int>(sampleRateIn / sampleRateOut);
        if (decimation < 1) decimation = 1;

        // 1. Calculate IQ Filter Coefficient (Bandwidth Control)
        // This filters the raw RF signal before demodulation.
        float iqAlpha = 1.0f;
        if (sampleRateIn > 0) {
            iqAlpha = 2.0f * (float)PI * (bandwidthHz / 2.0f) / (float)sampleRateIn;
            if (iqAlpha > 1.0f) iqAlpha = 1.0f;
        }

        // 2. Calculate Audio Filter Coefficient
        float audioAlpha = 0.0f;
        if (sampleRateOut > 0) {
            // Fixed ~16kHz lowpass for audio
            audioAlpha = 2.0f * (float)PI * 16000.0f / (float)sampleRateOut;
            if (audioAlpha > 1.0f) audioAlpha = 1.0f;
        }
        
        // 3. De-emphasis Coefficient (for WFM)
        float deemphAlpha = 0.0f;
        if (sampleRateIn > 0) {
            deemphAlpha = 2.0f * (float)PI * 2100.0f / (float)sampleRateIn;
            if (deemphAlpha > 1.0f) deemphAlpha = 1.0f;
        }

        Complex osc;
        Complex sample;
        Complex sum(0, 0); 
        int count = 0;

        for (size_t i = 0; i < rawIQ.size(); i++) {
            // A. Frequency Shift (Tuner)
            double angle = -2.0 * PI * (freqOffset / sampleRateIn) * i;
            double globalAngle = currentPhase + angle; 
            osc = std::polar(1.0, globalAngle);
            sample = rawIQ[i] * osc;

            // B. IQ Low-Pass Filter
            // Pass only the signal within the selected bandwidth.
            iqLpfState = iqLpfState + (Complex(iqAlpha, 0) * (sample - iqLpfState));
            Complex processedSample = iqLpfState; 

            // --- WFM PATH (High-rate processing) ---
            if (mode == Mode::WFM) {
                Complex phaseDiff = processedSample * std::conj(lastSample);
                lastSample = processedSample; 
                
                float rawDemod = std::arg(phaseDiff);

                // De-emphasis
                deemphState += deemphAlpha * (rawDemod - deemphState);
                float audioSample = deemphState;

                // Audio Decimation
                wfmSum += audioSample;
                wfmCount++;

                if (wfmCount >= decimation) {
                    float out = (wfmSum / (float)wfmCount); 
                    out *= 4.0f; // Gain

                    // DC Blocker
                    wfmDcState = 0.995f * wfmDcState + 0.005f * out;
                    out -= wfmDcState;

                    // Hard Limiter
                    if (out > 0.8f) out = 0.8f;
                    if (out < -0.8f) out = -0.8f;

                    audioOut.push_back(out);
                    wfmSum = 0.0f;
                    wfmCount = 0;
                }
            }
            // --- NARROWBAND PATH (AM, NFM, SSB) ---
            else {
                sum += processedSample;
                count++;

                if (count >= decimation) {
                    if (mode == Mode::OFF) {
                        audioOut.push_back(0.0f); 
                        sum = Complex(0, 0); 
                        count = 0; 
                        continue;
                    }

                    Complex filtered = sum / (double)count;
                    sum = Complex(0, 0);
                    count = 0;

                    float rawAudio = 0.0f;

                    if (mode == Mode::AM) {
                        static float dcBlock = 0.0f;
                        float mag = std::abs(filtered);
                        dcBlock = 0.995f * dcBlock + 0.005f * mag;
                        rawAudio = mag - dcBlock;
                    } 
                    else if (mode == Mode::NFM) {
                        // Note: reusing lastSample state variable for NFM discriminator
                        Complex phaseDiff = filtered * std::conj(lastSample);
                        float delta = std::arg(phaseDiff);
                        rawAudio = delta * 0.5f; 
                        lastSample = filtered; 
                    }
                    else if (mode == Mode::LSB || mode == Mode::USB) {
                        rawAudio = filtered.real() * 2.0f;
                    }
                    
                    // Audio Post-Filter
                    if (std::isnan(audioLpfState)) audioLpfState = 0.0f;
                    audioLpfState += audioAlpha * (rawAudio - audioLpfState);
                    
                    // Clamp
                    if (audioLpfState > 1.0f) audioLpfState = 1.0f;
                    if (audioLpfState < -1.0f) audioLpfState = -1.0f;

                    audioOut.push_back(audioLpfState);
                }
            }
        }
        
        // Update phase for next block
        currentPhase += -2.0 * PI * (freqOffset / sampleRateIn) * rawIQ.size();
        currentPhase = std::fmod(currentPhase, 2.0 * PI);

        return audioOut;
    }
};