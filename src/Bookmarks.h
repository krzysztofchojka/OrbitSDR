#pragma once

// =====================================================================
//  Frequency bookmarks and the scanner.
//
//  Bookmarks: name, frequency, demodulator mode + bandwidth, a "scan"
//  flag and a bank (memory group, e.g. "2 m repeaters", "Airband"),
//  kept in bookmarks.json in the settings folder (sorted by frequency).
//  The list and the scanner work on the chosen bank (or on all of them). The sidebar list: [scan box] name  MHz  mode  [x];
//  clicking a row tunes to it.
//
//  Scanner: steps through the bookmarks marked for scanning. After each
//  retune it waits for the radio to settle, then looks at the signal in
//  the tuner's band (SNR from the full FFT, see DSPWorker). Above the
//  threshold it stops and stays while the signal is there, plus the hang
//  time after it goes; then it moves on. The speaker is muted while it
//  searches. Tuning by hand stops it.
// =====================================================================

#include <SFML/Graphics.hpp>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <functional>
#include <algorithm>
#include <cstdio>
#include "UI.h"
#include "Demodulator.h"
#include "orbit/Json.h"

struct Bookmark {
    std::string name;
    long long freqHz = 0;
    Mode mode = Mode::NFM;
    float bandwidthHz = 12500.0f;
    bool scan = true;
    std::string bank = "General";
};

inline const std::string& defaultBank() { static const std::string b = "General"; return b; }

inline const char* modeShortName(Mode m) {
    switch (m) {
    case Mode::AM: return "AM";
    case Mode::NFM: return "NFM";
    case Mode::WFM: return "WFM";
    case Mode::LSB: return "LSB";
    case Mode::USB: return "USB";
    case Mode::CW: return "CW";
    case Mode::RAW: return "RAW";
    default: return "OFF";
    }
}

class BookmarkStore {
    std::string path;

    static std::string jsonEscape(const std::string& s) {
        std::string o;
        for (char c : s) {
            if (c == '"' || c == '\\') { o += '\\'; o += c; }
            else if ((unsigned char)c < 32) o += ' ';
            else o += c;
        }
        return o;
    }

public:
    std::vector<Bookmark> items;
    std::vector<std::string> banks;   // in the order they were created; "General" always exists

    void ensureBank(const std::string& b) {
        if (b.empty()) return;
        if (std::find(banks.begin(), banks.end(), b) == banks.end()) banks.push_back(b);
    }

    // Removes a bank; its bookmarks move to "General"
    void removeBank(const std::string& b) {
        if (b == defaultBank()) return;
        banks.erase(std::remove(banks.begin(), banks.end(), b), banks.end());
        for (auto& x : items) if (x.bank == b) x.bank = defaultBank();
        save();
    }

    void load(const std::string& p) {
        path = p;
        items.clear();
        banks.clear();
        ensureBank(defaultBank());
        std::ifstream f(path);
        if (!f.is_open()) return;
        std::stringstream ss;
        ss << f.rdbuf();
        orbit::JsonValue root;
        if (!orbit::parseJson(ss.str(), root)) return;
        if (const orbit::JsonValue* bl = root.get("banks"); bl && bl->type == orbit::JsonValue::Array)
            for (const auto& v : bl->arr) if (v.type == orbit::JsonValue::String) ensureBank(v.str);
        const orbit::JsonValue* arr = root.type == orbit::JsonValue::Array ? &root : root.get("bookmarks");
        if (!arr || arr->type != orbit::JsonValue::Array) return;
        for (const auto& v : arr->arr) {
            Bookmark b;
            b.name = v.getString("name");
            b.freqHz = (long long)v.getNumber("freq", 0);
            const int m = (int)v.getNumber("mode", (int)Mode::NFM);
            b.mode = (m >= 0 && m <= (int)Mode::CW) ? (Mode)m : Mode::NFM;
            b.bandwidthHz = (float)v.getNumber("bw", 12500);
            b.scan = v.getBool("scan", true);
            b.bank = v.getString("bank", defaultBank());
            if (b.bank.empty()) b.bank = defaultBank();
            ensureBank(b.bank);
            if (b.freqHz > 0) items.push_back(b);
        }
        sort();
    }

