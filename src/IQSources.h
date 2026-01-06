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
#include "NativeDialogs.h" 

#include <rtl-sdr.h>

#ifdef ENABLE_SDRPLAY
    #include <sdrplay_api.h>
#endif

using Complex = std::complex<double>;

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
};

// --- ROBUST FILE SOURCE (WAV Parser) ---
class FileSource : public IQSource {
    std::ifstream file;
    uint64_t dataStart = 0;     // Gdzie fizycznie zaczynają się dane
    uint64_t dataLength = 0;    // Długość danych w bajtach (liczona z pliku, nie z nagłówka)
    uint32_t sampleRate = 0;
    uint16_t bitsPerSample = 16;
    uint16_t channels = 2;
    uint64_t currentBytePos = 0; // Aktualna pozycja względem dataStart
    bool active = false;

public:
    bool open(std::string path, uint32_t requestedRate = 0) override {
        close();
        file.open(path, std::ios::binary);
        if (!file.is_open()) return false;

        // 1. Sprawdź RIFF
        char id[4];
        file.read(id, 4);
        if (strncmp(id, "RIFF", 4) != 0) return false;

        file.seekg(4, std::ios::cur); // Skip RIFF size (unreliable for big files)
        file.read(id, 4);
        if (strncmp(id, "WAVE", 4) != 0) return false;

        // 2. Szukaj chunków 'fmt ' i 'data'
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

                // Jeśli chunk fmt jest większy niż standardowe 16 bajtów (np. ma extension), pomiń resztę
                int bytesRead = 16;
                if (chunkSize > bytesRead) {
                    file.seekg(chunkSize - bytesRead, std::ios::cur);
                }
            }
            else if (strncmp(chunkID, "data", 4) == 0) {
                dataStart = file.tellg();
                foundData = true;
                // Nie polegamy na chunkSize z nagłówka, bo dla plików >4GB jest on błędny.
                // Obliczamy rozmiar fizycznie.
                
                // Zapisz aktualną pozycję
                std::streampos current = file.tellg();
                file.seekg(0, std::ios::end);
                std::streampos end = file.tellg();
                dataLength = (uint64_t)end - (uint64_t)current;
                
                // Wróć na początek danych
                file.seekg(current);
                break; // Mamy wszystko, przestajemy skanować
            }
            else {
                // Nieznany chunk (LIST, bext, JUNK itp.) - pomiń go
                file.seekg(chunkSize, std::ios::cur);
            }
        }

        if (!foundFmt || !foundData) return false;

        // Walidacja formatu
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

        // Oblicz ile bajtów czytać
        int bytesPerSampleTotal = (bitsPerSample / 8) * channels;
        int bytesToRead = count * bytesPerSampleTotal;
        
        // Bufor na surowe bajty
        std::vector<uint8_t> rawBuf(bytesToRead);
        file.read((char*)rawBuf.data(), bytesToRead);
        int bytesRead = (int)file.gcount();
        int samplesRead = bytesRead / bytesPerSampleTotal;

        currentBytePos += bytesRead;

        // Loop - zapętlanie
        if (file.eof() || bytesRead < bytesToRead) {
            file.clear();
            file.seekg(dataStart);
            currentBytePos = 0;
        }

        // KONWERSJA DO FLOAT
        if (bitsPerSample == 16) {
            // Standard 16-bit SIGNED (SDRPlay, Airspy, większość WAV)
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
            // 8-bit UNSIGNED (RTL-SDR raw, niektóre WAV)
            // Zakres 0..255, środek 127.5 lub 128
            const double scale = 1.0 / 127.5;
            uint8_t* ptr = rawBuf.data();
            for (int i = 0; i < samplesRead; i++) {
                double i_val = (ptr[i * 2] - 127.5) * scale;
                double q_val = (ptr[i * 2 + 1] - 127.5) * scale;
                out[i] = Complex(i_val, q_val);
            }
        }
        else if (bitsPerSample == 24) {
             // 24-bit SIGNED packed (rzadkie, ale HDSDR to robi)
             const double scale = 1.0 / 8388608.0;
             uint8_t* ptr = rawBuf.data();
             for(int i=0; i < samplesRead; i++) {
                 int32_t i_int = (ptr[i*6+0] << 8) | (ptr[i*6+1] << 16) | (ptr[i*6+2] << 24);
                 int32_t q_int = (ptr[i*6+3] << 8) | (ptr[i*6+4] << 16) | (ptr[i*6+5] << 24);
                 out[i] = Complex((i_int >> 8) * scale, (q_int >> 8) * scale);
             }
        }
        else if (bitsPerSample == 32) {
            // 32-bit FLOAT (często używane w SDR# IQ Rec)
            float* ptr = (float*)rawBuf.data();
            for(int i=0; i<samplesRead; i++) {
                out[i] = Complex(ptr[i*2], ptr[i*2+1]);
            }
        }

        return samplesRead;
    }

    double getSampleRate() override { return (double)sampleRate; }
    bool isSeekable() override { return true; }

    void seek(double percent) override {
        if (!file.is_open()) return;
        
        uint64_t targetByte = (uint64_t)(percent * dataLength);
        
        // Wyrównanie do ramki (block align)
        int blockAlign = (bitsPerSample / 8) * channels;
        targetByte -= (targetByte % blockAlign);

        if (targetByte >= dataLength) targetByte = dataLength - blockAlign;

        file.clear();
        file.seekg(dataStart + targetByte);
        currentBytePos = targetByte;
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

// --- RTL-SDR SOURCE (Bez zmian) ---
// --- RTL-SDR SOURCE (FIXED FOR V4: SYNC READ) ---
class RtlSdrSource : public IQSource {
    rtlsdr_dev_t* dev = nullptr;
    std::thread worker;
    std::atomic<bool> running {false};
    RingBuffer<Complex> ringBuffer;
    uint32_t sampleRate = 2048000;
    
    // Używamy atomic, aby UI mogło bezpiecznie ustawiać żądania,
    // a worker je aplikował w bezpiecznym momencie.
    std::atomic<long long> currentCenterFreq {100000000};
    std::atomic<long long> pendingCenterFreq {0}; // 0 oznacza brak zmiany
    
    std::atomic<int> currentGain {-999};
    std::atomic<int> pendingGain {-999}; // -999 oznacza brak zmiany
    
    std::mutex hwMtx; // Chroni tylko dostęp do wskaźnika 'dev' przy open/close
    std::vector<int> availableGains;

    // Bufor do odczytu z USB (wielokrotność 16k jest optymalna dla RTL)
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
        
        if (requestedRate > 0) sampleRate = requestedRate; else sampleRate = 2048000;
        
        // --- FIX NA AGC ---
        // Zamiast 0 (brak zmiany freq), ustawiamy aktualną, żeby worker ją wymusił
        pendingCenterFreq = currentCenterFreq.load(); 
        
        // Zamiast -999 (brak zmiany gainu), ustawiamy -1 (AGC).
        // To zmusi pętlę workera do jawnego wywołania rtlsdr_set_tuner_gain_mode(dev, 0)
        // w pierwszym przebiegu pętli.
        pendingGain = -1; 
        
        // Te wywołania bezpośrednie zostawiamy jako "fallback", ale to worker wykona właściwą robotę
        rtlsdr_set_sample_rate(dev, sampleRate);
        rtlsdr_set_center_freq(dev, currentCenterFreq);
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
        if (dev) {
            rtlsdr_close(dev);
            dev = nullptr;
        }
    }

    // --- NEW WORKER LOOP (SYNC) ---
    void workerLoop() {
        std::vector<uint8_t> buffer(USB_BUFFER_SIZE);
        const float inv = 1.0f / 127.5f;

        while (running) {
            // 1. Obsługa zmiany częstotliwości (Safe Tuning)
            long long newFreq = pendingCenterFreq.exchange(0);
            if (newFreq != 0) {
                if (dev) rtlsdr_set_center_freq(dev, (uint32_t)newFreq);
                currentCenterFreq = newFreq;
            }

            // 2. Obsługa zmiany wzmocnienia (Safe Gain)
            int newGain = pendingGain.exchange(-999);
            if (newGain != -999) {
                if (dev) {
                    if (newGain == -1) {
                        rtlsdr_set_tuner_gain_mode(dev, 0); // Auto
                    } else {
                        rtlsdr_set_tuner_gain_mode(dev, 1); // Manual
                        
                        // Znajdź najbliższy dostępny gain
                        int targetGain = newGain * 10;
                        int bestGain = targetGain;
                        int minDiff = 100000;
                        for (int g : availableGains) {
                            int diff = std::abs(g - targetGain);
                            if (diff < minDiff) { minDiff = diff; bestGain = g; }
                        }
                        rtlsdr_set_tuner_gain(dev, bestGain);
                    }
                }
                currentGain = newGain;
            }

            // 3. Odczyt synchroniczny (blokuje na chwilę, ale bezpiecznie)
            int n_read = 0;
            if (rtlsdr_read_sync(dev, buffer.data(), USB_BUFFER_SIZE, &n_read) < 0) {
                // Błąd odczytu (np. urządzenie odłączone)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            if (n_read > 0) {
                int samples = n_read / 2;
                std::vector<Complex> converted(samples);
                
                // Konwersja 8-bit unsigned -> float complex
                for (int i = 0; i < samples; i++) {
                    converted[i] = Complex(
                        (buffer[i * 2] - 127.5f) * inv, 
                        (buffer[i * 2 + 1] - 127.5f) * inv
                    );
                }
                ringBuffer.push(converted.data(), samples);
            }
        }
    }

    void start() override {
        if (running) return;
        running = true;
        if (dev) rtlsdr_reset_buffer(dev);
        worker = std::thread(&RtlSdrSource::workerLoop, this);
    }

    void stop() override {
        if (running) {
            running = false;
            // W trybie sync thread sam wyjdzie z pętli po zakończeniu read_sync.
            // rtlsdr_cancel_async nie jest potrzebne (i nie działa dla sync).
            if (worker.joinable()) worker.join();
        }
    }

    int read(Complex* buffer, int count) override { 
        return ringBuffer.pop(buffer, count); 
    }
    
    double getSampleRate() override { return (double)sampleRate; }
    bool isHardware() override { return true; }
    
    // --- Safe Setters (tylko ustawiają flagi dla workera) ---
    
    void setCenterFrequency(long long hz) override {
        // Nie wołamy rtlsdr_... bezpośrednio!
        pendingCenterFreq = hz;
    }
    
    void setGain(int db) override {
        // Nie wołamy rtlsdr_... bezpośrednio!
        pendingGain = db;
    }
    
    void setHardwareOption(std::string name, int value) override {
        std::lock_guard<std::mutex> lock(hwMtx);
        if (!dev) return;
        // Te funkcje są rzadkie i zazwyczaj bezpieczniejsze, ale idealnie
        // też powinny być w workerze. Dla uproszczenia zostawiamy tu,
        // bo rzadko się je zmienia w trakcie pracy.
        if (name == "direct_sampling") { rtlsdr_set_direct_sampling(dev, value); }
        else if (name == "bias_t") { rtlsdr_set_bias_tee(dev, value); }
    }
    
    std::vector<std::string> getAvailableSampleRatesText() override { return {"1.4 MSps", "1.8 MSps", "2.048 MSps", "2.4 MSps", "3.2 MSps"}; }
    std::vector<uint32_t> getAvailableSampleRatesValues() override { return {1400000, 1800000, 2048000, 2400000, 3200000}; }
};

