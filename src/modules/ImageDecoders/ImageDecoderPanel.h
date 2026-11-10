#pragma once

#include <SFML/Graphics.hpp>
#include <mutex>
#include <memory>
#include <string>
#include <vector>
#include <ctime>
#include <functional>
#include <algorithm>
#include <filesystem>
#include "../Core/SDRModule.h"
#include "../../NativeDialogs.h"
#include "ToneDemod.h"

// =====================================================================
//  Shared UI for the image decoders (SSTV, WEFAX): a large panel at the
//  bottom of the waterfall with the picture on the left (scrollable) and
//  the decoder settings on the right, plus PNG saving.
//
//  Threading: processAudio() runs on the DSP thread, draw()/handleEvent()
//  on the UI thread. Everything shared is guarded by 'mtx'. Widget
//  callbacks lock 'mtx' themselves, so draw() never holds it while
//  updating widgets.
// =====================================================================

namespace imgdec {

// "Label   <  value  >" option selector. Works inside a panel at the bottom
// of the window, where a drop-down list would open off screen.
class Selector : public Widget {
    sf::Text label, value;
    sf::RectangleShape leftBtn, rightBtn;
    sf::Text leftTxt, rightTxt;
    float width;
    bool enabled = true;
public:
    std::vector<std::string> options;
    int index = 0;
    std::function<void(int)> onChange;

    Selector(const std::string& lbl, float w, const sf::Font& f, std::vector<std::string> opts)
        : label(f, lbl, 24), value(f, "", 24), leftTxt(f, "<", 24), rightTxt(f, ">", 24), width(w), options(std::move(opts)) {
        for (sf::Text* t : {&label, &value, &leftTxt, &rightTxt}) t->setScale({0.5f, 0.5f});
        for (sf::RectangleShape* r : {&leftBtn, &rightBtn}) { r->setSize({22.0f, 22.0f}); r->setOutlineThickness(1.0f); }
        updateStyle();
    }
    void setIndex(int i) { if (i >= 0 && i < (int)options.size()) index = i; }
    float getHeight() const override { return 27.0f; }
    void setPosition(float x, float y) override {
        x = std::round(x); y = std::round(y);
        label.setPosition({x, y + 3});
        float bx = x + 78.0f;
        leftBtn.setPosition({bx, y});
        rightBtn.setPosition({x + width - 22.0f, y});
        leftTxt.setPosition({bx + 7, y + 2});
        rightTxt.setPosition({x + width - 15, y + 2});
        value.setString(options.empty() ? "" : options[index]);
        float mid = (bx + 22.0f + x + width - 22.0f) / 2.0f;
        value.setPosition({std::round(mid - value.getGlobalBounds().size.x / 2.0f), y + 3});
    }
    void setEnabled(bool e) override { enabled = e; updateStyle(); }
    void updateStyle() override {
        label.setFillColor(Theme::Text);
        value.setFillColor(enabled ? sf::Color::White : sf::Color(110, 110, 110));
        for (sf::RectangleShape* r : {&leftBtn, &rightBtn}) { r->setFillColor(Theme::BgDark); r->setOutlineColor(sf::Color(80, 80, 80)); }
    }
    bool isMouseOver(const sf::RenderWindow& win) const override {
        sf::Vector2f m = win.mapPixelToCoords(sf::Mouse::getPosition(win));
        return leftBtn.getGlobalBounds().contains(m) || rightBtn.getGlobalBounds().contains(m);
    }
    bool handleEvent(const sf::Event& ev, const sf::RenderWindow& win) override {
        if (!enabled || options.empty()) return false;
        const auto* mb = ev.getIf<sf::Event::MouseButtonPressed>();
        if (!mb || mb->button != sf::Mouse::Button::Left) return false;
        sf::Vector2f m = win.mapPixelToCoords(sf::Mouse::getPosition(win));
        int n = (int)options.size();
        if (leftBtn.getGlobalBounds().contains(m)) index = (index + n - 1) % n;
        else if (rightBtn.getGlobalBounds().contains(m)) index = (index + 1) % n;
        else return false;
        if (onChange) onChange(index);
        return true;
    }
    void draw(sf::RenderWindow& w) override {
        w.draw(leftBtn); w.draw(rightBtn); w.draw(leftTxt); w.draw(rightTxt); w.draw(label); w.draw(value);
    }
};

// Buttons side by side
class ButtonRow : public Widget {
public:
    std::vector<std::shared_ptr<SdrButton>> buttons;
    float gap = 8.0f;
    explicit ButtonRow(std::vector<std::shared_ptr<SdrButton>> b) : buttons(std::move(b)) {}
    float getHeight() const override { float h = 0; for (auto& b : buttons) h = std::max(h, b->getHeight()); return h; }
    void setPosition(float x, float y) override { for (auto& b : buttons) { b->setPosition(x, y); x += b->shape.getSize().x + gap; } }
    void draw(sf::RenderWindow& w) override { for (auto& b : buttons) b->draw(w); }
    bool handleEvent(const sf::Event& ev, const sf::RenderWindow& win) override { for (auto& b : buttons) if (b->handleEvent(ev, win)) return true; return false; }
    bool isMouseOver(const sf::RenderWindow& win) const override { for (auto& b : buttons) if (b->isMouseOver(win)) return true; return false; }
    void updateStyle() override { for (auto& b : buttons) b->updateStyle(); }
};

class ImageDecoderPanel : public SDRModule {
protected:
    static constexpr float SETTINGS_W = 270.0f;
    static constexpr int HIST_BINS = 40;           // tone histogram 1000..2600 Hz
    static constexpr double HIST_LO = 1000.0, HIST_HI = 2600.0;