    void save() const {
        if (path.empty()) return;
        std::ofstream f(path, std::ios::trunc);
        if (!f.is_open()) return;
        f << "{\n  \"banks\": [";
        for (size_t i = 0; i < banks.size(); i++) f << (i ? ", " : "") << "\"" << jsonEscape(banks[i]) << "\"";
        f << "],\n  \"bookmarks\": [\n";
        for (size_t i = 0; i < items.size(); i++) {
            const Bookmark& b = items[i];
            f << "    {\"name\": \"" << jsonEscape(b.name) << "\", \"freq\": " << b.freqHz << ", \"mode\": " << (int)b.mode
              << ", \"bw\": " << (long long)b.bandwidthHz << ", \"scan\": " << (b.scan ? "true" : "false")
              << ", \"bank\": \"" << jsonEscape(b.bank) << "\"}"
              << (i + 1 < items.size() ? "," : "") << "\n";
        }
        f << "  ]\n}\n";
    }

    void sort() {
        std::stable_sort(items.begin(), items.end(), [](const Bookmark& a, const Bookmark& b) { return a.freqHz < b.freqHz; });
    }

    // Returns the index of the new (or replaced) bookmark
    int add(const Bookmark& b) {
        ensureBank(b.bank);
        for (size_t i = 0; i < items.size(); i++)
            if (items[i].freqHz == b.freqHz && items[i].bank == b.bank) { items[i] = b; save(); return (int)i; }
        items.push_back(b);
        sort();
        save();
        for (size_t i = 0; i < items.size(); i++) if (items[i].freqHz == b.freqHz && items[i].bank == b.bank) return (int)i;
        return -1;
    }
};

// ---------------------------------------------------------------------
//  The list in the sidebar
// ---------------------------------------------------------------------
class BookmarkList : public Widget {
    const sf::Font& font;
    float x = 0, y = 0, w;
    float rowH = 20.0f;
    int maxRows = 10;
    int scroll = 0;
    int hoverRow = -1;

    // store indices of the rows shown (the chosen bank)
    std::vector<int> rowsIdx() const {
        std::vector<int> v;
        if (!store) return v;
        for (int i = 0; i < (int)store->items.size(); i++)
            if (bankFilter.empty() || store->items[i].bank == bankFilter) v.push_back(i);
        return v;
    }

public:
    BookmarkStore* store = nullptr;
    std::string bankFilter;   // "" = all banks
    int selected = -1;    // clicked last (Save updates it)
    int active = -1;      // the scanner is on it
    std::function<void(int)> onTune;
    std::function<void()> onChanged;   // scan flag / delete: saved, the sidebar re-lays out

    BookmarkList(float width, const sf::Font& f) : font(f), w(width) {}

    int count() const { return (int)rowsIdx().size(); }
    bool shows(int storeIdx) const {
        return store && storeIdx >= 0 && storeIdx < (int)store->items.size() &&
               (bankFilter.empty() || store->items[storeIdx].bank == bankFilter);
    }
    int visibleRows() const { return std::max(1, std::min(maxRows, count())); }
    float getHeight() const override { return count() == 0 ? 22.0f : visibleRows() * rowH + 4.0f; }
    void setPosition(float px, float py) override { x = px; y = py; }

    bool isMouseOver(const sf::RenderWindow& win) const override {
        const sf::Vector2f m = win.mapPixelToCoords(sf::Mouse::getPosition(win));
        return m.x >= x && m.x <= x + w && m.y >= y && m.y <= y + getHeight();
    }

    void ensureVisible(int storeIdx) {
        const auto v = rowsIdx();
        const int idx = (int)(std::find(v.begin(), v.end(), storeIdx) - v.begin());
        if (storeIdx < 0 || idx >= (int)v.size()) return;
        if (idx < scroll) scroll = idx;
        if (idx >= scroll + maxRows) scroll = idx - maxRows + 1;
    }

    void update(const sf::RenderWindow& win) override {
        hoverRow = -1;
        if (!isMouseOver(win) || count() == 0) return;
        const sf::Vector2f m = win.mapPixelToCoords(sf::Mouse::getPosition(win));
        const int r = (int)((m.y - y) / rowH);
        const auto v = rowsIdx();
        if (r >= 0 && r < visibleRows() && scroll + r < (int)v.size()) hoverRow = v[scroll + r];
    }

