#pragma once
#include "../Core/SDRModule.h"
#include "../../DSP.h" 
#include <deque>
#include <vector>
#include <complex>
#include <iostream>
#include <algorithm>
#include <mutex>
#include <cmath>

#define AIS_RATE 48000.0 
#define SAMPLES_PER_BIT 5.0f

// --- STRUKTURY DANYCH ---
struct AISMessage {
    int mmsi;
    std::string type;
    std::string info;
    std::string shipName;
    bool crcOk;
};

// --- NARZĘDZIA (CRC, HEX) ---
namespace AisTools {
    static const uint16_t fcstab[256] = {
        0x0000, 0x1189, 0x2312, 0x329b, 0x4624, 0x57ad, 0x6536, 0x74bf, 0x8c48, 0x9dc1, 0xaf5a, 0xbed3, 0xca6c, 0xdbe5, 0xe97e, 0xf8f7,
        0x1081, 0x0108, 0x3393, 0x221a, 0x56a5, 0x472c, 0x75b7, 0x643e, 0x9cc9, 0x8d40, 0xbfdb, 0xae52, 0xdaed, 0xcb64, 0xf9ff, 0xe876,
        0x2102, 0x308b, 0x0210, 0x1399, 0x6726, 0x76af, 0x4434, 0x55bd, 0xad4a, 0xbcc3, 0x8e58, 0x9fd1, 0xeb6e, 0xfae7, 0xc87c, 0xd9f5,
        0x3183, 0x200a, 0x1291, 0x0318, 0x77a7, 0x662e, 0x54b5, 0x453c, 0xbdcb, 0xac42, 0x9ed9, 0x8f50, 0xfbef, 0xea66, 0xd8fd, 0xc974,
        0x4204, 0x538d, 0x6116, 0x709f, 0x0420, 0x15a9, 0x2732, 0x36bb, 0xce4c, 0xdfc5, 0xed5e, 0xfcd7, 0x8868, 0x99e1, 0xab7a, 0xbaf3,
        0x5285, 0x430c, 0x7197, 0x601e, 0x14a1, 0x0528, 0x37b3, 0x263a, 0xdecd, 0xcf44, 0xfddf, 0xec56, 0x98e9, 0x8960, 0xbbfb, 0xaa72,
        0x6306, 0x728f, 0x4014, 0x519d, 0x2522, 0x34ab, 0x0630, 0x17b9, 0xef4e, 0xfec7, 0xcc5c, 0xddd5, 0xa96a, 0xb8e3, 0x8a78, 0x9bf1,
        0x7387, 0x620e, 0x5095, 0x411c, 0x35a3, 0x242a, 0x16b1, 0x0738, 0xffcf, 0xee46, 0xdcdd, 0xcd54, 0xb9eb, 0xa862, 0x9af9, 0x8b70,
        0x8408, 0x9581, 0xa71a, 0xb693, 0xc22c, 0xd3a5, 0xe13e, 0xf0b7, 0x0840, 0x19c9, 0x2b52, 0x3adb, 0x4e64, 0x5fed, 0x6d76, 0x7cff,
        0x9489, 0x8500, 0xb79b, 0xa612, 0xd2ad, 0xc324, 0xf1bf, 0xe036, 0x18c1, 0x0948, 0x3bd3, 0x2a5a, 0x5ee5, 0x4f6c, 0x7df7, 0x6c7e,
        0xa50a, 0xb483, 0x8618, 0x9791, 0xe32e, 0xf2a7, 0xc03c, 0xd1b5, 0x2942, 0x38cb, 0x0a50, 0x1bd9, 0x6f66, 0x7eef, 0x4c74, 0x5dfd,
        0xb58b, 0xa402, 0x9699, 0x8710, 0xf3af, 0xe226, 0xd0bd, 0xc134, 0x39c3, 0x284a, 0x1ad1, 0x0b58, 0x7fe7, 0x6e6e, 0x5cf5, 0x4d7c,
        0xc60c, 0xd785, 0xe51e, 0xf497, 0x8028, 0x91a1, 0xa33a, 0xb2b3, 0x4a44, 0x5bcd, 0x6956, 0x78df, 0x0c60, 0x1de9, 0x2f72, 0x3efb,
        0xd68d, 0xc704, 0xf59f, 0xe416, 0x90a9, 0x8120, 0xb3bb, 0xa232, 0x5ac5, 0x4b4c, 0x79d7, 0x685e, 0x1ce1, 0x0d68, 0x3ff3, 0x2e7a,
        0xe70e, 0xf687, 0xc41c, 0xd595, 0xa12a, 0xb0a3, 0x8238, 0x93b1, 0x6b46, 0x7acf, 0x4854, 0x59dd, 0x2d62, 0x3ceb, 0x0e70, 0x1ff9,
        0xf78f, 0xe606, 0xd49d, 0xc514, 0xb1ab, 0xa022, 0x92b9, 0x8330, 0x7bc7, 0x6a4e, 0x58d5, 0x495c, 0x3de3, 0x2c6a, 0x1ef1, 0x0f78
    };
    inline bool checkFCS(const std::vector<uint8_t>& bits) {
        if (bits.size() < 16) return false;
        uint16_t fcs = 0xFFFF; uint8_t currentByte = 0; int bitCount = 0;
        for (uint8_t bit : bits) {
            currentByte >>= 1; if (bit) currentByte |= 0x80; bitCount++;
            if (bitCount == 8) { fcs = (fcs >> 8) ^ fcstab[(fcs ^ currentByte) & 0xff]; bitCount = 0; currentByte = 0; }
        }
        return (fcs == 0xf0b8);
    }
    inline uint32_t getBits(const std::vector<uint8_t>& b, int start, int len) {
        uint32_t r = 0; for(int i=0; i<len; ++i) if(start + i < (int)b.size()) r = (r << 1) | b[start + i]; return r;
    }
    inline std::string getStr(const std::vector<uint8_t>& b, int start, int len) {
        std::string res = ""; const char* map = "@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^- !\"#$%&'()*+,-./0123456789:;<=>?";
        for(int i=0; i<len; i+=6) { uint8_t val = (uint8_t)getBits(b, start+i, 6); if (val < 64) res += map[val]; }
        res.erase(std::find_if(res.rbegin(), res.rend(), [](unsigned char ch) { return !std::isspace(ch) && ch != '@'; }).base(), res.end());
        return res;
    }
}

