#pragma once

// =====================================================================
//  Satellite map window (opened from the MAP button in the top bar).
//
//  Left: panel with the satellite list (groups, search, favourites,
//  pasted TLEs), details + polar plot + passes of the selected
//  satellite, Doppler / frequency tracking and the ground station.
//  Right: equirectangular world map with day/night shading, footprints
//  (exact fill, see buildFootprint), ground tracks, the station and the
//  sun. Wheel = zoom, drag = pan (wraps around the date line).
//
//  The window lives in the UI thread next to the main window; main.cpp
//  calls frame() once per loop.
// =====================================================================

#include "MapWidgets.h"
#include "SatCatalog.h"
#include "Tracker.h"
#include "RecordingTime.h"
#include "TileLayer.h"
#include "../modules/Core/PositionBus.h"
#include "../modules/Core/AprsPosition.h"
#include "../NativeDialogs.h"
#include "../MacWindowFix.h"
#include <SFML/Graphics.hpp>
#include <SFML/Window/Clipboard.hpp>
#include <memory>
#include <map>
#include <set>
#include <optional>
#include <chrono>

namespace orbit {

class MapWindow {
public:
    std::function<float()> uiScale;                       // same scale as the main window
    std::function<void(const std::string&)> onEnableDecoder;   // "SSTV", "APRS"
    // main.cpp: the IQ file being played (false = no file). progress 0..1
    std::function<bool(std::string& path, double& progress, double& durationSec)> recordingInfo;
    const sf::Image* icon = nullptr;

    MapWindow(const sf::Font& f, SatCatalog& c, SatTracker& t) : font(f), cat(c), trk(t), panel(PANEL_W, f) {
        tiles = std::make_unique<TileCache>(getConfigDir() + PATH_SEP + "tiles");
        tiles->apiUrl = cat.env.apiUrl;
        tiles->apiKey = cat.env.apiKey;
        buildPanel();
    }

    bool isOpen() const { return win && win->isOpen(); }

    // Time for the frequency / Doppler tracking: real time, or the time of the
    // recording being played / the manual date in those modes
    double trackerJd() { refreshTime(); return curBaseJd; }

    void openOrFocus() {
        if (isOpen()) {
#ifdef _WIN32
            HWND h = (HWND)win->getNativeHandle();
            if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
            SetForegroundWindow(h);
#endif
            win->requestFocus();
            return;
        }
        const sf::VideoMode desk = sf::VideoMode::getDesktopMode();
        const sf::Vector2u size{ desk.size.x * 80 / 100, desk.size.y * 80 / 100 };
        sf::ContextSettings cs;
        cs.antiAliasingLevel = 4;
        win = std::make_unique<sf::RenderWindow>(sf::VideoMode(size), "OrbitSDR - Satellite Map", sf::Style::Default, sf::State::Windowed, cs);
        win->setPosition({ (int)(desk.size.x - size.x) / 2 + 30, (int)(desk.size.y - size.y) / 2 + 30 });
        win->setVerticalSyncEnabled(false);
        win->setFramerateLimit(0);   // the main window's limit paces the loop
        if (icon && icon->getSize().x > 0) win->setIcon(*icon);
        applyView();
        camX = lonToX(trk.obs.lonDeg);   // start with the station in the middle
        camY = latToY(std::clamp(trk.obs.latDeg * 0.5, -60.0, 60.0));
        clampCamera();
        if (!mapLoaded && !mapDownloading) loadMap();
        cat.ensureGroup(groupId);
        cat.ensureFavorites();
        panel.updateStyle();   // the theme may have changed since the panel was built
        dirty = true;
    }

    // Events + drawing; call every main loop iteration
    void frame() {
        if (!win) return;
        if (!win->isOpen()) { closeCleanup(); return; }
        handleEvents();
        if (!win || !win->isOpen()) { closeCleanup(); return; }
        refreshTime();
        syncMacWindowSize(*win);   // macOS window tiling resizes without telling SFML
        if (win->getSize() != lastSize) applyView();
        panel.update(*win);
        // ~30 fps is plenty for a map; faster while the user interacts
        if (!dirty && drawClock.getElapsedTime().asMilliseconds() < 33) return;
        dirty = false;
        drawClock.restart();
        updateState();
        draw();
    }

    void loadSettings(SettingsManager& s) {
        groupId = s.getString("map_group", "amateur");
        bool known = false;
        for (auto& g : satGroups()) if (g.id == groupId) known = true;
        if (!known || groupId == "search") groupId = "amateur";
        selected = s.getInt("map_selected", 25544);
        showNight = s.getBool("map_night", true);
        showGrid = s.getBool("map_grid", true);
        showTracks = s.getBool("map_tracks", true);
        allLabels = s.getBool("map_all_labels", false);
        footprintMode = std::clamp(s.getInt("map_footprints", 1), 0, 2);
        // a manual date is not kept between runs, the recording mode is
        timeSource = s.getInt("map_time_source", 0) == (int)TimeSource::Recording ? TimeSource::Recording : TimeSource::Live;
        timeInputLocal = s.getBool("map_time_local", true);
        baseLayer = std::clamp(s.getInt("map_layer", 0), 0, 2);
        placeNames = s.getBool("map_place_names", true);
        const int kinds = s.getInt("map_pos_kinds", 15);
        for (int i = 0; i < (int)PosKind::Count; i++) showKind[i] = (kinds >> i) & 1;
        showKind[(int)PosKind::HF] = s.getBool("map_pos_hf", true);   // added later: own key, on by default
        posWindowIdx = std::clamp(s.getInt("map_pos_window", 2), 0, (int)POS_WINDOWS.size() - 1);
        syncPanelFromState();
    }
    void saveSettings(SettingsManager& s) const {
        s.set("map_group", groupId);
        s.set("map_selected", selected);
        s.set("map_night", showNight);
        s.set("map_grid", showGrid);
        s.set("map_tracks", showTracks);
        s.set("map_all_labels", allLabels);
        s.set("map_footprints", footprintMode);
        s.set("map_time_source", (int)timeSource);
        s.set("map_time_local", timeInputLocal);
        s.set("map_layer", baseLayer);
        s.set("map_place_names", placeNames);
        int kinds = 0;
        for (int i = 0; i < (int)PosKind::Count; i++) if (showKind[i]) kinds |= 1 << i;
        s.set("map_pos_kinds", kinds);
        s.set("map_pos_hf", showKind[(int)PosKind::HF]);
        s.set("map_pos_window", posWindowIdx);
    }

private:
    static constexpr float PANEL_W = 360.0f;
    static constexpr float TOOLBAR_H = 36.0f;
    static constexpr float STATUS_H = 24.0f;
    static constexpr double WW = 3600.0;   // world units: 0.1 deg
    static constexpr double WH = 1800.0;
    static constexpr float CW = PANEL_W - 50.0f;   // content width inside the panel
    static constexpr double MAX_ZOOM = 20000.0;      // ~5 m per pixel (street level)

    const sf::Font& font;
    SatCatalog& cat;
    SatTracker& trk;
    std::unique_ptr<sf::RenderWindow> win;
    Sidebar panel;
    sf::Vector2u lastSize{0, 0};
    sf::Vector2f logical{1, 1};
    bool dirty = true;
    sf::Clock drawClock, refreshClock;

    // --- state ---
    std::string groupId = "amateur";
    int selected = 0;
    double simOffsetSec = 0.0;
    bool showNight = true, showGrid = true, showTracks = true, allLabels = false;
    int footprintMode = 1;   // 0 selected, 1 selected + favourites, 2 all
    bool pickQth = false;

    // map view
    double zoom = 1.0, camX = WW / 2, camY = WH / 2;
    bool dragging = false, dragMoved = false;
    sf::Vector2f dragStart, dragLast;
    int hovered = 0;
    std::optional<SunPos> sunHover;
    sf::Vector2f mouseUi{-1, -1};
    float toolbarRightX = 1e9f;

    // tiles (zooming in) + positions heard by the decoders
    std::unique_ptr<TileCache> tiles;
    int baseLayer = 0;          // 0 satellite photos, 1 street map, 2 Blue Marble only
    bool placeNames = true;     // borders + names over the photos
    std::set<int> layersDrawn;  // for the attribution line
    bool showKind[(int)PosKind::Count] = {true, true, true, true, true};
    int posWindowIdx = 2;
    inline static const std::vector<std::pair<double, const char*>> POS_WINDOWS = {
        {15 * 60.0, "15 min"}, {3600.0, "1 h"}, {6 * 3600.0, "6 h"}, {24 * 3600.0, "24 h"}, {1e12, "all"}};
    std::vector<PosTrack> posTracks;   // this frame
    int hoveredPos = -1;               // index into posTracks
    sf::Vector2f hoveredPosUi;
    SdrButton tbKind[(int)PosKind::Count] = {{64, 24, "AIS", font}, {64, 24, "APRS", font}, {70, 24, "ACARS", font}, {66, 24, "ADS-B", font}, {56, 24, "HF", font}};
    SdrButton tbWindow{92, 24, "Last 6 h", font};

    // map picture
    sf::Texture mapTex;
    bool mapLoaded = false, mapDownloading = false;
    std::string mapStatus;
    long long mapFileSize = -1;   // size of the picture on disk when it was last tried
    sf::Clock mapWatchClock;

    // night shading
    sf::VertexArray night{sf::PrimitiveType::Triangles};
    double nightJd = 0;

    // satellites on screen
    struct SatPos { int id; LookAngles la; };
    std::vector<SatPos> positions;
    std::vector<int> listIds;
    double listSortedAt = 0;
    std::string lastSearch;
    float listScroll = 0;
    uint64_t listVersion = 0;
    std::string listGroup;

    // passes
    std::map<int, Pass> nextPass;           // for the list (real time)
    std::vector<Pass> selPasses;            // selected satellite (real time)
    int selPassesFor = -1;
    double obsKey = 0;

    // radio
    std::vector<Transmitter> txList;
    int txFor = -1;
    uint64_t txVersionSeen = 0;
    int txIndex = 0;
    long long chosenHz = 0;

    // --- widgets ---
    std::shared_ptr<Dropdown> ddGroup, ddTx, ddTarget, ddRx, ddFootprints, ddTime, ddLayer;
    std::vector<int> rxOptIds;                 // ddRx option -> receiver id
    std::vector<std::string> targetOptDec;     // ddTarget option -> decoder name ([0] tuner, [1] "" automatic)
    std::string trackChoicesKey;               // what the two lists were built from
    std::shared_ptr<Checkbox> chkPlaceNames;
    std::shared_ptr<SdrButton> btnRefresh, btnPaste, btnImport, btnFav, btnUseFreq, btnOff, btnFreq, btnDoppler, btnPick, btnApplyQth;
    std::shared_ptr<TextInput> txtSearch, txtFreq;
    std::shared_ptr<SdrButton> btnTimeReset;
    std::shared_ptr<CustomWidget> wDateTime;
    std::shared_ptr<Checkbox> chkTimeLocal;
    std::shared_ptr<CustomWidget> wTimeInfo;
    std::shared_ptr<LabeledInput> inLat, inLon, inAlt, inLoc;
    std::shared_ptr<CustomWidget> wList, wCatStatus, wInfo, wPolar, wPasses, wRadio;
    std::shared_ptr<Slider> slMinEl;
    std::shared_ptr<Checkbox> chkNight, chkGrid, chkTracks, chkLabels, chkSetMode;
    std::vector<std::shared_ptr<Dropdown>> allDropdowns;
    SdrButton tbM1h{44, 24, "-1h", font}, tbM10{44, 24, "-10m", font}, tbNow{50, 24, "NOW", font},
              tbP10{44, 24, "+10m", font}, tbP1h{44, 24, "+1h", font};
    sf::FloatRect radioDecoderBtn;   // "Enable SSTV decoder" link inside the radio block

    // --- time source ---
    enum class TimeSource { Live = 0, Recording = 1, Manual = 2 };
    TimeSource timeSource = TimeSource::Live;
    double curBaseJd = 0;                 // "now" of the map (before the -1h / +1h preview offset)
    double manualUnix = 0;                // Manual: the date set ...
    std::chrono::steady_clock::time_point manualSetAt;   // ... and when it was set (it runs on)
    std::string recPath;                  // Recording: the file the start time belongs to
    RecordingStart recStart;
    bool recStartUser = false;            // start time typed by the user
    bool recPlaying = false;
    double recProgress = 0, recDuration = 0;
    bool timeInputLocal = true;           // the date field is in local time (else UTC)

    void refreshTime() {
        const double now = nowJd();
        recPlaying = false;
        if (timeSource == TimeSource::Manual) {
            const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - manualSetAt).count();
            curBaseJd = unixToJd(manualUnix + el);
            return;
        }
        if (timeSource == TimeSource::Recording && recordingInfo) {
            std::string path;
            double prog = 0, dur = 0;
            if (recordingInfo(path, prog, dur) && !path.empty()) {
                if (path != recPath) {   // another file: detect its start time
                    recPath = path;
                    recStart = recordingStart(path, dur);
                    recStartUser = false;
                }
                recPlaying = true;
                recProgress = prog;
                recDuration = dur;
                if (recStart.unix > 0) { curBaseJd = unixToJd(recStart.unix + prog * dur); return; }
            }
        }
        curBaseJd = now;
    }
    double baseJd() const { return curBaseJd > 0 ? curBaseJd : nowJd(); }
    double lastPassBaseJd = 0;
    double simJd() const { return baseJd() + simOffsetSec / 86400.0; }

