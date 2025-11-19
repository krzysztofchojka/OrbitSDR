#pragma once

#include <string>
#include <vector>
#include <complex>
#include <thread>
#include <atomic>
#include <fstream>
#include <mutex>
#include <cstring> 
#include <algorithm> // std::min_element, std::abs
#include "RingBuffer.h"
#include "NativeDialogs.h" 

#include <rtl-sdr.h>

#ifdef ENABLE_SDRPLAY
    #include <sdrplay_api.h>
#endif

using Complex = std::complex<double>;

// --- BASE INTERFACE ---
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
    
    // gainDb: -1 for Auto (AGC), 0..50 for Manual
    virtual void setGain(int gainDb) {} 
    
    virtual bool isHardware() { return false; }
    virtual bool isSeekable() { return false; } 
    virtual void seek(double percent) {}
    virtual double getProgress() { return 0.0; }
};

// --- FILE SOURCE (WAV) ---
class FileSource : public IQSource {
    std::ifstream file;
    uint32_t dataStart = 0; 
    uint32_t dataSize = 0; 
    uint32_t sampleRate = 0; 
    uint64_t currentPos = 0; 
    bool active = false;

#pragma pack(push, 1)
    struct WavHeader { 
        char r[4]; uint32_t s; char w[4]; char f[4]; 
        uint32_t l; uint16_t t; uint16_t c; uint32_t sr; 
        uint32_t br; uint16_t a; uint16_t b; char d[4]; uint32_t ds; 
    };
#pragma pack(pop)

public:
    bool open(std::string path, uint32_t requestedRate = 0) override {
        close(); 
        file.open(path, std::ios::binary); 
        if (!file) return false;

        WavHeader h; 
        file.read((char*)&h, sizeof(h)); 
        if (h.c != 2) return false; // Must be stereo for IQ

        sampleRate = h.sr; 
        dataSize = h.ds; 
        dataStart = (uint32_t)file.tellg(); 
        currentPos = 0; 
        return true;
    }

    void close() override { if (file.is_open()) file.close(); active = false; }
    void start() override { active = true; }
    void stop() override { active = false; }

    int read(Complex* out, int count) override {
        if (!active || !file.is_open()) return 0;
        
        std::vector<int16_t> buf(count * 2);
        file.read((char*)buf.data(), buf.size() * 2);
        
        int readSamples = (int)file.gcount() / 4;
        for (int i = 0; i < readSamples; i++) {
            out[i] = Complex(buf[i * 2] / 32768.0, buf[i * 2 + 1] / 32768.0);
        }

        currentPos += readSamples * 4; 
        if (file.eof()) { file.clear(); file.seekg(dataStart); currentPos = 0; }
        return readSamples;
    }

    double getSampleRate() override { return (double)sampleRate; }
    bool isSeekable() override { return true; }
    
    void seek(double percent) override { 
        if (!file.is_open()) return; 
        uint64_t target = (uint64_t)(percent * dataSize); 
        target -= (target % 4); 
        file.clear(); file.seekg(dataStart + target); 
        currentPos = target; 
    }
    
    double getProgress() override { return dataSize > 0 ? (double)currentPos / dataSize : 0.0; }
    std::vector<std::string> getAvailableSampleRatesText() override { return {"File Default"}; }
    std::vector<uint32_t> getAvailableSampleRatesValues() override { return {0}; }
};

// --- RTL-SDR SOURCE ---
class RtlSdrSource : public IQSource {
    rtlsdr_dev_t* dev = nullptr; 
    std::thread worker; 
    std::atomic<bool> running {false}; 
    RingBuffer<Complex> ringBuffer;
    uint32_t sampleRate = 2048000; 
    uint32_t centerFreq = 100000000;
    std::mutex hwMtx;
    std::vector<int> availableGains; 

    static void rtlsdr_callback(unsigned char *buf, uint32_t len, void *ctx) {
        RtlSdrSource* self = (RtlSdrSource*)ctx; 
        if (!self->running) return;
        
        int samples = len / 2; 
        std::vector<Complex> converted(samples);
        for (int i = 0; i < samples; i++) {
            converted[i] = Complex((buf[i * 2] - 127.5) / 127.5, (buf[i * 2 + 1] - 127.5) / 127.5);
        }
        self->ringBuffer.push(converted.data(), samples);
    }

public:
    RtlSdrSource() : ringBuffer(1024 * 1024) {} 
    ~RtlSdrSource() { close(); }

