#pragma once

#include <vector>
#include <mutex>
#include <deque>
#include <string>
#include <algorithm>
#include <map>
#include <atomic>
#include "miniaudio.h"

// =====================================================================
//  The speaker. Every receiver (tab) pushes its audio into its own
//  stream; the callback mixes the streams, so several tabs can be heard
//  at once. Master volume / mute apply to the mix.
// =====================================================================

class AudioSink {
public:
    struct DeviceInfo {
        std::string name;
        ma_device_id id;
    };

    std::mutex mutex;
    std::map<int, std::deque<float>> streams;   // by stream id (the receiver's id)
    std::atomic<bool> masterMute {false};
    
    // INCREASED BUFFER: 1 second (48000 Hz * 2 channels * 1.0s)
    // Prevents playback stuttering by providing a safe buffer margin.
    const size_t MAX_BUFFER_SIZE = (size_t)(48000 * 2 * 1.0); 
    
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
        
        // Push the default device to the beginning of the list at index 0
        DeviceInfo defaultDev;
        defaultDev.name = "System Default Output";
        availableDevices.push_back(defaultDev);
        
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
        config.playback.channels = 2;
        config.sampleRate = sampleRate;
        config.dataCallback = data_callback;
        config.pUserData = this;
        config.performanceProfile = ma_performance_profile_low_latency;

        // If any device below "System Default" is selected, fetch the ID from miniaudio
        if (deviceIndex > 0 && deviceIndex < (int)availableDevices.size()) {
            config.playback.pDeviceID = &availableDevices[deviceIndex].id;
        } else {
            // If index is 0, passing NULL tells miniaudio to use the system default device
            config.playback.pDeviceID = NULL;
        }

        if (ma_device_init(&context, &config, &device) != MA_SUCCESS) return false;
        isInitialized = true;
        return true;
    }

    void start() { if (isInitialized) ma_device_start(&device); }
    void stop() { if (isInitialized) ma_device_stop(&device); }

    void pushSamples(const std::vector<float>& audioData, int stream = 0) {
        std::lock_guard<std::mutex> lock(mutex);
        std::deque<float>& sampleQueue = streams[stream];
        sampleQueue.insert(sampleQueue.end(), audioData.begin(), audioData.end());

        // If the buffer overflows (e.g., CPU generates data too fast), drop the OLDEST samples.
        // This is a last-resort fallback; flow control in main.cpp should prevent this.
        if (sampleQueue.size() > MAX_BUFFER_SIZE) {
            size_t toRemove = sampleQueue.size() - MAX_BUFFER_SIZE;
            if (toRemove % 2 != 0) toRemove++; 
            sampleQueue.erase(sampleQueue.begin(), sampleQueue.begin() + toRemove);
        }
    }

    size_t getBufferedCount(int stream = 0) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = streams.find(stream);
        return it == streams.end() ? 0 : it->second.size();
    }

    void clear(int stream) {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = streams.find(stream);
        if (it != streams.end()) it->second.clear();
    }
    void clear() {   // every stream
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& kv : streams) kv.second.clear();
    }
    void removeStream(int stream) {
        std::lock_guard<std::mutex> lock(mutex);
        streams.erase(stream);
    }

private:
    static void data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
        AudioSink* sink = (AudioSink*)pDevice->pUserData;
        float* out = (float*)pOutput;
        std::lock_guard<std::mutex> lock(sink->mutex);

        const size_t samplesNeeded = (size_t)frameCount * 2;
        std::fill(out, out + samplesNeeded, 0.0f);   // silence where a stream has no data (underrun)
        for (auto& kv : sink->streams) {
            std::deque<float>& q = kv.second;
            const size_t toRead = std::min(q.size(), samplesNeeded);
            for (size_t i = 0; i < toRead; ++i) { out[i] += q.front(); q.pop_front(); }
        }
        const bool mute = sink->masterMute.load();
        for (size_t i = 0; i < samplesNeeded; ++i) out[i] = mute ? 0.0f : std::clamp(out[i], -1.0f, 1.0f);
    }
};