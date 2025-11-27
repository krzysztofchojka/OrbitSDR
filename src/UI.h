#pragma once

#include <SFML/Graphics.hpp>
#include <algorithm>
#include <string>
#include <vector>
#include <iostream>
#include <cmath>

// --- SLIDER CLASS ---
class Slider {
public:
    sf::RectangleShape track, handle;
    sf::Text label;
    float minVal, maxVal, currentVal;
    bool isDragging = false;
    std::string name;

    Slider(float x, float y, float w, float minV, float maxV, float startV, std::string n, const sf::Font& font) 
        : minVal(minV), maxVal(maxV), currentVal(startV), name(n), label(font, n, 12) 
    {
        track.setSize({w, 5.f}); 
        track.setFillColor({80, 80, 80});
        
        handle.setSize({10.f, 20.f}); 
        handle.setFillColor({78, 78, 236}); 
        handle.setOrigin({5.f, 10.f}); // SFML 3: Vector
        
        label.setFillColor(sf::Color::White);
        
        setPosition(x, y);
    }

    void setPosition(float x, float y) {
        track.setPosition({x, y}); // SFML 3: Vector
        label.setPosition({x, y - 15}); // SFML 3: Vector
        updateHandlePos();
    }
    
    void setWidth(float w) {
        track.setSize({w, 5.f}); // SFML 3: Vector
        updateHandlePos();
    }

    void setLimits(float newMin, float newMax) {
        if (minVal == newMin && maxVal == newMax) return;
        minVal = newMin; maxVal = newMax;
        if (currentVal < minVal) currentVal = minVal;
        if (currentVal > maxVal) currentVal = maxVal;
        updateHandlePos();
    }

    void setText(std::string t) { label.setString(t); }

    void updateHandlePos() {
        float p = (currentVal - minVal) / (maxVal - minVal);
        p = std::clamp(p, 0.0f, 1.0f);
        // SFML 3: Vector
        handle.setPosition({
            track.getPosition().x + p * track.getSize().x, 
            track.getPosition().y + 2.5f
        });
    }

    void handleEvent(const sf::Event& ev, const sf::RenderWindow& win) {
        if (const auto* mb = ev.getIf<sf::Event::MouseButtonPressed>()) {
            if (mb->button == sf::Mouse::Button::Left) {
                sf::Vector2f m = win.mapPixelToCoords(sf::Mouse::getPosition(win));
                sf::FloatRect area = track.getGlobalBounds(); 
                // SFML 3: Rect members change
                area.position.y -= 10; area.size.y += 20; 
                
                if (area.contains(m)) { 
                    isDragging = true; 
                    updateValue(m.x); 
                }
            }
        } else if (const auto* mb = ev.getIf<sf::Event::MouseButtonReleased>()) {
            if (mb->button == sf::Mouse::Button::Left) isDragging = false;
        }
    }

    void update(const sf::RenderWindow& win) {
        if (isDragging) updateValue(win.mapPixelToCoords(sf::Mouse::getPosition(win)).x);
    }

    void updateValue(float mx) {
        // SFML 3: getSize().x instead of getGlobalBounds().width for shapes is safer usually, or track.getSize().x
        float p = std::clamp((mx - track.getPosition().x) / track.getSize().x, 0.0f, 1.0f);
        currentVal = minVal + p * (maxVal - minVal);
        updateHandlePos();
    }

    void draw(sf::RenderWindow& w) { w.draw(track); w.draw(handle); w.draw(label); }
};

class SdrButton {
public:
    sf::RectangleShape shape;
    sf::Text label;
    bool active = false;

    SdrButton(float x, float y, float w, float h, std::string t, const sf::Font& font) 
        : label(font, t, 14) 
    {
        shape.setSize({w, h}); // SFML 3
        shape.setFillColor(sf::Color(60, 60, 60));
        shape.setOutlineThickness(1); 
        shape.setOutlineColor(sf::Color::White);
        setPosition(x, y);
    }

