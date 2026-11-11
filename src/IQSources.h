#pragma once

#include <string>
#include <vector>
#include <complex>
#include <thread>
#include <atomic>
#include <fstream>
#include <mutex>
#include <cstring> 
#include <algorithm>
#include <sstream>
#include "RingBuffer.h"
#include "Decimator.h"
#include "NativeDialogs.h" 

#include <rtl-sdr.h>

#ifdef ENABLE_SDRPLAY
    #include <sdrplay_api.h>
#endif

using Complex = std::complex<double>;

// --- SAMPLE RATE LISTS ---
// Rates below the minimum hardware rate are produced in software: the radio runs
// at a base rate and the samples are decimated right after they are received.
inline const uint32_t RTL_DECIM_BASE_RATE = 2048000;
inline const uint32_t RTL_MIN_HW_RATE = 1400000;
inline const std::vector<uint32_t> RTL_SAMPLE_RATES = {
    1400000, 1800000, 2048000, 2400000, 3200000,
    1024000, 512000, 256000, 128000
};
inline const std::vector<std::string> RTL_SAMPLE_RATES_TXT = {
    "1.4 MSps", "1.8 MSps", "2.048 MSps", "2.4 MSps", "3.2 MSps",
    "1.024 MSps (decim)", "512 kSps (decim)", "256 kSps (decim)", "128 kSps (decim)"
};

inline const uint32_t SDRPLAY_DECIM_BASE_RATE = 2000000;
inline const uint32_t SDRPLAY_MIN_HW_RATE = 2000000;
inline const std::vector<uint32_t> SDRPLAY_SAMPLE_RATES = {
    2000000, 4000000, 6000000, 8000000, 10000000,
    1000000, 500000, 250000, 125000
};
inline const std::vector<std::string> SDRPLAY_SAMPLE_RATES_TXT = {
    "2.0 MSps", "4.0 MSps", "6.0 MSps", "8.0 MSps", "10.0 MSps",
    "1.0 MSps (decim)", "500 kSps (decim)", "250 kSps (decim)", "125 kSps (decim)"
};

struct SDRDeviceItem {
    std::string name;
    std::string id;
};

class IQSource {
public:
    virtual ~IQSource() {}
    virtual bool open(std::string id, uint32_t sampleRate = 0) = 0;
    virtual void close() = 0;
    virtual void start() = 0;
    virtual void stop() = 0;
    virtual int read(Complex* buffer, int count) = 0; 
    virtual double getSampleRate() = 0;
    virtual std::vector<std::string> getAvailableSampleRatesText() { return {}; }
    virtual std::vector<uint32_t> getAvailableSampleRatesValues() { return {}; }
    virtual void setCenterFrequency(long long hz) {}
    virtual void setGain(int gainDb) {} 
    virtual void setHardwareOption(std::string name, int value) {}
    virtual bool isHardware() { return false; }
    virtual bool isSeekable() { return false; } 
    virtual void seek(double percent) {}
    virtual double getProgress() { return 0.0; }

    // Current gain chosen by the software AGC in dB, or a negative value if not available
    virtual float getAutoGainDb() { return -1.0f; }
    // Recent fraction of ADC samples at full scale (0 = no overload)
    virtual float getOverloadFraction() { return 0.0f; }
    // Software AGC gain steps (for the RDS assisted tuning): the current step (-1 = AGC off),
    // the number of steps, and the highest step the AGC may use (-1 = no limit)
    virtual int getAgcStep() { return -1; }
    virtual int getAgcStepCount() { return 0; }
    virtual void setAgcMaxStep(int /*step*/) {}
    virtual bool isAgcLimited() { return false; }

    // --- Loop detection ---
    // Returns true only once, at the moment the file wrapped back to the start
    virtual bool didLoop() { return false; }
};

// --- ROBUST FILE SOURCE (WAV Parser) ---
class FileSource : public IQSource {
    std::ifstream file;
    uint64_t dataStart = 0;     // Where the sample data physically starts
    uint64_t dataLength = 0;    // Data length in bytes (taken from the file size, not the header)
    uint32_t sampleRate = 0;
    uint16_t bitsPerSample = 16;
    uint16_t channels = 2;
    uint64_t currentBytePos = 0; // Current position relative to dataStart
    bool active = false;
    
    // Loop flag
    bool looped = false;

public:
    bool open(std::string path, uint32_t requestedRate = 0) override {
        close();
        file.open(path, std::ios::binary);
        if (!file.is_open()) return false;

        // 1. Check RIFF
        char id[4];
        file.read(id, 4);
        if (strncmp(id, "RIFF", 4) != 0) return false;

        file.seekg(4, std::ios::cur); // Skip RIFF size (unreliable for big files)
        file.read(id, 4);
        if (strncmp(id, "WAVE", 4) != 0) return false;

        // 2. Look for the 'fmt ' and 'data' chunks
        bool foundFmt = false;
        bool foundData = false;

        while (file.good() && (!foundFmt || !foundData)) {
            char chunkID[4];
            uint32_t chunkSize;
            
            file.read(chunkID, 4);
            if (file.gcount() < 4) break;
            file.read((char*)&chunkSize, 4);
            if (file.gcount() < 4) break;

            // Handle metadata chunks that might have weird padding
            if (strncmp(chunkID, "fmt ", 4) == 0) {
                uint16_t audioFormat, numChannels;
                uint32_t sRate, byteRate;
                uint16_t blockAlign, bps;
                
                file.read((char*)&audioFormat, 2);
                file.read((char*)&numChannels, 2);
                file.read((char*)&sRate, 4);
                file.read((char*)&byteRate, 4);
                file.read((char*)&blockAlign, 2);
                file.read((char*)&bps, 2);

                sampleRate = sRate;
                channels = numChannels;
                bitsPerSample = bps;
                foundFmt = true;

                // If the fmt chunk is larger than the standard 16 bytes (e.g. has an extension), skip the rest
                int bytesRead = 16;
                if (chunkSize > bytesRead) {
                    file.seekg(chunkSize - bytesRead, std::ios::cur);
                }
            }
            else if (strncmp(chunkID, "data", 4) == 0) {
                dataStart = file.tellg();
                foundData = true;
                // Don't rely on the header chunkSize, it is wrong for files >4GB.
                // Compute the size from the actual file.
                
                // Remember the current position
                std::streampos current = file.tellg();
                file.seekg(0, std::ios::end);
                std::streampos end = file.tellg();
                dataLength = (uint64_t)end - (uint64_t)current;
                
                // Go back to the start of the data
                file.seekg(current);
                break; // Got everything, stop scanning
            }
            else {
                // Unknown chunk (LIST, bext, JUNK etc.) - skip it
                file.seekg(chunkSize, std::ios::cur);
            }
        }

        if (!foundFmt || !foundData) return false;

        // Format validation
        if (channels != 2) {
            std::cerr << "[FileSource] Error: Only Stereo (2 channel) files supported currently." << std::endl;
            return false;
        }

        currentBytePos = 0;
        return true;
    }

