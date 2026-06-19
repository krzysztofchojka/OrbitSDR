#pragma once

#include <string>
#include <map>
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cstdlib> // for getenv
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

// Returns ONLY the configuration directory path (without the filename)
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

    // Create directory if it does not exist
    MKDIR(path.c_str());
    return path;
}

// Returns the full path to settings.json
inline std::string getSettingsFilePath() {
    return getConfigDir() + PATH_SEP + "settings.json";
}

// Returns the full path to aprs.log
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

    // Ensure the directory exists
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

    // ROBUST LOAD FUNCTION
    void load(std::string filename) {
        data.clear();
        std::ifstream file(filename);
        if (!file.is_open()) return;

        std::string line;
        while (std::getline(file, line)) {
            try {
                // Simple safeguard against empty lines
                if (line.length() < 3) continue;

                size_t colPos = line.find(':');
                size_t q1 = line.find('"');
                
                if (colPos != std::string::npos && q1 != std::string::npos) {
                    size_t q2 = line.find('"', q1 + 1);
                    if (q2 != std::string::npos && q2 > q1) {
                        std::string key = line.substr(q1 + 1, q2 - q1 - 1);
                        
                        // Safeguard against out-of-bounds when retrieving value
                        if (colPos + 1 < line.length()) {
                            std::string valPart = line.substr(colPos + 1);
                            
                            // Stripping JSON formatting clutter
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
                // If any line is corrupted, ignore it and continue instead of crashing the program
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
            
            // Simple heuristic: is it a number/bool or a string?
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