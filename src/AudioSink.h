#pragma once

#include <vector>
#include <mutex>
#include <deque>
#include <string>
#include <algorithm>
#include "miniaudio.h"

class AudioSink {
public:
    struct DeviceInfo {
        std::string name;
        ma_device_id id;
    };

    // Thread safety and buffering
    std::mutex mutex;
    std::deque<float> sampleQueue;
    
    // Buffer limit: 150ms for 48kHz Stereo (2 channels)
    // 48000 samples/sec * 2 channels * 0.15 seconds
    const size_t MAX_BUFFER_SIZE = (size_t)(48000 * 2 * 0.15); 
    
    // Miniaudio state
    ma_context context;
    ma_device device;
    ma_device_config config;
    bool isInitialized = false;
    
    std::vector<DeviceInfo> availableDevices;

    AudioSink() {
        if (ma_context_init(NULL, 0, NULL, &context) != MA_SUCCESS) return;
        refreshDeviceList();
    }

    ~AudioSink() {
        if (isInitialized) ma_device_uninit(&device);
        ma_context_uninit(&context);
    }

    void refreshDeviceList() {
        availableDevices.clear();
        ma_device_info* pPlaybackDeviceInfos;
        ma_uint32 playbackDeviceCount;
        
        if (ma_context_get_devices(&context, &pPlaybackDeviceInfos, &playbackDeviceCount, NULL, NULL) == MA_SUCCESS) {
            for (ma_uint32 i = 0; i < playbackDeviceCount; ++i) {
                DeviceInfo info;
                info.name = pPlaybackDeviceInfos[i].name;
                info.id = pPlaybackDeviceInfos[i].id;
                availableDevices.push_back(info);
            }
        }
    }

    bool initDevice(int deviceIndex, int sampleRate) {
        if (isInitialized) {
            ma_device_uninit(&device);
            isInitialized = false;
        }

        config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_f32;
        config.playback.channels = 2; // Force Stereo
        config.sampleRate = sampleRate;
        config.dataCallback = data_callback;
        config.pUserData = this;
        //config.performanceProfile = ma_performance_profile_low_latency;
        config.performanceProfile = ma_performance_profile_conservative;
        if (deviceIndex >= 0 && deviceIndex < (int)availableDevices.size()) {
            config.playback.pDeviceID = &availableDevices[deviceIndex].id;
        }

        if (ma_device_init(&context, &config, &device) != MA_SUCCESS) return false;
        
        isInitialized = true;
        return true;
    }

    void start() { 
        if (isInitialized) ma_device_start(&device); 
    }

    void stop() { 
        if (isInitialized) ma_device_stop(&device); 
    }

    void pushSamples(const std::vector<float>& audioData) {
        std::lock_guard<std::mutex> lock(mutex);
        sampleQueue.insert(sampleQueue.end(), audioData.begin(), audioData.end());

        // Leaky Bucket implementation (Stereo adjusted)
        // If buffer exceeds limit, drop oldest samples but maintain L/R alignment
        if (sampleQueue.size() > MAX_BUFFER_SIZE) {
            size_t toRemove = sampleQueue.size() - MAX_BUFFER_SIZE;
            
            // Ensure we remove an even number of samples to keep stereo channels aligned
            if (toRemove % 2 != 0) toRemove++; 
            
            sampleQueue.erase(sampleQueue.begin(), sampleQueue.begin() + toRemove);
        }
    }

    size_t getBufferedCount() {
        std::lock_guard<std::mutex> lock(mutex);
        return sampleQueue.size();
    }
    
    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        sampleQueue.clear();
    }

private:
    static void data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
        AudioSink* sink = (AudioSink*)pDevice->pUserData;
        float* out = (float*)pOutput;
        std::lock_guard<std::mutex> lock(sink->mutex);
        
        // We need 2x samples because of stereo (L+R)
        size_t samplesNeeded = frameCount * 2;
        
        size_t available = sink->sampleQueue.size();
        size_t toRead = (available < samplesNeeded) ? available : samplesNeeded;

        for (size_t i = 0; i < toRead; ++i) {
            out[i] = sink->sampleQueue.front();
            sink->sampleQueue.pop_front();
        }

        // Fill the rest of the buffer with silence if we ran out of data
        for (size_t i = toRead; i < samplesNeeded; ++i) {
            out[i] = 0.0f;
        }
    }
};