    const sf::Font* font = nullptr;
    std::mutex mtx;
    ToneDemod tone;
    std::vector<float> trackScratch;
    float histogram[HIST_BINS] = {};

    std::vector<std::shared_ptr<Widget>> widgets;   // settings column, top to bottom

    // Texture of the picture (UI thread only)
    sf::Texture tex;
    unsigned texW = 0, texH = 0;
    uint64_t shownVersion = (uint64_t)-1;
    std::vector<uint8_t> rgbaScratch;

    sf::FloatRect panelRect, imageRect;
    float scrollY = 0.0f;
    bool followNewest = true;
    std::string message;                            // last user facing message (e.g. "Saved ...")

    struct PendingSave { std::vector<uint8_t> rgba; unsigned w = 0, h = 0; std::string tag; };
    std::unique_ptr<PendingSave> pendingSave;       // queued by the auto-save (DSP thread)

    // --- implemented by the decoders (called with mtx held) ---
    virtual void processTrack(const std::vector<float>& track) = 0;
    virtual std::string titleText() const = 0;
    virtual std::string statusText() = 0;
    virtual unsigned imageWidth() const = 0;
    virtual unsigned imageRows() const = 0;         // rows that exist (WEFAX grows, SSTV is fixed)
    virtual uint64_t imageVersion() const = 0;
    // Converts rows [y0, y1) to RGBA into 'out' (size (y1-y0) * width * 4)
    virtual void rowsToRgba(unsigned y0, unsigned y1, std::vector<uint8_t>& out) const = 0;
    // First row that changed since the last call (default: everything)
    virtual unsigned takeDirtyRow() { return 0; }
    virtual bool fitWholeImage() const = 0;         // SSTV: fit, WEFAX: fit width and scroll
    virtual std::string fileTag() const = 0;
    virtual void buildWidgets() = 0;

    // Copies the current picture for saving (mtx held)
    std::unique_ptr<PendingSave> snapshot() const {
        auto s = std::make_unique<PendingSave>();
        s->w = imageWidth(); s->h = imageRows();
        if (s->w == 0 || s->h == 0) return nullptr;
        rowsToRgba(0, s->h, s->rgba);
        s->tag = fileTag();
        return s;
    }

    // To be called from the decoder's "image complete" callback (DSP thread, mtx held)
    void queueAutoSave() {
        if (!autoSave) return;
        pendingSave = snapshot();
    }

