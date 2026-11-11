#pragma once

// =====================================================================
//  Runtime loading of the SDRplay API library.
//
//  OrbitSDR doesn't link against the SDRplay API. The library is loaded
//  when an SDRplay device is first used:
//    1. the API installed on the computer (it always matches the
//       installed SDRplay API service),
//    2. if that is missing, can't be loaded or can't open the API, the
//       copy bundled with OrbitSDR (API 3.15).
//  Without any SDRplay API the program still starts normally; only the
//  SDRplay source reports that no device is available.
// =====================================================================

#ifdef ENABLE_SDRPLAY

#include <string>
#include <vector>
#include <iostream>
#include <sdrplay_api.h>
#include "PlatformUtils.h"

#ifdef _WIN32
    #include <windows.h>
#else
    #include <dlfcn.h>
#endif

class SdrplayLib {
public:
    // Function table of the library in use (valid after open() succeeded)
    sdrplay_api_Close_t Close = nullptr;
    sdrplay_api_ApiVersion_t ApiVersion = nullptr;
    sdrplay_api_LockDeviceApi_t LockDeviceApi = nullptr;
    sdrplay_api_UnlockDeviceApi_t UnlockDeviceApi = nullptr;
    sdrplay_api_GetDevices_t GetDevices = nullptr;
    sdrplay_api_SelectDevice_t SelectDevice = nullptr;
    sdrplay_api_ReleaseDevice_t ReleaseDevice = nullptr;
    sdrplay_api_GetDeviceParams_t GetDeviceParams = nullptr;
    sdrplay_api_Init_t Init = nullptr;
    sdrplay_api_Uninit_t Uninit = nullptr;
    sdrplay_api_Update_t Update = nullptr;

    static SdrplayLib& get() { static SdrplayLib lib; return lib; }

    // Opens the API (sdrplay_api_Open), trying the installed library first and the
    // bundled one second. Returns false when no library could open the API.
    bool open() {
        for (Candidate& c : candidates()) {
            if (!load(c)) continue;
            if (c.fn.Open() != sdrplay_api_Success) {
                std::cerr << "[SDRplay] " << c.path << " could not open the API" << std::endl;
                continue;
            }
            use(c);
            return true;
        }
        return false;
    }

    // Path of the library in use (empty if none)
    const std::string& libraryPath() const { return activePath; }

private:
    struct Functions {
        sdrplay_api_Open_t Open = nullptr;
        sdrplay_api_Close_t Close = nullptr;
        sdrplay_api_ApiVersion_t ApiVersion = nullptr;
        sdrplay_api_LockDeviceApi_t LockDeviceApi = nullptr;
        sdrplay_api_UnlockDeviceApi_t UnlockDeviceApi = nullptr;
        sdrplay_api_GetDevices_t GetDevices = nullptr;
        sdrplay_api_SelectDevice_t SelectDevice = nullptr;
        sdrplay_api_ReleaseDevice_t ReleaseDevice = nullptr;
        sdrplay_api_GetDeviceParams_t GetDeviceParams = nullptr;
        sdrplay_api_Init_t Init = nullptr;
        sdrplay_api_Uninit_t Uninit = nullptr;
        sdrplay_api_Update_t Update = nullptr;
    };
    struct Candidate {
        std::string path;
        bool tried = false;     // load attempted
        bool ok = false;        // loaded with all functions
        void* handle = nullptr;
        Functions fn;
    };

    std::vector<Candidate> list;
    bool listBuilt = false;
    std::string activePath;

    std::vector<Candidate>& candidates() {
        if (!listBuilt) {
            listBuilt = true;
            auto add = [&](const std::string& path) { Candidate c; c.path = path; list.push_back(c); };
            for (const std::string& p : installedPaths()) add(p);
            for (const std::string& p : bundledPaths()) add(p);
        }
        return list;
    }

