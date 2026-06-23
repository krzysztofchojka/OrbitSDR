#pragma once

#include "DSP.h"
#include <vector>
#include <cmath>
#include <algorithm>
#include <complex>

enum class Mode { AM, NFM, WFM, LSB, USB, RAW, OFF };

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
             lastSampleRateCheck = sampleRateIn;
        }

        size_t estimatedOut = (size_t)(rawIQ.size() * sampleRateOut / sampleRateIn) * 2 + 20;
        audioOut.reserve(estimatedOut);

        double resampleRatio = sampleRateIn / sampleRateOut;
        if (resampleRatio < 1.0) resampleRatio = 1.0;

        float iqAlpha = 1.0f;
        if (sampleRateIn > 0) {
            // Dla modulacji jednowstęgowych (LSB, USB) filtr musi być 2x szerszy,
            // ponieważ całe pasmo znajduje się tylko po jednej stronie 0 Hz.
            float filterBw = (mode == Mode::LSB || mode == Mode::USB) ? bandwidthHz : (bandwidthHz / 2.0f);
            
            iqAlpha = 2.0f * (float)PI * filterBw / (float)sampleRateIn;
            if (iqAlpha > 1.0f) iqAlpha = 1.0f;
        }

        float audioAlpha = 0.0f; 
        if (sampleRateOut > 0) {
            audioAlpha = 2.0f * (float)PI * 4000.0f / (float)sampleRateOut;
            if (audioAlpha > 1.0f) audioAlpha = 1.0f;
        }
        
        float deemphAlpha = 0.0f;
        if (sampleRateIn > 0) {
            deemphAlpha = 1.0f - std::exp(-1.0f / (sampleRateIn * 75e-6));
        }

        if (mode == Mode::WFM && !filtersConfigured) {
            if (sampleRateIn > 0) {
                notchL.configureNotch(19000.0f, (float)sampleRateIn, 1.5f);
                notchR.configureNotch(19000.0f, (float)sampleRateIn, 1.5f);
                cutoffL.configureLowPass(15500.0f, (float)sampleRateIn, 0.707f);
                cutoffR.configureLowPass(15500.0f, (float)sampleRateIn, 0.707f);
                filtersConfigured = true;
            }
        }

        double phaseStepAngle = -2.0 * PI * (freqOffset / sampleRateIn);
        Complex ncoStep = std::polar(1.0, phaseStepAngle);
        
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

        // Variable declaration (FIX for compilation error)
        Complex sample; 

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
                    if(pllPhase > 2.0*PI) pllPhase -= 2.0*PI; else if(pllPhase < 0.0) pllPhase += 2.0*PI;
                    float carrier38k = std::sin(2.0 * pllPhase);
                    float l_minus_r = mpxSignal * carrier38k * 2.0f;
                    left = (mpxSignal + l_minus_r);
                    right = (mpxSignal - l_minus_r);
                }

                if (filtersConfigured) {
                    left = notchL.process(left); right = notchR.process(right);
                    left = cutoffL.process(left); right = cutoffR.process(right);
                }

                deemphStateL += deemphAlpha * (left - deemphStateL);
                deemphStateR += deemphAlpha * (right - deemphStateR);
                
                wfmSumL += deemphStateL; wfmSumR += deemphStateR; wfmCount++;
                
                wfmResampleAcc += 1.0;
                if (wfmResampleAcc >= resampleRatio) {
                    float finalL = (wfmSumL / (float)wfmCount) * 1.6f;
                    float finalR = (wfmSumR / (float)wfmCount) * 1.6f;
                    wfmDcState = 0.995f * wfmDcState + 0.005f * ((finalL + finalR) * 0.5f);
                    finalL -= wfmDcState; finalR -= wfmDcState;
                    finalL = std::tanh(finalL); finalR = std::tanh(finalR);
                    audioOut.push_back(finalL); audioOut.push_back(finalR); 
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
                        rawAudio = (mag - dcBlock) * 6.0f; // Initial AM boost
                        
                        // ADDED: Pass the signal to the filter (and thus to the AGC system!)
                        if (std::isnan(audioLpfState)) audioLpfState = 0.0f;
                        audioLpfState += audioAlpha * (rawAudio - audioLpfState);
                    }
                    else if (mode == Mode::NFM) {
                        Complex phaseDiff = filtered * std::conj(lastSample);
                        lastSample = filtered;
                        float phaseDelta = std::arg(phaseDiff);

                        // --- FIX AIS: Signal normalization ---
                        if (bandwidthHz > 14000.0) {
                            // 1. Normalization: Map -PI..PI to -1..1
                            // This eliminates the "green wall" in SDRangel
                            float normalized = phaseDelta / (float)PI; 
                            
                            // 2. Gain: GMSK is quiet, amplify 5x, but...
                            normalized *= 5.0f;

                            // 3. DC Blocker: To prevent the signal from "drifting"
                            static float dataDc = 0.0f;
                            dataDc = 0.99f * dataDc + 0.01f * normalized;
                            rawAudio = normalized - dataDc;

                            // 4. Hard Clamp: Safety measure against audio card clipping
                            rawAudio = std::max(-1.0f, std::min(1.0f, rawAudio));

                            audioLpfState = rawAudio;
                        } else {
                            // VOICE:
                            rawAudio = phaseDelta * 0.05f; 
                            if (std::isnan(audioLpfState)) audioLpfState = 0.0f;
                            audioLpfState += audioAlpha * (rawAudio - audioLpfState);
                        }
                    } else if (mode == Mode::LSB || mode == Mode::USB || mode == Mode::RAW) {
                        rawAudio = filtered.real();
                        if (mode != Mode::RAW) {
                            rawAudio *= 2.0f; // Boost for radio
                            
                            // Przefiltrujemy sygnał audio, aby odciąć to co jest poza zdefiniowanym zakresem Bandwidth/AudioAlpha
                            if (!filtersConfigured) {
                                // Zakładamy filtr audio mowy na częstotliwość od 300Hz do 2700Hz
                                cutoffL.configureLowPass(2700.0f, (float)sampleRateOut);
                                cutoffR.configureLowPass(2700.0f, (float)sampleRateOut);
                                filtersConfigured = true;
                            }
                            rawAudio = cutoffL.process(rawAudio);
                            
                            audioLpfState += audioAlpha * (rawAudio - audioLpfState);
                        }
                    }
                    
                    float finalAudio;
                    if (mode == Mode::NFM && bandwidthHz > 14000.0) {
                        finalAudio = audioLpfState;
                    } else if (mode == Mode::RAW) {
                        finalAudio = rawAudio; // Pure, untouched signal straight from the card!
                    } else {
                        // Standard AGC for voice
                        float absAudio = std::abs(audioLpfState);
                        if (absAudio > agcPeak) agcPeak = absAudio; else agcPeak *= 0.9995f;
                        if (agcPeak < 0.02f) agcPeak = 0.02f;
                        float targetGain = AGC_TARGET / agcPeak;
                        if (targetGain > 50.0f) targetGain = 50.0f;
                        agcGain += (targetGain - agcGain) * 0.01f;
                        finalAudio = audioLpfState * agcGain;
                        finalAudio = std::tanh(finalAudio);
                    }

                    audioOut.push_back(finalAudio);
                    audioOut.push_back(finalAudio);
                }
            }
        }
        return audioOut;
    }
};