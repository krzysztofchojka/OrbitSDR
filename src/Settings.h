#pragma once

#include <string>
#include <map>
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cstdlib> // dla getenv
#include <vector>

#ifdef _WIN32
    #include <direct.h>
    #define MKDIR(path) _mkdir(path)
    const char PATH_SEP = '\\';
#else
    #include <sys/stat.h>
    #include <sys/types.h>
    #define MKDIR(path) mkdir(path, 0755)
    const char PATH_SEP = '/';
#endif

// Funkcja zwracająca TYLKO katalog konfiguracyjny (bez nazwy pliku)
inline std::string getConfigDir() {
    std::string path;
    #ifdef _WIN32
        const char* appData = getenv("APPDATA");
        if (appData) path = std::string(appData) + "\\OrbitSDR";
        else path = "OrbitSDR_Config";
    #else
        const char* home = getenv("HOME");
        if (home) path = std::string(home) + "/.config/orbitsdr";
        else path = "./";
    #endif

    // Tworzymy katalog, jeśli nie istnieje
    MKDIR(path.c_str());
    return path;
}

// Funkcja zwracająca pełną ścieżkę do settings.json
inline std::string getSettingsFilePath() {
    return getConfigDir() + PATH_SEP + "settings.json";
}

// Funkcja zwracająca pełną ścieżkę do aprs.log
inline std::string getAprsLogFilePath() {
    return getConfigDir() + PATH_SEP + "aprs.log";
}

inline std::string getDefaultRecordingPath() {
    std::string path;
    #ifdef _WIN32
        const char* userProfile = getenv("USERPROFILE");
        if (userProfile) path = std::string(userProfile) + "\\Music\\OrbitSDR";
        else path = "C:\\OrbitSDR_Recordings";
    #else
        const char* home = getenv("HOME");
        if (home) path = std::string(home) + "/Music/OrbitSDR";
        else path = "/tmp/OrbitSDR";
    #endif

    // Upewnij się, że katalog istnieje
    MKDIR(path.c_str());
    return path;
}

class SettingsManager {
public:
    std::map<std::string, std::string> data;

    void set(std::string key, std::string value) { data[key] = value; }
    void set(std::string key, int value) { data[key] = std::to_string(value); }
    void set(std::string key, float value) { data[key] = std::to_string(value); }
    void set(std::string key, double value) { data[key] = std::to_string(value); }
    void set(std::string key, bool value) { data[key] = value ? "true" : "false"; }

    std::string getString(std::string key, std::string defaultVal) {
        if (data.find(key) != data.end()) return data[key];
        return defaultVal;
    }

    int getInt(std::string key, int defaultVal) {
        if (data.find(key) != data.end()) {
            try { return std::stoi(data[key]); } catch (...) {}
        }
        return defaultVal;
    }

    float getFloat(std::string key, float defaultVal) {
        if (data.find(key) != data.end()) {
            try { return std::stof(data[key]); } catch (...) {}
        }
        return defaultVal;
    }

    bool getBool(std::string key, bool defaultVal) {
        if (data.find(key) != data.end()) {
            return (data[key] == "true");
        }
        return defaultVal;
    }

    // PANCERNA FUNKCJA LOAD
    void load(std::string filename) {
        data.clear();
        std::ifstream file(filename);
        if (!file.is_open()) return;

        std::string line;
        while (std::getline(file, line)) {
            try {
                // Proste zabezpieczenie przed pustymi liniami
                if (line.length() < 3) continue;

                size_t colPos = line.find(':');
                size_t q1 = line.find('"');
                
                if (colPos != std::string::npos && q1 != std::string::npos) {
                    size_t q2 = line.find('"', q1 + 1);
                    if (q2 != std::string::npos && q2 > q1) {
                        std::string key = line.substr(q1 + 1, q2 - q1 - 1);
                        
                        // Zabezpieczenie przed wyjściem poza zakres przy pobieraniu wartości
                        if (colPos + 1 < line.length()) {
                            std::string valPart = line.substr(colPos + 1);
                            
                            // Czyszczenie śmieci JSONowych
                            valPart.erase(std::remove(valPart.begin(), valPart.end(), ','), valPart.end());
                            valPart.erase(std::remove(valPart.begin(), valPart.end(), '"'), valPart.end());
                            valPart.erase(std::remove(valPart.begin(), valPart.end(), ' '), valPart.end());
                            valPart.erase(std::remove(valPart.begin(), valPart.end(), '\r'), valPart.end());
                            valPart.erase(std::remove(valPart.begin(), valPart.end(), '\n'), valPart.end());
                            
                            if (!key.empty()) {
                                data[key] = valPart;
                            }
                        }
                    }
                }
            } catch (...) {
                // Jeśli jakakolwiek linia jest uszkodzona, ignorujemy ją i idziemy dalej
                // zamiast wywalać program
                std::cerr << "[Warning] Skipping corrupted line in settings.json" << std::endl;
            }
        }
    }

    void save(std::string filename) {
        std::ofstream file(filename);
        if (!file.is_open()) return;

        file << "{\n";
        int count = 0;
        for (auto const& [key, val] : data) {
            file << "  \"" << key << "\": ";
            
            // Prosta heurystyka: czy to liczba/bool czy string?
            bool isNum = !val.empty() && (isdigit(val[0]) || val[0] == '-');
            bool isBool = (val == "true" || val == "false");
            
            if (isNum || isBool) file << val;
            else file << "\"" << val << "\"";

            if (count < data.size() - 1) file << ",";
            file << "\n";
            count++;
        }
        file << "}\n";
    }
};