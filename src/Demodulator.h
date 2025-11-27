#pragma once

#include "DSP.h"
#include <vector>
#include <cmath>
#include <algorithm>

enum class Mode { AM, NFM, WFM, LSB, USB, OFF };

// --- Universal Biquad Filter Structure ---
struct Biquad {
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
    float a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f; // Filter state

    // Configuration: Notch Filter
    // Lower Q = wider notch.
    void configureNotch(float freq, float sampleRate, float q = 2.0f) {
        float w0 = 2.0f * (float)PI * freq / sampleRate;
        float alpha = std::sin(w0) / (2.0f * q);
        float cs = std::cos(w0);
        float norm = 1.0f / (1.0f + alpha);

        b0 = 1.0f * norm;
        b1 = -2.0f * cs * norm;
        b2 = 1.0f * norm;
        a1 = -2.0f * cs * norm;
        a2 = (1.0f - alpha) * norm;
        
        reset();
    }

    // Configuration: Low Pass Filter
    // Cuts off frequencies above 'freq'
    void configureLowPass(float freq, float sampleRate, float q = 0.707f) {
        float w0 = 2.0f * (float)PI * freq / sampleRate;
        float alpha = std::sin(w0) / (2.0f * q);
        float cs = std::cos(w0);
        float norm = 1.0f / (1.0f + alpha);

        b0 = ((1.0f - cs) / 2.0f) * norm;
        b1 = (1.0f - cs) * norm;
        b2 = ((1.0f - cs) / 2.0f) * norm;
        a1 = -2.0f * cs * norm;
        a2 = (1.0f - alpha) * norm;

        reset();
    }

    float process(float in) {
        float out = in * b0 + z1;
        z1 = in * b1 + z2 - out * a1;
        z2 = in * b2 - out * a2;
        return out;
    }

    void reset() { z1 = 0.0f; z2 = 0.0f; }
};

class Demodulator {
public:
    double sampleRateIn;
    double sampleRateOut;
    double currentPhase = 0;
    
    // Audio filter states
    float audioLpfState = 0.0f;
    float deemphStateL = 0.0f; 
    float deemphStateR = 0.0f;
    float wfmDcState = 0.0f;
    
    // IQ Filter state
    Complex iqLpfState = Complex(0.0, 0.0);

    // FM discriminator state
    Complex lastSample = Complex(1.0, 0.0); 

    // Stereo PLL State
    double pllPhase = 0.0;
    double pllFreq = 19000.0 * 2.0 * PI;
    double pllAlpha = 0.01;
    double pllBeta = 0.0001;
    
    // WFM Downsampling Buffers
    float wfmSumL = 0.0f;
    float wfmSumR = 0.0f;
    int wfmCount = 0;

    // --- WFM Filter Set ---
    // 1. Notch: Removes 19kHz pilot tone
    Biquad notchL, notchR;
    // 2. LPF: Removes high freq noise (>15kHz)
    Biquad cutoffL, cutoffR;
    
    bool filtersConfigured = false;
    double lastSampleRateCheck = 0;

    void clear() {
        audioLpfState = 0.0f;
        deemphStateL = 0.0f; deemphStateR = 0.0f;
        wfmDcState = 0.0f;
        iqLpfState = Complex(0.0, 0.0);
        lastSample = Complex(1.0, 0.0);
        wfmSumL = 0.0f; wfmSumR = 0.0f; wfmCount = 0;
        
        // Reset Stereo PLL
        pllPhase = 0.0;
        if (sampleRateIn > 0) pllFreq = (19000.0 / sampleRateIn) * 2.0 * PI;
        
        // Reset filters
        notchL.reset(); notchR.reset();
        cutoffL.reset(); cutoffR.reset();
        filtersConfigured = false;
    }

    Demodulator(double srIn, double srOut) : sampleRateIn(srIn), sampleRateOut(srOut) {
        if (sampleRateIn > 0) {
            pllFreq = (19000.0 / sampleRateIn) * 2.0 * PI;
        }
    }