    bool open(std::string id, uint32_t requestedRate = 0) override {
        std::lock_guard<std::mutex> lock(hwMtx);
        int dev_index = 0; try { dev_index = std::stoi(id); } catch(...) {}

        if (rtlsdr_open(&dev, dev_index) < 0) return false;

        if (requestedRate > 0) sampleRate = requestedRate; 
        else sampleRate = 2048000;

        rtlsdr_set_sample_rate(dev, sampleRate); 
        rtlsdr_set_center_freq(dev, centerFreq); 
        rtlsdr_set_tuner_gain_mode(dev, 0); 
        rtlsdr_reset_buffer(dev);
        
        int count = rtlsdr_get_tuner_gains(dev, NULL);
        if (count > 0) {
            availableGains.resize(count);
            rtlsdr_get_tuner_gains(dev, availableGains.data());
        }
        return true;
    }

    void close() override { 
        stop(); 
        std::lock_guard<std::mutex> lock(hwMtx);
        if (dev) { rtlsdr_close(dev); dev = nullptr; } 
    }

    void start() override { 
        if (running) return; 
        running = true; 
        if (dev) rtlsdr_reset_buffer(dev); 
        worker = std::thread([this]() { rtlsdr_read_async(dev, rtlsdr_callback, this, 0, 0); }); 
    }

    void stop() override { 
        if (running) { 
            running = false; 
            if (dev) rtlsdr_cancel_async(dev); 
            if (worker.joinable()) worker.join(); 
        } 
    }

    int read(Complex* buffer, int count) override { return ringBuffer.pop(buffer, count); }
    double getSampleRate() override { return (double)sampleRate; }
    bool isHardware() override { return true; }
    
    void setCenterFrequency(long long hz) override { 
        std::lock_guard<std::mutex> lock(hwMtx);
        centerFreq = hz; 
        if (dev && running) rtlsdr_set_center_freq(dev, centerFreq); 
    }
    
    void setGain(int db) override { 
        std::lock_guard<std::mutex> lock(hwMtx);
        if (!dev || !running) return;

        if (db == -1) {
            rtlsdr_set_tuner_gain_mode(dev, 0); 
        } else {
            rtlsdr_set_tuner_gain_mode(dev, 1);
            int targetGain = db * 10;
            int bestGain = 0;
            int minDiff = 100000;
            if (!availableGains.empty()) {
                for (int g : availableGains) {
                    int diff = std::abs(g - targetGain);
                    if (diff < minDiff) { minDiff = diff; bestGain = g; }
                }
            } else { bestGain = targetGain; }
            rtlsdr_set_tuner_gain(dev, bestGain); 
        } 
    }

    std::vector<std::string> getAvailableSampleRatesText() override {
        return {"1.024 MSps", "1.4 MSps", "1.8 MSps", "2.048 MSps", "2.4 MSps", "3.2 MSps"};
    }
    std::vector<uint32_t> getAvailableSampleRatesValues() override {
        return {1024000, 1400000, 1800000, 2048000, 2400000, 3200000};
    }
};

// --- SDRPLAY SOURCE ---

#ifdef ENABLE_SDRPLAY

class SdrPlaySource : public IQSource {
    bool isSelected = false;
    bool isInitialized = false;
    RingBuffer<Complex> ringBuffer;
    double sampleRate = 2000000.0;
    long long centerFreq = 100000000;
    std::mutex hwMtx;
    
    sdrplay_api_DeviceT currentDevice;
    sdrplay_api_DeviceParamsT *deviceParams = NULL;
    sdrplay_api_CallbackFnsT cbFns;
    std::atomic<bool> running {false};

    static void StreamCallback(short *xi, short *xq, sdrplay_api_StreamCbParamsT *params, unsigned int numSamples, unsigned int reset, void *cbContext) {
        SdrPlaySource* self = (SdrPlaySource*)cbContext;
        if (!self->running) return;
        
        Complex tempBuf[2048];
        unsigned int processed = 0;
        while (processed < numSamples) {
            unsigned int chunk = (numSamples - processed);
            if (chunk > 2048) chunk = 2048;
            for (unsigned int i = 0; i < chunk; i++) {
                tempBuf[i] = Complex(xi[processed + i] / 32768.0, xq[processed + i] / 32768.0);
            }
            self->ringBuffer.push(tempBuf, chunk);
            processed += chunk;
        }
    }
    static void EventCallback(sdrplay_api_EventT eventId, sdrplay_api_TunerSelectT tuner, sdrplay_api_EventParamsT *params, void *cbContext) {}

public:
    SdrPlaySource() : ringBuffer(1024 * 1024) {}
    ~SdrPlaySource() { close(); }