    void close() override {
        if (file.is_open()) file.close();
        active = false;
    }

    void start() override { active = true; }
    void stop() override { active = false; }

    int read(Complex* out, int count) override {
        if (!file.is_open()) return 0;
        
        // Reset the loop flag at the start of each read
        looped = false;

        // Work out how many bytes to read
        int bytesPerSampleTotal = (bitsPerSample / 8) * channels;
        int bytesToRead = count * bytesPerSampleTotal;
        
        // Buffer for the raw bytes
        std::vector<uint8_t> rawBuf(bytesToRead);
        file.read((char*)rawBuf.data(), bytesToRead);
        int bytesRead = (int)file.gcount();
        int samplesRead = bytesRead / bytesPerSampleTotal;

        currentBytePos += bytesRead;

        // Loop back to the start
        if (file.eof() || bytesRead < bytesToRead) {
            file.clear();
            file.seekg(dataStart);
            currentBytePos = 0;
            // Set the loop flag - the DSP worker reads it and resets the decoder
            looped = true; 
        }

        // CONVERSION TO FLOAT
        if (bitsPerSample == 16) {
            // Standard 16-bit SIGNED (SDRPlay, Airspy, most WAV files)
            const double scale = 1.0 / 32768.0;
            int16_t* ptr = (int16_t*)rawBuf.data();
            for (int i = 0; i < samplesRead; i++) {
                // I = left, Q = right
                double i_val = ptr[i * 2] * scale;
                double q_val = ptr[i * 2 + 1] * scale;
                out[i] = Complex(i_val, q_val);
            }
        }
        else if (bitsPerSample == 8) {
            // 8-bit UNSIGNED (raw RTL-SDR, some WAV files)
            // Range 0..255, midpoint 127.5 or 128
            const double scale = 1.0 / 127.5;
            uint8_t* ptr = rawBuf.data();
            for (int i = 0; i < samplesRead; i++) {
                double i_val = (ptr[i * 2] - 127.5) * scale;
                double q_val = (ptr[i * 2 + 1] - 127.5) * scale;
                out[i] = Complex(i_val, q_val);
            }
        }
        else if (bitsPerSample == 24) {
             // 24-bit SIGNED packed (rare, but HDSDR writes it)
             const double scale = 1.0 / 8388608.0;
             uint8_t* ptr = rawBuf.data();
             for(int i=0; i < samplesRead; i++) {
                 int32_t i_int = (ptr[i*6+0] << 8) | (ptr[i*6+1] << 16) | (ptr[i*6+2] << 24);
                 int32_t q_int = (ptr[i*6+3] << 8) | (ptr[i*6+4] << 16) | (ptr[i*6+5] << 24);
                 out[i] = Complex((i_int >> 8) * scale, (q_int >> 8) * scale);
             }
        }
        else if (bitsPerSample == 32) {
            // 32-bit FLOAT (commonly used by SDR# IQ Rec)
            float* ptr = (float*)rawBuf.data();
            for(int i=0; i<samplesRead; i++) {
                out[i] = Complex(ptr[i*2], ptr[i*2+1]);
            }
        }

        return samplesRead;
    }

    bool didLoop() override { return looped; }

    double getSampleRate() override { return (double)sampleRate; }
    bool isSeekable() override { return true; }

    void seek(double percent) override {
        if (!file.is_open()) return;
        
        uint64_t targetByte = (uint64_t)(percent * dataLength);
        
        // Align to a whole frame (block align)
        int blockAlign = (bitsPerSample / 8) * channels;
        targetByte -= (targetByte % blockAlign);

        if (targetByte >= dataLength) targetByte = dataLength - blockAlign;

        file.clear();
        file.seekg(dataStart + targetByte);
        currentBytePos = targetByte;
        
        // Signal a reset on manual seeking as well
        looped = true;
    }

    double getProgress() override {
        if (dataLength == 0) return 0.0;
        return (double)currentBytePos / (double)dataLength;
    }

    std::vector<std::string> getAvailableSampleRatesText() override { 
        return { "Format: " + std::to_string(bitsPerSample) + "-bit " + ((bitsPerSample==8)?"Unsigned":"Signed") }; 
    }
    std::vector<uint32_t> getAvailableSampleRatesValues() override { return {0}; }
};

