#pragma once

#include <SFML/Graphics.hpp>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>
#include "../../UI.h"

// =====================================================================
//  Modal window with the details of one decoded message (ISM, ACARS...).
//
//  A list of "label: value" rows plus an optional long text that is
//  word-wrapped. Close with the button, Escape or a click outside;
//  "Copy" puts everything on the clipboard. The mouse wheel scrolls.
//  While it is open the module gets every event first (see
//  SDRModule::modalOpen), so nothing behind it reacts.
// =====================================================================

class DetailsPopup {
    struct Row { std::string label, value; };
    const sf::Font* font = nullptr;
    bool open = false;
    std::string title, longText;
    std::vector<Row> rows;
    std::shared_ptr<SdrButton> btnClose, btnCopy;
    sf::FloatRect rect;
    float scroll = 0.0f, contentH = 0.0f, viewH = 0.0f;

    sf::Text text(const std::string& s, unsigned size, sf::Color c) const {
        sf::Text t(*font, sf::String::fromUtf8(s.begin(), s.end()), size);
        t.setScale({0.5f, 0.5f});
        t.setFillColor(c);
        return t;
    }

    // Splits 's' into lines of at most 'maxChars' characters (at spaces when possible),
    // keeping the line breaks of the original text
    static std::vector<std::string> wrap(const std::string& s, size_t maxChars) {
        std::vector<std::string> out;
        size_t start = 0;
        while (start <= s.size()) {
            size_t nl = s.find('\n', start);
            std::string line = s.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            while (line.size() > maxChars) {
                size_t cut = line.rfind(' ', maxChars);
                if (cut == std::string::npos || cut < maxChars / 2) cut = maxChars;
                out.push_back(line.substr(0, cut));
                line = line.substr(cut == maxChars ? cut : cut + 1);
            }
            out.push_back(line);
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
        return out;
    }

public:
    void init(const sf::Font& f) {
        font = &f;
        btnClose = std::make_shared<SdrButton>(80.0f, 28.0f, "Close", f);
        btnCopy = std::make_shared<SdrButton>(80.0f, 28.0f, "Copy", f);
        btnClose->onClick = [this]() { open = false; };
        btnCopy->onClick = [this]() {
            std::string all = title + "\n";
            for (const Row& r : rows) all += r.label + ": " + r.value + "\n";
            if (!longText.empty()) all += "\n" + longText + "\n";
            sf::Clipboard::setString(sf::String::fromUtf8(all.begin(), all.end()));
        };
    }

    void show(const std::string& t, const std::vector<std::pair<std::string, std::string>>& fields,
              const std::string& body = "") {
        title = t;
        rows.clear();
        for (const auto& f : fields) rows.push_back({f.first, f.second});
        longText = body;
        scroll = 0.0f;
        open = true;
    }

    bool isOpen() const { return open; }
    void close() { open = false; }

    // Returns true while the popup is open (every event is consumed)
    bool handleEvent(const sf::Event& ev, const sf::RenderWindow& win) {
        if (!open) return false;
        if (btnClose->handleEvent(ev, win)) return true;
        if (btnCopy->handleEvent(ev, win)) return true;
        sf::Vector2f m = win.mapPixelToCoords(sf::Mouse::getPosition(win));
        if (const auto* k = ev.getIf<sf::Event::KeyPressed>()) {
            if (k->code == sf::Keyboard::Key::Escape) open = false;
        }
        if (const auto* sc = ev.getIf<sf::Event::MouseWheelScrolled>()) {
            const float maxScroll = std::max(0.0f, contentH - viewH);
            scroll = std::clamp(scroll - sc->delta * 45.0f, 0.0f, maxScroll);
        }
        if (ev.getIf<sf::Event::MouseButtonPressed>() && !rect.contains(m)) open = false;
        return true;
    }

    void draw(sf::RenderWindow& win) {
        if (!open || !font) return;
        const sf::Vector2f vs = win.getView().getSize();
        const float w = std::min(720.0f, vs.x - 40.0f);
        const float h = std::min(520.0f, vs.y - 80.0f);
        const float x = std::round((vs.x - w) / 2.0f), y = std::round((vs.y - h) / 2.0f);
        rect = sf::FloatRect({x, y}, {w, h});

        sf::RectangleShape dim(vs);
        dim.setFillColor(sf::Color(0, 0, 0, 120));
        win.draw(dim);

        sf::RectangleShape bg({w, h});
        bg.setPosition({x, y});
        bg.setFillColor(sf::Color(30, 30, 35));
        bg.setOutlineThickness(2.0f);
        bg.setOutlineColor(Theme::Accent);
        win.draw(bg);

        sf::Text tt = text(title, 34, Theme::Accent);
        tt.setStyle(sf::Text::Bold);
        tt.setPosition({x + 20.0f, y + 14.0f});
        win.draw(tt);

        // scrollable content between the title and the buttons
        const float top = y + 48.0f, bottom = y + h - 52.0f;
        viewH = bottom - top;
        const float lineH = 16.0f;
        const float labelW = 150.0f;
        const size_t valueChars = (size_t)std::max(20.0f, (w - 40.0f - labelW) / 6.0f);
        const size_t textChars = (size_t)std::max(20.0f, (w - 40.0f) / 6.0f);

        float cy = top - scroll;
        auto visible = [&](float yy) { return yy >= top - 1.0f && yy + lineH <= bottom + 1.0f; };
        for (const Row& r : rows) {
            std::vector<std::string> lines = wrap(r.value.empty() ? "-" : r.value, valueChars);
            if (visible(cy)) {
                sf::Text l = text(r.label, 24, sf::Color(150, 150, 160));
                l.setPosition({x + 20.0f, cy});
                win.draw(l);
            }
            for (const std::string& ln : lines) {
                if (visible(cy)) {
                    sf::Text v = text(ln, 24, sf::Color(235, 235, 240));
                    v.setPosition({x + 20.0f + labelW, cy});
                    win.draw(v);
                }
                cy += lineH;
            }
            cy += 3.0f;
        }
        if (!longText.empty()) {
            cy += 8.0f;
            if (visible(cy)) {
                sf::Text l = text("Text", 24, sf::Color(150, 150, 160));
                l.setPosition({x + 20.0f, cy});
                win.draw(l);
            }
            cy += lineH + 2.0f;
            for (const std::string& ln : wrap(longText, textChars)) {
                if (visible(cy)) {
                    sf::Text v = text(ln, 24, sf::Color(255, 230, 120));
                    v.setPosition({x + 20.0f, cy});
                    win.draw(v);
                }
                cy += lineH;
            }
        }
        contentH = cy + scroll - top;
        if (contentH > viewH) {
            const float thumbH = std::max(20.0f, viewH * viewH / contentH);
            const float maxScroll = contentH - viewH;
            sf::RectangleShape thumb({4.0f, thumbH});
            thumb.setPosition({x + w - 10.0f, top + (viewH - thumbH) * (scroll / maxScroll)});
            thumb.setFillColor(sf::Color(200, 200, 200, 140));
            win.draw(thumb);
        }

        btnCopy->setPosition(x + 20.0f, y + h - 42.0f);
        btnCopy->draw(win);
        btnClose->setPosition(x + w - 100.0f, y + h - 42.0f);
        btnClose->draw(win);
    }
};
