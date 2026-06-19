#pragma once

#include <SFML/Graphics.hpp>
#include <algorithm>
#include <string>
#include <vector>
#include <iostream>
#include <cmath>
#include <functional>

// --- THEME SYSTEM ---
namespace Theme {
    inline sf::Color Accent     = sf::Color(0, 180, 255); 
    inline sf::Color AccentDim  = sf::Color(0, 100, 150); 
    inline sf::Color BgDark     = sf::Color(30, 30, 35);   
    inline sf::Color BgDarker   = sf::Color(19, 19, 21);   
    inline sf::Color Text       = sf::Color(240, 240, 240);
    inline sf::Color Glow       = sf::Color(0, 180, 255, 100);

    inline void setTheme(int index) {
        if (index == 0 || index == 4) { // Orbit Original (Blue)
            Accent = sf::Color(0, 190, 255); 
            AccentDim = sf::Color(0, 100, 140); 
            Glow = sf::Color(0, 190, 255, 120);
        } 
        else if (index == 1) { // Neon (Pink/Purple)
            // Changed to Pink to match the new waterfall colors
            Accent = sf::Color(255, 0, 150);     // Hot Pink
            AccentDim = sf::Color(150, 0, 80);   // Darker Pink
            Glow = sf::Color(255, 0, 255, 100);  // Purple Glow
        } 
        else if (index == 2) { // Matrix
            Accent = sf::Color(50, 255, 100); 
            AccentDim = sf::Color(20, 160, 50); 
            Glow = sf::Color(50, 255, 100, 100);
        } 
        else if (index == 3) { // Grayscale
            Accent = sf::Color(220, 220, 220); 
            AccentDim = sf::Color(120, 120, 120); 
            Glow = sf::Color(255, 255, 255, 80);
        } 
        /*else { // Orbit Plus
            Accent = sf::Color(0, 190, 255); 
            AccentDim = sf::Color(0, 100, 140); 
            Glow = sf::Color(0, 190, 255, 120);
        }*/
    }
}

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
    virtual bool isMouseOver(const sf::RenderWindow& win) const { return false; }
    virtual void updateStyle() {} 
};

// --- MODERN SLIDER (RETINA FIX V2 - NO ROUNDING ON MOVING PARTS) ---
class Slider : public Widget {
public:
    sf::RectangleShape trackBg, trackFill;
    sf::CircleShape knob, knobInner, halo;
    sf::Text label, valueDisplay;
    float minVal, maxVal, currentVal;
    bool isDragging = false, enabled = true;
    std::function<void(float)> onChange;

// --- MODERN SLIDER (RETINA FIX V2 - NO ROUNDING ON MOVING PARTS) ---
    Slider(float w, float minV, float maxV, float startV, std::string n, const sf::Font& font) 
        : minVal(minV), maxVal(maxV), currentVal(startV), 
          label(font, n, 24), valueDisplay(font, "", 20) 
    {
        label.setScale({0.5f, 0.5f});
        valueDisplay.setScale({0.5f, 0.5f});

        trackBg.setSize({w * 2.0f, 8.0f}); 
        trackBg.setScale({0.5f, 0.5f});
        trackBg.setFillColor(sf::Color(50, 50, 55));
        
        trackFill.setSize({0, 8.0f}); 
        trackFill.setScale({0.5f, 0.5f});

        // SUPER HIGH RES CIRCLES (128 points)
        knob.setRadius(16.0f); 
        knob.setOrigin({16.0f, 16.0f}); 
        knob.setScale({0.5f, 0.5f});
        knob.setPointCount(128); // Large number of points for smooth rendering
        knob.setOutlineThickness(4.0f); 

        knobInner.setRadius(6.0f); 
        knobInner.setOrigin({6.0f, 6.0f}); 
        knobInner.setScale({0.5f, 0.5f});
        knobInner.setPointCount(64); 
        knobInner.setFillColor(sf::Color(40, 40, 45));

        halo.setRadius(28.0f); 
        halo.setOrigin({28.0f, 28.0f}); 
        halo.setScale({0.5f, 0.5f});
        halo.setPointCount(128);
        
        label.setFillColor(Theme::Text); label.setStyle(sf::Text::Bold);
        valueDisplay.setFillColor(sf::Color(180, 180, 180));
        updateStyle(); updateHandlePos();
    }

