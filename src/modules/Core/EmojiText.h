#pragma once

// =====================================================================
//  Text that may hold emoji (Meshtastic short names are often one).
//
//  The UI font (Arial, Helvetica, DejaVu ...) has no emoji, so each code
//  point it lacks is drawn with a system emoji font instead, when there is
//  one that FreeType can draw as an outline: Segoe UI Emoji on Windows
//  (its monochrome glyphs), Noto Emoji / Symbola on Linux, Apple Symbols
//  on macOS. Without one the main font's box is drawn, as before.
//  Variation selectors and joiners are left out (a ZWJ sequence shows its
//  parts side by side).
//
//  badge(): the round "avatar" of a node, as the Meshtastic apps show it:
//  its short name on a colour taken from the node number.
// =====================================================================

#include <SFML/Graphics.hpp>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <cstdint>

namespace emoji {

inline const sf::Font* fallbackFont() {
    static sf::Font f;
    static int state = 0;   // 0 not tried, 1 loaded, 2 none
    if (state == 0) {
        state = 2;
        for (const char* p : {"C:/Windows/Fonts/seguiemj.ttf", "C:/Windows/Fonts/seguisym.ttf",
                              "/usr/share/fonts/truetype/noto/NotoEmoji-Regular.ttf", "/usr/share/fonts/noto/NotoEmoji-Regular.ttf",
                              "/usr/share/fonts/google-noto-emoji/NotoEmoji-Regular.ttf", "/usr/share/fonts/truetype/ancient-scripts/Symbola_hint.ttf",
                              "/usr/share/fonts/TTF/Symbola.ttf", "/System/Library/Fonts/Apple Symbols.ttf"}) {
            if (f.openFromFile(p)) { state = 1; break; }
        }
    }
    return state == 1 ? &f : nullptr;
}

inline bool invisible(char32_t c) { return c == 0x200D || (c >= 0xFE00 && c <= 0xFE0F); }

// runs of text that use one font
struct Run { const sf::Font* font; sf::String s; };
inline std::vector<Run> split(const sf::Font& main, const std::string& utf8) {
    const sf::String all = sf::String::fromUtf8(utf8.begin(), utf8.end());
    const sf::Font* fb = fallbackFont();
    std::vector<Run> runs;
    for (char32_t c : all) {
        if (invisible(c)) continue;
        const sf::Font* f = &main;
        if (c > 0x7F && fb && !main.hasGlyph(c) && fb->hasGlyph(c)) f = fb;
        if (runs.empty() || runs.back().font != f) runs.push_back({f, sf::String()});
        runs.back().s += c;
    }
    return runs;
}

// Width of the text (logical pixels) at `size`
inline float width(const sf::Font& main, const std::string& utf8, unsigned size) {
    float w = 0.0f;
    for (const Run& r : split(main, utf8)) {
        sf::Text t(*r.font, r.s, size * 2);
        w += t.getLocalBounds().position.x + t.getLocalBounds().size.x;
    }
    return w * 0.5f;
}

// Draws the text with its top left at (x, y), like the panels' sf::Text drawn at size x 2 and
// scaled by a half. Returns the width.
inline float draw(sf::RenderTarget& rt, const sf::Font& main, const std::string& utf8, float x, float y, unsigned size, sf::Color col) {
    float px = x;
    for (const Run& r : split(main, utf8)) {
        sf::Text t(*r.font, r.s, size * 2);
        t.setScale({0.5f, 0.5f});
        t.setFillColor(col);
        // emoji fonts sit lower / higher than the UI font: line their middles up
        float dy = 0.0f;
        if (r.font != &main) {
            const float ascM = (float)size * 0.72f;
            const sf::FloatRect b = t.getLocalBounds();
            dy = ascM * 0.5f - (b.position.y + b.size.y * 0.5f) * 0.5f + (float)size * 0.08f;
        }
        t.setPosition({std::round(px), std::round(y + dy)});
        rt.draw(t);
        px += (t.getLocalBounds().position.x + t.getLocalBounds().size.x) * 0.5f;
    }
    return px - x;
}

// The node's colour: the low 24 bits of its number as RGB (as the Meshtastic apps do)
inline sf::Color nodeColor(uint32_t num) { return sf::Color((num >> 16) & 0xFF, (num >> 8) & 0xFF, num & 0xFF); }
inline sf::Color textOn(sf::Color bg) {
    return 0.299 * bg.r + 0.587 * bg.g + 0.114 * bg.b > 150 ? sf::Color(15, 15, 20) : sf::Color::White;
}

// A rounded badge with the short name, centred on c, h high. Returns its size.
inline sf::Vector2f badge(sf::RenderTarget& rt, const sf::Font& main, const std::string& name, sf::Vector2f c, float h,
                          sf::Color fill, bool highlight = false) {
    const unsigned ts = (unsigned)std::max(7.0f, h * 0.55f);
    const float tw = width(main, name, ts);
    const float w = std::max(h, tw + h * 0.55f);
    const float r = h * 0.5f;
    const sf::Color outline = highlight ? sf::Color::White : sf::Color(0, 0, 0, 200);
    // a pill: two half circles and a rectangle, drawn as one convex shape
    sf::ConvexShape pill;
    const int seg = 10;
    pill.setPointCount(2 * (seg + 1));
    size_t k = 0;
    for (int i = 0; i <= seg; i++) {   // right end, top to bottom
        const double a = -M_PI / 2 + M_PI * i / seg;
        pill.setPoint(k++, {c.x + (w / 2 - r) + (float)std::cos(a) * r, c.y + (float)std::sin(a) * r});
    }
    for (int i = 0; i <= seg; i++) {   // left end, bottom to top
        const double a = M_PI / 2 + M_PI * i / seg;
        pill.setPoint(k++, {c.x - (w / 2 - r) + (float)std::cos(a) * r, c.y + (float)std::sin(a) * r});
    }
    pill.setFillColor(fill);
    pill.setOutlineColor(outline);
    pill.setOutlineThickness(highlight ? 2.0f : 1.0f);
    rt.draw(pill);
    draw(rt, main, name, c.x - tw / 2, c.y - ts * 0.62f, ts, textOn(fill));
    return {w, h};
}

}  // namespace emoji
