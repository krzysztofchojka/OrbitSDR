#pragma once

#include "ImageDecoderPanel.h"
#include "WefaxCore.h"

// WEFAX (HF radiofax) decoder panel: scrolling chart on the left, LPM / IOC /
// APT / slant / save controls on the right.
class WEFAXModule : public imgdec::ImageDecoderPanel {
    imgdec::WefaxDecoder dec;
    bool selectorsDirty = false;   // IOC changed on the DSP thread; the UI thread updates the selector

    std::shared_ptr<imgdec::Selector> selLpm, selIoc;
    std::shared_ptr<Checkbox> chkApt, chkAutoSave;
    std::shared_ptr<Slider> slSlant, slShift;
    std::shared_ptr<SdrButton> btnStart, btnStop;

    static const std::vector<int>& lpmValues() { static const std::vector<int> v = {60, 90, 100, 120, 180, 240}; return v; }
    static const std::vector<int>& iocValues() { static const std::vector<int> v = {576, 288}; return v; }

    static std::string slantText(double ppm) {
        char b[48]; std::snprintf(b, sizeof(b), "Slant fine tune: %+.0f ppm", ppm); return b;
    }
    static std::string shiftText(double px) {
        char b[48]; std::snprintf(b, sizeof(b), "Horizontal shift: %+.0f px", px); return b;
    }

    void syncSelectors() {
        auto idxOf = [](const std::vector<int>& v, int x) {
            for (size_t i = 0; i < v.size(); i++) if (v[i] == x) return (int)i;
            return 0;
        };
        if (selLpm) selLpm->setIndex(idxOf(lpmValues(), dec.lpm));
        if (selIoc) selIoc->setIndex(idxOf(iocValues(), dec.ioc));
    }

protected:
    void processTrack(const std::vector<float>& track) override {
        int iocBefore = dec.ioc;
        dec.process(track.data(), track.size());
        if (dec.ioc != iocBefore) selectorsDirty = true;   // IOC taken from the start tone
    }
    std::string titleText() const override { return "WEFAX"; }

    std::string statusText() override {
        if (selectorsDirty) { syncSelectors(); selectorsDirty = false; }
        char b[96];
        if (dec.getState() == imgdec::WefaxDecoder::State::Receiving) {
            std::snprintf(b, sizeof(b), "Receiving: line %d%s  (%+.0f ppm)", dec.lines,
                          dec.isPhased() ? "" : ", phasing", dec.effectiveSlantPpm());
            return b;
        }
        if (dec.autoApt) return dec.lines > 0 ? "Done - waiting for start tone" : "Waiting for start tone...";
        return dec.lines > 0 ? "Stopped" : "Idle - press Start";
    }

    unsigned imageWidth() const override { return dec.lines > 0 ? (unsigned)dec.width : 0; }
    unsigned imageRows() const override { return (unsigned)dec.lines; }
    uint64_t imageVersion() const override { return dec.version; }
    bool fitWholeImage() const override { return false; }

    unsigned takeDirtyRow() override {
        unsigned d = (unsigned)std::max(0, dec.dirtyFrom);
        dec.dirtyFrom = dec.lines;
        return d;
    }

    void rowsToRgba(unsigned y0, unsigned y1, std::vector<uint8_t>& out) const override {
        const unsigned w = (unsigned)dec.width;
        out.resize((size_t)(y1 - y0) * w * 4);
        for (unsigned y = y0; y < y1; y++)
            for (unsigned x = 0; x < w; x++) {
                uint8_t g = dec.gray[(size_t)y * w + x];
                uint8_t* d = &out[((size_t)(y - y0) * w + x) * 4];
                d[0] = d[1] = d[2] = g; d[3] = 255;
            }
    }

    std::string fileTag() const override { return "wefax"; }

