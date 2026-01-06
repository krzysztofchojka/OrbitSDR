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
    float z1 = 0.0f, z2 = 0.0f;

    void configureNotch(float freq, float sampleRate, float q = 2.0f) {
        if (sampleRate <= 0.0f) return;
        float w0 = 2.0f * (float)PI * freq / sampleRate;
        float alpha = std::sin(w0) / (2.0f * q);
        float cs = std::cos(w0);
        float norm = 1.0f / (1.0f + alpha);
        b0 = 1.0f * norm; b1 = -2.0f * cs * norm; b2 = 1.0f * norm;
        a1 = -2.0f * cs * norm; a2 = (1.0f - alpha) * norm;
        reset();
    }

    void configureLowPass(float freq, float sampleRate, float q = 0.707f) {
        if (sampleRate <= 0.0f) return;
        float w0 = 2.0f * (float)PI * freq / sampleRate;
        float alpha = std::sin(w0) / (2.0f * q);
        float cs = std::cos(w0);
        float norm = 1.0f / (1.0f + alpha);
        b0 = ((1.0f - cs) / 2.0f) * norm; b1 = (1.0f - cs) * norm; b2 = ((1.0f - cs) / 2.0f) * norm;
        a1 = -2.0f * cs * norm; a2 = (1.0f - alpha) * norm;
        reset();
    }

    inline float process(float in) {
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
    
    // Phasor state
    Complex currentPhasor = Complex(1.0, 0.0); 
    
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
    
    // --- RESAMPLING STATE ---
    float wfmSumL = 0.0f;
    float wfmSumR = 0.0f;
    int wfmCount = 0;
    double wfmResampleAcc = 0.0;

    Complex nbSum = Complex(0,0);
    int nbCount = 0;
    double nbResampleAcc = 0.0;

    // --- AGC STATE ---
    float agcPeak = 0.0f;     
    float agcGain = 1.0f;     
    // ZMIANA: static constexpr naprawia błąd operatora przypisania
    static constexpr float AGC_TARGET = 0.6f; 

    // Filters
    Biquad notchL, notchR;
    Biquad cutoffL, cutoffR;
    
    bool filtersConfigured = false;
    double lastSampleRateCheck = 0;

    void clear() {
        audioLpfState = 0.0f;
        deemphStateL = 0.0f; deemphStateR = 0.0f;
        wfmDcState = 0.0f;
        iqLpfState = Complex(0.0, 0.0);
        lastSample = Complex(1.0, 0.0);
        
        wfmSumL = 0.0f; wfmSumR = 0.0f; wfmCount = 0; wfmResampleAcc = 0.0;
        nbSum = Complex(0,0); nbCount = 0; nbResampleAcc = 0.0;
        
        // Reset AGC
        agcPeak = 0.0f;
        agcGain = 1.0f;

        pllPhase = 0.0;
        if (sampleRateIn > 0) pllFreq = (19000.0 / sampleRateIn) * 2.0 * PI;
        
        notchL.reset(); notchR.reset();
        cutoffL.reset(); cutoffR.reset();
        filtersConfigured = false;
        currentPhasor = Complex(1.0, 0.0);
    }

    Demodulator(double srIn, double srOut) : sampleRateIn(srIn), sampleRateOut(srOut) {
        updatePLLParams();
    }
    
    void updatePLLParams() {
        if (sampleRateIn > 0) {
            pllFreq = (19000.0 / sampleRateIn) * 2.0 * PI;
            double scale = 2000000.0 / sampleRateIn; 
            pllAlpha = 0.01 * scale;
            pllBeta = 0.0001 * scale * scale; 
            
            if (pllAlpha < 0.00001) pllAlpha = 0.00001;
            if (pllBeta < 0.0000001) pllBeta = 0.0000001;
        }
    }

    std::vector<float> process(const std::vector<Complex>& rawIQ, double freqOffset, double bandwidthHz, Mode mode, bool stereoEnabled) {
        std::vector<float> audioOut;
        if (rawIQ.empty()) return audioOut;

        if (sampleRateIn != lastSampleRateCheck) {
             updatePLLParams();
        }

        size_t estimatedOut = (size_t)(rawIQ.size() * sampleRateOut / sampleRateIn) * 2 + 20;
        audioOut.reserve(estimatedOut);

        double resampleRatio = sampleRateIn / sampleRateOut;
        if (resampleRatio < 1.0) resampleRatio = 1.0;

        float iqAlpha = 1.0f;
        if (sampleRateIn > 0) {
            iqAlpha = 2.0f * (float)PI * (bandwidthHz / 2.0f) / (float)sampleRateIn;
            if (iqAlpha > 1.0f) iqAlpha = 1.0f;
        }

        float audioAlpha = 0.0f; 
        if (sampleRateOut > 0) {
            audioAlpha = 2.0f * (float)PI * 16000.0f / (float)sampleRateOut;
            if (audioAlpha > 1.0f) audioAlpha = 1.0f;
        }
        
        float deemphAlpha = 0.0f;
        if (sampleRateIn > 0) {
            deemphAlpha = 1.0f - std::exp(-1.0f / (sampleRateIn * 75e-6));
        }

        // --- WFM Filter Config ---
        if (mode == Mode::WFM && (sampleRateIn != lastSampleRateCheck || !filtersConfigured)) {
            if (sampleRateIn > 0) {
                notchL.configureNotch(19000.0f, (float)sampleRateIn, 1.5f);
                notchR.configureNotch(19000.0f, (float)sampleRateIn, 1.5f);
                cutoffL.configureLowPass(15500.0f, (float)sampleRateIn, 0.707f);
                cutoffR.configureLowPass(15500.0f, (float)sampleRateIn, 0.707f);
                lastSampleRateCheck = sampleRateIn;
                filtersConfigured = true;
            }
        }

        double phaseStepAngle = -2.0 * PI * (freqOffset / sampleRateIn);
        Complex ncoStep = std::polar(1.0, phaseStepAngle);

        Complex sample;

        if (!stereoEnabled) {
             pllFreq = (19000.0 / sampleRateIn) * 2.0 * PI;
             pllPhase = 0.0;
        }
        
        double mag = std::abs(currentPhasor);
        if (mag > 0.0001) currentPhasor /= mag;
        else currentPhasor = Complex(1.0, 0.0);

        double targetPll = (19000.0 / sampleRateIn) * 2.0 * PI;
        double pllLimit = (1000.0 / sampleRateIn) * 2.0 * PI; 
        double pllMin = targetPll - pllLimit;
        double pllMax = targetPll + pllLimit;

        for (size_t i = 0; i < rawIQ.size(); i++) {
            sample = rawIQ[i] * currentPhasor;
            currentPhasor *= ncoStep; 

            if ((i & 0xFFF) == 0) {
                double m = std::abs(currentPhasor);
                if (m > 0.0001) currentPhasor *= (1.0 / m); 
            }

            iqLpfState += Complex(iqAlpha, 0) * (sample - iqLpfState);
            Complex processedSample = iqLpfState; 

            // --- WFM PATH ---
            if (mode == Mode::WFM) {
                Complex phaseDiff = processedSample * std::conj(lastSample);
                lastSample = processedSample; 
                float mpxSignal = std::arg(phaseDiff);

                float left = mpxSignal;
                float right = mpxSignal;

                if (stereoEnabled) {
                    float pilotRef = std::sin(pllPhase);
                    float pllError = mpxSignal * pilotRef;
                    
                    pllFreq += pllBeta * pllError;
                    
                    if (pllFreq < pllMin) pllFreq = pllMin;
                    if (pllFreq > pllMax) pllFreq = pllMax;

                    pllPhase += pllFreq + pllAlpha * pllError;
                    
                    if(pllPhase > 2.0*PI) pllPhase -= 2.0*PI;
                    else if(pllPhase < 0.0) pllPhase += 2.0*PI;

                    float carrier38k = std::sin(2.0 * pllPhase);
                    float l_minus_r = mpxSignal * carrier38k * 2.0f;

                    left = (mpxSignal + l_minus_r);
                    right = (mpxSignal - l_minus_r);
                }

                if (filtersConfigured) {
                    left = notchL.process(left);
                    right = notchR.process(right);
                    left = cutoffL.process(left);
                    right = cutoffR.process(right);
                }

                deemphStateL += deemphAlpha * (left - deemphStateL);
                deemphStateR += deemphAlpha * (right - deemphStateR);
                
                wfmSumL += deemphStateL;
                wfmSumR += deemphStateR;
                wfmCount++;
                
                wfmResampleAcc += 1.0;
                if (wfmResampleAcc >= resampleRatio) {
                    // Mniejsze wzmocnienie + soft clip dla WFM
                    float finalL = (wfmSumL / (float)wfmCount) * 1.5f; 
                    float finalR = (wfmSumR / (float)wfmCount) * 1.5f; 

                    wfmDcState = 0.995f * wfmDcState + 0.005f * ((finalL + finalR) * 0.5f);
                    finalL -= wfmDcState; finalR -= wfmDcState;
                    
                    finalL = std::tanh(finalL);
                    finalR = std::tanh(finalR);

                    audioOut.push_back(finalL); 
                    audioOut.push_back(finalR); 
                    
                    wfmSumL = 0.0f; wfmSumR = 0.0f; wfmCount = 0;
                    wfmResampleAcc -= resampleRatio;
                }
            }
            // --- NARROWBAND PATH (AM/NFM/SSB) WITH AGC ---
            else {
                nbSum += processedSample;
                nbCount++;
                nbResampleAcc += 1.0;

                if (nbResampleAcc >= resampleRatio) {
                    if (mode == Mode::OFF) {
                        audioOut.push_back(0.0f); audioOut.push_back(0.0f);
                        nbSum = Complex(0, 0); nbCount = 0; nbResampleAcc -= resampleRatio;
                        continue;
                    }

                    Complex filtered = nbSum / (double)nbCount;
                    nbSum = Complex(0, 0); nbCount = 0;
                    nbResampleAcc -= resampleRatio;

                    float rawAudio = 0.0f;

                    if (mode == Mode::AM) {
                        static float dcBlock = 0.0f;
                        float mag = std::abs(filtered);
                        dcBlock = 0.995f * dcBlock + 0.005f * mag;
                        rawAudio = mag - dcBlock;
                    } 
                    else if (mode == Mode::NFM) {
                        Complex phaseDiff = filtered * std::conj(lastSample);
                        rawAudio = std::arg(phaseDiff) * 0.15f; 
                        lastSample = filtered; 
                    }
                    else if (mode == Mode::LSB || mode == Mode::USB) {
                        rawAudio = filtered.real() * 2.0f;
                    }
                    
                    if (std::isnan(audioLpfState)) audioLpfState = 0.0f;
                    audioLpfState += audioAlpha * (rawAudio - audioLpfState);
                    
                    // --- AGC ---
                    float absAudio = std::abs(audioLpfState);
                    if (absAudio > agcPeak) {
                        agcPeak = absAudio; 
                    } else {
                        agcPeak *= 0.9995f; 
                    }
                    
                    if (agcPeak < 0.02f) agcPeak = 0.02f;

                    float targetGain = AGC_TARGET / agcPeak;
                    if (targetGain > 50.0f) targetGain = 50.0f;

                    agcGain += (targetGain - agcGain) * 0.01f;

                    float finalAudio = audioLpfState * agcGain;
                    
                    // Soft clip
                    finalAudio = std::tanh(finalAudio);

                    audioOut.push_back(finalAudio);
                    audioOut.push_back(finalAudio);
                }
            }
        }
        
        return audioOut;
    }
};