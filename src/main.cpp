#define MINIAUDIO_IMPLEMENTATION
#include <SFML/Graphics.hpp>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <optional>
#include <algorithm>

#include "DSP.h"
#include "AudioSink.h"
#include "Demodulator.h"
#include "UI.h"
#include "NativeDialogs.h"
#include "IQSources.h" // Changed from IQSources.h to match previous file

const int W_WIDTH = 1200, W_HEIGHT = 800;
const int SPEC_W = 900, SPEC_H = 250;
const int WATERFALL_H = 400;
const int FFT_SIZE = 1024;
const double AUDIO_RATE = 48000.0;
const int TOP_BAR_H = 60; 

const std::vector<uint32_t> RTL_RATES_VAL = {1024000, 1400000, 1800000, 2048000, 2400000, 3200000};
const std::vector<uint32_t> SDRPLAY_RATES_VAL = {2000000, 4000000, 6000000, 8000000, 10000000};

struct SharedData {
    std::mutex mtx;
    double tunedFreqPercent = 0.5;
    double bandwidth = 12000.0;
    float gain = 1.0f;
    Mode mode = Mode::NFM;
    bool isPlaying = false;
    float minDb = -120.0f;
    float maxDb = 0.0f;
    
    std::vector<double> fftSpectrum;
    std::vector<uint8_t> waterfallRow;
    bool newWaterfallData = false;
    
    std::string currentFilename = "None";
    float mouseX_spectrum = -1.0f; 
    float mouseY_spectrum = -1.0f;

    SharedData() : fftSpectrum(FFT_SIZE, -100.0), waterfallRow(SPEC_W * 4, 0) {}
};

std::mutex sourceMtx;
std::shared_ptr<IQSource> currentSource;

sf::Color getHeatmap(float v) {
    v = std::clamp(v, 0.0f, 1.0f);
    std::uint8_t r = 0, g = 0, b = 0;
    
    if (v < 0.25f) {
        b = static_cast<std::uint8_t>(v * 4 * 255);
    } else if (v < 0.5f) {
        b = 255; 
        g = static_cast<std::uint8_t>((v - 0.25f) * 4 * 255);
    } else if (v < 0.75f) {
        r = static_cast<std::uint8_t>((v - 0.5f) * 4 * 255); 
        g = 255; 
        b = static_cast<std::uint8_t>(255 - r);
    } else {
        r = 255; 
        g = static_cast<std::uint8_t>((1.0f - v) * 4 * 255);
    } 
    return {r, g, b};
}

std::string formatHz(long long hz) {
    std::stringstream ss;
    ss << std::fixed << std::setprecision(3) << (hz / 1000000.0) << " MHz";
    return ss.str();
}

// Draw Frequency Grid
void drawGrid(sf::RenderWindow& window, const sf::Font& font, float x, float y, float w, float h, long long cf, int sr, float minDb, float maxDb) {
    float dbStep = 20.0f;
    
    // Horizontal dB lines
    for (float db = 0; db >= -140; db -= dbStep) {
        if (db > maxDb || db < minDb) continue;
        float norm = (db - minDb) / (maxDb - minDb);
        float yPos = y + h - (norm * h);
        
        sf::RectangleShape line({w, 1}); 
        line.setPosition({x, yPos}); 
        line.setFillColor(sf::Color(100, 100, 100, 150)); 
        window.draw(line);
        
        sf::Text l(font, std::to_string((int)db), 10); 
        l.setPosition({x + 2, yPos - 12}); 
        l.setFillColor(sf::Color::White); 
        window.draw(l);
    }

    // Vertical Frequency lines
    double startFreq = (double)cf - sr / 2.0;
    for (int i = 0; i <= 8; i++) {
        float xPos = x + (w / 8) * i;
        
        sf::RectangleShape line({1, h}); 
        line.setPosition({xPos, y}); 
        line.setFillColor(sf::Color(100, 100, 100, 150)); 
        window.draw(line);
        
        double freqAtPoint = startFreq + (double)sr * ((double)i / 8.0);
        std::string freqStr = formatHz((long long)freqAtPoint);
        if (freqStr.size() > 4) freqStr = freqStr.substr(0, freqStr.size() - 4); 
        
        sf::Text l(font, freqStr, 10);
        sf::FloatRect b = l.getLocalBounds();
        l.setPosition({xPos - b.size.x / 2, y + h - 15}); 
        l.setFillColor(sf::Color::Yellow);
        window.draw(l);
    }
}

