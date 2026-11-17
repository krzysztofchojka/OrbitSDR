#pragma once

#include <vector>
#include <mutex>
#include <deque>
#include <string>
#include <algorithm>
#include <map>
#include <memory>
#include <atomic>
#include "miniaudio.h"

// =====================================================================
//  The speakers. Every receiver (tab) pushes its audio into its own
//  stream, and every stream goes to the output device chosen for that
//  receiver (Audio Output in the sidebar): e.g. RX1 on the speakers and
//  RX2 on headphones or a virtual cable into another program. A device
//  is opened when the first receiver is routed to it and closed when the
//  last one leaves. Each device's callback mixes its streams, so several
//  tabs on the same device are heard at once. Master mute applies to all.
// =====================================================================

class AudioSink {
public:
    struct DeviceInfo {
        std::string name;
        ma_device_id id;
    };

    std::atomic<bool> masterMute {false};
    std::vector<DeviceInfo> availableDevices;   // [0] = "System Default Output"

    // 1 second per stream (48000 Hz * 2 channels): a safe margin against stutter
    const size_t MAX_BUFFER_SIZE = (size_t)(48000 * 2 * 1.0);

    AudioSink() {
        if (ma_context_init(NULL, 0, NULL, &context) != MA_SUCCESS) return;
        contextOk = true;
        refreshDeviceList();
    }

    ~AudioSink() {
        {
            std::lock_guard<std::mutex> l(routeMtx);
            outs.clear();   // uninit every device (stops its callback) before the context goes
        }
        if (contextOk) ma_context_uninit(&context);
    }

    void refreshDeviceList() {
        availableDevices.clear();
        DeviceInfo def;
        def.name = "System Default Output";
        availableDevices.push_back(def);
        if (!contextOk) return;
        ma_device_info* infos;
        ma_uint32 count;
        if (ma_context_get_devices(&context, &infos, &count, NULL, NULL) == MA_SUCCESS) {
            for (ma_uint32 i = 0; i < count; ++i) {
                DeviceInfo info;
                info.name = infos[i].name;
                info.id = infos[i].id;
                availableDevices.push_back(info);
            }
        }
    }

    // The index in availableDevices of a device name ("" or unknown = 0, the system default)
    int deviceIndex(const std::string& name) const {
        for (size_t i = 1; i < availableDevices.size(); i++) if (availableDevices[i].name == name) return (int)i;
        return 0;
    }
    std::string deviceName(int index) const {
        return index > 0 && index < (int)availableDevices.size() ? availableDevices[index].name : std::string();
    }

    // Sends `stream` to the device `index` (0 = system default). Opens the device if
    // needed, closes the one nobody uses any more. Returns false if it could not be opened
    // (the stream then plays on the system default).
    bool setDevice(int stream, int index) {
        if (index < 0 || index >= (int)availableDevices.size()) index = 0;
        std::lock_guard<std::mutex> l(routeMtx);
        bool ok = true;
        if (!outs.count(index)) {
            auto o = std::make_unique<Out>();
            if (openOut(*o, index)) outs[index] = std::move(o);
            else {
                ok = false;
                index = 0;
                if (!outs.count(0)) { auto d = std::make_unique<Out>(); if (openOut(*d, 0)) outs[0] = std::move(d); }
            }
        }
        const auto old = route.find(stream);
        if (old != route.end() && old->second != index) dropStream(old->second, stream);
        route[stream] = index;
        closeUnused();
        return ok;
    }
    int deviceOf(int stream) {
        std::lock_guard<std::mutex> l(routeMtx);
        const auto it = route.find(stream);
        return it == route.end() ? 0 : it->second;
    }

    void pushSamples(const std::vector<float>& audioData, int stream = 0) {
        std::lock_guard<std::mutex> l(routeMtx);
        Out* o = outFor(stream);
        if (!o) return;
        std::lock_guard<std::mutex> q(o->mtx);
        std::deque<float>& sampleQueue = o->streams[stream];
        sampleQueue.insert(sampleQueue.end(), audioData.begin(), audioData.end());
        // last-resort fallback (the DSP thread waits for the buffer to drain): drop the oldest
        if (sampleQueue.size() > MAX_BUFFER_SIZE) {
            size_t toRemove = sampleQueue.size() - MAX_BUFFER_SIZE;
            if (toRemove % 2 != 0) toRemove++;
            sampleQueue.erase(sampleQueue.begin(), sampleQueue.begin() + toRemove);
        }
    }

