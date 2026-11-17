#pragma once
#include <SFML/Graphics.hpp>
#include <vector>
#include <complex>
#include <string>
#include <memory>
#include "../../UI.h"
#include "DecoderChannels.h"

struct ModuleContext {
    float sampleRate;
    const sf::Font& font;
};

class SDRModule {
public:
    std::string name;
    bool enabled = false;

    SDRModule(std::string n) : name(n) {}
    virtual ~SDRModule() {}

    virtual void init(const ModuleContext& ctx) = 0;
    
    // --- Resets the DSP state (without reloading the GUI) ---
    virtual void reset() {} 

    virtual void processAudio(const std::vector<float>& audio) {}
    virtual void processIQ(const std::vector<std::complex<double>>& iqData, double currentSampleRate, double tunedOffset) {}
    // Centre frequency of the radio in Hz (0 = unknown, e.g. a file without frequency in its name)
    virtual void setRadioFrequency(long long /*centerHz*/) {}
    // True for decoders of short bursts (ISM): the radio's AGC must not change the gain mid-packet
    virtual bool wantsBurstAgc() const { return false; }
    // True when the input switch (Source / Input) is set to Audio: then processAudio()
    // carries the signal and processIQ() must not be used for decoding
    virtual void setAudioInput(bool /*audio*/) {}
    // Bandwidth of the main tuner (Hz); decoder channels that follow the tuner may use it
    virtual void setTunerBandwidth(double /*hz*/) {}
    // Time (unix UTC seconds) of the first sample of the next processIQ()/processAudio() block:
    // the wall clock for a radio, the recording's time for a file. Slot based modes (FT8, WSPR)
    // use it to find the slot boundaries. reliable = false: a recording whose start time is only
    // guessed (from the file date), the decoder then has to find the slot boundaries itself.
    virtual void setStreamTime(double /*unixUtc*/, bool /*reliable*/) {}

    // The channels this decoder listens to (yellow bands + handles on the tuning bar).
    // nullptr for decoders without channels. Not shown while the input is audio.
    virtual ChannelSet* channelSet() { return nullptr; }

    virtual void draw(sf::RenderWindow& window, float x, float y, float w, float h) = 0;
    // Returns true if the event was used by the module's panel
    virtual bool handleEvent(const sf::Event& /*ev*/, const sf::RenderWindow& /*win*/) { return false; }
    // A popup window of the module (e.g. message details). While it is open the module
    // gets every event first, and drawModal() is called after the rest of the UI.
    virtual bool modalOpen() const { return false; }
    virtual void drawModal(sf::RenderWindow& /*window*/) {}
    // Height of the module's panel at the bottom of the waterfall
    virtual float panelHeight(float /*windowHeight*/) const { return 250.0f; }
};