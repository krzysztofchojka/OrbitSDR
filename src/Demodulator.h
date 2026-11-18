#pragma once

#include "DSP.h"
#include <vector>
#include <cmath>
#include <algorithm>
#include <complex>
#include <memory>
#include "modules/Digital/DigitalVoice.h"

// New modes go at the end: the saved "mode_index" and the bookmarks of older versions stay valid.
// DIGITAL: digital voice with automatic detection (DMR, dPMR, D-STAR, YSF, NXDN).
enum class Mode { AM, NFM, WFM, LSB, USB, RAW, OFF, CW, DIGITAL, DMR, DPMR, DSTAR, YSF, NXDN };
inline constexpr int MODE_LAST = (int)Mode::NXDN;

inline bool isDigitalMode(Mode m) { return (int)m >= (int)Mode::DIGITAL && (int)m <= (int)Mode::NXDN; }

inline dv::Want digitalWant(Mode m) {
    switch (m) {
    case Mode::DMR: return dv::Want::DMR;
    case Mode::DPMR: return dv::Want::DPMR;
    case Mode::DSTAR: return dv::Want::DSTAR;
    case Mode::YSF: return dv::Want::YSF;
    case Mode::NXDN: return dv::Want::NXDN;
    default: return dv::Want::Auto;
    }
}

inline const char* modeName(Mode m) {
    switch (m) {
    case Mode::AM: return "AM";
    case Mode::NFM: return "NFM";
    case Mode::WFM: return "WFM";
    case Mode::LSB: return "LSB";
    case Mode::USB: return "USB";
    case Mode::RAW: return "RAW";
    case Mode::OFF: return "OFF";
    case Mode::CW: return "CW";
    case Mode::DIGITAL: return "DIGITAL";
    case Mode::DMR: return "DMR";
    case Mode::DPMR: return "dPMR";
    case Mode::DSTAR: return "D-STAR";
    case Mode::YSF: return "YSF";
    case Mode::NXDN: return "NXDN";
    }
    return "?";
}

// FM discriminator (radians per sample at 48 kHz) -> the 16 bit scale the digital voice
// decoder expects: 5 units per Hz of deviation (DMR's outer symbols at +-1944 Hz ~ +-9700)
inline short discToShort(double phaseDelta) {
    const double v = phaseDelta * (48000.0 / (2.0 * 3.14159265358979323846)) * 5.0;
    return (short)std::clamp(v, -32767.0, 32767.0);
}

