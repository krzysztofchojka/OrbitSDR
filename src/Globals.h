#pragma once
#include <SFML/Graphics.hpp>
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <memory>
#include "Demodulator.h"
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

inline const std::vector<uint32_t> RTL_RATES_VAL = {1400000, 1800000, 2048000, 2400000, 3200000};
inline const std::vector<uint32_t> SDRPLAY_RATES_VAL = {2000000, 4000000, 6000000, 8000000, 10000000};
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
    SDRModule* activeDecoder = nullptr;
    
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

struct LayoutState { float winW, winH, sidebarX, specW, specH, waterfallH; };