#define MINIAUDIO_IMPLEMENTATION
#include <SFML/Graphics.hpp>
#include <SFML/Window/Clipboard.hpp> 
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
#include <deque>
#include <regex> 

#include "DSP.h"
#include "AudioSink.h"
#include "Demodulator.h"
#include "UI.h"
#include "Sidebar.h" 
#include "NativeDialogs.h"
#include "IQSources.h"
#include "APRS_Decoder.h"

// --- CONSTANTS ---
const int FFT_SIZE = 1024;
const int INTERNAL_WATERFALL_WIDTH = 1024;
const double AUDIO_RATE = 48000.0;
const int TOP_BAR_H = 60; 
const int SIDEBAR_W = 320;
const int TUNING_LATENCY_MS = 50; 

const std::vector<uint32_t> RTL_RATES_VAL = {1400000, 1800000, 2048000, 2400000, 3200000};
const std::vector<uint32_t> SDRPLAY_RATES_VAL = {2000000, 4000000, 6000000, 8000000, 10000000};
const std::vector<std::string> STEP_NAMES = {"None", "1 kHz", "5 kHz", "6.25k", "10 kHz", "12.5k", "25 kHz", "100 kHz"};
const std::vector<long long> STEP_VALUES = {0, 1000, 5000, 6250, 10000, 12500, 25000, 100000};

// Themes list
const std::vector<std::string> THEME_NAMES = {"Orbit Original", "Magma (Neon)", "Matrix (Green)", "Grayscale", "Orbit Plus (Aurora)"};

std::string getTimestamp() {
    auto now = std::time(nullptr); auto tm = *std::localtime(&now);
    std::ostringstream oss; oss << std::put_time(&tm, "[%Y-%m-%d %H:%M:%S]"); return oss.str();
}

std::deque<std::string> loadLastLogLines(const std::string& filename, int count) {
    std::deque<std::string> lines; std::ifstream file(filename); if (!file.is_open()) return lines;
    std::string line; while (std::getline(file, line)) { if (!line.empty()) { lines.push_back(line); if (lines.size() > count) lines.pop_front(); } } return lines;
}

std::string wrapText(const std::string& str, const sf::Font& font, unsigned int charSize, float maxWidth) {
    std::string result; std::string currentLine; std::stringstream ss(str); std::string word;
    while (ss >> word) {
        sf::Text testWord(font, word, charSize);
        if (testWord.getLocalBounds().size.x > maxWidth) {
            if (!currentLine.empty()) { result += currentLine + "\n"; currentLine = ""; }
            std::string part; for (char c : word) { part += c; sf::Text partTest(font, part, charSize); if (partTest.getLocalBounds().size.x > maxWidth - 10) { result += part + "\n"; part = ""; } } currentLine = part; continue;
        }
        std::string testLine = currentLine + (currentLine.empty() ? "" : " ") + word; sf::Text testText(font, testLine, charSize);
        if (testText.getLocalBounds().size.x > maxWidth) { if (!currentLine.empty()) { result += currentLine + "\n"; currentLine = word; } else { result += word + "\n"; } } else { currentLine = testLine; }
    } result += currentLine; return result;
}

struct WavWriter {
    std::ofstream file; uint32_t dataSize = 0; uint32_t sampleRate = 0; uint16_t channels = 0; bool active = false;
    void start(std::string path, uint32_t sr, uint16_t ch) { if (active) stop(); file.open(path, std::ios::binary); if (!file.is_open()) return; sampleRate = sr; channels = ch; dataSize = 0; active = true; char header[44] = {0}; file.write(header, 44); }
    void write(const float* data, size_t count) { if (!active) return; for(size_t i=0; i<count; i++) { float s = std::clamp(data[i], -1.0f, 1.0f); int16_t val = static_cast<int16_t>(s * 32767.0f); file.write((char*)&val, sizeof(int16_t)); } dataSize += count * sizeof(int16_t); }
    void stop() { if (!active || !file.is_open()) return; file.seekp(0); uint32_t fileSize = dataSize + 36; uint32_t byteRate = sampleRate * channels * 2; uint16_t blockAlign = channels * 2; file.write("RIFF", 4); file.write((char*)&fileSize, 4); file.write("WAVE", 4); file.write("fmt ", 4); uint32_t s1 = 16; uint16_t af = 1; uint16_t bps = 16; file.write((char*)&s1, 4); file.write((char*)&af, 2); file.write((char*)&channels, 2); file.write((char*)&sampleRate, 4); file.write((char*)&byteRate, 4); file.write((char*)&blockAlign, 2); file.write((char*)&bps, 2); file.write("data", 4); file.write((char*)&dataSize, 4); file.close(); active = false; }
};

enum class RecMode { AUDIO, BASEBAND };
struct AprsLastPacket { std::string raw, src, dest, comment, coords; float course = -1.0f, speed = -1.0f; };
struct SharedData { 
    std::mutex mtx; 
    double tunedFreqPercent = 0.5; double pendingSeekRequest = -1.0; double bandwidth = 12500.0; long long centerFreq = 0; 
    float volume = 0.5f; bool isMuted = false; 
    float rfGain = -1.0f; // -1.0 = AGC, >=0 = Manual
    float squelchThreshold = -100.0f; bool stereoEnabled = false; Mode mode = Mode::NFM; bool isPlaying = false; 
    float minDb = -90.0f; 
    float maxDb = 0.0f; 
    std::vector<double> fftSpectrum; std::vector<uint8_t> waterfallRow; bool newWaterfallData = false; std::string currentFilename = "None"; float mouseX_spectrum = -1.0f; float mouseY_spectrum = -1.0f; bool isRecording = false; RecMode recMode = RecMode::AUDIO; std::string recPath = ""; std::string recStatus = "Idle"; bool aprsEnabled = false; std::deque<std::string> aprsLog; AprsLastPacket lastAprs; 
    float zoomLevel = 1.0f; 
    
    // --- DEFAULT THEME: 4 = Orbit Plus (Aurora) ---
    int waterfallTheme = 4; 
    
    SharedData() : fftSpectrum(FFT_SIZE, -100.0), waterfallRow(INTERNAL_WATERFALL_WIDTH * 4, 0) {} 
};

std::mutex sourceMtx; std::shared_ptr<IQSource> currentSource;

// --- COLOR THEMES ---
sf::Color getHeatmap(float v, int theme) { 
    v = std::clamp(v, 0.0f, 1.0f); 
    std::uint8_t r=0,g=0,b=0; 

    if (theme == 0 || theme == 4) { // Orbit Original AND Orbit Plus (Shared Palette)
        // Original Classic: Deep Blue -> Cyan -> White/Yellowish
        if(v<0.25f) b=static_cast<std::uint8_t>(v*4*255);
        else if(v<0.5f) {b=255; g=static_cast<std::uint8_t>((v-0.25f)*4*255);}
        else if(v<0.75f) {r=static_cast<std::uint8_t>((v-0.5f)*4*255); g=255; b=static_cast<std::uint8_t>(255-r);}
        else {r=255; g=static_cast<std::uint8_t>((1.0f-v)*4*255);}
    }
    else if (theme == 1) { // Magma
        if(v < 0.33f) { r=0; g=0; b=static_cast<std::uint8_t>(v*3*255); }
        else if(v < 0.66f) { r=static_cast<std::uint8_t>((v-0.33f)*3*255); g=0; b=255; }
        else { r=255; g=static_cast<std::uint8_t>((v-0.66f)*3*255); b=static_cast<std::uint8_t>(255 - (v-0.66f)*3*255); }
    }
    else if (theme == 2) { // Matrix
        r = 0;
        if(v < 0.5f) { g = static_cast<std::uint8_t>(v * 2 * 200); b=0; }
        else { g = 200 + static_cast<std::uint8_t>((v-0.5f)*2*55); b = static_cast<std::uint8_t>((v-0.5f)*2*255); r = b; }
    }
    else if (theme == 3) { // Grayscale
        std::uint8_t lum = static_cast<std::uint8_t>(v * 255);
        r = lum; g = lum; b = lum;
    }
    return {r,g,b}; 
}

std::string formatHz(long long hz) { std::stringstream ss; ss << std::fixed << std::setprecision(3) << (hz / 1000000.0) << " MHz"; return ss.str(); }