    size_t getBufferedCount(int stream = 0) {
        std::lock_guard<std::mutex> l(routeMtx);
        Out* o = outFor(stream);
        if (!o || !o->ok) return 0;
        std::lock_guard<std::mutex> q(o->mtx);
        const auto it = o->streams.find(stream);
        return it == o->streams.end() ? 0 : it->second.size();
    }

    void clear(int stream) {
        std::lock_guard<std::mutex> l(routeMtx);
        if (Out* o = outFor(stream)) {
            std::lock_guard<std::mutex> q(o->mtx);
            const auto it = o->streams.find(stream);
            if (it != o->streams.end()) it->second.clear();
        }
    }
    void clear() {   // every stream
        std::lock_guard<std::mutex> l(routeMtx);
        for (auto& kv : outs) {
            std::lock_guard<std::mutex> q(kv.second->mtx);
            for (auto& s : kv.second->streams) s.second.clear();
        }
    }
    void removeStream(int stream) {
        std::lock_guard<std::mutex> l(routeMtx);
        const auto it = route.find(stream);
        if (it != route.end()) { dropStream(it->second, stream); route.erase(it); }
        closeUnused();
    }

private:
    struct Out {
        ma_device device;
        bool ok = false;
        std::mutex mtx;
        std::map<int, std::deque<float>> streams;   // by stream id (the receiver's id)
        std::atomic<bool>* mute = nullptr;
        ~Out() { if (ok) ma_device_uninit(&device); }
    };

    ma_context context;
    bool contextOk = false;
    std::mutex routeMtx;                          // route, outs (the callbacks only lock Out::mtx)
    std::map<int, std::unique_ptr<Out>> outs;     // by device index
    std::map<int, int> route;                     // stream -> device index

    bool openOut(Out& o, int index) {
        if (!contextOk) return false;
        ma_device_config config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_f32;
        config.playback.channels = 2;
        config.sampleRate = 48000;
        config.dataCallback = data_callback;
        config.pUserData = &o;
        config.performanceProfile = ma_performance_profile_low_latency;
        config.playback.pDeviceID = index > 0 ? &availableDevices[index].id : NULL;
        o.mute = &masterMute;
        if (ma_device_init(&context, &config, &o.device) != MA_SUCCESS) return false;
        o.ok = true;
        if (ma_device_start(&o.device) != MA_SUCCESS) { ma_device_uninit(&o.device); o.ok = false; return false; }
        return true;
    }
    // routeMtx held. Streams not routed yet play on the system default.
    Out* outFor(int stream) {
        const auto r = route.find(stream);
        const int idx = r == route.end() ? 0 : r->second;
        auto it = outs.find(idx);
        if (it == outs.end() && idx == 0) {
            auto d = std::make_unique<Out>();
            if (!openOut(*d, 0)) return nullptr;
            it = outs.emplace(0, std::move(d)).first;
            route[stream] = 0;
        }
        return it == outs.end() ? nullptr : it->second.get();
    }
    void dropStream(int index, int stream) {   // routeMtx held
        const auto it = outs.find(index);
        if (it == outs.end()) return;
        std::lock_guard<std::mutex> q(it->second->mtx);
        it->second->streams.erase(stream);
    }
    void closeUnused() {   // routeMtx held
        for (auto it = outs.begin(); it != outs.end();) {
            bool used = false;
            for (const auto& r : route) if (r.second == it->first) used = true;
            if (!used) it = outs.erase(it); else ++it;
        }
    }

    static void data_callback(ma_device* pDevice, void* pOutput, const void*, ma_uint32 frameCount) {
        Out* o = (Out*)pDevice->pUserData;
        float* out = (float*)pOutput;
        const size_t samplesNeeded = (size_t)frameCount * 2;
        std::fill(out, out + samplesNeeded, 0.0f);   // silence where a stream has no data (underrun)
        std::lock_guard<std::mutex> lock(o->mtx);
        for (auto& kv : o->streams) {
            std::deque<float>& q = kv.second;
            const size_t toRead = std::min(q.size(), samplesNeeded);
            for (size_t i = 0; i < toRead; ++i) { out[i] += q.front(); q.pop_front(); }
        }
        const bool mute = o->mute && o->mute->load();
        for (size_t i = 0; i < samplesNeeded; ++i) out[i] = mute ? 0.0f : std::clamp(out[i], -1.0f, 1.0f);
    }
};