    void setPosition(float x, float y) {
        shape.setPosition({x, y}); // SFML 3
        centerText();
    }
    
    void centerText() {
        sf::FloatRect textRect = label.getLocalBounds();
        sf::Vector2f shapePos = shape.getPosition();
        sf::Vector2f shapeSize = shape.getSize();
        
        // SFML 3: Rect uses size.x/y instead of width/height
        label.setPosition({
            shapePos.x + (shapeSize.x - textRect.size.x) / 2.0f, 
            shapePos.y + (shapeSize.y - textRect.size.y) / 2.0f - 4.0f
        });
    }

    bool isClicked(const sf::Event& ev, const sf::RenderWindow& win) {
        if (const auto* mb = ev.getIf<sf::Event::MouseButtonPressed>()) {
            if (mb->button == sf::Mouse::Button::Left) {
                sf::Vector2f m = win.mapPixelToCoords(sf::Mouse::getPosition(win));
                if (shape.getGlobalBounds().contains(m)) return true;
            }
        }
        return false;
    }

    void setActive(bool a) {
        active = a;
        shape.setFillColor(active ? sf::Color(78, 78, 236) : sf::Color(60, 60, 60));
    }

    void setText(std::string t) { label.setString(t); centerText(); }
    void setColor(sf::Color c) { shape.setFillColor(c); }
    void draw(sf::RenderWindow& w) { w.draw(shape); w.draw(label); }
};

class Dropdown {
public:
    sf::RectangleShape mainBox;
    sf::Text selectedText;
    sf::Font fontRef;
    bool isOpen = false;
    std::vector<std::string> options;
    int selectedIndex = 0;
    float x, y, w, h;

    Dropdown(float _x, float _y, float _w, float _h, const sf::Font& font) 
        : x(_x), y(_y), w(_w), h(_h), fontRef(font), selectedText(font, "", 12) 
    {
        mainBox.setSize({w, h}); // SFML 3
        mainBox.setFillColor(sf::Color(60, 60, 60));
        mainBox.setOutlineColor(sf::Color::White); 
        mainBox.setOutlineThickness(1);
        selectedText.setFillColor(sf::Color::White);
        setPosition(_x, _y);
    }

    void setPosition(float _x, float _y) {
        x = _x; y = _y;
        mainBox.setPosition({x, y}); // SFML 3
        selectedText.setPosition({x + 5, y + 5}); // SFML 3
    }

    void setOptions(const std::vector<std::string>& opts) {
        options = opts;
        if (!options.empty()) { selectedIndex = 0; selectedText.setString(options[0]); } 
        else { selectedText.setString("No Devices"); }
    }

    void setSelection(int index) {
        if (index >= 0 && index < (int)options.size()) {
            selectedIndex = index; 
            selectedText.setString(options[index]);
        }
    }

    bool handleEvent(const sf::Event& ev, const sf::RenderWindow& win) {
        if (const auto* mb = ev.getIf<sf::Event::MouseButtonPressed>()) {
            if (mb->button == sf::Mouse::Button::Left) {
                sf::Vector2f m = win.mapPixelToCoords(sf::Mouse::getPosition(win));
                
                if (mainBox.getGlobalBounds().contains(m)) { 
                    isOpen = !isOpen; 
                    return false; 
                }
                
                if (isOpen) {
                    for (size_t i = 0; i < options.size(); ++i) {
                        // SFML 3: Rect ctor needs vector
                        sf::FloatRect optionRect({x, y + (i + 1) * h}, {w, h});
                        if (optionRect.contains(m)) {
                            selectedIndex = i; 
                            selectedText.setString(options[i]);
                            isOpen = false; 
                            return true; 
                        }
                    }
                    isOpen = false; 
                }
            }
        }
        return false;
    }

