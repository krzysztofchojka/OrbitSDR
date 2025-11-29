#pragma once

#include <SFML/Graphics.hpp>
#include <algorithm>
#include <string>
#include <vector>
#include <iostream>
#include <cmath>
#include <functional>

// --- ABSTRAKCYJNA KLASA BAZOWA ---
class Widget {
public:
    virtual ~Widget() = default;
    virtual float getHeight() const = 0;
    virtual void setPosition(float x, float y) = 0;
    virtual void draw(sf::RenderWindow& win) = 0;
    virtual void drawOverlay(sf::RenderWindow& win) {} 
    virtual bool handleEvent(const sf::Event& ev, const sf::RenderWindow& win) { return false; }
    virtual void update(const sf::RenderWindow& win) {}
    virtual void setEnabled(bool e) {}
    // Nowa metoda do wykrywania hover (dla kursora)
    virtual bool isMouseOver(const sf::RenderWindow& win) const { return false; }
};

// --- SLIDER ---
class Slider : public Widget {
public:
    sf::RectangleShape track, handle;
    sf::Text label;
    float minVal, maxVal, currentVal;
    bool isDragging = false;
    bool enabled = true;
    std::string name;
    std::function<void(float)> onChange;

    Slider(float w, float minV, float maxV, float startV, std::string n, const sf::Font& font) 
        : minVal(minV), maxVal(maxV), currentVal(startV), name(n), label(font, n, 12) 
    {
        track.setSize({w, 5.f}); track.setFillColor({80, 80, 80});
        handle.setSize({10.f, 20.f}); handle.setFillColor({78, 78, 236}); handle.setOrigin({5.f, 10.f}); 
        label.setFillColor(sf::Color::White);
    }
    float getHeight() const override { return 35.0f; }
    void setWidth(float w) { track.setSize({w, 5.f}); updateHandlePos(); }
    void setPosition(float x, float y) override { track.setPosition({x, y + 20}); label.setPosition({x, y}); updateHandlePos(); }
    void setLimits(float newMin, float newMax) { if (minVal == newMin && maxVal == newMax) return; minVal = newMin; maxVal = newMax; currentVal = std::clamp(currentVal, minVal, maxVal); updateHandlePos(); }
    void setValueSilent(float val) { currentVal = std::clamp(val, minVal, maxVal); updateHandlePos(); }
    void setText(std::string t) { label.setString(t); } // Do aktualizacji na żywo
    void updateHandlePos() { float p = (currentVal - minVal) / (maxVal - minVal); p = std::clamp(p, 0.0f, 1.0f); handle.setPosition({track.getPosition().x + p * track.getSize().x, track.getPosition().y + 2.5f}); }
    
    void setEnabled(bool e) override { enabled = e; track.setFillColor(enabled ? sf::Color(80,80,80) : sf::Color(50,50,50)); handle.setFillColor(enabled ? sf::Color(78, 78, 236) : sf::Color(100,100,100)); }

    bool isMouseOver(const sf::RenderWindow& win) const override {
        if (!enabled) return false;
        sf::Vector2f m = win.mapPixelToCoords(sf::Mouse::getPosition(win));
        sf::FloatRect area = track.getGlobalBounds(); area.position.y -= 10; area.size.y += 20; 
        return area.contains(m);
    }

    bool handleEvent(const sf::Event& ev, const sf::RenderWindow& win) override {
        if (!enabled) return false;
        if (const auto* mb = ev.getIf<sf::Event::MouseButtonPressed>()) {
            if (mb->button == sf::Mouse::Button::Left) {
                sf::Vector2f m = win.mapPixelToCoords(sf::Mouse::getPosition(win));
                sf::FloatRect area = track.getGlobalBounds(); area.position.y -= 10; area.size.y += 20; 
                if (area.contains(m)) { isDragging = true; updateValue(m.x); return true; }
            }
        } else if (const auto* mb = ev.getIf<sf::Event::MouseButtonReleased>()) { if (mb->button == sf::Mouse::Button::Left) isDragging = false; }
        return false;
    }
    void update(const sf::RenderWindow& win) override { if (isDragging && enabled) updateValue(win.mapPixelToCoords(sf::Mouse::getPosition(win)).x); }
    void updateValue(float mx) { float p = std::clamp((mx - track.getPosition().x) / track.getSize().x, 0.0f, 1.0f); currentVal = minVal + p * (maxVal - minVal); updateHandlePos(); if(onChange) onChange(currentVal); }
    void draw(sf::RenderWindow& w) override { w.draw(track); w.draw(handle); w.draw(label); }
};