    void updateStyle() override {
        if (enabled) {
            trackFill.setFillColor(Theme::Accent); knob.setFillColor(Theme::BgDark); knob.setOutlineColor(Theme::Accent); halo.setFillColor(Theme::Glow);
        } else {
            trackFill.setFillColor(sf::Color(80, 80, 80)); knob.setOutlineColor(sf::Color(80, 80, 80)); halo.setFillColor(sf::Color::Transparent);
        }
    }

    float getHeight() const override { return 40.0f; }
    void setWidth(float w) { trackBg.setSize({w * 2.0f, 8.0f}); updateHandlePos(); }
    void setPosition(float x, float y) override { 
        // Round static layout elements
        x = std::round(x); y = std::round(y);
        label.setPosition({x, y});
        float trackY = y + 22.0f; // No rounding here to allow precise half-pixel placement
        trackBg.setPosition({x, trackY}); trackFill.setPosition({x, trackY});
        
        float valX = x + trackBg.getGlobalBounds().size.x - valueDisplay.getGlobalBounds().size.x;
        valueDisplay.setPosition({std::round(valX), y + 2});
        updateHandlePos(); 
    }
    void setLimits(float newMin, float newMax) { if (minVal == newMin && maxVal == newMax) return; minVal = newMin; maxVal = newMax; currentVal = std::clamp(currentVal, minVal, maxVal); updateHandlePos(); }
    void setValueSilent(float val) { currentVal = std::clamp(val, minVal, maxVal); updateHandlePos(); }
    void setText(std::string t) { label.setString(t); } 

    void updateHandlePos() { 
        float p = (currentVal - minVal) / (maxVal - minVal); p = std::clamp(p, 0.0f, 1.0f);
        
        // --- REMOVED ROUNDING (std::round) FOR SMOOTHNESS ON RETINA DISPLAYS ---
        // On Retina screens, 1.0f coordinates map to 2.0 physical pixels. 
        // Float values (e.g., 10.5) are rendered perfectly sharp without rounding.
        
        float width = trackBg.getGlobalBounds().size.x;
        float xPos = trackBg.getPosition().x + (p * width);
        float yPos = trackBg.getPosition().y + (trackBg.getGlobalBounds().size.y / 2.0f);
        
        knob.setPosition({xPos, yPos}); 
        knobInner.setPosition({xPos, yPos}); 
        halo.setPosition({xPos, yPos});
        
        // Fill width internal (x2)
        trackFill.setSize({p * trackBg.getSize().x, 8.0f});
    }
    
    void setEnabled(bool e) override { enabled = e; updateStyle(); }
    bool isMouseOver(const sf::RenderWindow& win) const override {
        if (!enabled) return false; sf::Vector2f m = win.mapPixelToCoords(sf::Mouse::getPosition(win));
        sf::FloatRect area = trackBg.getGlobalBounds(); area.position.y -= 15; area.size.y += 30; return area.contains(m);
    }
    bool handleEvent(const sf::Event& ev, const sf::RenderWindow& win) override {
        if (!enabled) return false;
        if (const auto* mb = ev.getIf<sf::Event::MouseButtonPressed>()) { if (mb->button == sf::Mouse::Button::Left && isMouseOver(win)) { isDragging = true; updateValue(win.mapPixelToCoords(sf::Mouse::getPosition(win)).x); return true; } } 
        else if (const auto* mb = ev.getIf<sf::Event::MouseButtonReleased>()) { if (mb->button == sf::Mouse::Button::Left) isDragging = false; }
        return false;
    }
    void update(const sf::RenderWindow& win) override { 
        if (isDragging && enabled) updateValue(win.mapPixelToCoords(sf::Mouse::getPosition(win)).x); 
        if (isDragging) { halo.setRadius(32.0f); halo.setOrigin({32.0f, 32.0f}); } 
        else { halo.setRadius(28.0f); halo.setOrigin({28.0f, 28.0f}); }
    }
    void updateValue(float mx) { 
        float trackX = trackBg.getPosition().x; float trackW = trackBg.getGlobalBounds().size.x;
        float p = std::clamp((mx - trackX) / trackW, 0.0f, 1.0f); currentVal = minVal + p * (maxVal - minVal); updateHandlePos(); if(onChange) onChange(currentVal); 
    }
    void draw(sf::RenderWindow& w) override { w.draw(label); w.draw(trackBg); w.draw(trackFill); if (enabled && (isDragging || isMouseOver(w))) w.draw(halo); w.draw(knob); w.draw(knobInner); }
};

