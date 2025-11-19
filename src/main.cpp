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
#include <fstream>
#include <ctime>

#include "DSP.h"
#include "AudioSink.h"
#include "Demodulator.h"
#include "UI.h"
#include "NativeDialogs.h"
#include "IQSources.h"

const int W_WIDTH = 1200, W_HEIGHT = 800;
const int SPEC_W = 900, SPEC_H = 250;
const int WATERFALL_H = 400;
const int FFT_SIZE = 1024;
const double AUDIO_RATE = 48000.0;
const int TOP_BAR_H = 60; 

const std::vector<uint32_t> RTL_RATES_VAL = {1024000, 1400000, 1800000, 2048000, 2400000, 3200000};
const std::vector<uint32_t> SDRPLAY_RATES_VAL = {2000000, 4000000, 6000000, 8000000, 10000000};

// --- WAV WRITER HELPER ---
struct WavWriter {
    std::ofstream file;
    uint32_t dataSize = 0;
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    bool active = false;

    void start(std::string path, uint32_t sr, uint16_t ch, bool isFloat = true) {
        if (active) stop();
        file.open(path, std::ios::binary);
        if (!file.is_open()) { std::cerr << "Failed to create file: " << path << "\n"; return; }
        
        sampleRate = sr;
        channels = ch;
        dataSize = 0;
        active = true;

        // Placeholder Header
        char header[44] = {0};
        file.write(header, 44);
    }

    void write(const float* data, size_t count) {
        if (!active) return;
        // Convert float (-1..1) to int16 for compatibility
        for(size_t i=0; i<count; i++) {
            float s = std::clamp(data[i], -1.0f, 1.0f);
            int16_t val = static_cast<int16_t>(s * 32767.0f);
            file.write((char*)&val, sizeof(int16_t));
        }
        dataSize += count * sizeof(int16_t);
    }

    void stop() {
        if (!active || !file.is_open()) return;
        
        // Fill Header
        file.seekp(0);
        uint32_t fileSize = dataSize + 36;
        uint32_t byteRate = sampleRate * channels * 2; // 16-bit
        uint16_t blockAlign = channels * 2;
        
        file.write("RIFF", 4);
        file.write((char*)&fileSize, 4);
        file.write("WAVE", 4);
        file.write("fmt ", 4);
        uint32_t subchunk1Size = 16;
        uint16_t audioFormat = 1; // PCM
        uint16_t bitsPerSample = 16;
        
        file.write((char*)&subchunk1Size, 4);
        file.write((char*)&audioFormat, 2);
        file.write((char*)&channels, 2);
        file.write((char*)&sampleRate, 4);
        file.write((char*)&byteRate, 4);
        file.write((char*)&blockAlign, 2);
        file.write((char*)&bitsPerSample, 2);
        file.write("data", 4);
        file.write((char*)&dataSize, 4);

        file.close();
        active = false;
    }
};

enum class RecMode { AUDIO, BASEBAND };

struct SharedData {
    std::mutex mtx;
    double tunedFreqPercent = 0.5;
    double bandwidth = 12000.0;
    
    // Nowe/Zmienione pola kontrolne
    float volume = 1.0f;     // Cyfrowa głośność
    bool isMuted = false;
    float rfGain = -1.0f;    // -1.0 oznacza AUTO, 0..50 manual
    
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

    // Pola Nagrywania
    bool isRecording = false;
    RecMode recMode = RecMode::AUDIO;
    std::string recPath = ""; 
    std::string recStatus = "Idle"; // Do wyświetlania nazwy pliku

    SharedData() : fftSpectrum(FFT_SIZE, -100.0), waterfallRow(SPEC_W * 4, 0) {}
};

std::mutex sourceMtx;
std::shared_ptr<IQSource> currentSource;

// ... Funkcje pomocnicze (getHeatmap, formatHz, drawGrid) bez zmian ...
// (Wklej tutaj funkcje getHeatmap, formatHz, drawGrid z poprzedniej wersji, aby skrócić kod)
sf::Color getHeatmap(float v) {
    v = std::clamp(v, 0.0f, 1.0f);
    std::uint8_t r=0,g=0,b=0;
    if(v<0.25f) b=static_cast<std::uint8_t>(v*4*255);
    else if(v<0.5f) {b=255; g=static_cast<std::uint8_t>((v-0.25f)*4*255);}
    else if(v<0.75f) {r=static_cast<std::uint8_t>((v-0.5f)*4*255); g=255; b=static_cast<std::uint8_t>(255-r);} 
    else {r=255; g=static_cast<std::uint8_t>((1.0f-v)*4*255);} 
    return {r,g,b};
}

std::string formatHz(long long hz) {
    std::stringstream ss;
    ss << std::fixed << std::setprecision(3) << (hz / 1000000.0) << " MHz";
    return ss.str();
}

