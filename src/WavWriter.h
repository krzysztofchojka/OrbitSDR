#pragma once
#include <fstream>
#include <string>
#include <algorithm>
#include <cstdint>

struct WavWriter {
    std::ofstream file; uint32_t dataSize = 0; uint32_t sampleRate = 0; uint16_t channels = 0; bool active = false;
    void start(std::string path, uint32_t sr, uint16_t ch) { 
        if (active) stop(); 
        file.open(path, std::ios::binary); 
        if (!file.is_open()) return; 
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
        uint32_t fileSize = dataSize + 36; uint32_t byteRate = sampleRate * channels * 2; uint16_t blockAlign = channels * 2; 
        file.write("RIFF", 4); file.write((char*)&fileSize, 4); file.write("WAVE", 4); file.write("fmt ", 4); 
        uint32_t s1 = 16; uint16_t af = 1; uint16_t bps = 16; 
        file.write((char*)&s1, 4); file.write((char*)&af, 2); file.write((char*)&channels, 2); 
        file.write((char*)&sampleRate, 4); file.write((char*)&byteRate, 4); file.write((char*)&blockAlign, 2); file.write((char*)&bps, 2); 
        file.write("data", 4); file.write((char*)&dataSize, 4); 
        file.close(); active = false; 
    }
};