// --- SOFTWARE AGC FOR 8-BIT RTL-SDR SAMPLES ---
// The RTL2832 digital AGC (and the tuner's own AGC) happily drive the 8-bit ADC into
// clipping on strong broadcast stations. The audio survives that, but weak parts of the
// signal such as the RDS subcarrier get buried in the intermodulation products.
// This AGC steps the tuner gain so the samples stay clear of full scale: down quickly
// when samples clip, up slowly when the level is low. The target level is kept low
// (RMS 0.06 .. 0.18 of full scale): with several strong local FM stations the tuner
// itself produces intermodulation well before the ADC clips. The 8-bit ADC still has
// about 40 dB of headroom above its quantisation noise at these levels.
class RtlSoftAgc {
public:
    std::vector<int> gains;        // available tuner gains in 0.1 dB, ascending
    int index = -1;
    bool enabled = false;
    std::atomic<float> overload {0.0f};   // smoothed fraction of clipped samples
    std::atomic<int> gainTenths {-1};     // current gain for the UI
    std::atomic<int> currentStep {-1};    // current index into 'gains', -1 = AGC off
    std::atomic<int> maxStep {-1};        // highest index allowed (set by the RDS assisted tuning)

    void start() {
        enabled = !gains.empty();
        if (!enabled) return;
        if (index < 0) index = (int)gains.size() / 2;   // start in the middle of the range
        holdoff = 1;
        gainTenths = gains[index];
        currentStep = index;
    }
    void stop() { enabled = false; gainTenths = -1; currentStep = -1; maxStep = -1; }

    // Looks at a buffer of interleaved 8-bit I/Q bytes. Returns true when the gain should change.
    bool analyse(const uint8_t* b, size_t n) {
        if (n == 0) return false;
        size_t clipped = 0;
        double sum = 0.0;
        for (size_t i = 0; i < n; i++) {
            const uint8_t v = b[i];
            if (v <= 1 || v >= 254) clipped++;
            const double x = (v - 127.5) / 127.5;
            sum += x * x;
        }
        const float clipFrac = (float)clipped / (float)n;
        const float rms = (float)std::sqrt(sum / n);
        overload = 0.7f * overload.load() + 0.3f * clipFrac;
        if (!enabled) return false;
        if (holdoff > 0) { holdoff--; return false; }

        int next = index;
        if (clipFrac > 0.0001f || rms > 0.18f) next = index - (clipFrac > 0.01f ? 3 : 1);
        else if (clipped == 0 && rms < 0.06f) next = index + 1;
        const int limit = maxStep;
        if (limit >= 0 && next > limit) next = limit;   // limit from the RDS assisted tuning
        next = std::clamp(next, 0, (int)gains.size() - 1);
        if (next == index) return false;
        index = next;
        holdoff = 2;   // let the new gain settle before judging again
        gainTenths = gains[index];
        currentStep = index;
        return true;
    }
    int gain() const { return gains[index]; }

private:
    int holdoff = 0;
};

// --- RTL-SDR SOURCE ---
class RtlSdrSource : public IQSource {
    rtlsdr_dev_t* dev = nullptr;
    std::thread worker;
    std::atomic<bool> running {false};
    RingBuffer<Complex> ringBuffer;
    uint32_t sampleRate = 2048000;   // sample rate seen by the app (after decimation)
    uint32_t hwSampleRate = 2048000; // sample rate set on the radio
    IQDecimator decimator;           // used only by the worker thread
    
    // Atomics let the UI post requests safely,
    // and the worker applies them at a safe moment.
    std::atomic<long long> currentCenterFreq {100000000};
    std::atomic<long long> pendingCenterFreq {0}; // 0 means no change
    
    std::atomic<int> currentGain {-999};
    std::atomic<int> pendingGain {-999}; // -999 means no change
    
    std::mutex hwMtx; // Only guards the 'dev' pointer during open/close
    std::vector<int> availableGains;
    RtlSoftAgc agc;   // used only by the worker thread (its atomics are read by the UI)

    // USB read buffer (a multiple of 16k is optimal for RTL)
    static const int USB_BUFFER_SIZE = 16 * 16384; 

public:
    RtlSdrSource() : ringBuffer(8 * 1024 * 1024) {}
    ~RtlSdrSource() { close(); }

    static std::vector<SDRDeviceItem> getDeviceList() {
        std::vector<SDRDeviceItem> list;
        int count = rtlsdr_get_device_count();
        for(int i=0; i<count; i++) {
            SDRDeviceItem item;
            item.name = std::string(rtlsdr_get_device_name(i));
            item.id = std::to_string(i);
            list.push_back(item);
        }
        return list;
    }

    bool open(std::string id, uint32_t requestedRate = 0) override {
        std::lock_guard<std::mutex> lock(hwMtx);
        int dev_index = 0; try { dev_index = std::stoi(id); } catch(...) {}
        
        if (rtlsdr_open(&dev, dev_index) < 0) return false;
        
        if (requestedRate == 0) requestedRate = 2048000;
        if (requestedRate < RTL_MIN_HW_RATE) {
            // Low sample rate: run the radio at 2.048 MSps and decimate in software
            hwSampleRate = RTL_DECIM_BASE_RATE;
            decimator.setFactor(computeDecimationFactor(hwSampleRate, requestedRate));
        } else {
            hwSampleRate = requestedRate;
            decimator.setFactor(1);
        }
        sampleRate = hwSampleRate / decimator.getFactor();
        
        // Instead of 0 (no change), queue the current frequency so the worker forces it
        pendingCenterFreq = currentCenterFreq.load(); 
        
        // Instead of -999 (no change), queue -1 (AGC).
        pendingGain = -1; 
        
        // Direct calls kept as a fallback
        rtlsdr_set_sample_rate(dev, hwSampleRate);
        rtlsdr_set_center_freq(dev, currentCenterFreq);
        rtlsdr_set_tuner_gain_mode(dev, 0);
        rtlsdr_set_agc_mode(dev, 0);   // the RTL2832 digital AGC overloads the ADC (see RtlSoftAgc)
        rtlsdr_reset_buffer(dev);
        
        int count = rtlsdr_get_tuner_gains(dev, NULL);
        if (count > 0) { 
            availableGains.resize(count); 
            rtlsdr_get_tuner_gains(dev, availableGains.data()); 
        }
        agc.gains = availableGains;
        std::sort(agc.gains.begin(), agc.gains.end());
        return true;
    }