// --- SDRPLAY SOURCE ---
#ifdef ENABLE_SDRPLAY

#ifndef sdrplay_api_Update_None
    #define sdrplay_api_Update_None (sdrplay_api_ReasonForUpdateT)0
#endif
// DEFINE TYPU EXTENSION DLA 4 ARGUMENTU
#ifndef sdrplay_api_Update_Ext1_None
    #define sdrplay_api_Update_Ext1_None (sdrplay_api_ReasonForUpdateExtension1T)0
#endif

class SdrPlaySource : public IQSource {
    bool isSelected = false; bool isInitialized = false; RingBuffer<Complex> ringBuffer; double sampleRate = 2000000.0; long long centerFreq = 100000000; std::mutex hwMtx;
    sdrplay_api_DeviceT currentDevice; sdrplay_api_DeviceParamsT *deviceParams = NULL; sdrplay_api_CallbackFnsT cbFns; std::atomic<bool> running {false};
    static std::atomic<int> activeInstances;

    static void StreamCallback(short *xi, short *xq, sdrplay_api_StreamCbParamsT *params, unsigned int numSamples, unsigned int reset, void *cbContext) {
        SdrPlaySource* self = (SdrPlaySource*)cbContext; if (!self->running) return;
        Complex tempBuf[2048]; unsigned int processed = 0;
        const double scale = 1.0 / 32768.0;
        while (processed < numSamples) {
            unsigned int chunk = (numSamples - processed); if (chunk > 2048) chunk = 2048;
            for (unsigned int i = 0; i < chunk; i++) { tempBuf[i] = Complex(xi[processed + i] * scale, xq[processed + i] * scale); }
            self->ringBuffer.push(tempBuf, chunk); processed += chunk;
        }
    }
    static void EventCallback(sdrplay_api_EventT eventId, sdrplay_api_TunerSelectT tuner, sdrplay_api_EventParamsT *params, void *cbContext) {}

public:
    SdrPlaySource() : ringBuffer(8 * 1024 * 1024) {}
    ~SdrPlaySource() { close(); }