// --- BUTTON ---
class SdrButton : public Widget {
public:
    sf::RectangleShape shape; sf::Text label; bool active = false; bool enabled = true;
    std::function<void()> onClick;
    SdrButton(float w, float h, std::string t, const sf::Font& font) : label(font, t, 13) {
        shape.setSize({w, h}); shape.setFillColor(sf::Color(40, 40, 45)); shape.setOutlineThickness(1); shape.setOutlineColor(sf::Color(60, 60, 60));
    }
    float getHeight() const override { return shape.getSize().y + 5.0f; }
    void setPosition(float x, float y) override { shape.setPosition({x, y}); centerText(); }
    void centerText() { sf::FloatRect tr = label.getLocalBounds(); sf::Vector2f sp = shape.getPosition(); sf::Vector2f ss = shape.getSize(); label.setPosition({sp.x + (ss.x - tr.size.x)/2.0f, sp.y + (ss.y - tr.size.y)/2.0f - 4.0f}); }
    
    void setEnabled(bool e) override { enabled = e; if(!enabled) shape.setFillColor(sf::Color(40,40,40)); else shape.setFillColor(active ? sf::Color(78, 78, 236) : sf::Color(60, 60, 60)); }

    bool isMouseOver(const sf::RenderWindow& win) const override {
        if (!enabled) return false;
        return shape.getGlobalBounds().contains(win.mapPixelToCoords(sf::Mouse::getPosition(win)));
    }

    bool isClicked(const sf::Event& ev, const sf::RenderWindow& win) {
        if (!enabled) return false;
        if (const auto* mb = ev.getIf<sf::Event::MouseButtonPressed>()) {
            if (mb->button == sf::Mouse::Button::Left) {
                sf::Vector2f m = win.mapPixelToCoords(sf::Mouse::getPosition(win));
                if (shape.getGlobalBounds().contains(m)) { if(onClick) onClick(); return true; }
            }
        } return false;
    }
    bool handleEvent(const sf::Event& ev, const sf::RenderWindow& win) override { return isClicked(ev, win); }
    void setActive(bool a) { active = a; if(enabled) shape.setFillColor(active ? sf::Color(78, 78, 236) : sf::Color(60, 60, 60)); }
    void setText(std::string t) { label.setString(t); centerText(); }
    void setColor(sf::Color c) { if(enabled) shape.setFillColor(c); }
    void draw(sf::RenderWindow& w) override { w.draw(shape); w.draw(label); }
};

// --- CHECKBOX ---
class Checkbox : public Widget {
public:
    sf::RectangleShape box, checkmark; sf::Text label; bool checked = false;
    std::function<void(bool)> onToggle;
    Checkbox(std::string text, const sf::Font& font, bool initial = false) : label(font, text, 12), checked(initial) {
        box.setSize({16, 16}); box.setFillColor(sf::Color(40, 40, 45)); box.setOutlineColor(sf::Color(60, 60, 60)); box.setOutlineThickness(1);
        checkmark.setSize({10, 10}); checkmark.setFillColor(sf::Color::Green); label.setFillColor(sf::Color::White);
    }
    float getHeight() const override { return 25.0f; }
    void setPosition(float x, float y) override { box.setPosition({x, y + 2}); checkmark.setPosition({x + 3, y + 5}); label.setPosition({x + 25, y}); }
    
    bool isMouseOver(const sf::RenderWindow& win) const override {
        sf::Vector2f m = win.mapPixelToCoords(sf::Mouse::getPosition(win));
        sf::FloatRect ca = box.getGlobalBounds(); ca.size.x += label.getGlobalBounds().size.x + 10;
        return ca.contains(m);
    }

    bool handleEvent(const sf::Event& ev, const sf::RenderWindow& win) override {
        if (const auto* mb = ev.getIf<sf::Event::MouseButtonPressed>()) {
            if (mb->button == sf::Mouse::Button::Left) {
                if (isMouseOver(win)) { checked = !checked; if(onToggle) onToggle(checked); return true; }
            }
        } return false;
    }
    void draw(sf::RenderWindow& w) override { w.draw(box); if (checked) w.draw(checkmark); w.draw(label); }
};

// --- DROPDOWN ---
class Dropdown : public Widget {
public:
    sf::RectangleShape mainBox; sf::Text selectedText; sf::Font fontRef;
    bool isOpen = false; std::vector<std::string> options; int selectedIndex = 0; float w, h;
    std::function<void(int)> onChange;
    static Dropdown* currentActive;

    Dropdown(float _w, float _h, const sf::Font& font) : w(_w), h(_h), fontRef(font), selectedText(font, "", 12) {
        mainBox.setSize({w, h}); mainBox.setFillColor(sf::Color(40, 40, 45)); mainBox.setOutlineColor(sf::Color(60, 60, 60)); mainBox.setOutlineThickness(1); selectedText.setFillColor(sf::Color::White);
    }
    float getHeight() const override { return h + 5.0f; }
    void setPosition(float x, float y) override { mainBox.setPosition({x, y}); selectedText.setPosition({x + 5, y + 4}); }
    void setOptions(const std::vector<std::string>& opts) { options = opts; if (!options.empty()) { selectedIndex = 0; selectedText.setString(options[0]); } else { selectedText.setString("None"); } }
    void setSelection(int index) { if (index >= 0 && index < (int)options.size()) { selectedIndex = index; selectedText.setString(options[index]); } }