    // ------------------------------------------------------------------
    //  Panel
    // ------------------------------------------------------------------
    void buildPanel() {
        // --- Satellites ---
        auto mSat = panel.addModule("Satellites");
        ddGroup = std::make_shared<Dropdown>(CW - 86.0f, 24.0f, font);
        std::vector<std::string> names;
        for (auto& g : satGroups()) names.push_back(g.label);
        ddGroup->setOptions(names);
        ddGroup->onChange = [this](int i) {
            groupId = satGroups()[i].id;
            txtSearch->setText("");
            listScroll = 0;
            cat.ensureGroup(groupId);
            listSortedAt = 0;
        };
        btnRefresh = std::make_shared<SdrButton>(80.0f, 24.0f, "Refresh", font);
        btnRefresh->onClick = [this]() { cat.ensureGroup(groupId, true); if (groupId != "favorites") cat.ensureFavorites(true); };
        auto row1 = std::make_shared<HRow>();
        row1->add(ddGroup, CW - 86.0f); row1->add(btnRefresh, 80.0f);
        mSat->addWidget(row1);

        txtSearch = std::make_shared<TextInput>(CW, 24.0f, font);
        txtSearch->onSubmit = [this](const std::string& q) {
            if (q.empty()) return;
            cat.searchOnline(q);
            pendingSearchSwitch = true;
        };
        mSat->addWidget(std::make_shared<Label>("Search (name or NORAD, Enter = search online):", font, 11, sf::Color(150, 150, 160)));
        mSat->addWidget(txtSearch);

        wList = std::make_shared<CustomWidget>(CW, 260.0f);
        wList->heightFn = [this]() { return std::clamp(logical.y * 0.30f, 150.0f, 420.0f); };
        wList->onDraw = [this](sf::RenderWindow& w, const sf::FloatRect& r) { drawList(w, r); };
        wList->onEvent = [this](const sf::Event& ev, sf::Vector2f l) { return listEvent(ev, l); };
        wList->hoverHand = true;
        mSat->addWidget(wList);

        btnPaste = std::make_shared<SdrButton>(100.0f, 24.0f, "Paste TLE", font);
        btnPaste->onClick = [this]() {
            const std::string clip = sf::Clipboard::getString().toAnsiString();
            addCustom(clip, "clipboard");
        };
        btnImport = std::make_shared<SdrButton>(100.0f, 24.0f, "Import file...", font);
        btnImport->onClick = [this]() {
            const std::string p = openTleFileDialog();
            if (p.empty()) return;
            std::ifstream f(p, std::ios::binary);
            std::stringstream ss; ss << f.rdbuf();
            addCustom(ss.str(), "file");
            dirty = true;
        };
        btnFav = std::make_shared<SdrButton>(CW - 212.0f, 24.0f, "Favorite", font);
        btnFav->onClick = [this]() { if (selected && cat.get(selected)) cat.toggleFavorite(selected); };
        auto row2 = std::make_shared<HRow>();
        row2->add(btnPaste, 100.0f); row2->add(btnImport, 100.0f); row2->add(btnFav, CW - 212.0f);
        mSat->addWidget(row2);

        wCatStatus = std::make_shared<CustomWidget>(CW, 30.0f);
        wCatStatus->onDraw = [this](sf::RenderWindow& w, const sf::FloatRect& r) {
            std::string s = cat.status;
            if (cat.busy()) s = "Working... " + s;
            sf::Color c = cat.statusError ? sf::Color(255, 150, 80) : sf::Color(140, 140, 150);
            const std::vector<std::string> lines = wrap(s, 11, r.size.x);
            for (size_t i = 0; i < lines.size() && i < 2; i++) drawText(w, font, lines[i], r.position.x, r.position.y + i * 14.0f, 11, c);
        };
        mSat->addWidget(wCatStatus);

        // --- Selected satellite ---
        auto mSel = panel.addModule("Satellite");
        wInfo = std::make_shared<CustomWidget>(CW, 200.0f);
        wInfo->onDraw = [this](sf::RenderWindow& w, const sf::FloatRect& r) { drawInfo(w, r); };
        mSel->addWidget(wInfo);
        wPolar = std::make_shared<CustomWidget>(CW, 232.0f);
        wPolar->onDraw = [this](sf::RenderWindow& w, const sf::FloatRect& r) { drawPolar(w, r); };
        mSel->addWidget(wPolar);
        wPasses = std::make_shared<CustomWidget>(CW, 18.0f * 9 + 6.0f);
        wPasses->onDraw = [this](sf::RenderWindow& w, const sf::FloatRect& r) { drawPasses(w, r); };
        wPasses->onEvent = [this](const sf::Event& ev, sf::Vector2f l) { return passesEvent(ev, l); };
        wPasses->hoverHand = true;
        mSel->addWidget(wPasses);

        // --- Radio / Doppler ---
        auto mRad = panel.addModule("Radio / Doppler");
        mRad->addWidget(std::make_shared<Label>("Transmitter (SatNOGS DB):", font, 11, sf::Color(150, 150, 160)));
        ddTx = std::make_shared<Dropdown>(CW, 24.0f, font);
        ddTx->setOptions({"-"});
        ddTx->onChange = [this](int i) { pickTransmitter(i); };
        mRad->addWidget(ddTx);
        txtFreq = std::make_shared<TextInput>(CW - 106.0f, 24.0f, font);
        txtFreq->onSubmit = [this](const std::string&) { useTypedFreq(); };
        btnUseFreq = std::make_shared<SdrButton>(100.0f, 24.0f, "Set MHz", font);
        btnUseFreq->onClick = [this]() { useTypedFreq(); };
        auto rowF = std::make_shared<HRow>();
        rowF->add(txtFreq, CW - 106.0f); rowF->add(btnUseFreq, 100.0f);
        mRad->addWidget(rowF);

        const float bw = (CW - 12.0f) / 3.0f;
        btnOff = std::make_shared<SdrButton>(bw, 26.0f, "Off", font);
        btnFreq = std::make_shared<SdrButton>(bw, 26.0f, "Track freq", font);
        btnDoppler = std::make_shared<SdrButton>(bw, 26.0f, "Track Doppler", font);
        btnOff->onClick = [this]() { trk.stop(); };
        btnFreq->onClick = [this]() { startTracking(TrackMode::Frequency); };
        btnDoppler->onClick = [this]() { startTracking(TrackMode::Doppler); };
        auto rowT = std::make_shared<HRow>();
        rowT->add(btnOff, bw); rowT->add(btnFreq, bw); rowT->add(btnDoppler, bw);
        mRad->addWidget(rowT);

        // which receiver (tab) and which of its channels follows the satellite
        mRad->addWidget(std::make_shared<Label>("Receiver:", font, 11, sf::Color(150, 150, 160)));
        ddRx = std::make_shared<Dropdown>(CW, 24.0f, font);
        ddRx->setOptions({"RX1"});
        ddRx->onChange = [this](int i) {
            if (i < 0 || i >= (int)rxOptIds.size() || rxOptIds[i] == trk.rxId) return;
            trk.rxId = rxOptIds[i];
            trk.lastCommandHz = 0; trk.userOffsetHz = 0; trk.demodApplied = false; trk.applyStatus.clear();
            trackChoicesKey.clear();
        };
        mRad->addWidget(ddRx);
        mRad->addWidget(std::make_shared<Label>("Tune:", font, 11, sf::Color(150, 150, 160)));
        ddTarget = std::make_shared<Dropdown>(CW, 24.0f, font);
        ddTarget->setOptions({"Tuner (VFO, what you hear)", "Decoder channel (automatic)"});
        ddTarget->onChange = [this](int i) {
            if (i < 0 || i >= (int)targetOptDec.size()) return;
            if (i == 0) trk.target = TrackTarget::Tuner;
            else { trk.target = TrackTarget::Decoder; trk.decoderName = targetOptDec[i]; }
            trk.lastCommandHz = 0; trk.userOffsetHz = 0; trk.applyStatus.clear();
            trackChoicesKey.clear();
        };
        mRad->addWidget(ddTarget);
        chkSetMode = std::make_shared<Checkbox>("Switch demodulator to the transmitter's mode", font, trk.setDemodMode);
        chkSetMode->onToggle = [this](bool b) { trk.setDemodMode = b; };
        mRad->addWidget(chkSetMode);
        wRadio = std::make_shared<CustomWidget>(CW, 104.0f);
        wRadio->onDraw = [this](sf::RenderWindow& w, const sf::FloatRect& r) { drawRadio(w, r); };
        wRadio->onEvent = [this](const sf::Event& ev, sf::Vector2f) {
            if (const auto* mb = ev.getIf<sf::Event::MouseButtonPressed>()) {
                if (mb->button == sf::Mouse::Button::Left && radioDecoderBtn.size.x > 0 &&
                    radioDecoderBtn.contains(win->mapPixelToCoords(mb->position)) && onEnableDecoder) {
                    onEnableDecoder(demodHintFor(currentTx()).decoder);
                    return true;
                }
            }
            return false;
        };
        mRad->addWidget(wRadio);

        // --- Ground station ---
        auto mQth = panel.addModule("Ground station");
        inLat = std::make_shared<LabeledInput>("Latitude", 98.0f, font);
        inLon = std::make_shared<LabeledInput>("Longitude", 98.0f, font);
        inAlt = std::make_shared<LabeledInput>("Altitude (m)", 98.0f, font);
        auto rowQ = std::make_shared<HRow>();
        rowQ->add(inLat, 98.0f); rowQ->add(inLon, 98.0f); rowQ->add(inAlt, 98.0f);
        mQth->addWidget(rowQ);
        inLoc = std::make_shared<LabeledInput>("Locator", 98.0f, font);
        btnPick = std::make_shared<SdrButton>(98.0f, 24.0f, "Pick on map", font);
        btnPick->onClick = [this]() { pickQth = !pickQth; btnPick->setActive(pickQth); };
        btnApplyQth = std::make_shared<SdrButton>(98.0f, 24.0f, "Apply", font);
        btnApplyQth->onClick = [this]() { applyQthInputs(); };
        inLat->input->onSubmit = [this](const std::string&) { applyQthInputs(); };
        inLon->input->onSubmit = [this](const std::string&) { applyQthInputs(); };
        inAlt->input->onSubmit = [this](const std::string&) { applyQthInputs(); };
        inLoc->input->onSubmit = [this](const std::string& s) { applyLocator(s); };
        auto rowQ2 = std::make_shared<HRow>();
        rowQ2->add(inLoc, 98.0f);
        rowQ2->add(std::make_shared<VOffset>(btnPick, 14.0f), 98.0f);
        rowQ2->add(std::make_shared<VOffset>(btnApplyQth, 14.0f), 98.0f);
        mQth->addWidget(rowQ2);
        slMinEl = std::make_shared<Slider>(CW, 0.0f, 30.0f, (float)trk.minElDeg, "Minimum elevation", font);
        slMinEl->onChange = [this](float v) {
            trk.minElDeg = std::round(v);
            slMinEl->setText("Minimum elevation for passes: " + std::to_string((int)trk.minElDeg) + "\xC2\xB0");
        };
        mQth->addWidget(slMinEl);

        // --- Map ---
        auto mMap = panel.addModule("Map");
        chkNight = std::make_shared<Checkbox>("Day / night", font, showNight);
        chkNight->onToggle = [this](bool b) { showNight = b; };
        chkGrid = std::make_shared<Checkbox>("Grid", font, showGrid);
        chkGrid->onToggle = [this](bool b) { showGrid = b; };
        chkTracks = std::make_shared<Checkbox>("Ground track of the selected satellite", font, showTracks);
        chkTracks->onToggle = [this](bool b) { showTracks = b; };
        chkLabels = std::make_shared<Checkbox>("Names of all satellites", font, allLabels);
        chkLabels->onToggle = [this](bool b) { allLabels = b; };
        ddFootprints = std::make_shared<Dropdown>(CW, 24.0f, font);
        ddFootprints->setOptions({"Footprint: selected", "Footprints: selected + favorites", "Footprints: all in list"});
        ddFootprints->onChange = [this](int i) { footprintMode = i; };
        auto rowC = std::make_shared<HRow>();
        rowC->add(chkNight, 120.0f); rowC->add(chkGrid, 100.0f);
        mMap->addWidget(rowC);
        mMap->addWidget(chkTracks);
        mMap->addWidget(chkLabels);
        mMap->addWidget(ddFootprints);
        ddLayer = std::make_shared<Dropdown>(CW, 24.0f, font);
        ddLayer->setOptions({"Zoom: satellite photos (Sentinel-2)", "Zoom: street map (OpenStreetMap)", "Zoom: off (Blue Marble only)"});
        ddLayer->onChange = [this](int i) { baseLayer = i; dirty = true; };
        mMap->addWidget(ddLayer);
        chkPlaceNames = std::make_shared<Checkbox>("Place names and borders over the photos", font, placeNames);
        chkPlaceNames->onToggle = [this](bool b) { placeNames = b; dirty = true; };
        mMap->addWidget(chkPlaceNames);

        // --- Time (old recordings) ---
        auto mTime = panel.addModule("Time");
        ddTime = std::make_shared<Dropdown>(CW, 24.0f, font);
        ddTime->setOptions({"Time: live (real time)", "Time: recording being played", "Time: manual date / time"});
        ddTime->onChange = [this](int i) {
            timeSource = (TimeSource)i;
            if (timeSource == TimeSource::Manual) {   // start from what the map shows now
                manualUnix = jdToUnix(simJd());
                manualSetAt = std::chrono::steady_clock::now();
            }
            simOffsetSec = 0;
            refreshTime();
            fillTimeInput();
            dirty = true;
        };
        mTime->addWidget(ddTime);
        // click the upper / lower half of a field (or turn the wheel over it) to change it
        wDateTime = std::make_shared<CustomWidget>(CW - 66.0f, 42.0f);
        wDateTime->hoverHand = true;
        wDateTime->onDraw = [this](sf::RenderWindow& w, const sf::FloatRect& r) { drawDateTime(w, r); };
        wDateTime->onEvent = [this](const sf::Event& ev, sf::Vector2f l) { return dateTimeEvent(ev, l); };
        btnTimeReset = std::make_shared<SdrButton>(58.0f, 24.0f, "Reset", font);
        btnTimeReset->onClick = [this]() {
            if (timeSource == TimeSource::Recording) recPath.clear();   // detect the start time again
            else if (timeSource == TimeSource::Manual) { manualUnix = jdToUnix(nowJd()); manualSetAt = std::chrono::steady_clock::now(); }
            simOffsetSec = 0;
            refreshTime();
            fillTimeInput();
        };
        auto rowTime = std::make_shared<HRow>();
        rowTime->add(wDateTime, CW - 66.0f); rowTime->add(std::make_shared<VOffset>(btnTimeReset, 9.0f), 58.0f);
        mTime->addWidget(rowTime);
        chkTimeLocal = std::make_shared<Checkbox>("Date in local time (off = UTC)", font, timeInputLocal);
        chkTimeLocal->onToggle = [this](bool b) { timeInputLocal = b; fillTimeInput(); };
        mTime->addWidget(chkTimeLocal);
        wTimeInfo = std::make_shared<CustomWidget>(CW, 44.0f);
        wTimeInfo->onDraw = [this](sf::RenderWindow& w, const sf::FloatRect& r) { drawTimeInfo(w, r); };
        mTime->addWidget(wTimeInfo);

        allDropdowns = {ddGroup, ddTx, ddRx, ddTarget, ddFootprints, ddTime, ddLayer};

        tbM1h.onClick = [this]() { simOffsetSec -= 3600; };
        tbM10.onClick = [this]() { simOffsetSec -= 600; };
        tbNow.onClick = [this]() { simOffsetSec = 0; };
        tbP10.onClick = [this]() { simOffsetSec += 600; };
        tbP1h.onClick = [this]() { simOffsetSec += 3600; };
        for (int i = 0; i < (int)PosKind::Count; i++) tbKind[i].onClick = [this, i]() { showKind[i] = !showKind[i]; };
        tbWindow.onClick = [this]() { posWindowIdx = (posWindowIdx + 1) % (int)POS_WINDOWS.size(); };
    }

    // A widget pushed down by a few pixels (buttons next to captioned inputs)
    class VOffset : public Widget {
    public:
        std::shared_ptr<Widget> inner; float dy;
        VOffset(std::shared_ptr<Widget> w, float d) : inner(w), dy(d) {}
        float getHeight() const override { return inner->getHeight() + dy; }
        void setPosition(float x, float y) override { inner->setPosition(x, y + dy); }
        void draw(sf::RenderWindow& w) override { inner->draw(w); }
        bool handleEvent(const sf::Event& ev, const sf::RenderWindow& win) override { return inner->handleEvent(ev, win); }
        bool isMouseOver(const sf::RenderWindow& win) const override { return inner->isMouseOver(win); }
        void updateStyle() override { inner->updateStyle(); }
    };

    bool pendingSearchSwitch = false;
    uint64_t searchVersionSeen = 0;

    // The receiver / tune lists from what main.cpp reported (rebuilt only when it changes,
    // never while one of them is open)
    void refreshTrackChoices() {
        if (!ddRx || !ddTarget || ddRx->isOpen || ddTarget->isOpen) return;
        const SatTracker::RxChoice* sel = nullptr;
        for (const auto& r : trk.receivers) if (r.id == trk.rxId) sel = &r;
        std::string key = std::to_string(trk.rxId) + "|" + std::to_string((int)trk.target) + "|" + trk.decoderName + "|";
        for (const auto& r : trk.receivers) {
            key += std::to_string(r.id) + ":" + r.name + ";";
            if (&r == sel) for (const auto& d : r.decoders) key += d + ",";
        }
        if (key == trackChoicesKey) return;
        trackChoicesKey = key;

        std::vector<std::string> rxOpts;
        rxOptIds.clear();
        int rxSel = 0;
        for (const auto& r : trk.receivers) {
            if (r.id == trk.rxId) rxSel = (int)rxOptIds.size();
            rxOptIds.push_back(r.id);
            rxOpts.push_back(r.name);
        }
        if (!sel) {   // the chosen receiver's tab was closed
            rxSel = (int)rxOptIds.size();
            rxOptIds.push_back(trk.rxId);
            rxOpts.push_back("RX" + std::to_string(trk.rxId) + " (closed)");
        }
        ddRx->setOptions(rxOpts);
        ddRx->setSelection(rxSel);

        std::vector<std::string> tOpts = {"Tuner (VFO, what you hear)", "Decoder channel (automatic)"};
        targetOptDec = {std::string(), std::string()};
        int tSel = trk.target == TrackTarget::Tuner ? 0 : 1;
        bool haveChosen = trk.decoderName.empty();
        if (sel) for (const auto& d : sel->decoders) {
            if (trk.target == TrackTarget::Decoder && d == trk.decoderName) { tSel = (int)tOpts.size(); haveChosen = true; }
            tOpts.push_back("Decoder channel: " + d);
            targetOptDec.push_back(d);
        }
        if (!haveChosen) {   // a decoder chosen before that is off now
            if (trk.target == TrackTarget::Decoder) tSel = (int)tOpts.size();
            tOpts.push_back("Decoder channel: " + trk.decoderName + " (off)");
            targetOptDec.push_back(trk.decoderName);
        }
        ddTarget->setOptions(tOpts);
        ddTarget->setSelection(tSel);
    }