void drawGrid(sf::RenderWindow& window, const sf::Font& font, float x, float y, float w, float h, long long cf, int sr, float minDb, float maxDb) {
    float dbStep = 20.0f;
    for (float db = 0; db >= -140; db -= dbStep) {
        if (db > maxDb || db < minDb) continue;
        float norm = (db - minDb) / (maxDb - minDb);
        float yPos = y + h - (norm * h);
        sf::RectangleShape line({w, 1}); line.setPosition({x, yPos}); line.setFillColor(sf::Color(100, 100, 100, 150)); window.draw(line);
        sf::Text l(font, std::to_string((int)db), 10); l.setPosition({x+2, yPos-12}); l.setFillColor(sf::Color::White); window.draw(l);
    }
    double startFreq = (double)cf - sr/2.0;
    for (int i = 0; i <= 8; i++) {
        float xPos = x + (w/8)*i;
        sf::RectangleShape line({1, h}); line.setPosition({xPos, y}); line.setFillColor(sf::Color(100, 100, 100, 150)); window.draw(line);
        double freqAtPoint = startFreq + (double)sr * ((double)i / 8.0);
        std::string freqStr = formatHz((long long)freqAtPoint);
        if(freqStr.size() > 4) freqStr = freqStr.substr(0, freqStr.size()-4); 
        sf::Text l(font, freqStr, 10);
        sf::FloatRect b = l.getLocalBounds();
        l.setPosition({xPos - b.size.x/2, y + h - 15}); l.setFillColor(sf::Color::White);
        window.draw(l);
    }
}

