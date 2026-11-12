#pragma once
#include <SFML/Graphics.hpp>
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <memory>
#include "Demodulator.h"
#include "RDS_Decoder.h"
#include "IQSources.h"
#include "modules/Core/SDRModule.h"

// --- CONSTANTS ---
inline const int FFT_SIZE = 8192;
inline const int INTERNAL_WATERFALL_WIDTH = 2048;
inline const int WATERFALL_HEIGHT_PX = 1024;
inline const double AUDIO_RATE = 48000.0;
inline const int TOP_BAR_H = 60;
inline const int SIDEBAR_W = 320;
inline const int TUNING_LATENCY_MS = 50;
inline const int GAIN_LATENCY_MS = 50;

inline const std::vector<uint32_t>& RTL_RATES_VAL = RTL_SAMPLE_RATES;
inline const std::vector<uint32_t>& SDRPLAY_RATES_VAL = SDRPLAY_SAMPLE_RATES;
inline const std::vector<std::string> STEP_NAMES = {"None", "1 kHz", "5 kHz", "6.25k", "10 kHz", "12.5k", "25 kHz", "100 kHz"};
inline const std::vector<long long> STEP_VALUES = {0, 1000, 5000, 6250, 10000, 12500, 25000, 100000};
inline const std::vector<std::string> THEME_NAMES = {"Orbit Original", "Neon (Pink/Blue)", "Matrix (Green)", "Grayscale", "Orbit Plus (Aurora)"};

// --- GLOBALS ---
inline std::mutex sourceMtx; 
inline std::shared_ptr<IQSource> currentSource;

struct GlobalHwState {
    bool fmNotch = false;
    bool mwNotch = false;
    bool biasT = false;
    int directSampling = 0;
    int antennaIndex = 0;
};
inline GlobalHwState hwState;

// APRS channel (APRS is built into the DSP worker, not a module): 144.800 (EU),
// 144.390 (US), 145.825 (ISS) ... or the tuner. See modules/Core/DecoderChannels.h
inline ChannelSet& aprsChannels() {
    static ChannelSet c;   // (holds a mutex: configured in place, not copied)
    static const bool configured = [] {
        c.name = "APRS";
        c.presets = { 144800000, 144390000, 144640000, 144575000, 145175000, 145825000, 144930000 };
        c.maxChannels = 1;
        c.defaultWidth = 12500.0;
        c.resizable = true;   // the APRS demodulator uses the channel width
        c.listenMode = Mode::NFM;
        return true;
    }();
    (void)configured;
    return c;
}

enum class RecMode { AUDIO, BASEBAND };

struct AprsLastPacket {
    std::string timestamp;
    std::string raw, src, dest, comment, coords;
    float course = -1.0f, speed = -1.0f;
};

struct SharedData {
    std::mutex mtx;
    double tunedFreqPercent = 0.5; double pendingSeekRequest = -1.0; double bandwidth = 12500.0; long long centerFreq = 0;
    double viewCenterPct = 0.5; 
    float volume = 0.5f; bool isMuted = false;
    float rfGain = -1.0f;
    float squelchThreshold = -100.0f; bool stereoEnabled = false; Mode mode = Mode::NFM; bool isPlaying = false;
    float minDb = -90.0f;
    float maxDb = 0.0f;
    std::vector<double> fftSpectrum; std::vector<uint8_t> waterfallRow; bool newWaterfallData = false; std::string currentFilename = "None"; float mouseX_spectrum = -1.0f; float mouseY_spectrum = -1.0f; bool isRecording = false; RecMode recMode = RecMode::AUDIO; std::string recPath = ""; std::string recStatus = "Idle";
    
    bool aprsEnabled = false;
    bool rdsEnabled = false;
    RdsInfo rds;               // latest RDS data, written by the DSP worker
    SDRModule* activeDecoder = nullptr;
    // Input signal type (Source / Input switch): false = I/Q from a radio, true = demodulated audio
    bool audioInput = false;
    
    std::deque<AprsLastPacket> aprsHistory;
    AprsLastPacket lastAprs;
    
    float zoomLevel = 1.0f;
    int waterfallTheme = 0;
    
    std::vector<double> inspectorSpectrum; 
    std::vector<uint8_t> inspectorWaterfallRow; 
    bool newInspectorData = false;
    
    SharedData() : fftSpectrum(INTERNAL_WATERFALL_WIDTH, -100.0), 
                   waterfallRow(INTERNAL_WATERFALL_WIDTH * 4, 0),
                   inspectorSpectrum(FFT_SIZE/2, -100.0),
                   inspectorWaterfallRow(280 * 4, 0) {} 
};

struct LayoutState {
    float winW, winH, sidebarX, specW, specH, waterfallH;
    float tuneBarH = 24.0f; // Height of the black tuning bar
};

struct BandRegion {
    long long startFreq;
    long long endFreq;
    std::string name;
    sf::Color color;
};

// Simple regex-based JSON parser, no external libraries needed
inline std::vector<BandRegion> loadBandPlan(const std::string& filepath) {
    std::vector<BandRegion> bands;
    std::ifstream file(filepath);
    if (!file.is_open()) return bands;

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    // Expected JSON format: {"start": 144000000, "end": 146000000, "name": "2m Amateur", "color": "255,0,0"}
    std::regex bandRegex("\"start\"\\s*:\\s*(\\d+).*?\"end\"\\s*:\\s*(\\d+).*?\"name\"\\s*:\\s*\"([^\"]+)\".*?\"color\"\\s*:\\s*\"(\\d+),(\\d+),(\\d+)\"");
    
    auto words_begin = std::sregex_iterator(content.begin(), content.end(), bandRegex);
    auto words_end = std::sregex_iterator();

    for (std::sregex_iterator i = words_begin; i != words_end; ++i) {
        std::smatch match = *i;
        BandRegion b;
        b.startFreq = std::stoll(match[1].str());
        b.endFreq = std::stoll(match[2].str());
        b.name = match[3].str();
        b.color = sf::Color(std::stoi(match[4]), std::stoi(match[5]), std::stoi(match[6]));
        bands.push_back(b);
    }
    return bands;
}

inline std::vector<BandRegion> globalBandPlan; // Global storage for the band plan