    bool isMouseOver(const sf::RenderWindow& win) const override {
        sf::Vector2f m = win.mapPixelToCoords(sf::Mouse::getPosition(win));
        if (mainBox.getGlobalBounds().contains(m)) return true;
        if (isOpen) {
            float sx = mainBox.getPosition().x; float sy = mainBox.getPosition().y;
            sf::FloatRect listRect({sx, sy + h}, {w, (float)options.size() * h});
            if (listRect.contains(m)) return true;
        }
        return false;
    }

    bool handleEvent(const sf::Event& ev, const sf::RenderWindow& win) override {
        if (const auto* mb = ev.getIf<sf::Event::MouseButtonPressed>()) {
            if (mb->button == sf::Mouse::Button::Left) {
                sf::Vector2f m = win.mapPixelToCoords(sf::Mouse::getPosition(win));
                if (currentActive && currentActive != this) return false;
                if (isOpen) {
                    float startX = mainBox.getPosition().x; float startY = mainBox.getPosition().y;
                    for (size_t i = 0; i < options.size(); ++i) {
                        sf::FloatRect optRect({startX, startY + (i + 1) * h}, {w, h});
                        if (optRect.contains(m)) { selectedIndex = i; selectedText.setString(options[i]); isOpen = false; currentActive = nullptr; if(onChange) onChange(i); return true; }
                    }
                    isOpen = false; currentActive = nullptr; return true; 
                }
                if (mainBox.getGlobalBounds().contains(m)) { isOpen = !isOpen; if(isOpen) currentActive = this; else currentActive = nullptr; return true; }
            }
        }
        return false;
    }
    void draw(sf::RenderWindow& win) override {
        win.draw(mainBox);
        std::string display = selectedText.getString(); if (display.length() > 22) display = display.substr(0, 20) + "..";
        sf::Text tempTxt = selectedText; tempTxt.setString(display); win.draw(tempTxt);
    }
    void drawOverlay(sf::RenderWindow& win) override {
        if (isOpen) {
            float startX = mainBox.getPosition().x; float startY = mainBox.getPosition().y;
            sf::RectangleShape listBg({w, (float)options.size() * h});
            listBg.setPosition({startX, startY + h}); listBg.setFillColor(sf::Color(50, 50, 50));
            listBg.setOutlineColor(sf::Color(60, 60, 60)); listBg.setOutlineThickness(1);
            win.draw(listBg);
            for (size_t i = 0; i < options.size(); ++i) {
                sf::Text optTxt(fontRef, options[i], 12); optTxt.setPosition({startX + 5, startY + (i + 1) * h + 4});
                sf::Vector2f m = win.mapPixelToCoords(sf::Mouse::getPosition(win)); sf::FloatRect itemRect({startX, startY + (i + 1) * h}, {w, h});
                if (itemRect.contains(m)) { sf::RectangleShape hl({w, h}); hl.setPosition(itemRect.position); hl.setFillColor(sf::Color(80, 80, 100)); win.draw(hl); }
                std::string s = options[i]; if (s.length() > 22) s = s.substr(0, 20) + ".."; optTxt.setString(s); win.draw(optTxt);
            }
        }
    }
};
inline Dropdown* Dropdown::currentActive = nullptr;

class Label : public Widget { sf::Text text; public: Label(std::string c, const sf::Font& f, int s=12, sf::Color cl=sf::Color::White) : text(f, c, s) { text.setFillColor(cl); } float getHeight() const override { return text.getCharacterSize() + 8.0f; } void setPosition(float x, float y) override { text.setPosition({x, y}); } void setText(std::string s) { text.setString(s); } void draw(sf::RenderWindow& w) override { w.draw(text); } };

class RowContainer : public Widget {
public: std::vector<std::shared_ptr<Widget>> widgets; float height = 30.0f; float spacing = 5.0f;
    void add(std::shared_ptr<Widget> w) { widgets.push_back(w); if (w->getHeight() > height) height = w->getHeight(); }
    float getHeight() const override { return height + 5.0f; }
    void setPosition(float x, float y) override { float cx = x; for(auto& w : widgets) { w->setPosition(cx, y); SdrButton* b = dynamic_cast<SdrButton*>(w.get()); if(b) cx += b->shape.getSize().x + spacing; else cx += 100 + spacing; } }
    void draw(sf::RenderWindow& w) override { for(auto& o : widgets) o->draw(w); }
    void setEnabled(bool e) override { for(auto& w : widgets) w->setEnabled(e); }
    bool isMouseOver(const sf::RenderWindow& win) const override { for(auto& w : widgets) if(w->isMouseOver(win)) return true; return false; }
    bool handleEvent(const sf::Event& ev, const sf::RenderWindow& win) override { bool h = false; for(auto& o : widgets) if(o->handleEvent(ev, win)) h = true; return h; }
};