    bool handleEvent(const sf::Event& ev, const sf::RenderWindow& win) override {
        if (!store || !isMouseOver(win)) return false;
        const sf::Vector2f m = win.mapPixelToCoords(sf::Mouse::getPosition(win));
        if (const auto* sc = ev.getIf<sf::Event::MouseWheelScrolled>()) {
            if (count() <= maxRows) return false;   // let the sidebar scroll
            scroll = std::clamp(scroll - (int)std::round(sc->delta), 0, count() - maxRows);
            return true;
        }
        if (const auto* mb = ev.getIf<sf::Event::MouseButtonPressed>()) {
            if (mb->button != sf::Mouse::Button::Left) return false;
            const int r = (int)((m.y - y) / rowH);
            const auto v = rowsIdx();
            if (r < 0 || scroll + r >= (int)v.size()) return true;
            const int idx = v[scroll + r];
            const float lx = m.x - x;
            if (lx < 18.0f) {                       // scan box
                store->items[idx].scan = !store->items[idx].scan;
                store->save();
                if (onChanged) onChanged();
            } else if (lx > w - 18.0f) {            // delete
                store->items.erase(store->items.begin() + idx);
                store->save();
                if (selected == idx) selected = -1; else if (selected > idx) selected--;
                if (active == idx) active = -1; else if (active > idx) active--;
                scroll = std::clamp(scroll, 0, std::max(0, count() - maxRows));
                if (onChanged) onChanged();
            } else {
                selected = idx;
                if (onTune) onTune(idx);
            }
            return true;
        }
        return false;
    }

    void draw(sf::RenderWindow& win) override {
        auto txt = [&](const std::string& s, float px, float py, sf::Color c) {
            sf::Text t(font, u8(s), 22);
            t.setScale({0.5f, 0.5f});
            t.setPosition({px, py});
            t.setFillColor(c);
            win.draw(t);
        };
        if (count() == 0) {
            txt(store && !store->items.empty() ? "This bank is empty: tune, type a name, press Add."
                                               : "No bookmarks yet: tune, type a name, press Add.", x, y + 3.0f, sf::Color(130, 130, 140));
            return;
        }
        const int rows = visibleRows();
        const auto v = rowsIdx();
        for (int r = 0; r < rows && scroll + r < (int)v.size(); r++) {
            const int idx = v[scroll + r];
            const Bookmark& b = store->items[idx];
            const float ry = y + r * rowH;
            sf::RectangleShape bg({w, rowH - 2.0f});
            bg.setPosition({x, ry});
            bg.setFillColor(idx == active ? Theme::AccentDim : idx == hoverRow ? sf::Color(50, 50, 58) : idx == selected ? sf::Color(40, 40, 48) : sf::Color(28, 28, 32));
            win.draw(bg);
            // scan box
            sf::RectangleShape box({11.0f, 11.0f});
            box.setPosition({x + 3.0f, ry + 3.5f});
            box.setFillColor(Theme::BgDark);
            box.setOutlineColor(sf::Color(110, 110, 110));
            box.setOutlineThickness(1.0f);
            win.draw(box);
            if (b.scan) {
                sf::RectangleShape mark({7.0f, 7.0f});
                mark.setPosition({x + 5.0f, ry + 5.5f});
                mark.setFillColor(Theme::Accent);
                win.draw(mark);
            }
            char f[32];
            std::snprintf(f, sizeof(f), "%.4f", b.freqHz / 1e6);
            std::string name = b.name.empty() ? std::string(f) : b.name;
            if (name.size() > 15) name = name.substr(0, 14) + ".";
            txt(name, x + 20.0f, ry + 2.0f, sf::Color(230, 230, 235));
            txt(f, x + w - 128.0f, ry + 2.0f, sf::Color(190, 200, 210));
            txt(modeShortName(b.mode), x + w - 52.0f, ry + 2.0f, sf::Color(150, 150, 160));
            txt("x", x + w - 12.0f, ry + 1.0f, idx == hoverRow ? sf::Color(255, 120, 120) : sf::Color(120, 120, 125));
        }
        if (count() > maxRows) {   // scroll bar
            const float h = rows * rowH;
            const float th = std::max(12.0f, h * maxRows / (float)count());
            const float ty = y + (h - th) * scroll / (float)(count() - maxRows);
            sf::RectangleShape t({3.0f, th});
            t.setPosition({x + w + 2.0f, ty});
            t.setFillColor(sf::Color(150, 150, 150, 160));
            win.draw(t);
        }
    }
};