// DSP Thread Function
void dspWorker(std::atomic<bool>& running, SharedData& shared, AudioSink& audio) {
    Demodulator demod(2000000, AUDIO_RATE); 
    double lastSampleRate = 0;
    std::vector<Complex> iqBuffer;
    std::vector<double> winFunc = makeWindow(FFT_SIZE);
    std::vector<double> localFftHistory(FFT_SIZE, -100.0);

    while (running) {
        std::shared_ptr<IQSource> src = nullptr;
        { 
            std::lock_guard<std::mutex> lock(sourceMtx); 
            src = currentSource; 
        }
        
        if (!src) { 
            std::this_thread::sleep_for(std::chrono::milliseconds(10)); 
            continue; 
        }

        double targetFreqPct, bw; 
        float audioGain; 
        Mode mode; 
        bool play; 
        float minDb, maxDb;
        
        {
            std::lock_guard<std::mutex> lock(shared.mtx);
            targetFreqPct = shared.tunedFreqPercent; 
            bw = shared.bandwidth;
            audioGain = shared.gain; 
            mode = shared.mode; 
            play = shared.isPlaying;
            minDb = shared.minDb; 
            maxDb = shared.maxDb;
        }

        if (!play) { 
            std::this_thread::sleep_for(std::chrono::milliseconds(50)); 
            continue; 
        }
        
        // Backpressure handling for file sources
        if (!src->isHardware()) {
            while (audio.getBufferedCount() > (AUDIO_RATE * 0.2)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                if (!running) return;
            }
        }

        double sr = src->getSampleRate();
        if (sr != lastSampleRate) { 
            demod = Demodulator(sr, AUDIO_RATE); 
            lastSampleRate = sr; 
        }

        int chunkSize = (int)sr / 60; 
        if (chunkSize > 200000) chunkSize = 200000;
        if (iqBuffer.size() != chunkSize) iqBuffer.resize(chunkSize);

        int readCount = src->read(iqBuffer.data(), chunkSize);

        if (readCount > 0) {
            double freqOffset = (targetFreqPct - 0.5) * sr;
            if (mode == Mode::USB) freqOffset += bw / 2.0;
            if (mode == Mode::LSB) freqOffset -= bw / 2.0;

            std::vector<Complex> chunkToProcess(iqBuffer.begin(), iqBuffer.begin() + readCount);
            auto audioData = demod.process(chunkToProcess, freqOffset, bw, mode);
            
            for (auto& s : audioData) s *= audioGain;
            audio.pushSamples(audioData);

            // FFT Calculation
            std::vector<Complex> fftData(FFT_SIZE);
            for (size_t i = 0; i < FFT_SIZE && i < chunkToProcess.size(); i++) {
                fftData[i] = chunkToProcess[i] * winFunc[i];
            }
            fft(fftData);

            // Generate Waterfall Row
            std::vector<uint8_t> tempRow(SPEC_W * 4);
            for (int x = 0; x < SPEC_W; x++) {
                int fftIdx = (int)((float)x / SPEC_W * FFT_SIZE);
                int shiftedIdx = (fftIdx + FFT_SIZE / 2) % FFT_SIZE;
                
                float rawMag = std::abs(fftData[shiftedIdx]) / FFT_SIZE;
                float rawDb = 20 * std::log10(rawMag + 1e-12);
                float norm = (rawDb - minDb) / (maxDb - minDb); 
                
                sf::Color c = getHeatmap(norm);
                int px = x * 4;
                tempRow[px] = c.r; 
                tempRow[px + 1] = c.g; 
                tempRow[px + 2] = c.b; 
                tempRow[px + 3] = 255;
            }

            // Smooth Spectrum for Line Graph
            for (int i = 0; i < FFT_SIZE; i++) {
                 int idx = (i + FFT_SIZE / 2) % FFT_SIZE;
                 float mag = std::abs(fftData[idx]) / FFT_SIZE;
                 float db = 20 * std::log10(mag + 1e-12);
                 localFftHistory[i] = localFftHistory[i] * 0.7 + db * 0.3;
            }
            
            {
                std::lock_guard<std::mutex> lock(shared.mtx);
                shared.fftSpectrum = localFftHistory;
                shared.waterfallRow = tempRow;
                shared.newWaterfallData = true;
            }
        } else { 
            std::this_thread::sleep_for(std::chrono::milliseconds(10)); 
        }
    }
}

