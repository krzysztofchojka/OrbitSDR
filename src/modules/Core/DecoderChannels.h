#pragma once

#include <vector>
#include <string>
#include <mutex>
#include <functional>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "../../Demodulator.h"   // Mode

// =====================================================================
//  Decoder channels: what every decoder listens to.
//
//  A channel is a frequency + width (the yellow band on the spectrum and
//  the handle on the tuning bar). Each decoder demodulates its own
//  channels from the I/Q, so it never changes the main demodulator.
//
//  Placement (one rule for all decoders):
//   - when the decoder is switched on, its channels go to the well known
//     frequencies inside the received band; if there is none, the channel
//     follows the tuner (VFO),
//   - multi-channel decoders (AIS, ACARS) add / drop the automatically
//     placed channels when the radio is retuned,
//   - the user drags a handle to move a channel (it snaps to the known
//     frequencies and to the tuner); dropped on the tuner it follows it,
//   - a channel outside the band is not decoded and shown as an arrow at
//     the edge of the tuning bar.
//
//  Thread safety: update() runs on the DSP thread, the rest on the UI
//  thread; everything is guarded by one mutex.
// =====================================================================

struct DecoderChannel {
    int id = 0;
    std::string name;          // label on the handle, e.g. "APRS", "131.725", "SSTV USB"
    long long freqHz = 0;      // absolute frequency (offset when the radio frequency is unknown)
    double widthHz = 12500.0;
    int sideband = 0;          // 0: centred on freqHz, +1: freqHz .. +width (USB), -1: LSB
    Mode listenMode = Mode::NFM;   // mode used when the user clicks the handle to listen
    bool followVfo = false;    // sits on the tuner and moves with it
    bool autoPlaced = false;   // placed by the program (may be moved / dropped on retune)
    bool fallback = false;     // automatic "follow the tuner" because no known frequency is in band
    bool customWidth = false;  // width set by the user (dragged band edge): kept over shape / tuner width

    double lowOffset(double centreOffset) const {
        return sideband > 0 ? centreOffset : sideband < 0 ? centreOffset - widthHz : centreOffset - widthHz / 2.0;
    }
    double highOffset(double centreOffset) const {
        return sideband > 0 ? centreOffset + widthHz : sideband < 0 ? centreOffset : centreOffset + widthHz / 2.0;
    }
};

// A channel as the decoder sees it in one block of samples
struct ResolvedChannel {
    int id;
    double offsetHz;    // from the radio's centre frequency
    long long freqHz;   // absolute
    bool inBand;
    DecoderChannel ch;
};

class ChannelSet {
    mutable std::mutex mtx;
    std::vector<DecoderChannel> channels;
    int nextId = 1;
    long long lastCentre = -1;
    double lastRate = 0.0;
    bool placed = false;

    static constexpr double EDGE_MARGIN = 2000.0;

    bool fits(long long f, long long centre, double rate) const {
        return std::abs((double)(f - centre)) + defaultWidth / 2.0 + EDGE_MARGIN < rate / 2.0;
    }

    DecoderChannel make(long long f) {
        DecoderChannel c;
        c.id = nextId++;
        c.freqHz = f;
        c.widthHz = defaultWidth;
        c.sideband = sideband;
        c.listenMode = listenMode;
        c.name = name;
        if (shape) shape(c);
        return c;
    }

    // shape() may set the width; a width chosen by the user wins
    void applyShape(DecoderChannel& c) const {
        const double w = c.widthHz;
        if (shape) shape(c);
        if (c.customWidth) c.widthHz = w;
    }

    // In-band presets, closest to the centre first
    std::vector<long long> presetsInBand(long long centre, double rate) const {
        std::vector<long long> v;
        if (centre <= 0) return v;   // radio frequency unknown (file without frequency)
        for (long long p : presets) if (fits(p, centre, rate)) v.push_back(p);
        std::sort(v.begin(), v.end(), [&](long long a, long long b) { return std::llabs(a - centre) < std::llabs(b - centre); });
        return v;
    }

    bool hasChannelAt(long long f) const {
        for (const auto& c : channels) if (!c.followVfo && std::llabs(c.freqHz - f) < 1000) return true;
        return false;
    }

    void placeAll(long long centre, double rate, long long vfoHz) {
        channels.clear();
        auto in = presetsInBand(centre, rate);
        if (in.size() > maxChannels) in.resize(maxChannels);
        for (long long f : in) { auto c = make(f); c.autoPlaced = true; channels.push_back(c); }
        if (channels.empty()) {
            auto c = make(vfoHz);
            c.followVfo = true; c.autoPlaced = true; c.fallback = true;
            channels.push_back(c);
        }
    }

    void rebalance(long long centre, double rate, long long vfoHz) {
        // automatic channels that left the band go, newly visible known frequencies come
        channels.erase(std::remove_if(channels.begin(), channels.end(), [&](const DecoderChannel& c) {
            return c.autoPlaced && !c.followVfo && !fits(c.freqHz, centre, rate); }), channels.end());
        auto in = presetsInBand(centre, rate);
        for (long long f : in) {
            if (channels.size() >= maxChannels + (hasFallback() ? 1 : 0)) break;
            if (!hasChannelAt(f)) { auto c = make(f); c.autoPlaced = true; channels.push_back(c); }
        }
        bool anyFixed = false;
        for (const auto& c : channels) if (!c.fallback) anyFixed = true;
        if (anyFixed)
            channels.erase(std::remove_if(channels.begin(), channels.end(), [](const DecoderChannel& c) { return c.fallback; }), channels.end());
        if (channels.empty()) {
            auto c = make(vfoHz);
            c.followVfo = true; c.autoPlaced = true; c.fallback = true;
            channels.push_back(c);
        }
    }

