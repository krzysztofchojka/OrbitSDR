#pragma once
#include <SFML/Graphics.hpp>
#include <string>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <regex>
#include <cmath>
#include <fstream>
#include "Globals.h"

inline std::string getTimestamp() {
    auto now = std::time(nullptr); auto tm = *std::localtime(&now);
    std::ostringstream oss; oss << std::put_time(&tm, "[%Y-%m-%d %H:%M:%S]"); return oss.str();
}

inline std::string truncatePath(std::string path, size_t maxLen) {
    if (path.length() <= maxLen) return path;
    return "..." + path.substr(path.length() - (maxLen - 3));
}

inline std::string wrapText(const std::string& str, const sf::Font& font, unsigned int charSize, float maxWidth) {
    std::string result; std::string currentLine; std::stringstream ss(str); std::string word;
    while (ss >> word) {
        sf::Text testWord(font, word, charSize);
        if (testWord.getLocalBounds().size.x > maxWidth) {
            if (!currentLine.empty()) { result += currentLine + "\n"; currentLine = ""; }
            std::string part; for (char c : word) { part += c; sf::Text partTest(font, part, charSize); if (partTest.getLocalBounds().size.x > maxWidth - 10) { result += part + "\n"; part = ""; } } currentLine = part; continue;
        }
        std::string testLine = currentLine + (currentLine.empty() ? "" : " ") + word; sf::Text testText(font, testLine, charSize);
        if (testText.getLocalBounds().size.x > maxWidth) { if (!currentLine.empty()) { result += currentLine + "\n"; currentLine = word; } else { result += word + "\n"; } } else { currentLine = testLine; }
    } result += currentLine; return result;
}

inline void parseAprsData(std::string raw, AprsLastPacket& pkt) {
    if (raw.size() > 21 && raw[0] == '[' && raw[20] == ']') pkt.timestamp = raw.substr(0, 21);
    else pkt.timestamp = getTimestamp();
    pkt.raw = raw;
    std::string content = raw;
    if (raw.size() > 0 && raw[0] == '[') {
        size_t cb = raw.find("] ");
        if (cb != std::string::npos) content = raw.substr(cb + 2);
    }
    size_t colon = content.find(':');
    if (colon == std::string::npos) return;
    std::string header = content.substr(0, colon); std::string body = content.substr(colon + 1);
    size_t arrow = header.find('>');
    if (arrow != std::string::npos) { pkt.src = header.substr(0, arrow); pkt.dest = header.substr(arrow + 1); }
    else { pkt.src = header; pkt.dest = "?"; }
    pkt.comment = body;
    std::regex coordRegex(R"((\d{4}\.\d{2})([NS]).(\d{5}\.\d{2})([EW]))"); std::smatch match;
    if (std::regex_search(body, match, coordRegex)) {
        float latVal = std::stof(match[1]) / 100.0f; float lonVal = std::stof(match[3]) / 100.0f;
        std::stringstream ss; ss << std::fixed << std::setprecision(2) << latVal << " " << match[2].str() << ", " << lonVal << " " << match[4].str();
        pkt.coords = ss.str();
    } else pkt.coords = "";
    std::regex courseRegex(R"((\d{3})/(\d{3}))");
    if (std::regex_search(body, match, courseRegex)) {
        try { pkt.course = std::stof(match[1]); pkt.speed = std::stof(match[2]); } catch(...) { pkt.course = -1; }
    } else pkt.course = -1;
}

inline std::deque<AprsLastPacket> loadLastLogLinesAsPackets(const std::string& filename, int count) {
    std::deque<AprsLastPacket> packets; std::ifstream file(filename);
    if (!file.is_open()) return packets;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty()) {
            AprsLastPacket pkt; parseAprsData(line, pkt); packets.push_back(pkt);
            if (packets.size() > count) packets.pop_front();
        }
    }
    return packets;
}

