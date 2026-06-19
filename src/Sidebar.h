#pragma once

#include "UI.h"
#include <vector>
#include <memory>
#include <string>

// Moduł
class Module {
public:
    std::string title; bool isOpen = true; std::vector<std::shared_ptr<Widget>> widgets;
    sf::RectangleShape headerBg; sf::Text headerText; sf::Text arrowText; float width;
    
    Module(std::string t, float w, const sf::Font& font) : title(t), width(w), headerText(font, t, 11), arrowText(font, "v", 12) {
        headerBg.setSize({width, 25.0f}); 
        headerBg.setFillColor(sf::Color(65, 65, 65));
        
        headerText.setFillColor(sf::Color::White); 
        arrowText.setFillColor(sf::Color(180, 180, 180)); 
        headerText.setStyle(sf::Text::Bold);
        headerText.setLetterSpacing(2);
    }

    void updateStyle() {
        for(auto& w : widgets) w->updateStyle();
    }
    
    void addWidget(std::shared_ptr<Widget> w) { widgets.push_back(w); }
    
    float getTotalHeight() const {
        float h = 25.0f; // Wysokość samego nagłówka
        if (isOpen) {
            h += 10.0f; // Margines górny (zamiast 5.0f)
            for (const auto& w : widgets) h += w->getHeight() + 5.0f;
            h += 5.0f;  // Margines dolny
        }
        return h;
    }
    
    void layout(float x, float y) {
        headerBg.setPosition({x + 5, y}); headerText.setPosition({x + 25, y + 6}); arrowText.setPosition({x + 10, y + 5}); arrowText.setString(isOpen ? "v" : ">");
        if (isOpen) { float cy = y + 35.0f; for (auto& w : widgets) { w->setPosition(x + 15, cy); cy += w->getHeight() + 5.0f; } }
    }
    
    void draw(sf::RenderWindow& win) { win.draw(headerBg); win.draw(headerText); win.draw(arrowText); if (isOpen) for (auto& w : widgets) w->draw(win); }
    void drawOverlay(sf::RenderWindow& win) { if (isOpen) for (auto& w : widgets) w->drawOverlay(win); }

    bool isMouseOver(const sf::RenderWindow& win) const {
        sf::Vector2f m = win.mapPixelToCoords(sf::Mouse::getPosition(win));
        if (headerBg.getGlobalBounds().contains(m)) return true;
        if (isOpen) { for (const auto& w : widgets) if(w->isMouseOver(win)) return true; }
        return false;
    }

    bool handleEvent(const sf::Event& ev, const sf::RenderWindow& win) {
        if (const auto* mb = ev.getIf<sf::Event::MouseButtonPressed>()) {
            if (mb->button == sf::Mouse::Button::Left) {
                sf::Vector2f m = win.mapPixelToCoords(sf::Mouse::getPosition(win));
                if (headerBg.getGlobalBounds().contains(m)) { isOpen = !isOpen; return true; }
            }
        }
        if (isOpen) { for (auto it = widgets.rbegin(); it != widgets.rend(); ++it) { if ((*it)->handleEvent(ev, win)) return true; } } return false;
    }
    void update(const sf::RenderWindow& win) { if (isOpen) for (auto& w : widgets) w->update(win); }
};

// Sidebar
class Sidebar {
public:
    float x, y, width, height; float totalContentHeight = 0; float scrollOffset = 0;
    std::vector<std::shared_ptr<Module>> modules; const sf::Font& font; sf::RectangleShape sbTrack, sbThumb;
    
    Sidebar(float w, const sf::Font& f) : width(w), font(f) { 
        sbTrack.setFillColor(sf::Color(19, 19, 21)); 
        sbThumb.setFillColor(sf::Color(80, 80, 80)); 
    }
    
    std::shared_ptr<Module> addModule(std::string title) { auto m = std::make_shared<Module>(title, width - 20.0f, font); modules.push_back(m); return m; }
    