    void draw(sf::RenderWindow& win) {
        win.draw(mainBox);
        std::string display = selectedText.getString();
        if (display.length() > 22) display = display.substr(0, 20) + "..";
        sf::Text tempTxt = selectedText; tempTxt.setString(display); 
        win.draw(tempTxt);

        if (isOpen) {
            for (size_t i = 0; i < options.size(); ++i) {
                sf::RectangleShape optBox({w, h}); 
                optBox.setPosition({x, y + (i + 1) * h}); // SFML 3
                optBox.setFillColor(sf::Color(80, 80, 80)); 
                optBox.setOutlineColor(sf::Color(100, 100, 100)); 
                optBox.setOutlineThickness(1);
                
                sf::Text optTxt(fontRef, options[i], 12); 
                optTxt.setPosition({x + 5, y + (i + 1) * h + 5}); // SFML 3
                
                sf::Vector2f m = win.mapPixelToCoords(sf::Mouse::getPosition(win));
                if (optBox.getGlobalBounds().contains(m)) optBox.setFillColor(sf::Color(120, 120, 120));
                
                win.draw(optBox);
                std::string s = options[i]; if (s.length() > 22) s = s.substr(0, 20) + "..";
                optTxt.setString(s); 
                win.draw(optTxt);
            }
        }
    }
};

class FrequencyDisplay {
public:
    long long frequency;
    const sf::Font& font;
    sf::Text text;
    sf::RectangleShape hoverRect;
    long long hoverPower = 0;
    bool isHovered = false;
    bool isTopHalf = true;
    bool enabled = true;
    float x, y;

    FrequencyDisplay(float _x, float _y, const sf::Font& f) 
        : font(f), text(font), x(_x), y(_y) 
    {
        frequency = 100000000;
        text.setCharacterSize(42); 
        text.setFillColor(sf::Color::White);
        hoverRect.setFillColor(sf::Color(255, 255, 255, 30)); 
        setPosition(_x, _y);
    }

    void setPosition(float _x, float _y) {
        x = _x; y = _y;
        text.setPosition({x + 8, y}); // SFML 3
    }

    void setFrequency(long long f) { frequency = f; }
    long long getFrequency() const { return frequency; }
    void setEnabled(bool e) { enabled = e; }

    std::string formatWithDots(long long freq) {
        std::string s = std::to_string(freq);
        while (s.length() < 10) s = "0" + s; 
        std::string result = "";
        int count = 0;
        for (int i = s.length() - 1; i >= 0; i--) {
            result = s[i] + result;
            count++;
            if (count % 3 == 0 && i > 0) result = "." + result;
        }
        return result;
    }

    void update(const sf::RenderWindow& win) {
        std::string str = formatWithDots(frequency);
        text.setString(str); 
        if (!enabled) { isHovered = false; return; }

        sf::Vector2f m = win.mapPixelToCoords(sf::Mouse::getPosition(win));
        sf::FloatRect bounds = text.getGlobalBounds();
        isHovered = false; hoverPower = 0;

        sf::FloatRect hitBounds = bounds; 
        hitBounds.position.x -= 2; hitBounds.size.x += 4; 
        hitBounds.position.y -= 2; hitBounds.size.y += 4;

        if (hitBounds.contains(m)) {
            long long currentPower = 1;
            float visualTop = bounds.position.y;
            float visualHeight = bounds.size.y;

            for (int i = str.length() - 1; i >= 0; i--) {
                char c = str[i];
                if (c == '.') continue; 
                sf::Vector2f charPos = text.findCharacterPos(i);
                float charWidth = text.findCharacterPos(i + 1).x - charPos.x;
                if (charWidth <= 0) charWidth = text.getCharacterSize() * 0.6f; 

                sf::FloatRect charRect({charPos.x, visualTop - 2.0f}, {charWidth, visualHeight + 4.0f});
                
                if (charRect.contains(m)) {
                    isHovered = true;
                    hoverPower = currentPower;
                    float midY = charRect.position.y + (charRect.size.y / 2.0f);
                    isTopHalf = (m.y < midY);
                    hoverRect.setSize({charWidth, charRect.size.y / 2.0f});
                    if (isTopHalf) hoverRect.setPosition(charRect.position);
                    else hoverRect.setPosition({charRect.position.x, midY});
                    break; 
                }
                currentPower *= 10;
            }
        }
    }

