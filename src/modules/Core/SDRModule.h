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
    
    // --- NOWOŚĆ: Metoda do resetowania stanu DSP (bez przeładowania GUI) ---
    virtual void reset() {} 

    virtual void processAudio(const std::vector<float>& audio) {}
    virtual void processIQ(const std::vector<std::complex<double>>& iqData, double currentSampleRate, double tunedOffset) {}

    virtual void draw(sf::RenderWindow& window, float x, float y, float w, float h) = 0;
    virtual void handleEvent(const sf::Event& ev, const sf::RenderWindow& win) {}
};