    void syncPanelFromState() {
        for (size_t i = 0; i < satGroups().size(); i++) if (satGroups()[i].id == groupId) ddGroup->setSelection((int)i);
        chkNight->checked = showNight; chkGrid->checked = showGrid; chkTracks->checked = showTracks; chkLabels->checked = allLabels;
        ddFootprints->setSelection(footprintMode);
        trackChoicesKey.clear();
        refreshTrackChoices();
        chkSetMode->checked = trk.setDemodMode;
        slMinEl->setValueSilent((float)trk.minElDeg);
        slMinEl->onChange((float)trk.minElDeg);
        fillQthInputs();
        ddTime->setSelection((int)timeSource);
        chkTimeLocal->checked = timeInputLocal;
        ddLayer->setSelection(baseLayer);
        chkPlaceNames->checked = placeNames;
        fillTimeInput();
    }

    // The date field shows: Recording = the start of the recording, Manual / Live = the map time
    double shownUnix() const {
        if (timeSource == TimeSource::Recording) return recStart.unix;
        return jdToUnix(simJd());
    }
    void fillTimeInput() { dirty = true; }   // the spinner reads shownUnix() when drawn

    // Sets the date shown in the spinner
    void applyUnix(double u) {
        timeMsg.clear();
        if (timeSource == TimeSource::Recording) {
            if (recPath.empty()) { timeMsg = "Open an IQ file first (source: File)"; return; }
            recStart.unix = u;
            recStart.how = "set by you";
            recStart.guess = false;
            recStartUser = true;
        } else {
            // Live + a change = the manual mode
            if (timeSource != TimeSource::Manual) {
                timeSource = TimeSource::Manual;
                ddTime->setSelection((int)timeSource);
            }
            manualUnix = u;
            manualSetAt = std::chrono::steady_clock::now();
        }
        simOffsetSec = 0;
        refreshTime();
        dirty = true;
    }

    // field 0 year, 1 month, 2 day, 3 hour, 4 minute, 5 second
    float dtWheelAcc = 0.0f;
    struct DtField { float x, w; };
    std::vector<DtField> dtLayout(float width) const {
        const float gap = 4.0f, unit = (width - 5 * gap) / 7.0f;   // the year is twice as wide
        std::vector<DtField> v;
        float x = 0;
        for (int i = 0; i < 6; i++) { const float w = i == 0 ? unit * 2 : unit; v.push_back({x, w}); x += w + gap; }
        return v;
    }
    std::tm brokenDown(double u) const {
        const std::time_t t = (std::time_t)std::floor(u);
        std::tm tm{};
#ifdef _WIN32
        if (timeInputLocal) localtime_s(&tm, &t); else gmtime_s(&tm, &t);
#else
        if (timeInputLocal) localtime_r(&t, &tm); else gmtime_r(&t, &tm);
#endif
        return tm;
    }
    void stepField(int field, int delta) {
        double u = shownUnix();
        if (u <= 0) u = jdToUnix(nowJd());
        const double frac = u - std::floor(u);
        if (field >= 2) {
            static const double sec[] = {0, 0, 86400, 3600, 60, 1};
            applyUnix(u + delta * sec[field]);
            return;
        }
        std::tm tm = brokenDown(u);
        int y = tm.tm_year + 1900, mo = tm.tm_mon + 1;
        if (field == 0) y += delta;
        else { mo += delta; while (mo > 12) { mo -= 12; y++; } while (mo < 1) { mo += 12; y--; } }
        static const int mdays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
        const int dmax = mdays[mo - 1] + (mo == 2 && leap ? 1 : 0);
        const double nu = civilToUnix(y, mo, std::min(tm.tm_mday, dmax), tm.tm_hour, tm.tm_min, tm.tm_sec, !timeInputLocal);
        if (nu > 0) applyUnix(nu + frac);
    }
    bool dateTimeEvent(const sf::Event& ev, sf::Vector2f l) {
        const auto lay = dtLayout(wDateTime->w);
        int field = -1;
        for (int i = 0; i < 6; i++) if (l.x >= lay[i].x && l.x < lay[i].x + lay[i].w) field = i;
        if (field < 0) return true;
        if (const auto* mb = ev.getIf<sf::Event::MouseButtonPressed>()) {
            if (mb->button == sf::Mouse::Button::Left) stepField(field, l.y < wDateTime->getHeight() / 2 ? 1 : -1);
            else if (mb->button == sf::Mouse::Button::Right) stepField(field, l.y < wDateTime->getHeight() / 2 ? 10 : -10);
            return true;
        }
        if (const auto* sc = ev.getIf<sf::Event::MouseWheelScrolled>()) {
            // a trackpad sends many fractional steps: add them up
            dtWheelAcc += sc->delta;
            while (dtWheelAcc >= 1.0f) { stepField(field, 1); dtWheelAcc -= 1.0f; }
            while (dtWheelAcc <= -1.0f) { stepField(field, -1); dtWheelAcc += 1.0f; }
            return true;
        }
        return true;
    }
    void drawDateTime(sf::RenderWindow& w, const sf::FloatRect& r) {
        const double u = shownUnix();
        const bool known = u > 0;
        const std::tm tm = brokenDown(known ? u : jdToUnix(nowJd()));
        char v[6][8];
        std::snprintf(v[0], 8, "%04d", tm.tm_year + 1900); std::snprintf(v[1], 8, "%02d", tm.tm_mon + 1);
        std::snprintf(v[2], 8, "%02d", tm.tm_mday);         std::snprintf(v[3], 8, "%02d", tm.tm_hour);
        std::snprintf(v[4], 8, "%02d", tm.tm_min);          std::snprintf(v[5], 8, "%02d", tm.tm_sec);
        static const char* sep[] = {"-", "-", "", ":", ":", ""};
        const auto lay = dtLayout(r.size.x);
        const sf::Vector2f m = w.mapPixelToCoords(sf::Mouse::getPosition(w));
        const float h = r.size.y, mid = r.position.y + h / 2;
        for (int i = 0; i < 6; i++) {
            const float x = r.position.x + lay[i].x, fw = lay[i].w;
            const sf::FloatRect fr({x, r.position.y}, {fw, h});
            const bool hov = fr.contains(m);
            sf::RectangleShape box({fw, h});
            box.setPosition({x, r.position.y});
            box.setFillColor(hov ? sf::Color(48, 48, 56) : sf::Color(32, 32, 38));
            box.setOutlineColor(hov ? Theme::Accent : sf::Color(70, 70, 78));
            box.setOutlineThickness(1.f);
            w.draw(box);
            // arrows: the half under the mouse lights up
            const bool upHov = hov && m.y < mid, dnHov = hov && m.y >= mid;
            sf::ConvexShape up(3), dn(3);
            const float cx = x + fw / 2;
            up.setPoint(0, {cx - 5, r.position.y + 9}); up.setPoint(1, {cx + 5, r.position.y + 9}); up.setPoint(2, {cx, r.position.y + 3});
            dn.setPoint(0, {cx - 5, r.position.y + h - 9}); dn.setPoint(1, {cx + 5, r.position.y + h - 9}); dn.setPoint(2, {cx, r.position.y + h - 3});
            up.setFillColor(upHov ? Theme::Accent : sf::Color(120, 120, 130));
            dn.setFillColor(dnHov ? Theme::Accent : sf::Color(120, 120, 130));
            w.draw(up); w.draw(dn);
            drawText(w, font, known ? v[i] : "--", cx, mid - 8, 13, known ? sf::Color(235, 235, 240) : sf::Color(130, 130, 140), true, 1);
            if (*sep[i]) drawText(w, font, sep[i], x + fw + 2, mid - 8, 13, sf::Color(150, 150, 160), false, 1);
        }
    }
    std::string timeMsg;

    void drawTimeInfo(sf::RenderWindow& w, const sf::FloatRect& r) {
        std::vector<std::pair<std::string, sf::Color>> lines;
        const sf::Color grey(140, 140, 150), warn(255, 150, 80), ok(90, 220, 120);
        if (!timeMsg.empty()) lines.push_back({timeMsg, warn});
        lines.push_back({std::string("Click the top / bottom of a field or use the wheel (right click: 10 steps). ") +
                         (timeInputLocal ? "Local time." : "UTC."), grey});
        switch (timeSource) {
        case TimeSource::Live:
            lines.push_back({"Real time. Type a date above for another moment.", grey});
            break;
        case TimeSource::Manual:
            lines.push_back({"Manual time, runs on from the date set. Tracking uses it too.", grey});
            break;
        case TimeSource::Recording:
            if (!recPlaying) lines.push_back({"No IQ file is playing (source: File) - real time is used.", warn});
            else if (recStart.unix <= 0) lines.push_back({"Start of the recording unknown - type it above.", warn});
            else {
                const std::string fn = std::filesystem::path(recPath).filename().string();
                lines.push_back({"Start " + recStart.how + (recStartUser ? "" : ", fix it above if wrong"), recStart.guess ? warn : ok});
                lines.push_back({fn + "  " + formatDuration(recProgress * recDuration) + " / " + formatDuration(recDuration), grey});
            }
            break;
        }
        // TLEs are accurate for days, not months: say so for old recordings
        if (const Satellite* s = cat.get(selected)) {
            const double d = std::fabs(baseJd() - s->epochJd);
            if (d > 7.0) lines.push_back({"TLE is " + std::to_string((int)d) + " days from this time - paste an older TLE for exact passes", warn});
        }
        float y = r.position.y;
        for (const auto& l : lines)
            for (const auto& t : wrap(l.first, 11, r.size.x)) { drawText(w, font, t, r.position.x, y, 11, l.second); y += 14.0f; }
    }

    void fillQthInputs() {
        char b[32];
        std::snprintf(b, sizeof(b), "%.4f", trk.obs.latDeg); inLat->input->setText(b);
        std::snprintf(b, sizeof(b), "%.4f", trk.obs.lonDeg); inLon->input->setText(b);
        std::snprintf(b, sizeof(b), "%.0f", trk.obs.altM); inAlt->input->setText(b);
        inLoc->input->setText(toMaidenhead(trk.obs.latDeg, trk.obs.lonDeg));
    }

    void applyQthInputs() {
        try {
            const double la = std::stod(inLat->input->getText());
            const double lo = std::stod(inLon->input->getText());
            const double al = inAlt->input->getText().empty() ? 0.0 : std::stod(inAlt->input->getText());
            if (la < -90 || la > 90 || lo < -180 || lo > 180) throw 1;
            // a locator typed by hand wins over unchanged coordinates
            double lla, llo;
            const std::string loc = inLoc->input->getText();
            if (fromMaidenhead(loc, lla, llo) && loc != toMaidenhead(trk.obs.latDeg, trk.obs.lonDeg) &&
                std::fabs(la - trk.obs.latDeg) < 1e-6 && std::fabs(lo - trk.obs.lonDeg) < 1e-6) {
                trk.obs.latDeg = lla; trk.obs.lonDeg = llo;
            } else { trk.obs.latDeg = la; trk.obs.lonDeg = lo; }
            trk.obs.altM = al;
        } catch (...) {
            cat.status = "Station: enter latitude / longitude in degrees (e.g. 52.2297, 21.0122)";
            cat.statusError = true;
        }
        fillQthInputs();
    }

    void applyLocator(const std::string& s) {
        double la, lo;
        if (fromMaidenhead(s, la, lo)) { trk.obs.latDeg = la; trk.obs.lonDeg = lo; }
        else { cat.status = "Locator: 4 or 6 characters, e.g. KO02 or KO02mf"; cat.statusError = true; }
        fillQthInputs();
    }

    void addCustom(const std::string& text, const char* from) {
        auto added = cat.addCustomTle(text);
        if (added.empty()) {
            cat.status = std::string("No TLE found in the ") + from + " (2 or 3 lines per satellite: name, line 1, line 2)";
            cat.statusError = true;
            return;
        }
        cat.status = "Added " + std::to_string(added.size()) + " satellite(s) to My TLEs";
        cat.statusError = false;
        groupId = "custom";
        for (size_t i = 0; i < satGroups().size(); i++) if (satGroups()[i].id == groupId) ddGroup->setSelection((int)i);
        txtSearch->setText("");
        selectSat(added.front());
        listSortedAt = 0;
    }

    // Picked in the list while off screen: bring it into view
    void centreOnIfHidden(int id) {
        const Satellite* s = cat.get(id);
        if (!s || !win) return;
        const LookAngles la = look(*s, trk.obs, simJd());
        if (!la.ok) return;
        auto r = mapRect();
        r.position.x += 40; r.position.y += 40; r.size.x -= 80; r.size.y -= 80;
        if (r.contains(latLonToUi(la.subLatDeg, la.subLonDeg))) return;
        camX = lonToX(la.subLonDeg);
        camY = latToY(la.subLatDeg);
        clampCamera();
    }

    void selectSat(int id) {
        if (id == selected) return;
        selected = id;
        selPassesFor = -1;
        txFor = -1;
        dirty = true;
    }

    // ------------------------------------------------------------------
    //  Radio
    // ------------------------------------------------------------------
    Transmitter currentTx() const {
        if (txIndex >= 0 && txIndex < (int)txList.size()) return txList[txIndex];
        Transmitter t; t.desc = "Custom frequency"; t.mode = "FM"; t.downHz = chosenHz;
        return t;
    }

    static std::string mhz(long long hz, int decimals = 4) {
        char b[32]; std::snprintf(b, sizeof(b), "%.*f", decimals, hz / 1e6); return b;
    }

    void refreshTransmitters() {
        if (!selected) return;
        const auto& list = cat.transmitters(selected);
        if (txFor == selected && txVersionSeen == cat.txVersion) return;
        const bool sameSat = (txFor == selected);
        txFor = selected;
        txVersionSeen = cat.txVersion;
        const long long keepHz = chosenHz;
        txList = list;
        std::vector<std::string> opts;
        for (const auto& t : txList) {
            std::string s = mhz(t.downHz, 3) + " " + t.mode + " " + t.desc;
            if (!t.alive) s = "(off) " + s;
            opts.push_back(s);
        }
        opts.push_back("Custom frequency...");
        ddTx->setOptions(opts);
        // keep the choice when the list was only refreshed; otherwise the first working one
        int pick = (int)txList.size();
        if (sameSat) {
            for (size_t i = 0; i < txList.size(); i++) if (txList[i].downHz == keepHz) { pick = (int)i; break; }
        } else if (trk.norad == selected && trk.downlinkHz > 0) {
            for (size_t i = 0; i < txList.size(); i++) if (txList[i].downHz == trk.downlinkHz) { pick = (int)i; break; }
            if (pick == (int)txList.size()) chosenHz = trk.downlinkHz;
        } else if (!txList.empty()) pick = 0;
        else chosenHz = 0;
        ddTx->setSelection(pick);
        pickTransmitter(pick);
    }

    void pickTransmitter(int i) {
        txIndex = i;
        if (i >= 0 && i < (int)txList.size()) chosenHz = txList[i].downHz;
        txtFreq->setText(chosenHz > 0 ? mhz(chosenHz, 5) : "");
    }

    void useTypedFreq() {
        try {
            double v = std::stod(txtFreq->getText());
            if (v > 10000) v /= 1e6;   // typed in Hz
            if (v < 0.01 || v > 12000) throw 1;
            chosenHz = (long long)std::llround(v * 1e6);
            txIndex = -1;
            for (size_t i = 0; i < txList.size(); i++) if (txList[i].downHz == chosenHz) txIndex = (int)i;
            ddTx->setSelection(txIndex >= 0 ? txIndex : (int)txList.size());
            if (trk.active() && trk.norad == selected) startTracking(trk.mode);
        } catch (...) {
            cat.status = "Frequency: type it in MHz, e.g. 145.800";
            cat.statusError = true;
        }
    }

