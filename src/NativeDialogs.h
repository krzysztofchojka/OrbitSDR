#pragma once

#include <string>
#include <array>
#include <memory>
#include <iostream>
#include <cstdio>

#ifdef _WIN32
    #define NOMINMAX
    #include <windows.h>
    #include <commdlg.h>
    #include <shlobj.h> // Dla przeglądania folderów
#endif

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

inline std::string openFileDialog() {
    std::string result = "";
    #ifdef _WIN32
        char filename[MAX_PATH] = {0};
        OPENFILENAMEA ofn;
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = NULL;
        ofn.lpstrFile = filename;
        ofn.nMaxFile = sizeof(filename);
        ofn.lpstrFilter = "WAV Files\0*.wav\0All Files\0*.*\0";
        ofn.nFilterIndex = 1;
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameA(&ofn)) result = filename;
    #elif __APPLE__
        const char* cmd = "osascript -e 'try' -e 'POSIX path of (choose file of type {\"wav\"} with prompt \"Select Baseband WAV\")' -e 'on error' -e 'return \"\"' -e 'end try'";
        std::array<char, 128> buffer;
        std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd, "r"), pclose);
        if (pipe) while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) result += buffer.data();
    #elif __linux__
        const char* cmd = "zenity --file-selection --file-filter='*.wav' --title='Select Baseband WAV'";
        std::array<char, 128> buffer;
        std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd, "r"), pclose);
        if (pipe) while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) result += buffer.data();
    #endif
    if (!result.empty() && (result.back() == '\n' || result.back() == '\r')) result.pop_back();
    return result;
}

// NOWOŚĆ: Wybór folderu
inline std::string selectFolderDialog() {
    std::string result = "";
    #ifdef _WIN32
        BROWSEINFOA bi = { 0 };
        bi.lpszTitle = "Select Folder for Recordings";
        LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
        if (pidl != 0) {
            char path[MAX_PATH];
            if (SHGetPathFromIDListA(pidl, path)) result = path;
            CoTaskMemFree(pidl);
        }
    #elif __APPLE__
        const char* cmd = "osascript -e 'try' -e 'POSIX path of (choose folder with prompt \"Select Recording Folder\")' -e 'on error' -e 'return \"\"' -e 'end try'";
        std::array<char, 128> buffer;
        std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd, "r"), pclose);
        if (pipe) while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) result += buffer.data();
    #elif __linux__
        const char* cmd = "zenity --file-selection --directory --title='Select Recording Folder'";
        std::array<char, 128> buffer;
        std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd, "r"), pclose);
        if (pipe) while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) result += buffer.data();
    #endif
    if (!result.empty() && (result.back() == '\n' || result.back() == '\r')) result.pop_back();
    return result;
}