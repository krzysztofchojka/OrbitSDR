#pragma once
#include <SFML/Graphics.hpp>
#include <vector>
#include <complex>
#include <string>
#include <memory>
#include "../../UI.h"

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

    virtual void draw(sf::RenderWindow& window, float x, float y, float w, float h) = 0;
    // Returns true if the event was used by the module's panel
    virtual bool handleEvent(const sf::Event& /*ev*/, const sf::RenderWindow& /*win*/) { return false; }
    // Height of the module's panel at the bottom of the waterfall
    virtual float panelHeight(float /*windowHeight*/) const { return 250.0f; }
};