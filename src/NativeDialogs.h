#pragma once

#include <string>
#include <array>
#include <memory>
#include <iostream>
#include <cstdio>
#include <cctype>
#include <functional>
#include <thread>

#ifdef _WIN32
    // --- FIX: check whether NOMINMAX is already defined ---
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <commdlg.h>
    #include <shlobj.h> // Required for directory browsing
#else
    #include <future>
    #include <chrono>
    #include <sys/wait.h>
#endif

// Called repeatedly while an external dialog program (zenity, osascript) is open. main.cpp
// sets it to drain the window's events: a window that stops answering the window manager
// gets GNOME's "OrbitSDR is not responding - Force quit / Wait" after ~5 s, which looks
// like a crash. (On Windows the dialogs run in-process and keep the window answering.)
inline std::function<void()>& dialogPump() {
    static std::function<void()> pump;
    return pump;
}
// The thread that owns the window (set with the pump): only it may drain the window's events.
inline std::thread::id& dialogPumpThread() {
    static std::thread::id id;
    return id;
}

#ifndef _WIN32
// Runs a dialog command and returns what it printed. The command runs on its own thread;
// meanwhile this (UI) thread keeps the window responsive through dialogPump().
inline std::string runDialog(const std::string& cmd) {
    auto task = std::async(std::launch::async, [cmd]() {
        std::string out;
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) return out;
        std::array<char, 256> buffer;
        while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) out += buffer.data();
        const int status = pclose(pipe);
        if (status != -1 && WIFEXITED(status) && WEXITSTATUS(status) == 127) {
            const std::string prog = cmd.substr(0, cmd.find(' '));
            std::fprintf(stderr, "[dialog] '%s' was not found%s\n", prog.c_str(),
                         prog == "zenity" ? ": install it (sudo apt install zenity) to pick files" : "");
        }
        return out;
    });
    const bool pump = dialogPump() && std::this_thread::get_id() == dialogPumpThread();
    while (task.wait_for(std::chrono::milliseconds(30)) != std::future_status::ready)
        if (pump) dialogPump()();
    return task.get();
}
#endif

// Displays a native message box/alert
inline void showPopup(std::string title, std::string message) {
    #ifdef _WIN32
        MessageBoxA(NULL, message.c_str(), title.c_str(), MB_OK | MB_ICONINFORMATION);
    #elif __APPLE__
        // AppleScript string (\ and " escaped) inside a single-quoted shell argument (' -> '\'')
        auto as = [](const std::string& t) {
            std::string o;
            for (char c : t) {
                if (c == '\\' || c == '"') o += '\\';
                if (c == '\'') { o += "'\\''"; continue; }
                o += c;
            }
            return o;
        };
        std::string cmd = "osascript -e 'display dialog \"" + as(message) + "\" with title \"" + as(title) + "\" buttons {\"OK\"} default button \"OK\" with icon note'";
        runDialog(cmd);
    #elif __linux__
        // double-quoted shell argument: \ " $ ` escaped
        auto sh = [](const std::string& t) {
            std::string o;
            for (char c : t) { if (c == '\\' || c == '"' || c == '$' || c == '`') o += '\\'; o += c; }
            return o;
        };
        std::string cmd = "zenity --info --title=\"" + sh(title) + "\" --text=\"" + sh(message) + "\"";
        runDialog(cmd);
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
        result = runDialog("osascript -e 'try' -e 'POSIX path of (choose file of type {\"wav\"} with prompt \"Select Baseband WAV\")' -e 'on error' -e 'return \"\"' -e 'end try'");
    #elif __linux__
        result = runDialog("zenity --file-selection --file-filter='*.wav' --title='Select Baseband WAV'");
    #endif

    // Trim trailing newline characters
    if (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }
    return result;
}

