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
#include "NativeDialogs.h"
#include "IQSources.h"
#include "APRS_Decoder.h"

// --- CONSTANTS ---
const int FFT_SIZE = 1024;
const int INTERNAL_WATERFALL_WIDTH = 1024;
const double AUDIO_RATE = 48000.0;

const int TOP_BAR_H = 60; 
const int SIDEBAR_W = 300; 

// --- TUNING CONFIG ---
const int TUNING_LATENCY_MS = 100; // Fixed latency to prevent USB stalling

// playback on 1.024msps is not smooth, disabled for now
const std::vector<uint32_t> RTL_RATES_VAL = {/*1024000,*/ 1400000, 1800000, 2048000, 2400000, 3200000};
const std::vector<uint32_t> SDRPLAY_RATES_VAL = {2000000, 4000000, 6000000, 8000000, 10000000};

const std::vector<std::string> STEP_NAMES = {"None", "1 kHz", "5 kHz", "6.25k", "10 kHz", "12.5k", "25 kHz", "100 kHz"};
const std::vector<long long> STEP_VALUES = {0, 1000, 5000, 6250, 10000, 12500, 25000, 100000};

// --- HELPER: TIMESTAMP ---
std::string getTimestamp() {
    auto now = std::time(nullptr);
    auto tm = *std::localtime(&now);
    std::ostringstream oss;
    oss << std::put_time(&tm, "[%Y-%m-%d %H:%M:%S]");
    return oss.str();
}

// --- HELPER: LOAD LOG HISTORY ---
std::deque<std::string> loadLastLogLines(const std::string& filename, int count) {
    std::deque<std::string> lines;
    std::ifstream file(filename);
    if (!file.is_open()) return lines;

    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty()) {
            lines.push_back(line);
            if (lines.size() > count) lines.pop_front();
        }
    }
    return lines;
}

// --- HELPER: TEXT WRAPPING ---
std::string wrapText(const std::string& str, const sf::Font& font, unsigned int charSize, float maxWidth) {
    std::string result;
    std::string currentLine;
    std::stringstream ss(str);
    std::string word;

    while (ss >> word) {
        sf::Text testWord(font, word, charSize);
        if (testWord.getLocalBounds().size.x > maxWidth) {
            if (!currentLine.empty()) { result += currentLine + "\n"; currentLine = ""; }
            std::string part;
            for (char c : word) {
                part += c;
                sf::Text partTest(font, part, charSize);
                if (partTest.getLocalBounds().size.x > maxWidth - 10) {
                    result += part + "\n";
                    part = "";
                }
            }
            currentLine = part;
            continue;
        }

        std::string testLine = currentLine + (currentLine.empty() ? "" : " ") + word;
        sf::Text testText(font, testLine, charSize);
        
        if (testText.getLocalBounds().size.x > maxWidth) {
            if (!currentLine.empty()) {
                result += currentLine + "\n";
                currentLine = word;
            } else {
                result += word + "\n";
            }
        } else {
            currentLine = testLine;
        }
    }
    result += currentLine;
    return result;
}

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
        sampleRate = sr; channels = ch; dataSize = 0; active = true;
        char header[44] = {0}; file.write(header, 44);
    }

    void write(const float* data, size_t count) {
        if (!active) return;
        for(size_t i=0; i<count; i++) {
            float s = std::clamp(data[i], -1.0f, 1.0f);
            int16_t val = static_cast<int16_t>(s * 32767.0f);
            file.write((char*)&val, sizeof(int16_t));
        }
        dataSize += count * sizeof(int16_t);
    }

    void stop() {
        if (!active || !file.is_open()) return;
        file.seekp(0);
        uint32_t fileSize = dataSize + 36;
        uint32_t byteRate = sampleRate * channels * 2;
        uint16_t blockAlign = channels * 2;
        file.write("RIFF", 4); file.write((char*)&fileSize, 4); file.write("WAVE", 4); file.write("fmt ", 4);
        uint32_t subchunk1Size = 16; uint16_t audioFormat = 1; uint16_t bitsPerSample = 16;
        file.write((char*)&subchunk1Size, 4); file.write((char*)&audioFormat, 2);
        file.write((char*)&channels, 2); file.write((char*)&sampleRate, 4);
        file.write((char*)&byteRate, 4); file.write((char*)&blockAlign, 2);
        file.write((char*)&bitsPerSample, 2); file.write("data", 4);
        file.write((char*)&dataSize, 4);
        file.close(); active = false;
    }
};

enum class RecMode { AUDIO, BASEBAND };

struct AprsLastPacket {
    std::string raw; 
    std::string src;
    std::string dest;
    std::string comment;
    std::string coords; 
    float course = -1.0f; 
    float speed = -1.0f;
};

struct SharedData {
    std::mutex mtx;
    double tunedFreqPercent = 0.5;
    
    // --- Safe Seek Request ---
    double pendingSeekRequest = -1.0;
    // -------------------------

    double bandwidth = 12500.0;
    long long centerFreq = 0;
    
    float volume = 1.0f;
    bool isMuted = false;
    float rfGain = -1.0f; 
    float squelchThreshold = -100.0f;
    
    bool stereoEnabled = false; 

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

    bool isRecording = false;
    RecMode recMode = RecMode::AUDIO;
    std::string recPath = ""; 
    std::string recStatus = "Idle";

    bool aprsEnabled = false;
    std::deque<std::string> aprsLog; 
    AprsLastPacket lastAprs;

    SharedData() : fftSpectrum(FFT_SIZE, -100.0), waterfallRow(INTERNAL_WATERFALL_WIDTH * 4, 0) {}
};

std::mutex sourceMtx;
std::shared_ptr<IQSource> currentSource;

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
    int divs = (int)(w / 100); 
    if (divs < 4) divs = 4;

    for (int i = 0; i <= divs; i++) {
        float xPos = x + (w/divs)*i;
        sf::RectangleShape line({1, h}); line.setPosition({xPos, y}); line.setFillColor(sf::Color(100, 100, 100, 150)); window.draw(line);
        double freqAtPoint = startFreq + (double)sr * ((double)i / divs);
        std::string freqStr = formatHz((long long)freqAtPoint);
        if(freqStr.size() > 4) freqStr = freqStr.substr(0, freqStr.size()-4); 
        sf::Text l(font, freqStr, 10);
        sf::FloatRect b = l.getLocalBounds();
        // SFML 3 fix
        l.setPosition({xPos - b.size.x/2, y + h - 15}); l.setFillColor(sf::Color::White);
        window.draw(l);
    }
}

void parseAprsData(std::string raw, AprsLastPacket& pkt) {
    pkt.raw = raw;
    std::string content = raw;
    if (raw.size() > 0 && raw[0] == '[') {
        size_t closeBracket = raw.find("] ");
        if (closeBracket != std::string::npos) content = raw.substr(closeBracket + 2);
    }

    size_t colon = content.find(':');
    if (colon == std::string::npos) return;

    std::string header = content.substr(0, colon);
    std::string body = content.substr(colon + 1);

    size_t arrow = header.find('>');
    if (arrow != std::string::npos) {
        pkt.src = header.substr(0, arrow);
        pkt.dest = header.substr(arrow + 1);
    } else {
        pkt.src = header;
        pkt.dest = "?";
    }
    pkt.comment = body;

    std::regex coordRegex(R"((\d{4}\.\d{2})([NS]).(\d{5}\.\d{2})([EW]))");
    std::smatch match;
    if (std::regex_search(body, match, coordRegex)) {
        float latVal = std::stof(match[1]) / 100.0f;
        float lonVal = std::stof(match[3]) / 100.0f;
        std::stringstream ss;
        ss << std::fixed << std::setprecision(2) << latVal << " " << match[2].str() << ", " << lonVal << " " << match[4].str();
        pkt.coords = ss.str();
    } else {
        pkt.coords = "";
    }

    std::regex courseRegex(R"((\d{3})/(\d{3}))");
    if (std::regex_search(body, match, courseRegex)) {
        try { pkt.course = std::stof(match[1]); pkt.speed = std::stof(match[2]); } catch(...) { pkt.course = -1; }
    } else { pkt.course = -1; }
}

