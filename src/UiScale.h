#pragma once

#include <SFML/Graphics.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <dlfcn.h>
#endif

#include "MacWindowFix.h"

// =====================================================================
//  The system's UI scale for the window ("Auto" in Display > UI Scale).
//
//  The whole UI is drawn in logical units and the view scaled by this, so
//  on a high-DPI screen it keeps a readable size. Sources, first that
//  answers wins:
//
//   all     ORBITSDR_UI_SCALE=1.5 (environment) - explicit override
//   macOS   the window's backing scale factor (Retina 2x)
//   Windows the window's DPI (GetDpiForWindow; older systems: the monitor's
//           or the screen's DPI). When Windows says 100 % although the panel
//           is dense (small laptops / mini PCs whose EDID made Windows pick
//           100 %), the panel's physical pixel density decides instead.
//   Linux   GDK_SCALE (x GDK_DPI_SCALE); Xft.dpi of the X resources (GNOME,
//           KDE, XWayland with native scaling set it to 96 x scale); GNOME's
//           scaling-factor. Nothing set: 1 (an XWayland window that the
//           compositor scales itself must not be scaled twice).
//
//  Result rounded to 0.25, clamped to 0.75 .. 3.
// =====================================================================

namespace uiscale {

struct Detected {
    float scale = 1.0f;
    std::string source;   // for the log / the UI ("Windows DPI 144", "Xft.dpi 192" ...)
};

inline float roundScale(float s) { return std::clamp(std::round(s * 4.0f) / 4.0f, 0.75f, 3.0f); }

// A comfortable size from the panel's pixel density: ~110 px per inch reads like 100 %.
// Only densities that clearly need more than 100 % count; implausible EDID data is ignored.
inline float scaleFromDensity(double px, double py, double mmx, double mmy) {
    if (px < 640 || py < 400 || mmx < 60 || mmy < 40 || mmx > 1600 || mmy > 1000) return 0.0f;
    const double diagPx = std::hypot(px, py), diagIn = std::hypot(mmx, mmy) / 25.4;
    const double ppi = diagPx / diagIn;
    if (ppi < 70.0 || ppi > 450.0) return 0.0f;
    const float s = roundScale((float)(ppi / 110.0));
    return s >= 1.25f ? s : 0.0f;
}

#if defined(_WIN32)
inline Detected detectWindows(HWND hwnd) {
    Detected d;
    UINT dpi = 0;
    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (user32 && hwnd) {
        typedef UINT(WINAPI * GetDpiForWindowFn)(HWND);
        if (auto fn = (GetDpiForWindowFn)(void*)GetProcAddress(user32, "GetDpiForWindow")) dpi = fn(hwnd);
    }
    HMONITOR mon = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) : nullptr;
    if (!dpi && mon) {   // Windows 8.1 / early 10
        if (HMODULE shcore = LoadLibraryA("shcore.dll")) {
            typedef HRESULT(WINAPI * GetDpiForMonitorFn)(HMONITOR, int, UINT*, UINT*);
            if (auto fn = (GetDpiForMonitorFn)(void*)GetProcAddress(shcore, "GetDpiForMonitor")) {
                UINT dx = 0, dy = 0;
                if (fn(mon, 0 /*MDT_EFFECTIVE_DPI*/, &dx, &dy) == S_OK) dpi = dx;
            }
        }
    }
    if (!dpi) {
        HDC dc = GetDC(nullptr);
        if (dc) { dpi = (UINT)GetDeviceCaps(dc, LOGPIXELSX); ReleaseDC(nullptr, dc); }
    }
    if (!dpi) dpi = 96;
    d.scale = roundScale(dpi / 96.0f);
    d.source = "Windows DPI " + std::to_string(dpi);