    std::vector<float> process(const std::vector<Complex>& rawIQ, double freqOffset, double bandwidthHz, Mode mode, bool stereoEnabled) {
        std::vector<float> audioOut;
        
        // Estimate output size
        size_t estimatedOut = (size_t)(rawIQ.size() * sampleRateOut / sampleRateIn) * 2 + 20;
        audioOut.reserve(estimatedOut);

        int decimation = static_cast<int>(sampleRateIn / sampleRateOut);
        if (decimation < 1) decimation = 1;

        // 1. Calculate Filter Alphas
        // IQ Low Pass Alpha (Variable Bandwidth)
        float iqAlpha = 1.0f;
        if (sampleRateIn > 0) {
            iqAlpha = 2.0f * (float)PI * (bandwidthHz / 2.0f) / (float)sampleRateIn;
            if (iqAlpha > 1.0f) iqAlpha = 1.0f;
        }

        // Audio Low Pass Alpha
        float audioAlpha = 0.0f; 
        if (sampleRateOut > 0) {
            audioAlpha = 2.0f * (float)PI * 16000.0f / (float)sampleRateOut; // For NFM/AM
            if (audioAlpha > 1.0f) audioAlpha = 1.0f;
        }
        
        // De-emphasis Alpha (75us)
        float deemphAlpha = 0.0f;
        if (sampleRateIn > 0) {
            deemphAlpha = 1.0f - std::exp(-1.0f / (sampleRateIn * 75e-6));
        }

        // --- WFM Filter Configuration ---
        if (mode == Mode::WFM && (sampleRateIn != lastSampleRateCheck || !filtersConfigured)) {
            if (sampleRateIn > 0) {
                // 1. Wide Notch at 19kHz
                notchL.configureNotch(19000.0f, (float)sampleRateIn, 1.5f);
                notchR.configureNotch(19000.0f, (float)sampleRateIn, 1.5f);
                
                // 2. Low Pass cutoff above 15.5kHz (Standard FM limit)
                cutoffL.configureLowPass(15500.0f, (float)sampleRateIn, 0.707f);
                cutoffR.configureLowPass(15500.0f, (float)sampleRateIn, 0.707f);
                
                lastSampleRateCheck = sampleRateIn;
                filtersConfigured = true;
            }
        }

        Complex osc;
        Complex sample;
        Complex sum(0, 0); 
        int count = 0;

        // Reset PLL if stereo is explicitly disabled
        if (!stereoEnabled) {
             pllFreq = (19000.0 / sampleRateIn) * 2.0 * PI;
             pllPhase = 0.0;
        }

        for (size_t i = 0; i < rawIQ.size(); i++) {
            // A. Frequency Shift (Tuning)
            double angle = -2.0 * PI * (freqOffset / sampleRateIn) * i;
            double globalAngle = currentPhase + angle; 
            osc = std::polar(1.0, globalAngle);
            sample = rawIQ[i] * osc;

            // B. IQ Low-Pass Filter (Channel Selection)
            iqLpfState = iqLpfState + (Complex(iqAlpha, 0) * (sample - iqLpfState));
            Complex processedSample = iqLpfState; 

            // --- WFM PATH (Wideband FM + Stereo) ---
            if (mode == Mode::WFM) {
                // FM Demodulation
                Complex phaseDiff = processedSample * std::conj(lastSample);
                lastSample = processedSample; 
                
                float mpxSignal = std::arg(phaseDiff); // Baseband Multiplex Signal

                float left = mpxSignal;
                float right = mpxSignal;

                if (stereoEnabled) {
                    // 1. PLL (Phase Locked Loop) locked to 19kHz Pilot Tone
                    float pilotRef = std::sin(pllPhase);
                    float pllError = mpxSignal * pilotRef;
                    
                    pllFreq += pllBeta * pllError;
                    pllPhase += pllFreq + pllAlpha * pllError;
                    
                    // Wrap Phase
                    while (pllPhase > 2.0 * PI) pllPhase -= 2.0 * PI;
                    while (pllPhase < 0.0) pllPhase += 2.0 * PI;

                    // PLL Stability Clamp
                    double targetPll = (19000.0 / sampleRateIn) * 2.0 * PI;
                    double limit = (500.0 / sampleRateIn) * 2.0 * PI;
                    if (pllFreq < (targetPll - limit) || pllFreq > (targetPll + limit)) {
                        pllFreq = targetPll; 
                    }

                    // 2. Stereo Demodulation (38kHz carrier)
                    float carrier38k = std::sin(2.0 * pllPhase);
                    float l_minus_r = mpxSignal * carrier38k * 3.0f; 

                    // 3. Matrix Decoding
                    left = (mpxSignal + l_minus_r);
                    right = (mpxSignal - l_minus_r);
                }

                // --- Apply Filters ---
                if (filtersConfigured) {
                    // Step 1: Notch 19kHz
                    left = notchL.process(left);
                    right = notchR.process(right);
                    
                    // Step 2: Low Pass cutoff
                    left = cutoffL.process(left);
                    right = cutoffR.process(right);
                }

                // De-emphasis Filter
                deemphStateL += deemphAlpha * (left - deemphStateL);
                deemphStateR += deemphAlpha * (right - deemphStateR);
                
                wfmSumL += deemphStateL;
                wfmSumR += deemphStateR;
                wfmCount++;

                // Decimation for Audio Output
                if (wfmCount >= decimation) {
                    float finalL = (wfmSumL / (float)wfmCount) * 4.0f; 
                    float finalR = (wfmSumR / (float)wfmCount) * 4.0f; 

                    // DC Blocker
                    wfmDcState = 0.995f * wfmDcState + 0.005f * ((finalL + finalR) * 0.5f);
                    finalL -= wfmDcState;
                    finalR -= wfmDcState;

                    // Hard Limiter
                    if (finalL > 0.9f) finalL = 0.9f; if (finalL < -0.9f) finalL = -0.9f;
                    if (finalR > 0.9f) finalR = 0.9f; if (finalR < -0.9f) finalR = -0.9f;

                    audioOut.push_back(finalL); 
                    audioOut.push_back(finalR); 
                    
                    wfmSumL = 0.0f; wfmSumR = 0.0f;
                    wfmCount = 0;
                }
            }
            // --- NARROWBAND PATH (AM, NFM, SSB) ---
            else {
                sum += processedSample;
                count++;

                if (count >= decimation) {
                    if (mode == Mode::OFF) {
                        audioOut.push_back(0.0f); audioOut.push_back(0.0f);
                        sum = Complex(0, 0); count = 0; continue;
                    }

                    Complex filtered = sum / (double)count;
                    sum = Complex(0, 0); count = 0;

                    float rawAudio = 0.0f;

                    if (mode == Mode::AM) {
                        // Envelope Detection
                        static float dcBlock = 0.0f;
                        float mag = std::abs(filtered);
                        dcBlock = 0.995f * dcBlock + 0.005f * mag;
                        rawAudio = mag - dcBlock;
                    } 
                    else if (mode == Mode::NFM) {
                        // Frequency Discrimination
                        Complex phaseDiff = filtered * std::conj(lastSample);
                        float delta = std::arg(phaseDiff);
                        rawAudio = delta * 0.15f; 
                        lastSample = filtered; 
                    }
                    else if (mode == Mode::LSB || mode == Mode::USB) {
                        // Product Detector
                        rawAudio = filtered.real() * 2.0f;
                    }
                    
                    // Final Audio Low-Pass
                    if (std::isnan(audioLpfState)) audioLpfState = 0.0f;
                    audioLpfState += audioAlpha * (rawAudio - audioLpfState);
                    
                    // Clipping
                    if (audioLpfState > 1.0f) audioLpfState = 1.0f;
                    if (audioLpfState < -1.0f) audioLpfState = -1.0f;

                    audioOut.push_back(audioLpfState);
                    audioOut.push_back(audioLpfState);
                }
            }
        }
        
        currentPhase += -2.0 * PI * (freqOffset / sampleRateIn) * rawIQ.size();
        currentPhase = std::fmod(currentPhase, 2.0 * PI);

        return audioOut;
    }
};