// --- MODERN BUTTON ---
class SdrButton : public Widget {
public:
    sf::RectangleShape shape; sf::Text label; bool active = false; bool enabled = true; std::function<void()> onClick;
    SdrButton(float w, float h, std::string t, const sf::Font& font) : label(font, t, 24) { 
        label.setScale({0.5f, 0.5f}); 
        shape.setSize({w, h}); shape.setOutlineThickness(1); updateStyle();
    }
    void updateStyle() override {
        if (!enabled) { shape.setFillColor(sf::Color(40, 40, 40)); shape.setOutlineColor(sf::Color(50, 50, 55)); label.setFillColor(sf::Color(100, 100, 100)); } 
        else { label.setFillColor(Theme::Text); if (active) { shape.setFillColor(Theme::AccentDim); shape.setOutlineColor(Theme::Accent); label.setFillColor(sf::Color::White); } else { shape.setFillColor(Theme::BgDark); shape.setOutlineColor(sf::Color(70, 70, 70)); } }
    }
    float getHeight() const override { return shape.getSize().y + 5.0f; }
    void setPosition(float x, float y) override { shape.setPosition({std::round(x), std::round(y)}); centerText(); }
    void centerText() { 
        sf::FloatRect tr = label.getGlobalBounds(); 
        sf::Vector2f sp = shape.getPosition(); sf::Vector2f ss = shape.getSize(); 
        label.setPosition({std::round(sp.x + (ss.x - tr.size.x)/2.0f), std::round(sp.y + (ss.y - tr.size.y)/2.0f - 4.0f)}); 
    }
    void setEnabled(bool e) override { enabled = e; updateStyle(); }
    bool isMouseOver(const sf::RenderWindow& win) const override { return enabled && shape.getGlobalBounds().contains(win.mapPixelToCoords(sf::Mouse::getPosition(win))); }
    bool isClicked(const sf::Event& ev, const sf::RenderWindow& win) {
        if (!enabled) return false;
        if (const auto* mb = ev.getIf<sf::Event::MouseButtonPressed>()) { if (mb->button == sf::Mouse::Button::Left && isMouseOver(win)) { if(onClick) onClick(); return true; } } return false;
    }
    bool handleEvent(const sf::Event& ev, const sf::RenderWindow& win) override { return isClicked(ev, win); }
    void setActive(bool a) { active = a; updateStyle(); }
    void setText(std::string t) { label.setString(t); centerText(); }
    void setColor(sf::Color c) { if(enabled && !active) shape.setFillColor(c); } 
    void draw(sf::RenderWindow& w) override { w.draw(shape); w.draw(label); }
};

// --- CHECKBOX (FIXED ALIGNMENT) ---
class Checkbox : public Widget {
public:
    sf::RectangleShape box, checkmark; sf::Text label; bool checked = false; std::function<void(bool)> onToggle;
    Checkbox(std::string text, const sf::Font& font, bool initial = false) : label(font, text, 24), checked(initial) { 
        label.setScale({0.5f, 0.5f});
        
        box.setSize({28.0f, 28.0f}); 
        box.setScale({0.5f, 0.5f}); // Visual 14px
        box.setOutlineThickness(2.0f);
        
        checkmark.setSize({16.0f, 16.0f}); 
        checkmark.setScale({0.5f, 0.5f}); // Visual 8px
        
        updateStyle();
    }
    void updateStyle() override { box.setFillColor(Theme::BgDark); box.setOutlineColor(sf::Color(100, 100, 100)); checkmark.setFillColor(Theme::Accent); label.setFillColor(Theme::Text); }
    float getHeight() const override { return 25.0f; }
    void setPosition(float x, float y) override { 
        x = std::round(x); y = std::round(y);
        box.setPosition({x, y + 3}); 
        
        // ALIGNMENT FIX:
        // Box Visual = 14px. Checkmark Visual = 8px.
        // Padding = (14 - 8) / 2 = 3px.
        // Box Y is y+3. So Checkmark Y is y+3+3 = y+6.
        checkmark.setPosition({x + 3, y + 6}); 
        
        label.setPosition({x + 22, y}); 
    }
    bool isMouseOver(const sf::RenderWindow& win) const override { sf::Vector2f m = win.mapPixelToCoords(sf::Mouse::getPosition(win)); sf::FloatRect ca = box.getGlobalBounds(); ca.size.x += label.getGlobalBounds().size.x + 10; return ca.contains(m); }
    bool handleEvent(const sf::Event& ev, const sf::RenderWindow& win) override { if (const auto* mb = ev.getIf<sf::Event::MouseButtonPressed>()) { if (mb->button == sf::Mouse::Button::Left && isMouseOver(win)) { checked = !checked; if(onToggle) onToggle(checked); return true; } } return false; }
    void draw(sf::RenderWindow& w) override { w.draw(box); if (checked) w.draw(checkmark); w.draw(label); }
};