// CW: a carrier on the tuned frequency is heard as this tone
inline constexpr double CW_PITCH_HZ = 700.0;

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
    double lastSampleRateCheck = 0;
    double lastBandwidth = 0;
    Mode lastMode = Mode::OFF;

    Complex currentPhasor = Complex(1.0, 0.0);
    DecimatingFIR channelFir;

    // NCO state for the Weaver method
    Complex ssbNcoPhase = Complex(1.0, 0.0);
    Complex ssbNcoStep = Complex(1.0, 0.0);
    Complex ssbRestorePhase = Complex(1.0, 0.0);
    Complex ssbRestoreStep = Complex(1.0, 0.0);
    int audioNcoNormCounter = 0;

    float audioLpfState = 0.0f;
    float dcBlock = 0.0f;   // AM carrier level (per instance: decoders run their own demodulators)
    float dataDc = 0.0f;    // wide NFM data DC
    float deemphStateL = 0.0f;
    float deemphStateR = 0.0f;
    float wfmDcState = 0.0f;
    Complex iqLpfState = Complex(0.0, 0.0);
    Complex lastSample = Complex(1.0, 0.0);

    double pllPhase = 0.0;
    double pllFreq = 19000.0 * 2.0 * PI;
    double pllAlpha = 0.01;
    double pllBeta = 0.0001;
    float wfmPilotLevel = 0.0f;

    float wfmSumL = 0.0f;
    float wfmSumR = 0.0f;
    int wfmCount = 0;
    double wfmResampleAcc = 0.0;

    Complex nbSum = Complex(0,0);
    int nbCount = 0;
    double nbResampleAcc = 0.0;

    float agcPeak = 0.0f;
    float agcGain = 1.0f;
    static constexpr float AGC_TARGET = 0.6f;

    Biquad notchL, notchR, cutoffL, cutoffR, audioCutoff;
    // CW: narrow channel filter (6th order, on I and Q) and the beat oscillator
    Biquad cwI[3], cwQ[3];
    Complex cwBfoPhase = Complex(1.0, 0.0);
    Complex cwBfoStep = Complex(1.0, 0.0);
    bool filtersConfigured = false;

    // Optional copy of the WFM multiplex signal (at sampleRateIn) for the RDS decoder.
    // Filled by process() while mpxTapEnabled is set and the mode is WFM.
    bool mpxTapEnabled = false;
    std::vector<float> mpxTap;

    // Digital voice modes: the decoder (shared with the UI, which reads its status), the input
    // is audio instead of I/Q (the discriminator audio of a receiver or a recording), and a copy
    // of the discriminator for the other decoders (they get it instead of the speech)
    std::shared_ptr<dv::Core> digital;
    bool digitalFromAudio = false;
    std::vector<short> discBuf;
    std::vector<float> digVoice;
    std::vector<float> discTap;

    void clear() {
        audioLpfState = 0.0f;
        deemphStateL = 0.0f; deemphStateR = 0.0f;
        wfmDcState = 0.0f;
        iqLpfState = Complex(0.0, 0.0);
        lastSample = Complex(1.0, 0.0);
        wfmSumL = 0.0f; wfmSumR = 0.0f; wfmCount = 0;
        wfmResampleAcc = 0.0;
        nbSum = Complex(0,0); nbCount = 0; nbResampleAcc = 0.0;
        agcPeak = 0.0f; agcGain = 1.0f;
        pllPhase = 0.0; wfmPilotLevel = 0.0f;
        
        if (sampleRateIn > 0) pllFreq = (19000.0 / sampleRateIn) * 2.0 * PI;

        notchL.reset(); notchR.reset(); cutoffL.reset(); cutoffR.reset(); audioCutoff.reset();
        for (int i = 0; i < 3; i++) { cwI[i].reset(); cwQ[i].reset(); }
        cwBfoPhase = Complex(1.0, 0.0);
        channelFir.reset();
        currentPhasor = Complex(1.0, 0.0);
        ssbNcoPhase = Complex(1.0, 0.0);
        ssbRestorePhase = Complex(1.0, 0.0);
        filtersConfigured = false;
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
        mpxTap.clear();
        discBuf.clear();
        discTap.clear();
        if (rawIQ.empty()) return audioOut;
        const bool dig = isDigitalMode(mode) && digital;
        const bool tapMpx = mpxTapEnabled && mode == Mode::WFM;
        if (tapMpx) mpxTap.reserve(rawIQ.size());

        if (sampleRateIn != lastSampleRateCheck || bandwidthHz != lastBandwidth || mode != lastMode) {
            updatePLLParams();
            lastSampleRateCheck = sampleRateIn; lastBandwidth = bandwidthHz; lastMode = mode;
            filtersConfigured = false;
        }

        size_t estimatedOut = (size_t)(rawIQ.size() * sampleRateOut / sampleRateIn) * 2 + 20;
        audioOut.reserve(estimatedOut);

        double resampleRatio = sampleRateIn / sampleRateOut;
        // WFM: the discriminator output is in radians per input sample, so its level depends on
        // the sample rate. Scale it so the full 75 kHz deviation gives the same audio level at
        // every sample rate (the factor is 1.5 at 1 MSps, the previous fixed value).
        const float wfmGain = (float)(0.7 * sampleRateIn / (2.0 * PI * 75000.0));
        if (resampleRatio < 1.0) resampleRatio = 1.0;

        float iqAlpha = 1.0f;
        if (sampleRateIn > 0) {
            iqAlpha = 2.0f * (float)PI * (bandwidthHz / 2.0f) / (float)sampleRateIn;
            if (iqAlpha > 1.0f) iqAlpha = 1.0f;
        }

        float audioAlpha = 0.0f;
        if (sampleRateOut > 0) {
            audioAlpha = 2.0f * (float)PI * 4000.0f / (float)sampleRateOut;
            if (audioAlpha > 1.0f) audioAlpha = 1.0f;
        }

        // European de-emphasis (50 us)
        float deemphAlpha = 0.0f;
        if (sampleRateIn > 0) deemphAlpha = 1.0f - std::exp(-1.0f / (sampleRateIn * 50e-6));

        if (!filtersConfigured) {
            if (sampleRateIn > 0) {
                notchL.configureNotch(19000.0f, (float)sampleRateIn, 1.5f);
                notchR.configureNotch(19000.0f, (float)sampleRateIn, 1.5f);
                cutoffL.configureLowPass(15500.0f, (float)sampleRateIn, 0.707f);
                cutoffR.configureLowPass(15500.0f, (float)sampleRateIn, 0.707f);

                // Parameters for narrow modes
                float firCutoff = bandwidthHz / 2.0f;
                if (firCutoff < 100.0f) firCutoff = 100.0f;
                channelFir.configure(firCutoff, (float)sampleRateOut, 101); // Runs on the decimated signal!
                audioCutoff.configureLowPass(2700.0f, (float)sampleRateOut);

                if (mode == Mode::CW) {
                    // Butterworth 6th order low-pass at half the bandwidth (the FIR alone is too wide)
                    static const float q[3] = {0.5176f, 0.7071f, 1.9319f};
                    const float fc = std::max(25.0f, (float)bandwidthHz / 2.0f);
                    for (int i = 0; i < 3; i++) { cwI[i].configureLowPass(fc, (float)sampleRateOut, q[i]); cwQ[i].configureLowPass(fc, (float)sampleRateOut, q[i]); }
                    cwBfoStep = std::polar(1.0, 2.0 * PI * CW_PITCH_HZ / sampleRateOut);
                }
                if (mode == Mode::USB) {
                    ssbNcoStep = std::polar(1.0, -2.0 * PI * (bandwidthHz / 2.0) / sampleRateOut);
                    ssbRestoreStep = std::polar(1.0, 2.0 * PI * (bandwidthHz / 2.0) / sampleRateOut);
                } else if (mode == Mode::LSB) {
                    ssbNcoStep = std::polar(1.0, 2.0 * PI * (bandwidthHz / 2.0) / sampleRateOut);
                    ssbRestoreStep = std::polar(1.0, -2.0 * PI * (bandwidthHz / 2.0) / sampleRateOut);
                }
                filtersConfigured = true;
            }
        }

        double phaseStepAngle = -2.0 * PI * (freqOffset / sampleRateIn);
        Complex ncoStep = std::polar(1.0, phaseStepAngle);

        if (!stereoEnabled) { pllFreq = (19000.0 / sampleRateIn) * 2.0 * PI; pllPhase = 0.0; }

        double mag = std::abs(currentPhasor);
        if (mag > 0.0001) currentPhasor /= mag; else currentPhasor = Complex(1.0, 0.0);

        double targetPll = (19000.0 / sampleRateIn) * 2.0 * PI;
        double pllLimit = (1000.0 / sampleRateIn) * 2.0 * PI;
        double pllMin = targetPll - pllLimit;
        double pllMax = targetPll + pllLimit;

        Complex sample;
        for (size_t i = 0; i < rawIQ.size(); i++) {
            sample = rawIQ[i] * currentPhasor;
            currentPhasor *= ncoStep;

            if ((i & 0xFFF) == 0) {
                double m = std::abs(currentPhasor);
                if (m > 0.0001) currentPhasor *= (1.0 / m);
            }

            // --- WFM PATH ---
            if (mode == Mode::WFM) {
                iqLpfState += Complex(iqAlpha, 0) * (sample - iqLpfState);
                Complex processedSample = iqLpfState;

                Complex phaseDiff = processedSample * std::conj(lastSample);
                lastSample = processedSample;
                float mpxSignal = std::arg(phaseDiff);
                if (tapMpx) mpxTap.push_back(mpxSignal);
                
                // MUTE very weak FM signals - removes quiet crackle from noise
                float rfMag = std::abs(processedSample);
                if (rfMag < 0.005f) mpxSignal *= (rfMag / 0.005f);

                float left = mpxSignal, right = mpxSignal;

                if (stereoEnabled) {
                    float pilotRef = std::sin(pllPhase);
                    float pilotI = mpxSignal * pilotRef;
                    float pilotQ = mpxSignal * std::cos(pllPhase); // Used to estimate the lock
                    
                    pllFreq += pllBeta * pilotI;
                    if (pllFreq < pllMin) pllFreq = pllMin;
                    if (pllFreq > pllMax) pllFreq = pllMax;

                    pllPhase += pllFreq + pllAlpha * pilotI;
                    if(pllPhase > 2.0*PI) pllPhase -= 2.0*PI;
                    else if(pllPhase < 0.0) pllPhase += 2.0*PI;

                    // Stereo signal quality estimate - stereo blend
                    wfmPilotLevel = 0.999f * wfmPilotLevel + 0.001f * std::abs(pilotQ);
                    
                    if (wfmPilotLevel > 0.003f) {
                        float blend = std::clamp((wfmPilotLevel - 0.003f) * 100.0f, 0.0f, 1.0f);
                        float carrier38k = std::sin(2.0 * pllPhase);
                        float l_minus_r = mpxSignal * carrier38k * 2.0f * blend;
                        left = mpxSignal + l_minus_r;
                        right = mpxSignal - l_minus_r;
                    }
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
                    float finalL = (wfmSumL / (float)wfmCount) * wfmGain;
                    float finalR = (wfmSumR / (float)wfmCount) * wfmGain;
                    wfmDcState = 0.995f * wfmDcState + 0.005f * ((finalL + finalR) * 0.5f);
                    finalL -= wfmDcState; finalR -= wfmDcState;
                    finalL = std::tanh(finalL); finalR = std::tanh(finalR);

                    audioOut.push_back(finalL); audioOut.push_back(finalR);
                    wfmSumL = 0.0f; wfmSumR = 0.0f; wfmCount = 0;
                    wfmResampleAcc -= resampleRatio;
                }
            }
            // --- NARROWBAND PATH (AM/NFM/SSB) ---
            else {
                // 1. Anti-alias filtering (boxcar) and reduction to 48 kHz
                nbSum += sample;
                nbCount++;
                nbResampleAcc += 1.0;

                if (nbResampleAcc >= resampleRatio) {
                    Complex decimated = nbSum / (double)nbCount;
                    nbSum = Complex(0, 0);
                    nbCount = 0;
                    nbResampleAcc -= resampleRatio;

                    if (mode == Mode::OFF) {
                        audioOut.push_back(0.0f); audioOut.push_back(0.0f);
                        continue;
                    }

                    if (dig) {
                        // channel filter, then the discriminator for the digital voice decoder
                        channelFir.push(decimated);
                        const Complex f = channelFir.compute();
                        short v;
                        if (digitalFromAudio) v = (short)std::clamp(f.real() * 32768.0, -32767.0, 32767.0);
                        else {
                            const Complex pd = f * std::conj(lastSample);
                            lastSample = f;
                            v = discToShort(std::arg(pd));
                        }
                        discBuf.push_back(v);
                        discTap.push_back(v / 32768.0f);
                        continue;
                    }

                    Complex filtered = {0,0};

                    if (mode == Mode::LSB || mode == Mode::USB) {
                        // Weaver: shift the band to 0 Hz
                        decimated *= ssbNcoPhase;
                        ssbNcoPhase *= ssbNcoStep;

                        channelFir.push(decimated);
                        filtered = channelFir.compute();

                        // Weaver: shift back
                        filtered *= ssbRestorePhase;
                        ssbRestorePhase *= ssbRestoreStep;

                        if (++audioNcoNormCounter >= 256) {
                            ssbNcoPhase /= std::abs(ssbNcoPhase);
                            ssbRestorePhase /= std::abs(ssbRestorePhase);
                            audioNcoNormCounter = 0;
                        }
                    } else if (mode == Mode::CW) {
                        // the carrier sits at 0 Hz: narrow filter, then shift it up to the pitch
                        channelFir.push(decimated);
                        filtered = channelFir.compute();
                        float fi = (float)filtered.real(), fq = (float)filtered.imag();
                        for (int k = 0; k < 3; k++) { fi = cwI[k].process(fi); fq = cwQ[k].process(fq); }
                        filtered = Complex(fi, fq) * cwBfoPhase;
                        cwBfoPhase *= cwBfoStep;
                        if (++audioNcoNormCounter >= 256) { cwBfoPhase /= std::abs(cwBfoPhase); audioNcoNormCounter = 0; }
                    } else {
                        // AM and NFM just pass through the FIR
                        channelFir.push(decimated);
                        filtered = channelFir.compute();
                    }

                    float rawAudio = 0.0f;

                    if (mode == Mode::AM) {
                        float mag = std::abs(filtered);
                        dcBlock = 0.995f * dcBlock + 0.005f * mag;
                        rawAudio = (mag - dcBlock) * 6.0f;
                        if (std::isnan(audioLpfState)) audioLpfState = 0.0f;
                        audioLpfState += audioAlpha * (rawAudio - audioLpfState);
                    } 
                    else if (mode == Mode::NFM) {
                        Complex phaseDiff = filtered * std::conj(lastSample);
                        lastSample = filtered;
                        float phaseDelta = std::arg(phaseDiff);

                        if (bandwidthHz > 14000.0) { 
                            float normalized = phaseDelta / (float)PI;
                            normalized *= 5.0f;
                            dataDc = 0.99f * dataDc + 0.01f * normalized;
                            rawAudio = std::clamp(normalized - dataDc, -1.0f, 1.0f);
                            audioLpfState = rawAudio;
                        } else {
                            rawAudio = phaseDelta * 0.05f;
                            if (std::isnan(audioLpfState)) audioLpfState = 0.0f;
                            audioLpfState += audioAlpha * (rawAudio - audioLpfState);
                        }
                    } 
                    else if (mode == Mode::LSB || mode == Mode::USB || mode == Mode::RAW || mode == Mode::CW) {
                        rawAudio = filtered.real();
                        if (mode != Mode::RAW) {
                            rawAudio *= 2.0f; 
                            rawAudio = audioCutoff.process(rawAudio);
                            audioLpfState += audioAlpha * (rawAudio - audioLpfState);
                        }
                    }

                    float finalAudio;
                    if (mode == Mode::NFM && bandwidthHz > 14000.0) {
                        finalAudio = audioLpfState;
                    } else if (mode == Mode::RAW) {
                        finalAudio = rawAudio;
                    } else {
                        // AGC
                        float absAudio = std::abs(audioLpfState);
                        if (absAudio > agcPeak) agcPeak = absAudio; else agcPeak *= 0.9995f;
                        if (agcPeak < 0.02f) agcPeak = 0.02f;

                        float targetGain = AGC_TARGET / agcPeak;
                        if (targetGain > 50.0f) targetGain = 50.0f;
                        agcGain += (targetGain - agcGain) * 0.01f;

                        finalAudio = std::tanh(audioLpfState * agcGain);
                    }

                    audioOut.push_back(finalAudio);
                    audioOut.push_back(finalAudio);
                }
            }
        }
        if (dig) {
            digVoice.clear();
            digital->setWant(digitalWant(mode));
            digital->process(discBuf.data(), discBuf.size(), &digVoice);
            for (float v : digVoice) { const float o = std::tanh(v * 1.5f); audioOut.push_back(o); audioOut.push_back(o); }
        }
        return audioOut;
    }
};