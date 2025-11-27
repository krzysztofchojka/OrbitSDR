#pragma once

#include <vector>
#include <cmath>
#include <iostream>
#include <functional>
#include <string>
#include <algorithm>
#include <deque>
#include <complex>

// Structure containing data for real-time visualization
struct AprsVisData {
    float demodSignal;
    bool sampleNow;
    int decodedBit;
};

class APRSDecoder {
private:
    // --- CONSTANTS ---
    const float BAUD_RATE = 1200.0f;
    
    float sampleRate;
    float samplesPerBit;

    // --- FAST DEMODULATOR STATE (SYNC) ---
    // Instantaneous I/Q Correlators
    float phase1200 = 0.0f, step1200 = 0.0f;
    float phase2200 = 0.0f, step2200 = 0.0f;
    float markI = 0.0f, markQ = 0.0f;
    float spaceI = 0.0f, spaceQ = 0.0f;
    float filterAlpha = 0.0f;

    // --- PLL STATE ---
    float pllPhase = 0.0f;
    float pllStep = 0.0f;
    float prevDemod = 0.0f;
    
    // --- HDLC & CRC STATE ---
    bool lastBitLevel = false;
    int onesCount = 0;
    bool frameActive = false;
    std::vector<uint8_t> rawBytes; // Buffer for CRC check
    uint8_t currentByte = 0;
    int bitIndex = 0;
    
public:
    // Callbacks
    std::function<void(AprsVisData)> onDebugData; 
    std::function<void(std::string)> onMessage;

    APRSDecoder(float sr) : sampleRate(sr) {
        if (sampleRate < 10000.0f) sampleRate = 48000.0f;
        
        samplesPerBit = sampleRate / BAUD_RATE;
        pllStep = 1.0f / samplesPerBit; // Full cycle (1.0) per bit

        // --- SETUP FAST DEMODULATOR ---
        step1200 = (2.0f * M_PI * 1200.0f) / sampleRate;
        step2200 = (2.0f * M_PI * 2200.0f) / sampleRate;
        
        // Fast low-pass filter (approx. 0.5 bit length)
        filterAlpha = 1.0f / (samplesPerBit * 0.5f);
    }

    // Calculates CRC-CCITT (AX.25)
    bool checkCRC(const std::vector<uint8_t>& data) {
        if (data.size() < 3) return false; // Minimum frame: 1 byte data + 2 bytes CRC
        
        uint16_t crc = 0xFFFF;
        
        // Calculate CRC for data (excluding the last 2 bytes which are the checksum)
        for (size_t i = 0; i < data.size() - 2; ++i) {
            uint8_t byte = data[i];
            for (int bit = 0; bit < 8; ++bit) {
                bool bitVal = (byte >> bit) & 1;
                bool xorFlag = (crc & 1) ^ bitVal;
                crc >>= 1;
                if (xorFlag) crc ^= 0x8408;
            }
        }
        crc ^= 0xFFFF; // Final XOR
        
        uint8_t fcsLo = data[data.size() - 2];
        uint8_t fcsHi = data[data.size() - 1];
        
        // Validation (AX.25 sends LSB first, CRC is also LSB first in the stream)
        return (uint8_t)(crc & 0xFF) == fcsLo && (uint8_t)(crc >> 8) == fcsHi;
    }