    void startTracking(TrackMode m) {
        const Satellite* s = cat.get(selected);
        if (!s) { cat.status = "Select a satellite first"; cat.statusError = true; return; }
        if (chosenHz <= 0) { cat.status = "Pick a transmitter or type a frequency first"; cat.statusError = true; return; }
        trk.start(selected, s->name, currentTx(), chosenHz, m);
    }

    // ------------------------------------------------------------------
    //  Events
    // ------------------------------------------------------------------
    void applyView() {
        lastSize = win->getSize();
        float s = uiScale ? uiScale() : 1.0f;
        if (s < 0.5f) s = 0.5f;
        logical = {(float)lastSize.x / s, (float)lastSize.y / s};
        win->setView(sf::View(sf::FloatRect({0.f, 0.f}, logical)));
        panel.setGeometry(0, 0, logical.y);
        panel.recalculateLayout();
        dirty = true;
    }

    sf::FloatRect mapRect() const {
        return sf::FloatRect({PANEL_W, TOOLBAR_H}, {std::max(50.0f, logical.x - PANEL_W), std::max(50.0f, logical.y - TOOLBAR_H - STATUS_H)});
    }

    void closeCleanup() {
        for (auto& d : allDropdowns) {
            if (Dropdown::currentActive == d.get()) Dropdown::currentActive = nullptr;
            d->isOpen = false;
        }
        if (TextInput::currentFocus == txtSearch.get() || TextInput::currentFocus == txtFreq.get() ||
            TextInput::currentFocus == inLat->input.get() || TextInput::currentFocus == inLon->input.get() ||
            TextInput::currentFocus == inAlt->input.get() || TextInput::currentFocus == inLoc->input.get())
            TextInput::currentFocus->blur();
        win.reset();
        dragging = false;
        pickQth = false;
        btnPick->setActive(false);
    }

    void handleEvents() {
        while (const std::optional<sf::Event> ev = win->pollEvent()) {
            dirty = true;
            if (ev->is<sf::Event::Closed>()) { win->close(); return; }
            if (ev->is<sf::Event::Resized>()) { applyView(); continue; }
            if (const auto* mm = ev->getIf<sf::Event::MouseMoved>()) mouseUi = win->mapPixelToCoords(mm->position);
            if (ev->is<sf::Event::MouseLeft>()) mouseUi = {-1, -1};

            // the list scrolls itself (the panel would scroll as a whole)
            if (const auto* sc = ev->getIf<sf::Event::MouseWheelScrolled>()) {
                const sf::Vector2f m = win->mapPixelToCoords(sc->position);
                if (!Dropdown::currentActive && sc->wheel == sf::Mouse::Wheel::Vertical && wList->rect.contains(m) && wList->getHeight() > 0) {
                    listScroll = std::max(0.0f, listScroll - sc->delta * 40.0f);
                    continue;
                }
                // zoom by the actual amount: a mouse wheel notch is 1.0, a trackpad sends many
                // small (and momentum) steps; horizontal trackpad movement is ignored
                if (m.x > PANEL_W && mapRect().contains(m)) {
                    if (sc->wheel == sf::Mouse::Wheel::Vertical && sc->delta != 0.0f)
                        zoomAt(m, std::pow(1.25, std::clamp((double)sc->delta, -3.0, 3.0)));
                    continue;
                }
            }

            if (const auto* mr = ev->getIf<sf::Event::MouseButtonReleased>()) {
                if (mr->button == sf::Mouse::Button::Left && dragging) {
                    dragging = false;
                    if (!dragMoved) mapClick(win->mapPixelToCoords(mr->position));
                    continue;
                }
            }
            if (dragging) {
                if (const auto* mm = ev->getIf<sf::Event::MouseMoved>()) {
                    const sf::Vector2f m = win->mapPixelToCoords(mm->position);
                    if (std::hypot(m.x - dragStart.x, m.y - dragStart.y) > 4) dragMoved = true;
                    const double ppw = pixelsPerWorld();
                    camX -= (m.x - dragLast.x) / ppw;
                    camY -= (m.y - dragLast.y) / ppw;
                    dragLast = m;
                    continue;
                }
            }

            if (!Dropdown::currentActive) {
                if (tbM1h.isClicked(*ev, *win) || tbM10.isClicked(*ev, *win) || tbNow.isClicked(*ev, *win) ||
                    tbP10.isClicked(*ev, *win) || tbP1h.isClicked(*ev, *win)) continue;
                bool used = tbWindow.isClicked(*ev, *win);
                for (auto& b : tbKind) used = used || b.isClicked(*ev, *win);
                if (used) continue;
            }

            if (panel.handleEvent(*ev, *win)) continue;

            if (const auto* mb = ev->getIf<sf::Event::MouseButtonPressed>()) {
                const sf::Vector2f m = win->mapPixelToCoords(mb->position);
                if (mb->button == sf::Mouse::Button::Left && mapRect().contains(m)) {
                    dragging = true; dragMoved = false; dragStart = dragLast = m;
                }
                if (mb->button == sf::Mouse::Button::Right && mapRect().contains(m) && hovered) {
                    selectSat(hovered);
                }
            }
            if (const auto* kp = ev->getIf<sf::Event::KeyPressed>()) {
                if (TextInput::currentFocus) continue;
                if (kp->code == sf::Keyboard::Key::Escape) { pickQth = false; btnPick->setActive(false); }
                else if (kp->code == sf::Keyboard::Key::Equal || kp->code == sf::Keyboard::Key::Add) zoomAt(centerOfMap(), 1.25);
                else if (kp->code == sf::Keyboard::Key::Hyphen || kp->code == sf::Keyboard::Key::Subtract) zoomAt(centerOfMap(), 0.8);
            }
        }
    }

    sf::Vector2f centerOfMap() const { auto r = mapRect(); return {r.position.x + r.size.x / 2, r.position.y + r.size.y / 2}; }

    void mapClick(sf::Vector2f m) {
        if (pickQth) {
            double la, lo;
            uiToLatLon(m, la, lo);
            trk.obs.latDeg = std::clamp(la, -89.9, 89.9);
            trk.obs.lonDeg = wrapLon(lo);
            fillQthInputs();
            pickQth = false;
            btnPick->setActive(false);
            return;
        }
        if (hovered) { selectSat(hovered); return; }
        if (!mapLoaded && !mapDownloading) loadMap();   // retry after a failed download
    }

    bool listEvent(const sf::Event& ev, sf::Vector2f l) {
        if (const auto* mb = ev.getIf<sf::Event::MouseButtonPressed>()) {
            if (mb->button != sf::Mouse::Button::Left) return false;
            const int row = (int)((l.y - 18.0f + listScroll) / ROW_H);
            if (l.y < 18.0f || row < 0 || row >= (int)listIds.size()) return true;
            const int id = listIds[row];
            if (l.x < 20.0f) cat.toggleFavorite(id);
            else { selectSat(id); centreOnIfHidden(id); }
            return true;
        }
        return false;
    }

    bool passesEvent(const sf::Event& ev, sf::Vector2f l) {
        if (const auto* mb = ev.getIf<sf::Event::MouseButtonPressed>()) {
            if (mb->button != sf::Mouse::Button::Left) return false;
            const int row = (int)((l.y - 20.0f) / 18.0f);
            if (l.y < 20.0f || row < 0 || row >= (int)selPasses.size()) return true;
            const Pass& p = selPasses[row];
            // jump the map to the middle of that pass; NOW returns to real time
            simOffsetSec = (p.alwaysUp ? 0.0 : (p.tcaJd - baseJd()) * 86400.0);
            return true;
        }
        return false;
    }

    // ------------------------------------------------------------------
    //  Map geometry
    // ------------------------------------------------------------------
    double pixelsPerWorld() const {
        const auto r = mapRect();
        return std::max(r.size.x / WW, r.size.y / WH) * zoom;
    }
    void clampCamera() {
        const auto r = mapRect();
        const double ppw = pixelsPerWorld();
        const double vh = r.size.y / ppw;
        if (vh >= WH) camY = WH / 2;
        else camY = std::clamp(camY, vh / 2, WH - vh / 2);
        camX = std::fmod(camX, WW);
        if (camX < 0) camX += WW;
    }
    void zoomAt(sf::Vector2f m, double f) {
        double wx, wy;
        uiToWorld(m, wx, wy);
        zoom = std::clamp(zoom * f, 1.0, MAX_ZOOM);
        const auto r = mapRect();
        const double ppw = pixelsPerWorld();
        camX = wx - (m.x - (r.position.x + r.size.x / 2)) / ppw;
        camY = wy - (m.y - (r.position.y + r.size.y / 2)) / ppw;
        clampCamera();
    }
    void uiToWorld(sf::Vector2f m, double& wx, double& wy) const {
        const auto r = mapRect();
        const double ppw = pixelsPerWorld();
        wx = camX + (m.x - (r.position.x + r.size.x / 2)) / ppw;
        wy = camY + (m.y - (r.position.y + r.size.y / 2)) / ppw;
    }
    void uiToLatLon(sf::Vector2f m, double& lat, double& lon) const {
        double wx, wy;
        uiToWorld(m, wx, wy);
        lon = wrapLon(wx / 10.0 - 180.0);
        lat = std::clamp(90.0 - wy / 10.0, -90.0, 90.0);
    }
    static double lonToX(double lon) { return (lon + 180.0) * 10.0; }
    static double latToY(double lat) { return (90.0 - lat) * 10.0; }
    // Screen position of a lat/lon (the copy of the world nearest to the camera)
    sf::Vector2f latLonToUi(double lat, double lon) const {
        const auto r = mapRect();
        const double ppw = pixelsPerWorld();
        double x = lonToX(wrapLon(lon));
        x += WW * std::round((camX - x) / WW);
        return {(float)(r.position.x + r.size.x / 2 + (x - camX) * ppw), (float)(r.position.y + r.size.y / 2 + (latToY(lat) - camY) * ppw)};
    }
    sf::View worldView() const {
        const auto r = mapRect();
        const double ppw = pixelsPerWorld();
        sf::View v({(float)camX, (float)camY}, {(float)(r.size.x / ppw), (float)(r.size.y / ppw)});
        v.setViewport(sf::FloatRect({r.position.x / logical.x, r.position.y / logical.y}, {r.size.x / logical.x, r.size.y / logical.y}));
        return v;
    }
    // Draw something given in world units around x in [0, WW) on every visible copy
    void drawWrapped(const sf::Drawable& d, double spanLeft = 0, double spanRight = WW) {
        const auto r = mapRect();
        const double vw = r.size.x / pixelsPerWorld();
        const double left = camX - vw / 2, right = camX + vw / 2;
        for (int k = -2; k <= 2; k++) {
            if (spanRight + k * WW < left || spanLeft + k * WW > right) continue;
            sf::RenderStates st;
            st.transform.translate({(float)(k * WW), 0.f});
            win->draw(d, st);
        }
    }

    // ------------------------------------------------------------------
    //  Map picture
    // ------------------------------------------------------------------
    // ------------------------------------------------------------------
    //  Detail tiles
    // ------------------------------------------------------------------
    // One tile layer at the level that matches the zoom; missing tiles show a scaled
    // part of an ancestor that is already loaded (or the world picture below).
    void drawTileLayer(int layer, int minZ) {
        const auto r = mapRect();
        const double ppw = pixelsPerWorld();
        const double ppd = ppw * 10.0;   // screen units per degree
        const float s = uiScale ? std::max(0.5f, uiScale()) : 1.0f;
        const TileSource& ts = tileSources()[layer];
        // device pixels per degree decide the level (crisp on high-DPI screens)
        int z = (int)std::ceil(std::log2(ppd * s * 180.0 / 256.0) - 0.25);
        if (z < minZ) return;
        z = std::clamp(z, 0, ts.maxZ);
        const int ny = 1 << z, nx = 2 << z;
        const double tdeg = 180.0 / ny;
        double wx0, wy0, wx1, wy1;
        uiToWorld(r.position, wx0, wy0);
        uiToWorld(r.position + r.size, wx1, wy1);
        const int tx0 = (int)std::floor(wx0 / 10.0 / tdeg), tx1 = (int)std::floor(wx1 / 10.0 / tdeg);
        const int ty0 = std::clamp((int)std::floor(wy0 / 10.0 / tdeg), 0, ny - 1);
        const int ty1 = std::clamp((int)std::floor(wy1 / 10.0 / tdeg), 0, ny - 1);
        if (tx1 - tx0 > 80 || ty1 - ty0 > 60) return;   // (cannot happen at these levels, but never hang)

        sf::View clip(r);
        clip.setViewport(sf::FloatRect({r.position.x / logical.x, r.position.y / logical.y}, {r.size.x / logical.x, r.size.y / logical.y}));
        win->setView(clip);
        const double ccx = (tx0 + tx1) / 2.0, ccy = (ty0 + ty1) / 2.0;
        const double ox = r.position.x + r.size.x / 2.0, oy = r.position.y + r.size.y / 2.0;
        for (int ty = ty0; ty <= ty1; ty++) {
            for (int tx = tx0; tx <= tx1; tx++) {
                const int wx = ((tx % nx) + nx) % nx;
                const TileKey k{layer, z, ty, wx};
                const double prio = (tx - ccx) * (tx - ccx) + (ty - ccy) * (ty - ccy);
                const sf::Texture* t = tiles->get(k, prio);
                float u0 = 0, v0 = 0, u1 = 256, v1 = 256;
                if (!t) {   // a loaded ancestor, scaled up
                    for (int up = 1; up <= std::min(z, 7) && !t; up++) {
                        t = tiles->peek(TileKey{layer, z - up, ty >> up, wx >> up});
                        if (t) {
                            const float sz = 256.0f / (float)(1 << up);
                            u0 = (float)(wx & ((1 << up) - 1)) * sz; v0 = (float)(ty & ((1 << up) - 1)) * sz;
                            u1 = u0 + sz; v1 = v0 + sz;
                        }
                    }
                }
                if (!t) continue;
                const float tw = (float)t->getSize().x / 256.0f, th = (float)t->getSize().y / 256.0f;   // 512 px tiles too
                const float x0 = (float)std::round(ox + (tx * tdeg * 10.0 - camX) * ppw), x1 = (float)std::round(ox + ((tx + 1) * tdeg * 10.0 - camX) * ppw);
                const float y0 = (float)std::round(oy + (ty * tdeg * 10.0 - camY) * ppw), y1 = (float)std::round(oy + ((ty + 1) * tdeg * 10.0 - camY) * ppw);
                sf::Vertex q[4] = {
                    {{x0, y0}, sf::Color::White, {u0 * tw, v0 * th}}, {{x1, y0}, sf::Color::White, {u1 * tw, v0 * th}},
                    {{x0, y1}, sf::Color::White, {u0 * tw, v1 * th}}, {{x1, y1}, sf::Color::White, {u1 * tw, v1 * th}}};
                sf::RenderStates st;
                st.texture = t;
                win->draw(q, 4, sf::PrimitiveType::TriangleStrip, st);
                layersDrawn.insert(layer);
            }
        }
    }

    void drawAttribution(const sf::FloatRect& r) {
        if (layersDrawn.empty()) return;
        std::string a;
        for (int l : layersDrawn) {
            if (l == 2 && layersDrawn.count(1)) continue;
            a += (a.empty() ? "" : "  |  ") + tileSources()[l].attribution;
        }
        const float tw = textWidth(font, a, 10);
        sf::RectangleShape bg({tw + 10, 15});
        bg.setPosition({r.position.x + r.size.x - tw - 12, r.position.y + r.size.y - 16});
        bg.setFillColor(sf::Color(0, 0, 0, 150));
        win->draw(bg);
        drawText(*win, font, a, r.position.x + r.size.x - 7, r.position.y + r.size.y - 15, 10, sf::Color(220, 220, 225), false, 2);
    }

    // ------------------------------------------------------------------
    //  Positions heard by the decoders (AIS, APRS, ACARS, ADS-B)
    // ------------------------------------------------------------------
    enum class Icon { Dot, Ship, Plane, Car, Person, House, Tower, Weather, Balloon, Square, Diamond };