// --- AGC ---
struct IQ_AGC {
    float gain = 1.0f; float targetLevel = 0.8f; float maxGain = 30000.0f; float attack = 0.15f; float decay = 0.02f; 
    std::complex<float> process(std::complex<float> in) {
        float mag = std::abs(in);
        if (mag > 0.0000001f) { float error = targetLevel - (mag * gain); if (error < 0) gain += error * attack; else gain += error * decay; } else { gain += decay; }
        if (gain > maxGain) gain = maxGain; if (gain < 1.0f) gain = 1.0f; return in * gain;
    }
};

// --- SILNIK KORELACJI I DEKODOWANIA ---
class AISCorrelationDecoder {
public:
    std::vector<float> buffer;
    bool locked = false;
    int samplesSinceLock = 0;
    int bitsRead = 0;
    
    // NRZI State
    bool lastLevel = false;
    int onesCount = 0;
    std::vector<uint8_t> frameBits;
    
    // Wzorzec preambuły (fala 2.4kHz przy próbkowaniu 48kHz = okres 20 próbek)
    // 10 próbek High, 10 próbek Low
    std::vector<float> preambleTemplate;

    AISCorrelationDecoder() {
        // Generujemy wzorzec 2.4kHz (idealna preambuła AIS w NRZI/FM)
        for(int i=0; i<20; i++) {
            if(i < 10) preambleTemplate.push_back(1.0f);
            else preambleTemplate.push_back(-1.0f);
        }
        buffer.reserve(48000); // 1 sekunda
    }

    void reset() {
        buffer.clear();
        locked = false;
        frameBits.clear();
    }

