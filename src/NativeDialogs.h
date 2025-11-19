#pragma once

#include <string>
#include <array>
#include <memory>
#include <iostream>
#include <cstdio>

#ifdef _WIN32
    #define NOMINMAX // Important: Prevents conflict between windows.h and std::min/max
    #include <windows.h>
    #include <commdlg.h>
#endif

// Display a native popup message
inline void showPopup(std::string title, std::string message) {
    #ifdef _WIN32
        MessageBoxA(NULL, message.c_str(), title.c_str(), MB_OK | MB_ICONINFORMATION);
    #elif __APPLE__
        std::string cmd = "osascript -e 'display dialog \"" + message + "\" with title \"" + title + "\" buttons {\"OK\"} default button \"OK\" with icon note'";
        system(cmd.c_str());
    #elif __linux__
        std::string cmd = "zenity --info --title=\"" + title + "\" --text=\"" + message + "\"";
        system(cmd.c_str());
    #endif
}

// Open native file dialog and return the selected path
inline std::string openFileDialog() {
    std::string result = "";
    
    #ifdef _WIN32
        // --- WINDOWS IMPLEMENTATION ---
        char filename[MAX_PATH] = {0};
        OPENFILENAMEA ofn; // ANSI Version
        ZeroMemory(&ofn, sizeof(ofn));
        
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = NULL; // No owner window
        ofn.lpstrFile = filename;
        ofn.nMaxFile = sizeof(filename);
        
        // Filter format: Description\0*.ext\0NextDesc\0*.ext\0\0
        ofn.lpstrFilter = "WAV Files\0*.wav\0All Files\0*.*\0";
        ofn.nFilterIndex = 1;
        ofn.lpstrFileTitle = NULL;
        ofn.nMaxFileTitle = 0;
        ofn.lpstrInitialDir = NULL;
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

        if (GetOpenFileNameA(&ofn)) {
            result = filename;
        }

    #elif __APPLE__
        // --- MACOS IMPLEMENTATION ---
        // Uses AppleScript to open a native finder dialog
        const char* cmd = "osascript -e 'try' -e 'POSIX path of (choose file of type {\"wav\"} with prompt \"Select Baseband WAV\")' -e 'on error' -e 'return \"\"' -e 'end try'";
        std::array<char, 128> buffer;
        std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd, "r"), pclose);
        
        if (pipe) {
            while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
                result += buffer.data();
            }
        }
        
    #elif __linux__
        // --- LINUX IMPLEMENTATION ---
        // Requires Zenity installed
        const char* cmd = "zenity --file-selection --file-filter='*.wav' --title='Select Baseband WAV'";
        std::array<char, 128> buffer;
        std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd, "r"), pclose);
        
        if (pipe) {
            while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
                result += buffer.data();
            }
        }
    #endif

    // Remove trailing newline characters (common in shell outputs)
    if (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }

    return result;
}