// --- DROPDOWN ---
class Dropdown : public Widget {
public:
    sf::RectangleShape mainBox; sf::Text selectedText; sf::Font fontRef;
    bool isOpen = false; std::vector<std::string> options; int selectedIndex = 0; float w, h;
    std::function<void(int)> onChange; static Dropdown* currentActive;
    Dropdown(float _w, float _h, const sf::Font& font) : w(_w), h(_h), fontRef(font), selectedText(font, "", 24) { 
        selectedText.setScale({0.5f, 0.5f});
        mainBox.setSize({w, h}); mainBox.setOutlineThickness(1); updateStyle();
    }
    void updateStyle() override { mainBox.setFillColor(Theme::BgDark); mainBox.setOutlineColor(sf::Color(80, 80, 80)); selectedText.setFillColor(Theme::Text); }
    float getHeight() const override { return h + 5.0f; }
    void setPosition(float x, float y) override { mainBox.setPosition({x, y}); selectedText.setPosition({x + 5, y + 4}); }
    void setOptions(const std::vector<std::string>& opts) { options = opts; if (!options.empty()) { selectedIndex = 0; selectedText.setString(options[0]); } else { selectedText.setString("None"); } }
    void setSelection(int index) { if (index >= 0 && index < (int)options.size()) { selectedIndex = index; selectedText.setString(options[index]); } }
    bool isMouseOver(const sf::RenderWindow& win) const override { sf::Vector2f m = win.mapPixelToCoords(sf::Mouse::getPosition(win)); if (mainBox.getGlobalBounds().contains(m)) return true; if (isOpen) { float sx = mainBox.getPosition().x; float sy = mainBox.getPosition().y; sf::FloatRect listRect({sx, sy + h}, {w, (float)options.size() * h}); if (listRect.contains(m)) return true; } return false; }
    bool handleEvent(const sf::Event& ev, const sf::RenderWindow& win) override {
        if (const auto* mb = ev.getIf<sf::Event::MouseButtonPressed>()) { if (mb->button == sf::Mouse::Button::Left) { sf::Vector2f m = win.mapPixelToCoords(sf::Mouse::getPosition(win)); if (currentActive && currentActive != this) return false; if (isOpen) { float startX = mainBox.getPosition().x; float startY = mainBox.getPosition().y; for (size_t i = 0; i < options.size(); ++i) { sf::FloatRect optRect({startX, startY + (i + 1) * h}, {w, h}); if (optRect.contains(m)) { selectedIndex = i; selectedText.setString(options[i]); isOpen = false; currentActive = nullptr; if(onChange) onChange(i); return true; } } isOpen = false; currentActive = nullptr; return true; } if (mainBox.getGlobalBounds().contains(m)) { isOpen = !isOpen; if(isOpen) currentActive = this; else currentActive = nullptr; return true; } } } return false;
    }
    void draw(sf::RenderWindow& win) override {
        win.draw(mainBox); std::string display = selectedText.getString(); if (display.length() > 22) display = display.substr(0, 20) + ".."; sf::Text tempTxt = selectedText; tempTxt.setString(display); win.draw(tempTxt);
        sf::CircleShape arrow(3, 3); arrow.setOrigin({3,3}); arrow.setPosition({mainBox.getPosition().x + w - 10, mainBox.getPosition().y + h/2 + 2}); arrow.setFillColor(sf::Color(150,150,150)); win.draw(arrow);
    }
    void drawOverlay(sf::RenderWindow& win) override {
        if (isOpen) {
            float startX = mainBox.getPosition().x; float startY = mainBox.getPosition().y;
            sf::RectangleShape listBg({w, (float)options.size() * h}); listBg.setPosition({startX, startY + h}); listBg.setFillColor(sf::Color(40, 40, 42)); listBg.setOutlineColor(Theme::Accent); listBg.setOutlineThickness(1); win.draw(listBg);
            for (size_t i = 0; i < options.size(); ++i) {
                sf::Text optTxt(fontRef, options[i], 24); 
                optTxt.setScale({0.5f, 0.5f}); 
                optTxt.setPosition({startX + 5, startY + (i + 1) * h + 4});
                sf::Vector2f m = win.mapPixelToCoords(sf::Mouse::getPosition(win)); sf::FloatRect itemRect({startX, startY + (i + 1) * h}, {w, h});
                if (itemRect.contains(m)) { sf::RectangleShape hl({w, h}); hl.setPosition(itemRect.position); hl.setFillColor(Theme::Glow); win.draw(hl); }
                std::string s = options[i]; if (s.length() > 22) s = s.substr(0, 20) + ".."; optTxt.setString(s); win.draw(optTxt);
            }
        }
    }
};
inline Dropdown* Dropdown::currentActive = nullptr;