    static Icon iconFor(const PosReport& p) {
        switch (p.kind) {
        case PosKind::AIS: return p.symCode == 'B' ? Icon::Square : p.symCode == 'N' ? Icon::Diamond : Icon::Ship;
        case PosKind::ACARS: case PosKind::ADSB: return Icon::Plane;
        case PosKind::HF: return Icon::Tower;
        default: break;
        }
        const std::string n = aprspos::symbolName(p.symTable, p.symCode);
        if (n == "car" || n == "truck" || n == "van" || n == "jeep" || n == "motorcycle" || n == "bicycle" || n == "RV" ||
            n == "bus" || n == "ambulance" || n == "fire truck") return Icon::Car;
        if (n == "boat" || n == "ship") return Icon::Ship;
        if (n == "aircraft" || n == "helicopter") return Icon::Plane;
        if (n == "person") return Icon::Person;
        if (n == "house" || n == "house (yagi)") return Icon::House;
        if (n == "digipeater" || n == "gateway" || n == "repeater" || n == "ARES") return Icon::Tower;
        if (n == "weather") return Icon::Weather;
        if (n == "balloon") return Icon::Balloon;
        return Icon::Dot;
    }
    static sf::Color kindColor(PosKind k) {
        switch (k) {
        case PosKind::AIS: return sf::Color(70, 200, 255);
        case PosKind::APRS: return sf::Color(255, 120, 90);
        case PosKind::ACARS: return sf::Color(215, 150, 255);
        case PosKind::ADSB: return sf::Color(255, 220, 0);
        case PosKind::HF: return sf::Color(120, 235, 140);
        default: return sf::Color::White;
        }
    }

    // A filled polygon (convex parts) rotated by `deg` (clockwise from north) around p
    void drawPoly(sf::Vector2f p, std::initializer_list<sf::Vector2f> pts, double deg, sf::Color fill, float scale) {
        const double a = deg * M_PI / 180.0, c = std::cos(a), s = std::sin(a);
        sf::ConvexShape sh(pts.size());
        size_t i = 0;
        for (const auto& q : pts) {
            sh.setPoint(i++, {p.x + (float)((q.x * c - q.y * s) * scale), p.y + (float)((q.x * s + q.y * c) * scale)});
        }
        sh.setFillColor(fill);
        sh.setOutlineColor(sf::Color(0, 0, 0, 200));
        sh.setOutlineThickness(1.0f);
        win->draw(sh);
    }

    void drawIcon(sf::Vector2f p, Icon ic, double course, sf::Color col, bool hl) {
        const float k = hl ? 1.35f : 1.0f;
        const double crs = course >= 0 ? course : 0.0;
        auto circle = [&](sf::Vector2f c, float rad, sf::Color f) {
            sf::CircleShape cs(rad); cs.setOrigin({rad, rad}); cs.setPosition(c);
            cs.setFillColor(f); cs.setOutlineColor(sf::Color(0, 0, 0, 200)); cs.setOutlineThickness(1.f);
            win->draw(cs);
        };
        switch (ic) {
        case Icon::Ship:
            drawPoly(p, {{0, -9}, {4.5f, -3}, {4.5f, 8}, {-4.5f, 8}, {-4.5f, -3}}, crs, col, k);
            break;
        case Icon::Plane:
            drawPoly(p, {{0, -10}, {1.6f, -7}, {1.6f, 7}, {0, 9}, {-1.6f, 7}, {-1.6f, -7}}, crs, col, k);   // fuselage
            drawPoly(p, {{0, -3}, {10, 2}, {10, 4}, {0, 1}, {-10, 4}, {-10, 2}}, crs, col, k);              // wings
            drawPoly(p, {{0, 5}, {4, 8}, {4, 9.5f}, {-4, 9.5f}, {-4, 8}}, crs, col, k);                      // tail
            break;
        case Icon::Car:
            drawPoly(p, {{-4, -7}, {4, -7}, {4, 7}, {-4, 7}}, crs, col, k);
            drawPoly(p, {{-3, -4}, {3, -4}, {3, -1}, {-3, -1}}, crs, sf::Color(30, 30, 40, 220), k);   // windscreen
            break;
        case Icon::Person:
            circle({p.x, p.y - 5 * k}, 2.8f * k, col);
            drawPoly(p, {{-4, 8}, {4, 8}, {2.5f, -1}, {-2.5f, -1}}, 0, col, k);
            break;
        case Icon::House:
            drawPoly(p, {{-6, -1}, {0, -8}, {6, -1}, {6, 6}, {-6, 6}}, 0, col, k);
            break;
        case Icon::Tower:
            drawPoly(p, {{0, -9}, {5, 8}, {-5, 8}}, 0, col, k);
            circle({p.x, p.y - 9 * k}, 2.5f * k, sf::Color::White);
            break;
        case Icon::Weather:
            circle(p, 6.5f * k, col);
            drawText(*win, font, "W", p.x, p.y - 7 * k, 10, sf::Color(20, 20, 30), true, 1);
            break;
        case Icon::Balloon:
            circle({p.x, p.y - 3 * k}, 5.5f * k, col);
            drawPoly(p, {{-0.6f, 2}, {0.6f, 2}, {0.6f, 9}, {-0.6f, 9}}, 0, col, k);
            break;
        case Icon::Square:
            drawPoly(p, {{-5, -5}, {5, -5}, {5, 5}, {-5, 5}}, 0, col, k);
            break;
        case Icon::Diamond:
            drawPoly(p, {{0, -7}, {6, 0}, {0, 7}, {-6, 0}}, 0, col, k);
            break;
        default:
            circle(p, 5.0f * k, col);
            break;
        }
        // direction tick for icons that do not turn themselves
        if (course >= 0 && ic != Icon::Ship && ic != Icon::Plane && ic != Icon::Car) {
            const double a = course * M_PI / 180.0;
            sf::Vertex l[2] = {{p, sf::Color(255, 255, 255, 200)},
                               {{p.x + (float)(std::sin(a) * 14 * k), p.y - (float)(std::cos(a) * 14 * k)}, sf::Color(255, 255, 255, 200)}};
            win->draw(l, 2, sf::PrimitiveType::Lines);
        }
    }

    // Where the object was at the map's time: its last report not after that moment
    static bool positionAt(const PosTrack& t, double nowU, double& lat, double& lon, double& when) {
        for (auto it = t.trail.rbegin(); it != t.trail.rend(); ++it) {
            if (it->unix <= nowU + 5.0) { lat = it->lat; lon = it->lon; when = it->unix; return true; }
        }
        return false;
    }

    void drawPositions(const sf::FloatRect& r) {
        posTracks = positionBus().snapshot();
        hoveredPos = -1;
        const double nowU = jdToUnix(simJd());
        const double span = POS_WINDOWS[posWindowIdx].first;
        const double ppd = pixelsPerWorld() * 10.0;
        // labels only where they fit: few objects, or zoomed in
        int visible = 0;
        for (const auto& t : posTracks) {
            double la, lo, wh;
            if (showKind[(int)t.last.kind] && positionAt(t, nowU, la, lo, wh) && nowU - wh <= span && r.contains(latLonToUi(la, lo))) visible++;
        }
        const bool labels = visible <= 40 || ppd > 300.0;
        float best = 12.0f;
        for (size_t i = 0; i < posTracks.size(); i++) {
            const PosTrack& t = posTracks[i];
            if (!showKind[(int)t.last.kind]) continue;
            double la, lo, wh;
            if (!positionAt(t, nowU, la, lo, wh) || nowU - wh > span) continue;
            const sf::Vector2f p = latLonToUi(la, lo);
            const sf::Color col = kindColor(t.last.kind);
            // trail inside the time window
            sf::VertexArray tr(sf::PrimitiveType::LineStrip);
            for (const auto& pt : t.trail) {
                if (pt.unix > nowU + 5.0 || nowU - pt.unix > span) continue;
                tr.append({latLonToUi(pt.lat, pt.lon), sf::Color(col.r, col.g, col.b, 130)});
            }
            if (tr.getVertexCount() > 1) win->draw(tr);
            if (!r.contains(p)) continue;
            const float d = std::hypot(p.x - mouseUi.x, p.y - mouseUi.y);
            if (d < best && r.contains(mouseUi)) { best = d; hoveredPos = (int)i; hoveredPosUi = p; }
            // old reports fade out
            const double age = std::max(0.0, nowU - wh);
            const sf::Color c(col.r, col.g, col.b, (std::uint8_t)(age > 1800 ? 150 : 255));
            drawIcon(p, iconFor(t.last), t.last.courseDeg, c, false);
            if (labels) drawText(*win, font, t.last.label, p.x + 9, p.y - 7, 10, sf::Color(235, 235, 240));
        }
        if (hoveredPos >= 0) {
            const PosTrack& t = posTracks[hoveredPos];
            drawIcon(hoveredPosUi, iconFor(t.last), t.last.courseDeg, kindColor(t.last.kind), true);
        }
    }

    void drawPositionTooltip(const sf::FloatRect& r) {
        const PosTrack& t = posTracks[hoveredPos];
        const PosReport& p = t.last;
        const double nowU = jdToUnix(simJd());
        double la = p.lat, lo = p.lon, wh = p.unix;
        positionAt(t, nowU, la, lo, wh);
        std::string s = std::string(posKindName(p.kind)) + "  " + p.label;
        if (p.id != p.label && p.label.find(p.id) == std::string::npos) s += "  [" + p.id + "]";
        if (!p.info.empty()) s += "\n" + p.info;
        if (p.kind == PosKind::APRS) {
            const std::string sym = aprspos::symbolName(p.symTable, p.symCode);
            if (!sym.empty()) s += "\nSymbol: " + sym;
        }
        char b[160];
        std::string mv;
        if (p.speedKn >= 0) { std::snprintf(b, sizeof(b), "%.1f kn", p.speedKn); mv += b; }
        if (p.courseDeg >= 0) { std::snprintf(b, sizeof(b), "%s%.0f\xC2\xB0", mv.empty() ? "" : "  course ", p.courseDeg); mv += b; }
        if (p.altM > -1e8) { std::snprintf(b, sizeof(b), "  alt %.0f m", p.altM); mv += b; }
        if (!mv.empty()) s += "\n" + mv;
        std::snprintf(b, sizeof(b), "\n%.5f, %.5f   heard %s ago (%d reports)", la, lo, formatDuration(std::max(0.0, nowU - wh)).c_str(), t.count);
        s += b;
        sf::Text tx = makeText(font, s, mouseUi.x + 14, mouseUi.y + 14, 11, sf::Color::White);
        sf::FloatRect bb = tx.getGlobalBounds();
        if (bb.position.x + bb.size.x + 8 > r.position.x + r.size.x) tx.move({-(bb.size.x + 36), 0.f});
        if (bb.position.y + bb.size.y + 8 > r.position.y + r.size.y) tx.move({0.f, -(bb.size.y + 30)});
        bb = tx.getGlobalBounds();
        sf::RectangleShape bg({bb.size.x + 12, bb.size.y + 10});
        bg.setPosition({bb.position.x - 6, bb.position.y - 5});
        bg.setFillColor(sf::Color(20, 20, 26, 235)); bg.setOutlineColor(kindColor(p.kind)); bg.setOutlineThickness(1.f);
        win->draw(bg); win->draw(tx);
    }

    // The picture on disk (true when it is shown now)
    bool loadMapFile() {
        mapFileSize = fileSize(cat.mapFilePath());
        sf::Image img;
        if (mapFileSize <= 0 || !img.loadFromFile(cat.mapFilePath())) return false;
        setMapImage(img);
        if (mapLoaded) mapStatus.clear();
        dirty = true;
        return mapLoaded;
    }
    void loadMap() {
        if (loadMapFile()) return;
        mapDownloading = true;
        mapStatus = "Downloading the world map (about 3 MB)...";
        cat.downloadMap([this](bool ok, const std::string& why) {
            mapDownloading = false;
            if (ok && loadMapFile()) return;
            mapStatus = std::string(ok ? "The downloaded world map could not be opened." : "World map not available (" + why + ").") +
                        "\nClick the map to try again, or save a 2:1 world map (JPEG / PNG) as\n" + cat.mapFilePath();
            dirty = true;
        });
    }
    static long long fileSize(const std::string& p) {
        std::ifstream f(p, std::ios::binary | std::ios::ate);
        return f.is_open() ? (long long)f.tellg() : -1;
    }
    void setMapImage(sf::Image& img) {
        const unsigned maxS = std::min(sf::Texture::getMaximumSize(), 8192u);
        // halve until the GPU accepts it (box filter)
        while (img.getSize().x > maxS || img.getSize().y > maxS) {
            const sf::Vector2u s = img.getSize();
            const sf::Vector2u n{s.x / 2, s.y / 2};
            std::vector<std::uint8_t> px((size_t)n.x * n.y * 4);
            const std::uint8_t* src = img.getPixelsPtr();
            for (unsigned y = 0; y < n.y; y++)
                for (unsigned x = 0; x < n.x; x++)
                    for (int c = 0; c < 4; c++) {
                        const size_t a = ((size_t)(2 * y) * s.x + 2 * x) * 4 + c, b = a + (size_t)s.x * 4;
                        px[((size_t)y * n.x + x) * 4 + c] = (std::uint8_t)((src[a] + src[a + 4] + src[b] + src[b + 4] + 2) / 4);
                    }
            img = sf::Image(n, px.data());
        }
        if (mapTex.loadFromImage(img)) {
            mapTex.setSmooth(true);
            (void)mapTex.generateMipmap();
            mapLoaded = true;
        }
    }

    void buildNight(double jd) {
        const SunPos sun = sunPosition(jd);
        night.clear();
        const int stepLon = 4, stepLat = 3;
        auto shade = [&](double lat, double lon) {
            const double el = sunElevationAt(sun, lat, lon);
            // full dark below -12 deg (nautical twilight), smooth across the terminator
            const double a = std::clamp((0.8 - el) / 12.8, 0.0, 1.0);
            return sf::Color(0, 4, 20, (std::uint8_t)(a * 150));
        };
        for (int lat = -90; lat < 90; lat += stepLat)
            for (int lon = -180; lon < 180; lon += stepLon) {
                const float x0 = (float)lonToX(lon), x1 = (float)lonToX(lon + stepLon);
                const float y0 = (float)latToY(lat), y1 = (float)latToY(lat + stepLat);
                const sf::Color c00 = shade(lat, lon), c10 = shade(lat, lon + stepLon),
                                c01 = shade(lat + stepLat, lon), c11 = shade(lat + stepLat, lon + stepLon);
                if (c00.a == 0 && c10.a == 0 && c01.a == 0 && c11.a == 0) continue;
                night.append({{x0, y0}, c00}); night.append({{x1, y0}, c10}); night.append({{x1, y1}, c11});
                night.append({{x0, y0}, c00}); night.append({{x1, y1}, c11}); night.append({{x0, y1}, c01});
            }
        nightJd = jd;
    }