    void setGeometry(float _x, float _y, float _h) { x = _x; y = _y; height = _h; sbTrack.setPosition({x + width - 12, y}); sbTrack.setSize({12, height}); recalculateLayout(); }
    void updateStyle() {
        for(auto& m : modules) m->updateStyle();
    }
    void recalculateLayout() {
        totalContentHeight = 0;
        float cy = y - scrollOffset;
        for (auto& m : modules) {
            m->layout(x, cy);
            float h = m->getTotalHeight();
            cy += h + 5.0f;
            totalContentHeight += h + 5.0f;
        }
        
        // Dodajemy stały bufor 60px! Dzięki temu zescrollujesz nieco niżej niż krawędź ostatniego widgetu.
        totalContentHeight += 60.0f;

        if (totalContentHeight > height) {
            float th = (height / totalContentHeight) * height;
            if (th < 30) th = 30;
            sbThumb.setSize({10, th});
            float ty = y + (scrollOffset / (totalContentHeight - height)) * (height - th);
            sbThumb.setPosition({x + width - 11, ty});
        } else {
            scrollOffset = 0;
        }
    }
    
    // ZMIANA: Sprawdza czy myszka jest nad CAŁYM panelem sidebara, nie tylko nad widgetami
    bool isMouseOver(const sf::RenderWindow& win) const {
        sf::Vector2f m = win.mapPixelToCoords(sf::Mouse::getPosition(win));
        return (m.x >= x && m.x <= x + width && m.y >= y && m.y <= y + height);
    }

    bool isAnyWidgetHovered(const sf::RenderWindow& win) const {
        if (!isMouseOver(win)) return false;
        for (const auto& m : modules) if(m->isMouseOver(win)) return true;
        return false;
    }

    // ZMIANA: Zwraca bool (true = zdarzenie obsłużone/przechwycone)
    bool handleEvent(const sf::Event& ev, const sf::RenderWindow& win) {
        // 1. Priorytet: Dropdowny (zawsze na wierzchu)
        if (Dropdown::currentActive) { 
            for (auto& mod : modules) { 
                if(mod->handleEvent(ev, win)) return true; 
            } 
            // Jeśli kliknięto poza dropdownem, to też konsumujemy kliknięcie żeby go zamknąć
            return true; 
        }

        bool captured = false;

        // 2. Scrollowanie Sidebara
        if (const auto* sc = ev.getIf<sf::Event::MouseWheelScrolled>()) {
            sf::Vector2f m = win.mapPixelToCoords(sf::Vector2i((int)sc->position.x, (int)sc->position.y));
            if (m.x > x && m.x < x + width) {
                scrollOffset -= sc->delta * 30.0f; if (scrollOffset < 0) scrollOffset = 0; float maxS = std::max(0.0f, totalContentHeight - height); if (scrollOffset > maxS) scrollOffset = maxS; recalculateLayout();
                return true; // Scroll nad sidebarem przechwycony
            }
        }

        // 3. Kliknięcia i widgety
        sf::Vector2f m = win.mapPixelToCoords(sf::Mouse::getPosition(win));
        // Jeśli mysz jest fizycznie nad sidebarem, uznajemy to za "przechwycenie" (captured = true)
        // Zapobiega to klikaniu w spektrum/aprs "przez" sidebar.
        if (m.x > x && m.x < x + width && m.y > y && m.y < y + height) {
            captured = true; 
            for (auto& mod : modules) { 
                if(mod->handleEvent(ev, win)) { 
                    recalculateLayout(); 
                    return true; 
                } 
            }
        }
        
        return captured;
    }

    void update(const sf::RenderWindow& win) { for(auto& m : modules) m->update(win); }
    
    void draw(sf::RenderWindow& win) {
        sf::RectangleShape bg({width, height}); 
        bg.setPosition({x, y}); 
        bg.setFillColor(sf::Color(19, 19, 21)); 
        win.draw(bg);
        
        sf::RectangleShape leftBorder({1.0f, height}); 
        leftBorder.setPosition({x, y});
        leftBorder.setFillColor(sf::Color(80, 80, 80)); 
        win.draw(leftBorder);

        for (auto& m : modules) { float mt = m->headerBg.getPosition().y; float mb = mt + m->getTotalHeight(); if (mb > y && mt < y + height) m->draw(win); }
        if (totalContentHeight > height) { win.draw(sbTrack); win.draw(sbThumb); }
        for (auto& m : modules) { float mt = m->headerBg.getPosition().y; float mb = mt + m->getTotalHeight(); if (mb > y && mt < y + height) m->drawOverlay(win); }
    }
};