class FrequencyDisplay {
public:
    long long frequency; const sf::Font& font; sf::Text text; sf::RectangleShape hoverRect; long long hoverPower = 0; bool isHovered = false, isTopHalf = true, enabled = true; float x, y;
    FrequencyDisplay(float _x, float _y, const sf::Font& f) : font(f), text(font), x(_x), y(_y) { frequency = 100000000; text.setCharacterSize(42); text.setFillColor(sf::Color::White); hoverRect.setFillColor(sf::Color(255, 255, 255, 30)); setPosition(_x, _y); }
    void setPosition(float _x, float _y) { x = _x; y = _y; text.setPosition({x + 8, y}); }
    void setFrequency(long long f) { frequency = f; } long long getFrequency() const { return frequency; } void setEnabled(bool e) { enabled = e; }
    std::string formatWithDots(long long freq) { std::string s = std::to_string(freq); while (s.length() < 10) s = "0" + s; std::string r = ""; int c = 0; for (int i = s.length() - 1; i >= 0; i--) { r = s[i] + r; c++; if (c % 3 == 0 && i > 0) r = "." + r; } return r; }
    void update(const sf::RenderWindow& win) {
        text.setString(formatWithDots(frequency)); if (!enabled) { isHovered = false; return; }
        sf::Vector2f m = win.mapPixelToCoords(sf::Mouse::getPosition(win)); sf::FloatRect b = text.getGlobalBounds(); isHovered = false; hoverPower = 0; sf::FloatRect hb = b; hb.position.x -= 2; hb.size.x += 4; hb.position.y -= 2; hb.size.y += 4;
        if (hb.contains(m)) { long long cp = 1; float vt = b.position.y; float vh = b.size.y; for (int i = text.getString().getSize() - 1; i >= 0; i--) { char c = text.getString()[i]; if (c == '.') continue; sf::Vector2f cp_pos = text.findCharacterPos(i); float cw = text.findCharacterPos(i + 1).x - cp_pos.x; if (cw <= 0) cw = text.getCharacterSize() * 0.6f; sf::FloatRect cr({cp_pos.x, vt - 2.0f}, {cw, vh + 4.0f}); if (cr.contains(m)) { isHovered = true; hoverPower = cp; float midY = cr.position.y + (cr.size.y / 2.0f); isTopHalf = (m.y < midY); hoverRect.setSize({cw, cr.size.y / 2.0f}); if (isTopHalf) hoverRect.setPosition(cr.position); else hoverRect.setPosition({cr.position.x, midY}); break; } cp *= 10; } }
    }
    bool handleEvent(const sf::Event& ev) { if (!enabled || !isHovered || hoverPower == 0) return false; if (const auto* mb = ev.getIf<sf::Event::MouseButtonPressed>()) { if (mb->button == sf::Mouse::Button::Left) { if (isTopHalf) frequency += hoverPower; else frequency -= hoverPower; if (frequency < 0) frequency = 0; return true; } } if (const auto* sc = ev.getIf<sf::Event::MouseWheelScrolled>()) { if (sc->wheel == sf::Mouse::Wheel::Vertical) { if (sc->delta > 0) frequency += hoverPower; else frequency -= hoverPower; if (frequency < 0) frequency = 0; return true; } } return false; }
    void draw(sf::RenderWindow& win) { sf::FloatRect b = text.getGlobalBounds(); sf::RectangleShape bg({b.size.x + 30, b.size.y + 28}); bg.setPosition({x - 5, y - 8}); bg.setFillColor(sf::Color(19, 19, 21)); /*bg.setOutlineColor(enabled ? sf::Color(60, 60, 60) : sf::Color(40, 40, 45)); bg.setOutlineThickness(1); win.draw(bg); */if (enabled && isHovered && hoverPower > 0) win.draw(hoverRect); std::string str = formatWithDots(frequency); bool lz = true; sf::Text tt = text; for (size_t i = 0; i < str.length(); ++i) { char c = str[i]; if (c != '0' && c != '.') lz = false; if (i == str.length() - 1) lz = false; if (!enabled) tt.setFillColor(sf::Color(60, 60, 60)); else { if (lz) tt.setFillColor(sf::Color(90, 90, 90)); else tt.setFillColor(sf::Color::White); } sf::Vector2f p = text.findCharacterPos(i); tt.setPosition(p); tt.setString(std::string(1, c)); win.draw(tt); } }
    bool isMouseOver(const sf::RenderWindow& win) const { return isHovered; }
};