int main() {
    {
        std::lock_guard<std::mutex> lock(sourceMtx);
        currentSource = std::make_shared<FileSource>();
        if (currentSource->open("None")) {}
    }

    AudioSink audio;
    std::vector<std::string> deviceNames;
    for (const auto& dev : audio.availableDevices) deviceNames.push_back(dev.name);
    
    audio.initDevice(0, (int)AUDIO_RATE);
    audio.start();

    SharedData sharedData;
    sharedData.isPlaying = false;

    std::atomic<bool> dspRunning {true};
    std::thread dspThread(dspWorker, std::ref(dspRunning), std::ref(sharedData), std::ref(audio));

    sf::RenderWindow window(sf::VideoMode({(unsigned)W_WIDTH, (unsigned)W_HEIGHT}), "OrbitSDR");
    window.setFramerateLimit(60);

    auto cursorArrow = sf::Cursor::createFromSystem(sf::Cursor::Type::Arrow);
    auto cursorHand = sf::Cursor::createFromSystem(sf::Cursor::Type::Hand);

    sf::Font font; 
    if (!font.openFromFile("/System/Library/Fonts/Helvetica.ttc") && 
        !font.openFromFile("C:/Windows/Fonts/arial.ttf") && 
        !font.openFromFile("arial.ttf")) {
        // Font loading failed handling if needed
    }

    int px = 920; 
    
    // === UI SETUP ===
    sf::RectangleShape topBar({(float)W_WIDTH, (float)TOP_BAR_H});
    topBar.setFillColor(sf::Color(30, 30, 30)); 
    topBar.setPosition({0, 0});
    
    FrequencyDisplay freqVFO(20, 8, font); 
    freqVFO.setFrequency(100000000);

    bool stickyCenterMode = false; 
    SdrButton btnTuningMode(320, 10, 40, 40, "FIX", font); 
    btnTuningMode.setColor(sf::Color(80, 80, 80));

    // PLAY Button
    SdrButton btnPlay(370, 10, 40, 40, ">", font); 
    btnPlay.setColor(sf::Color(150, 0, 0)); 
    bool isPlaying = false; 

    // Dropdowns
    sf::Text labelSource(font, "Source:", 12); 
    labelSource.setPosition({430, 10});
    Dropdown sourceDropdown(430, 25, 160, 25, font);
    sourceDropdown.setOptions({"File (WAV)", "RTL-SDR", "SDRPlay"});

    sf::Text labelRate(font, "File:", 12); 
    labelRate.setPosition({600, 10});
    Dropdown rateDropdown(600, 25, 160, 25, font);
    rateDropdown.setOptions({"None"}); 

    sf::Text labelAudio(font, "Audio:", 12); 
    labelAudio.setPosition({770, 10});
    Dropdown audioDropdown(770, 25, 160, 25, font);
    audioDropdown.setOptions(deviceNames);

    // Sliders
    int sideY = TOP_BAR_H + 10; 
    int sliderY = sideY + 30; 
    Slider gainSlider(px, sliderY, 200, 0.0f, 50.0f, 1.0f, "Audio Gain", font);
    Slider bwSlider(px, sliderY+50, 200, 1000.0f, 220000.0f, 12000.0f, "Filter BW (Hz)", font);
    Slider minDbSlider(px, sliderY+100, 200, -120.0f, -20.0f, -90.0f, "Min dB", font);
    Slider maxDbSlider(px, sliderY+150, 200, -40.0f, 40.0f, 0.0f, "Max dB", font);

    // Mode Buttons
    int btnY = sliderY + 200;
    SdrButton btnNFM(px, btnY, 45, 30, "NFM", font);
    SdrButton btnAM(px + 50, btnY, 45, 30, "AM", font);
    SdrButton btnWFM(px + 100, btnY, 45, 30, "WFM", font);
    SdrButton btnOFF(px + 150, btnY, 45, 30, "OFF", font);
    SdrButton btnLSB(px, btnY+40, 95, 30, "LSB", font);
    SdrButton btnUSB(px + 100, btnY+40, 95, 30, "USB", font);
    btnNFM.setActive(true); 

    Slider timeSlider(20, W_HEIGHT - 30, W_WIDTH - 40, 0.0f, 1.0f, 0.0f, "Timeline", font);

    // Waterfall Textures
    std::vector<std::uint8_t> waterfall(SPEC_W * WATERFALL_H * 4, 0);
    sf::Texture wTex; 
    if (!wTex.resize({(unsigned)SPEC_W, (unsigned)WATERFALL_H})) return 1;
    sf::Sprite wSpr(wTex); 
    wSpr.setPosition({0, (float)SPEC_H + TOP_BAR_H}); 
    
    long long currentCenterFreq = 0;
    long long pendingCenterFreq = 0;
    sf::Clock debouncer;

    auto resetBtns = [&](SdrButton* active) {
        btnNFM.setActive(false); btnAM.setActive(false); btnWFM.setActive(false); 
        btnOFF.setActive(false); btnLSB.setActive(false); btnUSB.setActive(false);
        active->setActive(true);
    };

    // Change Source Lambda
    auto changeSource = [&](int sourceIdx, int rateIdx, std::string pathOverride = "") {
        std::shared_ptr<IQSource> oldSource;
        { 
            std::lock_guard<std::mutex> lock(sourceMtx); 
            oldSource = currentSource; 
            currentSource = nullptr; 
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (oldSource) { oldSource->stop(); oldSource->close(); }

        std::shared_ptr<IQSource> newSource;
        uint32_t targetRate = 0;

        if (sourceIdx == 1) {
             if (rateIdx < RTL_RATES_VAL.size()) targetRate = RTL_RATES_VAL[rateIdx];
             else targetRate = 2048000;
        }
        if (sourceIdx == 2) {
             if (rateIdx < SDRPLAY_RATES_VAL.size()) targetRate = SDRPLAY_RATES_VAL[rateIdx];
             else targetRate = 2000000;
        }

        if (sourceIdx == 0) { 
            // FILE SOURCE
            newSource = std::make_shared<FileSource>(); 
            std::string path = pathOverride.empty() ? "None" : pathOverride;
            { 
                std::lock_guard<std::mutex> lock(sharedData.mtx);
                size_t lastSlash = path.find_last_of("/\\");
                sharedData.currentFilename = (lastSlash == std::string::npos) ? path : path.substr(lastSlash + 1);
                
                // Parse Frequency from filename (e.g., "recording_104500000Hz.wav")
                std::string fn = sharedData.currentFilename;
                currentCenterFreq = 0; 
                size_t hzPos = fn.find("Hz");
                if (hzPos != std::string::npos) {
                    size_t underscorePos = fn.rfind('_', hzPos);
                    if (underscorePos != std::string::npos) {
                        std::string freqStr = fn.substr(underscorePos + 1, hzPos - underscorePos - 1);
                        try {
                            long long parsedFreq = std::stoll(freqStr);
                            currentCenterFreq = parsedFreq;
                            freqVFO.setFrequency(parsedFreq); 
                        } catch (...) {}
                    }
                }
            }
            newSource->open(path);
            rateDropdown.setOptions({sharedData.currentFilename});

        } else if (sourceIdx == 1) { 
            // RTL-SDR
            newSource = std::make_shared<RtlSdrSource>();
            rateDropdown.setOptions(newSource->getAvailableSampleRatesText());
            rateDropdown.setSelection(rateIdx);
            if (!newSource->open("0", targetRate)) {
                 sourceDropdown.selectedIndex = 0; 
                 sourceDropdown.selectedText.setString("File");
                 newSource = std::make_shared<FileSource>(); 
                 newSource->open("None");
            } else {
                newSource->setCenterFrequency(freqVFO.getFrequency());
                currentCenterFreq = freqVFO.getFrequency();
            }

        } else { 
            // SDRPlay
            newSource = std::make_shared<SdrPlaySource>();
            rateDropdown.setOptions(newSource->getAvailableSampleRatesText());
            rateDropdown.setSelection(rateIdx);
            if (newSource->open("", targetRate)) { 
                newSource->setCenterFrequency(freqVFO.getFrequency());
                currentCenterFreq = freqVFO.getFrequency();
            }
        }

        { std::lock_guard<std::mutex> lock(sourceMtx); currentSource = newSource; }
        { std::lock_guard<std::mutex> lock(sharedData.mtx); sharedData.isPlaying = false; }
        
        isPlaying = false;
        audio.stop(); 
        btnPlay.setText(">"); 
        btnPlay.setColor(sf::Color(150, 0, 0));
        audio.clear(); 
        std::fill(waterfall.begin(), waterfall.end(), 0);
    };

    // === MAIN LOOP ===
    while (window.isOpen()) {
        {
             std::lock_guard<std::mutex> lock(sharedData.mtx);
             sharedData.gain = gainSlider.currentVal;
             sharedData.bandwidth = bwSlider.currentVal;
             sharedData.minDb = minDbSlider.currentVal;
             sharedData.maxDb = maxDbSlider.currentVal;
        }
        
        // Check State (Hardware vs File)
        bool isHw = false;
        double hwSampleRate = 2000000.0;
        { 
            std::lock_guard<std::mutex> l(sourceMtx); 
            if (currentSource) { 
                isHw = currentSource->isHardware(); 
                hwSampleRate = currentSource->getSampleRate(); 
            } 
        }

        // UI Updates based on Source Type
        if (isHw) {
            labelRate.setString("Rate:");
            freqVFO.setEnabled(true); 
        } else {
            labelRate.setString("File:");
            freqVFO.setEnabled(false); 
        }

        if (!sourceDropdown.isOpen && !rateDropdown.isOpen && !audioDropdown.isOpen) {
             freqVFO.update(window);
        }

        // POLL EVENTS
        while (const std::optional<sf::Event> ev = window.pollEvent()) {
            if (ev->is<sf::Event::Closed>()) window.close();

            // Handle Dropdowns
            if (sourceDropdown.handleEvent(*ev, window)) {
                changeSource(sourceDropdown.selectedIndex, 0);
            }
            
            // Special handling for File Dialog triggering vs Rate selection
            bool fileDialogTriggered = false;
            if (!isHw && !sourceDropdown.isOpen && !audioDropdown.isOpen) {
                if (const auto* mb = ev->getIf<sf::Event::MouseButtonPressed>()) {
                     if (mb->button == sf::Mouse::Button::Left) {
                        sf::Vector2f m = window.mapPixelToCoords(sf::Mouse::getPosition(window));
                        if (rateDropdown.mainBox.getGlobalBounds().contains(m)) {
                            std::string path = openFileDialog();
                            if (!path.empty()) changeSource(0, 0, path);
                            fileDialogTriggered = true;
                        }
                     }
                }
            }
            
            if (!fileDialogTriggered && isHw) {
                if (rateDropdown.handleEvent(*ev, window)) {
                    changeSource(sourceDropdown.selectedIndex, rateDropdown.selectedIndex);
                }
            }

            if (audioDropdown.handleEvent(*ev, window)) { 
                audio.stop(); 
                audio.initDevice(audioDropdown.selectedIndex, (int)AUDIO_RATE); 
                if (isPlaying) audio.start(); 
            }


            if (!sourceDropdown.isOpen && !rateDropdown.isOpen && !audioDropdown.isOpen) {
                
                // VFO Logic
                if (isHw && freqVFO.handleEvent(*ev)) { 
                        long long targetVFO = freqVFO.getFrequency();
                        if (stickyCenterMode) {
                            pendingCenterFreq = targetVFO;
                            debouncer.restart();
                            { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.tunedFreqPercent = 0.5; }
                        } else {
                            double halfBW = hwSampleRate / 2.0;
                            double minF = (double)currentCenterFreq - halfBW;
                            double maxF = (double)currentCenterFreq + halfBW;
                            
                            if (targetVFO > maxF || targetVFO < minF) {
                                pendingCenterFreq = targetVFO;
                                debouncer.restart();
                                { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.tunedFreqPercent = 0.5; }
                            } else {
                                double pct = 0.5 + ((double)(targetVFO - currentCenterFreq) / hwSampleRate);
                                { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.tunedFreqPercent = pct; }
                            }
                        }
                }

                // FIX/CTR Toggle
                if (isHw && btnTuningMode.isClicked(*ev, window)) {
                    stickyCenterMode = !stickyCenterMode;
                    if (stickyCenterMode) {
                        btnTuningMode.setText("CTR"); 
                        btnTuningMode.setColor(sf::Color(0, 100, 200)); 
                        pendingCenterFreq = freqVFO.getFrequency();
                        debouncer.restart();
                        { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.tunedFreqPercent = 0.5; }
                    } else {
                        btnTuningMode.setText("FIX"); 
                        btnTuningMode.setColor(sf::Color(80, 80, 80)); 
                    }
                }

                // Sliders & Buttons
                gainSlider.handleEvent(*ev, window); 
                bwSlider.handleEvent(*ev, window);
                minDbSlider.handleEvent(*ev, window); 
                maxDbSlider.handleEvent(*ev, window);
                if (!isHw) timeSlider.handleEvent(*ev, window);

                if (btnNFM.isClicked(*ev, window)) { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.mode = Mode::NFM; resetBtns(&btnNFM); bwSlider.currentVal = 12000; bwSlider.updateHandlePos(); }
                if (btnAM.isClicked(*ev, window))  { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.mode = Mode::AM; resetBtns(&btnAM); bwSlider.currentVal = 8000; bwSlider.updateHandlePos(); }
                if (btnWFM.isClicked(*ev, window)) { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.mode = Mode::WFM; resetBtns(&btnWFM); bwSlider.currentVal = 180000; bwSlider.updateHandlePos(); }
                if (btnOFF.isClicked(*ev, window)) { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.mode = Mode::OFF; resetBtns(&btnOFF); }
                if (btnLSB.isClicked(*ev, window)) { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.mode = Mode::LSB; resetBtns(&btnLSB); bwSlider.currentVal = 3000; bwSlider.updateHandlePos(); }
                if (btnUSB.isClicked(*ev, window)) { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.mode = Mode::USB; resetBtns(&btnUSB); bwSlider.currentVal = 3000; bwSlider.updateHandlePos(); }

                if (btnPlay.isClicked(*ev, window)) {
                    bool s; 
                    { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.isPlaying = !sharedData.isPlaying; s = sharedData.isPlaying; }
                    if (s) { 
                        btnPlay.setText("||"); 
                        btnPlay.setColor(sf::Color(0, 150, 0)); 
                        audio.start(); 
                        { std::lock_guard<std::mutex> l(sourceMtx); if (currentSource) currentSource->start(); }
                    } else { 
                        btnPlay.setText(">"); 
                        btnPlay.setColor(sf::Color(150, 0, 0)); 
                        audio.stop();
                        { std::lock_guard<std::mutex> l(sourceMtx); if (currentSource) currentSource->stop(); }
                    }
                    isPlaying = s;
                }

                // Waterfall Tuning
                if (const auto* mb = ev->getIf<sf::Event::MouseButtonPressed>()) {
                    if (mb->button == sf::Mouse::Button::Left) {
                        sf::Vector2f m = window.mapPixelToCoords(sf::Mouse::getPosition(window));
                        float graphY = m.y - TOP_BAR_H;
                        if (m.x < SPEC_W && graphY >= 0 && graphY < (SPEC_H + WATERFALL_H)) { 
                            double clickPct = m.x / SPEC_W;
                            double offsetHz = (clickPct - 0.5) * hwSampleRate;
                            long long clickedFreq = currentCenterFreq + (long long)offsetHz;

                            if (stickyCenterMode && isHw) {
                                pendingCenterFreq = clickedFreq;
                                debouncer.restart();
                                { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.tunedFreqPercent = 0.5; }
                                freqVFO.setFrequency(clickedFreq);
                            } else {
                                { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.tunedFreqPercent = clickPct; }
                                freqVFO.setFrequency(clickedFreq);
                            }
                        }
                    }
                }

                // Mouse Hover Spectrum
                if (const auto* me = ev->getIf<sf::Event::MouseMoved>()) {
                    sf::Vector2f m = window.mapPixelToCoords(me->position);
                    float graphY = m.y - TOP_BAR_H;
                    if (m.x >= 0 && m.x < SPEC_W && graphY >= 0 && graphY < SPEC_H + WATERFALL_H) {
                        std::lock_guard<std::mutex> l(sharedData.mtx);
                        sharedData.mouseX_spectrum = m.x;
                        sharedData.mouseY_spectrum = graphY; 
                    } else {
                        std::lock_guard<std::mutex> l(sharedData.mtx);
                        sharedData.mouseX_spectrum = -1.0f; 
                    }
                }
            }
        }
        
        // --- CURSORS HANDLING ---
        {
            bool hover = false;
            sf::Vector2f m = window.mapPixelToCoords(sf::Mouse::getPosition(window));

            if (btnPlay.shape.getGlobalBounds().contains(m)) hover = true;
            if (freqVFO.enabled && freqVFO.isHovered) hover = true;
            if (isHw && btnTuningMode.shape.getGlobalBounds().contains(m)) hover = true;
            if (!isHw && rateDropdown.mainBox.getGlobalBounds().contains(m)) hover = true; 

            if (btnNFM.shape.getGlobalBounds().contains(m)) hover = true;
            if (btnAM.shape.getGlobalBounds().contains(m)) hover = true;
            if (btnWFM.shape.getGlobalBounds().contains(m)) hover = true;
            if (btnOFF.shape.getGlobalBounds().contains(m)) hover = true;
            if (btnLSB.shape.getGlobalBounds().contains(m)) hover = true;
            if (btnUSB.shape.getGlobalBounds().contains(m)) hover = true;

            if (gainSlider.track.getGlobalBounds().contains(m)) hover = true;
            if (bwSlider.track.getGlobalBounds().contains(m)) hover = true;
            if (minDbSlider.track.getGlobalBounds().contains(m)) hover = true;
            if (maxDbSlider.track.getGlobalBounds().contains(m)) hover = true;
            if (!isHw && timeSlider.track.getGlobalBounds().contains(m)) hover = true;

            if (sourceDropdown.mainBox.getGlobalBounds().contains(m)) hover = true;
            if (isHw && rateDropdown.mainBox.getGlobalBounds().contains(m)) hover = true; 
            if (audioDropdown.mainBox.getGlobalBounds().contains(m)) hover = true;

            if (hover) { if (cursorHand) window.setMouseCursor(*cursorHand); } 
            else { if (cursorArrow) window.setMouseCursor(*cursorArrow); }
        }

        // Pending Tune Execution (Debounce)
        if (pendingCenterFreq != 0 && debouncer.getElapsedTime().asMilliseconds() > 150) {
             std::lock_guard<std::mutex> l(sourceMtx);
             if (currentSource && currentSource->isHardware()) {
                 currentSource->setCenterFrequency(pendingCenterFreq);
                 currentCenterFreq = pendingCenterFreq;
             }
             pendingCenterFreq = 0;
        }

        // Update Sliders
        if (!sourceDropdown.isOpen) {
             gainSlider.update(window); 
             bwSlider.update(window);
             minDbSlider.update(window); 
             maxDbSlider.update(window);
             
             bool isHw = false; 
             double prog = 0; 
             { std::lock_guard<std::mutex> l(sourceMtx); if (currentSource) { isHw = currentSource->isHardware(); prog = currentSource->getProgress(); } }
             
             if (!isHw) {
                 timeSlider.update(window);
                 if (timeSlider.isDragging) { 
                     std::lock_guard<std::mutex> l(sourceMtx); 
                     if (currentSource) currentSource->seek(timeSlider.currentVal); 
                 } else { 
                     timeSlider.currentVal = prog; 
                     timeSlider.updateHandlePos(); 
                 }
             }
        }

        // Retrieve Data for Rendering
        std::vector<double> spectrum; 
        std::vector<uint8_t> row; 
        bool newRow = false; 
        double tunePct = 0.5; 
        Mode mode = Mode::NFM;
        {
            std::lock_guard<std::mutex> lock(sharedData.mtx);
            spectrum = sharedData.fftSpectrum;
            if (sharedData.newWaterfallData) { 
                row = sharedData.waterfallRow; 
                sharedData.newWaterfallData = false; 
                newRow = true; 
            }
            tunePct = sharedData.tunedFreqPercent; 
            mode = sharedData.mode;
        }

        if (newRow) {
             std::copy_backward(waterfall.begin(), waterfall.end() - SPEC_W * 4, waterfall.end());
             std::copy(row.begin(), row.end(), waterfall.begin());
             wTex.update(waterfall.data());
        }

        // === DRAWING ===
        window.clear(sf::Color::Black);
        
        long long cf = 0; 
        double sr = 2e6;
        { std::lock_guard<std::mutex> l(sourceMtx); if (currentSource) { sr = currentSource->getSampleRate(); } }
        if (currentSource) cf = currentCenterFreq;

        // Header and Grid
        window.draw(topBar);
        window.draw(labelSource); sourceDropdown.draw(window);
        window.draw(labelRate); rateDropdown.draw(window);
        window.draw(labelAudio); audioDropdown.draw(window);

        drawGrid(window, font, 0, TOP_BAR_H, SPEC_W, SPEC_H, cf, sr, minDbSlider.currentVal, maxDbSlider.currentVal);

        // Draw Spectrum Line
        sf::VertexArray lines(sf::PrimitiveType::Lines, spectrum.size());
        for (size_t i = 0; i < spectrum.size(); i++) {
             float norm = (spectrum[i] - minDbSlider.currentVal) / (maxDbSlider.currentVal - minDbSlider.currentVal);
             float y = SPEC_H - (norm * SPEC_H);
             if (y < 0) y = 0; 
             if (y > SPEC_H) y = SPEC_H;
             
             lines[i].position = { (float)i / spectrum.size() * SPEC_W, y + TOP_BAR_H }; 
             lines[i].color = sf::Color::Cyan;
        }
        window.draw(lines);
        window.draw(wSpr); 

        // Mouse Guidelines
        float mouseX = -1.0f; 
        float mouseY = -1.0f;
        { std::lock_guard<std::mutex> lock(sharedData.mtx); mouseX = sharedData.mouseX_spectrum; mouseY = sharedData.mouseY_spectrum; }

        if (mouseX != -1.0f) {
            sf::Color guideColor(100, 100, 100);
            sf::VertexArray lineFFT(sf::PrimitiveType::Lines, 2);
            lineFFT[0].position = {mouseX, (float)TOP_BAR_H}; 
            lineFFT[0].color = guideColor;
            lineFFT[1].position = {mouseX, (float)SPEC_H + TOP_BAR_H}; 
            lineFFT[1].color = guideColor;
            window.draw(lineFFT);

            if (mouseY > (float)SPEC_H) {
                sf::VertexArray lineWaterfall(sf::PrimitiveType::Lines, 2);
                lineWaterfall[0].position = {mouseX, (float)SPEC_H + TOP_BAR_H}; 
                lineWaterfall[0].color = guideColor;
                lineWaterfall[1].position = {mouseX, (float)(SPEC_H + WATERFALL_H + TOP_BAR_H)}; 
                lineWaterfall[1].color = guideColor;
                window.draw(lineWaterfall);
            }
        }

        // Tuner Filter Overlay
        sf::RectangleShape tunerRect;
        float bwPixels = (bwSlider.currentVal / sr) * SPEC_W; 
        if (bwPixels < 2.0f) bwPixels = 2.0f;
        
        float rectX = tunePct * SPEC_W - bwPixels / 2.0f;
        if (mode == Mode::USB) rectX += bwPixels / 2.0f; 
        if (mode == Mode::LSB) rectX -= bwPixels / 2.0f;
        
        tunerRect.setSize({bwPixels, (float)SPEC_H}); 
        tunerRect.setPosition({rectX, (float)TOP_BAR_H});
        tunerRect.setFillColor(mode == Mode::OFF ? sf::Color(50, 50, 50, 40) : sf::Color(0, 255, 0, 40));
        tunerRect.setOutlineThickness(0); 
        window.draw(tunerRect);

        // Center Line
        sf::VertexArray centerLine(sf::PrimitiveType::Lines, 2);
        float centerX = tunePct * SPEC_W;
        centerLine[0].position = {centerX, (float)TOP_BAR_H}; 
        centerLine[0].color = sf::Color::Red; 
        centerLine[1].position = {centerX, (float)SPEC_H + TOP_BAR_H}; 
        centerLine[1].color = sf::Color::Red; 
        window.draw(centerLine);

        if (!isHw) { timeSlider.draw(window); }
        else { btnTuningMode.draw(window); }
        
        freqVFO.draw(window); 
        btnPlay.draw(window);

        gainSlider.draw(window); 
        bwSlider.draw(window); 
        minDbSlider.draw(window); 
        maxDbSlider.draw(window);
        
        btnNFM.draw(window); 
        btnAM.draw(window); 
        btnWFM.draw(window); 
        btnOFF.draw(window); 
        btnLSB.draw(window); 
        btnUSB.draw(window);

        // Draw Dropdowns on top
        if (audioDropdown.isOpen) audioDropdown.draw(window);
        if (rateDropdown.isOpen) rateDropdown.draw(window);
        if (sourceDropdown.isOpen) sourceDropdown.draw(window);

        window.display();
    }
    
    dspRunning = false;
    if (dspThread.joinable()) dspThread.join();
    return 0;
}