#pragma once

#include "ImageDecoderPanel.h"
#include "SstvCore.h"

// SSTV decoder panel: picture on the left, mode / slant / save controls on the right.
class SSTVModule : public imgdec::ImageDecoderPanel {
    imgdec::SstvDecoder dec;
    int selectedMode = -1;   // -1 = automatic (VIS)

    std::shared_ptr<Dropdown> ddMode;
    std::shared_ptr<Checkbox> chkAutoSlant, chkAutoSave;
    std::shared_ptr<Slider> slSlant, slShift;
    std::shared_ptr<SdrButton> btnStart, btnStop;

    static std::string slantText(double ppm) {
        char b[48]; std::snprintf(b, sizeof(b), "Slant fine tune: %+.0f ppm", ppm); return b;
    }
    static std::string shiftText(double px) {
        char b[48]; std::snprintf(b, sizeof(b), "Horizontal shift: %+.0f px", px); return b;
    }

protected:
    void processTrack(const std::vector<float>& track) override { dec.process(track.data(), track.size()); }
    std::string titleText() const override { return "SSTV"; }

    std::string statusText() override {
        const auto& modes = imgdec::sstvModes();
        if (dec.getState() == imgdec::SstvDecoder::State::Receiving) {
            char b[96];
            std::snprintf(b, sizeof(b), "%s: line %d / %d  (%+.0f ppm)", modes[dec.getModeIdx()].name,
                          dec.receivedLines, dec.totalLines(), dec.effectiveSlantPpm());
            return b;
        }
        if (dec.getModeIdx() >= 0 && dec.receivedLines > 0)
            return std::string(modes[dec.getModeIdx()].name) + " done - waiting for VIS";
        return "Waiting for VIS header...";
    }

    unsigned imageWidth() const override { return dec.receivedLines > 0 || dec.getState() == imgdec::SstvDecoder::State::Receiving ? (unsigned)dec.width : 0; }
    unsigned imageRows() const override { return imageWidth() ? (unsigned)dec.height : 0; }
    uint64_t imageVersion() const override { return dec.version; }
    bool fitWholeImage() const override { return true; }

    void rowsToRgba(unsigned y0, unsigned y1, std::vector<uint8_t>& out) const override {
        const unsigned w = (unsigned)dec.width;
        out.resize((size_t)(y1 - y0) * w * 4);
        for (unsigned y = y0; y < y1; y++)
            for (unsigned x = 0; x < w; x++) {
                const uint8_t* s = &dec.rgb[((size_t)y * w + x) * 3];
                uint8_t* d = &out[((size_t)(y - y0) * w + x) * 4];
                d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255;
            }
    }

    std::string fileTag() const override {
        std::string t = "sstv";
        if (dec.getModeIdx() >= 0) {
            t += "_";
            for (const char* c = imgdec::sstvModes()[dec.getModeIdx()].name; *c; c++) if (*c != ' ') t += *c;
        }
        return t;
    }

    void buildWidgets() override {
        const float cw = SETTINGS_W - 24.0f;
        std::vector<std::string> names = {"Auto (VIS)"};
        for (const auto& m : imgdec::sstvModes()) names.push_back(m.name);
        ddMode = std::make_shared<Dropdown>(cw, 24.0f, *font);
        ddMode->setOptions(names);
        ddMode->onChange = [this](int i) { selectedMode = i - 1; };

        btnStart = std::make_shared<SdrButton>((cw - 8.0f) / 2.0f, 24.0f, "Start now", *font);
        btnStop  = std::make_shared<SdrButton>((cw - 8.0f) / 2.0f, 24.0f, "Stop", *font);
        btnStart->onClick = [this]() {
            if (selectedMode < 0) { message = "Pick a mode to start without VIS"; return; }
            std::lock_guard<std::mutex> l(mtx);
            dec.startManual(selectedMode);
            message.clear();
        };
        btnStop->onClick = [this]() { std::lock_guard<std::mutex> l(mtx); dec.stop(); };

        chkAutoSlant = std::make_shared<Checkbox>("Automatic slant correction", *font, true);
        chkAutoSlant->onToggle = [this](bool b) { std::lock_guard<std::mutex> l(mtx); dec.autoSlant = b; };

        slSlant = std::make_shared<Slider>(cw, -1000.0f, 1000.0f, 0.0f, slantText(0), *font);
        slSlant->onChange = [this](float v) {
            v = std::round(v);
            std::lock_guard<std::mutex> l(mtx);
            if (dec.manualSlantPpm == v) return;
            dec.manualSlantPpm = v; dec.refresh();
            slSlant->setText(slantText(v));
        };
        slShift = std::make_shared<Slider>(cw, -100.0f, 100.0f, 0.0f, shiftText(0), *font);
        slShift->onChange = [this](float v) {
            v = std::round(v);
            std::lock_guard<std::mutex> l(mtx);
            if (dec.shiftPx == v) return;
            dec.shiftPx = v; dec.refresh();
            slShift->setText(shiftText(v));
        };

        chkAutoSave = std::make_shared<Checkbox>("Save every picture automatically", *font, false);
        chkAutoSave->onToggle = [this](bool b) { std::lock_guard<std::mutex> l(mtx); autoSave = b; };

        auto fileButtons = makeFileButtons([this]() { std::lock_guard<std::mutex> l(mtx); dec.clear(); message.clear(); });

        widgets = { std::make_shared<Label>("Mode", *font, 11, Theme::Text), ddMode,
                    std::make_shared<imgdec::ButtonRow>(std::vector<std::shared_ptr<SdrButton>>{btnStart, btnStop}),
                    chkAutoSlant, slSlant, slShift, chkAutoSave, fileButtons };
    }

public:
    SSTVModule() : ImageDecoderPanel("SSTV", 1750.0, 900.0) {
        dec.onImageComplete = [this]() { queueAutoSave(); };   // runs with mtx held
    }

    // --- persistent settings ---
    bool getAutoSlant() { std::lock_guard<std::mutex> l(mtx); return dec.autoSlant; }
    float getSlantPpm() { std::lock_guard<std::mutex> l(mtx); return (float)dec.manualSlantPpm; }
    bool getAutoSave() { std::lock_guard<std::mutex> l(mtx); return autoSave; }
    void applySettings(bool autoSlantOn, float slantPpm, bool autoSaveOn) {
        std::lock_guard<std::mutex> l(mtx);
        dec.autoSlant = autoSlantOn; dec.manualSlantPpm = std::round(slantPpm); autoSave = autoSaveOn;
        if (chkAutoSlant) chkAutoSlant->checked = autoSlantOn;
        if (chkAutoSave) chkAutoSave->checked = autoSaveOn;
        if (slSlant) { slSlant->setValueSilent((float)dec.manualSlantPpm); slSlant->setText(slantText(dec.manualSlantPpm)); }
    }
};