    void process(const std::vector<float>& audioIn) {
        for (float sample : audioIn) {
            
            // --- 1. FAST DEMODULATION (I/Q) ---
            
            // Mark (1200 Hz) generator & mix
            phase1200 += step1200;
            if (phase1200 > 6.283185f) phase1200 -= 6.283185f;
            float mI = sample * cos(phase1200);
            float mQ = sample * sin(phase1200);

            // Space (2200 Hz) generator & mix
            phase2200 += step2200;
            if (phase2200 > 6.283185f) phase2200 -= 6.283185f;
            float sI = sample * cos(phase2200);
            float sQ = sample * sin(phase2200);

            // Low-pass filtering
            markI += (mI - markI) * filterAlpha;
            markQ += (mQ - markQ) * filterAlpha;
            spaceI += (sI - spaceI) * filterAlpha;
            spaceQ += (sQ - spaceQ) * filterAlpha;

            // Energy (squared magnitude is faster than sqrt)
            float magMark = markI * markI + markQ * markQ;
            float magSpace = spaceI * spaceI + spaceQ * spaceQ;

            // Decision signal (Mark - Space)
            float fastDemodVal = (magMark - magSpace); 

            // --- 2. PLL & DECODING ---
            bool sampleNow = false;
            int debugBit = -1;

            pllPhase += pllStep;

            // Zero-crossing detection on the I/Q decision signal
            if ((prevDemod < 0 && fastDemodVal >= 0) || (prevDemod >= 0 && fastDemodVal < 0)) {
                // PLL Synchronization
                float error = pllPhase - 0.5f;
                if (std::abs(error) > 0.25f) { 
                     pllPhase -= error * 0.2f; // Correction
                }
            }
            prevDemod = fastDemodVal;

            if (pllPhase >= 1.0f) {
                pllPhase -= 1.0f;
                sampleNow = true;

                // Bit Slicer
                bool currentLevel = (fastDemodVal > 0.0f);
                
                // NRZI Decoding
                int bit = (currentLevel == lastBitLevel) ? 1 : 0;
                lastBitLevel = currentLevel;
                debugBit = bit;

                processHdlcBit(bit);
            }
        }
    }

private:
    // Helper to extract Callsign and SSID from 7 AX.25 bytes
    std::string extractCallsign(const std::vector<uint8_t>& data, int offset) {
        std::string call = "";
        
        // First 6 bytes are the callsign (shifted 1 bit left)
        for (int i = 0; i < 6; i++) {
            char c = (data[offset + i] >> 1); // Correct bit shift
            if (c != ' ') call += c; // Ignore trailing spaces
        }
        
        // 7th byte is the SSID
        uint8_t ssidByte = data[offset + 6];
        int ssid = (ssidByte >> 1) & 0x0F; // Extract SSID from bits
        
        if (ssid > 0) {
            call += "-" + std::to_string(ssid);
        }
        return call;
    }

    void processHdlcBit(int bit) {
        // --- 1. Flag Detection (0x7E) ---
        if (bit == 0 && onesCount == 6) { 
            if (frameActive) {
                // Full frame received, checking length (min 16 bytes for valid APRS)
                if (rawBytes.size() > 16) { 
                    if (checkCRC(rawBytes)) {
                        // --- 2. AX.25 PARSING ---
                        try {
                            // Format: [DEST(7)] [SRC(7)] [DIGI(0-56)] [CTRL(1)] [PID(1)] [PAYLOAD...] [FCS(2)]
                            
                            // AX.25 addresses are always 7 bytes
                            std::string dest = extractCallsign(rawBytes, 0);   // Destination (e.g., APZ001)
                            std::string src  = extractCallsign(rawBytes, 7);   // Source (e.g., SQ5XYZ-9)
                            
                            // Find start of payload
                            int payloadStart = 14; // Start after DEST(7) + SRC(7)
                            while (payloadStart < rawBytes.size() - 2) {
                                // Check LSB of the last byte of the current address field (HDLC extension bit)
                                if ((rawBytes[payloadStart - 1] & 1) == 1) {
                                    break; // End of address list
                                }
                                payloadStart += 7; // Jump to next address
                            }

                            payloadStart += 2; // Skip Control (0x03) + PID (0xF0)

                            // Extract message content
                            std::string message = "";
                            for (size_t i = payloadStart; i < rawBytes.size() - 2; i++) {
                                char c = (char)rawBytes[i];
                                if (c >= 32 && c <= 126) message += c; // Printable characters only
                            }

                            // Format: SQ5XYZ-9>APZ001: Message
                            std::string formattedLog = src + ">" + dest + ":" + message;
                            
                            std::cout << " [APRS] " << formattedLog << std::endl;
                            
                            if (onMessage) onMessage(formattedLog);

                        } catch (...) {
                            std::cout << " [AX.25 ERR] Parse fail" << std::endl;
                        }
                    } 
                }
            }
            // Reset state
            frameActive = true;
            rawBytes.clear();
            onesCount = 0;
            currentByte = 0;
            bitIndex = 0;
            return;
        }

        // --- Bit Stuffing ---
        if (bit == 0 && onesCount == 5) { 
            onesCount = 0;
            return;
        }

        // --- Ones Counting (Abort detection) ---
        if (bit == 1) {
            onesCount++;
            if (onesCount > 6) { // Abort condition
                frameActive = false;
                rawBytes.clear();
                onesCount = 0;
                return;
            }
        } else {
            onesCount = 0;
        }

        // --- Assemble bits into bytes ---
        if (frameActive) {
            if (bit == 1) currentByte |= (1 << bitIndex);
            bitIndex++;
            
            if (bitIndex == 8) {
                rawBytes.push_back(currentByte);
                currentByte = 0;
                bitIndex = 0;
            }
        }
    }
};