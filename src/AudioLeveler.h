#pragma once

#include <vector>
#include <cmath>
#include <algorithm>

// =====================================================================
//  Speaker loudness leveller.
//
//  The demodulators produce very different levels (WFM music, NFM with
//  noise, SSB with AGC ...). This slow RMS based gain brings them to a
//  similar loudness, so switching modes doesn't blast the speakers.
//
//  - Works on the speaker path only (decoders get the unchanged audio).
//  - Gain range -18 .. +12 dB, so the character of the audio stays.
//  - Turning down is instant (a louder mode never blasts), turning up is
//    slow (~1 s), and silence (closed squelch) doesn't change the gain.
// =====================================================================

class AudioLeveler {
    float gain = 1.0f;
    float power = -1.0f;   // smoothed power, < 0 = no measurement yet

public:
    float targetRms = 0.1f;   // -20 dBFS
    float minGain = 0.125f;   // -18 dB (NFM noise in the 25 kHz data mode is nearly full scale)
    float maxGain = 4.0f;     // +12 dB

    void reset() { gain = 1.0f; power = -1.0f; }

    // stereo: interleaved L/R samples, processed in place
    void process(std::vector<float>& stereo) {
        if (stereo.size() < 2) return;
        double sum = 0.0;
        for (float v : stereo) sum += (double)v * v;
        const float blockPower = (float)(sum / stereo.size());

        if (blockPower > 1e-5f) {   // ignore silence (below -50 dBFS)
            if (power < 0.0f || blockPower > power) power = blockPower;   // fast attack
            else power += 0.05f * (blockPower - power);                     // ~1 s release with 50 ms blocks
        }
        const float target = (power > 0.0f) ? std::clamp(targetRms / std::sqrt(power), minGain, maxGain) : gain;

        if (target < gain) {
            // Getting quieter: apply at once
            gain = target;
            for (auto& v : stereo) v *= gain;
        } else {
            // Getting louder: ramp over the block to avoid clicks
            const size_t frames = stereo.size() / 2;
            const float step = (target - gain) / (float)frames;
            float g = gain;
            for (size_t i = 0; i + 1 < stereo.size(); i += 2) {
                g += step;
                stereo[i] *= g;
                stereo[i + 1] *= g;
            }
            gain = target;
        }
    }
};