// Opens a native file picker for TLE element sets (.tle / .txt, any file allowed)
inline std::string openTleFileDialog() {
    std::string result = "";
    #ifdef _WIN32
        char filename[MAX_PATH] = {0};
        OPENFILENAMEA ofn;
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.lpstrFile = filename;
        ofn.nMaxFile = sizeof(filename);
        ofn.lpstrFilter = "TLE files (*.tle;*.txt)\0*.tle;*.txt\0All Files\0*.*\0";
        ofn.nFilterIndex = 1;
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameA(&ofn)) result = filename;
    #else
        #ifdef __APPLE__
        const char* cmd = "osascript -e 'try' -e 'POSIX path of (choose file with prompt \"Select a TLE file\")' -e 'on error' -e 'return \"\"' -e 'end try'";
        #else
        const char* cmd = "zenity --file-selection --title='Select a TLE file' 2>/dev/null";
        #endif
        result = runDialog(cmd);
    #endif
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) result.pop_back();
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
        result = runDialog("osascript -e 'try' -e 'POSIX path of (choose folder with prompt \"" + title + "\")' -e 'on error' -e 'return \"\"' -e 'end try'");
    #elif __linux__
        result = runDialog("zenity --file-selection --directory --title='" + title + "'");
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
        result = runDialog("osascript -e 'try' -e 'POSIX path of (choose file name with prompt \"Save picture\" default name \"" + fileName +
                           "\" default location (POSIX file \"" + folder + "\"))' -e 'on error' -e 'return \"\"' -e 'end try'");
    #elif __linux__
        result = runDialog("zenity --file-selection --save --confirm-overwrite --title='Save picture' --filename='" + folder + "/" + fileName + "'");
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
        result = runDialog("osascript -e 'try' -e 'POSIX path of (choose file name with prompt \"" + title + "\" default name \"" + fileName +
                           "\" default location (POSIX file \"" + folder + "\"))' -e 'on error' -e 'return \"\"' -e 'end try'");
    #elif __linux__
        result = runDialog("zenity --file-selection --save --confirm-overwrite --title='" + title + "' --filename='" + folder + "/" + fileName + "'");
    #endif
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) result.pop_back();
    return result;
}

// Opens a native file picker for a JSON file (bookmark banks). Empty when cancelled.
inline std::string openJsonFileDialog(const std::string& title = "Import bookmarks") {
    std::string result = "";
    #ifdef _WIN32
        char filename[MAX_PATH] = {0};
        OPENFILENAMEA ofn;
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.lpstrFile = filename;
        ofn.nMaxFile = sizeof(filename);
        ofn.lpstrFilter = "JSON file (*.json)\0*.json\0All Files\0*.*\0";
        ofn.nFilterIndex = 1;
        ofn.lpstrTitle = title.c_str();
        ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameA(&ofn)) result = filename;
    #elif __APPLE__
        result = runDialog("osascript -e 'try' -e 'POSIX path of (choose file with prompt \"" + title + "\")' -e 'on error' -e 'return \"\"' -e 'end try'");
    #elif __linux__
        result = runDialog("zenity --file-selection --file-filter='*.json' --title='" + title + "' 2>/dev/null");
    #endif
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) result.pop_back();
    return result;
}

// Opens a native "Save as" dialog for a JSON file; the .json extension is added when missing
inline std::string saveJsonFileDialog(const std::string& folder, const std::string& fileName,
                                      const std::string& title = "Export bookmarks") {
    std::string result = "";
    #ifdef _WIN32
        char filename[MAX_PATH] = {0};
        fileName.copy(filename, MAX_PATH - 1);
        std::string initialDir = folder;
        for (auto& c : initialDir) if (c == '/') c = '\\';
        OPENFILENAMEA ofn;
        ZeroMemory(&ofn, sizeof(ofn));
        ofn.lStructSize = sizeof(ofn);
        ofn.lpstrFile = filename;
        ofn.nMaxFile = sizeof(filename);
        ofn.lpstrFilter = "JSON file (*.json)\0*.json\0All Files\0*.*\0";
        ofn.nFilterIndex = 1;
        ofn.lpstrInitialDir = initialDir.c_str();
        ofn.lpstrDefExt = "json";
        ofn.lpstrTitle = title.c_str();
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetSaveFileNameA(&ofn)) result = filename;
    #elif __APPLE__
        result = runDialog("osascript -e 'try' -e 'POSIX path of (choose file name with prompt \"" + title + "\" default name \"" + fileName +
                           "\" default location (POSIX file \"" + folder + "\"))' -e 'on error' -e 'return \"\"' -e 'end try'");
    #elif __linux__
        result = runDialog("zenity --file-selection --save --confirm-overwrite --title='" + title + "' --filename='" + folder + "/" + fileName + "' 2>/dev/null");
    #endif
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) result.pop_back();
    if (!result.empty()) {
        std::string lower = result;
        for (auto& c : lower) c = (char)std::tolower((unsigned char)c);
        if (lower.size() < 5 || lower.compare(lower.size() - 5, 5, ".json") != 0) result += ".json";
    }
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