    // Footprint = all points that see the satellite above `minEl`. Filled exactly, row by row:
    // for each latitude the covered longitudes are lon0 +- dl, from
    //   cos(c) = sin(lat) sin(lat0) + cos(lat) cos(lat0) cos(dl),  c = footprint angle.
    // This also covers footprints that contain a pole (dl = 180 deg rows), with no gaps.
    void buildFootprint(double lat0, double lon0, double ang, sf::Color fill, sf::VertexArray& tris) const {
        if (ang <= 0) return;
        const double x0 = lonToX(wrapLon(lon0));
        const double phi0 = lat0 * DEG2RAD;
        const double cosc = std::cos(ang);
        auto halfWidthDeg = [&](double latDeg) {
            const double phi = latDeg * DEG2RAD;
            const double den = std::cos(phi) * std::cos(phi0);
            if (std::fabs(den) < 1e-9) return (std::fabs(latDeg - lat0) <= ang * RAD2DEG) ? 180.0 : -1.0;
            const double v = (cosc - std::sin(phi) * std::sin(phi0)) / den;
            if (v <= -1.0) return 180.0;
            if (v > 1.0) return -1.0;
            return std::acos(v) * RAD2DEG;
        };
        const double top = std::min(90.0, lat0 + ang * RAD2DEG), bot = std::max(-90.0, lat0 - ang * RAD2DEG);
        const int rows = std::max(8, (int)((top - bot) / 0.75));
        double prevLat = bot, prevW = std::max(0.0, halfWidthDeg(bot));
        for (int i = 1; i <= rows; i++) {
            const double lat = bot + (top - bot) * i / rows;
            const double w = std::max(0.0, halfWidthDeg(lat));
            const float ya = (float)latToY(prevLat), yb = (float)latToY(lat);
            const float xa0 = (float)(x0 - prevW * 10), xa1 = (float)(x0 + prevW * 10);
            const float xb0 = (float)(x0 - w * 10), xb1 = (float)(x0 + w * 10);
            tris.append({{xa0, ya}, fill}); tris.append({{xa1, ya}, fill}); tris.append({{xb1, yb}, fill});
            tris.append({{xa0, ya}, fill}); tris.append({{xb1, yb}, fill}); tris.append({{xb0, yb}, fill});
            prevLat = lat; prevW = w;
        }
    }
    // Outline: points at the footprint distance in every direction, unwrapped around lon0
    void buildFootprintOutline(double lat0, double lon0, double ang, sf::Color c, sf::VertexArray& lines) const {
        if (ang <= 0) return;
        const double baseX = lonToX(wrapLon(lon0));
        double prevX = 0, prevY = 0;
        for (int b = 0; b <= 360; b += 2) {
            double la, lo;
            destinationPoint(lat0, 0.0, ang, b * DEG2RAD, la, lo);   // relative to lon0
            double x = baseX + lo * 10.0, y = latToY(la);
            if (b > 0) {
                // a point that wrapped over a pole: keep the line continuous
                while (x - prevX > WW / 2) x -= WW;
                while (prevX - x > WW / 2) x += WW;
                lines.append({{(float)prevX, (float)prevY}, c});
                lines.append({{(float)x, (float)y}, c});
            }
            prevX = x; prevY = y;
        }
    }

    // ------------------------------------------------------------------
    //  State update (positions, list, passes)
    // ------------------------------------------------------------------
    static constexpr float ROW_H = 19.0f;

    void updateState() {
        const double now = baseJd();
        const double t = simJd();
        // search results arrived: show them
        // (as soon as there is something to show: the satellites already known come first)
        if (pendingSearchSwitch) {
            if (!cat.searchResults.empty() && groupId != "search") {
                groupId = "search";
                for (size_t i = 0; i < satGroups().size(); i++) if (satGroups()[i].id == groupId) ddGroup->setSelection((int)i);
                txtSearch->setText("");
                listSortedAt = 0;
            }
            if (!cat.searching) pendingSearchSwitch = false;
        }
        // no map yet: pick up a picture the user saved by hand
        if (!mapLoaded && !mapDownloading && mapWatchClock.getElapsedTime().asSeconds() > 2.0f) {
            mapWatchClock.restart();
            const long long sz = fileSize(cat.mapFilePath());
            if (sz > 0 && sz != mapFileSize) loadMapFile();
        }
        // station / min elevation changed: passes again
        // left open for days: keep the shown group and the favourites fresh (no-op while fresh)
        if (refreshClock.getElapsedTime().asSeconds() > 600.0f) {
            refreshClock.restart();
            cat.ensureGroup(groupId);
            cat.ensureFavorites();
        }
        const double key = trk.obs.latDeg * 1e4 + trk.obs.lonDeg * 7.0 + trk.obs.altM * 0.001 + trk.minElDeg * 1e7;
        if (key != obsKey) { obsKey = key; nextPass.clear(); selPassesFor = -1; }
        // the map's "now" jumped (time source changed, seek in the recording, manual date): passes again
        if (std::fabs(now - lastPassBaseJd) * 86400.0 > 120.0) { nextPass.clear(); selPassesFor = -1; }
        lastPassBaseJd = now;

        // list contents
        const std::string search = txtSearch->getText();
        if (search != lastSearch || cat.version != listVersion || groupId != listGroup) {
            lastSearch = search; listVersion = cat.version; listGroup = groupId;
            listSortedAt = 0;
            listScroll = 0;
        }
        if (now - listSortedAt > 1.0 / 86400.0) rebuildList(now);

        // passes for the list, a few per frame (time budget)
        {
            auto t0 = std::chrono::steady_clock::now();
            std::vector<int> want = listIds;
            for (int f : cat.favorites) want.push_back(f);
            if (selected) want.push_back(selected);
            for (int id : want) {
                auto it = nextPass.find(id);
                if (it != nextPass.end() && (it->second.alwaysUp || it->second.losJd > now)) continue;
                const Satellite* s = cat.get(id);
                if (!s) continue;
                Pass p = findNextPass(*s, trk.obs, now, 1.5, trk.minElDeg);
                if (!p.valid) p.losJd = now + 30.0 / 1440.0;   // nothing soon: ask again later
                nextPass[id] = p;
                if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(5)) break;
            }
        }
        // passes of the selected satellite
        if (selected && (selPassesFor != selected || (!selPasses.empty() && !selPasses.front().alwaysUp && selPasses.front().losJd < now))) {
            if (const Satellite* s = cat.get(selected)) selPasses = findPasses(*s, trk.obs, now, 8, 3.0, trk.minElDeg);
            else selPasses.clear();
            selPassesFor = selected;
        }

        // positions to draw (simulated time)
        positions.clear();
        std::set<int> drawn;
        auto addPos = [&](int id) {
            if (!id || drawn.count(id)) return;
            const Satellite* s = cat.get(id);
            if (!s) return;
            drawn.insert(id);
            LookAngles la = look(*s, trk.obs, t);
            if (la.ok) positions.push_back({id, la});
        };
        for (int id : listIds) addPos(id);
        for (int id : cat.favorites) addPos(id);
        addPos(selected);
        addPos(trk.norad);

        if (showNight && std::fabs(t - nightJd) * 86400.0 > 20.0) buildNight(t);
        refreshTransmitters();
        clampCamera();