    std::string pictureFolder() const {
        std::string dir = saveDir ? saveDir() : std::string();
        if (dir.empty()) dir = ".";
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(dir), ec);
        return dir;
    }

    static std::string defaultFileName(const PendingSave& s) {
        char ts[32];
        std::time_t now = std::time(nullptr);
        std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", std::localtime(&now));
        return s.tag + "_" + ts + ".png";
    }

    void writeImage(const PendingSave& s, const std::string& path) {
        sf::Image img(sf::Vector2u{s.w, s.h}, s.rgba.data());
        const size_t cut = path.find_last_of("/\\");
        const std::string name = (cut == std::string::npos) ? path : path.substr(cut + 1);
        if (img.saveToFile(std::filesystem::u8path(path))) message = "Saved: " + name;
        else message = "Could not save " + name;
    }

    // Auto save: straight into the default folder
    void writeImage(const PendingSave& s) { writeImage(s, pictureFolder() + "/" + defaultFileName(s)); }

    // "Save..." button: the system file dialog, starting in the default folder
    void saveNow() {
        std::unique_ptr<PendingSave> s;
        { std::lock_guard<std::mutex> l(mtx); s = snapshot(); }
        if (!s) { message = "Nothing to save yet"; return; }
        std::string path = savePictureDialog(pictureFolder(), defaultFileName(*s));
        if (!path.empty()) writeImage(*s, path);
    }

    // "Folder..." button: changes the default folder
    void chooseFolder() {
        std::string dir = selectFolderDialog("Select the default folder for pictures");
        if (dir.empty()) return;
        if (setSaveDir) setSaveDir(dir);
        message.clear();
    }

    // Save / Folder / Clear buttons shared by the image decoders
    std::shared_ptr<ButtonRow> makeFileButtons(std::function<void()> onClear) {
        const float bw = (SETTINGS_W - 24.0f - 16.0f) / 3.0f;
        auto save = std::make_shared<SdrButton>(bw, 24.0f, "Save...", *font);
        auto folder = std::make_shared<SdrButton>(bw, 24.0f, "Folder...", *font);
        auto clear = std::make_shared<SdrButton>(bw, 24.0f, "Clear", *font);
        save->onClick = [this]() { saveNow(); };
        folder->onClick = [this]() { chooseFolder(); };
        clear->onClick = std::move(onClear);
        return std::make_shared<ButtonRow>(std::vector<std::shared_ptr<SdrButton>>{save, folder, clear});
    }

    void updateHistogram(const std::vector<float>& track) {
        for (auto& h : histogram) h *= 0.85f;
        for (float f : track) {
            int b = (int)((f - HIST_LO) / (HIST_HI - HIST_LO) * HIST_BINS);
            if (b >= 0 && b < HIST_BINS) histogram[b] += 1.0f;
        }
    }

    sf::Text text(const std::string& s, unsigned size, sf::Color c) const {
        sf::Text t(*font, sf::String::fromUtf8(s.begin(), s.end()), size);
        t.setScale({0.5f, 0.5f});
        t.setFillColor(c);
        return t;
    }

    // UI thread, mtx held: bring the texture up to date
    void syncTexture() {
        const unsigned w = imageWidth(), rows = imageRows();
        if (w == 0 || rows == 0) { texW = 0; texH = 0; shownVersion = imageVersion(); return; }
        if (imageVersion() == shownVersion && w == texW) return;
        unsigned firstRow = takeDirtyRow();
        const unsigned maxSize = sf::Texture::getMaximumSize();
        if (w != texW || rows > texH) {
            unsigned capacity = fitWholeImage() ? rows : std::min(maxSize, ((rows + 511) / 512) * 512);
            if (!tex.resize({w, std::max(1u, capacity)})) return;
            tex.setSmooth(true);
            texW = w; texH = capacity;
            firstRow = 0;
        }
        const unsigned last = std::min(rows, texH);
        if (firstRow < last) {
            rowsToRgba(firstRow, last, rgbaScratch);
            tex.update(rgbaScratch.data(), {w, last - firstRow}, {0, firstRow});
        }
        shownVersion = imageVersion();
    }

public:
    bool autoSave = false;
    std::function<std::string()> saveDir;                  // default folder for pictures
    std::function<void(const std::string&)> setSaveDir;    // changes the default folder