    static std::vector<std::string> installedPaths() {
        std::vector<std::string> p;
#ifdef _WIN32
        // The SDRplay installer records its folder in the registry
        HKEY key;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\SDRplay\\Service\\API", 0, KEY_READ | KEY_WOW64_64KEY, &key) == ERROR_SUCCESS) {
            char dir[MAX_PATH] = {0};
            DWORD size = sizeof(dir), type = 0;
            if (RegQueryValueExA(key, "Install_Dir", NULL, &type, (LPBYTE)dir, &size) == ERROR_SUCCESS && type == REG_SZ && dir[0])
                p.push_back(std::string(dir) + "\\x64\\sdrplay_api.dll");
            RegCloseKey(key);
        }
        p.push_back("C:\\Program Files\\SDRplay\\API\\x64\\sdrplay_api.dll");
#elif defined(__APPLE__)
        p = { "/usr/local/lib/libsdrplay_api.so.3", "/usr/local/lib/libsdrplay_api.dylib",
              "/opt/homebrew/lib/libsdrplay_api.so.3" };
#else
        p = { "/usr/local/lib/libsdrplay_api.so.3", "/usr/local/lib/libsdrplay_api.so",
              "/usr/lib/libsdrplay_api.so.3", "/usr/lib64/libsdrplay_api.so.3",
              "/usr/lib/x86_64-linux-gnu/libsdrplay_api.so.3", "/usr/lib/aarch64-linux-gnu/libsdrplay_api.so.3" };
#endif
        return p;
    }

    static std::vector<std::string> bundledPaths() {
        const std::string exe = getExecutableDir();
#ifdef _WIN32
        return { exe + "\\sdrplay_api.dll" };
#elif defined(__APPLE__)
        return { exe + "/../Frameworks/libsdrplay_api.so.3", exe + "/libsdrplay_api.so.3" };
#else
        return { exe + "/libs/libsdrplay_api.so.3", exe + "/libsdrplay_api.so.3" };
#endif
    }

    static void* symbol(void* handle, const char* name) {
#ifdef _WIN32
        return (void*)GetProcAddress((HMODULE)handle, name);
#else
        return dlsym(handle, name);
#endif
    }

    // Loads the library once and looks up all functions
    static bool load(Candidate& c) {
        if (c.tried) return c.ok;
        c.tried = true;
        if (!fileExistsReadable(c.path)) return false;
#ifdef _WIN32
        // Altered search path: the library's own folder is searched for its dependencies
        c.handle = (void*)LoadLibraryExA(c.path.c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
#else
        c.handle = dlopen(c.path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
        if (!c.handle) {
            std::cerr << "[SDRplay] could not load " << c.path << std::endl;
            return false;
        }
        Functions& f = c.fn;
        bool all = true;
        auto bind = [&](auto& ptr, const char* name) {
            ptr = reinterpret_cast<std::remove_reference_t<decltype(ptr)>>(symbol(c.handle, name));
            if (!ptr) all = false;
        };
        bind(f.Open, "sdrplay_api_Open");
        bind(f.Close, "sdrplay_api_Close");
        bind(f.ApiVersion, "sdrplay_api_ApiVersion");
        bind(f.LockDeviceApi, "sdrplay_api_LockDeviceApi");
        bind(f.UnlockDeviceApi, "sdrplay_api_UnlockDeviceApi");
        bind(f.GetDevices, "sdrplay_api_GetDevices");
        bind(f.SelectDevice, "sdrplay_api_SelectDevice");
        bind(f.ReleaseDevice, "sdrplay_api_ReleaseDevice");
        bind(f.GetDeviceParams, "sdrplay_api_GetDeviceParams");
        bind(f.Init, "sdrplay_api_Init");
        bind(f.Uninit, "sdrplay_api_Uninit");
        bind(f.Update, "sdrplay_api_Update");
        if (!all) {
            std::cerr << "[SDRplay] " << c.path << " is missing API functions" << std::endl;
            return false;   // the handle stays loaded; harmless and avoids unloading issues
        }
        c.ok = true;
        return true;
    }

    void use(const Candidate& c) {
        const Functions& f = c.fn;
        Close = f.Close; ApiVersion = f.ApiVersion;
        LockDeviceApi = f.LockDeviceApi; UnlockDeviceApi = f.UnlockDeviceApi;
        GetDevices = f.GetDevices; SelectDevice = f.SelectDevice; ReleaseDevice = f.ReleaseDevice;
        GetDeviceParams = f.GetDeviceParams; Init = f.Init; Uninit = f.Uninit; Update = f.Update;
        if (activePath != c.path) std::cerr << "[SDRplay] using " << c.path << std::endl;
        activePath = c.path;
    }
};

// The rest of the code calls the API functions by their usual names; they go to the
// library that is in use. sdrplay_api_Open is replaced by SdrplayLib::get().open().
#define sdrplay_api_Close           SdrplayLib::get().Close
#define sdrplay_api_ApiVersion      SdrplayLib::get().ApiVersion
#define sdrplay_api_LockDeviceApi   SdrplayLib::get().LockDeviceApi
#define sdrplay_api_UnlockDeviceApi SdrplayLib::get().UnlockDeviceApi
#define sdrplay_api_GetDevices      SdrplayLib::get().GetDevices
#define sdrplay_api_SelectDevice    SdrplayLib::get().SelectDevice
#define sdrplay_api_ReleaseDevice   SdrplayLib::get().ReleaseDevice
#define sdrplay_api_GetDeviceParams SdrplayLib::get().GetDeviceParams
#define sdrplay_api_Init            SdrplayLib::get().Init
#define sdrplay_api_Uninit          SdrplayLib::get().Uninit
#define sdrplay_api_Update          SdrplayLib::get().Update

#endif // ENABLE_SDRPLAY