    void close() override {
        stop();
        std::lock_guard<std::mutex> lock(hwMtx);
        if (dev) {
            rtlsdr_close(dev);
            dev = nullptr;
        }
    }

    // --- WORKER LOOP (SYNC) ---
    void workerLoop() {
            std::vector<uint8_t> buffer(USB_BUFFER_SIZE);
            const float inv = 1.0f / 127.5f;

            while (running) {
            // 1. Handle frequency changes (safe tuning)
            long long newFreq = pendingCenterFreq.exchange(0);
            if (newFreq != 0) {
                if (dev) {
                    // Give the USB/I2C controller a short break after the last bulk transfer
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    
                    int r = rtlsdr_set_center_freq(dev, (uint32_t)newFreq);
                    if (r < 0) {
                        // If the R820T/R828D tuner was busy and rejected the command, retry
                        std::this_thread::sleep_for(std::chrono::milliseconds(15));
                        rtlsdr_set_center_freq(dev, (uint32_t)newFreq);
                    }
                    
                    // Reset the USB hardware buffer and our in-memory ring buffer
                    rtlsdr_reset_buffer(dev);
                    ringBuffer.clear();
                    decimator.reset();
                }
                currentCenterFreq = newFreq;
            }

                // 2. Handle gain changes (safe gain)
                int newGain = pendingGain.exchange(-999);
                if (newGain != -999) {
                    if (dev) {
                        if (newGain == -1) {
                            // AGC: software AGC on the tuner gain (manual gain mode underneath)
                            rtlsdr_set_agc_mode(dev, 0);
                            agc.start();
                            if (agc.enabled) {
                                rtlsdr_set_tuner_gain_mode(dev, 1);
                                rtlsdr_set_tuner_gain(dev, agc.gain());
                            } else {
                                rtlsdr_set_tuner_gain_mode(dev, 0);   // no gain table: tuner AGC
                            }
                        } else {
                            // Manual gain
                            agc.stop();
                            rtlsdr_set_tuner_gain_mode(dev, 1); 
                            rtlsdr_set_agc_mode(dev, 0);        
                            
                            // Find the closest available gain
                            int targetGain = newGain * 10;
                            int bestGain = targetGain;
                            int minDiff = 100000;
                            for (int g : availableGains) {
                                int diff = std::abs(g - targetGain);
                                if (diff < minDiff) {
                                    minDiff = diff;
                                    bestGain = g;
                                }
                            }
                            rtlsdr_set_tuner_gain(dev, bestGain);
                        }
                        // Drop stale samples after a gain change too
                        ringBuffer.clear();
                    }
                    currentGain = newGain;
                }

            // 3. Synchronous read (blocks briefly, but safely)
            int n_read = 0;
            if (rtlsdr_read_sync(dev, buffer.data(), USB_BUFFER_SIZE, &n_read) < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            if (n_read > 0) {
                // Software AGC: small gain steps, the samples are not thrown away
                if (agc.analyse(buffer.data(), (size_t)n_read) && dev) rtlsdr_set_tuner_gain(dev, agc.gain());

                int samples = n_read / 2;
                std::vector<Complex> converted(samples);
                
                // Convert 8-bit unsigned -> float complex
                for (int i = 0; i < samples; i++) {
                    converted[i] = Complex(
                        (buffer[i * 2] - 127.5f) * inv, 
                        (buffer[i * 2 + 1] - 127.5f) * inv
                    );
                }
                size_t outCount = 0;
                const Complex* out = decimator.process(converted.data(), (size_t)samples, outCount);
                if (outCount > 0) ringBuffer.push(out, outCount);
            }
        }
    }

    void start() override {
        if (running) return;
        decimator.reset();
        running = true;
        if (dev) rtlsdr_reset_buffer(dev);
        worker = std::thread(&RtlSdrSource::workerLoop, this);
    }

    void stop() override {
        if (running) {
            running = false;
            if (worker.joinable()) worker.join();
        }
    }

    int read(Complex* buffer, int count) override { 
        return ringBuffer.pop(buffer, count); 
    }
    
    double getSampleRate() override { return (double)sampleRate; }
    bool isHardware() override { return true; }
    float getAutoGainDb() override { int g = agc.gainTenths; return g < 0 ? -1.0f : g / 10.0f; }
    float getOverloadFraction() override { return agc.overload; }
    int getAgcStep() override { return agc.currentStep; }
    int getAgcStepCount() override { return (int)agc.gains.size(); }
    void setAgcMaxStep(int step) override { agc.maxStep = step; }
    bool isAgcLimited() override { return agc.maxStep >= 0 && agc.currentStep >= 0; }
    
    void setCenterFrequency(long long hz) override {
        pendingCenterFreq = hz;
    }
    
    void setGain(int db) override {
        pendingGain = db;
    }
    
    void setHardwareOption(std::string name, int value) override {
        std::lock_guard<std::mutex> lock(hwMtx);
        if (!dev) return;
        if (name == "direct_sampling") { rtlsdr_set_direct_sampling(dev, value); }
        else if (name == "bias_t") { rtlsdr_set_bias_tee(dev, value); }
    }
    
    std::vector<std::string> getAvailableSampleRatesText() override { return RTL_SAMPLE_RATES_TXT; }
    std::vector<uint32_t> getAvailableSampleRatesValues() override { return RTL_SAMPLE_RATES; }
};

#ifdef _WIN32
// Windows: state of the SDRplay API service, which every SDRplay API program talks to.
// Returns "running", "stopped", "missing" or "" (unknown).
inline std::string sdrplayServiceState() {
    SC_HANDLE scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) return "";
    std::string state;
    SC_HANDLE svc = OpenServiceA(scm, "SDRplayAPIService", SERVICE_QUERY_STATUS);
    if (!svc) {
        state = (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) ? "missing" : "";
    } else {
        SERVICE_STATUS st;
        if (QueryServiceStatus(svc, &st)) state = (st.dwCurrentState == SERVICE_RUNNING) ? "running" : "stopped";
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return state;
}

// Starts the SDRplay API service. Needs administrator rights, so Windows asks for
// confirmation (UAC). Returns true when the request was started.
inline bool sdrplayStartService() {
    HINSTANCE r = ShellExecuteA(NULL, "runas", "sc.exe", "start SDRplayAPIService", NULL, SW_HIDE);
    return (INT_PTR)r > 32;
}
#endif

// --- SDRPLAY SOURCE ---
#ifdef ENABLE_SDRPLAY

#ifndef sdrplay_api_Update_None
    #define sdrplay_api_Update_None (sdrplay_api_ReasonForUpdateT)0
#endif
#ifndef sdrplay_api_Update_Ext1_None
    #define sdrplay_api_Update_Ext1_None (sdrplay_api_ReasonForUpdateExtension1T)0
#endif

// Opens the SDRplay API and warns when the installed API differs from the headers
// this program was built with (the API is usually compatible, so it only warns).
inline bool sdrplayApiOpen() {
    if (sdrplay_api_Open() != sdrplay_api_Success) return false;
    float ver = 0.0f;
    if (sdrplay_api_ApiVersion(&ver) == sdrplay_api_Success && std::fabs(ver - SDRPLAY_API_VERSION) > 0.0001f)
        std::cerr << "[SDRplay] API version " << ver << " installed, built for " << SDRPLAY_API_VERSION << std::endl;
    return true;
}


class SdrPlaySource : public IQSource {
    bool isSelected = false; bool isInitialized = false; RingBuffer<Complex> ringBuffer; double sampleRate = 2000000.0; double hwSampleRate = 2000000.0; long long centerFreq = 100000000; std::mutex hwMtx;
    sdrplay_api_DeviceT currentDevice; sdrplay_api_DeviceParamsT *deviceParams = NULL; sdrplay_api_CallbackFnsT cbFns; std::atomic<bool> running {false};
    static std::atomic<int> activeInstances;
    IQDecimator decimator;              // used only by the stream callback
    std::vector<Complex> cbBuf;