    // Windows at 100 % on a dense panel: go by the panel's physical size
    if (dpi <= 96 && mon) {
        MONITORINFOEXA mi;
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoA(mon, &mi)) {
            HDC mdc = CreateDCA("DISPLAY", mi.szDevice, nullptr, nullptr);
            if (mdc) {
                const double px = GetDeviceCaps(mdc, HORZRES), py = GetDeviceCaps(mdc, VERTRES);
                const double mmx = GetDeviceCaps(mdc, HORZSIZE), mmy = GetDeviceCaps(mdc, VERTSIZE);
                DeleteDC(mdc);
                const float s = scaleFromDensity(px, py, mmx, mmy);
                if (s > d.scale) {
                    d.scale = s;
                    char b[96];
                    std::snprintf(b, sizeof(b), "panel %.0fx%.0f px, %.0fx%.0f mm (Windows at 100 %%)", px, py, mmx, mmy);
                    d.source = b;
                }
            }
        }
    }
    return d;
}
#endif

#if defined(__linux__)
// Xft.dpi from the X server's resource database (RESOURCE_MANAGER), read through a
// libX11 loaded at run time (no link dependency). 0 when not set / no X.
inline float linuxXftDpi() {
    void* h = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
    if (!h) return 0.0f;
    typedef void* (*OpenFn)(const char*);
    typedef char* (*ResFn)(void*);
    typedef int (*CloseFn)(void*);
    auto openD = (OpenFn)dlsym(h, "XOpenDisplay");
    auto res = (ResFn)dlsym(h, "XResourceManagerString");
    auto closeD = (CloseFn)dlsym(h, "XCloseDisplay");
    float dpi = 0.0f;
    if (openD && res && closeD) {
        if (void* dpy = openD(nullptr)) {   // a fresh connection: the current resources
            if (const char* s = res(dpy)) {
                const char* p = std::strstr(s, "Xft.dpi:");
                if (p) dpi = (float)std::atof(p + 8);
            }
            closeD(dpy);
        }
    }
    dlclose(h);
    return dpi;
}

inline int linuxGnomeScalingFactor() {
    FILE* f = popen("gsettings get org.gnome.desktop.interface scaling-factor 2>/dev/null", "r");
    if (!f) return 0;
    char b[64] = {0};
    const size_t n = fread(b, 1, sizeof(b) - 1, f);
    pclose(f);
    b[n] = 0;
    // exactly "uint32 N" (anything else: no GNOME, an error message ...)
    unsigned v = 0;
    char rest = 0;
    if (std::sscanf(b, "uint32 %u%c", &v, &rest) >= 1 && v <= 8) return (int)v;
    return 0;
}

inline Detected detectLinux() {
    Detected d;
    if (const char* g = std::getenv("GDK_SCALE")) {
        const float s = (float)std::atof(g);
        if (s > 0.0f) {
            float k = 1.0f;
            if (const char* ds = std::getenv("GDK_DPI_SCALE")) { const float v = (float)std::atof(ds); if (v > 0.0f) k = v; }
            d.scale = roundScale(s * k);
            d.source = "GDK_SCALE";
            return d;
        }
    }
    const float xft = linuxXftDpi();
    if (xft >= 48.0f && std::fabs(xft - 96.0f) > 1.0f) {
        d.scale = roundScale(xft / 96.0f);
        d.source = "Xft.dpi " + std::to_string((int)xft);
        return d;
    }
    const int g = linuxGnomeScalingFactor();
    if (g > 1) {
        d.scale = roundScale((float)g);
        d.source = "GNOME scaling-factor " + std::to_string(g);
        return d;
    }
    d.source = xft > 0.0f ? "Xft.dpi 96" : "no system scale";
    return d;
}
#endif

inline Detected detect(sf::RenderWindow& window) {
    if (const char* e = std::getenv("ORBITSDR_UI_SCALE")) {
        const float s = (float)std::atof(e);
        if (s > 0.0f) return {roundScale(s), "ORBITSDR_UI_SCALE"};
    }
#if defined(_WIN32)
    return detectWindows((HWND)window.getNativeHandle());
#elif defined(__APPLE__)
    return {detectMacUiScale(window), "macOS backing scale"};
#elif defined(__linux__)
    (void)window;
    return detectLinux();
#else
    (void)window;
    return {};
#endif
}

} // namespace uiscale