class Label : public Widget { sf::Text text; public: 
    Label(std::string c, const sf::Font& f, int s=12, sf::Color cl=sf::Color::White) : text(f, c, s * 2) { 
        text.setScale({0.5f, 0.5f}); 
        text.setFillColor(cl); 
    } 
    float getHeight() const override { return text.getGlobalBounds().size.y + 8.0f; } void setPosition(float x, float y) override { text.setPosition({x, y}); } void setText(std::string s) { text.setString(s); } void draw(sf::RenderWindow& w) override { w.draw(text); } void updateStyle() override { text.setFillColor(Theme::Text); } 
};

class RowContainer : public Widget {
public: std::vector<std::shared_ptr<Widget>> widgets; float height = 30.0f; float spacing = 5.0f;
    void add(std::shared_ptr<Widget> w) { widgets.push_back(w); if (w->getHeight() > height) height = w->getHeight(); }
    float getHeight() const override { return height + 5.0f; }
    void setPosition(float x, float y) override { float cx = x; for(auto& w : widgets) { w->setPosition(cx, y); SdrButton* b = dynamic_cast<SdrButton*>(w.get()); if(b) cx += b->shape.getSize().x + spacing; else cx += 100 + spacing; } }
    void draw(sf::RenderWindow& w) override { for(auto& o : widgets) o->draw(w); }
    void setEnabled(bool e) override { for(auto& w : widgets) w->setEnabled(e); }
    bool isMouseOver(const sf::RenderWindow& win) const override { for(auto& w : widgets) if(w->isMouseOver(win)) return true; return false; }
    bool handleEvent(const sf::Event& ev, const sf::RenderWindow& win) override { bool h = false; for(auto& o : widgets) if(o->handleEvent(ev, win)) h = true; return h; }
    void updateStyle() override { for(auto& w : widgets) w->updateStyle(); }
};

class FrequencyDisplay {
public:
    long long frequency; const sf::Font& font; sf::Text text; sf::RectangleShape hoverRect; long long hoverPower = 0; bool isHovered = false, isTopHalf = true, enabled = true; float x, y;
    FrequencyDisplay(float _x, float _y, const sf::Font& f) : font(f), text(font), x(_x), y(_y) { frequency = 100000000; text.setCharacterSize(84); text.setScale({0.5f, 0.5f}); updateStyle(); text.setOutlineThickness(0); hoverRect.setFillColor(sf::Color(255, 255, 255, 30)); setPosition(_x, _y); }
    
    // ENFORCED WHITE/GRAY - ignores Theme::Accent
    void updateStyle() {
        text.setFillColor(sf::Color::White);
    }