    explicit ImageDecoderPanel(const std::string& n, double centreHz, double halfBwHz)
        : SDRModule(n), tone(48000.0, centreHz, halfBwHz) {}

    void init(const ModuleContext& ctx) override {
        font = &ctx.font;
        tone.setRate(ctx.sampleRate);
        buildWidgets();
    }

    float panelHeight(float windowHeight) const override {
        // At least 390 px so the whole settings column fits
        return std::clamp(windowHeight * 0.45f, 390.0f, 560.0f);
    }

    void processAudio(const std::vector<float>& audio) override {
        std::lock_guard<std::mutex> l(mtx);
        trackScratch.clear();
        tone.process(audio.data(), audio.size(), trackScratch);
        updateHistogram(trackScratch);
        processTrack(trackScratch);
    }

    bool handleEvent(const sf::Event& ev, const sf::RenderWindow& win) override {
        if (!enabled) return false;
        for (auto& w : widgets) if (w->handleEvent(ev, win)) return true;
        sf::Vector2f m = win.mapPixelToCoords(sf::Mouse::getPosition(win));
        if (const auto* sc = ev.getIf<sf::Event::MouseWheelScrolled>()) {
            if (!panelRect.contains(m)) return false;
            if (imageRect.contains(m)) {
                scrollY -= sc->delta * 60.0f;
                followNewest = false;   // re-enabled in draw() when scrolled to the bottom
            }
            return true;
        }
        if (ev.getIf<sf::Event::MouseButtonPressed>() && panelRect.contains(m)) return true;
        return false;
    }