    void buildWidgets() override {
        const float cw = SETTINGS_W - 24.0f;
        std::vector<std::string> lpmNames, iocNames;
        for (int v : lpmValues()) lpmNames.push_back(std::to_string(v));
        for (int v : iocValues()) iocNames.push_back(std::to_string(v));

        selLpm = std::make_shared<imgdec::Selector>("LPM", cw, *font, lpmNames);
        selLpm->onChange = [this](int i) {
            std::lock_guard<std::mutex> l(mtx);
            dec.lpm = lpmValues()[i];
            dec.refresh();
        };
        selIoc = std::make_shared<imgdec::Selector>("IOC", cw, *font, iocNames);
        selIoc->onChange = [this](int i) {
            std::lock_guard<std::mutex> l(mtx);
            dec.ioc = iocValues()[i];   // applies to the next picture (it sets the line width)
        };
        syncSelectors();

        chkApt = std::make_shared<Checkbox>("Auto start / stop (APT tones)", *font, true);
        chkApt->onToggle = [this](bool b) { std::lock_guard<std::mutex> l(mtx); dec.autoApt = b; };

        btnStart = std::make_shared<SdrButton>((cw - 8.0f) / 2.0f, 24.0f, "Start now", *font);
        btnStop  = std::make_shared<SdrButton>((cw - 8.0f) / 2.0f, 24.0f, "Stop", *font);
        btnStart->onClick = [this]() { std::lock_guard<std::mutex> l(mtx); dec.startManual(); message.clear(); };
        btnStop->onClick = [this]() { std::lock_guard<std::mutex> l(mtx); dec.stop(); };

        slSlant = std::make_shared<Slider>(cw, -1000.0f, 1000.0f, 0.0f, slantText(0), *font);
        slSlant->onChange = [this](float v) {
            v = std::round(v);
            std::lock_guard<std::mutex> l(mtx);
            if (dec.manualSlantPpm == v) return;
            dec.manualSlantPpm = v; dec.refresh();
            slSlant->setText(slantText(v));
        };
        slShift = std::make_shared<Slider>(cw, -905.0f, 905.0f, 0.0f, shiftText(0), *font);
        slShift->onChange = [this](float v) {
            v = std::round(v);
            std::lock_guard<std::mutex> l(mtx);
            if (dec.shiftPx == v) return;
            dec.shiftPx = v; dec.refresh();
            slShift->setText(shiftText(v));
        };

        chkAutoSave = std::make_shared<Checkbox>("Save every chart automatically", *font, false);
        chkAutoSave->onToggle = [this](bool b) { std::lock_guard<std::mutex> l(mtx); autoSave = b; };

        auto fileButtons = makeFileButtons([this]() { std::lock_guard<std::mutex> l(mtx); dec.clear(); message.clear(); });

        widgets = { selLpm, selIoc, chkApt,
                    std::make_shared<imgdec::ButtonRow>(std::vector<std::shared_ptr<SdrButton>>{btnStart, btnStop}),
                    slSlant, slShift, chkAutoSave, fileButtons };
    }

public:
    WEFAXModule() : ImageDecoderPanel("WEFAX", 1900.0, 500.0) {
        channels.name = "WEFAX USB";
        // Assigned frequencies (DWD Pinneberg, Northwood, NOAA Boston, Point Reyes);
        // the USB dial sits 1.9 kHz below them
        const long long assigned[] = { 3855000, 7880000, 13882500, 2618500, 4610000, 8040000, 11086500,
                                       4235000, 6340500, 9110000, 12750000, 4346000, 8682000, 12786000 };
        for (long long f : assigned) channels.presets.push_back(f - 1900);
        channels.maxChannels = 1;
        channels.followTunerWidth = true;
        channels.resizable = true;
        channels.defaultWidth = 3000.0;
        channels.sideband = +1;
        channels.listenMode = Mode::USB;
        dec.onImageComplete = [this]() { queueAutoSave(); };   // runs with mtx held
    }

    // --- persistent settings ---
    int getLpm() { std::lock_guard<std::mutex> l(mtx); return dec.lpm; }
    int getIoc() { std::lock_guard<std::mutex> l(mtx); return dec.ioc; }
    bool getAutoApt() { std::lock_guard<std::mutex> l(mtx); return dec.autoApt; }
    float getSlantPpm() { std::lock_guard<std::mutex> l(mtx); return (float)dec.manualSlantPpm; }
    bool getAutoSave() { std::lock_guard<std::mutex> l(mtx); return autoSave; }
    void applySettings(int lpm, int ioc, bool apt, float slantPpm, bool autoSaveOn) {
        std::lock_guard<std::mutex> l(mtx);
        if (std::find(lpmValues().begin(), lpmValues().end(), lpm) != lpmValues().end()) dec.lpm = lpm;
        if (ioc == 288 || ioc == 576) dec.ioc = ioc;
        dec.autoApt = apt; dec.manualSlantPpm = std::round(slantPpm); autoSave = autoSaveOn;
        syncSelectors();
        if (chkApt) chkApt->checked = apt;
        if (chkAutoSave) chkAutoSave->checked = autoSaveOn;
        if (slSlant) { slSlant->setValueSilent((float)dec.manualSlantPpm); slSlant->setText(slantText(dec.manualSlantPpm)); }
    }
};