    static void StreamCallback(short *xi, short *xq, sdrplay_api_StreamCbParamsT *params, unsigned int numSamples, unsigned int reset, void *cbContext) {
        SdrPlaySource* self = (SdrPlaySource*)cbContext; if (!self->running) return;
        const double scale = 1.0 / 32768.0;
        auto& buf = self->cbBuf;
        buf.resize(numSamples);
        for (unsigned int i = 0; i < numSamples; i++) buf[i] = Complex(xi[i] * scale, xq[i] * scale);
        size_t outCount = 0;
        const Complex* out = self->decimator.process(buf.data(), numSamples, outCount);
        if (outCount > 0) self->ringBuffer.push(out, outCount);
    }
    static void EventCallback(sdrplay_api_EventT eventId, sdrplay_api_TunerSelectT tuner, sdrplay_api_EventParamsT *params, void *cbContext) {}

public:
    SdrPlaySource() : ringBuffer(8 * 1024 * 1024) {}
    ~SdrPlaySource() { close(); }

    static std::vector<SDRDeviceItem> getDeviceList() {
        std::vector<SDRDeviceItem> list;
        bool openedLocally = false;
        if (activeInstances == 0) {
            if (!sdrplayApiOpen()) return list;
            openedLocally = true;
        }
        sdrplay_api_DeviceT devs[6]; unsigned int nDevs = 0;
        // The device list must be locked while it is read (other SDRplay programs may use it too)
        sdrplay_api_LockDeviceApi();
        sdrplay_api_GetDevices(devs, &nDevs, 6);
        sdrplay_api_UnlockDeviceApi();
        for(unsigned int i=0; i<nDevs; i++) {
            SDRDeviceItem item;
            if (devs[i].hwVer == SDRPLAY_RSP1_ID) item.name = "RSP1";
            else if (devs[i].hwVer == SDRPLAY_RSP1A_ID) item.name = "RSP1A";
            else if (devs[i].hwVer == SDRPLAY_RSP2_ID) item.name = "RSP2";
            else if (devs[i].hwVer == SDRPLAY_RSPduo_ID) item.name = "RSPduo";
            else if (devs[i].hwVer == SDRPLAY_RSPdx_ID) item.name = "RSPdx";
            else item.name = "RSP Unknown";
            item.name += " (" + std::string(devs[i].SerNo) + ")";
            item.id = std::string(devs[i].SerNo);
            list.push_back(item);
        }
        if (openedLocally && activeInstances == 0) sdrplay_api_Close();
        return list;
    }