    // Zwraca true jeśli zdekodowano ramkę
    bool pushSample(float sample, std::vector<uint8_t>& outBits) {
        // Dodaj do bufora (okno przesuwne 20 próbek dla korelacji)
        buffer.push_back(sample);
        
        // Czyść bufor, żeby nie zjadł pamięci, ale zostaw margines na korelację
        if (!locked && buffer.size() > 40) {
            buffer.erase(buffer.begin(), buffer.begin() + (buffer.size() - 40));
        }

        if (locked) {
            samplesSinceLock++;
            // Próbkuj co 5 próbek (środek bitu)
            // Ale musimy trafić w środek. Skoro korelacja była na początku, to pierwszy bit jest w sampleSinceLock = 2.5 (zaokrąglamy do 2 lub 3)
            if (samplesSinceLock % 5 == 2) {
                // SAMPLE!
                bool level = (sample > 0.0f);
                return processBit(level, outBits);
            }
            
            // Limit długości ramki (żeby nie wisiał w nieskończoność)
            if (bitsRead > 300) {
                locked = false; // Timeout
            }
        } 
        else {
            // Szukamy korelacji
            if (buffer.size() >= 20) {
                float score = 0.0f;
                // Mnożymy ostatnie 20 próbek przez wzorzec
                for(int i=0; i<20; i++) {
                    score += buffer[buffer.size() - 20 + i] * preambleTemplate[i];
                }
                
                // Próg detekcji (doświadczalnie: 10.0 oznacza, że 10 próbek idealnie pasowało amplitudą 1.0)
                if (score > 12.0f) {
                    locked = true;
                    samplesSinceLock = 0;
                    bitsRead = 0;
                    onesCount = 0;
                    frameBits.clear();
                    lastLevel = (sample > 0); // Init NRZI
                    // std::cout << "[AIS] SYNC FOUND! Score: " << score << std::endl;
                }
            }
        }
        return false;
    }

    bool processBit(bool level, std::vector<uint8_t>& outBits) {
        bitsRead++;
        int bit = (level != lastLevel) ? 0 : 1; // NRZI Decode
        lastLevel = level;

        if (bit == 1) {
            onesCount++;
            if (onesCount == 6) { // Flaga 0x7E (koniec ramki)
                locked = false; // Koniec pakietu
                outBits = frameBits;
                return true; // Mamy ramkę!
            }
        } else {
            if (onesCount == 5) { onesCount = 0; return false; } // De-stuffing
            onesCount = 0;
        }

        frameBits.push_back(bit);
        return false;
    }
};

class AISDecoder : public SDRModule {
private:
    Channelizer channelA; 
    IQ_AGC agc; 
    
    std::complex<float> lastSample = {1.0f, 0.0f}; 
    float avgDc = 0.0f;
    float lpfState = 0.0f;

    // DWIE GŁOWICE KORELACYJNE
    AISCorrelationDecoder decNormal;
    AISCorrelationDecoder decInverted;

    std::deque<AISMessage> log; 
    std::vector<float> scope; 
    std::mutex mtx; 
    const sf::Font* fontRef = nullptr;
    double lastSr = 0; 
    int debugCounter = 0;

public:
    AISDecoder() : SDRModule("AIS Pro (Correlation)") {
        // Inverted decoder potrzebuje odwróconego wzorca
        for(auto& v : decInverted.preambleTemplate) v *= -1.0f;
    }
    
    void init(const ModuleContext& ctx) override { fontRef = &ctx.font; }

    void reset() override {
        lastSr = 0; channelA = Channelizer(); agc = IQ_AGC(); 
        lastSample = {1.0f, 0.0f}; avgDc = 0.0f; lpfState = 0.0f;
        decNormal.reset(); decInverted.reset();
        { std::lock_guard<std::mutex> l(mtx); scope.clear(); }
        std::cout << "[AIS] State Reset" << std::endl;
    }

