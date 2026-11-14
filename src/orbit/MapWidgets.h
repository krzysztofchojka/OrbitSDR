#pragma once

// Small widgets used by the satellite map panel (on top of UI.h / Sidebar.h)

#include "../UI.h"
#include "../Sidebar.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <cmath>

namespace orbit {

// Crisp text the same way as the rest of the app (double size, scaled by 0.5), UTF-8 aware.
// align: 0 = left, 1 = centre, 2 = right of x
inline sf::Text makeText(const sf::Font& font, const std::string& s, float x, float y, unsigned size,
                         sf::Color c, bool bold = false, int align = 0) {
    sf::Text t(font, sf::String::fromUtf8(s.begin(), s.end()), size * 2);
    t.setScale({0.5f, 0.5f});
    t.setFillColor(c);
    if (bold) t.setStyle(sf::Text::Bold);
    float w = t.getGlobalBounds().size.x;
    if (align == 1) x -= w / 2.0f;
    else if (align == 2) x -= w;
    t.setPosition({std::round(x), std::round(y)});
    return t;
}
inline void drawText(sf::RenderTarget& rt, const sf::Font& font, const std::string& s, float x, float y, unsigned size,
                     sf::Color c, bool bold = false, int align = 0) {
    rt.draw(makeText(font, s, x, y, size, c, bold, align));
}
inline float textWidth(const sf::Font& font, const std::string& s, unsigned size) {
    sf::Text t(font, sf::String::fromUtf8(s.begin(), s.end()), size * 2);
    return t.getGlobalBounds().size.x * 0.5f;
}

// Five-pointed star (favourites), drawn rather than a glyph so every font works
inline void drawStar(sf::RenderTarget& rt, float cx, float cy, float r, sf::Color fill, sf::Color outline) {
    sf::ConvexShape s(10);
    for (int i = 0; i < 10; i++) {
        const float a = -1.5707963f + i * 3.14159265f / 5.0f;
        const float rr = (i % 2 == 0) ? r : r * 0.45f;
        s.setPoint(i, {cx + rr * std::cos(a), cy + rr * std::sin(a)});
    }
    s.setFillColor(fill);
    s.setOutlineColor(outline);
    s.setOutlineThickness(1.0f);
    rt.draw(s);
}

// Widgets in one row, each with its own width (RowContainer assumes 100 px for non-buttons)
class HRow : public Widget {
public:
    std::vector<std::pair<std::shared_ptr<Widget>, float>> items;
    float gap = 6.0f;
    void add(std::shared_ptr<Widget> w, float width) { items.push_back({w, width}); }
    float getHeight() const override {
        float h = 0;
        for (auto& it : items) h = std::max(h, it.first->getHeight());
        return h;
    }
    void setPosition(float x, float y) override {
        float cx = x;
        for (auto& it : items) { it.first->setPosition(cx, y); cx += it.second + gap; }
    }
    void draw(sf::RenderWindow& w) override { for (auto& it : items) it.first->draw(w); }
    void drawOverlay(sf::RenderWindow& w) override { for (auto& it : items) it.first->drawOverlay(w); }
    bool handleEvent(const sf::Event& ev, const sf::RenderWindow& win) override {
        for (auto& it : items) if (it.first->handleEvent(ev, win)) return true;
        return false;
    }
    void update(const sf::RenderWindow& win) override { for (auto& it : items) it.first->update(win); }
    bool isMouseOver(const sf::RenderWindow& win) const override {
        for (auto& it : items) if (it.first->isMouseOver(win)) return true;
        return false;
    }
    void updateStyle() override { for (auto& it : items) it.first->updateStyle(); }
};

// A widget drawn by a function (lists, plots, info blocks)
class CustomWidget : public Widget {
public:
    float w, h;
    sf::FloatRect rect;
    std::function<float()> heightFn;
    std::function<void(sf::RenderWindow&, const sf::FloatRect&)> onDraw;
    // local = mouse position relative to the widget's top-left corner
    std::function<bool(const sf::Event&, sf::Vector2f local)> onEvent;
    bool hoverHand = false;   // show the hand cursor over it

    CustomWidget(float width, float height) : w(width), h(height) {}
    float getHeight() const override { return heightFn ? heightFn() : h; }
    void setPosition(float x, float y) override { rect = sf::FloatRect({x, y}, {w, getHeight()}); }
    bool isMouseOver(const sf::RenderWindow& win) const override {
        return rect.contains(win.mapPixelToCoords(sf::Mouse::getPosition(win)));
    }
    bool handleEvent(const sf::Event& ev, const sf::RenderWindow& win) override {
        if (!onEvent) return false;
        const bool isPress = ev.is<sf::Event::MouseButtonPressed>(), isWheel = ev.is<sf::Event::MouseWheelScrolled>();
        if (!isPress && !isWheel) return false;
        const sf::Vector2f m = win.mapPixelToCoords(sf::Mouse::getPosition(win));
        if (!rect.contains(m)) return false;
        return onEvent(ev, {m.x - rect.position.x, m.y - rect.position.y});
    }
    void draw(sf::RenderWindow& win) override { if (onDraw) onDraw(win, rect); }
};

// Small caption above a text field
class LabeledInput : public Widget {
public:
    std::shared_ptr<TextInput> input;
    sf::Text caption;
    LabeledInput(const std::string& cap, float width, const sf::Font& font)
        : input(std::make_shared<TextInput>(width, 24.0f, font)), caption(font, cap, 20) {
        caption.setScale({0.5f, 0.5f});
        caption.setFillColor(sf::Color(150, 150, 160));
    }
    float getHeight() const override { return 14.0f + input->getHeight(); }
    void setPosition(float x, float y) override { caption.setPosition({x, y}); input->setPosition(x, y + 14.0f); }
    void draw(sf::RenderWindow& w) override { w.draw(caption); input->draw(w); }
    bool handleEvent(const sf::Event& ev, const sf::RenderWindow& win) override { return input->handleEvent(ev, win); }
    bool isMouseOver(const sf::RenderWindow& win) const override { return input->isMouseOver(win); }
    void updateStyle() override { input->updateStyle(); }
};

} // namespace orbit
