#pragma once

// =====================================================================
//  macOS: window tiling (dragging a window to the top edge of the screen,
//  or the Window > Fill / tile commands) resizes the NSWindow without
//  SFML noticing: getSize() keeps the old size and the picture stays in
//  the lower left corner with a black band above it. The green button
//  works because it goes through the normal resize path.
//
//  syncMacWindowSize() compares the real content size of the NSWindow
//  with SFML's idea of it once per frame and, when they differ, tells
//  SFML the new size (it then resizes its view and GL surface and sends
//  a Resized event). Elsewhere it does nothing.
//
//  Plain C++ through the Objective-C runtime (no .mm file needed);
//  link with -framework AppKit.
// =====================================================================

#include <SFML/Window.hpp>

#ifdef __APPLE__
#include <objc/message.h>
#include <objc/runtime.h>
#include <CoreGraphics/CGGeometry.h>
#include <cstdlib>
#include <cmath>

inline void syncMacWindowSize(sf::WindowBase& w) {
    id nsWindow = (id)w.getNativeHandle();
    if (!nsWindow) return;
    // only NSWindow handles (SFML may also hand out an NSView for embedded windows)
    if (!((BOOL(*)(id, SEL, Class))objc_msgSend)(nsWindow, sel_registerName("isKindOfClass:"), objc_getClass("NSWindow"))) return;
    // not while minimised / in the middle of a live resize (SFML handles those)
    if (((BOOL(*)(id, SEL))objc_msgSend)(nsWindow, sel_registerName("isMiniaturized"))) return;
    id view = ((id(*)(id, SEL))objc_msgSend)(nsWindow, sel_registerName("contentView"));
    if (!view) return;
    if (((BOOL(*)(id, SEL))objc_msgSend)(view, sel_registerName("inLiveResize"))) return;
#if defined(__x86_64__)
    CGRect fr = ((CGRect(*)(id, SEL))objc_msgSend_stret)(view, sel_registerName("frame"));
#else
    CGRect fr = ((CGRect(*)(id, SEL))objc_msgSend)(view, sel_registerName("frame"));
#endif
    const double scale = ((double(*)(id, SEL))objc_msgSend)(nsWindow, sel_registerName("backingScaleFactor"));
    if (fr.size.width < 1 || fr.size.height < 1) return;

    // SFML reports points or pixels depending on NSHighResolutionCapable: learn which from
    // the first frame (sizes agree then), keep using it
    static int mode = 0;   // 0 unknown, 1 points, 2 pixels
    const sf::Vector2u cur = w.getSize();
    if (mode == 0) {
        const double rp = cur.x / fr.size.width;
        mode = std::fabs(rp - scale) < 0.05 && scale > 1.01 ? 2 : 1;
    }
    const double k = mode == 2 ? scale : 1.0;
    const sf::Vector2u want{(unsigned)std::lround(fr.size.width * k), (unsigned)std::lround(fr.size.height * k)};
    if (std::abs((int)want.x - (int)cur.x) > 2 || std::abs((int)want.y - (int)cur.y) > 2) w.setSize(want);
}

// Auto UI scale on macOS: with NSHighResolutionCapable SFML's window is in real pixels
// (2x on Retina), so the UI is drawn at the backing scale factor to keep its size, and the
// text is rasterised at full resolution (sharp). Without HiDPI (window in points) it is 1.
inline float detectMacUiScale(sf::WindowBase& w) {
    id nsWindow = (id)w.getNativeHandle();
    if (!nsWindow) return 1.0f;
    if (!((BOOL(*)(id, SEL, Class))objc_msgSend)(nsWindow, sel_registerName("isKindOfClass:"), objc_getClass("NSWindow"))) return 1.0f;
    id view = ((id(*)(id, SEL))objc_msgSend)(nsWindow, sel_registerName("contentView"));
    if (!view) return 1.0f;
#if defined(__x86_64__)
    CGRect fr = ((CGRect(*)(id, SEL))objc_msgSend_stret)(view, sel_registerName("frame"));
#else
    CGRect fr = ((CGRect(*)(id, SEL))objc_msgSend)(view, sel_registerName("frame"));
#endif
    if (fr.size.width < 1) return 1.0f;
    const double r = w.getSize().x / fr.size.width;
    return r > 1.2 ? (float)std::round(r * 4.0) / 4.0f : 1.0f;
}
#else
inline void syncMacWindowSize(sf::WindowBase&) {}
inline float detectMacUiScale(sf::WindowBase&) { return 1.0f; }
#endif