    static std::vector<SDRDeviceItem> getDeviceList() {
        std::vector<SDRDeviceItem> list;
        bool openedLocally = false;
        if (activeInstances == 0) {
            sdrplay_api_ErrT err = sdrplay_api_Open();
            if (err != sdrplay_api_Success) return list;
            openedLocally = true;
        }
        sdrplay_api_DeviceT devs[6]; unsigned int nDevs = 0;
        sdrplay_api_GetDevices(devs, &nDevs, 6);
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
        std::lock_guard<std::mutex> lock(hwMtx); if (isSelected) close();
        if (activeInstances == 0) if (sdrplay_api_Open() != sdrplay_api_Success) return false;
        activeInstances++;
        sdrplay_api_DeviceT devs[6]; unsigned int nDevs = 0; sdrplay_api_GetDevices(devs, &nDevs, 6); 
        if (nDevs == 0) { activeInstances--; if(activeInstances == 0) sdrplay_api_Close(); return false; }
        int foundIdx = 0;
        if (!id.empty()) { for(unsigned int i=0; i<nDevs; i++) { if (std::string(devs[i].SerNo) == id) { foundIdx = i; break; } } }
        currentDevice = devs[foundIdx]; currentDevice.tuner = sdrplay_api_Tuner_A; 
        if (sdrplay_api_SelectDevice(&currentDevice) != sdrplay_api_Success) { activeInstances--; if(activeInstances == 0) sdrplay_api_Close(); return false; } 
        isSelected = true;
        if (sdrplay_api_GetDeviceParams(currentDevice.dev, &deviceParams) != sdrplay_api_Success) { close(); return false; }
        if (requestedRate > 0) sampleRate = (double)requestedRate; else sampleRate = 2000000.0;
        deviceParams->devParams->fsFreq.fsHz = sampleRate; deviceParams->rxChannelA->tunerParams.rfFreq.rfHz = (double)centerFreq;
        deviceParams->rxChannelA->tunerParams.bwType = sdrplay_api_BW_1_536;
        if (sampleRate > 2000000) deviceParams->rxChannelA->tunerParams.bwType = sdrplay_api_BW_5_000;
        if (sampleRate > 8000000) deviceParams->rxChannelA->tunerParams.bwType = sdrplay_api_BW_8_000;
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

    void start() override { if (running || !isSelected) return; std::lock_guard<std::mutex> lock(hwMtx); if (deviceParams) { deviceParams->devParams->fsFreq.fsHz = sampleRate; deviceParams->rxChannelA->tunerParams.rfFreq.rfHz = (double)centerFreq; } memset(&cbFns, 0, sizeof(cbFns)); cbFns.StreamACbFn = StreamCallback; cbFns.EventCbFn = EventCallback; if (sdrplay_api_Init(currentDevice.dev, &cbFns, this) == sdrplay_api_Success) { isInitialized = true; running = true; } }
    void stop() override { if (isInitialized) { running = false; sdrplay_api_Uninit(currentDevice.dev); isInitialized = false; } }
    int read(Complex* buffer, int count) override { return ringBuffer.pop(buffer, count); }
    double getSampleRate() override { return sampleRate; }
    bool isHardware() override { return true; }
    
    // --- FIX: Używamy sdrplay_api_Update_Ext1_None jako 4. argumentu ---
    void setCenterFrequency(long long hz) override { 
        std::lock_guard<std::mutex> lock(hwMtx); 
        centerFreq = hz; 
        if (running && deviceParams) { 
            deviceParams->rxChannelA->tunerParams.rfFreq.rfHz = (double)hz; 
            sdrplay_api_Update(currentDevice.dev, sdrplay_api_Tuner_A, sdrplay_api_Update_Tuner_Frf, sdrplay_api_Update_Ext1_None); 
        } 
    }
    
    // --- FIX: Używamy sdrplay_api_Update_Ext1_None jako 4. argumentu ---
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
        std::lock_guard<std::mutex> lock(hwMtx); if (!running || !deviceParams) return;
        if (name == "antenna") {
             if (currentDevice.hwVer == SDRPLAY_RSPdx_ID) {
                if (value == 0) deviceParams->devParams->rspDxParams.antennaSel = sdrplay_api_RspDx_ANTENNA_A; 
                if (value == 1) deviceParams->devParams->rspDxParams.antennaSel = sdrplay_api_RspDx_ANTENNA_B; 
                if (value == 2) deviceParams->devParams->rspDxParams.antennaSel = sdrplay_api_RspDx_ANTENNA_C; 
                // Tu też 4 argument:
                sdrplay_api_Update(currentDevice.dev, sdrplay_api_Tuner_A, sdrplay_api_Update_None, sdrplay_api_Update_RspDx_AntennaControl);
            }
        }
    }
    std::vector<std::string> getAvailableSampleRatesText() override { return {"2.0 MSps", "4.0 MSps", "6.0 MSps", "8.0 MSps", "10.0 MSps"}; }
    std::vector<uint32_t> getAvailableSampleRatesValues() override { return {2000000, 4000000, 6000000, 8000000, 10000000}; }
};
std::atomic<int> SdrPlaySource::activeInstances{0};

#else
class SdrPlaySource : public IQSource {
public: 
    static std::vector<SDRDeviceItem> getDeviceList() { return {}; }
    bool open(std::string id, uint32_t r = 0) override { showPopup("Feature Not Available", "Run ./build.sh and enable SDRPlay."); return false; } void close() override {} void start() override {} void stop() override {} int read(Complex* b, int c) override { return 0; } double getSampleRate() override { return 2000000; } bool isHardware() override { return true; } 
};
#endif