void dspWorker(std::atomic<bool>& running, SharedData& shared, AudioSink& audio) {
    Demodulator demod(2000000, AUDIO_RATE); 
    APRSDecoder aprsDecoder(AUDIO_RATE); 
    
    aprsDecoder.onMessage = [&](std::string msg) {
        std::string ts = getTimestamp();
        std::string fullLog = ts + " " + msg;
        std::ofstream logFile("aprs.log", std::ios::app);
        if (logFile.is_open()) { logFile << fullLog << "\n"; logFile.close(); }
        std::lock_guard<std::mutex> l(shared.mtx);
        shared.aprsLog.push_back(fullLog);
        if (shared.aprsLog.size() > 500) shared.aprsLog.pop_front(); 
        parseAprsData(fullLog, shared.lastAprs);
    };

    double lastSampleRate = 0;
    std::vector<Complex> iqBuffer;
    std::vector<double> winFunc = makeWindow(FFT_SIZE);
    std::vector<double> localFftHistory(FFT_SIZE, -100.0);
    
    WavWriter recorder;
    float lastRfGain = -999.0f;

    while (running) {
        std::shared_ptr<IQSource> src = nullptr;
        { std::lock_guard<std::mutex> lock(sourceMtx); src = currentSource; }
        
        if (!src) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }

        double targetFreqPct, bw; 
        float vol, rfGainReq; bool muted;
        Mode mode; bool play, aprsActive;
        float minDb, maxDb;
        bool doRecord; RecMode rMode; std::string rPath;
        float sqThr;
        bool stereo;
        double seekReq = -1.0;
        
        {
            std::lock_guard<std::mutex> lock(shared.mtx);
            
            // --- Safe Seek Request ---
            seekReq = shared.pendingSeekRequest;
            shared.pendingSeekRequest = -1.0;
            // -------------------------

            float rawPct = shared.tunedFreqPercent;
            if (std::isnan(rawPct) || std::isinf(rawPct)) rawPct = 0.5f;
            rawPct = std::clamp(rawPct, 0.0f, 1.0f);
            targetFreqPct = rawPct;

            bw = shared.bandwidth;
            vol = shared.volume; muted = shared.isMuted;
            rfGainReq = shared.rfGain;
            mode = shared.mode; play = shared.isPlaying;
            minDb = shared.minDb; maxDb = shared.maxDb;
            doRecord = shared.isRecording; rMode = shared.recMode; rPath = shared.recPath;
            aprsActive = shared.aprsEnabled;
            sqThr = shared.squelchThreshold;
            stereo = shared.stereoEnabled;
        }

        // --- PERFORM SEEK IN DSP THREAD ---
        bool justSeeked = false;
        if (seekReq >= 0.0 && src->isSeekable()) {
            src->seek(seekReq);
            demod.clear(); // Reset filter states (prevents explosion)
            audio.clear(); // Clear old audio buffer
            justSeeked = true;
        }

        // Logic: if stopped, sleep unless we just seeked (we want to render 1 frame for preview)
        if (!play && !justSeeked) { 
            std::this_thread::sleep_for(std::chrono::milliseconds(50)); 
            continue; 
        }

        if (src->isHardware() && std::abs(rfGainReq - lastRfGain) > 0.1f) {
            src->setGain((int)rfGainReq); 
            lastRfGain = rfGainReq;
        }

        if (doRecord && !recorder.active) {
             long long currentCenterHz; { std::lock_guard<std::mutex> l(shared.mtx); currentCenterHz = shared.centerFreq; }
             char timeBuf[32]; std::time_t now = std::time(nullptr); std::strftime(timeBuf, sizeof(timeBuf), "%Y%m%d_%H%M%S", std::localtime(&now));
             std::string filename; std::string freqLabel;
             if (rMode == RecMode::AUDIO) {
                 double offset = (targetFreqPct - 0.5) * src->getSampleRate();
                 long long tunedHz = currentCenterHz + (long long)offset;
                 freqLabel = "_" + std::to_string(tunedHz / 1000) + "kHz";
                 filename = (rPath.empty() ? "." : rPath) + "/rec_" + std::string(timeBuf) + freqLabel + "_audio.wav";
                 recorder.start(filename, (int)AUDIO_RATE, 2);
             } else {
                 freqLabel = "_" + std::to_string(currentCenterHz) + "Hz";
                 filename = (rPath.empty() ? "." : rPath) + "/rec_" + std::string(timeBuf) + freqLabel + "_IQ.wav";
                 recorder.start(filename, (int)src->getSampleRate(), 2);
             }
             { std::lock_guard<std::mutex> l(shared.mtx); shared.recStatus = "REC: " + filename; }
        } else if (!doRecord && recorder.active) {
             recorder.stop();
             { std::lock_guard<std::mutex> l(shared.mtx); shared.recStatus = "Saved."; }
        }

        if (play && !src->isHardware() && !justSeeked) {
            size_t safeLevel = 9600; 
            while (audio.getBufferedCount() > safeLevel) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                if (!running) return;
            }
        }

        double sr = src->getSampleRate();
        if (sr != lastSampleRate) { demod = Demodulator(sr, AUDIO_RATE); lastSampleRate = sr; }

        int chunkSize = (int)sr / 60; 
        // Optimization: read less data if just previewing (paused)
        if (!play && justSeeked) chunkSize = FFT_SIZE * 2;

        if (chunkSize > 200000) chunkSize = 200000;
        if (iqBuffer.size() != chunkSize) iqBuffer.resize(chunkSize);

        int readCount = src->read(iqBuffer.data(), chunkSize);

        if (readCount > 0) {
            if (recorder.active && rMode == RecMode::BASEBAND) {
                std::vector<float> rawFloat(readCount * 2);
                for(int i=0; i<readCount; i++) { rawFloat[i*2] = (float)iqBuffer[i].real(); rawFloat[i*2+1] = (float)iqBuffer[i].imag(); }
                recorder.write(rawFloat.data(), rawFloat.size());
            }

            std::vector<Complex> fftData(FFT_SIZE);
            for (size_t i = 0; i < FFT_SIZE && i < readCount; i++) fftData[i] = iqBuffer[i] * winFunc[i];
            fft(fftData);

            int visualBin = (int)(targetFreqPct * FFT_SIZE); 
            if (visualBin < 0) visualBin = 0; if (visualBin >= FFT_SIZE) visualBin = FFT_SIZE - 1;
            int shiftedIdx = (visualBin + FFT_SIZE / 2) % FFT_SIZE;
            
            float signalMag = std::abs(fftData[shiftedIdx]) / FFT_SIZE;
            int left = (shiftedIdx - 1 + FFT_SIZE) % FFT_SIZE;
            int right = (shiftedIdx + 1) % FFT_SIZE;
            signalMag += (std::abs(fftData[left]) + std::abs(fftData[right])) / FFT_SIZE * 0.5f;
            float rssiDb = 20.0f * std::log10(signalMag + 1e-12f);

            // --- AUDIO & DEMODULATION (Only if PLAYING) ---
            if (play) {
                double freqOffset = (targetFreqPct - 0.5) * sr;
                if (mode == Mode::USB) freqOffset += bw / 2.0;
                if (mode == Mode::LSB) freqOffset -= bw / 2.0;

                std::vector<Complex> chunkToProcess(iqBuffer.begin(), iqBuffer.begin() + readCount);
                auto audioData = demod.process(chunkToProcess, freqOffset, bw, mode, stereo);
                
                if (aprsActive) {
                    std::vector<float> mono; mono.reserve(audioData.size()/2);
                    for(size_t i=0; i<audioData.size(); i+=2) mono.push_back(audioData[i]);
                    aprsDecoder.process(mono);
                }

                if (rssiDb < sqThr) {
                    std::fill(audioData.begin(), audioData.end(), 0.0f);
                }

                float finalVol = muted ? 0.0f : vol;
                for (auto& s : audioData) s *= finalVol;
                audio.pushSamples(audioData);

                if (recorder.active && rMode == RecMode::AUDIO) recorder.write(audioData.data(), audioData.size());
            }

            // --- WATERFALL & SPECTRUM (Always update, even on preview) ---
            std::vector<uint8_t> tempRow(INTERNAL_WATERFALL_WIDTH * 4);
            for (int x = 0; x < INTERNAL_WATERFALL_WIDTH; x++) {
                int fftIdx = (int)((float)x / INTERNAL_WATERFALL_WIDTH * FFT_SIZE);
                int sIdx = (fftIdx + FFT_SIZE / 2) % FFT_SIZE;
                float rawMag = std::abs(fftData[sIdx]) / FFT_SIZE;
                float rawDb = 20 * std::log10(rawMag + 1e-12);
                float norm = (rawDb - minDb) / (maxDb - minDb); 
                sf::Color c = getHeatmap(norm);
                int px = x * 4; tempRow[px] = c.r; tempRow[px + 1] = c.g; tempRow[px + 2] = c.b; tempRow[px + 3] = 255;
            }

            // Fix for instant preview: if paused, alpha = 1.0 (instant update), else 0.3 (smooth)
            float alpha = (play) ? 0.3f : 1.0f;
            for (int i = 0; i < FFT_SIZE; i++) {
                 int idx = (i + FFT_SIZE / 2) % FFT_SIZE;
                 float mag = std::abs(fftData[idx]) / FFT_SIZE;
                 float db = 20 * std::log10(mag + 1e-12);
                 localFftHistory[i] = localFftHistory[i] * (1.0f - alpha) + db * alpha;
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

struct LayoutState {
    float winW, winH;
    float sidebarX;
    float specW;
    float specH;
    float waterfallH;
    float timelineH;
};

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
    
    // --- LOAD LAST LOGS ---
    sharedData.aprsLog = loadLastLogLines("aprs.log", 10);

    std::atomic<bool> dspRunning {true};
    std::thread dspThread(dspWorker, std::ref(dspRunning), std::ref(sharedData), std::ref(audio));

    sf::RenderWindow window(sf::VideoMode({1200, 800}), "OrbitSDR");
    window.setFramerateLimit(60);

    auto cursorArrow = sf::Cursor::createFromSystem(sf::Cursor::Type::Arrow);
    auto cursorHand = sf::Cursor::createFromSystem(sf::Cursor::Type::Hand);
    sf::Font font; if (!font.openFromFile("/System/Library/Fonts/Helvetica.ttc") && !font.openFromFile("C:/Windows/Fonts/arial.ttf") && !font.openFromFile("arial.ttf")) {}

    sf::RectangleShape topBar; topBar.setFillColor(sf::Color(30, 30, 30));
    
    FrequencyDisplay freqVFO(20, 8, font); freqVFO.setFrequency(100000000);
    SdrButton btnTuningMode(0, 0, 40, 40, "FIX", font); btnTuningMode.setColor(sf::Color(80, 80, 80)); bool stickyCenterMode = false;
    SdrButton btnPlay(0, 0, 40, 40, ">", font); btnPlay.setColor(sf::Color(116, 57, 57)); bool isPlaying = false; 
    SdrButton btnMute(0, 0, 40, 40, "M", font); bool isMuted = false;
    Slider volSlider(0, 0, 170, 0.0f, 1.0f, 0.5f, "Volume", font);

    sf::Text labelSource(font, "Source:", 12);
    Dropdown sourceDropdown(0, 0, 160, 25, font); sourceDropdown.setOptions({"File (Baseband)", "RTL-SDR", "SDRPlay"});
    sf::Text labelRate(font, "Rate:", 12);
    Dropdown rateDropdown(0, 0, 160, 25, font); rateDropdown.setOptions({"None"}); 
    sf::Text labelAudio(font, "Audio:", 12);
    Dropdown audioDropdown(0, 0, 160, 25, font); audioDropdown.setOptions(deviceNames);

    Slider rfGainSlider(0, 0, 160, 0.0f, 50.0f, 0.0f, "RF Gain (dB)", font);
    SdrButton btnAgc(0, 0, 30, 30, "A", font); btnAgc.setActive(true); bool agcEnabled = true;
    Slider bwSlider(0, 0, 200, 1000.0f, 220000.0f, 12500.0f, "Filter BW", font);
    Slider minDbSlider(0, 0, 200, -120.0f, -20.0f, -90.0f, "Min dB", font);
    Slider maxDbSlider(0, 0, 200, -40.0f, 40.0f, 0.0f, "Max dB", font);
    Slider squelchSlider(0, 0, 200, -100.0f, 0.0f, -100.0f, "Squelch (Carrier)", font);
    
    sf::Text labelSnap(font, "Step:", 12);
    Dropdown stepDropdown(0, 0, 100, 25, font); stepDropdown.setOptions(STEP_NAMES); stepDropdown.setSelection(5);
    
    Checkbox stereoCheck(0, 0, "Stereo WFM", font);

    sf::Text txtAnalog(font, "ANALOG", 14); txtAnalog.setFillColor(sf::Color(150, 150, 150));
    sf::RectangleShape lineAnalog({200.0f, 1.0f}); lineAnalog.setFillColor(sf::Color(100, 100, 100));
    SdrButton btnNFM(0, 0, 45, 30, "NFM", font); btnNFM.setActive(true); 
    SdrButton btnAM(0, 0, 45, 30, "AM", font);
    SdrButton btnWFM(0, 0, 45, 30, "WFM", font);
    SdrButton btnOFF(0, 0, 45, 30, "OFF", font);
    SdrButton btnLSB(0, 0, 95, 30, "LSB", font);
    SdrButton btnUSB(0, 0, 95, 30, "USB", font);

    sf::Text txtDigital(font, "DIGITAL", 14); txtDigital.setFillColor(sf::Color(150, 150, 150));
    sf::RectangleShape lineDigital({200.0f, 1.0f}); lineDigital.setFillColor(sf::Color(100, 100, 100));
    SdrButton btnAPRS(0, 0, 200, 30, "APRS", font);

    sf::RectangleShape recPanel({260, 130}); recPanel.setFillColor(sf::Color(40,40,40)); recPanel.setOutlineColor(sf::Color::White); recPanel.setOutlineThickness(1);
    sf::Text labelRec(font, "Recording", 14);
    SdrButton btnRecAudio(0, 0, 80, 25, "Audio", font); btnRecAudio.setActive(true);
    SdrButton btnRecIQ(0, 0, 80, 25, "Baseband", font); 
    sf::Text pathText(font, "Path: ./ (Default)", 10); pathText.setFillColor(sf::Color::Cyan);
    SdrButton btnSelectFolder(0, 0, 100, 25, "Set Folder", font);
    SdrButton btnRecStart(0, 0, 60, 35, "REC", font); 
    RecMode currentRecMode = RecMode::AUDIO; std::string currentRecPath = "";

    Slider timeSlider(0, 0, 100, 0.0f, 1.0f, 0.0f, "Timeline", font);

    // --- APRS UI ELEMENTS ---
    SdrButton btnCopyCur(0, 0, 90, 25, "COPY MSG", font);
    SdrButton btnCopyHist(0, 0, 90, 25, "COPY LOG", font);
    
    // Feedback Timer for copy buttons
    sf::Clock feedbackClock;
    bool showCopyMsgFeedback = false;
    bool showCopyLogFeedback = false;

    std::vector<std::uint8_t> waterfall(INTERNAL_WATERFALL_WIDTH * 2048 * 4, 0); 
    sf::Texture wTex; 
    if (!wTex.resize({INTERNAL_WATERFALL_WIDTH, 2048})) return 1; 
    sf::Sprite wSpr(wTex); 
    
    LayoutState layout;
    sf::Vector2u lastSize = window.getSize();
    
    float sidebarContentHeight = 900.0f; 
    float scrollPercent = 0.0f;
    float scrollOffset = 0.0f;
    bool isScrolling = false;

    float aprsLogScrollOffset = 0.0f;

    auto updateLayout = [&](int w, int h) {
        layout.winW = (float)w; layout.winH = (float)h;
        layout.sidebarX = layout.winW - SIDEBAR_W + 20; 
        layout.specW = layout.winW - SIDEBAR_W;
        layout.specH = 250.0f;
        layout.timelineH = 40.0f;
        layout.waterfallH = layout.winH - TOP_BAR_H - layout.specH - layout.timelineH;
        if (layout.waterfallH < 100) layout.waterfallH = 100;

        topBar.setSize({layout.winW, (float)TOP_BAR_H});
        freqVFO.setPosition(20, 8);
        btnTuningMode.setPosition(320, 10);
        btnPlay.setPosition(370, 10);
        
        labelSource.setPosition({430, 10}); sourceDropdown.setPosition(430, 25);
        labelRate.setPosition({600, 10}); rateDropdown.setPosition(600, 25);
        labelAudio.setPosition({770, 10}); audioDropdown.setPosition(770, 25);
        
        float volX = layout.winW - 250; 
        btnMute.setPosition(volX + 180, 10);
        volSlider.setPosition(volX, 25);

        // --- SIDEBAR SCROLL LOGIC ---
        float sidebarVisibleH = layout.winH - TOP_BAR_H;
        float maxScroll = std::max(0.0f, sidebarContentHeight - sidebarVisibleH + 50.0f);
        scrollOffset = scrollPercent * maxScroll;

        float px = layout.sidebarX;
        float startY = TOP_BAR_H + 30;
        float sy = startY - scrollOffset; 

        rfGainSlider.setPosition(px, sy); btnAgc.setPosition(px + 170, sy - 5); sy += 50;
        bwSlider.setPosition(px, sy); sy += 50;
        minDbSlider.setPosition(px, sy); sy += 50;
        maxDbSlider.setPosition(px, sy); sy += 50;
        squelchSlider.setPosition(px, sy); sy += 50;
        
        labelSnap.setPosition({px, sy}); 
        stepDropdown.setPosition(px + 40, sy - 5); 
        stereoCheck.setPosition(px + 150, sy - 3);
        sy += 35;
        
        txtAnalog.setPosition({px, sy}); lineAnalog.setPosition({px, sy+20}); sy += 30;
        btnNFM.setPosition(px, sy); btnAM.setPosition(px+50, sy); btnWFM.setPosition(px+100, sy); btnOFF.setPosition(px+150, sy); sy += 40;
        btnLSB.setPosition(px, sy); btnUSB.setPosition(px+100, sy); sy += 45;

        txtDigital.setPosition({px, sy}); lineDigital.setPosition({px, sy+20}); sy += 30;
        btnAPRS.setPosition(px, sy); sy += 50;

        recPanel.setPosition({px - 10, sy}); labelRec.setPosition({px, sy+5}); sy += 30;
        btnRecAudio.setPosition(px, sy); btnRecIQ.setPosition(px+90, sy); sy += 35;
        pathText.setPosition({px, sy}); sy += 20;
        btnSelectFolder.setPosition(px, sy); btnRecStart.setPosition(px+120, sy-5);

        timeSlider.setPosition(20, layout.winH - 30);
        timeSlider.setWidth(layout.winW - 40);

        wSpr.setPosition({0, TOP_BAR_H + layout.specH});
        float scaleX = layout.specW / (float)INTERNAL_WATERFALL_WIDTH;
        wSpr.setScale({scaleX, 1.0f}); 
        wSpr.setTextureRect(sf::IntRect({0, 0}, {INTERNAL_WATERFALL_WIDTH, (int)layout.waterfallH})); 
    };

    updateLayout(window.getSize().x, window.getSize().y);

    long long currentCenterFreq = 0;
    long long pendingCenterFreq = 0;
    sf::Clock debouncer;
    sf::Clock interactionCooldown;

    auto resetBtns = [&](SdrButton* active) {
        btnNFM.setActive(false); btnAM.setActive(false); btnWFM.setActive(false); 
        btnOFF.setActive(false); btnLSB.setActive(false); btnUSB.setActive(false); btnAPRS.setActive(false); 
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

        bool success = false;

        if (sourceIdx == 0) { 
            newSource = std::make_shared<FileSource>(); 
            std::string path = pathOverride.empty() ? "None" : pathOverride;
            { 
                std::lock_guard<std::mutex> lock(sharedData.mtx);
                sharedData.currentFilename = path;
                size_t lastSlash = path.find_last_of("/\\");
                std::string fn = (lastSlash == std::string::npos) ? path : path.substr(lastSlash + 1);
                sharedData.currentFilename = fn;
                long long parsedFreq = 0; bool foundFreq = false;
                size_t hzPos = fn.find("Hz");
                if (hzPos != std::string::npos) {
                    size_t underscorePos = fn.rfind('_', hzPos);
                    if (underscorePos != std::string::npos) {
                        try { parsedFreq = std::stoll(fn.substr(underscorePos + 1, hzPos - underscorePos - 1)); foundFreq = true; } catch (...) {}
                    }
                }
                if (foundFreq) { currentCenterFreq = parsedFreq; freqVFO.setFrequency(parsedFreq); } 
                else { currentCenterFreq = 0; freqVFO.setFrequency(0); }
            }
            success = newSource->open(path);
            rateDropdown.setOptions({sharedData.currentFilename});
        } else if (sourceIdx == 1) { 
            // --- FIX RTL-SDR SYNC ---
            newSource = std::make_shared<RtlSdrSource>();
            rateDropdown.setOptions(newSource->getAvailableSampleRatesText()); rateDropdown.setSelection(rateIdx);
            success = newSource->open("0", targetRate);
            
            // Hard Reset Tuner Position & Frequency
            long long startFreq = 97800000; // Start at 97.8 MHz
            freqVFO.setFrequency(startFreq);
            currentCenterFreq = startFreq;
            newSource->setCenterFrequency(startFreq);
            { 
                std::lock_guard<std::mutex> l(sharedData.mtx); 
                sharedData.tunedFreqPercent = 0.5; 
                sharedData.centerFreq = startFreq;
            }
            // ------------------------
            
            if (!success) { 
                sourceDropdown.selectedIndex = 0; sourceDropdown.selectedText.setString("File (Baseband)"); 
                newSource = std::make_shared<FileSource>(); newSource->open("None"); 
            }
        } else if (sourceIdx == 2) { 
            // --- SDRPLAY SYNC ---
            newSource = std::make_shared<SdrPlaySource>();
            rateDropdown.setOptions(newSource->getAvailableSampleRatesText()); 
            rateDropdown.setSelection(rateIdx);
            success = newSource->open("", targetRate);
            
            // Hard Reset Tuner Position & Frequency
            long long startFreq = 97800000; // Start at 97.8 MHz
            freqVFO.setFrequency(startFreq);
            currentCenterFreq = startFreq;
            newSource->setCenterFrequency(startFreq);
            { 
                std::lock_guard<std::mutex> l(sharedData.mtx); 
                sharedData.tunedFreqPercent = 0.5; 
                sharedData.centerFreq = startFreq;
            }
            // ---------------------------
        }

        { std::lock_guard<std::mutex> lock(sourceMtx); currentSource = newSource; }
        { std::lock_guard<std::mutex> lock(sharedData.mtx); sharedData.isPlaying = false; sharedData.isRecording = false; }
        isPlaying = false; btnRecStart.setText("REC");
        audio.stop(); btnPlay.setText(">"); btnPlay.setColor(sf::Color(116, 57, 57)); audio.clear(); std::fill(waterfall.begin(), waterfall.end(), 0);
    };

    bool isDraggingScale = false; // --- DRAGGING FLAG FOR PANNING ---
    bool isDraggingTuner = false;
    bool isSpectrumDragging = false; 
    float lastDragX = 0.0f;
    Mode lastMode = Mode::NFM;

    // --- HELPER FOR TUNING LOGIC ---
    auto applySpectrumTuning = [&](float mouseX) {
        bool isHw = false; double hwSampleRate = 2e6;
        { std::lock_guard<std::mutex> l(sourceMtx); if (currentSource) { isHw = currentSource->isHardware(); hwSampleRate = currentSource->getSampleRate(); } }

        double clickPct = mouseX / layout.specW;
        double offsetHz = (clickPct - 0.5) * hwSampleRate;
        long long clickedFreq = currentCenterFreq + (long long)offsetHz;
        
        long long step = STEP_VALUES[stepDropdown.selectedIndex];
        if (step > 0) {
            clickedFreq = (long long)std::round((double)clickedFreq / step) * step;
            double newOffset = (double)(clickedFreq - currentCenterFreq);
            clickPct = 0.5 + (newOffset / hwSampleRate);
        }

        // --- CLAMP PROTECTION: Ensure tuner stays within view ---
        clickPct = std::clamp(clickPct, 0.0, 1.0);

        if (stickyCenterMode && isHw) { 
            pendingCenterFreq = clickedFreq; debouncer.restart(); 
            { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.tunedFreqPercent = 0.5; } 
            freqVFO.setFrequency(clickedFreq); 
        } 
        else { 
            { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.tunedFreqPercent = clickPct; } 
            freqVFO.setFrequency(clickedFreq); 
        }
    };


    while (window.isOpen()) {
        
        sf::Vector2u currSize = window.getSize();
        if (currSize != lastSize && currSize.x > 0 && currSize.y > 0) {
            sf::FloatRect visibleArea({0.f, 0.f}, {(float)currSize.x, (float)currSize.y});
            window.setView(sf::View(visibleArea));
            updateLayout(currSize.x, currSize.y);
            lastSize = currSize;
        }

        {
            std::lock_guard<std::mutex> lock(sharedData.mtx);
            sharedData.bandwidth = bwSlider.currentVal;
            sharedData.squelchThreshold = squelchSlider.currentVal;
            sharedData.minDb = minDbSlider.currentVal; sharedData.maxDb = maxDbSlider.currentVal;
            sharedData.volume = volSlider.currentVal;
            sharedData.rfGain = agcEnabled ? -1.0f : rfGainSlider.currentVal;
            sharedData.isMuted = isMuted;
            sharedData.recMode = currentRecMode; sharedData.recPath = currentRecPath; sharedData.centerFreq = currentCenterFreq;
            sharedData.stereoEnabled = stereoCheck.checked;
            
            if (sharedData.isRecording) { pathText.setString(sharedData.recStatus); pathText.setFillColor(sf::Color::Red); } 
            else if (sharedData.recStatus != "Idle") { pathText.setString(sharedData.recStatus); pathText.setFillColor(sf::Color::Green); }
        }
        
        // --- Deklaracja 'prog' ---
        bool isHw = false; double hwSampleRate = 2000000.0; double prog = 0.0; 
        { 
            std::lock_guard<std::mutex> l(sourceMtx); 
            if (currentSource) { 
                isHw = currentSource->isHardware(); 
                hwSampleRate = currentSource->getSampleRate(); 
                prog = currentSource->getProgress(); // Tu pobieramy postęp odtwarzania
            } 
        }
        // -----------------------------------

        if (isHw) { labelRate.setString("Rate:"); freqVFO.setEnabled(true); } else { labelRate.setString("File:"); freqVFO.setEnabled(false); }

        if (!sourceDropdown.isOpen && !rateDropdown.isOpen && !audioDropdown.isOpen && !stepDropdown.isOpen) freqVFO.update(window);

        while (const std::optional<sf::Event> ev = window.pollEvent()) {
            if (ev->is<sf::Event::Closed>()) window.close();
            
            // --- SCROLLING LOGIC (Sidebar & VFO & Spectrum) ---
            if (const auto* scroll = ev->getIf<sf::Event::MouseWheelScrolled>()) {
                 sf::Vector2f m = window.mapPixelToCoords(sf::Vector2i((int)scroll->position.x, (int)scroll->position.y));
                 
                 // Sidebar Scroll
                 if (m.x > layout.sidebarX - 20) {
                     float delta = scroll->delta * -0.05f; 
                     scrollPercent += delta;
                     scrollPercent = std::clamp(scrollPercent, 0.0f, 1.0f);
                     updateLayout(window.getSize().x, window.getSize().y);
                 }
                 // APRS Log Scroll
                 else if (sharedData.aprsEnabled && m.y > (layout.winH - 200)) {
                     aprsLogScrollOffset += scroll->delta * 20.0f;
                     if (aprsLogScrollOffset > 0.0f) aprsLogScrollOffset = 0.0f;
                 }
                 // --- FIX: SCROLL WHEEL TUNING WITH SNAPPING ---
                 else if (m.y > TOP_BAR_H && m.x < layout.specW) {
                     long long step = STEP_VALUES[stepDropdown.selectedIndex];
                     if (step == 0) step = 100; 
                     
                     long long current = freqVFO.getFrequency();
                     // Calculate raw next step
                     long long rawNext = current + (long long)(scroll->delta * step);
                     
                     // Snap to grid logic
                     if (step > 0) {
                        long long remainder = rawNext % step;
                        if (remainder > step / 2) rawNext += (step - remainder);
                        else rawNext -= remainder;
                     }

                     if (rawNext < 0) rawNext = 0;
                     
                     freqVFO.setFrequency(rawNext);
                     
                     // Sync internal tuner position
                     if (stickyCenterMode && isHw) {
                         pendingCenterFreq = rawNext; debouncer.restart();
                         { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.tunedFreqPercent = 0.5; }
                     } else {
                         // --- SMART TUNER UPDATE (SCROLL) ---
                         // Tuner stays on the same ABSOLUTE frequency, but relative percent changes
                         double newOffset = (double)(rawNext - currentCenterFreq);
                         double clickPct = 0.5 + (newOffset / hwSampleRate);
                         
                         // --- FIX: CLAMP TUNER TO SCREEN ---
                         if (clickPct < 0.0) clickPct = 0.0;
                         if (clickPct > 1.0) clickPct = 1.0;
                         // -----------------------------------
                         
                         { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.tunedFreqPercent = clickPct; }
                     }
                 }
            }
            
            float scrollTrackH = layout.winH - TOP_BAR_H;
            float thumbH = 50.0f; 
            
            if (const auto* mb = ev->getIf<sf::Event::MouseButtonPressed>()) {
                if (mb->button == sf::Mouse::Button::Left) {
                    sf::Vector2f m = window.mapPixelToCoords(sf::Mouse::getPosition(window));
                    if (m.x >= layout.winW - 12 && m.y >= TOP_BAR_H) {
                        isScrolling = true;
                    }
                    
                    // --- INTERACTION WITH APRS DASHBOARD ---
                    if (sharedData.aprsEnabled) {
                        float overlayH = 250.0f; float overlayY = layout.winH - overlayH;
                        float dashWidth = layout.winW * 0.6f;
                        float logW = layout.winW - dashWidth;
                        
                        if (btnCopyCur.isClicked(*ev, window)) {
                            std::string txt; { std::lock_guard<std::mutex> l(sharedData.mtx); txt = sharedData.lastAprs.raw; }
                            if (!txt.empty()) { sf::Clipboard::setString(txt); showCopyMsgFeedback = true; feedbackClock.restart(); }
                        }
                        if (btnCopyHist.isClicked(*ev, window)) {
                            std::string allLog;
                            { std::lock_guard<std::mutex> l(sharedData.mtx); for(const auto& s : sharedData.aprsLog) allLog += s + "\n"; }
                            if (!allLog.empty()) { sf::Clipboard::setString(allLog); showCopyLogFeedback = true; feedbackClock.restart(); }
                        }

                        if (m.x > dashWidth && m.y > overlayY + 30 && m.y < layout.winH - 10) {
                            float logListY = overlayY + 30.0f + aprsLogScrollOffset;
                            float relativeY = m.y - logListY;
                            if (relativeY >= 0) {
                                int clickedIndex = (int)(relativeY / 18.0f); 
                                std::lock_guard<std::mutex> l(sharedData.mtx);
                                if (clickedIndex < sharedData.aprsLog.size()) {
                                    parseAprsData(sharedData.aprsLog[clickedIndex], sharedData.lastAprs);
                                }
                            }
                        }
                    }
                }
            }
            if (const auto* mr = ev->getIf<sf::Event::MouseButtonReleased>()) {
                if (mr->button == sf::Mouse::Button::Left) {
                    isScrolling = false;
                    isSpectrumDragging = false; 
                    isDraggingScale = false; // Stop axis dragging
                }
            }
            if (isScrolling) {
                sf::Vector2f m = window.mapPixelToCoords(sf::Mouse::getPosition(window));
                float relY = m.y - TOP_BAR_H;
                scrollPercent = relY / (scrollTrackH - thumbH);
                scrollPercent = std::clamp(scrollPercent, 0.0f, 1.0f);
                updateLayout(window.getSize().x, window.getSize().y);
            }

            // --- MOUSE MOVE & DRAGGING LOGIC ---
            if (const auto* mm = ev->getIf<sf::Event::MouseMoved>()) {
                sf::Vector2f m = window.mapPixelToCoords(mm->position);
                float graphY = m.y - TOP_BAR_H;
                
                // Show/Hide Cursor Line
                bool overAprs = (sharedData.aprsEnabled && m.y > (layout.winH - 250));
                if (!overAprs && m.x >= 0 && m.x < layout.specW && graphY >= 0 && graphY < layout.specH + layout.waterfallH) { 
                    std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.mouseX_spectrum = m.x; sharedData.mouseY_spectrum = graphY; 
                } else { 
                    std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.mouseX_spectrum = -1.0f; 
                }

                if (isSpectrumDragging) {
                    applySpectrumTuning(m.x);
                }

                // --- FIX: PANNING (AXIS DRAGGING) ---
                if (isDraggingScale) {
                    float dx = lastDragX - m.x;
                    lastDragX = m.x;
                    double hzPerPx = hwSampleRate / layout.specW;
                    long long shift = (long long)(dx * hzPerPx);
                    
                    long long nextCenter = currentCenterFreq + shift;
                    if (nextCenter < 0) nextCenter = 0;
                    
                    // --- LIVE UPDATE LOGIC (Decoupled UI and Hardware) ---
                    
                    // 1. Update Visuals IMMEDIATELY (Smooth UI)
                    currentCenterFreq = nextCenter;
                    { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.centerFreq = nextCenter; }

                    // --- FIX: STATIC TUNER ON SCREEN ---
                    // We do NOT update tunedFreqPercent. The tuner line stays physically where it is.
                    // But since Center Frequency changed, the VFO (what we are listening to) has changed.
                    double currentPct = 0.0;
                    { std::lock_guard<std::mutex> l(sharedData.mtx); currentPct = sharedData.tunedFreqPercent; }
                    
                    double offset = (currentPct - 0.5) * hwSampleRate;
                    long long vfoFreq = nextCenter + (long long)offset;
                    freqVFO.setFrequency(vfoFreq);

                    // 2. Update Hardware WITH THROTTLE (Smooth Audio)
                    if (debouncer.getElapsedTime().asMilliseconds() > TUNING_LATENCY_MS) {
                         { std::lock_guard<std::mutex> l(sourceMtx); if (currentSource) currentSource->setCenterFrequency(nextCenter); }
                         debouncer.restart();
                    }
                }
            }

            if (interactionCooldown.getElapsedTime().asMilliseconds() < 500) continue;

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
                            interactionCooldown.restart();
                        }
                      }
                }
            }
            if (!fileDialogTriggered && isHw) {
                if (rateDropdown.handleEvent(*ev, window)) changeSource(sourceDropdown.selectedIndex, rateDropdown.selectedIndex);
            }
            if (audioDropdown.handleEvent(*ev, window)) { audio.stop(); audio.initDevice(audioDropdown.selectedIndex, (int)AUDIO_RATE); if (isPlaying) audio.start(); }
            
            bool stepWasOpen = stepDropdown.isOpen;
            stepDropdown.handleEvent(*ev, window);

            if (window.hasFocus() && interactionCooldown.getElapsedTime().asMilliseconds() > 500) {
                if (!sourceDropdown.isOpen && !rateDropdown.isOpen && !audioDropdown.isOpen && !stepDropdown.isOpen && !stepWasOpen) {
                    
                    sf::Vector2f mPos = window.mapPixelToCoords(sf::Mouse::getPosition(window));
                    bool underTopBar = (mPos.y < TOP_BAR_H);

                    volSlider.handleEvent(*ev, window);
                    if (btnMute.isClicked(*ev, window)) { isMuted = !isMuted; btnMute.setColor(isMuted ? sf::Color(116, 57, 57) : sf::Color(60, 60, 60)); }
                    
                    if (isHw && btnTuningMode.isClicked(*ev, window)) { 
                        stickyCenterMode = !stickyCenterMode; 
                        if(stickyCenterMode) { 
                            btnTuningMode.setText("CTR"); 
                            btnTuningMode.setColor(sf::Color(0,100,200)); 
                            { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.tunedFreqPercent = 0.5; }
                            pendingCenterFreq = freqVFO.getFrequency(); 
                            debouncer.restart();
                        } else { 
                            btnTuningMode.setText("FIX"); 
                            btnTuningMode.setColor(sf::Color(80,80,80)); 
                        } 
                    }

                    if (!underTopBar) {
                        rfGainSlider.handleEvent(*ev, window);
                        bwSlider.handleEvent(*ev, window); minDbSlider.handleEvent(*ev, window); maxDbSlider.handleEvent(*ev, window);
                        squelchSlider.handleEvent(*ev, window);
                        
                        Mode currentModeChk; { std::lock_guard<std::mutex> l(sharedData.mtx); currentModeChk = sharedData.mode; }
                        if (currentModeChk == Mode::WFM) {
                            stereoCheck.isClicked(*ev, window);
                        }
                        
                        if (btnAgc.isClicked(*ev, window)) { agcEnabled = !agcEnabled; btnAgc.setActive(agcEnabled); }
                        if (btnRecAudio.isClicked(*ev, window)) { currentRecMode = RecMode::AUDIO; btnRecAudio.setActive(true); btnRecIQ.setActive(false); }
                        if (btnRecIQ.isClicked(*ev, window)) { currentRecMode = RecMode::BASEBAND; btnRecAudio.setActive(false); btnRecIQ.setActive(true); }
                        if (btnSelectFolder.isClicked(*ev, window)) { std::string folder = selectFolderDialog(); if (!folder.empty()) currentRecPath = folder; interactionCooldown.restart(); }
                        if (btnRecStart.isClicked(*ev, window)) { bool s; { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.isRecording = !sharedData.isRecording; s = sharedData.isRecording; } if (s) { btnRecStart.setText("STOP"); } else { btnRecStart.setText("REC"); } }
                        if (btnNFM.isClicked(*ev, window)) { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.mode = Mode::NFM; sharedData.aprsEnabled = false; resetBtns(&btnNFM); bwSlider.currentVal = 12500; bwSlider.updateHandlePos(); }
                        if (btnAM.isClicked(*ev, window))  { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.mode = Mode::AM; sharedData.aprsEnabled = false; resetBtns(&btnAM); bwSlider.currentVal = 8000; bwSlider.updateHandlePos(); }
                        if (btnWFM.isClicked(*ev, window)) { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.mode = Mode::WFM; sharedData.aprsEnabled = false; resetBtns(&btnWFM); bwSlider.currentVal = 180000; bwSlider.updateHandlePos(); }
                        if (btnOFF.isClicked(*ev, window)) { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.mode = Mode::OFF; sharedData.aprsEnabled = false; resetBtns(&btnOFF); }
                        if (btnLSB.isClicked(*ev, window)) { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.mode = Mode::LSB; sharedData.aprsEnabled = false; resetBtns(&btnLSB); bwSlider.currentVal = 3000; bwSlider.updateHandlePos(); }
                        if (btnUSB.isClicked(*ev, window)) { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.mode = Mode::USB; sharedData.aprsEnabled = false; resetBtns(&btnUSB); bwSlider.currentVal = 3000; bwSlider.updateHandlePos(); }
                        if (btnAPRS.isClicked(*ev, window)) { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.mode = Mode::NFM; sharedData.aprsEnabled = true; resetBtns(&btnAPRS); bwSlider.currentVal = 12500; bwSlider.updateHandlePos(); }
                    }

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

                    if (!isHw) {
                         // --- OPTIMIZED SEEKING LOGIC (Fixed) ---
                         timeSlider.handleEvent(*ev, window);
                         timeSlider.update(window); 
                         if (timeSlider.isDragging) { 
                             std::lock_guard<std::mutex> l(sharedData.mtx);
                             sharedData.pendingSeekRequest = timeSlider.currentVal;
                         } else { 
                             timeSlider.currentVal = prog; 
                             timeSlider.updateHandlePos(); 
                         } 
                    }

                    if (btnPlay.isClicked(*ev, window)) {
                        bool s; { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.isPlaying = !sharedData.isPlaying; s = sharedData.isPlaying; }
                        if (s) { btnPlay.setText("||"); btnPlay.setColor(sf::Color(78, 78, 236)); audio.start(); { std::lock_guard<std::mutex> l(sourceMtx); if (currentSource) currentSource->start(); } } 
                        else { btnPlay.setText(">"); btnPlay.setColor(sf::Color(116, 57, 57)); audio.stop(); { std::lock_guard<std::mutex> l(sourceMtx); if (currentSource) currentSource->stop(); } }
                        isPlaying = s;
                    }

                    sf::Vector2f m_fs = window.mapPixelToCoords(sf::Mouse::getPosition(window));
                    float axisY_FreqScale = (float)TOP_BAR_H + layout.specH;
                    bool inFreqScaleZone = (m_fs.y >= axisY_FreqScale - 10 && m_fs.y <= axisY_FreqScale + 20 && m_fs.x < layout.specW);
                    
                    if (const auto* mb = ev->getIf<sf::Event::MouseButtonPressed>()) {
                        if (mb->button == sf::Mouse::Button::Left) {
                            sf::Vector2f m = window.mapPixelToCoords(sf::Mouse::getPosition(window));
                            float graphY = m.y - TOP_BAR_H;
                            
                            // PROTECTION: Don't tune if mouse is over APRS panel OR over Timeline Slider
                            bool overAprs = (sharedData.aprsEnabled && m.y > (layout.winH - 250));
                            // --- FIX: Prevent Tuner jump when using Timeline Slider ---
                            bool overTimeline = (timeSlider.isDragging || (m.y > layout.winH - 40)); 

                            // --- LOGIC SPLIT: Panning vs Tuning ---
                            if (inFreqScaleZone && !overTimeline) {
                                isDraggingScale = true; // 1. Priority: Panning
                                lastDragX = m.x;
                            }
                            else if (!overAprs && !overTimeline && m.x < layout.specW && graphY >= 0 && graphY < (layout.specH + layout.waterfallH + 30)) { 
                                isSpectrumDragging = true; // 2. Fallback: Tuning
                                applySpectrumTuning(m.x);
                            }
                        }
                    }
                }
            }
        } 

        // Update Feedback States
        if (feedbackClock.getElapsedTime().asSeconds() > 1.0f) {
            showCopyMsgFeedback = false;
            showCopyLogFeedback = false;
        }

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
            if (btnAPRS.shape.getGlobalBounds().contains(m)) hover = true;
            if (volSlider.track.getGlobalBounds().contains(m)) hover = true;
            if (rfGainSlider.track.getGlobalBounds().contains(m)) hover = true;
            
            // Hover logic for APRS history
            if (sharedData.aprsEnabled) {
                float overlayH = 250.0f; float overlayY = layout.winH - overlayH;
                float dashWidth = layout.winW * 0.6f;
                if (m.x > dashWidth && m.y > overlayY + 30 && m.y < layout.winH - 10) {
                    hover = true; // Hand cursor for history
                }
                if (btnCopyCur.shape.getGlobalBounds().contains(m)) hover = true;
                if (btnCopyHist.shape.getGlobalBounds().contains(m)) hover = true;
            }

            float axisY = (float)TOP_BAR_H + layout.specH;
            if (isHw && m.y >= axisY - 10 && m.y <= axisY + 20 && m.x < layout.specW) { hover = true; }
            if (window.hasFocus()) { if (hover) { if (cursorHand) window.setMouseCursor(*cursorHand); } else { if (cursorArrow) window.setMouseCursor(*cursorArrow); } }
        }

        // Final cleanup for pending freqs
        if (pendingCenterFreq != 0 && debouncer.getElapsedTime().asMilliseconds() > TUNING_LATENCY_MS) { 
            std::lock_guard<std::mutex> l(sourceMtx); 
            if (currentSource && currentSource->isHardware()) { 
                currentSource->setCenterFrequency(pendingCenterFreq); 
                currentCenterFreq = pendingCenterFreq; 
            } 
            pendingCenterFreq = 0; 
        }

        Mode currentMode; { std::lock_guard<std::mutex> l(sharedData.mtx); currentMode = sharedData.mode; }
        
        if (!sourceDropdown.isOpen) {
             volSlider.update(window); rfGainSlider.update(window);
             squelchSlider.update(window);
             
             if (currentMode != lastMode) {
                 if (currentMode == Mode::WFM) bwSlider.setLimits(50000, 250000); 
                 else if (currentMode == Mode::NFM || currentMode == Mode::AM) bwSlider.setLimits(4000, 40000);   
                 else bwSlider.setLimits(1000, 10000);   
                 lastMode = currentMode;
             }
             bwSlider.update(window);
             std::string bwTxt = "BW: " + std::to_string((int)bwSlider.currentVal) + " Hz"; if (bwSlider.currentVal >= 1000) bwTxt = "BW: " + std::to_string((int)(bwSlider.currentVal/1000)) + " kHz"; bwSlider.setText(bwTxt);
             minDbSlider.update(window); maxDbSlider.update(window);
             bool isHw = false; double prog = 0; { std::lock_guard<std::mutex> l(sourceMtx); if (currentSource) { isHw = currentSource->isHardware(); prog = currentSource->getProgress(); } }
             if (!isHw) { 
                 timeSlider.update(window); 
                 if (timeSlider.isDragging) { 
                     std::lock_guard<std::mutex> l(sharedData.mtx);
                     sharedData.pendingSeekRequest = timeSlider.currentVal;
                 } else { 
                     timeSlider.currentVal = prog; 
                     timeSlider.updateHandlePos(); 
                 } 
             }
        }

        std::vector<double> spectrum; std::vector<uint8_t> row; bool newRow = false; double tunePct = 0.5; Mode mode = Mode::NFM; bool drawAprs = false;
        { 
            std::lock_guard<std::mutex> lock(sharedData.mtx); 
            spectrum = sharedData.fftSpectrum; 
            if (sharedData.newWaterfallData) { row = sharedData.waterfallRow; sharedData.newWaterfallData = false; newRow = true; } 
            tunePct = sharedData.tunedFreqPercent; mode = sharedData.mode; if (sharedData.aprsEnabled) drawAprs = true;
        }

        if (newRow) { 
            std::copy_backward(waterfall.begin(), waterfall.end() - INTERNAL_WATERFALL_WIDTH * 4, waterfall.end()); 
            std::copy(row.begin(), row.end(), waterfall.begin()); 
            wTex.update(waterfall.data()); 
        }

        window.clear(sf::Color::Black);
        long long cf = 0; double sr = 2e6; { std::lock_guard<std::mutex> l(sourceMtx); if (currentSource) { sr = currentSource->getSampleRate(); } } if (currentSource) cf = currentCenterFreq;

        // --- DRAWING ORDER FIXED ---
        
        // 1. Draw Waterfall & Spectrum
        drawGrid(window, font, 0, TOP_BAR_H, layout.specW, layout.specH, cf, sr, minDbSlider.currentVal, maxDbSlider.currentVal);

        sf::VertexArray lines(sf::PrimitiveType::Lines, spectrum.size());
        for (size_t i = 0; i < spectrum.size(); i++) { 
            float norm = (spectrum[i] - minDbSlider.currentVal) / (maxDbSlider.currentVal - minDbSlider.currentVal); float y = layout.specH - (norm * layout.specH); if (y < 0) y = 0; if (y > layout.specH) y = layout.specH; 
            lines[i].position = { (float)i / spectrum.size() * layout.specW, y + TOP_BAR_H }; lines[i].color = sf::Color::Cyan; 
        }
        window.draw(lines); 
        
        wSpr.setTextureRect(sf::IntRect({0, 0}, {INTERNAL_WATERFALL_WIDTH, (int)layout.waterfallH}));
        window.draw(wSpr); 

        float mouseX = -1.0f; float mouseY = -1.0f; { std::lock_guard<std::mutex> lock(sharedData.mtx); mouseX = sharedData.mouseX_spectrum; mouseY = sharedData.mouseY_spectrum; }
        if (mouseX != -1.0f) { sf::Color guideColor(100, 100, 100); sf::VertexArray lineFFT(sf::PrimitiveType::Lines, 2); lineFFT[0].position = {mouseX, (float)TOP_BAR_H}; lineFFT[0].color = guideColor; lineFFT[1].position = {mouseX, (float)layout.specH + TOP_BAR_H}; lineFFT[1].color = guideColor; window.draw(lineFFT); if (mouseY > (float)layout.specH) { sf::VertexArray lineWaterfall(sf::PrimitiveType::Lines, 2); lineWaterfall[0].position = {mouseX, (float)layout.specH + TOP_BAR_H}; lineWaterfall[0].color = guideColor; lineWaterfall[1].position = {mouseX, (float)(layout.specH + layout.waterfallH + TOP_BAR_H)}; lineWaterfall[1].color = guideColor; window.draw(lineWaterfall); } }

        sf::RectangleShape tunerRect; float bwPixels = (bwSlider.currentVal / sr) * layout.specW; if (bwPixels < 2.0f) bwPixels = 2.0f;
        float rectX = tunePct * layout.specW - bwPixels / 2.0f; if (mode == Mode::USB) rectX += bwPixels / 2.0f; if (mode == Mode::LSB) rectX -= bwPixels / 2.0f;
        tunerRect.setSize({bwPixels, (float)layout.specH}); tunerRect.setPosition({rectX, (float)TOP_BAR_H});
        tunerRect.setFillColor(mode == Mode::OFF ? sf::Color(50, 50, 50, 40) : sf::Color(200, 200, 200, 50)); tunerRect.setOutlineThickness(0); window.draw(tunerRect);

        sf::VertexArray centerLine(sf::PrimitiveType::Lines, 2); float centerX = tunePct * layout.specW;
        centerLine[0].position = {centerX, (float)TOP_BAR_H}; centerLine[0].color = sf::Color::Red; centerLine[1].position = {centerX, (float)layout.specH + TOP_BAR_H}; centerLine[1].color = sf::Color::Red; window.draw(centerLine);

        // 2. Draw Sidebar Elements (LAYER 2)
        auto drawSidebarElement = [&](auto& elem, float yPos) {
            if (yPos > TOP_BAR_H && yPos < window.getSize().y) {
                elem.draw(window);
            }
        };
        
        rfGainSlider.draw(window); btnAgc.draw(window);
        bwSlider.draw(window); minDbSlider.draw(window); maxDbSlider.draw(window);
        squelchSlider.draw(window);
        window.draw(labelSnap); stepDropdown.draw(window); 
        
        if (mode == Mode::WFM) stereoCheck.draw(window); 

        window.draw(txtAnalog); window.draw(lineAnalog);
        btnNFM.draw(window); btnAM.draw(window); btnWFM.draw(window); btnOFF.draw(window); btnLSB.draw(window); btnUSB.draw(window);
        window.draw(txtDigital); window.draw(lineDigital);
        btnAPRS.draw(window);
        window.draw(recPanel); window.draw(labelRec); btnRecAudio.draw(window); btnRecIQ.draw(window); window.draw(pathText); btnSelectFolder.draw(window); btnRecStart.draw(window);

        // 3. Draw Sidebar Scrollbar (LAYER 3)
        if (sidebarContentHeight > (layout.winH - TOP_BAR_H)) {
            float trackH = layout.winH - TOP_BAR_H;
            float thumbH = 50.0f; 
            float thumbY = TOP_BAR_H + scrollPercent * (trackH - thumbH);
            
            sf::RectangleShape sbTrack({12.0f, trackH});
            sbTrack.setPosition({layout.winW - 12.0f, (float)TOP_BAR_H});
            sbTrack.setFillColor(sf::Color(20, 20, 20));
            window.draw(sbTrack);

            sf::RectangleShape sbThumb({10.0f, thumbH});
            sbThumb.setPosition({layout.winW - 11.0f, thumbY});
            sbThumb.setFillColor(sf::Color(100, 100, 100));
            sbThumb.setOutlineColor(sf::Color(60,60,60)); sbThumb.setOutlineThickness(1);
            window.draw(sbThumb);
        }

        // 4. Draw APRS Dashboard (LAYER 4 - OVERLAY)
        if (drawAprs) {
            AprsLastPacket pkt; std::deque<std::string> logs;
            { std::lock_guard<std::mutex> l(sharedData.mtx); pkt = sharedData.lastAprs; logs = sharedData.aprsLog; }

            float overlayH = 250.0f; float overlayY = layout.winH - overlayH;
            sf::RectangleShape bg({layout.winW, overlayH}); bg.setPosition({0, overlayY}); bg.setFillColor(sf::Color(20, 20, 25, 255)); bg.setOutlineColor(sf::Color::White); bg.setOutlineThickness(1); window.draw(bg);

            float dashWidth = layout.winW * 0.6f;
            
            float compassX = 60.0f; float compassY = overlayY + 70.0f; float compassR = 40.0f;
            sf::CircleShape compass(compassR); 
            compass.setOrigin({compassR, compassR}); // SFML 3 fix
            compass.setPosition({compassX, compassY}); // SFML 3 fix
            compass.setFillColor(sf::Color::Transparent); compass.setOutlineColor(sf::Color(100,100,100)); compass.setOutlineThickness(2);
            window.draw(compass);
            
            sf::Text dirT(font, "N", 12); dirT.setFillColor(sf::Color::Yellow); 
            dirT.setPosition({compassX - 4, compassY - compassR - 15}); // SFML 3 fix
            window.draw(dirT);
            
            // Rysuj małą "szpilkę" północy dla estetyki
            sf::RectangleShape northTick({2, 6}); northTick.setFillColor(sf::Color::Yellow);
            northTick.setPosition({compassX - 1, compassY - compassR});
            window.draw(northTick);

            if (pkt.course >= 0) {
                sf::VertexArray arrow(sf::PrimitiveType::Lines, 2);
                float rad = (pkt.course - 90.0f) * (3.14159f / 180.0f);
                arrow[0].position = {compassX, compassY}; arrow[0].color = sf::Color::Red;
                arrow[1].position = {compassX + cos(rad)*compassR, compassY + sin(rad)*compassR}; arrow[1].color = sf::Color::Red;
                window.draw(arrow);
                
                sf::Text crsTxt(font, std::to_string((int)pkt.course) + " deg", 12);
                crsTxt.setPosition({compassX - 20, compassY + compassR + 5}); // SFML 3 fix
                window.draw(crsTxt);
            }

            float textX = compassX + compassR + 40.0f;
            float curY = overlayY + 20.0f;

            sf::Text lblCall(font, pkt.src.empty() ? "-- WAITING --" : pkt.src, 32);
            lblCall.setPosition({textX, curY}); // SFML 3 fix
            lblCall.setFillColor(sf::Color::Green); lblCall.setStyle(sf::Text::Bold);
            window.draw(lblCall);

            if (!pkt.dest.empty()) {
                // SFML 3 fix: size.x instead of width
                sf::Text lblDest(font, "> " + pkt.dest, 20);
                lblDest.setPosition({textX + lblCall.getLocalBounds().size.x + 15, curY + 12}); 
                lblDest.setFillColor(sf::Color(200,200,200));
                window.draw(lblDest);
            }

            curY += 40.0f;
            if (!pkt.coords.empty()) {
                sf::Text lblGPS(font, "GPS: " + pkt.coords, 18);
                lblGPS.setPosition({textX, curY}); // SFML 3 fix
                lblGPS.setFillColor(sf::Color::Cyan);
                window.draw(lblGPS);
                curY += 25.0f;
            }

            if (!pkt.comment.empty()) {
                // Zastosuj zawijanie wierszy
                std::string wrappedMsg = wrapText(pkt.comment, font, 16, dashWidth - 80.0f - textX);
                
                sf::Text lblMsg(font, wrappedMsg, 16);
                lblMsg.setPosition({textX, curY}); // SFML 3 fix
                lblMsg.setFillColor(sf::Color::Yellow);
                window.draw(lblMsg);
                
                // Copy Msg Button Positioned below text
                btnCopyCur.setPosition(textX, curY + lblMsg.getLocalBounds().size.y + 10);
                // Visual feedback handling
                if (showCopyMsgFeedback) { btnCopyCur.setText("COPIED!"); btnCopyCur.setColor(sf::Color::Green); }
                else { btnCopyCur.setText("COPY MSG"); btnCopyCur.setColor(sf::Color(60, 60, 60)); }
                btnCopyCur.draw(window);
            }

            float logX = dashWidth; float logW = layout.winW - dashWidth;
            sf::RectangleShape divLine({2, overlayH}); divLine.setPosition({logX, overlayY}); // SFML 3 fix
            divLine.setFillColor(sf::Color::White); window.draw(divLine);

            sf::Text logTitle(font, "PACKET HISTORY", 12); logTitle.setPosition({logX + 10, overlayY + 5}); // SFML 3 fix
            logTitle.setFillColor(sf::Color(150,150,150)); window.draw(logTitle);
            
            // Copy Log Button
            btnCopyHist.setPosition(layout.winW - 100, overlayY + 2);
            if (showCopyLogFeedback) { btnCopyHist.setText("COPIED!"); btnCopyHist.setColor(sf::Color::Green); }
            else { btnCopyHist.setText("COPY LOG"); btnCopyHist.setColor(sf::Color(60, 60, 60)); }
            btnCopyHist.draw(window);

            float logListY = overlayY + 30.0f + aprsLogScrollOffset;
            float lineHeight = 18.0f;
            
            // Mouse hover detection for History
            sf::Vector2f mPos = window.mapPixelToCoords(sf::Mouse::getPosition(window));
            int hoverIdx = -1;
            
            if (mPos.x > logX && mPos.y > overlayY + 30 && mPos.y < layout.winH - 10) {
                float relY = mPos.y - logListY;
                if (relY >= 0) {
                    hoverIdx = (int)(relY / lineHeight);
                }
            }

            int idx = 0;
            for (auto it = logs.begin(); it != logs.end(); ++it) { 
                float yPos = logListY + (idx * lineHeight);
                
                if (yPos > overlayY + 20 && yPos < overlayY + overlayH - 10) {
                    // Highlight background if hovered
                    if (idx == hoverIdx) {
                        sf::RectangleShape hl({logW - 4, lineHeight});
                        hl.setPosition({logX + 2, yPos});
                        hl.setFillColor(sf::Color(50, 50, 70));
                        window.draw(hl);
                    }

                    std::string line = *it;
                    if (line.length() > 55) line = line.substr(0, 52) + "..."; 
                    sf::Text logLine(font, line, 14);
                    logLine.setPosition({logX + 10, yPos}); // SFML 3 fix
                    if (idx == hoverIdx) logLine.setFillColor(sf::Color::White);
                    else logLine.setFillColor(sf::Color(200, 200, 200));
                    window.draw(logLine);
                }
                idx++;
            }
        }

        // 5. Draw TIMELINE (ON TOP OF APRS)
        if (!isHw) { timeSlider.draw(window); } else { btnTuningMode.draw(window); }

        // 6. Draw Top Bar (ALWAYS ON TOP)
        window.draw(topBar);
        freqVFO.draw(window); btnPlay.draw(window);
        if (isHw) btnTuningMode.draw(window);
        window.draw(labelSource); sourceDropdown.draw(window);
        window.draw(labelRate); rateDropdown.draw(window);
        window.draw(labelAudio); audioDropdown.draw(window);
        btnMute.draw(window); volSlider.draw(window);

        if (audioDropdown.isOpen) audioDropdown.draw(window);
        if (rateDropdown.isOpen) rateDropdown.draw(window);
        if (sourceDropdown.isOpen) sourceDropdown.draw(window);
        if (stepDropdown.isOpen) stepDropdown.draw(window);

        window.display();
    }
    dspRunning = false; if (dspThread.joinable()) dspThread.join();
    return 0;
}