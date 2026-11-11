#pragma once

#include <string>
#include <array>
#include <memory>
#include <iostream>
#include <cstdio>
#include <cctype>

#ifdef _WIN32
    // --- FIX: check whether NOMINMAX is already defined ---
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <commdlg.h>
    #include <shlobj.h> // Required for directory browsing
#endif

// Displays a native message box/alert
inline void showPopup(std::string title, std::string message) {
    #ifdef _WIN32
        MessageBoxA(NULL, message.c_str(), title.c_str(), MB_OK | MB_ICONINFORMATION);
    #elif __APPLE__
        std::string cmd = "osascript -e 'display dialog \"" + message + "\" with title \"" + title + "\" buttons {\"OK\"} default button \"OK\" with icon note'";
        int ret = system(cmd.c_str()); (void)ret;
    #elif __linux__
        std::string cmd = "zenity --info --title=\"" + title + "\" --text=\"" + message + "\"";
        int ret = system(cmd.c_str()); (void)ret;
    #endif
}

// Opens a native file picker dialog (filtered for WAV files)
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
        
        if (GetOpenFileNameA(&ofn)) {
            result = filename;
        }
    #elif __APPLE__
        const char* cmd = "osascript -e 'try' -e 'POSIX path of (choose file of type {\"wav\"} with prompt \"Select Baseband WAV\")' -e 'on error' -e 'return \"\"' -e 'end try'";
        std::array<char, 128> buffer;
        std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd, "r"), pclose);
        if (pipe) {
            while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
                result += buffer.data();
            }
        }
    #elif __linux__
        const char* cmd = "zenity --file-selection --file-filter='*.wav' --title='Select Baseband WAV'";
        std::array<char, 128> buffer;
        std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd, "r"), pclose);
        if (pipe) {
            while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
                result += buffer.data();
            }
        }
    #endif
    
    // Trim trailing newline characters
    if (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }
    return result;
}

// Opens a native folder selection dialog
inline std::string selectFolderDialog(const std::string& title = "Select Folder for Recordings") {
    std::string result = "";
    #ifdef _WIN32
        BROWSEINFOA bi = { 0 };
        bi.lpszTitle = title.c_str();
        LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
        
        if (pidl != 0) {
            char path[MAX_PATH];
            if (SHGetPathFromIDListA(pidl, path)) {
                result = path;
            }
            CoTaskMemFree(pidl);
        }
    #elif __APPLE__
        std::string cmd = "osascript -e 'try' -e 'POSIX path of (choose folder with prompt \"" + title + "\")' -e 'on error' -e 'return \"\"' -e 'end try'";
        std::array<char, 128> buffer;
        std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
        if (pipe) {
            while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
                result += buffer.data();
            }
        }
    #elif __linux__
        std::string cmd = "zenity --file-selection --directory --title='" + title + "'";
        std::array<char, 128> buffer;
        std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
        if (pipe) {
            while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
                result += buffer.data();
            }
        }
    #endif
    
    // Trim trailing newline characters
    if (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }
    return result;
}

// Opens a native "Save as" dialog for a PNG picture, starting in 'folder' with the
// suggested file name. Returns the chosen path, or an empty string when cancelled.
inline std::string savePictureDialog(const std::string& folder, const std::string& fileName) {
    std::string result = "";
    #ifdef _WIN32
        char filename[MAX_PATH] = {0};
        fileName.copy(filename, MAX_PATH - 1);
        std::string initialDir = folder;
        for (auto& c : initialDir) if (c == '/') c = '\\';
        OPENFILENAMEA ofn;
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = NULL;
        ofn.lpstrFile = filename;
        ofn.nMaxFile = sizeof(filename);
        ofn.lpstrFilter = "PNG image\0*.png\0All Files\0*.*\0";
        ofn.nFilterIndex = 1;
        ofn.lpstrInitialDir = initialDir.c_str();
        ofn.lpstrDefExt = "png";
        ofn.lpstrTitle = "Save picture";
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetSaveFileNameA(&ofn)) result = filename;
    #elif __APPLE__
        std::string cmd = "osascript -e 'try' -e 'POSIX path of (choose file name with prompt \"Save picture\" default name \"" + fileName +
                          "\" default location (POSIX file \"" + folder + "\"))' -e 'on error' -e 'return \"\"' -e 'end try'";
        std::array<char, 256> buffer;
        std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
        if (pipe) while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) result += buffer.data();
    #elif __linux__
        std::string cmd = "zenity --file-selection --save --confirm-overwrite --title='Save picture' --filename='" + folder + "/" + fileName + "'";
        std::array<char, 256> buffer;
        std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
        if (pipe) while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) result += buffer.data();
    #endif
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) result.pop_back();
    // Make sure the file gets the .png extension
    if (!result.empty()) {
        std::string lower = result;
        for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
        if (lower.size() < 4 || lower.compare(lower.size() - 4, 4, ".png") != 0) result += ".png";
    }
    return result;
}

// Opens a native "Save as" dialog for a text/log file, starting in 'folder' with the
// suggested file name. Returns the chosen path, or an empty string when cancelled.
inline std::string saveTextFileDialog(const std::string& folder, const std::string& fileName,
                                      const std::string& title = "Save log") {
    std::string result = "";
    #ifdef _WIN32
        char filename[MAX_PATH] = {0};
        fileName.copy(filename, MAX_PATH - 1);
        std::string initialDir = folder;
        for (auto& c : initialDir) if (c == '/') c = '\\';
        OPENFILENAMEA ofn;
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = NULL;
        ofn.lpstrFile = filename;
        ofn.nMaxFile = sizeof(filename);
        ofn.lpstrFilter = "Text / log file\0*.txt;*.log\0All Files\0*.*\0";
        ofn.nFilterIndex = 1;
        ofn.lpstrInitialDir = initialDir.c_str();
        ofn.lpstrDefExt = "txt";
        ofn.lpstrTitle = title.c_str();
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetSaveFileNameA(&ofn)) result = filename;
    #elif __APPLE__
        std::string cmd = "osascript -e 'try' -e 'POSIX path of (choose file name with prompt \"" + title + "\" default name \"" + fileName +
                          "\" default location (POSIX file \"" + folder + "\"))' -e 'on error' -e 'return \"\"' -e 'end try'";
        std::array<char, 256> buffer;
        std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
        if (pipe) while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) result += buffer.data();
    #elif __linux__
        std::string cmd = "zenity --file-selection --save --confirm-overwrite --title='" + title + "' --filename='" + folder + "/" + fileName + "'";
        std::array<char, 256> buffer;
        std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
        if (pipe) while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) result += buffer.data();
    #endif
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) result.pop_back();
    return result;
}

// Opens a file (e.g. a log) in the system's default application
inline void openWithDefaultApp(const std::string& path) {
    #ifdef _WIN32
        ShellExecuteA(NULL, "open", path.c_str(), NULL, NULL, SW_SHOWNORMAL);
    #elif __APPLE__
        std::string cmd = "open \"" + path + "\" &";
        int ret = system(cmd.c_str()); (void)ret;
    #elif __linux__
        std::string cmd = "xdg-open \"" + path + "\" >/dev/null 2>&1 &";
        int ret = system(cmd.c_str()); (void)ret;
    #endif
}