    void handleMessage(std::vector<uint8_t> bits, bool inv) {
        if (AisTools::checkFCS(bits)) {
            if (bits.size() > 16) bits.resize(bits.size() - 16);
            int msgType = AisTools::getBits(bits, 0, 6);
            int mmsi = AisTools::getBits(bits, 8, 30);
            
            AISMessage m; 
            m.mmsi = mmsi; 
            m.type = "Type " + std::to_string(msgType);
            m.crcOk = true;
            m.info = "SOG: ?"; 
            
            if (msgType >= 1 && msgType <= 3) {
                float speed = AisTools::getBits(bits, 50, 10) / 10.0f;
                m.info = "SOG: " + std::to_string(speed) + " kn";
            } else if (msgType == 5) {
                m.shipName = AisTools::getStr(bits, 112, 120);
                m.info = m.shipName;
            }

            if (inv) m.type += " [INV]";
            
            std::lock_guard<std::mutex> l(mtx);
            log.push_front(m); 
            if(log.size() > 15) log.pop_back();
            std::cout << "\033[1;32m>>> CORRELATION MATCH: MMSI " << m.mmsi << "\033[0m" << std::endl;
        } else {
            // std::cout << "[CRC FAIL] Len: " << bits.size() << std::endl;
        }
    }

    void processIQ(const std::vector<std::complex<double>>& iqIn, double sampleRate, double tunedOffset) override {
        if (!enabled) return;
        if (std::abs(sampleRate - lastSr) > 100.0) {
            lastSr = sampleRate; channelA.configure(sampleRate, AIS_RATE); 
            std::cout << "[AIS] Correlator Configured. Rate: 48000" << std::endl;
        }
        channelA.setCenter(tunedOffset, sampleRate);
        std::complex<float> baseband;
        for (const auto& s : iqIn) {
            if (channelA.process(s, baseband)) {
                baseband *= 50.0f; 
                baseband = agc.process(baseband); 
                runDemod(baseband);
            }
        }
    }

    void runDemod(std::complex<float> sample) {
        // FM Demod
        std::complex<float> d = sample * std::conj(lastSample);
        lastSample = sample;
        float fm = std::arg(d); 
        avgDc = 0.99f * avgDc + 0.01f * fm; 
        float rawVal = fm - avgDc;
        lpfState += 0.6f * (rawVal - lpfState);
        float val = lpfState;

        // Scope
        { std::lock_guard<std::mutex> l(mtx); scope.push_back(val * 2.0f); if (scope.size() > 500) scope.erase(scope.begin(), scope.begin() + (scope.size() - 500)); }

        // --- PUSH TO CORRELATORS ---
        std::vector<uint8_t> bits;
        if (decNormal.pushSample(val, bits)) handleMessage(bits, false);
        if (decInverted.pushSample(val, bits)) handleMessage(bits, true);
    }

    void draw(sf::RenderWindow& win, float x, float y, float w, float h) override {
        if (!enabled) return;
        sf::RectangleShape bg({w, h}); bg.setPosition({x, y}); bg.setFillColor(sf::Color(20, 25, 30, 230)); 
        bg.setOutlineColor(Theme::Accent); bg.setOutlineThickness(1); win.draw(bg);
        sf::Text title(*fontRef, "AIS PRO (Correlation Engine)", 16); title.setPosition({x + 10, y + 5}); title.setFillColor(Theme::Accent); win.draw(title);
        float scopeH = 60.0f; float scopeY = y + h - scopeH - 10;
        sf::RectangleShape scopeBg({w - 20, scopeH}); scopeBg.setPosition({x + 10, scopeY}); scopeBg.setFillColor(sf::Color(0, 0, 0, 100)); win.draw(scopeBg);
        std::lock_guard<std::mutex> lock(mtx);
        if (!scope.empty()) {
            sf::VertexArray line(sf::PrimitiveType::LineStrip, scope.size());
            for (size_t i = 0; i < scope.size(); i++) {
                float val = scope[i]; float px = x + 10 + (i * (w - 20) / 500.0f); float py = scopeY + (scopeH / 2) - (val * (scopeH / 2));
                py = std::clamp(py, scopeY, scopeY + scopeH); line[i].position = {px, py}; line[i].color = sf::Color::Green;
            }
            win.draw(line);
        }
        float listY = y + 35.0f;
        for (const auto& m : log) {
            std::string line = "[" + m.type + "] " + std::to_string(m.mmsi) + " " + m.shipName + " " + m.info;
            sf::Text t(*fontRef, line, 14); t.setPosition({x + 10, listY}); t.setFillColor(sf::Color::White); win.draw(t); listY += 18.0f; if (listY > scopeY) break;
        }
    }
};