        refreshTrackChoices();
        // button states
        btnFav->setText(cat.favorites.count(selected) ? "Unfavorite" : "Favorite");
        const bool trackingThis = trk.active() && trk.norad == selected;
        btnOff->setActive(!trk.active());
        btnFreq->setActive(trackingThis && trk.mode == TrackMode::Frequency);
        btnDoppler->setActive(trackingThis && trk.mode == TrackMode::Doppler);
        tbNow.setActive(std::fabs(simOffsetSec) < 1.0);
    }

    void rebuildList(double now) {
        listSortedAt = now;
        std::vector<int> ids;
        std::string q = lastSearch;
        for (auto& c : q) c = (char)std::toupper((unsigned char)c);
        while (!q.empty() && q.back() == ' ') q.pop_back();
        if (!q.empty()) {
            for (const auto& kv : cat.sats) {
                std::string n = upper(kv.second.name);
                if (n.find(q) != std::string::npos || std::to_string(kv.first).rfind(q, 0) == 0) ids.push_back(kv.first);
                if (ids.size() >= 400) break;
            }
        } else ids = cat.members(groupId);

        // satellites up now first (highest first), then by next AOS, then the rest by name
        struct Key { int id; int cls; double v; std::string name; };
        std::vector<Key> keys;
        for (int id : ids) {
            const Satellite* s = cat.get(id);
            if (!s) continue;
            const double el = elevationAt(*s, trk.obs, now);
            Key k{id, 2, 0.0, s->name};
            auto it = nextPass.find(id);
            if (el > 0) { k.cls = 0; k.v = -el; }
            else if (it != nextPass.end() && it->second.valid) { k.cls = 1; k.v = it->second.aosJd; }
            keys.push_back(k);
        }
        std::sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) {
            if (a.cls != b.cls) return a.cls < b.cls;
            if (a.cls < 2 && a.v != b.v) return a.v < b.v;
            return a.name < b.name;
        });
        listIds.clear();
        for (auto& k : keys) listIds.push_back(k.id);
        if (!selected && !listIds.empty()) selectSat(listIds.front());
    }

    // ------------------------------------------------------------------
    //  Drawing
    // ------------------------------------------------------------------
    std::vector<std::string> wrap(const std::string& s, unsigned size, float width) const {
        std::vector<std::string> out;
        std::string line, word;
        auto flush = [&]() {
            std::string cand = line.empty() ? word : line + " " + word;
            if (!line.empty() && textWidth(font, cand, size) > width) { out.push_back(line); line = word; }
            else line = cand;
            word.clear();
        };
        for (char c : s) { if (c == ' ') flush(); else word += c; }
        if (!word.empty()) flush();
        if (!line.empty()) out.push_back(line);
        return out;
    }

    static sf::Color ageColor(double days) {
        if (days < 3) return sf::Color(110, 220, 120);
        if (days < 7) return sf::Color(240, 200, 80);
        return sf::Color(255, 110, 90);
    }

    void draw() {
        win->clear(sf::Color(8, 12, 22));
        const auto r = mapRect();
        const double t = simJd();

        // ===== world (zoomable) =====
        win->setView(worldView());
        if (mapLoaded) {
            sf::Sprite sp(mapTex);
            sp.setScale({(float)(WW / mapTex.getSize().x), (float)(WH / mapTex.getSize().y)});
            drawWrapped(sp);
        } else {
            sf::RectangleShape ocean({(float)WW, (float)WH});
            ocean.setFillColor(sf::Color(14, 30, 52));
            drawWrapped(ocean);
        }
        // detail tiles over the world picture when zoomed in
        tiles->beginFrame();
        layersDrawn.clear();
        if (baseLayer == 0) drawTileLayer(0, 4);   // Blue Marble is as sharp below level 4
        else if (baseLayer == 1) drawTileLayer(1, 0);
        win->setView(worldView());
        if (showNight && night.getVertexCount()) drawWrapped(night);
        if (baseLayer == 0 && placeNames) { drawTileLayer(2, 4); win->setView(worldView()); }
        tiles->endFrame();
        if (tiles->loading()) dirty = true;
        if (showGrid || !mapLoaded) {
            sf::VertexArray g(sf::PrimitiveType::Lines);
            const sf::Color gc(255, 255, 255, mapLoaded ? 28 : 50), eq(255, 255, 255, mapLoaded ? 55 : 80);
            for (int lon = -180; lon < 180; lon += 30) {
                g.append({{(float)lonToX(lon), 0.f}, gc}); g.append({{(float)lonToX(lon), (float)WH}, gc});
            }
            for (int lat = -60; lat <= 60; lat += 30) {
                const sf::Color c = lat == 0 ? eq : gc;
                g.append({{0.f, (float)latToY(lat)}, c}); g.append({{(float)WW, (float)latToY(lat)}, c});
            }
            drawWrapped(g);
        }

        // footprints
        const sf::Color acc = Theme::Accent;
        for (const auto& p : positions) {
            const bool isSel = p.id == selected, isFav = cat.favorites.count(p.id) > 0, isTrk = trk.active() && p.id == trk.norad;
            const bool show = isSel || isTrk || footprintMode == 2 || (footprintMode == 1 && isFav);
            if (!show) continue;
            sf::VertexArray outline(sf::PrimitiveType::Lines);
            const double ang0 = footprintAngle(p.la.altKm, 0.0);
            sf::Color oc = isSel ? sf::Color(acc.r, acc.g, acc.b, 220) : sf::Color(200, 200, 210, isFav ? 120 : 60);
            buildFootprintOutline(p.la.subLatDeg, p.la.subLonDeg, ang0, oc, outline);
            if (isSel || isTrk) {
                sf::VertexArray fill(sf::PrimitiveType::Triangles);
                const double angMin = footprintAngle(p.la.altKm, trk.minElDeg);
                buildFootprint(p.la.subLatDeg, p.la.subLonDeg, angMin, sf::Color(acc.r, acc.g, acc.b, isSel ? 55 : 35), fill);
                drawWrapped(fill, -WW, 2 * WW);
            }
            drawWrapped(outline, -WW, 2 * WW);
        }

        // ground track of the selected satellite: last half orbit dim, next orbit bright
        if (showTracks && selected) {
            if (const Satellite* s = cat.get(selected)) {
                const double per = std::max(s->periodMinutes(), 1.0);
                const double span = std::min(per * 1.5, 1440.0);
                const double stepMin = std::clamp(per / 180.0, 0.25, 10.0);
                sf::VertexArray past(sf::PrimitiveType::LineStrip), fut(sf::PrimitiveType::LineStrip);
                double prevX = 0;
                bool first = true;
                for (double m = -per * 0.5; m <= span - per * 0.5; m += stepMin) {
                    LookAngles la = look(*s, trk.obs, t + m / 1440.0);
                    if (!la.ok) continue;
                    double x = lonToX(wrapLon(la.subLonDeg));
                    if (!first) { while (x - prevX > WW / 2) x -= WW; while (prevX - x > WW / 2) x += WW; }
                    first = false;
                    prevX = x;
                    const sf::Vector2f pt{(float)x, (float)latToY(la.subLatDeg)};
                    if (m <= 0) past.append({pt, sf::Color(255, 255, 255, 70)});
                    if (m + stepMin > 0) fut.append({pt, sf::Color(255, 220, 90, 200)});   // last past point bridges both
                }
                drawWrapped(past, -2 * WW, 3 * WW);
                drawWrapped(fut, -2 * WW, 3 * WW);
            }
        }

        // ===== overlay (screen units) =====
        win->setView(sf::View(sf::FloatRect({0.f, 0.f}, logical)));
        auto inMap = [&](sf::Vector2f p) { return r.contains(p); };

        // sun
        {
            const SunPos sun = sunPosition(t);
            const sf::Vector2f p = latLonToUi(sun.subLatDeg, sun.subLonDeg);
            if (inMap(p)) {
                sf::CircleShape c(7.f); c.setOrigin({7.f, 7.f}); c.setPosition(p);
                c.setFillColor(sf::Color(255, 210, 60)); c.setOutlineColor(sf::Color(255, 240, 160, 120)); c.setOutlineThickness(3.f);
                win->draw(c);
                drawText(*win, font, "Sun", p.x + 12, p.y - 7, 11, sf::Color(255, 220, 120), true);
                sunHover = std::hypot(p.x - mouseUi.x, p.y - mouseUi.y) < 10.0f ? std::optional<SunPos>(sun) : std::nullopt;
            } else sunHover.reset();
        }
        // station
        {
            const sf::Vector2f p = latLonToUi(trk.obs.latDeg, trk.obs.lonDeg);
            if (inMap(p)) {
                sf::RectangleShape a({12.f, 2.f}), b({2.f, 12.f});
                a.setOrigin({6.f, 1.f}); b.setOrigin({1.f, 6.f}); a.setPosition(p); b.setPosition(p);
                a.setFillColor(sf::Color(255, 80, 80)); b.setFillColor(sf::Color(255, 80, 80));
                win->draw(a); win->draw(b);
                drawText(*win, font, "QTH " + toMaidenhead(trk.obs.latDeg, trk.obs.lonDeg), p.x + 8, p.y - 16, 11, sf::Color(255, 140, 140), true);
            }
        }

        drawPositions(r);

        // satellites
        hovered = 0;
        float bestD = 12.0f;
        for (const auto& p : positions) {
            const sf::Vector2f sp = latLonToUi(p.la.subLatDeg, p.la.subLonDeg);
            if (!inMap(sp)) continue;
            const float d = std::hypot(sp.x - mouseUi.x, sp.y - mouseUi.y);
            if (d < bestD && r.contains(mouseUi)) { bestD = d; hovered = p.id; }
        }
        for (int pass = 0; pass < 2; pass++) {   // others first, highlighted on top
            for (const auto& p : positions) {
                const bool isSel = p.id == selected, isTrk = trk.active() && p.id == trk.norad;
                const bool isFav = cat.favorites.count(p.id) > 0;
                const bool top = isSel || isTrk || p.id == hovered;
                if ((pass == 0) == top) continue;
                const sf::Vector2f sp = latLonToUi(p.la.subLatDeg, p.la.subLonDeg);
                if (!inMap(sp)) continue;
                const bool up = p.la.elDeg > 0;
                const float rad = isSel ? 5.5f : isFav ? 4.0f : 3.0f;
                sf::CircleShape c(rad); c.setOrigin({rad, rad}); c.setPosition(sp);
                c.setFillColor(up ? sf::Color(90, 255, 120) : isSel ? acc : isFav ? sf::Color(255, 220, 90) : sf::Color(220, 225, 235));
                c.setOutlineColor(sf::Color(0, 0, 0, 180)); c.setOutlineThickness(1.f);
                win->draw(c);
                if (isSel || isTrk) {
                    sf::CircleShape ring(rad + 4); ring.setOrigin({rad + 4, rad + 4}); ring.setPosition(sp);
                    ring.setFillColor(sf::Color::Transparent);
                    ring.setOutlineColor(isTrk ? sf::Color(255, 150, 50) : acc); ring.setOutlineThickness(2.f);
                    win->draw(ring);
                }
                if (isSel || isFav || isTrk || allLabels || p.id == hovered) {
                    const Satellite* s = cat.get(p.id);
                    if (s) {
                        const std::string n = s->name;
                        sf::Text tx = makeText(font, n, sp.x + rad + 4, sp.y - 8, 11, isSel ? sf::Color::White : sf::Color(230, 230, 235), isSel);
                        sf::FloatRect b = tx.getGlobalBounds();
                        sf::RectangleShape bg({b.size.x + 6, b.size.y + 4});
                        bg.setPosition({b.position.x - 3, b.position.y - 2});
                        bg.setFillColor(sf::Color(0, 0, 0, isSel ? 170 : 110));
                        win->draw(bg);
                        win->draw(tx);
                    }
                }
            }
        }
        // tooltip
        if (hovered) {
            auto it = std::find_if(positions.begin(), positions.end(), [&](const SatPos& p) { return p.id == hovered; });
            const Satellite* s = cat.get(hovered);
            if (it != positions.end() && s) {
                char b[160];
                std::snprintf(b, sizeof(b), "%s\nEl %.1f\xC2\xB0  Az %.0f\xC2\xB0  %.0f km\nAlt %.0f km   click: select", s->name.c_str(),
                              it->la.elDeg, it->la.azDeg, it->la.rangeKm, it->la.altKm);
                sf::Text tx = makeText(font, b, mouseUi.x + 14, mouseUi.y + 14, 11, sf::Color::White);
                sf::FloatRect bb = tx.getGlobalBounds();
                float dx = (bb.position.x + bb.size.x + 8 > r.position.x + r.size.x) ? -(bb.size.x + 36) : 0.f;
                tx.move({dx, 0.f});
                bb = tx.getGlobalBounds();
                sf::RectangleShape bg({bb.size.x + 12, bb.size.y + 10});
                bg.setPosition({bb.position.x - 6, bb.position.y - 5});
                bg.setFillColor(sf::Color(20, 20, 26, 230)); bg.setOutlineColor(acc); bg.setOutlineThickness(1.f);
                win->draw(bg); win->draw(tx);
            }
        }

        if (hoveredPos >= 0 && !hovered) drawPositionTooltip(r);
        drawAttribution(r);

        if (sunHover && !hovered && hoveredPos < 0) {
            char b[200];
            std::snprintf(b, sizeof(b), "Sun - subsolar point (the Sun is overhead here)\nLat %.1f\xC2\xB0  Lon %.1f\xC2\xB0   moves with the map time",
                          sunHover->subLatDeg, sunHover->subLonDeg);
            sf::Text tx = makeText(font, b, mouseUi.x + 14, mouseUi.y + 14, 11, sf::Color::White);
            sf::FloatRect bb = tx.getGlobalBounds();
            if (bb.position.x + bb.size.x + 8 > r.position.x + r.size.x) tx.move({-(bb.size.x + 36), 0.f});
            bb = tx.getGlobalBounds();
            sf::RectangleShape bg({bb.size.x + 12, bb.size.y + 10});
            bg.setPosition({bb.position.x - 6, bb.position.y - 5});
            bg.setFillColor(sf::Color(20, 20, 26, 230)); bg.setOutlineColor(sf::Color(255, 210, 60)); bg.setOutlineThickness(1.f);
            win->draw(bg); win->draw(tx);
        }

        if (!mapLoaded && !mapStatus.empty()) {
            // paragraphs split at '\n', each wrapped to the map width
            std::vector<std::string> lines;
            std::string para;
            std::istringstream ps(mapStatus);
            while (std::getline(ps, para)) for (auto& l : wrap(para, 14, r.size.x - 60.0f)) lines.push_back(l);
            float y = r.position.y + r.size.y / 2 - lines.size() * 10.0f;
            for (const auto& l : lines) {
                drawText(*win, font, l, r.position.x + r.size.x / 2, y, 14, sf::Color(200, 210, 230), false, 1);
                y += 20.0f;
            }
        }
        if (pickQth)
            drawText(*win, font, "Click on the map to set your station (Esc cancels)", r.position.x + r.size.x / 2, r.position.y + 12, 14, sf::Color(255, 200, 90), true, 1);

        drawToolbar();
        drawStatusBar();
        panel.draw(*win);

        // cursor
        const bool overClickable = (hovered != 0) || (wList->isMouseOver(*win)) || (wPasses->isMouseOver(*win)) ||
                                   panel.isAnyWidgetHovered(*win) || tbNow.isMouseOver(*win) || tbM1h.isMouseOver(*win) ||
                                   tbM10.isMouseOver(*win) || tbP10.isMouseOver(*win) || tbP1h.isMouseOver(*win) ||
                                   tbWindow.isMouseOver(*win) || tbKind[0].isMouseOver(*win) || tbKind[1].isMouseOver(*win) ||
                                   tbKind[2].isMouseOver(*win) || tbKind[3].isMouseOver(*win) || tbKind[4].isMouseOver(*win);
        static auto curArrow = sf::Cursor::createFromSystem(sf::Cursor::Type::Arrow);
        static auto curHand = sf::Cursor::createFromSystem(sf::Cursor::Type::Hand);
        static auto curCross = sf::Cursor::createFromSystem(sf::Cursor::Type::Cross);
        static auto curMove = sf::Cursor::createFromSystem(sf::Cursor::Type::SizeAll);
        if (pickQth && r.contains(mouseUi) && curCross) win->setMouseCursor(*curCross);
        else if (dragging && dragMoved && curMove) win->setMouseCursor(*curMove);
        else if (overClickable && curHand) win->setMouseCursor(*curHand);
        else if (curArrow) win->setMouseCursor(*curArrow);

        win->display();
    }

    void drawToolbar() {
        sf::RectangleShape bar({logical.x - PANEL_W, TOOLBAR_H});
        bar.setPosition({PANEL_W, 0});
        bar.setFillColor(sf::Color(19, 19, 21));
        win->draw(bar);
        float x = PANEL_W + 10;
        for (SdrButton* b : {&tbM1h, &tbM10, &tbNow, &tbP10, &tbP1h}) { b->setPosition(x, 6); b->draw(*win); x += b->shape.getSize().x + 4; }
        // right side: which decoder positions are shown, and how far back
        {
            float rx = logical.x - 10.0f;
            tbWindow.setText(std::string("Last ") + POS_WINDOWS[posWindowIdx].second);
            rx -= tbWindow.shape.getSize().x;
            tbWindow.setPosition(rx, 6); tbWindow.draw(*win);
            rx -= 8.0f;
            for (int i = (int)PosKind::Count - 1; i >= 0; i--) {
                const size_t n = positionBus().count((PosKind)i);
                tbKind[i].setText(std::string(posKindName((PosKind)i)) + (n ? " " + std::to_string(n) : ""));
                tbKind[i].setActive(showKind[i]);
                rx -= tbKind[i].shape.getSize().x;
                tbKind[i].setPosition(rx, 6); tbKind[i].draw(*win);
                rx -= 4.0f;
            }
            toolbarRightX = rx;
        }
        x += 10;
        const double t = simJd();
        std::string ts = "UTC " + formatJd(t, false, "%Y-%m-%d %H:%M:%S") + "   local " + formatJd(t, true, "%H:%M:%S");
        if (x + textWidth(font, ts, 13) < toolbarRightX) drawText(*win, font, ts, x, 10, 13, sf::Color(220, 220, 225));
        x += textWidth(font, ts, 13) + 16;
        if (timeSource != TimeSource::Live) {
            const std::string src = timeSource == TimeSource::Manual ? "MANUAL TIME" : recPlaying && recStart.unix > 0 ? "RECORDING TIME" : "RECORDING: NO FILE";
            if (x + textWidth(font, src, 13) < toolbarRightX) drawText(*win, font, src, x, 10, 13, sf::Color(255, 170, 70), true);
            x += textWidth(font, src, 13) + 16;
        }
        if (std::fabs(simOffsetSec) >= 1.0) {
            std::string s = "SIMULATED TIME " + std::string(simOffsetSec > 0 ? "+" : "") + formatDuration(simOffsetSec);
            if (x + textWidth(font, s, 13) < toolbarRightX) drawText(*win, font, s, x, 10, 13, sf::Color(255, 160, 60), true);
            x += textWidth(font, s, 13) + 16;
        }
        // data age of the shown group, right aligned before the position buttons;
        // shortened, then left out when the window is too narrow
        sf::Color ac(150, 150, 160);
        const double h = cat.groupAgeHours(groupId);
        std::string when = h < 0 ? std::string() : (h < 1 ? std::to_string((int)(h * 60)) + " min ago" : formatDuration(h * 3600) + " ago");
        if (h > STALE_DAYS * 24) { when += " - STALE"; ac = sf::Color(255, 110, 90); }
        const std::string busy = cat.busy() ? "updating...  " : "";
        const std::vector<std::string> variants = {
            busy + CatalogLabel() + (when.empty() ? "" : ": downloaded " + when),
            busy + (when.empty() ? CatalogLabel() : "TLE " + when),
            cat.busy() ? std::string("updating...") : std::string() };
        for (const auto& v : variants) {
            if (v.empty()) break;
            if (toolbarRightX - 12 - textWidth(font, v, 12) > x) { drawText(*win, font, v, toolbarRightX - 12, 10, 12, ac, false, 2); break; }
        }
    }
    std::string CatalogLabel() const { return SatCatalog::groupLabel(groupId); }

    void drawStatusBar() {
        const float y = logical.y - STATUS_H;
        sf::RectangleShape bar({logical.x - PANEL_W, STATUS_H});
        bar.setPosition({PANEL_W, y});
        bar.setFillColor(sf::Color(19, 19, 21));
        win->draw(bar);
        const auto r = mapRect();
        if (r.contains(mouseUi)) {
            double la, lo;
            uiToLatLon(mouseUi, la, lo);
            // distance and bearing from the station
            const double p1 = trk.obs.latDeg * DEG2RAD, p2 = la * DEG2RAD, dl = (lo - trk.obs.lonDeg) * DEG2RAD;
            const double c = std::acos(std::clamp(std::sin(p1) * std::sin(p2) + std::cos(p1) * std::cos(p2) * std::cos(dl), -1.0, 1.0));
            double brg = std::atan2(std::sin(dl) * std::cos(p2), std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl)) * RAD2DEG;
            if (brg < 0) brg += 360;
            char b[160];
            std::snprintf(b, sizeof(b), "%.2f\xC2\xB0%c  %.2f\xC2\xB0%c   %s   %.0f km from QTH, bearing %.0f\xC2\xB0",
                          std::fabs(la), la >= 0 ? 'N' : 'S', std::fabs(lo), lo >= 0 ? 'E' : 'W', toMaidenhead(la, lo).c_str(), c * SGP_RE, brg);
            drawText(*win, font, b, PANEL_W + 10, y + 5, 12, sf::Color(180, 180, 190));
        } else {
            drawText(*win, font, "Wheel: zoom   Drag: move   Click a satellite to select it", PANEL_W + 10, y + 5, 12, sf::Color(120, 120, 130));
        }
        const SunPos sun = sunPosition(simJd());
        const double se = sunElevationAt(sun, trk.obs.latDeg, trk.obs.lonDeg);
        char b[80];
        std::snprintf(b, sizeof(b), "Sun at QTH: %.0f\xC2\xB0 (%s)", se, se > 0 ? "day" : se > -6 ? "twilight" : "night");
        drawText(*win, font, b, logical.x - 12, y + 5, 12, sf::Color(180, 180, 190), false, 2);
    }

    void drawList(sf::RenderWindow& w, const sf::FloatRect& r) {
        sf::RectangleShape bg(r.size);
        bg.setPosition(r.position);
        bg.setFillColor(sf::Color(24, 24, 28));
        bg.setOutlineColor(sf::Color(60, 60, 66)); bg.setOutlineThickness(1.f);
        w.draw(bg);
        const double now = baseJd();
        char hdr[96];
        std::snprintf(hdr, sizeof(hdr), "%d satellites", (int)listIds.size());
        drawText(w, font, hdr, r.position.x + 22, r.position.y + 2, 11, sf::Color(130, 130, 140));
        drawText(w, font, "now / next pass", r.position.x + r.size.x - 6, r.position.y + 2, 11, sf::Color(130, 130, 140), false, 2);
        const float top = r.position.y + 18.0f;
        const float visH = r.size.y - 18.0f;
        const float maxScroll = std::max(0.0f, listIds.size() * ROW_H - visH);
        listScroll = std::min(listScroll, maxScroll);
        if (listIds.empty()) {
            std::string msg = cat.busy() ? "Downloading..." : (groupId == "custom" ? "Paste or import TLEs to add satellites" :
                              groupId == "favorites" ? "Click the star next to a satellite" : "No satellites - press Refresh");
            drawText(w, font, msg, r.position.x + r.size.x / 2, top + 30, 12, sf::Color(140, 140, 150), false, 1);
            return;
        }
        const sf::Vector2f m = w.mapPixelToCoords(sf::Mouse::getPosition(w));
        const int first = (int)(listScroll / ROW_H);
        for (int i = first; i < (int)listIds.size(); i++) {
            const float y = top + i * ROW_H - listScroll;
            if (y > r.position.y + r.size.y - ROW_H + 2) break;
            const int id = listIds[i];
            const Satellite* s = cat.get(id);
            if (!s) continue;
            const sf::FloatRect rowR({r.position.x + 1, y}, {r.size.x - 2, ROW_H});
            if (id == selected) {
                sf::RectangleShape hl(rowR.size); hl.setPosition(rowR.position); hl.setFillColor(Theme::AccentDim); w.draw(hl);
            } else if (rowR.contains(m)) {
                sf::RectangleShape hl(rowR.size); hl.setPosition(rowR.position); hl.setFillColor(sf::Color(255, 255, 255, 18)); w.draw(hl);
            }
            const bool fav = cat.favorites.count(id) > 0;
            drawStar(w, r.position.x + 11, y + ROW_H / 2, 6.0f, fav ? sf::Color(255, 210, 60) : sf::Color::Transparent,
                     fav ? sf::Color(255, 210, 60) : sf::Color(110, 110, 120));
            if (trk.active() && trk.norad == id) {
                sf::CircleShape dot(3.f); dot.setOrigin({3.f, 3.f}); dot.setPosition({r.position.x + 24, y + ROW_H / 2});
                dot.setFillColor(sf::Color(255, 150, 50)); w.draw(dot);
            }
            std::string name = s->name;
            while (name.size() > 4 && textWidth(font, name, 12) > r.size.x - 130) name = name.substr(0, name.size() - 2);
            if (name != s->name) name += "..";
            drawText(w, font, name, r.position.x + 30, y + 2, 12, sf::Color(230, 230, 235));
            // right column: elevation now, or time to the next pass
            const double el = elevationAt(*s, trk.obs, now);
            std::string right;
            sf::Color rc(150, 150, 160);
            if (el > 0) {
                char b[32]; std::snprintf(b, sizeof(b), "UP %.0f\xC2\xB0", el); right = b;
                rc = el >= trk.minElDeg ? sf::Color(90, 255, 120) : sf::Color(150, 220, 150);
            } else {
                auto it = nextPass.find(id);
                if (it == nextPass.end()) right = "...";
                else if (!it->second.valid) right = "-";
                else {
                    char b[48]; std::snprintf(b, sizeof(b), "%s  %.0f\xC2\xB0", formatDuration((it->second.aosJd - now) * 86400.0).c_str(), it->second.maxElDeg);
                    right = b;
                    if ((it->second.aosJd - now) * 1440.0 < 15) rc = sf::Color(240, 200, 80);
                }
            }
            drawText(w, font, right, r.position.x + r.size.x - 6, y + 2, 12, rc, false, 2);
        }
        if (maxScroll > 0) {
            const float th = std::max(20.0f, visH * visH / (listIds.size() * ROW_H));
            sf::RectangleShape sb({4.f, th});
            sb.setPosition({r.position.x + r.size.x - 5, top + (visH - th) * (listScroll / maxScroll)});
            sb.setFillColor(sf::Color(100, 100, 110));
            w.draw(sb);
        }
    }

    void drawInfo(sf::RenderWindow& w, const sf::FloatRect& r) {
        const Satellite* s = cat.get(selected);
        float x = r.position.x, y = r.position.y;
        if (!s) { drawText(w, font, "Select a satellite in the list or on the map", x, y + 4, 12, sf::Color(140, 140, 150)); return; }
        const double t = simJd();
        const LookAngles la = look(*s, trk.obs, t, true);
        drawText(w, font, s->name, x, y, 16, Theme::Accent, true);
        y += 22;
        char b[200];
        std::snprintf(b, sizeof(b), "NORAD %d   %s   %s", s->norad, s->intlDesig.c_str(), cat.custom.count(s->norad) ? "(your TLE)" : "");
        drawText(w, font, b, x, y, 11, sf::Color(150, 150, 160));
        y += 16;
        const double ageDays = nowJd() - s->epochJd;
        std::snprintf(b, sizeof(b), "TLE epoch %s UTC  (%.1f days old)", formatJd(s->epochJd, false, "%Y-%m-%d %H:%M").c_str(), ageDays);
        drawText(w, font, b, x, y, 11, ageColor(ageDays));
        y += 18;
        auto row = [&](const std::string& k1, const std::string& v1, const std::string& k2, const std::string& v2, sf::Color c1 = sf::Color::White) {
            drawText(w, font, k1, x, y, 12, sf::Color(140, 140, 150));
            drawText(w, font, v1, x + 62, y, 12, c1);
            drawText(w, font, k2, x + 172, y, 12, sf::Color(140, 140, 150));
            drawText(w, font, v2, x + 234, y, 12, sf::Color::White);
            y += 17;
        };
        if (!la.ok) {
            drawText(w, font, s->decayed ? "Decayed" : "Propagation failed (TLE too old or invalid)", x, y, 12, sf::Color(255, 110, 90));
            return;
        }
        auto f = [](const char* fmt, double v) { char bb[48]; std::snprintf(bb, sizeof(bb), fmt, v); return std::string(bb); };
        row("Azimuth", f("%.1f\xC2\xB0", la.azDeg), "Elevation", f("%.1f\xC2\xB0", la.elDeg), la.elDeg > 0 ? sf::Color(90, 255, 120) : sf::Color::White);
        row("Range", f("%.0f km", la.rangeKm), "Rate", f("%+.3f km/s", la.rangeRateKms));
        row("Altitude", f("%.0f km", la.altKm), "Speed", f("%.2f km/s", la.speedKms));
        char pos[64];
        std::snprintf(pos, sizeof(pos), "%.2f\xC2\xB0%c %.2f\xC2\xB0%c", std::fabs(la.subLatDeg), la.subLatDeg >= 0 ? 'N' : 'S', std::fabs(la.subLonDeg), la.subLonDeg >= 0 ? 'E' : 'W');
        row("Position", pos, "Locator", toMaidenhead(la.subLatDeg, la.subLonDeg));
        row("Period", f("%.1f min", s->periodMinutes()), "Incl.", f("%.1f\xC2\xB0", s->incloDeg));
        const double fpKm = 2.0 * footprintAngle(la.altKm, 0.0) * SGP_RE;
        row("Sunlight", la.eclipsed ? "eclipse" : "sunlit", "Footprint", f("%.0f km", fpKm));
        y += 2;
        // next / current pass (the map's time without the preview offset)
        const double now = baseJd();
        std::string nx;
        sf::Color nc(220, 220, 225);
        if (!selPasses.empty()) {
            const Pass& p = selPasses.front();
            if (p.alwaysUp) { nx = "Always above the horizon (geostationary)"; nc = sf::Color(90, 255, 120); }
            else if (p.aosJd <= now) { nx = "IN PASS now - LOS in " + formatDuration((p.losJd - now) * 86400.0) + f(", max %.0f\xC2\xB0", p.maxElDeg); nc = sf::Color(90, 255, 120); }
            else nx = "Next pass in " + formatDuration((p.aosJd - now) * 86400.0) + f(", max %.0f\xC2\xB0", p.maxElDeg);
        } else nx = "No pass above " + std::to_string((int)trk.minElDeg) + "\xC2\xB0 in the next 3 days";
        drawText(w, font, nx, x, y, 12, nc, true);
        if (s->deepSpace) drawText(w, font, "High orbit: positions are approximate (no lunar/solar terms)", x, y + 17, 10, sf::Color(150, 150, 160));
    }

    void drawPolar(sf::RenderWindow& w, const sf::FloatRect& r) {
        const float R = std::min(r.size.x, r.size.y) / 2.0f - 14.0f;
        const sf::Vector2f c{r.position.x + r.size.x / 2.0f, r.position.y + r.size.y / 2.0f};
        auto toXY = [&](double az, double el) {
            const double rr = (90.0 - std::clamp(el, 0.0, 90.0)) / 90.0 * R;
            return sf::Vector2f{c.x + (float)(rr * std::sin(az * DEG2RAD)), c.y - (float)(rr * std::cos(az * DEG2RAD))};
        };
        sf::CircleShape bgc(R); bgc.setOrigin({R, R}); bgc.setPosition(c);
        bgc.setFillColor(sf::Color(16, 22, 30)); bgc.setOutlineColor(sf::Color(90, 90, 100)); bgc.setOutlineThickness(1.f);
        bgc.setPointCount(96);
        w.draw(bgc);
        for (int el : {30, 60}) {
            const float rr = (float)((90 - el) / 90.0 * R);
            sf::CircleShape ring(rr); ring.setOrigin({rr, rr}); ring.setPosition(c); ring.setPointCount(72);
            ring.setFillColor(sf::Color::Transparent); ring.setOutlineColor(sf::Color(70, 70, 80)); ring.setOutlineThickness(1.f);
            w.draw(ring);
        }
        if (trk.minElDeg > 0) {
            const float rr = (float)((90 - trk.minElDeg) / 90.0 * R);
            sf::CircleShape ring(rr); ring.setOrigin({rr, rr}); ring.setPosition(c); ring.setPointCount(72);
            ring.setFillColor(sf::Color::Transparent); ring.setOutlineColor(sf::Color(255, 160, 60, 90)); ring.setOutlineThickness(1.f);
            w.draw(ring);
        }
        sf::VertexArray cross(sf::PrimitiveType::Lines);
        cross.append({{c.x - R, c.y}, sf::Color(70, 70, 80)}); cross.append({{c.x + R, c.y}, sf::Color(70, 70, 80)});
        cross.append({{c.x, c.y - R}, sf::Color(70, 70, 80)}); cross.append({{c.x, c.y + R}, sf::Color(70, 70, 80)});
        w.draw(cross);
        drawText(w, font, "N", c.x, c.y - R - 14, 11, sf::Color(200, 200, 210), true, 1);
        drawText(w, font, "S", c.x, c.y + R + 1, 11, sf::Color(200, 200, 210), true, 1);
        drawText(w, font, "E", c.x + R + 4, c.y - 7, 11, sf::Color(200, 200, 210), true);
        drawText(w, font, "W", c.x - R - 5, c.y - 7, 11, sf::Color(200, 200, 210), true, 2);

        const Satellite* s = cat.get(selected);
        if (!s) return;
        const double t = simJd();
        // the pass shown: the one in progress at the (simulated) time, else the next one
        Pass p;
        for (const auto& q : selPasses) if (q.alwaysUp || q.losJd > t) { p = q; break; }
        if (simOffsetSec != 0.0 && (!p.valid || p.aosJd > t + 0.5)) p = findNextPass(*s, trk.obs, t, 1.0, trk.minElDeg);
        if (p.valid && !p.alwaysUp) {
            sf::VertexArray path(sf::PrimitiveType::LineStrip);
            const int n = 80;
            for (int i = 0; i <= n; i++) {
                const double jt = p.aosJd + (p.losJd - p.aosJd) * i / n;
                const LookAngles la = look(*s, trk.obs, jt);
                if (la.ok) path.append({toXY(la.azDeg, la.elDeg), sf::Color(255, 220, 90)});
            }
            w.draw(path);
            auto dot = [&](sf::Vector2f pt, sf::Color col, float rad) {
                sf::CircleShape d(rad); d.setOrigin({rad, rad}); d.setPosition(pt); d.setFillColor(col); w.draw(d);
            };
            dot(toXY(p.aosAzDeg, 0), sf::Color(90, 255, 120), 3.5f);
            dot(toXY(p.losAzDeg, 0), sf::Color(255, 90, 90), 3.5f);
            char b[96];
            std::snprintf(b, sizeof(b), "%s  max %.0f\xC2\xB0", formatJd(p.aosJd, true, "%a %H:%M").c_str(), p.maxElDeg);
            drawText(w, font, b, r.position.x, r.position.y, 11, sf::Color(170, 170, 180));
        } else if (!p.valid) {
            drawText(w, font, "no pass soon", c.x, c.y + R / 2, 11, sf::Color(140, 140, 150), false, 1);
        }
        const LookAngles now = look(*s, trk.obs, t);
        if (now.ok && now.elDeg > 0) {
            sf::CircleShape d(5.f); d.setOrigin({5.f, 5.f}); d.setPosition(toXY(now.azDeg, now.elDeg));
            d.setFillColor(Theme::Accent); d.setOutlineColor(sf::Color::White); d.setOutlineThickness(1.5f);
            w.draw(d);
        }
    }

    void drawPasses(sf::RenderWindow& w, const sf::FloatRect& r) {
        float x = r.position.x, y = r.position.y;
        const sf::Color hc(130, 130, 140);
        drawText(w, font, "Pass (local time)", x, y, 11, hc);
        drawText(w, font, "Length", x + 140, y, 11, hc);
        drawText(w, font, "Max", x + 192, y, 11, hc);
        drawText(w, font, "Az AOS-LOS", x + 232, y, 11, hc);
        y += 20;
        if (selPasses.empty()) { drawText(w, font, "-", x, y, 12, sf::Color(140, 140, 150)); return; }
        const sf::Vector2f m = w.mapPixelToCoords(sf::Mouse::getPosition(w));
        const double t = simJd(), now = baseJd();
        static const char* dirs[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
        auto dir = [](double az) { return dirs[(int)std::fmod((az + 22.5) / 45.0, 8.0)]; };
        for (const auto& p : selPasses) {
            if (y > r.position.y + r.size.y - 16) break;
            const sf::FloatRect rowR({x - 4, y - 1}, {r.size.x + 4, 18.f});
            const bool cur = (t >= p.aosJd && t <= p.losJd);
            if (cur || rowR.contains(m)) {
                sf::RectangleShape hl(rowR.size); hl.setPosition(rowR.position);
                hl.setFillColor(cur ? sf::Color(Theme::AccentDim.r, Theme::AccentDim.g, Theme::AccentDim.b, 160) : sf::Color(255, 255, 255, 18));
                w.draw(hl);
            }
            if (p.alwaysUp) { drawText(w, font, "always visible", x, y, 12, sf::Color(90, 255, 120)); y += 18; continue; }
            const sf::Color c = p.maxElDeg >= 45 ? sf::Color(90, 255, 120) : p.maxElDeg >= 20 ? sf::Color(240, 220, 120) : sf::Color(190, 190, 200);
            const bool live = p.aosJd <= now && p.losJd >= now;
            drawText(w, font, live ? "now" : formatJd(p.aosJd, true, "%a %d %H:%M"), x, y, 12, live ? sf::Color(90, 255, 120) : sf::Color(225, 225, 230));
            drawText(w, font, formatDuration(p.durationSec()), x + 140, y, 12, sf::Color(200, 200, 205));
            char b[32]; std::snprintf(b, sizeof(b), "%.0f\xC2\xB0", p.maxElDeg);
            drawText(w, font, b, x + 192, y, 12, c, true);
            drawText(w, font, std::string(dir(p.aosAzDeg)) + " - " + dir(p.losAzDeg), x + 232, y, 12, sf::Color(200, 200, 205));
            y += 18;
        }
    }

    void drawRadio(sf::RenderWindow& w, const sf::FloatRect& r) {
        float x = r.position.x, y = r.position.y;
        radioDecoderBtn = sf::FloatRect();
        const Satellite* s = cat.get(selected);
        char b[160];
        if (trk.active()) {
            const bool other = trk.norad != selected;
            std::snprintf(b, sizeof(b), "%s %s  %s MHz", trk.mode == TrackMode::Doppler ? "Doppler tracking" : "Tracking",
                          trk.satName.c_str(), mhz(trk.downlinkHz, 4).c_str());
            drawText(w, font, b, x, y, 12, sf::Color(255, 160, 60), true);
            y += 17;
            std::snprintf(b, sizeof(b), "Radio %s MHz   Doppler %+.0f Hz (%+.0f Hz/s)", mhz(trk.wantHz, 6).c_str(), trk.dopplerHz, trk.dopplerRateHzS);
            drawText(w, font, b, x, y, 12, sf::Color(230, 230, 235));
            y += 16;
            if (trk.userOffsetHz != 0) {
                std::snprintf(b, sizeof(b), "Your fine tuning: %+lld Hz (kept on top of the correction)", (long long)trk.userOffsetHz);
                drawText(w, font, b, x, y, 11, sf::Color(170, 170, 180));
                y += 15;
            }
            if (!trk.applyStatus.empty()) { drawText(w, font, trk.applyStatus, x, y, 11, sf::Color(255, 150, 80)); y += 15; }
            if (other) { drawText(w, font, "(the tracked satellite is not the selected one)", x, y, 11, sf::Color(150, 150, 160)); y += 15; }
        } else if (s && chosenHz > 0) {
            // preview for the selected satellite (real time)
            const LookAngles la = look(*s, trk.obs, baseJd());
            if (la.ok) {
                std::snprintf(b, sizeof(b), "Doppler now %+.0f Hz  ->  %s MHz", la.dopplerHz((double)chosenHz),
                              mhz(chosenHz + (long long)std::llround(la.dopplerHz((double)chosenHz)), 6).c_str());
                drawText(w, font, b, x, y, 12, sf::Color(210, 210, 220));
                y += 17;
            }
            drawText(w, font, "Not tracking. Track freq tunes once, Track Doppler follows the pass.", x, y, 11, sf::Color(140, 140, 150));
            y += 15;
        } else {
            drawText(w, font, "Pick a satellite and a transmitter.", x, y, 12, sf::Color(140, 140, 150));
            y += 17;
        }
        if (s && chosenHz > 0) {
            const Transmitter t = currentTx();
            const DemodHint h = demodHintFor(t);
            drawText(w, font, "Mode: " + h.text + (t.alive ? "" : "   (SatNOGS marks it inactive)"), x, y, 11, sf::Color(170, 170, 180));
            y += 15;
            if (!h.decoder.empty()) {
                if (trk.activeDecoderName == h.decoder) {
                    drawText(w, font, h.decoder + " decoder is on" + std::string(trk.target == TrackTarget::Decoder ? " (its channel is tracked)" : ""),
                             x, y, 11, sf::Color(90, 255, 120));
                } else {
                    sf::Text tx = makeText(font, "> Enable the " + h.decoder + " decoder", x, y, 12, Theme::Accent, true);
                    radioDecoderBtn = tx.getGlobalBounds();
                    radioDecoderBtn.position.y -= 2; radioDecoderBtn.size.y += 6;
                    w.draw(tx);
                    wRadio->hoverHand = true;
                }
            }
        }
    }
};

} // namespace orbit