void drawGrid(sf::RenderWindow& window, const sf::Font& font, float x, float y, float w, float h, long long cf, double visibleSpan, float minDb, float maxDb) {
    float dbStep = 20.0f; 
    for (float db = 0; db >= -140; db -= dbStep) { 
        if (db > maxDb || db < minDb) continue; 
        float norm = (db - minDb) / (maxDb - minDb); 
        float yPos = y + h - (norm * h); 
        sf::RectangleShape line({w, 1}); line.setPosition({x, yPos}); 
        line.setFillColor(sf::Color(80, 80, 80, 100)); 
        window.draw(line); 
        sf::Text l(font, std::to_string((int)db), 10); l.setPosition({x+2, yPos-12}); 
        l.setFillColor(sf::Color(200, 200, 200, 150)); 
        window.draw(l); 
    }

    double startFreq = (double)cf - visibleSpan / 2.0;
    double endFreq   = (double)cf + visibleSpan / 2.0;

    double stepHz = 200000.0; 
    if (visibleSpan < 1000000) stepHz = 100000.0; 
    if (visibleSpan < 500000) stepHz = 50000.0;   
    if (visibleSpan < 200000) stepHz = 25000.0;   
    if (visibleSpan > 5000000) stepHz = 500000.0; 
    if (visibleSpan > 10000000) stepHz = 1000000.0;

    long long firstLineFreq = (long long)(ceil(startFreq / stepHz) * stepHz);

    for (double f = (double)firstLineFreq; f < endFreq; f += stepHz) {
        float normPos = (float)((f - startFreq) / visibleSpan);
        float xPos = x + normPos * w;

        sf::RectangleShape line({1, h}); 
        line.setPosition({xPos, y}); 
        line.setFillColor(sf::Color(80, 80, 80, 100)); 
        window.draw(line);

        std::string freqStr = formatHz((long long)f); 
        if(freqStr.size() > 4) freqStr = freqStr.substr(0, freqStr.size()-4);
        
        sf::Text l(font, freqStr, 10); 
        sf::FloatRect b = l.getLocalBounds(); 
        l.setPosition({xPos - b.size.x/2, y + h - 15}); 
        l.setFillColor(sf::Color(220, 220, 220, 200)); 
        window.draw(l);
    }
}

void parseAprsData(std::string raw, AprsLastPacket& pkt) {
    pkt.raw = raw; std::string content = raw; if (raw.size() > 0 && raw[0] == '[') { size_t cb = raw.find("] "); if (cb != std::string::npos) content = raw.substr(cb + 2); }
    size_t colon = content.find(':'); if (colon == std::string::npos) return; std::string header = content.substr(0, colon); std::string body = content.substr(colon + 1);
    size_t arrow = header.find('>'); if (arrow != std::string::npos) { pkt.src = header.substr(0, arrow); pkt.dest = header.substr(arrow + 1); } else { pkt.src = header; pkt.dest = "?"; }
    pkt.comment = body; std::regex coordRegex(R"((\d{4}\.\d{2})([NS]).(\d{5}\.\d{2})([EW]))"); std::smatch match; if (std::regex_search(body, match, coordRegex)) { float latVal = std::stof(match[1]) / 100.0f; float lonVal = std::stof(match[3]) / 100.0f; std::stringstream ss; ss << std::fixed << std::setprecision(2) << latVal << " " << match[2].str() << ", " << lonVal << " " << match[4].str(); pkt.coords = ss.str(); } else { pkt.coords = ""; }
    std::regex courseRegex(R"((\d{3})/(\d{3}))"); if (std::regex_search(body, match, courseRegex)) { try { pkt.course = std::stof(match[1]); pkt.speed = std::stof(match[2]); } catch(...) { pkt.course = -1; } } else { pkt.course = -1; }
}

void dspWorker(std::atomic<bool>& running, SharedData& shared, AudioSink& audio) {
    Demodulator demod(2000000, AUDIO_RATE); APRSDecoder aprsDecoder(AUDIO_RATE);
    aprsDecoder.onMessage = [&](std::string msg) { std::string ts = getTimestamp(); std::string fullLog = ts + " " + msg; std::ofstream logFile("aprs.log", std::ios::app); if (logFile.is_open()) { logFile << fullLog << "\n"; logFile.close(); } std::lock_guard<std::mutex> l(shared.mtx); shared.aprsLog.push_back(fullLog); if (shared.aprsLog.size() > 500) shared.aprsLog.pop_front(); parseAprsData(fullLog, shared.lastAprs); };
    double lastSampleRate = 0; std::vector<Complex> iqBuffer; std::vector<double> winFunc = makeWindow(FFT_SIZE); std::vector<double> localFftHistory(FFT_SIZE, -100.0); WavWriter recorder; 
    float lastRfGain = -9999.0f;

    while (running) {
        std::shared_ptr<IQSource> src = nullptr; { std::lock_guard<std::mutex> lock(sourceMtx); src = currentSource; }
        if (!src) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }
        double targetFreqPct, bw; float vol, rfGainReq; bool muted; Mode mode; bool play, aprsActive; float minDb, maxDb; bool doRecord; RecMode rMode; std::string rPath; float sqThr; bool stereo; double seekReq = -1.0; int themeID = 0;
        { std::lock_guard<std::mutex> lock(shared.mtx); seekReq = shared.pendingSeekRequest; shared.pendingSeekRequest = -1.0; float rawPct = shared.tunedFreqPercent; if (std::isnan(rawPct) || std::isinf(rawPct)) rawPct = 0.5f; targetFreqPct = std::clamp(rawPct, 0.0f, 1.0f); bw = shared.bandwidth; vol = shared.volume; muted = shared.isMuted; rfGainReq = shared.rfGain; mode = shared.mode; play = shared.isPlaying; minDb = shared.minDb; maxDb = shared.maxDb; doRecord = shared.isRecording; rMode = shared.recMode; rPath = shared.recPath; aprsActive = shared.aprsEnabled; sqThr = shared.squelchThreshold; stereo = shared.stereoEnabled; themeID = shared.waterfallTheme; }
        bool justSeeked = false; if (seekReq >= 0.0 && src->isSeekable()) { src->seek(seekReq); demod.clear(); audio.clear(); justSeeked = true; } if (!play && !justSeeked) { std::this_thread::sleep_for(std::chrono::milliseconds(50)); continue; }
        
        if (src->isHardware()) {
            if (std::abs(rfGainReq - lastRfGain) > 0.1f) {
                src->setGain((int)rfGainReq); lastRfGain = rfGainReq; 
            }
        }

        if (doRecord && !recorder.active) { long long currentCenterHz; { std::lock_guard<std::mutex> l(shared.mtx); currentCenterHz = shared.centerFreq; } char timeBuf[32]; std::time_t now = std::time(nullptr); std::strftime(timeBuf, sizeof(timeBuf), "%Y%m%d_%H%M%S", std::localtime(&now)); std::string filename, freqLabel; if (rMode == RecMode::AUDIO) { double offset = (targetFreqPct - 0.5) * src->getSampleRate(); long long tunedHz = currentCenterHz + (long long)offset; freqLabel = "_" + std::to_string(tunedHz / 1000) + "kHz"; filename = (rPath.empty() ? "." : rPath) + "/rec_" + std::string(timeBuf) + freqLabel + "_audio.wav"; recorder.start(filename, (int)AUDIO_RATE, 2); } else { freqLabel = "_" + std::to_string(currentCenterHz) + "Hz"; filename = (rPath.empty() ? "." : rPath) + "/rec_" + std::string(timeBuf) + freqLabel + "_IQ.wav"; recorder.start(filename, (int)src->getSampleRate(), 2); } { std::lock_guard<std::mutex> l(shared.mtx); shared.recStatus = "REC: " + filename; } } else if (!doRecord && recorder.active) { recorder.stop(); { std::lock_guard<std::mutex> l(shared.mtx); shared.recStatus = "Saved."; } }
        if (play && !src->isHardware() && !justSeeked) { size_t safeLevel = 9600; while (audio.getBufferedCount() > safeLevel) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); if (!running) return; } }
        double sr = src->getSampleRate(); if (sr != lastSampleRate) { demod = Demodulator(sr, AUDIO_RATE); lastSampleRate = sr; }
        int chunkSize = (int)sr / 60; if (!play && justSeeked) chunkSize = FFT_SIZE * 2; if (chunkSize > 200000) chunkSize = 200000; if (iqBuffer.size() != chunkSize) iqBuffer.resize(chunkSize);
        int readCount = src->read(iqBuffer.data(), chunkSize);
        if (readCount > 0) {
            if (recorder.active && rMode == RecMode::BASEBAND) { std::vector<float> rawFloat(readCount * 2); for(int i=0; i<readCount; i++) { rawFloat[i*2] = (float)iqBuffer[i].real(); rawFloat[i*2+1] = (float)iqBuffer[i].imag(); } recorder.write(rawFloat.data(), rawFloat.size()); }
            std::vector<Complex> fftData(FFT_SIZE); for (size_t i = 0; i < FFT_SIZE && i < readCount; i++) fftData[i] = iqBuffer[i] * winFunc[i]; fft(fftData);
            int visualBin = (int)(targetFreqPct * FFT_SIZE); if (visualBin < 0) visualBin = 0; if (visualBin >= FFT_SIZE) visualBin = FFT_SIZE - 1;
            int shiftedIdx = (visualBin + FFT_SIZE / 2) % FFT_SIZE; float signalMag = std::abs(fftData[shiftedIdx]) / FFT_SIZE; int left = (shiftedIdx - 1 + FFT_SIZE) % FFT_SIZE; int right = (shiftedIdx + 1) % FFT_SIZE; signalMag += (std::abs(fftData[left]) + std::abs(fftData[right])) / FFT_SIZE * 0.5f; float rssiDb = 20.0f * std::log10(signalMag + 1e-12f);
            if (play) { double freqOffset = (targetFreqPct - 0.5) * sr; if (mode == Mode::USB) freqOffset += bw / 2.0; if (mode == Mode::LSB) freqOffset -= bw / 2.0; std::vector<Complex> chunkToProcess(iqBuffer.begin(), iqBuffer.begin() + readCount); auto audioData = demod.process(chunkToProcess, freqOffset, bw, mode, stereo); if (aprsActive) { std::vector<float> mono; mono.reserve(audioData.size()/2); for(size_t i=0; i<audioData.size(); i+=2) mono.push_back(audioData[i]); aprsDecoder.process(mono); } if (rssiDb < sqThr) std::fill(audioData.begin(), audioData.end(), 0.0f); float finalVol = muted ? 0.0f : vol; for (auto& s : audioData) s *= finalVol; audio.pushSamples(audioData); if (recorder.active && rMode == RecMode::AUDIO) recorder.write(audioData.data(), audioData.size()); }
            std::vector<uint8_t> tempRow(INTERNAL_WATERFALL_WIDTH * 4); for (int x = 0; x < INTERNAL_WATERFALL_WIDTH; x++) { int fftIdx = (int)((float)x / INTERNAL_WATERFALL_WIDTH * FFT_SIZE); int sIdx = (fftIdx + FFT_SIZE / 2) % FFT_SIZE; float rawMag = std::abs(fftData[sIdx]) / FFT_SIZE; float rawDb = 20 * std::log10(rawMag + 1e-12); float norm = (rawDb - minDb) / (maxDb - minDb); sf::Color c = getHeatmap(norm, themeID); int px = x * 4; tempRow[px] = c.r; tempRow[px + 1] = c.g; tempRow[px + 2] = c.b; tempRow[px + 3] = 255; }
            float alpha = (play) ? 0.3f : 1.0f; for (int i = 0; i < FFT_SIZE; i++) { int idx = (i + FFT_SIZE / 2) % FFT_SIZE; float mag = std::abs(fftData[idx]) / FFT_SIZE; float db = 20 * std::log10(mag + 1e-12); localFftHistory[i] = localFftHistory[i] * (1.0f - alpha) + db * alpha; }
            { std::lock_guard<std::mutex> lock(shared.mtx); shared.fftSpectrum = localFftHistory; shared.waterfallRow = tempRow; shared.newWaterfallData = true; }
        } else { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    }
    if (recorder.active) recorder.stop();
}