    void setPosition(float _x, float _y) { x = _x; y = _y; text.setPosition({x + 8, y}); }
    void setFrequency(long long f) { frequency = f; } long long getFrequency() const { return frequency; } void setEnabled(bool e) { enabled = e; }
    std::string formatWithDots(long long freq) { std::string s = std::to_string(freq); while (s.length() < 10) s = "0" + s; std::string r = ""; int c = 0; for (int i = s.length() - 1; i >= 0; i--) { r = s[i] + r; c++; if (c % 3 == 0 && i > 0) r = "." + r; } return r; }
    void update(const sf::RenderWindow& win) {
        text.setString(formatWithDots(frequency));
        if (!enabled) {
            isHovered = false;
            return;
        }
        
        sf::Vector2f m = win.mapPixelToCoords(sf::Mouse::getPosition(win));
        isHovered = false;
        hoverPower = 0;

        long long cp = 1;
        float vt = text.getGlobalBounds().position.y;
        float vh = text.getGlobalBounds().size.y;

        // Check each character individually, which resolves the issue of clipped "shared" hitboxes
        for (int i = text.getString().getSize() - 1; i >= 0; i--) {
            char c = text.getString()[i];
            if (c == '.') continue;
            
            sf::Vector2f cp_pos = text.findCharacterPos(i);
            float cw = text.findCharacterPos(i + 1).x - cp_pos.x;
            if (cw <= 0) cw = text.getCharacterSize() * 0.5f * 0.6f;
            
            // Create an exact bounding box for a single digit with a small margin
            sf::FloatRect cr({cp_pos.x, vt - 5.0f}, {cw, vh + 10.0f});
            
            if (cr.contains(m)) {
                isHovered = true;
                hoverPower = cp;
                float midY = cr.position.y + (cr.size.y / 2.0f);
                isTopHalf = (m.y < midY);
                hoverRect.setSize({cw, cr.size.y / 2.0f});
                
                if (isTopHalf) hoverRect.setPosition(cr.position);
                else hoverRect.setPosition({cr.position.x, midY});
                break;
            }
            cp *= 10;
        }
    }
    bool handleEvent(const sf::Event& ev) { if (!enabled || !isHovered || hoverPower == 0) return false; if (const auto* mb = ev.getIf<sf::Event::MouseButtonPressed>()) { if (mb->button == sf::Mouse::Button::Left) { if (isTopHalf) frequency += hoverPower; else frequency -= hoverPower; if (frequency < 0) frequency = 0; return true; } } if (const auto* sc = ev.getIf<sf::Event::MouseWheelScrolled>()) { if (sc->wheel == sf::Mouse::Wheel::Vertical) { if (sc->delta > 0) frequency += hoverPower; else frequency -= hoverPower; if (frequency < 0) frequency = 0; return true; } } return false; }
    
    void draw(sf::RenderWindow& win) { 
        sf::FloatRect b = text.getGlobalBounds(); sf::RectangleShape bg({b.size.x + 30, b.size.y + 28}); bg.setPosition({x - 5, y - 8}); bg.setFillColor(sf::Color(15, 15, 17)); 
        if (enabled && isHovered && hoverPower > 0) win.draw(hoverRect); 
        std::string str = formatWithDots(frequency); bool lz = true; sf::Text tt = text; 
        for (size_t i = 0; i < str.length(); ++i) { 
            char c = str[i]; if (c != '0' && c != '.') lz = false; if (i == str.length() - 1) lz = false; 
            if (!enabled) tt.setFillColor(sf::Color(60, 60, 60)); 
            else { 
                if (lz) tt.setFillColor(sf::Color(80, 80, 80)); 
                else tt.setFillColor(sf::Color::White); 
            } 
            sf::Vector2f p = text.findCharacterPos(i); tt.setPosition(p); tt.setString(std::string(1, c)); win.draw(tt); 
        } 
    }
    bool isMouseOver(const sf::RenderWindow& win) const { return isHovered; }
};


class Spacer : public Widget {
    float h;
public:
    Spacer(float height) : h(height) {}
    float getHeight() const override { return h; }
    void setPosition(float x, float y) override {}
    void draw(sf::RenderWindow& win) override {}
};