    bool open(std::string id, uint32_t requestedRate = 0) override {
        if (isSelected) close();   // close() takes hwMtx itself
        std::lock_guard<std::mutex> lock(hwMtx);
        if (activeInstances == 0) if (!sdrplayApiOpen()) return false;
        activeInstances++;
        // Keep the device list locked from reading it until the device is selected
        sdrplay_api_LockDeviceApi();
        sdrplay_api_DeviceT devs[6]; unsigned int nDevs = 0; sdrplay_api_GetDevices(devs, &nDevs, 6);
        if (nDevs == 0) { sdrplay_api_UnlockDeviceApi(); activeInstances--; if(activeInstances == 0) sdrplay_api_Close(); return false; }
        int foundIdx = 0;
        if (!id.empty()) { for(unsigned int i=0; i<nDevs; i++) { if (std::string(devs[i].SerNo) == id) { foundIdx = i; break; } } }
        currentDevice = devs[foundIdx]; currentDevice.tuner = sdrplay_api_Tuner_A;
        const bool selected = (sdrplay_api_SelectDevice(&currentDevice) == sdrplay_api_Success);
        sdrplay_api_UnlockDeviceApi();
        if (!selected) { activeInstances--; if(activeInstances == 0) sdrplay_api_Close(); return false; }
        isSelected = true;
        if (sdrplay_api_GetDeviceParams(currentDevice.dev, &deviceParams) != sdrplay_api_Success) { close(); return false; }
        if (requestedRate == 0) requestedRate = 2000000;
        if (requestedRate < SDRPLAY_MIN_HW_RATE) {
            // Low sample rate: run the radio at 2.0 MSps and decimate in software
            hwSampleRate = (double)SDRPLAY_DECIM_BASE_RATE;
            decimator.setFactor(computeDecimationFactor(SDRPLAY_DECIM_BASE_RATE, requestedRate));
        } else {
            hwSampleRate = (double)requestedRate;
            decimator.setFactor(1);
        }
        sampleRate = hwSampleRate / decimator.getFactor();
        deviceParams->devParams->fsFreq.fsHz = hwSampleRate; deviceParams->rxChannelA->tunerParams.rfFreq.rfHz = (double)centerFreq;
        deviceParams->rxChannelA->tunerParams.bwType = sdrplay_api_BW_1_536;
        if (hwSampleRate > 2000000) deviceParams->rxChannelA->tunerParams.bwType = sdrplay_api_BW_5_000;
        if (hwSampleRate > 8000000) deviceParams->rxChannelA->tunerParams.bwType = sdrplay_api_BW_8_000;
        deviceParams->rxChannelA->tunerParams.ifType = sdrplay_api_IF_Zero; 
        deviceParams->rxChannelA->ctrlParams.agc.enable = sdrplay_api_AGC_50HZ; 
        return true;
    }

    void close() override { 
        stop(); 
        std::lock_guard<std::mutex> lock(hwMtx); 
        if (isSelected) { 
            sdrplay_api_ReleaseDevice(&currentDevice); 
            isSelected = false; 
            deviceParams = NULL; 
            activeInstances--;
            if (activeInstances <= 0) { activeInstances = 0; sdrplay_api_Close(); }
        } 
    }

    void start() override { if (running || !isSelected) return; std::lock_guard<std::mutex> lock(hwMtx); if (deviceParams) { deviceParams->devParams->fsFreq.fsHz = hwSampleRate; deviceParams->rxChannelA->tunerParams.rfFreq.rfHz = (double)centerFreq; } decimator.reset(); memset(&cbFns, 0, sizeof(cbFns)); cbFns.StreamACbFn = StreamCallback; cbFns.EventCbFn = EventCallback; if (sdrplay_api_Init(currentDevice.dev, &cbFns, this) == sdrplay_api_Success) { isInitialized = true; running = true; } }
    void stop() override { if (isInitialized) { running = false; sdrplay_api_Uninit(currentDevice.dev); isInitialized = false; } }
    int read(Complex* buffer, int count) override { return ringBuffer.pop(buffer, count); }
    double getSampleRate() override { return sampleRate; }
    bool isHardware() override { return true; }
    
    void setCenterFrequency(long long hz) override { 
        std::lock_guard<std::mutex> lock(hwMtx); 
        centerFreq = hz; 
        if (running && deviceParams) { 
            deviceParams->rxChannelA->tunerParams.rfFreq.rfHz = (double)hz; 
            sdrplay_api_Update(currentDevice.dev, sdrplay_api_Tuner_A, sdrplay_api_Update_Tuner_Frf, sdrplay_api_Update_Ext1_None); 
        } 
    }
    
    void setGain(int db) override {
        std::lock_guard<std::mutex> lock(hwMtx); if (!running || !deviceParams) return;
        if (db == -1) { 
            if (deviceParams->rxChannelA->ctrlParams.agc.enable != sdrplay_api_AGC_50HZ) {
                deviceParams->rxChannelA->ctrlParams.agc.enable = sdrplay_api_AGC_50HZ; 
                sdrplay_api_Update(currentDevice.dev, sdrplay_api_Tuner_A, sdrplay_api_Update_Ctrl_Agc, sdrplay_api_Update_Ext1_None);
            }
        } else { 
            deviceParams->rxChannelA->ctrlParams.agc.enable = sdrplay_api_AGC_DISABLE; 
            int reduction = (50 - db); if (reduction < 0) reduction = 0; if (reduction > 59) reduction = 59;
            deviceParams->rxChannelA->tunerParams.gain.gRdB = reduction;
            deviceParams->rxChannelA->tunerParams.gain.LNAstate = 0;
            sdrplay_api_Update(currentDevice.dev, sdrplay_api_Tuner_A, (sdrplay_api_ReasonForUpdateT)(sdrplay_api_Update_Ctrl_Agc | sdrplay_api_Update_Tuner_Gr), sdrplay_api_Update_Ext1_None);
        }
    }
    
