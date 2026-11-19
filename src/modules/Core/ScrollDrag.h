#pragma once

#include <SFML/Graphics.hpp>
#include <algorithm>

// =====================================================================
//  ScrollDrag - a vertical scroll bar whose thumb can be grabbed.
//
//  Every frame the owner lays it out (where the bar is, how tall the
//  thumb is, the scroll value and its maximum) and draws it; its
//  handleEvent() then lets the mouse
//    - grab the thumb and drag it (the content follows),
//    - click the bar above / below the thumb: the thumb jumps there
//      (centred on the mouse) and can be dragged on from that point.
//  The value is in the owner's units (rows, pixels). The grab area is
//  wider than the drawn thumb (a few pixels), so it is easy to hit.
//
//  hide() when the content fits (no bar): the column takes no clicks.
// =====================================================================

class ScrollDrag {
    sf::FloatRect track;           // the thumb's column: x, top, width (drawn), height
    float thumbH = 0.0f, value = 0.0f, maxValue = 0.0f;
    bool shown = false, drag = false, hover = false;
    float grab = 0.0f;             // mouse y - thumb top, while dragging
    float hitW = 14.0f;            // grab width, centred on the drawn thumb

    float thumbTop() const {
        const float f = maxValue > 0.0f ? std::clamp(value / maxValue, 0.0f, 1.0f) : 0.0f;
        return track.position.y + (track.size.y - thumbH) * f;
    }
    sf::FloatRect hitRect() const {
        const float w = std::max(hitW, track.size.x);
        const float cx = track.position.x + track.size.x / 2.0f;
        return sf::FloatRect({cx - w / 2.0f, track.position.y}, {w, track.size.y});
    }
    float valueAt(float top) const {
        const float range = track.size.y - thumbH;
        if (range <= 0.0f) return 0.0f;
        return std::clamp((top - track.position.y) / range, 0.0f, 1.0f) * maxValue;
    }

public:
    // trackRect: the bar (the thumb is drawn at its x with its width); thumbHeight in the
    // same units; v / maxV: the scroll position and its maximum (> 0 when there is a bar)
    void layout(sf::FloatRect trackRect, float thumbHeight, float v, float maxV) {
        track = trackRect;
        thumbH = std::min(thumbHeight, trackRect.size.y);
        value = v;
        maxValue = maxV;
        shown = maxV > 0.0f && trackRect.size.y > 0.0f;
        if (!shown) drag = false;
    }
    void hide() { shown = false; drag = false; hover = false; }
    void setHitWidth(float w) { hitW = w; }   // narrower where the bar sits next to buttons
    bool dragging() const { return drag; }

    void draw(sf::RenderTarget& win, sf::Color normal = sf::Color(200, 200, 200, 140)) const {
        if (!shown) return;
        sf::RectangleShape t({track.size.x, thumbH});
        t.setPosition({track.position.x, thumbTop()});
        t.setFillColor(drag ? sf::Color(235, 235, 240, 230) : hover ? sf::Color(220, 220, 225, 200) : normal);
        win.draw(t);
    }

    // true when the event was the bar's (value then holds the new position)
    template <typename V>
    bool handleEvent(const sf::Event& ev, const sf::RenderWindow& win, V& v) {
        if (const auto* mp = ev.getIf<sf::Event::MouseButtonPressed>()) {
            if (!shown || mp->button != sf::Mouse::Button::Left) return false;
            const sf::Vector2f m = win.mapPixelToCoords(mp->position);
            if (!hitRect().contains(m)) return false;
            const float top = thumbTop();
            if (m.y >= top && m.y <= top + thumbH) {
                grab = m.y - top;                 // on the thumb: drag from where it was taken
            } else {
                grab = thumbH / 2.0f;             // on the bar: jump there, then drag
                value = valueAt(m.y - grab);
                v = (V)value;
            }
            drag = true;
            return true;
        }
        if (const auto* mm = ev.getIf<sf::Event::MouseMoved>()) {
            const sf::Vector2f m = win.mapPixelToCoords(mm->position);
            hover = shown && hitRect().contains(m);
            if (!drag) return false;
            if (!sf::Mouse::isButtonPressed(sf::Mouse::Button::Left)) { drag = false; return false; }   // release missed
            value = valueAt(m.y - grab);
            v = (V)value;
            return true;
        }
        if (const auto* mr = ev.getIf<sf::Event::MouseButtonReleased>()) {
            if (drag && mr->button == sf::Mouse::Button::Left) { drag = false; return true; }
        }
        return false;
    }
};