// --- DSP WORKER (Zmodyfikowany o nagrywanie i Gain) ---
void dspWorker(std::atomic<bool>& running, SharedData& shared, AudioSink& audio) {
    Demodulator demod(2000000, AUDIO_RATE); 
    double lastSampleRate = 0;
    std::vector<Complex> iqBuffer;
    std::vector<double> winFunc = makeWindow(FFT_SIZE);
    std::vector<double> localFftHistory(FFT_SIZE, -100.0);
    
    WavWriter recorder;
    float lastRfGain = -999.0f; // do wykrywania zmian

    while (running) {
        std::shared_ptr<IQSource> src = nullptr;
        { std::lock_guard<std::mutex> lock(sourceMtx); src = currentSource; }
        
        if (!src) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }

        double targetFreqPct, bw; 
        float vol, rfGainReq; bool muted;
        Mode mode; bool play; float minDb, maxDb;
        bool doRecord; RecMode rMode; std::string rPath;
        
        {
            std::lock_guard<std::mutex> lock(shared.mtx);
            targetFreqPct = shared.tunedFreqPercent; 
            bw = shared.bandwidth;
            vol = shared.volume; muted = shared.isMuted;
            rfGainReq = shared.rfGain;
            mode = shared.mode; play = shared.isPlaying;
            minDb = shared.minDb; maxDb = shared.maxDb;
            
            doRecord = shared.isRecording;
            rMode = shared.recMode;
            rPath = shared.recPath;
        }

        // OBSŁUGA RF GAIN (Sprzętowa)
        if (src->isHardware() && std::abs(rfGainReq - lastRfGain) > 0.1f) {
            // Jeśli -1 to Auto, w przeciwnym razie wartość dB
            src->setGain((int)rfGainReq); 
            lastRfGain = rfGainReq;
        }

        // OBSŁUGA NAGRYWANIA - Start/Stop
        if (doRecord && !recorder.active) {
             // Generowanie nazwy pliku
             char timeBuf[32]; std::time_t now = std::time(nullptr);
             std::strftime(timeBuf, sizeof(timeBuf), "%Y%m%d_%H%M%S", std::localtime(&now));
             
             std::string filename;
             if (rPath.empty()) filename = "rec_" + std::string(timeBuf);
             else filename = rPath + "/rec_" + std::string(timeBuf);

             if (rMode == RecMode::AUDIO) {
                 filename += "_audio.wav";
                 recorder.start(filename, (int)AUDIO_RATE, 1);
             } else {
                 filename += "_IQ.wav";
                 recorder.start(filename, (int)src->getSampleRate(), 2);
             }
             { std::lock_guard<std::mutex> l(shared.mtx); shared.recStatus = "REC: " + filename; }
        } else if (!doRecord && recorder.active) {
             recorder.stop();
             { std::lock_guard<std::mutex> l(shared.mtx); shared.recStatus = "Saved."; }
        }


        if (!play) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); continue; }
        
        if (!src->isHardware()) {
            while (audio.getBufferedCount() > (AUDIO_RATE * 0.2)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                if (!running) return;
            }
        }

        double sr = src->getSampleRate();
        if (sr != lastSampleRate) { demod = Demodulator(sr, AUDIO_RATE); lastSampleRate = sr; }

        int chunkSize = (int)sr / 60; 
        if (chunkSize > 200000) chunkSize = 200000;
        if (iqBuffer.size() != chunkSize) iqBuffer.resize(chunkSize);

        int readCount = src->read(iqBuffer.data(), chunkSize);

        if (readCount > 0) {
            // Nagrywanie Baseband (IQ)
            if (recorder.active && rMode == RecMode::BASEBAND) {
                // Konwersja IQ (Complex) na float array (L R L R)
                std::vector<float> rawFloat(readCount * 2);
                for(int i=0; i<readCount; i++) {
                    rawFloat[i*2] = (float)iqBuffer[i].real();
                    rawFloat[i*2+1] = (float)iqBuffer[i].imag();
                }
                recorder.write(rawFloat.data(), rawFloat.size());
            }

            double freqOffset = (targetFreqPct - 0.5) * sr;
            if (mode == Mode::USB) freqOffset += bw / 2.0;
            if (mode == Mode::LSB) freqOffset -= bw / 2.0;

            std::vector<Complex> chunkToProcess(iqBuffer.begin(), iqBuffer.begin() + readCount);
            auto audioData = demod.process(chunkToProcess, freqOffset, bw, mode);
            
            // Aplikacja GŁOŚNOŚCI (Volume)
            float finalVol = muted ? 0.0f : vol;
            for (auto& s : audioData) s *= finalVol;
            
            audio.pushSamples(audioData);

            // Nagrywanie Audio
            if (recorder.active && rMode == RecMode::AUDIO) {
                recorder.write(audioData.data(), audioData.size());
            }

            // FFT Processing (Standard)
            std::vector<Complex> fftData(FFT_SIZE);
            for (size_t i = 0; i < FFT_SIZE && i < chunkToProcess.size(); i++) fftData[i] = chunkToProcess[i] * winFunc[i];
            fft(fftData);
            std::vector<uint8_t> tempRow(SPEC_W * 4);
            for (int x = 0; x < SPEC_W; x++) {
                int fftIdx = (int)((float)x / SPEC_W * FFT_SIZE);
                int shiftedIdx = (fftIdx + FFT_SIZE / 2) % FFT_SIZE;
                float rawMag = std::abs(fftData[shiftedIdx]) / FFT_SIZE;
                float rawDb = 20 * std::log10(rawMag + 1e-12);
                float norm = (rawDb - minDb) / (maxDb - minDb); 
                sf::Color c = getHeatmap(norm);
                int px = x * 4; tempRow[px] = c.r; tempRow[px + 1] = c.g; tempRow[px + 2] = c.b; tempRow[px + 3] = 255;
            }
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
        } else { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    }
    if (recorder.active) recorder.stop();
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
    sf::Font font; if (!font.openFromFile("/System/Library/Fonts/Helvetica.ttc") && !font.openFromFile("C:/Windows/Fonts/arial.ttf") && !font.openFromFile("arial.ttf")) {}

    // === UI LAYOUT ===
    int px = 920; 
    sf::RectangleShape topBar({(float)W_WIDTH, (float)TOP_BAR_H});
    topBar.setFillColor(sf::Color(30, 30, 30)); topBar.setPosition({0, 0});
    
    FrequencyDisplay freqVFO(20, 8, font); freqVFO.setFrequency(100000000);
    bool stickyCenterMode = false; 
    SdrButton btnTuningMode(320, 10, 40, 40, "FIX", font); btnTuningMode.setColor(sf::Color(80, 80, 80));

    SdrButton btnPlay(370, 10, 40, 40, ">", font); btnPlay.setColor(sf::Color(116, 57, 57)); 
    bool isPlaying = false; 

    // --- NOWOŚĆ: VOLUME I MUTE W PRAWYM GÓRNYM ROGU ---
    SdrButton btnMute(1140, 10, 40, 40, "M", font); // Ikona Mute
    Slider volSlider(960, 25, 170, 0.0f, 1.0f, 0.5f, "Volume", font);
    bool isMuted = false;

    sf::Text labelSource(font, "Source:", 12); labelSource.setPosition({430, 10});
    Dropdown sourceDropdown(430, 25, 160, 25, font); sourceDropdown.setOptions({"File (WAV)", "RTL-SDR", "SDRPlay"});

    sf::Text labelRate(font, "File:", 12); labelRate.setPosition({600, 10});
    Dropdown rateDropdown(600, 25, 160, 25, font); rateDropdown.setOptions({"None"}); 

    sf::Text labelAudio(font, "Audio:", 12); labelAudio.setPosition({770, 10});
    Dropdown audioDropdown(770, 25, 160, 25, font); audioDropdown.setOptions(deviceNames);

    // --- SIDEBAR CONTROLS ---
    int sideY = TOP_BAR_H + 10; 
    int sliderY = sideY + 20; 

    // NOWOŚĆ: RF Gain Slider + Auto Checkbox
    Slider rfGainSlider(px, sliderY, 160, 0.0f, 50.0f, 0.0f, "RF Gain (dB)", font);
    SdrButton btnAgc(px + 170, sliderY - 5, 30, 30, "A", font); // Auto Gain Button
    btnAgc.setActive(true); // Domyślnie Auto
    bool agcEnabled = true;

    Slider bwSlider(px, sliderY+50, 200, 1000.0f, 220000.0f, 12000.0f, "Filter BW (Hz)", font);
    Slider minDbSlider(px, sliderY+100, 200, -120.0f, -20.0f, -90.0f, "Min dB", font);
    Slider maxDbSlider(px, sliderY+150, 200, -40.0f, 40.0f, 0.0f, "Max dB", font);

    int btnY = sliderY + 200;
    SdrButton btnNFM(px, btnY, 45, 30, "NFM", font);
    SdrButton btnAM(px + 50, btnY, 45, 30, "AM", font);
    SdrButton btnWFM(px + 100, btnY, 45, 30, "WFM", font);
    SdrButton btnOFF(px + 150, btnY, 45, 30, "OFF", font);
    SdrButton btnLSB(px, btnY+40, 95, 30, "LSB", font);
    SdrButton btnUSB(px + 100, btnY+40, 95, 30, "USB", font);
    btnNFM.setActive(true); 

    // --- NOWOŚĆ: PANEL NAGRYWANIA ---
    int recY = btnY + 90;
    sf::RectangleShape recPanel({260, 130});
    recPanel.setPosition({(float)px - 10, (float)recY});
    recPanel.setFillColor(sf::Color(40,40,40)); recPanel.setOutlineColor(sf::Color::White); recPanel.setOutlineThickness(1);

    sf::Text labelRec(font, "Recording", 14); labelRec.setPosition({(float)px, (float)recY + 5});
    
    SdrButton btnRecAudio(px, recY + 30, 80, 25, "Audio", font); btnRecAudio.setActive(true);
    SdrButton btnRecIQ(px + 90, recY + 30, 80, 25, "Baseband", font); 
    RecMode currentRecMode = RecMode::AUDIO;

    sf::Text pathText(font, "Path: ./ (Default)", 10); pathText.setPosition({(float)px, (float)recY + 65}); pathText.setFillColor(sf::Color::Cyan);
    SdrButton btnSelectFolder(px, recY + 85, 100, 25, "Set Folder", font);

    SdrButton btnRecStart(px + 120, recY + 80, 60, 35, "REC", font); /*btnRecStart.setColor(sf::Color(150,0,0));*/
    std::string currentRecPath = "";

    Slider timeSlider(20, W_HEIGHT - 30, W_WIDTH - 40, 0.0f, 1.0f, 0.0f, "Timeline", font);

    // Waterfall Textures
    std::vector<std::uint8_t> waterfall(SPEC_W * WATERFALL_H * 4, 0);
    sf::Texture wTex; if (!wTex.resize({(unsigned)SPEC_W, (unsigned)WATERFALL_H})) return 1;
    sf::Sprite wSpr(wTex); wSpr.setPosition({0, (float)SPEC_H + TOP_BAR_H}); 
    
    long long currentCenterFreq = 0;
    long long pendingCenterFreq = 0;
    sf::Clock debouncer;

    auto resetBtns = [&](SdrButton* active) {
        btnNFM.setActive(false); btnAM.setActive(false); btnWFM.setActive(false); 
        btnOFF.setActive(false); btnLSB.setActive(false); btnUSB.setActive(false);
        active->setActive(true);
    };

    auto changeSource = [&](int sourceIdx, int rateIdx, std::string pathOverride = "") {
        std::shared_ptr<IQSource> oldSource;
        { std::lock_guard<std::mutex> lock(sourceMtx); oldSource = currentSource; currentSource = nullptr; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (oldSource) { oldSource->stop(); oldSource->close(); }

        std::shared_ptr<IQSource> newSource;
        uint32_t targetRate = 0;
        if (sourceIdx == 1) targetRate = (rateIdx < RTL_RATES_VAL.size()) ? RTL_RATES_VAL[rateIdx] : 2048000;
        if (sourceIdx == 2) targetRate = (rateIdx < SDRPLAY_RATES_VAL.size()) ? SDRPLAY_RATES_VAL[rateIdx] : 2000000;

        if (sourceIdx == 0) { 
            newSource = std::make_shared<FileSource>(); 
            std::string path = pathOverride.empty() ? "None" : pathOverride;
            { 
                std::lock_guard<std::mutex> lock(sharedData.mtx);
                size_t lastSlash = path.find_last_of("/\\");
                sharedData.currentFilename = (lastSlash == std::string::npos) ? path : path.substr(lastSlash + 1);
                // Parse Frequency logic here (omitted for brevity)
            }
            newSource->open(path);
            rateDropdown.setOptions({sharedData.currentFilename});
        } else if (sourceIdx == 1) { 
            newSource = std::make_shared<RtlSdrSource>();
            rateDropdown.setOptions(newSource->getAvailableSampleRatesText()); rateDropdown.setSelection(rateIdx);
            if (!newSource->open("0", targetRate)) { sourceDropdown.selectedIndex = 0; sourceDropdown.selectedText.setString("File"); newSource = std::make_shared<FileSource>(); newSource->open("None"); } 
            else { newSource->setCenterFrequency(freqVFO.getFrequency()); currentCenterFreq = freqVFO.getFrequency(); }
        } else { 
            newSource = std::make_shared<SdrPlaySource>();
            rateDropdown.setOptions(newSource->getAvailableSampleRatesText()); rateDropdown.setSelection(rateIdx);
            if (newSource->open("", targetRate)) { newSource->setCenterFrequency(freqVFO.getFrequency()); currentCenterFreq = freqVFO.getFrequency(); }
        }

        { std::lock_guard<std::mutex> lock(sourceMtx); currentSource = newSource; }
        { std::lock_guard<std::mutex> lock(sharedData.mtx); sharedData.isPlaying = false; sharedData.isRecording = false; }
        isPlaying = false; btnRecStart.setText("REC"); //btnRecStart.setColor(sf::Color(150,0,0));
        audio.stop(); btnPlay.setText(">"); btnPlay.setColor(sf::Color(116, 57, 57)); audio.clear(); std::fill(waterfall.begin(), waterfall.end(), 0);
    };

    // === MAIN LOOP ===
    while (window.isOpen()) {
        // Sync UI -> Shared Data
        {
             std::lock_guard<std::mutex> lock(sharedData.mtx);
             sharedData.bandwidth = bwSlider.currentVal;
             sharedData.minDb = minDbSlider.currentVal;
             sharedData.maxDb = maxDbSlider.currentVal;
             sharedData.volume = volSlider.currentVal;
             sharedData.rfGain = agcEnabled ? -1.0f : rfGainSlider.currentVal;
             sharedData.isMuted = isMuted;
             sharedData.recMode = currentRecMode;
             sharedData.recPath = currentRecPath;
             
             // Update status text from recording state
             if (sharedData.isRecording) {
                 pathText.setString(sharedData.recStatus);
                 pathText.setFillColor(sf::Color::Red);
             } else if (sharedData.recStatus != "Idle") {
                 pathText.setString(sharedData.recStatus);
                 pathText.setFillColor(sf::Color::Green);
             }
        }
        
        bool isHw = false; double hwSampleRate = 2000000.0;
        { std::lock_guard<std::mutex> l(sourceMtx); if (currentSource) { isHw = currentSource->isHardware(); hwSampleRate = currentSource->getSampleRate(); } }

        if (isHw) { labelRate.setString("Rate:"); freqVFO.setEnabled(true); } 
        else { labelRate.setString("File:"); freqVFO.setEnabled(false); }

        if (!sourceDropdown.isOpen && !rateDropdown.isOpen && !audioDropdown.isOpen) freqVFO.update(window);

        while (const std::optional<sf::Event> ev = window.pollEvent()) {
            if (ev->is<sf::Event::Closed>()) window.close();
            if (sourceDropdown.handleEvent(*ev, window)) changeSource(sourceDropdown.selectedIndex, 0);
            
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
                if (rateDropdown.handleEvent(*ev, window)) changeSource(sourceDropdown.selectedIndex, rateDropdown.selectedIndex);
            }
            if (audioDropdown.handleEvent(*ev, window)) { audio.stop(); audio.initDevice(audioDropdown.selectedIndex, (int)AUDIO_RATE); if (isPlaying) audio.start(); }


            if (!sourceDropdown.isOpen && !rateDropdown.isOpen && !audioDropdown.isOpen) {
                if (isHw && freqVFO.handleEvent(*ev)) { 
                        long long targetVFO = freqVFO.getFrequency();
                        if (stickyCenterMode) {
                            pendingCenterFreq = targetVFO; debouncer.restart();
                            { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.tunedFreqPercent = 0.5; }
                        } else {
                            double halfBW = hwSampleRate / 2.0;
                            double minF = (double)currentCenterFreq - halfBW; double maxF = (double)currentCenterFreq + halfBW;
                            if (targetVFO > maxF || targetVFO < minF) { pendingCenterFreq = targetVFO; debouncer.restart(); { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.tunedFreqPercent = 0.5; } } 
                            else { double pct = 0.5 + ((double)(targetVFO - currentCenterFreq) / hwSampleRate); { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.tunedFreqPercent = pct; } }
                        }
                }
                if (isHw && btnTuningMode.isClicked(*ev, window)) {
                    stickyCenterMode = !stickyCenterMode;
                    if (stickyCenterMode) { btnTuningMode.setText("CTR"); btnTuningMode.setColor(sf::Color(0, 100, 200)); pendingCenterFreq = freqVFO.getFrequency(); debouncer.restart(); { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.tunedFreqPercent = 0.5; } } 
                    else { btnTuningMode.setText("FIX"); btnTuningMode.setColor(sf::Color(80, 80, 80)); }
                }

                // SLIDERS
                volSlider.handleEvent(*ev, window);
                rfGainSlider.handleEvent(*ev, window);
                bwSlider.handleEvent(*ev, window); minDbSlider.handleEvent(*ev, window); maxDbSlider.handleEvent(*ev, window);
                if (!isHw) timeSlider.handleEvent(*ev, window);

                // MUTE BTN
                if (btnMute.isClicked(*ev, window)) {
                    isMuted = !isMuted;
                    btnMute.setColor(isMuted ? sf::Color(116, 57, 57) : sf::Color(60, 60, 60));
                }

                // AGC BTN
                if (btnAgc.isClicked(*ev, window)) {
                    agcEnabled = !agcEnabled;
                    btnAgc.setActive(agcEnabled);
                }

                // REC CONTROLS
                if (btnRecAudio.isClicked(*ev, window)) { currentRecMode = RecMode::AUDIO; btnRecAudio.setActive(true); btnRecIQ.setActive(false); }
                if (btnRecIQ.isClicked(*ev, window)) { currentRecMode = RecMode::BASEBAND; btnRecAudio.setActive(false); btnRecIQ.setActive(true); }
                
                if (btnSelectFolder.isClicked(*ev, window)) {
                    std::string folder = selectFolderDialog();
                    if (!folder.empty()) {
                        currentRecPath = folder;
                        std::string shortPath = folder;
                        if (shortPath.length() > 25) shortPath = "..." + shortPath.substr(shortPath.length() - 22);
                        pathText.setString("Path: " + shortPath);
                        pathText.setFillColor(sf::Color::Cyan);
                    }
                }

                if (btnRecStart.isClicked(*ev, window)) {
                    bool s; { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.isRecording = !sharedData.isRecording; s = sharedData.isRecording; }
                    if (s) { btnRecStart.setText("STOP"); /*btnRecStart.setColor(sf::Color(78,78,236));*/ }
                    else { btnRecStart.setText("REC"); /*btnRecStart.setColor(sf::Color(150,0,0));*/ }
                }

                // MODES
                if (btnNFM.isClicked(*ev, window)) { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.mode = Mode::NFM; resetBtns(&btnNFM); bwSlider.currentVal = 12000; bwSlider.updateHandlePos(); }
                if (btnAM.isClicked(*ev, window))  { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.mode = Mode::AM; resetBtns(&btnAM); bwSlider.currentVal = 8000; bwSlider.updateHandlePos(); }
                if (btnWFM.isClicked(*ev, window)) { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.mode = Mode::WFM; resetBtns(&btnWFM); bwSlider.currentVal = 180000; bwSlider.updateHandlePos(); }
                if (btnOFF.isClicked(*ev, window)) { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.mode = Mode::OFF; resetBtns(&btnOFF); }
                if (btnLSB.isClicked(*ev, window)) { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.mode = Mode::LSB; resetBtns(&btnLSB); bwSlider.currentVal = 3000; bwSlider.updateHandlePos(); }
                if (btnUSB.isClicked(*ev, window)) { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.mode = Mode::USB; resetBtns(&btnUSB); bwSlider.currentVal = 3000; bwSlider.updateHandlePos(); }

                if (btnPlay.isClicked(*ev, window)) {
                    bool s; { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.isPlaying = !sharedData.isPlaying; s = sharedData.isPlaying; }
                    if (s) { btnPlay.setText("||"); btnPlay.setColor(sf::Color(78, 78, 236)); audio.start(); { std::lock_guard<std::mutex> l(sourceMtx); if (currentSource) currentSource->start(); } } 
                    else { btnPlay.setText(">"); btnPlay.setColor(sf::Color(116, 57, 57)); audio.stop(); { std::lock_guard<std::mutex> l(sourceMtx); if (currentSource) currentSource->stop(); } }
                    isPlaying = s;
                }

                if (const auto* mb = ev->getIf<sf::Event::MouseButtonPressed>()) {
                    if (mb->button == sf::Mouse::Button::Left) {
                        sf::Vector2f m = window.mapPixelToCoords(sf::Mouse::getPosition(window));
                        float graphY = m.y - TOP_BAR_H;
                        if (m.x < SPEC_W && graphY >= 0 && graphY < (SPEC_H + WATERFALL_H)) { 
                            double clickPct = m.x / SPEC_W;
                            double offsetHz = (clickPct - 0.5) * hwSampleRate;
                            long long clickedFreq = currentCenterFreq + (long long)offsetHz;
                            if (stickyCenterMode && isHw) { pendingCenterFreq = clickedFreq; debouncer.restart(); { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.tunedFreqPercent = 0.5; } freqVFO.setFrequency(clickedFreq); } 
                            else { { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.tunedFreqPercent = clickPct; } freqVFO.setFrequency(clickedFreq); }
                        }
                    }
                }
                if (const auto* me = ev->getIf<sf::Event::MouseMoved>()) {
                    sf::Vector2f m = window.mapPixelToCoords(me->position);
                    float graphY = m.y - TOP_BAR_H;
                    if (m.x >= 0 && m.x < SPEC_W && graphY >= 0 && graphY < SPEC_H + WATERFALL_H) { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.mouseX_spectrum = m.x; sharedData.mouseY_spectrum = graphY; } 
                    else { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.mouseX_spectrum = -1.0f; }
                }
            }
        }
        
        // --- CURSORS ---
        {
            bool hover = false;
            sf::Vector2f m = window.mapPixelToCoords(sf::Mouse::getPosition(window));
            if (btnPlay.shape.getGlobalBounds().contains(m)) hover = true;
            if (freqVFO.enabled && freqVFO.isHovered) hover = true;
            if (isHw && btnTuningMode.shape.getGlobalBounds().contains(m)) hover = true;
            if (!isHw && rateDropdown.mainBox.getGlobalBounds().contains(m)) hover = true; 
            if (btnMute.shape.getGlobalBounds().contains(m)) hover = true;
            if (btnRecAudio.shape.getGlobalBounds().contains(m)) hover = true;
            if (btnRecIQ.shape.getGlobalBounds().contains(m)) hover = true;
            if (btnSelectFolder.shape.getGlobalBounds().contains(m)) hover = true;
            if (btnRecStart.shape.getGlobalBounds().contains(m)) hover = true;
            
            if (volSlider.track.getGlobalBounds().contains(m)) hover = true;
            if (rfGainSlider.track.getGlobalBounds().contains(m)) hover = true;

            if (hover) { if (cursorHand) window.setMouseCursor(*cursorHand); } else { if (cursorArrow) window.setMouseCursor(*cursorArrow); }
        }

        if (pendingCenterFreq != 0 && debouncer.getElapsedTime().asMilliseconds() > 150) {
             std::lock_guard<std::mutex> l(sourceMtx);
             if (currentSource && currentSource->isHardware()) { currentSource->setCenterFrequency(pendingCenterFreq); currentCenterFreq = pendingCenterFreq; }
             pendingCenterFreq = 0;
        }

        if (!sourceDropdown.isOpen) {
             volSlider.update(window);
             rfGainSlider.update(window);
             bwSlider.update(window); minDbSlider.update(window); maxDbSlider.update(window);
             bool isHw = false; double prog = 0; { std::lock_guard<std::mutex> l(sourceMtx); if (currentSource) { isHw = currentSource->isHardware(); prog = currentSource->getProgress(); } }
             if (!isHw) { timeSlider.update(window); if (timeSlider.isDragging) { std::lock_guard<std::mutex> l(sourceMtx); if (currentSource) currentSource->seek(timeSlider.currentVal); } else { timeSlider.currentVal = prog; timeSlider.updateHandlePos(); } }
        }

        std::vector<double> spectrum; std::vector<uint8_t> row; bool newRow = false; double tunePct = 0.5; Mode mode = Mode::NFM;
        { std::lock_guard<std::mutex> lock(sharedData.mtx); spectrum = sharedData.fftSpectrum; if (sharedData.newWaterfallData) { row = sharedData.waterfallRow; sharedData.newWaterfallData = false; newRow = true; } tunePct = sharedData.tunedFreqPercent; mode = sharedData.mode; }

        if (newRow) { std::copy_backward(waterfall.begin(), waterfall.end() - SPEC_W * 4, waterfall.end()); std::copy(row.begin(), row.end(), waterfall.begin()); wTex.update(waterfall.data()); }

        window.clear(sf::Color::Black);
        long long cf = 0; double sr = 2e6; { std::lock_guard<std::mutex> l(sourceMtx); if (currentSource) { sr = currentSource->getSampleRate(); } } if (currentSource) cf = currentCenterFreq;

        window.draw(topBar);
        window.draw(labelSource); sourceDropdown.draw(window);
        window.draw(labelRate); rateDropdown.draw(window);
        window.draw(labelAudio); audioDropdown.draw(window);

        // Rysowanie Volume i Mute
        btnMute.draw(window); volSlider.draw(window);

        drawGrid(window, font, 0, TOP_BAR_H, SPEC_W, SPEC_H, cf, sr, minDbSlider.currentVal, maxDbSlider.currentVal);

        sf::VertexArray lines(sf::PrimitiveType::Lines, spectrum.size());
        for (size_t i = 0; i < spectrum.size(); i++) { float norm = (spectrum[i] - minDbSlider.currentVal) / (maxDbSlider.currentVal - minDbSlider.currentVal); float y = SPEC_H - (norm * SPEC_H); if (y < 0) y = 0; if (y > SPEC_H) y = SPEC_H; lines[i].position = { (float)i / spectrum.size() * SPEC_W, y + TOP_BAR_H }; lines[i].color = sf::Color::Cyan; }
        window.draw(lines); window.draw(wSpr); 

        float mouseX = -1.0f; float mouseY = -1.0f; { std::lock_guard<std::mutex> lock(sharedData.mtx); mouseX = sharedData.mouseX_spectrum; mouseY = sharedData.mouseY_spectrum; }
        if (mouseX != -1.0f) { sf::Color guideColor(100, 100, 100); sf::VertexArray lineFFT(sf::PrimitiveType::Lines, 2); lineFFT[0].position = {mouseX, (float)TOP_BAR_H}; lineFFT[0].color = guideColor; lineFFT[1].position = {mouseX, (float)SPEC_H + TOP_BAR_H}; lineFFT[1].color = guideColor; window.draw(lineFFT); if (mouseY > (float)SPEC_H) { sf::VertexArray lineWaterfall(sf::PrimitiveType::Lines, 2); lineWaterfall[0].position = {mouseX, (float)SPEC_H + TOP_BAR_H}; lineWaterfall[0].color = guideColor; lineWaterfall[1].position = {mouseX, (float)(SPEC_H + WATERFALL_H + TOP_BAR_H)}; lineWaterfall[1].color = guideColor; window.draw(lineWaterfall); } }

        sf::RectangleShape tunerRect; float bwPixels = (bwSlider.currentVal / sr) * SPEC_W; if (bwPixels < 2.0f) bwPixels = 2.0f;
        float rectX = tunePct * SPEC_W - bwPixels / 2.0f; if (mode == Mode::USB) rectX += bwPixels / 2.0f; if (mode == Mode::LSB) rectX -= bwPixels / 2.0f;
        tunerRect.setSize({bwPixels, (float)SPEC_H}); tunerRect.setPosition({rectX, (float)TOP_BAR_H});
        tunerRect.setFillColor(mode == Mode::OFF ? sf::Color(50, 50, 50, 40) : sf::Color(200, 200, 200, 50)); tunerRect.setOutlineThickness(0); window.draw(tunerRect);

        sf::VertexArray centerLine(sf::PrimitiveType::Lines, 2); float centerX = tunePct * SPEC_W;
        centerLine[0].position = {centerX, (float)TOP_BAR_H}; centerLine[0].color = sf::Color::Red; centerLine[1].position = {centerX, (float)SPEC_H + TOP_BAR_H}; centerLine[1].color = sf::Color::Red; window.draw(centerLine);

        if (!isHw) { timeSlider.draw(window); } else { btnTuningMode.draw(window); }
        
        freqVFO.draw(window); btnPlay.draw(window);

        // Draw Controls
        rfGainSlider.draw(window); btnAgc.draw(window);

        bwSlider.draw(window); minDbSlider.draw(window); maxDbSlider.draw(window);
        btnNFM.draw(window); btnAM.draw(window); btnWFM.draw(window); btnOFF.draw(window); btnLSB.draw(window); btnUSB.draw(window);

        // Draw Recording Panel
        window.draw(recPanel);
        window.draw(labelRec);
        btnRecAudio.draw(window); btnRecIQ.draw(window);
        window.draw(pathText);
        btnSelectFolder.draw(window);
        btnRecStart.draw(window);

        if (audioDropdown.isOpen) audioDropdown.draw(window);
        if (rateDropdown.isOpen) rateDropdown.draw(window);
        if (sourceDropdown.isOpen) sourceDropdown.draw(window);

        window.display();
    }
    dspRunning = false; if (dspThread.joinable()) dspThread.join();
    return 0;
}