    bool handleEvent(const sf::Event& ev) {
        if (!enabled || !isHovered || hoverPower == 0) return false;
        if (const auto* mb = ev.getIf<sf::Event::MouseButtonPressed>()) {
            if (mb->button == sf::Mouse::Button::Left) {
                if (isTopHalf) frequency += hoverPower; else frequency -= hoverPower;
                if (frequency < 0) frequency = 0;
                return true; 
            }
        }
        if (const auto* scroll = ev.getIf<sf::Event::MouseWheelScrolled>()) {
            if (scroll->wheel == sf::Mouse::Wheel::Vertical) {
                if (scroll->delta > 0) frequency += hoverPower; else frequency -= hoverPower;
                if (frequency < 0) frequency = 0;
                return true; 
            }
        }
        return false;
    }

    void draw(sf::RenderWindow& win) {
        sf::FloatRect b = text.getGlobalBounds();
        sf::RectangleShape bg({b.size.x + 30, b.size.y + 28});
        bg.setPosition({x - 5, y - 8});
        bg.setFillColor(sf::Color(20, 20, 20)); 
        bg.setOutlineColor(enabled ? sf::Color(60, 60, 60) : sf::Color(40, 40, 40));
        bg.setOutlineThickness(1);
        win.draw(bg);

        if (enabled && isHovered && hoverPower > 0) win.draw(hoverRect);

        std::string str = formatWithDots(frequency);
        bool leadingZero = true;
        sf::Text tempText = text; 
        
        for (size_t i = 0; i < str.length(); ++i) {
            char c = str[i];
            if (c != '0' && c != '.') leadingZero = false;
            if (i == str.length() - 1) leadingZero = false; 
            if (!enabled) tempText.setFillColor(sf::Color(60, 60, 60)); 
            else {
                if (leadingZero) tempText.setFillColor(sf::Color(90, 90, 90)); 
                else tempText.setFillColor(sf::Color::White); 
            }
            sf::Vector2f p = text.findCharacterPos(i);
            tempText.setPosition(p);
            tempText.setString(std::string(1, c));
            win.draw(tempText);
        }
    }
};

class Checkbox {
public:
    sf::RectangleShape box;
    sf::RectangleShape checkmark;
    sf::Text label;
    bool checked = false;

    Checkbox(float x, float y, std::string text, const sf::Font& font, bool initial = false) 
        : label(font, text, 12), checked(initial) 
    {
        box.setSize({20, 20});
        box.setPosition({x, y}); // SFML 3
        box.setFillColor(sf::Color(40, 40, 40));
        box.setOutlineColor(sf::Color::White);
        box.setOutlineThickness(1);

        checkmark.setSize({10, 10});
        checkmark.setPosition({x + 5, y + 5}); // SFML 3
        checkmark.setFillColor(sf::Color::Green);

        label.setFillColor(sf::Color::White);
        label.setPosition({x, y - 16}); // SFML 3
    }

    void setPosition(float x, float y) {
        box.setPosition({x, y});
        checkmark.setPosition({x + 5, y + 5});
        label.setPosition({x, y - 16});
    }

    bool isClicked(const sf::Event& ev, const sf::RenderWindow& win) {
        if (const auto* mb = ev.getIf<sf::Event::MouseButtonPressed>()) {
            if (mb->button == sf::Mouse::Button::Left) {
                sf::Vector2f m = win.mapPixelToCoords(sf::Mouse::getPosition(win));
                if (box.getGlobalBounds().contains(m)) {
                    checked = !checked;
                    return true;
                }
            }
        }
        return false;
    }

    void draw(sf::RenderWindow& w) {
        w.draw(box);
        if (checked) w.draw(checkmark);
        w.draw(label);
    }
};