    void draw(sf::RenderWindow& win, float x, float y, float w, float h) override {
        if (!enabled || !font) return;
        panelRect = sf::FloatRect({x, y}, {w, h});

        std::string status;
        float hist[HIST_BINS];
        std::unique_ptr<PendingSave> toSave;
        unsigned imgW = 0, rows = 0;
        {
            std::lock_guard<std::mutex> l(mtx);
            syncTexture();
            status = statusText();
            std::copy(std::begin(histogram), std::end(histogram), hist);
            toSave = std::move(pendingSave);
            imgW = imageWidth(); rows = imageRows();
        }
        if (toSave) writeImage(*toSave);
        for (auto& wd : widgets) wd->update(win);

        // --- background ---
        sf::RectangleShape bg({w, h});
        bg.setPosition({x, y});
        bg.setFillColor(sf::Color(20, 22, 27, 245));
        bg.setOutlineColor(Theme::Accent);
        bg.setOutlineThickness(1.0f);
        win.draw(bg);

        // --- picture ---
        const float pad = 8.0f;
        imageRect = sf::FloatRect({x + pad, y + pad}, {w - SETTINGS_W - 2.0f * pad, h - 2.0f * pad});
        sf::RectangleShape imgBg(imageRect.size);
        imgBg.setPosition(imageRect.position);
        imgBg.setFillColor(sf::Color(8, 8, 10));
        win.draw(imgBg);

        if (texW > 0 && rows > 0 && imageRect.size.x > 10.0f && imageRect.size.y > 10.0f) {
            const unsigned shownRows = std::min(rows, texH);
            float scale = imageRect.size.x / (float)imgW;
            if (fitWholeImage()) scale = std::min(scale, imageRect.size.y / (float)shownRows);
            const float contentH = shownRows * scale;
            const float maxScroll = std::max(0.0f, contentH - imageRect.size.y);
            if (followNewest) scrollY = maxScroll;
            scrollY = std::clamp(scrollY, 0.0f, maxScroll);
            if (scrollY >= maxScroll - 0.5f) followNewest = true;

            int srcY = (int)(scrollY / scale);
            int srcH = std::min((int)shownRows - srcY, (int)std::ceil(imageRect.size.y / scale));
            if (srcH > 0) {
                sf::Sprite spr(tex, sf::IntRect({0, srcY}, {(int)imgW, srcH}));
                spr.setScale({scale, scale});
                spr.setPosition(imageRect.position);
                win.draw(spr);
            }
            if (maxScroll > 0.0f) {   // scroll bar
                float trackH = imageRect.size.y;
                float thumbH = std::max(20.0f, trackH * imageRect.size.y / contentH);
                float thumbY = imageRect.position.y + (trackH - thumbH) * (scrollY / maxScroll);
                sf::RectangleShape thumb({4.0f, thumbH});
                thumb.setPosition({imageRect.position.x + imageRect.size.x - 6.0f, thumbY});
                thumb.setFillColor(sf::Color(200, 200, 200, 140));
                win.draw(thumb);
            }
        } else {
            sf::Text t = text("No picture yet", 28, sf::Color(110, 110, 120));
            t.setPosition({imageRect.position.x + 12.0f, imageRect.position.y + 10.0f});
            win.draw(t);
        }

        // --- settings column ---
        const float cx = x + w - SETTINGS_W + 4.0f;
        float cy = y + pad;
        sf::Text title = text(titleText(), 30, Theme::Accent);
        title.setStyle(sf::Text::Bold);
        title.setPosition({cx, cy});
        win.draw(title);
        cy += 22.0f;
        sf::Text st = text(status, 22, sf::Color(220, 220, 225));
        st.setPosition({cx, cy});
        win.draw(st);
        cy += 18.0f;

        // Tone histogram: shows where the audio tones are, to help with tuning
        const float hx = cx, hw = SETTINGS_W - 24.0f, hh = 28.0f;
        sf::RectangleShape hb({hw, hh});
        hb.setPosition({hx, cy});
        hb.setFillColor(sf::Color(10, 10, 12));
        win.draw(hb);
        float hmax = 1.0f;
        for (float v : hist) hmax = std::max(hmax, v);
        const float bw = hw / HIST_BINS;
        for (int i = 0; i < HIST_BINS; i++) {
            float bh = (hh - 2.0f) * std::sqrt(hist[i] / hmax);
            sf::RectangleShape bar({bw - 1.0f, bh});
            bar.setPosition({hx + i * bw, cy + hh - bh});
            bar.setFillColor(Theme::Accent);
            win.draw(bar);
        }
        for (double mk : {SYNC_HZ, BLACK_HZ, WHITE_HZ}) {
            float mx = hx + (float)((mk - HIST_LO) / (HIST_HI - HIST_LO)) * hw;
            sf::RectangleShape line({1.0f, hh});
            line.setPosition({mx, cy});
            line.setFillColor(sf::Color(255, 90, 90, 200));
            win.draw(line);
        }
        cy += hh + 2.0f;
        sf::Text hl = text("1200    1500                      2300 Hz", 18, sf::Color(130, 130, 140));
        hl.setPosition({hx + (float)((SYNC_HZ - HIST_LO) / (HIST_HI - HIST_LO)) * hw - 8.0f, cy});
        win.draw(hl);
        cy += 16.0f;

        for (auto& wd : widgets) {
            wd->setPosition(cx, cy);
            wd->draw(win);
            cy += wd->getHeight() + 3.0f;
        }
        {
            // Last message, or the default picture folder
            std::string line = message;
            sf::Color col(150, 220, 150);
            if (line.empty()) {
                std::string dir = saveDir ? saveDir() : std::string(".");
                if (dir.size() > 38) dir = "..." + dir.substr(dir.size() - 35);
                line = "Folder: " + dir;
                col = sf::Color(130, 130, 140);
            }
            if (line.size() > 46) line = line.substr(0, 45) + "...";
            sf::Text mt = text(line, 20, col);
            mt.setPosition({cx, std::min(cy + 2.0f, y + h - 16.0f)});
            win.draw(mt);
        }
        for (auto& wd : widgets) wd->drawOverlay(win);
    }
};

} // namespace imgdec