    bool open(std::string id, uint32_t requestedRate = 0) override {
        std::lock_guard<std::mutex> lock(hwMtx);
        if (isSelected) close();

        if (sdrplay_api_Open() != sdrplay_api_Success) return false;
        
        sdrplay_api_DeviceT devs[6]; unsigned int nDevs = 0;
        sdrplay_api_GetDevices(devs, &nDevs, 6);
        if (nDevs == 0) { sdrplay_api_Close(); return false; }
        
        currentDevice = devs[0]; currentDevice.tuner = sdrplay_api_Tuner_A; 
        if (sdrplay_api_SelectDevice(&currentDevice) != sdrplay_api_Success) { sdrplay_api_Close(); return false; }
        isSelected = true;

        if (sdrplay_api_GetDeviceParams(currentDevice.dev, &deviceParams) != sdrplay_api_Success) { close(); return false; }

        if (requestedRate > 0) sampleRate = (double)requestedRate; else sampleRate = 2000000.0;

        deviceParams->devParams->fsFreq.fsHz = sampleRate;
        deviceParams->rxChannelA->tunerParams.rfFreq.rfHz = (double)centerFreq;
        
        deviceParams->rxChannelA->tunerParams.bwType = sdrplay_api_BW_1_536;
        if (sampleRate > 2000000) deviceParams->rxChannelA->tunerParams.bwType = sdrplay_api_BW_5_000;
        if (sampleRate > 8000000) deviceParams->rxChannelA->tunerParams.bwType = sdrplay_api_BW_8_000;

        deviceParams->rxChannelA->tunerParams.ifType = sdrplay_api_IF_Zero;
        // FIX: USE SPECIFIC AGC RATE (50HZ) AS PER DOCS
        deviceParams->rxChannelA->ctrlParams.agc.enable = sdrplay_api_AGC_50HZ; 

        return true;
    }

    void close() override {
        stop();
        std::lock_guard<std::mutex> lock(hwMtx);
        if (isSelected) {
            sdrplay_api_ReleaseDevice(&currentDevice);
            sdrplay_api_Close();
            isSelected = false;
            deviceParams = NULL;
        }
    }

    void start() override {
        if (running || !isSelected) return;
        std::lock_guard<std::mutex> lock(hwMtx);
        
        if (deviceParams) {
             deviceParams->devParams->fsFreq.fsHz = sampleRate;
             deviceParams->rxChannelA->tunerParams.rfFreq.rfHz = (double)centerFreq;
        }
        memset(&cbFns, 0, sizeof(cbFns));
        cbFns.StreamACbFn = StreamCallback; cbFns.EventCbFn = EventCallback;
        if (sdrplay_api_Init(currentDevice.dev, &cbFns, this) == sdrplay_api_Success) { isInitialized = true; running = true; }
    }

    void stop() override {
        if (isInitialized) {
            running = false;
            sdrplay_api_Uninit(currentDevice.dev);
            isInitialized = false;
        }
    }

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
    
    // Correct Gain Implementation for SDRPlay using Doc 3.0
    void setGain(int db) override {
        std::lock_guard<std::mutex> lock(hwMtx);
        if (!running || !deviceParams) return;

        if (db == -1) {
            // Enable AGC (Use 50Hz refresh)
            deviceParams->rxChannelA->ctrlParams.agc.enable = sdrplay_api_AGC_50HZ;
            sdrplay_api_Update(currentDevice.dev, sdrplay_api_Tuner_A, sdrplay_api_Update_Ctrl_Agc, sdrplay_api_Update_Ext1_None);
        } else {
            // Disable AGC
            deviceParams->rxChannelA->ctrlParams.agc.enable = sdrplay_api_AGC_DISABLE;
            
            // Manual Logic: Force LNA to Max Sensitivity (State 0)
            deviceParams->rxChannelA->tunerParams.gain.LNAstate = 0;

            // Map Slider (0..50) to Gain Reduction dB (0..60+)
            // Slider 50 (Max Gain) -> 0 dB Reduction
            // Slider 0 (Min Gain)  -> ~60 dB Reduction
            int reduction = (50 - db) * 1.2; 
            if (reduction < 0) reduction = 0;
            
            deviceParams->rxChannelA->tunerParams.gain.gRdB = reduction;
            
            // Update AGC, Tuner Gain (LNA/gRdB). Note: Update_Tuner_Gr covers both.
            sdrplay_api_Update(currentDevice.dev, sdrplay_api_Tuner_A, 
                               (sdrplay_api_ReasonForUpdateT)(sdrplay_api_Update_Ctrl_Agc | sdrplay_api_Update_Tuner_Gr), 
                               sdrplay_api_Update_Ext1_None);
        }
    }

    std::vector<std::string> getAvailableSampleRatesText() override {
        return {"2.0 MSps", "4.0 MSps", "6.0 MSps", "8.0 MSps", "10.0 MSps"};
    }
    std::vector<uint32_t> getAvailableSampleRatesValues() override {
        return {2000000, 4000000, 6000000, 8000000, 10000000};
    }
};

#else

class SdrPlaySource : public IQSource {
public:
    bool open(std::string id, uint32_t r = 0) override { 
        showPopup("Feature Not Available", "Run ./build.sh and enable SDRPlay."); return false; 
    }
    void close() override {} void start() override {} void stop() override {}
    int read(Complex* b, int c) override { return 0; }
    double getSampleRate() override { return 2000000; }
    bool isHardware() override { return true; }
};

#endif