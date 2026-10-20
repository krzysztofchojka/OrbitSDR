#pragma once
#include <string>
#include <fstream>
#include <cmath>
#include <algorithm>
#include <vector>
#include <cstdlib>

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#elif defined(__APPLE__)
    #include <mach-o/dyld.h>
    #include <limits.h>
#else
    #include <unistd.h>
    #include <limits.h>
#endif

// Returns the directory that contains the running executable (no trailing slash).
// On macOS this is OrbitSDR.app/Contents/MacOS, on Windows/Linux the folder of the binary.
inline std::string getExecutableDir() {
    std::string full;
#ifdef _WIN32
    char buf[MAX_PATH] = {0};
    DWORD n = GetModuleFileNameA(NULL, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) full = std::string(buf, n);
#elif defined(__APPLE__)
    char buf[PATH_MAX] = {0};
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        char resolved[PATH_MAX] = {0};
        if (realpath(buf, resolved)) full = resolved;
        else full = buf;
    }
#else
    char buf[PATH_MAX] = {0};
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) full = std::string(buf, (size_t)n);
#endif
    if (full.empty()) return ".";
    size_t pos = full.find_last_of("/\\");
    if (pos == std::string::npos) return ".";
    return full.substr(0, pos);
}

inline bool fileExistsReadable(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return f.good();
}

// Looks for a resource file (bandplan.json, icon.png...) in all the places where it can live:
//  - next to the executable                       (Windows / Linux / dev builds)
//  - ../Resources relative to the executable      (macOS .app bundle)
//  - current working directory                    (running from the repo folder)
// Returns an empty string when nothing was found.
inline std::string findResourceFile(const std::string& name) {
    std::string exeDir = getExecutableDir();
    std::vector<std::string> candidates = {
        exeDir + "/" + name,
        exeDir + "/../Resources/" + name,
        "./" + name,
        "../Resources/" + name
    };
    for (const auto& c : candidates) {
        if (fileExistsReadable(c)) return c;
    }
    return "";
}

#ifdef _WIN32
// Detects the Windows display scaling (100% / 125% / 150% ...) for the window.
// Returns a value rounded to 0.25 and clamped to 1.0 - 3.0.
inline float detectWindowsUiScale(void* hwndPtr) {
    HWND hwnd = (HWND)hwndPtr;
    UINT dpi = 96;

    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (user32 && hwnd) {
        typedef UINT (WINAPI *GetDpiForWindowProc)(HWND);
        void* fnPtr = (void*)GetProcAddress(user32, "GetDpiForWindow");
        if (fnPtr) {
            GetDpiForWindowProc fn = (GetDpiForWindowProc)fnPtr;
            UINT d = fn(hwnd);
            if (d > 0) dpi = d;
        }
    }

    float scale = (float)dpi / 96.0f;
    scale = std::round(scale * 4.0f) / 4.0f;
    return std::clamp(scale, 1.0f, 3.0f);
}
#endif