    void setHardwareOption(std::string name, int value) override {
    std::lock_guard<std::mutex> lock(hwMtx);
    
    // Guard against editing an uninitialised structure
    if (!deviceParams) return;

    if (name == "antenna") {
        if (currentDevice.hwVer == SDRPLAY_RSPdx_ID || currentDevice.hwVer == SDRPLAY_RSPdxR2_ID) {
            if (value == 0) deviceParams->devParams->rspDxParams.antennaSel = sdrplay_api_RspDx_ANTENNA_A;
            else if (value == 1) deviceParams->devParams->rspDxParams.antennaSel = sdrplay_api_RspDx_ANTENNA_B;
            else if (value == 2) deviceParams->devParams->rspDxParams.antennaSel = sdrplay_api_RspDx_ANTENNA_C;
            
            if (running) sdrplay_api_Update(currentDevice.dev, sdrplay_api_Tuner_A, sdrplay_api_Update_None, sdrplay_api_Update_RspDx_AntennaControl);
        }
        else if (currentDevice.hwVer == SDRPLAY_RSP2_ID) {
            if (value == 0) deviceParams->rxChannelA->rsp2TunerParams.antennaSel = sdrplay_api_Rsp2_ANTENNA_A;
            else if (value == 1) deviceParams->rxChannelA->rsp2TunerParams.antennaSel = sdrplay_api_Rsp2_ANTENNA_B;
            
            if (running) sdrplay_api_Update(currentDevice.dev, sdrplay_api_Tuner_A, sdrplay_api_Update_Rsp2_AntennaControl, sdrplay_api_Update_Ext1_None);
        }
    }
    else if (name == "bias_t") {
        if (currentDevice.hwVer == SDRPLAY_RSP1A_ID || currentDevice.hwVer == SDRPLAY_RSP1B_ID) {
            deviceParams->rxChannelA->rsp1aTunerParams.biasTEnable = value ? 1 : 0;
            if (running) sdrplay_api_Update(currentDevice.dev, sdrplay_api_Tuner_A, sdrplay_api_Update_Rsp1a_BiasTControl, sdrplay_api_Update_Ext1_None);
        }
        else if (currentDevice.hwVer == SDRPLAY_RSP2_ID) {
            deviceParams->rxChannelA->rsp2TunerParams.biasTEnable = value ? 1 : 0;
            if (running) sdrplay_api_Update(currentDevice.dev, sdrplay_api_Tuner_A, sdrplay_api_Update_Rsp2_BiasTControl, sdrplay_api_Update_Ext1_None);
        }
        else if (currentDevice.hwVer == SDRPLAY_RSPduo_ID) {
            deviceParams->rxChannelA->rspDuoTunerParams.biasTEnable = value ? 1 : 0;
            if (running) sdrplay_api_Update(currentDevice.dev, sdrplay_api_Tuner_A, sdrplay_api_Update_RspDuo_BiasTControl, sdrplay_api_Update_Ext1_None);
        }
        else if (currentDevice.hwVer == SDRPLAY_RSPdx_ID || currentDevice.hwVer == SDRPLAY_RSPdxR2_ID) {
            deviceParams->devParams->rspDxParams.biasTEnable = value ? 1 : 0;
            if (running) sdrplay_api_Update(currentDevice.dev, sdrplay_api_Tuner_A, sdrplay_api_Update_None, sdrplay_api_Update_RspDx_BiasTControl);
        }
    }
    else if (name == "fm_notch") {
        if (currentDevice.hwVer == SDRPLAY_RSP1A_ID || currentDevice.hwVer == SDRPLAY_RSP1B_ID) {
            deviceParams->devParams->rsp1aParams.rfNotchEnable = value ? 1 : 0;
            if (running) sdrplay_api_Update(currentDevice.dev, sdrplay_api_Tuner_A, sdrplay_api_Update_Rsp1a_RfNotchControl, sdrplay_api_Update_Ext1_None);
        }
        else if (currentDevice.hwVer == SDRPLAY_RSP2_ID) {
            deviceParams->rxChannelA->rsp2TunerParams.rfNotchEnable = value ? 1 : 0;
            if (running) sdrplay_api_Update(currentDevice.dev, sdrplay_api_Tuner_A, sdrplay_api_Update_Rsp2_RfNotchControl, sdrplay_api_Update_Ext1_None);
        }
        else if (currentDevice.hwVer == SDRPLAY_RSPduo_ID) {
            deviceParams->rxChannelA->rspDuoTunerParams.rfNotchEnable = value ? 1 : 0;
            if (running) sdrplay_api_Update(currentDevice.dev, sdrplay_api_Tuner_A, sdrplay_api_Update_RspDuo_RfNotchControl, sdrplay_api_Update_Ext1_None);
        }
        else if (currentDevice.hwVer == SDRPLAY_RSPdx_ID || currentDevice.hwVer == SDRPLAY_RSPdxR2_ID) {
            deviceParams->devParams->rspDxParams.rfNotchEnable = value ? 1 : 0;
            if (running) sdrplay_api_Update(currentDevice.dev, sdrplay_api_Tuner_A, sdrplay_api_Update_None, sdrplay_api_Update_RspDx_RfNotchControl);
        }
    }
    else if (name == "mw_notch") {
        // Only the RSPduo exposes this particular switch in the API 3.x structures.
        if (currentDevice.hwVer == SDRPLAY_RSPduo_ID) {
            deviceParams->rxChannelA->rspDuoTunerParams.tuner1AmNotchEnable = value ? 1 : 0;
            if (running) sdrplay_api_Update(currentDevice.dev, sdrplay_api_Tuner_A, sdrplay_api_Update_RspDuo_Tuner1AmNotchControl, sdrplay_api_Update_Ext1_None);
        }
    }
}
    std::vector<std::string> getAvailableSampleRatesText() override { return SDRPLAY_SAMPLE_RATES_TXT; }
    std::vector<uint32_t> getAvailableSampleRatesValues() override { return SDRPLAY_SAMPLE_RATES; }
};
std::atomic<int> SdrPlaySource::activeInstances{0};

#else
class SdrPlaySource : public IQSource {
public: 
    static std::vector<SDRDeviceItem> getDeviceList() { return {}; }
    bool open(std::string id, uint32_t r = 0) override { showPopup("Feature Not Available", "Run ./build.sh and enable SDRPlay."); return false; } void close() override {} void start() override {} void stop() override {} int read(Complex* b, int c) override { return 0; } double getSampleRate() override { return 2000000; } bool isHardware() override { return true; } 
};
#endif
// --- SOUND CARD / LINE-IN SOURCE ---
class AudioCaptureSource : public IQSource {
    ma_context context;
    ma_device device;
    bool contextReady = false;
    bool isInitialized = false;
    RingBuffer<Complex> ringBuffer;
    uint32_t sampleRate = 48000;
    std::atomic<bool> running{false};
    std::vector<Complex> scratch; // only touched from the audio callback thread