inline sf::Color getHeatmap(float v, int theme) {
    v = std::clamp(v, 0.0f, 1.0f); 
    std::uint8_t r=0,g=0,b=0;
    
    if (theme == 0) {
        // 1. Magia kontrastu: krzywa gamma "zgniatająca" szumy tła
        // Wartości bliskie zera ciemnieją mocniej, sygnały pozostają jasne
        v = std::pow(v, 1.2f); 
        
        // 2. Wielostopniowa paleta kolorów
        if (v < 0.2f) { // Czarne tło -> Ciemny granat
            float t = v / 0.2f;
            r = 0; g = 0; b = static_cast<std::uint8_t>(t * 120);
        } else if (v < 0.4f) { // Granat -> Jasnoniebieski
            float t = (v - 0.2f) / 0.2f;
            r = 0; g = static_cast<std::uint8_t>(t * 150); b = 120 + static_cast<std::uint8_t>(t * 135);
        } else if (v < 0.6f) { // Jasnoniebieski -> Żółto-Zielony
            float t = (v - 0.4f) / 0.2f;
            r = static_cast<std::uint8_t>(t * 180); g = 150 + static_cast<std::uint8_t>(t * 105); b = 255 - static_cast<std::uint8_t>(t * 200);
        } else if (v < 0.8f) { // Żółto-Zielony -> Czysty Żółty
            float t = (v - 0.6f) / 0.2f;
            r = 180 + static_cast<std::uint8_t>(t * 75); g = 255; b = 55 - static_cast<std::uint8_t>(t * 55);
        } else if (v < 0.95f) { // Żółty -> Czerwony (Mocne sygnały)
            float t = (v - 0.8f) / 0.15f;
            r = 255; g = 255 - static_cast<std::uint8_t>(t * 220); b = 0;
        } else { // Czerwony -> Biały (Absolutne przesterowanie na czubkach)
            float t = (v - 0.95f) / 0.05f;
            r = 255; g = 35 + static_cast<std::uint8_t>(t * 220); b = static_cast<std::uint8_t>(t * 255);
        }
    } 
    else if (theme == 1 || theme == 4) { // INTELIGENTNY NEON (Synthwave z ciemnym tłem)
        v = std::pow(v, 1.2f); 
        
        if (v < 0.2f) { // Szum tła: Czarny -> Ciemny granat (uspokojone tło)
            float t = v / 0.2f;
            r = 0; g = 0; b = static_cast<std::uint8_t>(t * 120);
        } else if (v < 0.4f) { // Słabe sygnały: Ciemny granat -> Elektryczna Purpura
            float t = (v - 0.2f) / 0.2f;
            r = static_cast<std::uint8_t>(t * 150); g = 0; b = 120 + static_cast<std::uint8_t>(t * 80);
        } else if (v < 0.6f) { // Średnie sygnały: Purpura -> Gorąca Magenta (Hot Pink)
            float t = (v - 0.4f) / 0.2f;
            r = 150 + static_cast<std::uint8_t>(t * 105); g = 0; b = 200 - static_cast<std::uint8_t>(t * 50);
        } else if (v < 0.8f) { // Mocne sygnały: Magenta -> Neonowy Pomarańcz
            float t = (v - 0.6f) / 0.2f;
            r = 255; g = static_cast<std::uint8_t>(t * 120); b = 150 - static_cast<std::uint8_t>(t * 150);
        } else if (v < 0.95f) { // Bardzo mocne: Pomarańcz -> Jasny Żółto-Biały
            float t = (v - 0.8f) / 0.15f;
            r = 255; g = 120 + static_cast<std::uint8_t>(t * 100); b = static_cast<std::uint8_t>(t * 100);
        } else { // Przesterowanie/Piki: Żółto-Biały -> Czysta Biel
            float t = (v - 0.95f) / 0.05f;
            r = 255; g = 220 + static_cast<std::uint8_t>(t * 35); b = 100 + static_cast<std::uint8_t>(t * 155);
        }
    }
    else if (theme == 2) { // Matrix
        v = std::pow(v, 1.2f);
        r = 0; if(v < 0.5f) { g = static_cast<std::uint8_t>(v * 2 * 200); b=0; } else { g = 200 + static_cast<std::uint8_t>((v-0.5f)*2*55); b = static_cast<std::uint8_t>((v-0.5f)*2*255); r = b; }
    } 
    else if (theme == 3) { // Grayscale
        std::uint8_t lum = static_cast<std::uint8_t>(std::pow(v, 1.2f) * 255); r = lum; g = lum; b = lum;
    }
    
    return {r, g, b};
}

inline std::string formatHz(long long hz) { std::stringstream ss; ss << std::fixed << std::setprecision(3) << (hz / 1000000.0) << " MHz"; return ss.str(); }

inline void drawGrid(sf::RenderWindow& window, const sf::Font& font, float x, float y, float w, float h, long long cf, double hwSampleRate, double viewStartPct, double viewEndPct, float minDb, float maxDb) {
    float dbStep = 20.0f;
    for (float db = 0; db >= -140; db -= dbStep) {
        if (db > maxDb || db < minDb) continue;
        float norm = (db - minDb) / (maxDb - minDb); float yPos = y + h - (norm * h);
        sf::RectangleShape line({w, 1}); line.setPosition(sf::Vector2f(x, yPos)); line.setFillColor(sf::Color(80, 80, 80, 100)); window.draw(line);
        sf::Text l(font, std::to_string((int)db), 20); l.setScale(sf::Vector2f(0.5f, 0.5f));
        l.setPosition(sf::Vector2f(x+2, yPos-12)); l.setFillColor(sf::Color(200, 200, 200, 150)); window.draw(l);
    }
    double startFreq = (double)cf + (viewStartPct - 0.5) * hwSampleRate;
    double endFreq = (double)cf + (viewEndPct - 0.5) * hwSampleRate;
    double visibleSpan = endFreq - startFreq;
    double stepHz = 200000.0;
    if (visibleSpan < 1000000) stepHz = 100000.0; if (visibleSpan < 500000) stepHz = 50000.0; if (visibleSpan < 200000) stepHz = 25000.0; if (visibleSpan > 5000000) stepHz = 500000.0; if (visibleSpan > 10000000) stepHz = 1000000.0;
    long long firstLineFreq = (long long)(ceil(startFreq / stepHz) * stepHz);
    for (double f = (double)firstLineFreq; f < endFreq; f += stepHz) {
        float normPos = (float)((f - startFreq) / visibleSpan); float xPos = x + normPos * w;
        sf::RectangleShape line({1, h}); line.setPosition(sf::Vector2f(xPos, y)); line.setFillColor(sf::Color(80, 80, 80, 100)); window.draw(line);
        std::string freqStr = formatHz((long long)f); if(freqStr.size() > 4) freqStr = freqStr.substr(0, freqStr.size()-4);
        sf::Text l(font, freqStr, 20); l.setScale(sf::Vector2f(0.5f, 0.5f));
        sf::FloatRect b = l.getGlobalBounds(); l.setPosition(sf::Vector2f(xPos - b.size.x/2, y + h - 15)); l.setFillColor(sf::Color(220, 220, 220, 200)); window.draw(l);
    }
}