// ---------------------------------------------------------------------
//  Scanner state machine (UI thread, called every frame)
// ---------------------------------------------------------------------
class Scanner {
public:
    enum class State { Off, Settle, Listen, Hold, Hang };
    State state = State::Off;
    int current = -1;          // bookmark index
    float thresholdDb = 12.0f; // SNR that stops the scan
    float hangSec = 3.0f;      // stay this long after the signal is gone
    float settleSec = 0.25f;   // after a retune: wait for fresh samples
    float listenSec = 0.15f;   // then look this long for a signal
    long long tunedHz = 0;     // where the scanner put the tuner (manual tuning stops it)

    std::function<void(int)> tune;       // tunes to a bookmark
    std::function<bool(int)> eligible;   // in the chosen bank (empty: all)

    bool running() const { return state != State::Off; }

    void start(const std::vector<Bookmark>& b, int from, double now) {
        clock = now;
        if (!anyMarked(b)) { state = State::Off; return; }
        current = from;
        next(b);
    }
    void stop() { state = State::Off; }

    void skip(const std::vector<Bookmark>& b, double now) { clock = now; if (running()) next(b); }

    // snrDb: the tuner band's SNR; fresh: a new value since the last call
    void update(const std::vector<Bookmark>& b, double now, float snrDb, bool fresh) {
        clock = now;
        if (state == State::Off) return;
        if (!anyMarked(b)) { state = State::Off; return; }
        if (current < 0 || current >= (int)b.size()) { next(b); return; }
        switch (state) {
        case State::Settle:
            if (now - t0 >= settleSec) { state = State::Listen; t0 = now; }
            break;
        case State::Listen:
            if (fresh && snrDb >= thresholdDb) { state = State::Hold; lastSignal = now; }
            else if (now - t0 >= listenSec) next(b);
            break;
        case State::Hold:
            if (fresh && snrDb >= thresholdDb - 3.0f) lastSignal = now;   // a little hysteresis
            if (now - lastSignal > 0.4) { state = State::Hang; }
            break;
        case State::Hang:
            if (fresh && snrDb >= thresholdDb) { state = State::Hold; lastSignal = now; }
            else if (now - lastSignal >= hangSec + 0.4) next(b);
            break;
        default: break;
        }
    }

    // The speaker is on only while a signal is held
    bool muteWanted() const { return state == State::Settle || state == State::Listen; }

    std::string status(const std::vector<Bookmark>& b) const {
        if (state == State::Off) return "Scanner: off";
        std::string n = current >= 0 && current < (int)b.size()
                            ? (b[current].name.empty() ? std::to_string(b[current].freqHz) : b[current].name) : "-";
        switch (state) {
        case State::Hold: return "Signal: " + n;
        case State::Hang: return "Signal gone, waiting: " + n;
        default: return "Scanning... " + n;
        }
    }

private:
    double t0 = 0.0, lastSignal = 0.0, clock = 0.0;

    bool ok(const std::vector<Bookmark>& b, int i) const { return b[i].scan && (!eligible || eligible(i)); }
    bool anyMarked(const std::vector<Bookmark>& b) const {
        for (int i = 0; i < (int)b.size(); i++) if (ok(b, i)) return true;
        return false;
    }

    void next(const std::vector<Bookmark>& b) {
        const int n = (int)b.size();
        for (int k = 1; k <= n; k++) {
            const int i = ((current < 0 ? -1 : current) + k) % n;
            if (ok(b, i)) {
                current = i;
                state = State::Settle;
                t0 = clock;
                tunedHz = b[i].freqHz;
                if (tune) tune(i);
                return;
            }
        }
        state = State::Off;
    }
};