    static void data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
        (void)pOutput;
        AudioCaptureSource* src = (AudioCaptureSource*)pDevice->pUserData;
        if (!src || !src->running || !pInput || frameCount == 0) return;

        // Keep latency bounded: if the DSP thread is not keeping up (or the capture clock
        // runs slightly faster than the playback clock), drop input instead of growing forever.
        if (src->ringBuffer.available() + frameCount > (size_t)(src->sampleRate / 2)) return;

        const float* in = (const float*)pInput;
        int channels = (int)pDevice->capture.channels;
        if (channels < 1) channels = 1;

        if (src->scratch.size() < frameCount) src->scratch.resize(frameCount);

        for (ma_uint32 i = 0; i < frameCount; i++) {
            // Mix all channels down to mono
            float mono = 0.0f;
            for (int c = 0; c < channels; c++) mono += in[i * channels + c];
            mono /= (float)channels;

            // Audio goes into the real part (I). Imaginary part (Q) = 0
            src->scratch[i] = Complex(mono, 0.0);
        }
        src->ringBuffer.push(src->scratch.data(), frameCount);
    }

public:
    AudioCaptureSource() : ringBuffer(8 * 1024 * 1024) {
        contextReady = (ma_context_init(NULL, 0, NULL, &context) == MA_SUCCESS);
    }

    ~AudioCaptureSource() {
        close();
        if (contextReady) ma_context_uninit(&context);
    }

    static std::vector<SDRDeviceItem> getDeviceList() {
        std::vector<SDRDeviceItem> list;
        ma_context ctx;
        if (ma_context_init(NULL, 0, NULL, &ctx) == MA_SUCCESS) {
            ma_device_info* pPlaybackInfos;
            ma_uint32 playbackCount;
            ma_device_info* pCaptureInfos;
            ma_uint32 captureCount;

            if (ma_context_get_devices(&ctx, &pPlaybackInfos, &playbackCount, &pCaptureInfos, &captureCount) == MA_SUCCESS) {
                for (ma_uint32 i = 0; i < captureCount; i++) {
                    SDRDeviceItem item;
                    item.name = pCaptureInfos[i].name;
                    item.id = std::to_string(i); // The index is used as the ID
                    list.push_back(item);
                }
            }
            ma_context_uninit(&ctx);
        }
        return list;
    }

    bool open(std::string id, uint32_t requestedRate = 0) override {
        close();
        if (!contextReady) return false;
        sampleRate = (requestedRate > 0) ? requestedRate : 48000;

        ma_device_config config = ma_device_config_init(ma_device_type_capture);
        config.capture.format = ma_format_f32;
        config.capture.channels = 2; // Always request 2 channels and mix down to mono manually
        config.sampleRate = sampleRate;
        config.dataCallback = data_callback;
        config.pUserData = this;

        int devIdx = -1;
        try { devIdx = std::stoi(id); } catch(...) {}

        if (devIdx >= 0) {
            ma_device_info* pPlaybackInfos;
            ma_uint32 playbackCount;
            ma_device_info* pCaptureInfos;
            ma_uint32 captureCount;
            if (ma_context_get_devices(&context, &pPlaybackInfos, &playbackCount, &pCaptureInfos, &captureCount) == MA_SUCCESS) {
                if (devIdx < (int)captureCount) {
                    config.capture.pDeviceID = &pCaptureInfos[devIdx].id;
                } else {
                    return false; // device list changed since it was enumerated
                }
            }
        }

        if (ma_device_init(&context, &config, &device) != MA_SUCCESS) {
            // Some devices refuse stereo capture (e.g. mono-only microphones). Retry with mono.
            config.capture.channels = 1;
            if (ma_device_init(&context, &config, &device) != MA_SUCCESS) return false;
        }
        ringBuffer.clear();
        isInitialized = true;
        return true;
    }

    void close() override {
        stop();
        if (isInitialized) {
            ma_device_uninit(&device);
            isInitialized = false;
        }
    }

    void start() override {
        if (isInitialized && !running) {
            ringBuffer.clear(); // never play back stale audio captured before the pause
            running = true;
            if (ma_device_start(&device) != MA_SUCCESS) running = false;
        }
    }

    void stop() override {
        if (isInitialized && running) {
            running = false;
            ma_device_stop(&device);
        }
    }

    int read(Complex* buffer, int count) override { return (int)ringBuffer.pop(buffer, count); }
    double getSampleRate() override { return sampleRate; }

    std::vector<std::string> getAvailableSampleRatesText() override { return {"48.0 kHz"}; }
    std::vector<uint32_t> getAvailableSampleRatesValues() override { return {48000}; }
};

#include "RtlTcpSource.h"