    bool hasFallback() const {
        for (const auto& c : channels) if (c.fallback) return true;
        return false;
    }

public:
    // --- configuration (set once by the decoder) ---
    std::string name = "Decoder";
    std::vector<long long> presets;      // well known frequencies (snap targets)
    size_t maxChannels = 1;              // > 1: one channel per known frequency in band
    double defaultWidth = 12500.0;
    int sideband = 0;
    Mode listenMode = Mode::NFM;
    std::function<void(DecoderChannel&)> shape;   // optional: name / width / mode from the frequency
    // A channel that follows the tuner also takes the tuner's bandwidth (the grey band),
    // so it hears exactly what the main demodulator hears. Wide FM signals (satellite
    // SSTV with Doppler) don't fit the decoder's default width.
    bool followTunerWidth = false;
    // The decoder demodulates with the channel width, so the user may drag the band edges
    bool resizable = false;

    bool multi() const { return maxChannels > 1; }

    // Forget all channels; they are placed again on the next update (decoder switched on)
    void clear() {
        std::lock_guard<std::mutex> l(mtx);
        channels.clear();
        placed = false;
    }

    // DSP thread, every block: places the channels and resolves their offsets
    std::vector<ResolvedChannel> update(long long centreHz, double rate, double vfoOffsetHz, double tunerWidthHz = 0.0) {
        std::lock_guard<std::mutex> l(mtx);
        const long long vfoHz = centreHz + (long long)std::llround(vfoOffsetHz);
        if (!placed) {
            placeAll(centreHz, rate, vfoHz);
            placed = true;
        } else if (multi() && (centreHz != lastCentre || rate != lastRate)) {
            rebalance(centreHz, rate, vfoHz);
        }
        lastCentre = centreHz;
        lastRate = rate;

        std::vector<ResolvedChannel> out;
        for (auto& c : channels) {
            if (c.followVfo) {
                c.freqHz = vfoHz;
                applyShape(c);
                if (followTunerWidth && tunerWidthHz > 0.0 && !c.customWidth) c.widthHz = tunerWidthHz;
            }
            const double off = c.followVfo ? vfoOffsetHz : (double)(c.freqHz - centreHz);
            const bool in = std::max(std::abs(c.lowOffset(off)), std::abs(c.highOffset(off))) < rate / 2.0;
            out.push_back({c.id, off, c.freqHz, in, c});
        }
        return out;
    }

    // --- UI thread ---
    std::vector<DecoderChannel> snapshot() const {
        std::lock_guard<std::mutex> l(mtx);
        return channels;
    }

    void moveTo(int id, long long freqHz) {
        std::lock_guard<std::mutex> l(mtx);
        for (auto& c : channels) if (c.id == id) {
            c.freqHz = freqHz;
            c.followVfo = false; c.autoPlaced = false; c.fallback = false;
            applyShape(c);
        }
    }

    // The user dragged an edge of the band: this channel keeps the width
    void setChannelWidth(int id, double w) {
        std::lock_guard<std::mutex> l(mtx);
        for (auto& c : channels) if (c.id == id) { c.widthHz = w; c.customWidth = true; }
    }

    // Back to the automatic width (the decoder's default, or the tuner's while following it)
    void resetChannelWidth(int id) {
        std::lock_guard<std::mutex> l(mtx);
        for (auto& c : channels) if (c.id == id) {
            c.customWidth = false;
            c.widthHz = defaultWidth;
            if (shape) shape(c);
        }
    }

    // True while the channel's width simply is the tuner's (its edges are the grey band's)
    bool widthFollowsTuner(const DecoderChannel& c) const {
        return followTunerWidth && c.followVfo && !c.customWidth;
    }

    void followTuner(int id) {
        std::lock_guard<std::mutex> l(mtx);
        for (auto& c : channels) if (c.id == id) { c.followVfo = true; c.autoPlaced = false; c.fallback = false; }
    }

    void setWidth(double w) {
        std::lock_guard<std::mutex> l(mtx);
        defaultWidth = w;
        for (auto& c : channels) c.widthHz = w;
    }

    // One line for the decoder panels, e.g. "APRS: 144.800 MHz" or "APRS: follows the tuner"
    std::string describe() const {
        std::lock_guard<std::mutex> l(mtx);
        const long long centreHz = lastCentre;
        const double rate = lastRate;
        std::string s;
        for (const auto& c : channels) {
            // channels named by their frequency (ACARS) need no extra label
            const bool numericName = !c.name.empty() && c.name[0] >= '0' && c.name[0] <= '9';
            const std::string head = numericName ? std::string() : c.name + ": ";
            char b[96];
            if (c.followVfo) std::snprintf(b, sizeof(b), "%sfollows the tuner", head.c_str());
            else {
                const bool in = rate <= 0.0 || std::abs((double)(c.freqHz - centreHz)) < rate / 2.0;
                std::snprintf(b, sizeof(b), "%s%.3f MHz%s", head.c_str(), c.freqHz / 1e6, in ? "" : " (out of band!)");
            }
            if (!s.empty()) s += "   ";
            s += b;
        }
        return s;
    }
};