struct LayoutState { float winW, winH, sidebarX, specW, specH, waterfallH; };

int main() {
    { std::lock_guard<std::mutex> lock(sourceMtx); currentSource = std::make_shared<FileSource>(); if (currentSource->open("None")) {} }
    AudioSink audio; std::vector<std::string> deviceNames; for (const auto& dev : audio.availableDevices) deviceNames.push_back(dev.name);
    audio.initDevice(0, (int)AUDIO_RATE); audio.start();
    SharedData sharedData; sharedData.aprsLog = loadLastLogLines("aprs.log", 10);
    std::atomic<bool> dspRunning {true}; std::thread dspThread(dspWorker, std::ref(dspRunning), std::ref(sharedData), std::ref(audio));
    sf::RenderWindow window(sf::VideoMode({1280, 800}), "OrbitSDR"); window.setFramerateLimit(60);
    sf::Font font; if (!font.openFromFile("/System/Library/Fonts/Helvetica.ttc") && !font.openFromFile("C:/Windows/Fonts/arial.ttf") && !font.openFromFile("arial.ttf")) { std::cerr << "Font not found!\n"; }
    auto cursorArrow = sf::Cursor::createFromSystem(sf::Cursor::Type::Arrow);
    auto cursorHand = sf::Cursor::createFromSystem(sf::Cursor::Type::Hand);
    auto cursorSizeH = sf::Cursor::createFromSystem(sf::Cursor::Type::SizeHorizontal);

    sf::RectangleShape topBar; topBar.setFillColor(sf::Color(19, 19, 21));
    FrequencyDisplay freqVFO(20, 8, font); freqVFO.setFrequency(100000000);
    SdrButton btnTuningMode(40, 40, "FIX", font); btnTuningMode.setColor(sf::Color(80, 80, 80)); bool stickyCenterMode = false;
    SdrButton btnPlay(40, 40, ">", font); btnPlay.setColor(sf::Color(116, 57, 57));
    SdrButton btnMute(40, 40, "M", font); Slider volSlider(150, 0.0f, 1.0f, 0.5f, "Volume", font); Slider timeSlider(100, 0.0f, 1.0f, 0.0f, "Timeline", font);
    Sidebar sidebar(SIDEBAR_W, font);

    auto chkAprs = std::make_shared<Checkbox>("Enable APRS Decoder", font); 
    auto btnCopyAprs = std::make_shared<SdrButton>(200, 25, "Copy Last Packet", font); 
    auto btnNFM = std::make_shared<SdrButton>(40, 25, "NFM", font); btnNFM->setActive(true); auto btnAM = std::make_shared<SdrButton>(40, 25, "AM", font); auto btnWFM = std::make_shared<SdrButton>(40, 25, "WFM", font); auto btnUSB = std::make_shared<SdrButton>(40, 25, "USB", font); auto btnLSB = std::make_shared<SdrButton>(40, 25, "LSB", font); auto btnOFF = std::make_shared<SdrButton>(40, 25, "OFF", font);
    auto rowModes = std::make_shared<RowContainer>(); rowModes->add(btnNFM); rowModes->add(btnAM); rowModes->add(btnWFM); rowModes->add(btnUSB); rowModes->add(btnLSB); rowModes->add(btnOFF);
    auto slBW = std::make_shared<Slider>(SIDEBAR_W - 40, 1000.0f, 200000.0f, 12500.0f, "Bandwidth (Hz)", font); 

    auto modSource = sidebar.addModule("Source / Input");
    auto ddSource = std::make_shared<Dropdown>(SIDEBAR_W - 40, 25.0f, font); ddSource->setOptions({"File (Baseband)", "RTL-SDR", "SDRPlay"});
    modSource->addWidget(std::make_shared<Label>("Input Device:", font)); modSource->addWidget(ddSource);
    auto ddRate = std::make_shared<Dropdown>(SIDEBAR_W - 40, 25.0f, font); ddRate->setOptions({"None"});
    modSource->addWidget(std::make_shared<Label>("Sample Rate / File:", font)); modSource->addWidget(ddRate);

    auto modRadio = sidebar.addModule("Radio Control");
    auto slGain = std::make_shared<Slider>(SIDEBAR_W - 40, 0.0f, 50.0f, 0.0f, "RF Gain (dB)", font); 
    slGain->onChange = [&](float v) { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.rfGain = v; }; 
    auto chkAgc = std::make_shared<Checkbox>("Automatic Gain Control (AGC)", font, true); 
    chkAgc->onToggle = [&](bool b) { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.rfGain = b ? -1.0f : slGain->currentVal; slGain->setEnabled(!b); }; 
    slGain->setEnabled(false);

    auto lblAntenna = std::make_shared<Label>("Antenna (SDRPlay):", font);
    auto ddAntenna = std::make_shared<Dropdown>(SIDEBAR_W - 40, 25.0f, font);
    ddAntenna->setOptions({"Antenna A / Default", "Antenna B", "Antenna C / Hi-Z"});
    ddAntenna->onChange = [&](int idx) { std::lock_guard<std::mutex> l(sourceMtx); if(currentSource) currentSource->setHardwareOption("antenna", idx); };

    auto lblDirectSamp = std::make_shared<Label>("Direct Sampling (RTL):", font);
    auto ddDirectSamp = std::make_shared<Dropdown>(SIDEBAR_W - 40, 25.0f, font);
    ddDirectSamp->setOptions({"Off (Default)", "I-ADC", "Q-ADC"});
    ddDirectSamp->onChange = [&](int idx) { std::lock_guard<std::mutex> l(sourceMtx); if(currentSource) currentSource->setHardwareOption("direct_sampling", idx); };

    auto rowFilters = std::make_shared<RowContainer>();
    auto chkFmNotch = std::make_shared<Checkbox>("FM Notch", font); chkFmNotch->onToggle = [&](bool b) { std::lock_guard<std::mutex> l(sourceMtx); if(currentSource) currentSource->setHardwareOption("fm_notch", b?1:0); };
    auto chkMwNotch = std::make_shared<Checkbox>("MW Notch", font); chkMwNotch->onToggle = [&](bool b) { std::lock_guard<std::mutex> l(sourceMtx); if(currentSource) currentSource->setHardwareOption("mw_notch", b?1:0); };
    rowFilters->add(chkFmNotch); rowFilters->add(chkMwNotch);

    auto chkBiasT = std::make_shared<Checkbox>("Bias-T Power", font); 
    chkBiasT->onToggle = [&](bool b) { std::lock_guard<std::mutex> l(sourceMtx); if(currentSource) currentSource->setHardwareOption("bias_t", b?1:0); };

    auto updateRadioControls = [&](int sourceIdx) {
        modRadio->widgets.clear();
        if (sourceIdx != 0) { modRadio->addWidget(slGain); modRadio->addWidget(chkAgc); }
        if (sourceIdx == 1) { modRadio->addWidget(lblDirectSamp); modRadio->addWidget(ddDirectSamp); modRadio->addWidget(chkBiasT); } 
        else if (sourceIdx == 2) { modRadio->addWidget(lblAntenna); modRadio->addWidget(ddAntenna); modRadio->addWidget(rowFilters); modRadio->addWidget(chkBiasT); }
        sidebar.recalculateLayout();
    };
    updateRadioControls(0);

    std::function<void(int, int, std::string)> changeSource;
    long long currentCenterFreq = 0; long long pendingCenterFreq = 0;
    
    changeSource = [&](int sourceIdx, int rateIdx, std::string pathOverride) {
        std::shared_ptr<IQSource> oldSource; { std::lock_guard<std::mutex> lock(sourceMtx); oldSource = currentSource; currentSource = nullptr; }
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); if (oldSource) { oldSource->stop(); oldSource->close(); }
        std::shared_ptr<IQSource> newSource; uint32_t targetRate = 0;
        if (sourceIdx == 1) targetRate = (rateIdx < RTL_RATES_VAL.size()) ? RTL_RATES_VAL[rateIdx] : 2048000;
        if (sourceIdx == 2) targetRate = (rateIdx < SDRPLAY_RATES_VAL.size()) ? SDRPLAY_RATES_VAL[rateIdx] : 2000000;
        bool success = false;
        
        updateRadioControls(sourceIdx);

        if (sourceIdx == 0) { 
            newSource = std::make_shared<FileSource>(); 
            std::string path = pathOverride.empty() ? "None" : pathOverride; 
            success = newSource->open(path); 
            { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.currentFilename = path; } 
            ddRate->setOptions({path.substr(path.find_last_of("/\\") + 1)});
            
            std::regex freqRegex(R"(_(\d+)Hz_)");
            std::smatch match;
            if (std::regex_search(path, match, freqRegex)) {
                try {
                    long long f = std::stoll(match[1]);
                    currentCenterFreq = f;
                    freqVFO.setFrequency(f);
                    { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.centerFreq = f; }
                } catch(...) {}
            }
        }
        else if (sourceIdx == 1) { newSource = std::make_shared<RtlSdrSource>(); ddRate->setOptions(newSource->getAvailableSampleRatesText()); ddRate->setSelection(rateIdx); success = newSource->open("0", targetRate); long long startFreq = 97800000; freqVFO.setFrequency(startFreq); currentCenterFreq = startFreq; newSource->setCenterFrequency(startFreq); { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.tunedFreqPercent = 0.5; sharedData.centerFreq = startFreq; } }
        else if (sourceIdx == 2) { newSource = std::make_shared<SdrPlaySource>(); ddRate->setOptions(newSource->getAvailableSampleRatesText()); ddRate->setSelection(rateIdx); success = newSource->open("", targetRate); long long startFreq = 97800000; freqVFO.setFrequency(startFreq); currentCenterFreq = startFreq; newSource->setCenterFrequency(startFreq); { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.tunedFreqPercent = 0.5; sharedData.centerFreq = startFreq; } }
        if (!success && sourceIdx != 0) { ddSource->setSelection(0); changeSource(0,0,"None"); return; }
        { std::lock_guard<std::mutex> lock(sourceMtx); currentSource = newSource; } { std::lock_guard<std::mutex> lock(sharedData.mtx); sharedData.isPlaying = false; sharedData.isRecording = false; } audio.stop(); btnPlay.setText(">"); btnPlay.setColor(sf::Color(116, 57, 57)); audio.clear();
    };

    ddSource->onChange = [&](int idx) { if (idx == 0) { std::string path = openFileDialog(); if (!path.empty()) changeSource(0, 0, path); else ddSource->setSelection(1); } else changeSource(idx, 0, ""); };
    ddRate->onChange = [&](int idx) { if (ddSource->selectedIndex == 0) { std::string path = openFileDialog(); if (!path.empty()) changeSource(0, 0, path); } else changeSource(ddSource->selectedIndex, idx, ""); };

    auto modAudio = sidebar.addModule("Audio Output");
    auto ddAudio = std::make_shared<Dropdown>(SIDEBAR_W - 40, 25.0f, font); ddAudio->setOptions(deviceNames); ddAudio->onChange = [&](int idx) { audio.stop(); audio.initDevice(idx, (int)AUDIO_RATE); if (sharedData.isPlaying) audio.start(); }; modAudio->addWidget(ddAudio);

    auto modDemod = sidebar.addModule("Demodulator");
    modDemod->addWidget(rowModes);
    slBW->onChange = [&](float v) { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.bandwidth = v; std::string txt = "Bandwidth: " + (v >= 1000 ? std::to_string((int)(v/1000)) + " kHz" : std::to_string((int)v) + " Hz"); slBW->setText(txt); }; slBW->onChange(12500.0f); modDemod->addWidget(slBW);
    auto slSq = std::make_shared<Slider>(SIDEBAR_W - 40, -100.0f, 0.0f, -100.0f, "Squelch (dB)", font); slSq->onChange = [&](float v) { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.squelchThreshold = v; }; modDemod->addWidget(slSq);
    auto chkStereo = std::make_shared<Checkbox>("Stereo (WFM only)", font); chkStereo->onToggle = [&](bool b) { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.stereoEnabled = b; }; modDemod->addWidget(chkStereo);
    
    Mode previousMode = Mode::NFM;
    auto setMode = [&](Mode m, SdrButton* me) { 
        if (chkAprs->checked) { chkAprs->checked = false; { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.aprsEnabled = false; } rowModes->setEnabled(true); }
        btnNFM->setActive(false); btnAM->setActive(false); btnWFM->setActive(false); btnUSB->setActive(false); btnLSB->setActive(false); btnOFF->setActive(false); me->setActive(true); 
        std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.mode = m; 
        if (m == Mode::WFM) { slBW->setLimits(50000, 200000); slBW->setValueSilent(180000); sharedData.bandwidth = 180000; slBW->setText("Bandwidth: 180 kHz"); } 
        else if (m == Mode::NFM || m == Mode::AM) { slBW->setLimits(4000, 40000); slBW->setValueSilent(12500); sharedData.bandwidth = 12500; slBW->setText("Bandwidth: 12.5 kHz"); } 
        else if (m == Mode::OFF) { } else { slBW->setLimits(1000, 10000); slBW->setValueSilent(3000); sharedData.bandwidth = 3000; slBW->setText("Bandwidth: 3 kHz"); } 
    };
    btnNFM->onClick = [&](){ setMode(Mode::NFM, btnNFM.get()); }; btnAM->onClick = [&](){ setMode(Mode::AM, btnAM.get()); }; btnWFM->onClick = [&](){ setMode(Mode::WFM, btnWFM.get()); }; btnUSB->onClick = [&](){ setMode(Mode::USB, btnUSB.get()); }; btnLSB->onClick = [&](){ setMode(Mode::LSB, btnLSB.get()); }; btnOFF->onClick = [&](){ setMode(Mode::OFF, btnOFF.get()); };

    chkAprs->onToggle = [&](bool b) { 
        { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.aprsEnabled = b; } rowModes->setEnabled(!b); 
        if (b) {
            { std::lock_guard<std::mutex> l(sharedData.mtx); previousMode = sharedData.mode; }
            btnNFM->setActive(true); btnAM->setActive(false); btnWFM->setActive(false); btnUSB->setActive(false); btnLSB->setActive(false); btnOFF->setActive(false);
            { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.mode = Mode::NFM; sharedData.bandwidth = 12000; }
            slBW->setValueSilent(12000); slBW->setText("Bandwidth: 12 kHz");
        } else {
            if(previousMode == Mode::NFM) setMode(Mode::NFM, btnNFM.get()); else if(previousMode == Mode::AM) setMode(Mode::AM, btnAM.get()); else if(previousMode == Mode::WFM) setMode(Mode::WFM, btnWFM.get()); else if(previousMode == Mode::USB) setMode(Mode::USB, btnUSB.get()); else if(previousMode == Mode::LSB) setMode(Mode::LSB, btnLSB.get()); else setMode(Mode::OFF, btnOFF.get());
        }
    }; 
    auto modDigi = sidebar.addModule("Decoders"); modDigi->addWidget(chkAprs);
    btnCopyAprs->onClick = [&](){ std::lock_guard<std::mutex> l(sharedData.mtx); sf::Clipboard::setString(sharedData.lastAprs.raw); }; modDigi->addWidget(btnCopyAprs);

    auto modDisp = sidebar.addModule("Display");
    
    // --- FIX: MIN DB SLIDER SYNC ---
    auto slMinDb = std::make_shared<Slider>(SIDEBAR_W - 40, -120.0f, -20.0f, -90.0f, "Min dB", font); 
    slMinDb->onChange = [&](float v) { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.minDb = v; }; 
    modDisp->addWidget(slMinDb);
    
    auto slMaxDb = std::make_shared<Slider>(SIDEBAR_W - 40, -40.0f, 40.0f, 0.0f, "Max dB", font); slMaxDb->onChange = [&](float v) { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.maxDb = v; }; modDisp->addWidget(slMaxDb);
    
    // --- ZOOM SLIDER ---
    auto slZoom = std::make_shared<Slider>(SIDEBAR_W - 40, 1.0f, 8.0f, 1.0f, "Zoom", font); 
    slZoom->onChange = [&](float v) { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.zoomLevel = v; }; 
    modDisp->addWidget(slZoom);

    // --- THEME DROPDOWN ---
    auto ddTheme = std::make_shared<Dropdown>(SIDEBAR_W - 40, 25.0f, font); 
    ddTheme->setOptions(THEME_NAMES); 
    ddTheme->setSelection(4); // Default to Orbit Plus
    ddTheme->onChange = [&](int idx) { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.waterfallTheme = idx; };
    modDisp->addWidget(std::make_shared<Label>("Waterfall Theme:", font));
    modDisp->addWidget(ddTheme);

    auto ddSnap = std::make_shared<Dropdown>(150.0f, 25.0f, font); ddSnap->setOptions(STEP_NAMES); ddSnap->setSelection(5); modDisp->addWidget(std::make_shared<Label>("Tuning Step:", font)); modDisp->addWidget(ddSnap);

    auto modRec = sidebar.addModule("Recording");
    auto rowRecMode = std::make_shared<RowContainer>(); auto btnRecAudio = std::make_shared<SdrButton>(80, 25, "Audio", font); btnRecAudio->setActive(true); auto btnRecIQ = std::make_shared<SdrButton>(80, 25, "Baseband", font);
    btnRecAudio->onClick = [&](){ btnRecAudio->setActive(true); btnRecIQ->setActive(false); std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.recMode = RecMode::AUDIO; }; btnRecIQ->onClick = [&](){ btnRecAudio->setActive(false); btnRecIQ->setActive(true); std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.recMode = RecMode::BASEBAND; };
    rowRecMode->add(btnRecAudio); rowRecMode->add(btnRecIQ); modRec->addWidget(rowRecMode);
    auto btnSetFolder = std::make_shared<SdrButton>(110, 25, "Output Folder...", font); btnSetFolder->onClick = [&](){ std::string p = selectFolderDialog(); if(!p.empty()) { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.recPath = p; } }; 
    rowRecMode->add(btnSetFolder);
    auto btnRecToggle = std::make_shared<SdrButton>(SIDEBAR_W-40, 35, "START RECORDING", font); btnRecToggle->setColor(sf::Color(40, 40, 45));
    btnRecToggle->onClick = [&](){ std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.isRecording = !sharedData.isRecording; if(sharedData.isRecording) { btnRecToggle->setText("STOP RECORDING"); btnRecToggle->setColor(sf::Color(180, 50, 50)); } else { btnRecToggle->setText("START RECORDING"); btnRecToggle->setColor(sf::Color(40, 40, 45)); } }; modRec->addWidget(btnRecToggle);

    std::vector<std::uint8_t> waterfall(INTERNAL_WATERFALL_WIDTH * 2048 * 4, 0); 
    sf::Texture wTex; if(!wTex.resize({INTERNAL_WATERFALL_WIDTH, 2048})) return 1; sf::Sprite wSpr(wTex); 
    LayoutState layout; sf::Vector2u lastSize = window.getSize();
    
    // --- UPDATED LAYOUT LOGIC (Fixed Waterfall Width) ---
    auto updateLayout = [&](int w, int h) {
        layout.winW = (float)w; layout.winH = (float)h; 
        layout.sidebarX = layout.winW - SIDEBAR_W; 
        layout.specW = layout.sidebarX; // FFT area stops at sidebar
        layout.specH = 250.0f; 
        layout.waterfallH = layout.winH - TOP_BAR_H - layout.specH; 
        if (layout.waterfallH < 100) layout.waterfallH = 100;

        topBar.setSize({layout.winW, (float)TOP_BAR_H}); 
        freqVFO.setPosition(20, 4); btnTuningMode.setPosition(370, 10); btnPlay.setPosition(320, 10); volSlider.setPosition(layout.winW - 170, 10); btnMute.setPosition(layout.winW - 220, 10);
        sidebar.setGeometry(layout.sidebarX, TOP_BAR_H, layout.winH - TOP_BAR_H);
        timeSlider.setPosition(20, layout.winH - 30); timeSlider.setWidth(layout.specW - 40);

        wSpr.setPosition({0, TOP_BAR_H + layout.specH}); 
        // FIX: Scale waterfall to specW (not full window), preventing overlap
        float scaleX = layout.specW / (float)INTERNAL_WATERFALL_WIDTH; 
        wSpr.setScale({scaleX, 1.0f}); 
        // Correct SFML 3 Syntax for IntRect
        wSpr.setTextureRect(sf::IntRect({0, 0}, {INTERNAL_WATERFALL_WIDTH, (int)layout.waterfallH})); 
    };
    updateLayout(window.getSize().x, window.getSize().y);

    sf::Clock debouncer; sf::Clock interactionCooldown; 
    bool isDraggingScale = false, isSpectrumDragging = false; 
    bool isDraggingBW = false; float dragStartBW = 0.0f; float dragStartMouseX = 0.0f;
    float lastDragX = 0.0f; float aprsLogScrollOffset = 0.0f;

    auto applySpectrumTuning = [&](float mouseX) { 
        bool isHw = false; double hwSampleRate = 2e6; 
        { std::lock_guard<std::mutex> l(sourceMtx); if (currentSource) { isHw = currentSource->isHardware(); hwSampleRate = currentSource->getSampleRate(); } } 
        float zoom = 1.0f; { std::lock_guard<std::mutex> l(sharedData.mtx); zoom = sharedData.zoomLevel; }
        
        // Adjust tuning calculation for Zoom
        double effectiveRate = hwSampleRate / zoom;
        double clickPct = mouseX / layout.specW; 
        double offsetHz = (clickPct - 0.5) * effectiveRate; 
        
        long long clickedFreq = currentCenterFreq + (long long)offsetHz; 
        long long step = STEP_VALUES[ddSnap->selectedIndex]; 
        if (step > 0) { clickedFreq = (long long)std::round((double)clickedFreq / step) * step; double newOffset = (double)(clickedFreq - currentCenterFreq); clickPct = 0.5 + (newOffset / effectiveRate); } 
        
        if (stickyCenterMode && isHw) { 
            pendingCenterFreq = clickedFreq; debouncer.restart(); 
            { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.tunedFreqPercent = 0.5; } 
            freqVFO.setFrequency(clickedFreq); 
        } else { 
            { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.tunedFreqPercent = 0.5 + (offsetHz / hwSampleRate); } 
            freqVFO.setFrequency(clickedFreq); 
        } 
    };

    while (window.isOpen()) {
        sf::Vector2u currSize = window.getSize(); if (currSize != lastSize && currSize.x > 0 && currSize.y > 0) { sf::FloatRect visibleArea({0.f, 0.f}, {(float)currSize.x, (float)currSize.y}); window.setView(sf::View(visibleArea)); updateLayout(currSize.x, currSize.y); lastSize = currSize; }
        bool isHw = false; double hwSampleRate = 2e6; double prog = 0.0; { std::lock_guard<std::mutex> l(sourceMtx); if (currentSource) { isHw = currentSource->isHardware(); hwSampleRate = currentSource->getSampleRate(); prog = currentSource->getProgress(); } }
        if (isHw) freqVFO.setEnabled(true); else freqVFO.setEnabled(false); if (!ddSource->isOpen && !ddRate->isOpen) freqVFO.update(window); volSlider.update(window); if (std::abs(volSlider.currentVal - sharedData.volume) > 0.01f) { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.volume = volSlider.currentVal; }
        if (!isHw) { timeSlider.update(window); if (timeSlider.isDragging) { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.pendingSeekRequest = timeSlider.currentVal; } else { timeSlider.currentVal = prog; timeSlider.updateHandlePos(); } }

        bool aprsOn = false; double tunePct = 0.5; Mode mode = Mode::NFM; float currentZoom = 1.0f; int currentTheme = 0;
        { std::lock_guard<std::mutex> l(sharedData.mtx); aprsOn = sharedData.aprsEnabled; tunePct = sharedData.tunedFreqPercent; mode = sharedData.mode; currentZoom = sharedData.zoomLevel; currentTheme = sharedData.waterfallTheme; }
        slZoom->update(window); // Update zoom slider

        while (const std::optional<sf::Event> ev = window.pollEvent()) {
            if (ev->is<sf::Event::Closed>()) window.close();
            volSlider.handleEvent(*ev, window);
            slZoom->handleEvent(*ev, window); // Handle zoom events
            if (!isHw) timeSlider.handleEvent(*ev, window);
            if (btnMute.isClicked(*ev, window)) { static bool m = false; m = !m; btnMute.setColor(m ? sf::Color(116, 57, 57) : sf::Color(60, 60, 60)); std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.isMuted = m; }
            if (btnPlay.isClicked(*ev, window)) { bool s; { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.isPlaying = !sharedData.isPlaying; s = sharedData.isPlaying; } if (s) { btnPlay.setText("||"); btnPlay.setColor(sf::Color(78, 78, 236)); audio.start(); { std::lock_guard<std::mutex> l(sourceMtx); if (currentSource) currentSource->start(); } } else { btnPlay.setText(">"); btnPlay.setColor(sf::Color(116, 57, 57)); audio.stop(); { std::lock_guard<std::mutex> l(sourceMtx); if (currentSource) currentSource->stop(); } } }
            if (isHw && btnTuningMode.isClicked(*ev, window)) { stickyCenterMode = !stickyCenterMode; if(stickyCenterMode) { btnTuningMode.setText("CTR"); btnTuningMode.setColor(sf::Color(0,100,200)); { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.tunedFreqPercent = 0.5; } pendingCenterFreq = freqVFO.getFrequency(); debouncer.restart(); } else { btnTuningMode.setText("FIX"); btnTuningMode.setColor(sf::Color(80,80,80)); } }
            if (isHw && freqVFO.handleEvent(*ev)) { long long targetVFO = freqVFO.getFrequency(); if (stickyCenterMode) { pendingCenterFreq = targetVFO; debouncer.restart(); { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.tunedFreqPercent = 0.5; } } else { double halfBW = hwSampleRate / 2.0; double minF = (double)currentCenterFreq - halfBW; double maxF = (double)currentCenterFreq + halfBW; if (targetVFO > maxF || targetVFO < minF) { pendingCenterFreq = targetVFO; debouncer.restart(); { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.tunedFreqPercent = 0.5; } } else { double pct = 0.5 + ((double)(targetVFO - currentCenterFreq) / hwSampleRate); { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.tunedFreqPercent = pct; } } } }
            
            bool overAprs = (aprsOn && window.mapPixelToCoords(sf::Mouse::getPosition(window)).y > (layout.winH - 200));
            if (const auto* scroll = ev->getIf<sf::Event::MouseWheelScrolled>()) {
                if (overAprs) {
                    aprsLogScrollOffset += scroll->delta * 20.0f; if (aprsLogScrollOffset > 0.0f) aprsLogScrollOffset = 0.0f;
                } else {
                    sf::Vector2f m = window.mapPixelToCoords(sf::Vector2i((int)scroll->position.x, (int)scroll->position.y)); 
                    if (m.y > TOP_BAR_H && m.x < layout.specW) { long long step = STEP_VALUES[ddSnap->selectedIndex]; if (step == 0) step = 100; long long current = freqVFO.getFrequency(); long long rawNext = current + (long long)(scroll->delta * step); if (step > 0) { long long remainder = rawNext % step; if (remainder > step / 2) rawNext += (step - remainder); else rawNext -= remainder; } if (rawNext < 0) rawNext = 0; freqVFO.setFrequency(rawNext); if (stickyCenterMode && isHw) { pendingCenterFreq = rawNext; debouncer.restart(); { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.tunedFreqPercent = 0.5; } } else { double newOffset = (double)(rawNext - currentCenterFreq); double clickPct = 0.5 + (newOffset / hwSampleRate); clickPct = std::clamp(clickPct, 0.0, 1.0); { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.tunedFreqPercent = clickPct; } } }
                }
            }

            if (const auto* mb = ev->getIf<sf::Event::MouseButtonPressed>()) { 
                if (mb->button == sf::Mouse::Button::Left) { 
                    sf::Vector2f m = window.mapPixelToCoords(sf::Mouse::getPosition(window)); 
                    bool overTimeline = (timeSlider.isDragging || (m.y > layout.winH - 40)); 
                    bool inFreqScaleZone = (m.y >= TOP_BAR_H + layout.specH - 10 && m.y <= TOP_BAR_H + layout.specH + 20 && m.x < layout.specW); 
                    
                    // --- ZOOMED BANDWIDTH HIT TEST ---
                    double offsetFromCenter = (tunePct - 0.5); 
                    float visualCenterX = (0.5 + (offsetFromCenter * currentZoom)) * layout.specW;
                    
                    float bwPixels = (slBW->currentVal / (hwSampleRate / currentZoom)) * layout.specW;
                    float leftEdge = visualCenterX - bwPixels / 2.0f;
                    float rightEdge = visualCenterX + bwPixels / 2.0f;
                    if (mode == Mode::USB) { leftEdge = visualCenterX; rightEdge = visualCenterX + bwPixels; }
                    if (mode == Mode::LSB) { leftEdge = visualCenterX - bwPixels; rightEdge = visualCenterX; }
                    
                    bool hitEdge = (std::abs(m.x - leftEdge) < 6 || std::abs(m.x - rightEdge) < 6) && (m.y > TOP_BAR_H && m.y < TOP_BAR_H + layout.specH);
                    
                    if (hitEdge && !overAprs) {
                        isDraggingBW = true; dragStartBW = slBW->currentVal; dragStartMouseX = m.x;
                    } 
                    else if (inFreqScaleZone && !overTimeline && !overAprs) { isDraggingScale = true; lastDragX = m.x; } 
                    else if (!overTimeline && !overAprs && m.x < layout.specW && m.y > TOP_BAR_H && m.y < (TOP_BAR_H + layout.specH + layout.waterfallH)) { isSpectrumDragging = true; applySpectrumTuning(m.x); } 
                } 
            } 
            else if (const auto* mr = ev->getIf<sf::Event::MouseButtonReleased>()) { 
                if (mr->button == sf::Mouse::Button::Left) { isSpectrumDragging = false; isDraggingScale = false; isDraggingBW = false; } 
            } 
            else if (const auto* mm = ev->getIf<sf::Event::MouseMoved>()) { 
                sf::Vector2f m = window.mapPixelToCoords(mm->position); 
                
                // --- ZOOMED CURSOR LOGIC ---
                double offsetFromCenter = (tunePct - 0.5); 
                float visualCenterX = (0.5 + (offsetFromCenter * currentZoom)) * layout.specW;
                float bwPixels = (slBW->currentVal / (hwSampleRate / currentZoom)) * layout.specW;
                float leftEdge = visualCenterX - bwPixels / 2.0f;
                float rightEdge = visualCenterX + bwPixels / 2.0f;
                if (mode == Mode::USB) { leftEdge = visualCenterX; rightEdge = visualCenterX + bwPixels; }
                if (mode == Mode::LSB) { leftEdge = visualCenterX - bwPixels; rightEdge = visualCenterX; }
                
                bool hitEdge = (std::abs(m.x - leftEdge) < 6 || std::abs(m.x - rightEdge) < 6) && (m.y > TOP_BAR_H && m.y < TOP_BAR_H + layout.specH);
                if ((hitEdge || isDraggingBW) && cursorSizeH) window.setMouseCursor(*cursorSizeH); 

                if (isDraggingBW) {
                    float hzPerPx = (hwSampleRate / currentZoom) / layout.specW;
                    float dist = std::abs(m.x - visualCenterX);
                    float newBW = dist * hzPerPx * 2.0f; 
                    if (mode == Mode::USB || mode == Mode::LSB) newBW = dist * hzPerPx;
                    newBW = std::clamp(newBW, slBW->minVal, slBW->maxVal);
                    slBW->setValueSilent(newBW); slBW->onChange(newBW);
                }
                else if (isSpectrumDragging) applySpectrumTuning(m.x); 
                
                if (isDraggingScale) { 
                    float dx = lastDragX - m.x; lastDragX = m.x; 
                    double hzPerPx = (hwSampleRate / currentZoom) / layout.specW; 
                    long long shift = (long long)(dx * hzPerPx); 
                    long long nextCenter = currentCenterFreq + shift; if (nextCenter < 0) nextCenter = 0; 
                    currentCenterFreq = nextCenter; { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.centerFreq = nextCenter; } 
                    
                    if (debouncer.getElapsedTime().asMilliseconds() > TUNING_LATENCY_MS) { { std::lock_guard<std::mutex> l(sourceMtx); if (currentSource) currentSource->setCenterFrequency(nextCenter); } debouncer.restart(); } 
                } 
                if (!overAprs && m.x >= 0 && m.x < layout.specW && m.y >= TOP_BAR_H && m.y < (TOP_BAR_H + layout.specH + layout.waterfallH)) { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.mouseX_spectrum = m.x; sharedData.mouseY_spectrum = m.y - TOP_BAR_H; } else { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.mouseX_spectrum = -1.0f; }
            }
            
            sidebar.handleEvent(*ev, window);
        }
        if (pendingCenterFreq != 0 && debouncer.getElapsedTime().asMilliseconds() > TUNING_LATENCY_MS) { std::lock_guard<std::mutex> l(sourceMtx); if (currentSource && currentSource->isHardware()) { currentSource->setCenterFrequency(pendingCenterFreq); currentCenterFreq = pendingCenterFreq; } pendingCenterFreq = 0; }
        sidebar.update(window);
        
        bool showHand = false; sf::Vector2f mousePos = window.mapPixelToCoords(sf::Mouse::getPosition(window));
        if (sidebar.isAnyWidgetHovered(window)) showHand = true;
        if (freqVFO.isHovered) showHand = true;
        if (mousePos.y >= TOP_BAR_H + layout.specH - 10 && mousePos.y <= TOP_BAR_H + layout.specH + 20 && mousePos.x < layout.specW) showHand = true;
        if (volSlider.isMouseOver(window) || timeSlider.isMouseOver(window)) showHand = true;
        if (btnPlay.isMouseOver(window) || btnMute.isMouseOver(window) || btnTuningMode.isMouseOver(window)) showHand = true;
        if (aprsOn && mousePos.y > (layout.winH - 200) && mousePos.x < layout.specW) showHand = true;
        
        if (isDraggingBW && cursorSizeH) { window.setMouseCursor(*cursorSizeH); } else if (showHand && cursorHand) { window.setMouseCursor(*cursorHand); } else if (cursorArrow) { window.setMouseCursor(*cursorArrow); }

        std::vector<double> spectrum; std::vector<uint8_t> row; bool newRow = false; { std::lock_guard<std::mutex> lock(sharedData.mtx); spectrum = sharedData.fftSpectrum; if (sharedData.newWaterfallData) { row = sharedData.waterfallRow; sharedData.newWaterfallData = false; newRow = true; } }
        if (newRow) { std::copy_backward(waterfall.begin(), waterfall.end() - INTERNAL_WATERFALL_WIDTH * 4, waterfall.end()); std::copy(row.begin(), row.end(), waterfall.begin()); wTex.update(waterfall.data()); }
        
        window.clear(sf::Color::Black); long long cf = 0; if (currentSource) cf = currentCenterFreq;
        drawGrid(window, font, 0, TOP_BAR_H, layout.specW, layout.specH, cf, hwSampleRate / currentZoom, slMinDb->currentVal, slMaxDb->currentVal);
        
        // --- FFT DRAWING (Zoomed & Theme Colored) ---
        // SFML 3.0 Compatible: Using resize() + indexing instead of append()
        std::vector<double> smoothedSpectrum = spectrum;
        for (size_t i = 1; i < spectrum.size() - 1; i++) { smoothedSpectrum[i] = (spectrum[i-1] + spectrum[i] * 2.0 + spectrum[i+1]) / 4.0; }
        sf::VertexArray fillArea(sf::PrimitiveType::TriangleStrip);
        sf::VertexArray outline(sf::PrimitiveType::LineStrip);
        
        // Default Blue (Orbit Original)
        sf::Color coreColor(100, 200, 255, 255); 
        sf::Color glowColor(0, 150, 255, 180); 
        sf::Color bottomColor(0, 100, 200, 0);   

        if (currentTheme == 1) { // Magma (Orange/Pink)
            coreColor = sf::Color(255, 200, 100, 255);
            glowColor = sf::Color(255, 100, 50, 180);
            bottomColor = sf::Color(200, 50, 0, 0);
        }
        else if (currentTheme == 2) { // Matrix (Green)
            coreColor = sf::Color(100, 255, 100, 255);
            glowColor = sf::Color(0, 255, 50, 180);
            bottomColor = sf::Color(0, 200, 0, 0);
        }
        else if (currentTheme == 3) { // Grayscale (White)
            coreColor = sf::Color(255, 255, 255, 255);
            glowColor = sf::Color(200, 200, 200, 180);
            bottomColor = sf::Color(100, 100, 100, 0);
        }
        // Theme 4 (Orbit Plus) uses logic per vertex below

        int centerBin = spectrum.size() / 2;
        int spanBins = (int)(spectrum.size() / currentZoom);
        int startBin = centerBin - spanBins / 2;
        int endBin = centerBin + spanBins / 2;
        if (startBin < 0) startBin = 0; if (endBin > (int)spectrum.size()) endBin = spectrum.size();
        
        int numPoints = endBin - startBin;
        if (numPoints < 0) numPoints = 0;

        // Resize first (SFML 3 optimization)
        fillArea.resize(numPoints * 2);
        outline.resize(numPoints);

        for (int i = 0; i < numPoints; ++i) {
            int spectrumIdx = startBin + i;
            float normalizedPos = (float)i / (float)spanBins;
            float x = normalizedPos * layout.specW;
            
            float norm = (smoothedSpectrum[spectrumIdx] - slMinDb->currentVal) / (slMaxDb->currentVal - slMinDb->currentVal);
            float y = layout.specH - (norm * layout.specH);
            if (y < 0) y = 0; if (y > layout.specH) y = layout.specH;
            float topY = y + TOP_BAR_H; float bottomY = layout.specH + TOP_BAR_H;
            
            // Dynamic Color for Orbit Plus
            if (currentTheme == 4) {
               sf::Color dynColor = getHeatmap(norm, 4);
               // FIX: Default coreColor (Blue) stays, only fill is dynamic
               // And made much lighter (alpha 180->100)
               coreColor = sf::Color(100, 200, 255, 255); 
               glowColor = dynColor; glowColor.a = 150; 
               bottomColor = dynColor; bottomColor.a = 20; 
            }

            // SFML 3 Vertex Initialization
            fillArea[2 * i]     = sf::Vertex{sf::Vector2f(x, topY), glowColor};
            fillArea[2 * i + 1] = sf::Vertex{sf::Vector2f(x, bottomY), bottomColor};
            outline[i]          = sf::Vertex{sf::Vector2f(x, topY), coreColor};
        }

        // Draw fill with Additive Blend for "Aurora" effect
        sf::RenderStates states;
        states.blendMode = sf::BlendAdd;
        window.draw(fillArea, states);
        
        // Draw outline normally
        window.draw(outline);
        
        // --- WATERFALL ZOOM ---
        int visibleTexW = (int)(INTERNAL_WATERFALL_WIDTH / currentZoom);
        int texX = (INTERNAL_WATERFALL_WIDTH - visibleTexW) / 2;
        // SFML 3 IntRect Fix
        wSpr.setTextureRect(sf::IntRect({texX, 0}, {visibleTexW, (int)layout.waterfallH}));
        float scaleX = layout.specW / (float)visibleTexW;
        wSpr.setScale({scaleX, 1.0f});
        window.draw(wSpr);
        
        float mx = -1.0f; float my = -1.0f; { std::lock_guard<std::mutex> l(sharedData.mtx); mx = sharedData.mouseX_spectrum; my = sharedData.mouseY_spectrum; }
        if (mx != -1.0f) { sf::Color guideColor(100, 100, 100); sf::VertexArray lineFFT(sf::PrimitiveType::Lines, 2); lineFFT[0].position = {mx, (float)TOP_BAR_H}; lineFFT[0].color = guideColor; lineFFT[1].position = {mx, (float)layout.specH + TOP_BAR_H}; lineFFT[1].color = guideColor; window.draw(lineFFT); if (my > (float)layout.specH) { sf::VertexArray lineWaterfall(sf::PrimitiveType::Lines, 2); lineWaterfall[0].position = {mx, (float)layout.specH + TOP_BAR_H}; lineWaterfall[0].color = guideColor; lineWaterfall[1].position = {mx, (float)(layout.specH + layout.waterfallH + TOP_BAR_H)}; lineWaterfall[1].color = guideColor; window.draw(lineWaterfall); } }

        sf::RectangleShape tunerRect; 
        double offsetFromCenter = (tunePct - 0.5); 
        float visualCenterX = (0.5 + (offsetFromCenter * currentZoom)) * layout.specW;
        float bwPixels = (slBW->currentVal / (hwSampleRate / currentZoom)) * layout.specW; 
        if (bwPixels < 2.0f) bwPixels = 2.0f; 
        
        float rectX = visualCenterX - bwPixels / 2.0f; 
        if (mode == Mode::USB) rectX += bwPixels / 2.0f; 
        if (mode == Mode::LSB) rectX -= bwPixels / 2.0f; 
        
        tunerRect.setSize({bwPixels, (float)layout.specH}); tunerRect.setPosition({rectX, (float)TOP_BAR_H}); 
        tunerRect.setFillColor(mode == Mode::OFF ? sf::Color(50, 50, 50, 40) : sf::Color(200, 200, 200, 50)); 
        window.draw(tunerRect);
        
        sf::VertexArray centerLine(sf::PrimitiveType::Lines, 2); 
        centerLine[0].position = {visualCenterX, (float)TOP_BAR_H}; centerLine[0].color = sf::Color::Red; 
        centerLine[1].position = {visualCenterX, (float)layout.specH + TOP_BAR_H}; centerLine[1].color = sf::Color::Red; 
        window.draw(centerLine);
        
        if (aprsOn) { 
            float aprsH = 200.0f; float overlayY = layout.winH - aprsH;
            sf::RectangleShape bg({layout.specW, aprsH}); bg.setPosition({0, overlayY}); bg.setFillColor(sf::Color(20, 20, 25, 240)); bg.setOutlineColor(sf::Color::White); bg.setOutlineThickness(1); window.draw(bg); 
            AprsLastPacket pkt; std::deque<std::string> logs; { std::lock_guard<std::mutex> l(sharedData.mtx); pkt = sharedData.lastAprs; logs = sharedData.aprsLog; } 
            float compassX = 60.0f; float compassY = overlayY + 70.0f; float compassR = 40.0f;
            sf::CircleShape compass(compassR); compass.setOrigin({compassR, compassR}); compass.setPosition({compassX, compassY}); compass.setFillColor(sf::Color::Transparent); compass.setOutlineColor(sf::Color(100,100,100)); compass.setOutlineThickness(2); window.draw(compass);
            sf::Text dirT(font, "N", 12); dirT.setFillColor(sf::Color::Yellow); dirT.setPosition({compassX - 4, compassY - compassR - 15}); window.draw(dirT);
            sf::RectangleShape northTick({2, 6}); northTick.setFillColor(sf::Color::Yellow); northTick.setPosition({compassX - 1, compassY - compassR}); window.draw(northTick);
            if (pkt.course >= 0) { sf::VertexArray arrow(sf::PrimitiveType::Lines, 2); float rad = (pkt.course - 90.0f) * (3.14159f / 180.0f); arrow[0].position = {compassX, compassY}; arrow[0].color = sf::Color::Red; arrow[1].position = {compassX + cos(rad)*compassR, compassY + sin(rad)*compassR}; arrow[1].color = sf::Color::Red; window.draw(arrow); sf::Text crsTxt(font, std::to_string((int)pkt.course) + " deg", 12); crsTxt.setPosition({compassX - 20, compassY + compassR + 5}); window.draw(crsTxt); }
            float textX = compassX + compassR + 40.0f; float curY = overlayY + 20.0f;
            sf::Text lblCall(font, pkt.src.empty() ? "-- WAITING --" : pkt.src, 32); lblCall.setPosition({textX, curY}); lblCall.setFillColor(sf::Color::Green); lblCall.setStyle(sf::Text::Bold); window.draw(lblCall);
            if (!pkt.dest.empty()) { sf::Text lblDest(font, "> " + pkt.dest, 20); lblDest.setPosition({textX + lblCall.getLocalBounds().size.x + 15, curY + 12}); lblDest.setFillColor(sf::Color(200,200,200)); window.draw(lblDest); }
            curY += 40.0f; if (!pkt.coords.empty()) { sf::Text lblGPS(font, "GPS: " + pkt.coords, 18); lblGPS.setPosition({textX, curY}); lblGPS.setFillColor(sf::Color::Cyan); window.draw(lblGPS); curY += 25.0f; }
            if (!pkt.comment.empty()) { std::string wrappedMsg = wrapText(pkt.comment, font, 16, layout.specW - 350.0f - textX); sf::Text lblMsg(font, wrappedMsg, 16); lblMsg.setPosition({textX, curY}); lblMsg.setFillColor(sf::Color::Yellow); window.draw(lblMsg); }
            float logX = layout.specW - 300; float logW = 300;
            sf::RectangleShape divLine({2, aprsH}); divLine.setPosition({logX, overlayY}); divLine.setFillColor(sf::Color::White); window.draw(divLine);
            sf::Text logTitle(font, "PACKET HISTORY", 12); logTitle.setPosition({logX + 10, overlayY + 5}); logTitle.setFillColor(sf::Color(150,150,150)); window.draw(logTitle);
            float listTopY = overlayY + 25.0f; int i=0; 
            for(const auto& log : logs) { 
                float yPos = listTopY + aprsLogScrollOffset + (i*16.0f);
                if (yPos > listTopY - 10 && yPos < layout.winH - 5) { std::string lShort = log; if(lShort.length()>35) lShort=lShort.substr(0,32)+"..."; sf::Text l(font, lShort, 12); l.setPosition({logX + 10, yPos}); l.setFillColor(sf::Color(200,200,200)); window.draw(l); }
                i++; 
            }
        }

        if (!isHw) timeSlider.draw(window); 
        sidebar.draw(window); 
        window.draw(topBar); freqVFO.draw(window); btnPlay.draw(window); if (isHw) btnTuningMode.draw(window); btnMute.draw(window); volSlider.draw(window);
        window.display();
    }
    dspRunning = false; if (dspThread.joinable()) dspThread.join();
    return 0;
}