#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
#endif

#define MINIAUDIO_IMPLEMENTATION
#include <SFML/Graphics.hpp>
#include <SFML/Window/Clipboard.hpp>
#include <iostream>
#include <optional>
#include <regex>
#include <cstring>

// Core System
#include "DSP.h"
#include "AudioSink.h"
#include "Demodulator.h"
#include "UI.h"
#include "Sidebar.h"
#include "NativeDialogs.h"
#include "IQSources.h"
#include "APRS_Decoder.h"
#include "Settings.h"
#include "PlatformUtils.h"
#include "Bookmarks.h"

// Extracted Modules
#include "Globals.h"
#include "Utils.h"
#include "WavWriter.h"
#include "InspectorDSP.h"
#include "DSPWorker.h"

// Custom Decoders
#include "modules/Core/SDRModule.h"
#include "modules/AIS/AIS_Decoder.h"
#include "modules/ImageDecoders/SSTV_Module.h"
#include "modules/ImageDecoders/WEFAX_Module.h"
#include "modules/ISM/ISM_Decoder.h"
#include "modules/ACARS/ACARS_Decoder.h"
#include "modules/Digital/DigitalVoice_Decoder.h"
#include "modules/ADSB/ADSB_Decoder.h"
#include "modules/WSJT/WSJT_Decoder.h"
#include "modules/VDL2/VDL2_Decoder.h"
#include "modules/CW/CW_Decoder.h"
#include "modules/Maritime/NAVTEX_Decoder.h"
#include "modules/Maritime/DSC_Decoder.h"
#include "modules/Analyzer/Analyzer_Decoder.h"

// Satellite map and tracking
#include "orbit/MapWindow.h"
#include "MacWindowFix.h"
#include "UiScale.h"

// =====================================================================
//  One receiver = one tab: a source (radio, file, sound card, rtl_tcp),
//  its own DSP thread, its own decoders and the UI state that belongs to
//  it (frequency, source choice ...). The UI shows one receiver at a time
//  (`cur` in main); the others keep receiving and decoding in the
//  background without drawing anything.
// =====================================================================
// Settings values lose spaces, commas and quotes (see SettingsManager::load): device ids
// (sound card names ...) are stored as hex
inline std::string toHex(const std::string& s) {
    static const char* d = "0123456789abcdef";
    std::string o;
    for (unsigned char c : s) { o += d[c >> 4]; o += d[c & 15]; }
    return o;
}
inline std::string fromHex(const std::string& h) {
    std::string o;
    for (size_t i = 0; i + 1 < h.size(); i += 2) {
        auto v = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : 0; };
        o += (char)(v(h[i]) * 16 + v(h[i + 1]));
    }
    return o;
}

struct Receiver {
    int id = 1;
    SharedData sd;
    std::atomic<bool> running {true};
    std::thread dsp;

    // UI state of this receiver
    long long centerFreq = 0, pendingCenterFreq = 0;
    int sourceType = 0;
    bool tcpConnected = false;
    std::string tcpAddr;
    std::vector<SDRDeviceItem> cachedDevices;
    bool inputIsAudio = false;
    bool stickyCenter = false;
    Mode modeBeforeAudio = Mode::NFM;
    std::string selectedTab;              // decoder tab shown in the decoder panel
    unsigned long long seenDrops = 0;     // decoder blocks skipped that the UI already warned about
    int openType = -1;                    // the device this receiver has open (type + id), so
    std::string openDevice;               // another receiver doesn't try to open it too
    float pendingRfGain = -999.0f;
    sf::Clock debouncer, gainDebouncer;
    // sidebar widgets that are not mirrored in SharedData (saved when the tab is left)
    std::vector<std::string> deviceOptions = {"No Devices Found"}, rateOptions = {"None"};
    int deviceSel = 0, rateSel = 0;
    bool agc = true;
    float gain = 0.0f, gainMin = 0.0f, gainMax = 50.0f;
    std::string hostText;
    bool uiSaved = false;
    std::string audioDevice;              // its output device by name ("" = system default)

    // decoders of this receiver
    std::vector<std::unique_ptr<SDRModule>> owned;
    std::vector<SDRModule*> decMods;      // in the order of the Decoders checkboxes (nullptr = APRS)
    AISDecoder* ais = nullptr;
    SSTVModule* sstv = nullptr;
    WEFAXModule* wefax = nullptr;
    ISMDecoder* ism = nullptr;
    ACARSDecoder* acars = nullptr;
    VDL2Decoder* vdl2 = nullptr;
    ADSBDecoder* adsb = nullptr;
    WSJTDecoder* wsjt = nullptr;
    CWDecoder* cw = nullptr;
    NAVTEXDecoder* navtex = nullptr;
    DSCDecoder* dsc = nullptr;
    DigitalVoiceDecoder* dvoice = nullptr;
    SignalAnalyzer* analyzer = nullptr;

    // a short name for the tab
    std::string title() const {
        static const char* kinds[] = {"File", "RTL-SDR", "SDRplay", "Sound card", "rtl_tcp"};
        std::string t = "RX" + std::to_string(id) + "  " + kinds[std::clamp(sourceType, 0, 4)];
        long long f = centerFreq;
        if (f > 0) { char b[32]; std::snprintf(b, sizeof(b), "  %.3f", f / 1e6); t += b; }
        return t;
    }

    ~Receiver() {
        running = false;
        if (dsp.joinable()) dsp.join();
        std::lock_guard<std::mutex> l(sd.sourceMtx);
        if (sd.source) { sd.source->stop(); sd.source->close(); }
    }
};

int main() {
    #ifdef _WIN32
    HMODULE hUser32 = LoadLibraryA("user32.dll");
    if (hUser32) {
        typedef BOOL (WINAPI *SetProcessDpiAwarenessContextProc)(void*);
        auto setDpiAware = (SetProcessDpiAwarenessContextProc)GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
        if (setDpiAware) {
            setDpiAware((void*)-4);
        }
    }
    #endif

    SettingsManager settingsMgr;
    std::string settingsPath = getSettingsFilePath();
    std::cout << "[INFO] Loading settings from: " << settingsPath << std::endl;
    settingsMgr.load(settingsPath);

    long long savedFreq = (long long)settingsMgr.getFloat("frequency", 97800000.0f);
    if (savedFreq < 1000000) {
        std::cerr << "[Warning] Invalid startup frequency. Resetting to 97.8 MHz." << std::endl;
        savedFreq = 97800000;
    }

    AudioSink audio;
    std::vector<std::string> deviceNames;
    for (const auto& dev : audio.availableDevices) deviceNames.push_back(dev.name);

    // The receivers (tabs); `cur` is the one shown. Created below, once the font exists.
    std::vector<std::unique_ptr<Receiver>> receivers;
    Receiver* cur = nullptr;

    sf::ContextSettings settings;
    settings.antiAliasingLevel = 8;
    // Open at 85 % of the screen, centred. A window as large as the screen would hide its
    // bottom part (timeline, status bar) behind the taskbar / dock.
    const sf::VideoMode desktop = sf::VideoMode::getDesktopMode();
    const sf::Vector2u startSize{ desktop.size.x * 85 / 100, desktop.size.y * 85 / 100 };
    sf::RenderWindow window(sf::VideoMode(startSize), "OrbitSDR", sf::Style::Default, sf::State::Windowed, settings);
    window.setPosition({ (int)(desktop.size.x - startSize.x) / 2, (int)(desktop.size.y - startSize.y) / 2 });
#ifdef _WIN32
    // On Windows start maximised: the system fits the window into the area above the taskbar
    ShowWindow((HWND)window.getNativeHandle(), SW_MAXIMIZE);
#endif
    window.setFramerateLimit(60);

    // While a zenity / osascript dialog is open the window must keep answering the window
    // manager (GNOME offers "Force quit" for a window silent for ~5 s). The events that come
    // in meanwhile are kept and handled after the dialog, in order, as before.
    std::deque<sf::Event> heldEvents;
    dialogPumpThread() = std::this_thread::get_id();
    dialogPump() = [&window, &heldEvents]() {
        while (const std::optional<sf::Event> e = window.pollEvent()) heldEvents.push_back(*e);
    };

    // Icon: looked up next to the executable and in ../Resources (macOS bundle)
    sf::Image icon;
    {
        std::string iconPath = findResourceFile("icon.png");
        if (!iconPath.empty() && icon.loadFromFile(iconPath)) {
            window.setIcon(icon);
        }
    }

    sf::Font font;
    if (!font.openFromFile("/System/Library/Fonts/Helvetica.ttc") &&
        !font.openFromFile("C:/Windows/Fonts/arial.ttf") &&
        !font.openFromFile("/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf") &&
        !font.openFromFile("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf") &&
        !font.openFromFile("/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf") &&
        !font.openFromFile("/usr/share/fonts/TTF/DejaVuSans.ttf") &&
        !font.openFromFile("arial.ttf")) {
        std::cerr << "Font not found!\n";
    }

    // --- UI SCALE SETUP ---
    // The whole UI is drawn in "logical" coordinates and the window view is scaled,
    // so every widget / mouse hit-test automatically follows the chosen scale.
    // index 0 = Auto (the system's scale, see UiScale.h); Ctrl / Cmd + "+", "-", "0" step it
    const std::vector<std::string> UI_SCALE_NAMES  = {"Auto (system)", "75%", "100%", "125%", "150%", "175%", "200%", "250%", "300%"};
    const std::vector<float>       UI_SCALE_VALUES = {0.0f, 0.75f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f};
    int uiScaleIdx = 0;
    float autoUiScale = 1.0f;
    auto detectAutoScale = [&]() -> bool {   // true when it changed
        const uiscale::Detected d = uiscale::detect(window);
        if (std::fabs(d.scale - autoUiScale) < 0.01f) return false;
        autoUiScale = d.scale;
        std::cout << "[INFO] System UI scale: " << autoUiScale << " (" << d.source << ")" << std::endl;
        return true;
    };
    autoUiScale = 0.0f;
    detectAutoScale();
    auto getUiScale = [&]() -> float {
        int idx = std::clamp(uiScaleIdx, 0, (int)UI_SCALE_VALUES.size() - 1);
        if (idx == 0) return autoUiScale;
        return UI_SCALE_VALUES[idx];
    };
    std::function<void()> applyUiScale; // assigned after updateLayout is defined

    // Image decoders save their pictures into the picture folder: the one chosen by the
    // user, or OrbitSDR inside the system Pictures folder
    std::string pictureDirCustom = settingsMgr.getString("picture_path", "");
    std::string pictureDirDefault = getDefaultPicturesPath();
    auto pictureDir = [&]() { return pictureDirCustom.empty() ? pictureDirDefault : pictureDirCustom; };
    auto setPictureDir = [&](const std::string& d) { pictureDirCustom = d; };

    auto cursorArrow = sf::Cursor::createFromSystem(sf::Cursor::Type::Arrow);
    auto cursorHand = sf::Cursor::createFromSystem(sf::Cursor::Type::Hand);
    auto cursorSizeH = sf::Cursor::createFromSystem(sf::Cursor::Type::SizeHorizontal);

    sf::RectangleShape topBar;
    topBar.setFillColor(sf::Color(19, 19, 21));

    Theme::setTheme(0);

    FrequencyDisplay freqVFO(20, 8, font);
    freqVFO.setFrequency(savedFreq);

    SdrButton btnTuningMode(40, 40, "FIX", font);
    btnTuningMode.setColor(sf::Color(80, 80, 80));

    SdrButton btnPlay(40, 40, ">", font);
    btnPlay.setColor(sf::Color(116, 57, 57));
    SdrButton btnMute(40, 40, "M", font);

    // --- Satellite map / tracking (MAP button in the top bar, see src/orbit/) ---
    SdrButton btnMap(56, 40, "MAP", font);
    orbit::SatCatalog satCatalog;
    satCatalog.init();
    orbit::SatTracker satTracker;
    satTracker.loadSettings(settingsMgr);
    orbit::MapWindow mapWindow(font, satCatalog, satTracker);
    mapWindow.uiScale = [&]() { return getUiScale(); };
    mapWindow.icon = &icon;
    mapWindow.loadSettings(settingsMgr);
    // "Time: recording being played": the map follows the IQ file's playback position
    mapWindow.recordingInfo = [&](std::string& path, double& progress, double& durationSec) {
        std::lock_guard<std::mutex> l(cur->sd.sourceMtx);
        if (!cur->sd.source) return false;
        path = cur->sd.source->getFilePath();   // empty for radios and the sound card
        if (path.empty()) return false;
        progress = cur->sd.source->getProgress();
        durationSec = cur->sd.source->getDurationSec();
        return true;
    };
    sf::Clock trackClock;

    // --- Signal pipelines: tabs of their own after the receiver tabs (pl::Hub, PipelineHub.h),
    //     nearly the whole window while shown. Their Channel inputs take the orange Analyzer
    //     channel of a receiver (the Analyzer is switched on there by itself). ---
    int pipeUid = 0;                 // the pipeline tab shown (0: none)
    bool pipeShown = false;          // a pipeline tab is the selected one
    auto shownWs = [&]() -> pl::Workspace* { return pipeShown ? pl::Hub::get().byUid(pipeUid) : nullptr; };
    pl::hooks().storePath = []() { return getConfigFilePath("pipelines.json"); };
    pl::hooks().saveJsonDialog = [](const std::string& name) { return saveJsonFileDialog(getDefaultExportPath(), name + ".json", "Export pipeline"); };
    pl::hooks().openJsonDialog = []() { return openJsonFileDialog("Import pipeline"); };
    pl::hooks().popup = [](const std::string& t, const std::string& m) { showPopup(t, m); };
    pl::hooks().imageDir = [&]() { return pictureDir(); };   // Image view "Save PNG": the pictures folder
    // WAV / byte files of the pipeline: the recordings folder (Recording in the sidebar)
    pl::hooks().recordDir = [&]() {
        std::string p;
        if (cur) { std::lock_guard<std::mutex> l(cur->sd.mtx); p = cur->sd.recPath; }
        return p.empty() ? getDefaultExportPath() : p;
    };
    // Audio out blocks on a sound card of their own: a stream of the speaker mixer each
    pl::hooks().audioDevices = [&]() {
        std::vector<std::string> v;
        for (const auto& d : audio.availableDevices) v.push_back(d.name);
        return v;
    };
    // (a pipeline comes in uneven chunks from its own thread: 150 ms start-up buffer)
    pl::hooks().audioRoute = [&](int stream, int device) { audio.setPrebuffer(stream, 0.15); audio.setDevice(stream, device); };
    pl::hooks().audioPush = [&](int stream, const std::vector<float>& lr) { audio.pushSamples(lr, stream); };
    pl::hooks().audioClose = [&](int stream) { audio.removeStream(stream); };
    // the receivers a Channel input can take ("RX2  RTL-SDR  101.700"; the first word is the key)
    pl::hooks().receivers = [&]() {
        std::vector<std::string> v;
        for (auto& r : receivers) v.push_back(r->title());
        return v;
    };

    // --- Receivers: each gets a source (none yet), a DSP thread and its own decoders ---
    int nextRxId = 1;
    auto newReceiver = [&]() -> Receiver* {
        auto r = std::make_unique<Receiver>();
        Receiver* rx = r.get();
        rx->id = nextRxId++;
        {
            std::lock_guard<std::mutex> l(rx->sd.sourceMtx);
            rx->sd.source = std::make_shared<FileSource>();   // idle placeholder, reads nothing
            rx->sd.source->open("None");
        }
        rx->sd.aprsHistory = loadLastLogLinesAsPackets(getAprsLogFilePath(), 20);
        rx->centerFreq = rx->sd.centerFreq = savedFreq;
        rx->hostText = settingsMgr.getString("rtltcp_host", "127.0.0.1:1234");
        ModuleContext ctx = { (float)AUDIO_RATE, font };
        auto add = [&](auto* m) { rx->owned.emplace_back(m); m->init(ctx); return m; };
        rx->ais = add(new AISDecoder());
        rx->sstv = add(new SSTVModule());
        rx->sstv->saveDir = pictureDir;
        rx->sstv->setSaveDir = setPictureDir;
        rx->wefax = add(new WEFAXModule());
        rx->wefax->saveDir = pictureDir;
        rx->wefax->setSaveDir = setPictureDir;
        rx->ism = add(new ISMDecoder());
        rx->ism->log.setPath(getIsmLogFilePath());
        rx->ism->exportDir = getDefaultExportPath();
        rx->acars = add(new ACARSDecoder());
        rx->acars->log.setPath(getAcarsLogFilePath());
        rx->acars->exportDir = getDefaultExportPath();
        rx->vdl2 = add(new VDL2Decoder());
        rx->vdl2->log.setPath(getConfigFilePath("vdl2.log"));
        rx->vdl2->exportDir = getDefaultExportPath();
        rx->adsb = add(new ADSBDecoder());
        rx->adsb->onOpenMap = [&]() { mapWindow.openOrFocus(); };
        rx->wsjt = add(new WSJTDecoder());
        rx->wsjt->log.setPath(getConfigFilePath("wsjt.log"));
        rx->wsjt->exportDir = getDefaultExportPath();
        rx->cw = add(new CWDecoder());
        rx->cw->log.setPath(getConfigFilePath("cw.log"));
        rx->navtex = add(new NAVTEXDecoder());
        rx->navtex->log.setPath(getConfigFilePath("navtex.log"));
        rx->navtex->exportDir = getDefaultExportPath();
        rx->dsc = add(new DSCDecoder());
        rx->dsc->log.setPath(getConfigFilePath("dsc.log"));
        rx->dsc->exportDir = getDefaultExportPath();
        rx->dvoice = add(new DigitalVoiceDecoder());
        rx->dvoice->log.setPath(getConfigFilePath("digitalvoice.log"));
        rx->dvoice->exportDir = getDefaultExportPath();
        rx->analyzer = add(new SignalAnalyzer());
        rx->analyzer->rxId = rx->id;
        // "Pipeline >": the tab that takes this receiver, or a new one taking it
        rx->analyzer->onOpenPipeline = [&, rx]() {
            pl::Workspace* w = pl::Hub::get().taking(rx->id);
            if (!w) w = pl::Hub::get().add("", "RX" + std::to_string(rx->id), &font);
            pipeUid = w->uid;
            pipeShown = true;
        };
        // the order of the Decoders checkboxes
        rx->decMods = { nullptr, rx->dvoice, rx->ais, rx->sstv, rx->wefax, rx->ism, rx->acars, rx->vdl2, rx->adsb, rx->wsjt, rx->cw, rx->navtex, rx->dsc, rx->analyzer };
        rx->sd.visible = false;
        rx->sd.audioOut = true;          // its speaker button on the tab (on by default)
        rx->sd.audioStream = rx->id;     // its own stream in the speaker's mix
        audio.setDevice(rx->sd.audioStream, 0);
        rx->dsp = std::thread(dspWorker, std::ref(rx->running), std::ref(rx->sd), std::ref(audio));
        receivers.push_back(std::move(r));
        return rx;
    };
    cur = newReceiver();
    cur->sd.visible = true;
    cur->sd.audioOut = true;

    // Volume slider: the position maps to -40 .. +8 dB, so the low levels are easy to set
    auto volumeGain = [](float pos) { return pos <= 0.001f ? 0.0f : std::pow(10.0f, (pos * 48.0f - 40.0f) / 20.0f); };
    auto volumeText = [](float pos) {
        if (pos <= 0.001f) return std::string("Volume: muted");
        char b[32]; std::snprintf(b, sizeof(b), "Volume: %+.0f dB", pos * 48.0f - 40.0f); return std::string(b);
    };
    float savedVolPos = std::clamp(settingsMgr.getFloat("volume_pos", 0.7f), 0.0f, 1.0f);
    Slider volSlider(150, 0.0f, 1.0f, savedVolPos, volumeText(savedVolPos), font);
    float lastVolPos = -1.0f;
    Slider timeSlider(100, 0.0f, 1.0f, 0.0f, "Timeline", font);

    Sidebar sidebar(SIDEBAR_W, font);

    auto modDecoders = sidebar.addModule("Decoders");
    auto chkAprs = std::make_shared<Checkbox>("APRS (packet radio, 144.800)", font);
    auto btnCopyAprs = std::make_shared<SdrButton>(200, 25, "Copy Last Packet", font);
    auto chkAIS = std::make_shared<Checkbox>("AIS (ships, 161.975 / 162.025)", font);

    auto btnNFM = std::make_shared<SdrButton>(36, 25, "NFM", font);
    auto btnAM  = std::make_shared<SdrButton>(30, 25, "AM", font);
    auto btnWFM = std::make_shared<SdrButton>(38, 25, "WFM", font);
    auto btnUSB = std::make_shared<SdrButton>(34, 25, "USB", font);
    auto btnLSB = std::make_shared<SdrButton>(34, 25, "LSB", font);
    auto btnCW  = std::make_shared<SdrButton>(30, 25, "CW", font);
    auto btnRAW = std::make_shared<SdrButton>(36, 25, "RAW", font);
    auto btnOFF = std::make_shared<SdrButton>(32, 25, "OFF", font);

    auto rowModes = std::make_shared<RowContainer>();
    rowModes->spacing = 3.0f;
    rowModes->add(btnNFM); rowModes->add(btnAM); rowModes->add(btnWFM);
    rowModes->add(btnUSB); rowModes->add(btnLSB); rowModes->add(btnCW); rowModes->add(btnRAW); rowModes->add(btnOFF);

    // Digital voice (second row): DIGITAL finds the system by itself, the others decode only that one
    auto btnDIG   = std::make_shared<SdrButton>(56, 25, "DIGITAL", font);
    auto btnDMR   = std::make_shared<SdrButton>(38, 25, "DMR", font);
    auto btnDPMR  = std::make_shared<SdrButton>(42, 25, "dPMR", font);
    auto btnDSTAR = std::make_shared<SdrButton>(50, 25, "D-STAR", font);
    auto btnYSF   = std::make_shared<SdrButton>(34, 25, "YSF", font);
    auto btnNXDN  = std::make_shared<SdrButton>(42, 25, "NXDN", font);
    auto rowDigital = std::make_shared<RowContainer>();
    rowDigital->spacing = 3.0f;
    rowDigital->add(btnDIG); rowDigital->add(btnDMR); rowDigital->add(btnDPMR);
    rowDigital->add(btnDSTAR); rowDigital->add(btnYSF); rowDigital->add(btnNXDN);
    const std::vector<std::pair<Mode, SdrButton*>> modeButtons = {
        {Mode::NFM, btnNFM.get()}, {Mode::AM, btnAM.get()}, {Mode::WFM, btnWFM.get()}, {Mode::USB, btnUSB.get()},
        {Mode::LSB, btnLSB.get()}, {Mode::CW, btnCW.get()}, {Mode::RAW, btnRAW.get()}, {Mode::OFF, btnOFF.get()},
        {Mode::DIGITAL, btnDIG.get()}, {Mode::DMR, btnDMR.get()}, {Mode::DPMR, btnDPMR.get()},
        {Mode::DSTAR, btnDSTAR.get()}, {Mode::YSF, btnYSF.get()}, {Mode::NXDN, btnNXDN.get()} };
    auto buttonFor = [modeButtons](Mode m) {
        for (const auto& mb : modeButtons) if (mb.first == m) return mb.second;
        return modeButtons[7].second;   // OFF
    };

    // Below the rows while a digital mode is on: what is heard, the DMR time slots
    auto grpDigital = std::make_shared<WidgetGroup>();
    auto lblDigital = std::make_shared<ColorLabel>("Searching...\n \n \n ", font, 11, sf::Color(200, 200, 210));
    auto btnTsBoth = std::make_shared<SdrButton>(70, 22, "TS1 + TS2", font);
    auto btnTs1 = std::make_shared<SdrButton>(64, 22, "TS1 only", font);
    auto btnTs2 = std::make_shared<SdrButton>(64, 22, "TS2 only", font);
    auto rowTs = std::make_shared<RowContainer>();
    rowTs->spacing = 3.0f;
    rowTs->add(btnTsBoth); rowTs->add(btnTs1); rowTs->add(btnTs2);
    auto chkMuteEnc = std::make_shared<Checkbox>("Mute encrypted calls", font, true);
    grpDigital->add(lblDigital);
    grpDigital->add(rowTs);
    grpDigital->add(chkMuteEnc);
    if (!dv::available())
        grpDigital->add(std::make_shared<Label>("Digital voice is not in this build", font, 11, sf::Color(230, 150, 120)));
    else if (!dv::vocoderAvailable())
        grpDigital->add(std::make_shared<Label>("Built without the voice codec: info only, no speech", font, 11, sf::Color(230, 150, 120)));
    grpDigital->setShown(false);

    auto slBW = std::make_shared<Slider>(SIDEBAR_W - 40, 1000.0f, 200000.0f, 12500.0f, "Bandwidth (Hz)", font);

    // Input signal type: I/Q from a radio, or audio that is already demodulated (sound card,
    // a receiver's audio output, audio recordings). Set automatically when a source is opened,
    // the user can change it. Every demodulator and decoder follows this switch.
    std::function<void(bool)> applyInputType;   // assigned once the demodulator buttons exist
    auto lblInput = std::make_shared<Label>("Input signal:", font);
    auto btnInIQ = std::make_shared<SdrButton>(136, 25, "I/Q (radio)", font);
    auto btnInAudio = std::make_shared<SdrButton>(136, 25, "Audio", font);
    btnInIQ->setActive(true);
    btnInIQ->onClick = [&]() { if (applyInputType) applyInputType(false); };
    btnInAudio->onClick = [&]() { if (applyInputType) applyInputType(true); };
    auto rowInput = std::make_shared<RowContainer>();
    rowInput->add(btnInIQ); rowInput->add(btnInAudio);

    auto modSource = sidebar.addModule("Source / Input");
    auto ddSourceType = std::make_shared<Dropdown>(SIDEBAR_W - 40, 25.0f, font);
    ddSourceType->setOptions({"File Source", "RTL-SDR", "SDRPlay", "Sound Card (Line-In)", "RTL-TCP (Network)"});
    modSource->addWidget(std::make_shared<Label>("Source Type:", font));
    modSource->addWidget(ddSourceType);

    auto lblDevice = std::make_shared<Label>("Select Device:", font);
    auto ddDevice = std::make_shared<Dropdown>(SIDEBAR_W - 40, 25.0f, font);
    auto btnRefresh = std::make_shared<SdrButton>(SIDEBAR_W - 40, 25, "Refresh Device List", font);
    auto ddRate = std::make_shared<Dropdown>(SIDEBAR_W - 40, 25.0f, font);
    ddRate->setOptions({"None"});

    auto lblHost = std::make_shared<Label>("Server (host:port):", font);
    auto txtHost = std::make_shared<TextInput>(SIDEBAR_W - 40, 25.0f, font);
    txtHost->setText(settingsMgr.getString("rtltcp_host", "127.0.0.1:1234"));
    auto btnConnect = std::make_shared<SdrButton>(SIDEBAR_W - 40, 25, "Connect", font);

    auto modRadio = sidebar.addModule("Radio Control");
    auto slGain = std::make_shared<Slider>(SIDEBAR_W - 40, 0.0f, 50.0f, 0.0f, "RF Gain (dB)", font);
    auto chkAgc = std::make_shared<Checkbox>("Automatic Gain Control (AGC)", font, true);


    slGain->onChange = [&](float v) {
        std::lock_guard<std::mutex> l(cur->sd.mtx);
        cur->sd.rfGain = v;
        std::stringstream ss;
        ss << std::fixed << std::setprecision(1);
        if (cur->sourceType == 1 || cur->sourceType == 4) {
            ss << "RF Gain: " << v << " dB";
        } else {
            ss << "RF Gain (dB): " << (int)v;
        }
        slGain->setText(ss.str());
        cur->pendingRfGain = v;
    };

    chkAgc->onToggle = [&](bool b) {
        if (!b) {
            // Switching to manual: carry on from the gain the AGC had chosen, and show it as manual
            float agcGain = -1.0f;
            {
                std::lock_guard<std::mutex> l(cur->sd.sourceMtx);
                if (cur->sd.source) agcGain = cur->sd.source->getAutoGainDb();
            }
            if (agcGain >= 0.0f) slGain->setValueSilent(agcGain);
            slGain->onChange(slGain->currentVal);   // updates the label and queues the gain
        }
        std::lock_guard<std::mutex> l(cur->sd.mtx);
        cur->sd.rfGain = b ? -1.0f : slGain->currentVal;
        slGain->setEnabled(!b);
        cur->pendingRfGain = b ? -1.0f : slGain->currentVal;
    };
    slGain->setEnabled(false);

    auto lblAntenna = std::make_shared<Label>("Antenna (SDRPlay):", font);
    auto ddAntenna = std::make_shared<Dropdown>(SIDEBAR_W - 40, 25.0f, font);
    ddAntenna->setOptions({"Antenna A / Default", "Antenna B", "Antenna C / Hi-Z"});
    ddAntenna->onChange = [&](int idx) {
        cur->sd.hw.antennaIndex = idx;
        std::lock_guard<std::mutex> l(cur->sd.sourceMtx);
        if(cur->sd.source) cur->sd.source->setHardwareOption("antenna", idx);
    };

    auto lblDirectSamp = std::make_shared<Label>("Direct Sampling (RTL):", font);
    auto ddDirectSamp = std::make_shared<Dropdown>(SIDEBAR_W - 40, 25.0f, font);
    ddDirectSamp->setOptions({"Off (Default)", "I-ADC", "Q-ADC"});
    ddDirectSamp->onChange = [&](int idx) {
        cur->sd.hw.directSampling = idx;
        std::lock_guard<std::mutex> l(cur->sd.sourceMtx);
        if(cur->sd.source) cur->sd.source->setHardwareOption("direct_sampling", idx);
    };

    auto rowFilters = std::make_shared<RowContainer>();
    auto chkFmNotch = std::make_shared<Checkbox>("FM Notch", font);
    chkFmNotch->onToggle = [&](bool b) {
        cur->sd.hw.fmNotch = b;
        std::lock_guard<std::mutex> l(cur->sd.sourceMtx);
        if(cur->sd.source) cur->sd.source->setHardwareOption("fm_notch", b?1:0);
    };
    auto chkMwNotch = std::make_shared<Checkbox>("MW Notch", font);
    chkMwNotch->onToggle = [&](bool b) {
        cur->sd.hw.mwNotch = b;
        std::lock_guard<std::mutex> l(cur->sd.sourceMtx);
        if(cur->sd.source) cur->sd.source->setHardwareOption("mw_notch", b?1:0);
    };
    rowFilters->add(chkFmNotch);
    rowFilters->add(chkMwNotch);

    // Frequency correction (ppm): an RTL-SDR without TCXO is often 20-60 ppm off, a few kHz
    // on 2 m - enough to lose APRS / narrow FM and to misplace FT8 / CW
    auto slPpm = std::make_shared<Slider>(SIDEBAR_W - 40, -100.0f, 100.0f, 0.0f, "Frequency correction: 0 ppm", font);
    auto ppmText = [](int v) { return "Frequency correction: " + std::string(v > 0 ? "+" : "") + std::to_string(v) + " ppm"; };
    slPpm->onChange = [&, ppmText](float v) {
        const int ppm = (int)std::lround(v);
        slPpm->setText(ppmText(ppm));
        if (ppm == cur->sd.hw.ppm) return;
        cur->sd.hw.ppm = ppm;
        std::lock_guard<std::mutex> l(cur->sd.sourceMtx);
        if (cur->sd.source) cur->sd.source->setHardwareOption("ppm", ppm);
    };

    auto chkBiasT = std::make_shared<Checkbox>("Bias-T Power", font);
    chkBiasT->onToggle = [&](bool b) {
        cur->sd.hw.biasT = b;
        std::lock_guard<std::mutex> l(cur->sd.sourceMtx);
        if(cur->sd.source) cur->sd.source->setHardwareOption("bias_t", b?1:0);
    };

    auto updateRadioControls = [&](int sourceIdx) {
        modRadio->widgets.clear();
        if (sourceIdx != 0) {
            modRadio->addWidget(slGain);
            modRadio->addWidget(chkAgc);
        }
        if (sourceIdx == 1 || sourceIdx == 2 || sourceIdx == 4) {
            modRadio->addWidget(slPpm);
            slPpm->setValueSilent((float)cur->sd.hw.ppm);
            slPpm->setText(ppmText(cur->sd.hw.ppm));
        }
        if (sourceIdx == 1 || sourceIdx == 4) {
            modRadio->addWidget(lblDirectSamp);
            modRadio->addWidget(ddDirectSamp);
            modRadio->addWidget(chkBiasT);
            ddDirectSamp->setSelection(cur->sd.hw.directSampling);
            chkBiasT->checked = cur->sd.hw.biasT;
        } else if (sourceIdx == 2) {
            modRadio->addWidget(lblAntenna);
            modRadio->addWidget(ddAntenna);
            modRadio->addWidget(rowFilters);
            modRadio->addWidget(chkBiasT);
            ddAntenna->setSelection(cur->sd.hw.antennaIndex);
            chkFmNotch->checked = cur->sd.hw.fmNotch;
            chkMwNotch->checked = cur->sd.hw.mwNotch;
            chkBiasT->checked = cur->sd.hw.biasT;
        }
        sidebar.recalculateLayout();
        sidebar.updateStyle();
    };
    updateRadioControls(0);


    // Startup restores the last source silently; errors are only explained when the user
    // picks a source or refreshes the device list
    bool quietSourceErrors = false;

    std::function<void(int, std::string, int, std::string)> doOpenSource = [&](int sourceIdx, std::string deviceID, int rateIdx, std::string pathOverride) {
        {
            std::lock_guard<std::mutex> lock(cur->sd.sourceMtx);
            if (cur->sd.source) {
                cur->sd.source->stop();
                cur->sd.source->close();
                cur->sd.source = nullptr;
            }
        }
        cur->openType = -1;
        cur->openDevice.clear();
        // Whatever happens next, we are not connected to an RTL-TCP server any more
        cur->tcpConnected = false;
        btnConnect->setText("Connect");
        btnConnect->setActive(false);

        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        std::shared_ptr<IQSource> newSource;
        uint32_t targetRate = 0;
        bool success = false;
        cur->sourceType = sourceIdx;
        updateRadioControls(sourceIdx);
        if (sourceIdx != 0 && sourceIdx != 3 && applyInputType) applyInputType(false);   // radios: always I/Q

        if (sourceIdx == 0) {
            newSource = std::make_shared<FileSource>();
            std::string path = pathOverride.empty() ? "None" : pathOverride;
            success = newSource->open(path);
            if (!success && path != "None")
                showPopup("Error", "Could not open the file:\n" + path + "\n\nOnly WAV files are supported: stereo (I/Q) or mono (audio).");
            {
                std::lock_guard<std::mutex> l(cur->sd.mtx);
                cur->sd.currentFilename = path;
            }
            std::string fileName = path.substr(path.find_last_of("/\\") + 1);
            ddRate->setOptions({fileName, "[ Change File... ]"});
            ddRate->setSelection(0);

            const long long f = orbit::frequencyFromName(path);   // SDR++, SDRuno, HDSDR, OrbitSDR ...
            cur->centerFreq = f;
            freqVFO.setFrequency(f);
            { std::lock_guard<std::mutex> l(cur->sd.mtx); cur->sd.centerFreq = f; }
        }
        else if (sourceIdx == 1) {
            targetRate = (rateIdx < RTL_RATES_VAL.size()) ? RTL_RATES_VAL[rateIdx] : 2048000;
            auto rtl = std::make_shared<RtlSdrSource>();
            ddRate->setOptions(rtl->getAvailableSampleRatesText());
            ddRate->setSelection(rateIdx);
            success = rtl->open(deviceID, targetRate);
            newSource = rtl;

            if (cur->centerFreq < 1000000) cur->centerFreq = 97800000;
            freqVFO.setFrequency(cur->centerFreq);
            newSource->setCenterFrequency(cur->centerFreq);
            {
                std::lock_guard<std::mutex> l(cur->sd.mtx);
                cur->sd.tunedFreqPercent = 0.5;
                cur->sd.viewCenterPct = 0.5;
                cur->sd.centerFreq = cur->centerFreq;
            }
            slGain->setLimits(0.0f, 49.6f);
            slGain->setText("RF Gain: AGC");
            newSource->setHardwareOption("direct_sampling", cur->sd.hw.directSampling);
            newSource->setHardwareOption("bias_t", cur->sd.hw.biasT ? 1 : 0);
            newSource->setHardwareOption("ppm", cur->sd.hw.ppm);
        }
        else if (sourceIdx == 2) {
            targetRate = (rateIdx < SDRPLAY_RATES_VAL.size()) ? SDRPLAY_RATES_VAL[rateIdx] : 2000000;
            auto sdr = std::make_shared<SdrPlaySource>();
            ddRate->setOptions(sdr->getAvailableSampleRatesText());
            ddRate->setSelection(rateIdx);
            success = sdr->open(deviceID, targetRate);
            newSource = sdr;

            if (cur->centerFreq < 1000000) cur->centerFreq = 97800000;
            freqVFO.setFrequency(cur->centerFreq);
            newSource->setCenterFrequency(cur->centerFreq);
            {
                std::lock_guard<std::mutex> l(cur->sd.mtx);
                cur->sd.tunedFreqPercent = 0.5;
                cur->sd.viewCenterPct = 0.5;
                cur->sd.centerFreq = cur->centerFreq;
            }
            slGain->setLimits(0.0f, 50.0f);
            chkAgc->checked = true;
            slGain->setEnabled(false);
            cur->sd.rfGain = -1.0f;

            newSource->setHardwareOption("antenna", cur->sd.hw.antennaIndex);
            newSource->setHardwareOption("fm_notch", cur->sd.hw.fmNotch ? 1 : 0);
            newSource->setHardwareOption("mw_notch", cur->sd.hw.mwNotch ? 1 : 0);
            newSource->setHardwareOption("bias_t", cur->sd.hw.biasT ? 1 : 0);
            newSource->setHardwareOption("ppm", cur->sd.hw.ppm);
        }
        else if (sourceIdx == 3) {
            // Sound card / line-in used as a baseband source
            auto audioIn = std::make_shared<AudioCaptureSource>();
            std::vector<uint32_t> audioRates = audioIn->getAvailableSampleRatesValues();
            if (rateIdx < 0 || rateIdx >= (int)audioRates.size()) rateIdx = 0;
            targetRate = audioRates.empty() ? 48000 : audioRates[rateIdx];
            ddRate->setOptions(audioIn->getAvailableSampleRatesText());
            ddRate->setSelection(rateIdx);
            success = audioIn->open(deviceID, targetRate);
            newSource = audioIn;

            // Baseband signal: no hardware centre frequency, show 0 Hz (same as file playback)
            cur->centerFreq = 0;
            freqVFO.setFrequency(0);
            {
                std::lock_guard<std::mutex> l(cur->sd.mtx);
                cur->sd.tunedFreqPercent = 0.5;
                cur->sd.viewCenterPct = 0.5;
                cur->sd.centerFreq = 0;
            }
        }
        else if (sourceIdx == 4) {
            targetRate = (rateIdx < (int)RTL_RATES_VAL.size()) ? RTL_RATES_VAL[rateIdx] : 2048000;
            auto tcp = std::make_shared<RtlTcpSource>();
            ddRate->setOptions(tcp->getAvailableSampleRatesText());
            ddRate->setSelection(rateIdx);
            success = tcp->open(deviceID, targetRate);   // deviceID = "host:port"
            newSource = tcp;

            if (cur->centerFreq < 1000000) cur->centerFreq = 97800000;
            freqVFO.setFrequency(cur->centerFreq);
            newSource->setCenterFrequency(cur->centerFreq);
            {
                std::lock_guard<std::mutex> l(cur->sd.mtx);
                cur->sd.tunedFreqPercent = 0.5;
                cur->sd.viewCenterPct = 0.5;
                cur->sd.centerFreq = cur->centerFreq;
            }
            slGain->setLimits(0.0f, 49.6f);
            slGain->setText("RF Gain: AGC");
            newSource->setHardwareOption("direct_sampling", cur->sd.hw.directSampling);
            newSource->setHardwareOption("bias_t", cur->sd.hw.biasT ? 1 : 0);
            newSource->setHardwareOption("ppm", cur->sd.hw.ppm);
        }

        if (!success && sourceIdx != 0) {
            if (!quietSourceErrors) showPopup("Error", "Could not open device.");
            return;
        }

        {
            std::lock_guard<std::mutex> lock(cur->sd.sourceMtx);
            cur->sd.source = newSource;
        }
        if (sourceIdx >= 1 && sourceIdx <= 3) { cur->openType = sourceIdx; cur->openDevice = deviceID; }
        // Sound card and mono WAV files are audio, everything else is I/Q (the user can change it)
        if (applyInputType) applyInputType(newSource && newSource->isAudio());

        // RTL-TCP connected successfully -> the button turns into "Disconnect"
        if (sourceIdx == 4) {
            cur->tcpConnected = true;
            cur->tcpAddr = deviceID;
            btnConnect->setText("Disconnect");
            btnConnect->setActive(true);
        }

        {
            std::lock_guard<std::mutex> lock(cur->sd.mtx);
            cur->sd.isPlaying = false;
            cur->sd.isRecording = false;
        }
        btnPlay.setText(">");
        btnPlay.setActive(false);
        audio.clear(cur->sd.audioStream);
    };

    // Cleanly drops the RTL-TCP connection and leaves the app in an idle (no source) state
    std::function<void()> doDisconnectTcp = [&]() {
        {
            std::lock_guard<std::mutex> lock(cur->sd.sourceMtx);
            if (cur->sd.source) {
                cur->sd.source->stop();
                cur->sd.source->close();
            }
            cur->sd.source = std::make_shared<FileSource>();   // idle placeholder, reads nothing
        }
        {
            std::lock_guard<std::mutex> lock(cur->sd.mtx);
            cur->sd.isPlaying = false;
            cur->sd.isRecording = false;
        }
        btnPlay.setText(">");
        btnPlay.setActive(false);
        audio.clear(cur->sd.audioStream);

        cur->tcpConnected = false;
        btnConnect->setText("Connect");
        btnConnect->setActive(false);
    };


    std::function<void()> refreshSourceUI = [&]() {
        modSource->widgets.clear();
        modSource->addWidget(std::make_shared<Label>("Source Type:", font));
        modSource->addWidget(ddSourceType);
        // Only files and the sound card can be either; radios always deliver I/Q
        if (ddSourceType->selectedIndex == 0 || ddSourceType->selectedIndex == 3) {
            modSource->addWidget(lblInput);
            modSource->addWidget(rowInput);
        }

        if (ddSourceType->selectedIndex == 0) {
            modSource->addWidget(std::make_shared<Label>("Loaded File:", font));
            std::string fname;
            { std::lock_guard<std::mutex> l(cur->sd.mtx); fname = cur->sd.currentFilename; }
            if (fname == "None" || fname.empty()) {
                ddRate->setOptions({"[ Select File... ]"});
            } else {
                std::string shortName = fname.substr(fname.find_last_of("/\\") + 1);
                ddRate->setOptions({shortName, "[ Change File... ]"});
            }
            modSource->addWidget(ddRate);
        } else if (ddSourceType->selectedIndex == 4) {
            modSource->addWidget(lblHost);
            modSource->addWidget(txtHost);
            modSource->addWidget(btnConnect);
            modSource->addWidget(std::make_shared<Label>("Sample Rate:", font));
            modSource->addWidget(ddRate);
        } else {
            modSource->addWidget(lblDevice);
            modSource->addWidget(ddDevice);
            modSource->addWidget(btnRefresh);
            modSource->addWidget(std::make_shared<Label>("Sample Rate:", font));
            modSource->addWidget(ddRate);
        }
        sidebar.recalculateLayout();
        sidebar.updateStyle();
    };

    // No SDRplay device: tell the user why (service stopped / API missing / no device).
    // May refill cur->cachedDevices when the user lets us start the API service.
    auto explainNoSdrplay = [&]() {
        // the API only lists devices nobody uses: is it open in another tab?
        for (auto& r : receivers)
            if (r.get() != cur && r->openType == 2) {
                showPopup("SDRplay", "The SDRplay device is already open in RX" + std::to_string(r->id) +
                          ".\n\nA device can be used by one tab at a time.");
                return;
            }
#ifdef _WIN32
        const std::string svc = sdrplayServiceState();
        if (svc == "stopped") {
            // The service crashed or was stopped: offer to start it (needs administrator rights)
            int answer = MessageBoxA(NULL,
                "The SDRplay API Service is not running (it may have crashed).\n"
                "OrbitSDR and other SDRplay API programs need it to see the device.\n"
                "(SDRconnect doesn't use it, so it can still work.)\n\n"
                "Start the service now? Windows will ask for administrator permission.",
                "SDRplay", MB_YESNO | MB_ICONWARNING);
            if (answer == IDYES && sdrplayStartService()) {
                for (int i = 0; i < 20 && sdrplayServiceState() != "running"; i++)
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                cur->cachedDevices = SdrPlaySource::getDeviceList();
            }
            if (cur->cachedDevices.empty() && answer == IDYES)
                showPopup("SDRplay", "No SDRplay device found yet.\n\nIf the service didn't start, open 'Services' (services.msc), select 'SDRplay API Service' and click Start - or restart the computer.");
        } else if (svc == "missing") {
            showPopup("SDRPlay Info", "The SDRplay API is not installed.\n\nIf you want to use SDRPlay, please install the official API from:\nhttps://www.sdrplay.com/downloads/");
        } else {
            showPopup("SDRPlay Info", "No SDRPlay device found.\n\nCheck the USB connection and close other programs that use the device (e.g. SDRuno or SDRconnect).");
        }
#else
        showPopup("SDRPlay Info", "No SDRplay device found.\n\n"
                  "- Close SDRconnect / SDRuno / another OrbitSDR: they keep the device, and the SDRplay API "
                  "(which OrbitSDR uses) then doesn't see it. SDRconnect works without the API, so it can still show the device.\n"
                  "- Check the USB cable, then press Refresh Device List.\n"
                  "- No API installed? Get it from https://www.sdrplay.com/downloads/");
#endif
    };

    // The receiver (other than the shown one) that has this device open, or nullptr
    auto deviceUser = [&](int type, const std::string& id) -> Receiver* {
        for (auto& r : receivers)
            if (r.get() != cur && r->openType == type && r->openDevice == id) return r.get();
        return nullptr;
    };

    ddSourceType->onChange = [&](int idx) {
        // Switching away from RTL-TCP while connected: drop the connection first
        if (idx != 4 && cur->tcpConnected) doDisconnectTcp();

        if (idx == 0) {
            std::string path = openFileDialog();
            if (!path.empty()) {
                doOpenSource(0, "", 0, path);
                refreshSourceUI();
            } else refreshSourceUI();
        }
        else if (idx == 4) {
            if (!cur->tcpConnected) {
                ddRate->setOptions(RTL_SAMPLE_RATES_TXT);
                ddRate->setSelection(2);
            }
            refreshSourceUI();   // shows host:port, connection is made only by the Connect button
        } else {
            // Release this tab's own device first: the drivers only list devices nobody has
            // open (picking SDRplay again while it runs here showed "No Devices Found")
            if (cur->openType >= 1) {
                {
                    std::lock_guard<std::mutex> lock(cur->sd.sourceMtx);
                    if (cur->sd.source) { cur->sd.source->stop(); cur->sd.source->close(); }
                    cur->sd.source = std::make_shared<FileSource>();   // idle placeholder
                }
                cur->openType = -1;
                cur->openDevice.clear();
                { std::lock_guard<std::mutex> l(cur->sd.mtx); cur->sd.isPlaying = false; }
                btnPlay.setText(">");
                btnPlay.setActive(false);
            }
            cur->cachedDevices.clear();
            std::vector<std::string> names;

            if (idx == 1) cur->cachedDevices = RtlSdrSource::getDeviceList();
            else if (idx == 2) {
                cur->cachedDevices = SdrPlaySource::getDeviceList();
                if (cur->cachedDevices.empty() && !quietSourceErrors) explainNoSdrplay();
            }
            else if (idx == 3) cur->cachedDevices = AudioCaptureSource::getDeviceList();

            for(auto& d : cur->cachedDevices) names.push_back(d.name);
            if (names.empty()) names.push_back("No Devices Found");
            ddDevice->setOptions(names);

            refreshSourceUI();

            // the first device no other receiver is using
            int pick = -1;
            for (size_t d = 0; d < cur->cachedDevices.size() && pick < 0; d++)
                if (!deviceUser(idx, cur->cachedDevices[d].id)) pick = (int)d;
            if (pick >= 0) {
                ddDevice->setSelection(pick);
                doOpenSource(idx, cur->cachedDevices[pick].id, 0, "");
            } else if (!cur->cachedDevices.empty() && !quietSourceErrors) {
                showPopup("Source", "Every device of this type is already used by another receiver (tab).");
            }
        }
    };

    ddDevice->onChange = [&](int idx) {
        if (idx >= 0 && idx < (int)cur->cachedDevices.size()) {
            if (Receiver* u = deviceUser(ddSourceType->selectedIndex, cur->cachedDevices[idx].id)) {
                showPopup("Source", "This device is already used by RX" + std::to_string(u->id) + ".");
                return;
            }
            doOpenSource(ddSourceType->selectedIndex, cur->cachedDevices[idx].id, ddRate->selectedIndex, "");
        }
    };

    btnRefresh->onClick = [&]() {
        {
            std::lock_guard<std::mutex> lock(cur->sd.sourceMtx);
            if (cur->sd.source) {
                cur->sd.source->stop();
                cur->sd.source->close();
                cur->sd.source = nullptr;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        int idx = ddSourceType->selectedIndex;
        if (idx == 1) cur->cachedDevices = RtlSdrSource::getDeviceList();
        else if (idx == 2) {
            cur->cachedDevices = SdrPlaySource::getDeviceList();
            if (cur->cachedDevices.empty()) explainNoSdrplay();
        }
        else if (idx == 3) cur->cachedDevices = AudioCaptureSource::getDeviceList();

        std::vector<std::string> names;
        for(auto& d : cur->cachedDevices) names.push_back(d.name);

        if (names.empty()) {
            names.push_back("No Devices Found");
            ddDevice->setOptions(names);
        } else {
            ddDevice->setOptions(names);
            ddDevice->setSelection(0);
            doOpenSource(idx, cur->cachedDevices[0].id, ddRate->selectedIndex, "");
        }
    };

    // Connect <-> Disconnect toggle
    btnConnect->onClick = [&]() {
        if (cur->tcpConnected) {
            doDisconnectTcp();
            return;
        }
        std::string addr = txtHost->getText();
        if (addr.empty()) return;
        doOpenSource(4, addr, ddRate->selectedIndex, "");
    };

    // Enter in the host field only CONNECTS (it must never disconnect an active session)
    txtHost->onSubmit = [&](const std::string&) {
        if (!cur->tcpConnected && btnConnect->onClick) btnConnect->onClick();
    };

    ddRate->onChange = [&](int idx) {
        if (ddSourceType->selectedIndex == 0) {
            std::string currentOpt = "";
            if(idx >= 0 && idx < ddRate->options.size()) currentOpt = ddRate->options[idx];

            if (currentOpt == "[ Select File... ]" || currentOpt == "[ Change File... ]") {
                std::string path = openFileDialog();
                if (!path.empty()) doOpenSource(0, "", 0, path);
                else refreshSourceUI();
            }
        }
        else if (ddSourceType->selectedIndex == 4) {
            // Changing the sample rate re-connects to the same server, but only while connected
            if (cur->tcpConnected && !cur->tcpAddr.empty()) doOpenSource(4, cur->tcpAddr, idx, "");
        } else if (ddSourceType->selectedIndex != 0 && !cur->cachedDevices.empty()) {
            doOpenSource(ddSourceType->selectedIndex, cur->cachedDevices[ddDevice->selectedIndex].id, idx, "");
        }
    };

    refreshSourceUI();

    auto modAudio = sidebar.addModule("Audio Output");
    auto ddAudio = std::make_shared<Dropdown>(SIDEBAR_W - 40, 25.0f, font);
    ddAudio->setOptions(deviceNames);
    // per receiver: each tab can play on its own device (speakers, headphones, a virtual cable)
    auto lblAudioDev = std::make_shared<Label>("Output of this receiver (per tab):", font, 11, sf::Color(150, 150, 160));
    ddAudio->onChange = [&](int idx) {
        if (!audio.setDevice(cur->sd.audioStream, idx)) {
            ddAudio->setSelection(0);
            idx = 0;
        }
        cur->audioDevice = audio.deviceName(idx);
    };
    modAudio->addWidget(lblAudioDev);
    modAudio->addWidget(ddAudio);
    {
        // which sound system plays (Linux: PulseAudio / PipeWire or ALSA). "Null" = none was found
        const bool none = audio.backendName == "Null" || audio.backendName == "none";
        auto lblAudioSys = std::make_shared<Label>(
            none ? "No sound system found (PulseAudio/ALSA)"
                 : "Sound system: " + audio.backendName,
            font, 11, none ? sf::Color(255, 150, 90) : sf::Color(120, 120, 130));
        modAudio->addWidget(lblAudioSys);
    }

    auto modDemod = sidebar.addModule("Demodulator");
    modDemod->addWidget(rowModes);
    modDemod->addWidget(rowDigital);
    modDemod->addWidget(grpDigital);

    slBW->onChange = [&](float v) {
        std::lock_guard<std::mutex> l(cur->sd.mtx);
        cur->sd.bandwidth = v;
        char b[48];
        if (v >= 10000) std::snprintf(b, sizeof(b), "Bandwidth: %.0f kHz", v / 1000.0f);
        else if (v >= 1000) std::snprintf(b, sizeof(b), "Bandwidth: %.1f kHz", v / 1000.0f);
        else std::snprintf(b, sizeof(b), "Bandwidth: %.0f Hz", v);
        slBW->setText(b);
    };
    slBW->onChange(12500.0f);
    modDemod->addWidget(slBW);

    auto slSq = std::make_shared<Slider>(SIDEBAR_W - 40, -100.0f, 0.0f, -100.0f, "Squelch (dB)", font);
    slSq->onChange = [&](float v) { std::lock_guard<std::mutex> l(cur->sd.mtx); cur->sd.squelchThreshold = v; };
    modDemod->addWidget(slSq);

    auto chkStereo = std::make_shared<Checkbox>("Stereo (WFM only)", font);
    chkStereo->onToggle = [&](bool b) { std::lock_guard<std::mutex> l(cur->sd.mtx); cur->sd.stereoEnabled = b; };
    modDemod->addWidget(chkStereo);

    auto chkRds = std::make_shared<Checkbox>("RDS (WFM only)", font);
    chkRds->onToggle = [&](bool b) { std::lock_guard<std::mutex> l(cur->sd.mtx); cur->sd.rdsEnabled = b; };
    modDemod->addWidget(chkRds);

    // --- Demodulator mode buttons (only for listening: the decoders have their own channels) ---
    auto setMode = [&](Mode m, SdrButton* me) {
        // audio input: only RAW (and the digital voice modes, which decode discriminator audio)
        if (cur->inputIsAudio && m != Mode::RAW && m != Mode::OFF && !isDigitalMode(m)) { m = Mode::RAW; me = btnRAW.get(); }
        if (isDigitalMode(m) && !dv::available()) { m = Mode::NFM; me = btnNFM.get(); }

        rowModes->setEnabled(true);
        rowDigital->setEnabled(dv::available());
        for (const auto& mb : modeButtons) mb.second->setActive(false);
        me->setActive(true);
        if (cur->inputIsAudio) {   // audio input: the other demodulators make no sense
            btnNFM->setEnabled(false); btnAM->setEnabled(false); btnWFM->setEnabled(false);
            btnUSB->setEnabled(false); btnLSB->setEnabled(false); btnCW->setEnabled(false);
        }
        if (grpDigital->setShown(isDigitalMode(m))) sidebar.recalculateLayout();
        rowTs->setEnabled(m == Mode::DIGITAL || m == Mode::DMR);

        std::lock_guard<std::mutex> l(cur->sd.mtx);
        cur->sd.mode = m;

        if (m == Mode::WFM) {
            slBW->setLimits(50000, 200000); slBW->setValueSilent(180000);
            cur->sd.bandwidth = 180000; slBW->setText("Bandwidth: 180 kHz");
        } else if (m == Mode::RAW) {
            slBW->setLimits(1000, 200000); slBW->setValueSilent(48000);
            cur->sd.bandwidth = 48000; slBW->setText("Bandwidth: 48.0 kHz");
        } else if (m == Mode::NFM || m == Mode::AM) {
            slBW->setLimits(4000, 40000); slBW->setValueSilent(12500);
            cur->sd.bandwidth = 12500; slBW->setText("Bandwidth: 12.5 kHz");
        } else if (m == Mode::CW) {
            slBW->setLimits(50, 3000); slBW->setValueSilent(500);
            cur->sd.bandwidth = 500; slBW->setText("Bandwidth: 500 Hz");
        } else if (m == Mode::DPMR) {
            slBW->setLimits(4000, 25000); slBW->setValueSilent(7000);
            cur->sd.bandwidth = 7000; slBW->setText("Bandwidth: 7.0 kHz");
        } else if (isDigitalMode(m)) {
            slBW->setLimits(4000, 25000); slBW->setValueSilent(12500);
            cur->sd.bandwidth = 12500; slBW->setText("Bandwidth: 12.5 kHz");
        } else if (m == Mode::OFF) {
            // no demodulator: the tuner's width only feeds the decoders (e.g. a wide Analyzer
            // channel for LRPT); the previous mode's limit (NFM 40 kHz) must not hold it back
            slBW->setLimits(1000, 2000000);
            slBW->setValueSilent((float)cur->sd.bandwidth);
        } else {
            slBW->setLimits(1000, 10000); slBW->setValueSilent(3000);
            cur->sd.bandwidth = 3000; slBW->setText("Bandwidth: 3 kHz");
        }
    };

    for (const auto& mb : modeButtons) {
        const Mode m = mb.first;
        SdrButton* b = mb.second;
        b->onClick = [&setMode, m, b]() { setMode(m, b); };
    }
    auto restoreMode = [&](Mode m) { setMode(m, buttonFor(m)); };

    auto setTsUi = [&](int mask) {
        btnTsBoth->setActive(mask == 3); btnTs1->setActive(mask == 1); btnTs2->setActive(mask == 2);
    };
    setTsUi(3);
    btnTsBoth->onClick = [&]() { setTsUi(3); std::lock_guard<std::mutex> l(cur->sd.mtx); cur->sd.digSlotMask = 3; };
    btnTs1->onClick = [&]() { setTsUi(1); std::lock_guard<std::mutex> l(cur->sd.mtx); cur->sd.digSlotMask = 1; };
    btnTs2->onClick = [&]() { setTsUi(2); std::lock_guard<std::mutex> l(cur->sd.mtx); cur->sd.digSlotMask = 2; };
    chkMuteEnc->onToggle = [&](bool b) { std::lock_guard<std::mutex> l(cur->sd.mtx); cur->sd.digMuteEncrypted = b; };

    // Four lines about the digital voice signal for the demodulator panel
    sf::Clock digitalUiClock;
    auto digitalStatusText = [](const dv::Status& ds, Mode m) {
        std::string l1, l2, l3, l4;
        if (ds.proto.empty() || ds.sinceSync > 3.0) {
            l1 = m == Mode::DIGITAL ? "Searching: DMR, dPMR, D-STAR, YSF, NXDN..." : std::string("Searching for ") + modeName(m) + "...";
            l2 = ds.everSync ? "(signal lost)" : "Tune to a digital voice channel";
        } else {
            l1 = ds.proto;
            for (const auto& f : ds.fields) { if (f.first == "Color code" || f.first == "Colour code") l1 += "   CC " + f.second; else if (f.first == "RAN") l1 += "   RAN " + f.second; }
            if (!ds.station.empty()) l1 += "   " + ds.station;
            if (!ds.sync) l1 += "   (no frames)";
            auto slotLine = [&](int sl) {
                const dv::SlotView& v = ds.slot[sl];
                std::string t = ds.nSlots > 1 ? "TS" + std::to_string(sl + 1) + ": " : "";
                if (v.voice) t += v.encrypted ? "ENCRYPTED  " : "VOICE  ";
                else t += v.activity.empty() ? "-" : v.activity;
                if (!v.from.empty() || !v.to.empty()) t += (v.voice ? "" : "   last ") + v.from + (v.to.empty() ? "" : " > " + v.to);
                if (v.voice && v.voiceSec > 0.5) t += "  " + std::to_string((int)v.voiceSec) + " s";
                return t;
            };
            l2 = slotLine(0);
            if (ds.nSlots > 1) l3 = slotLine(1);
            // talker alias, encryption details, emergency / GPS of the slot that speaks
            int sp = -1;
            for (int sl = 0; sl < ds.nSlots && sl < 2; sl++) if (ds.slot[sl].voice) { sp = sl; break; }
            if (sp >= 0) {
                const dv::SlotView& v = ds.slot[sp];
                if (v.encrypted && !v.encText.empty()) l4 = "Encrypted: " + v.encText;
                else if (!v.alias.empty()) l4 = "Alias: " + v.alias;
                else if (!v.extra.empty()) l4 = v.extra;
            } else if (!ds.system.empty()) l4 = ds.system;
            if (ds.nSlots == 1) { l3 = l4; l4.clear(); }
        }
        auto cut = [](std::string s) { return s.size() > 52 ? s.substr(0, 51) + "." : s; };
        return cut(l1) + "\n" + cut(l2) + "\n" + cut(l3) + "\n" + cut(l4);
    };

    // --- Bookmarks and the scanner (see Bookmarks.h) ---
    BookmarkStore bookmarks;
    bookmarks.load(getConfigFilePath("bookmarks.json"));
    Scanner scanner;
    int pendingBookmarkTune = -1;   // applied in the main loop (the tuning helpers live there)
    sf::Clock scanClock;
    unsigned long long lastLevelSeq = 0;
    auto modBook = sidebar.addModule("Bookmarks / Scanner");
    auto ddBank = std::make_shared<Dropdown>(SIDEBAR_W - 40, 25.0f, font);
    auto btnBankNew = std::make_shared<SdrButton>(128, 25, "New bank", font);
    auto btnBankDel = std::make_shared<SdrButton>(128, 25, "Delete bank", font);
    auto rowBank = std::make_shared<RowContainer>();
    rowBank->add(btnBankNew); rowBank->add(btnBankDel);
    auto btnBankExport = std::make_shared<SdrButton>(128, 25, "Export bank...", font);
    auto btnBankImport = std::make_shared<SdrButton>(128, 25, "Import...", font);
    auto rowBankIo = std::make_shared<RowContainer>();
    rowBankIo->add(btnBankExport); rowBankIo->add(btnBankImport);
    auto txtBookName = std::make_shared<TextInput>(SIDEBAR_W - 40, 25.0f, font);
    auto btnBookAdd = std::make_shared<SdrButton>(128, 25, "Add current", font);
    auto btnBookSave = std::make_shared<SdrButton>(128, 25, "Update selected", font);
    auto rowBook = std::make_shared<RowContainer>();
    rowBook->add(btnBookAdd); rowBook->add(btnBookSave);
    auto bookList = std::make_shared<BookmarkList>(SIDEBAR_W - 44, font);
    bookList->store = &bookmarks;
    auto btnScan = std::make_shared<SdrButton>(128, 25, "Scan", font);
    auto btnScanSkip = std::make_shared<SdrButton>(128, 25, "Skip >", font);
    auto rowScan = std::make_shared<RowContainer>();
    rowScan->add(btnScan); rowScan->add(btnScanSkip);
    auto slScanThr = std::make_shared<Slider>(SIDEBAR_W - 40, 3.0f, 40.0f, 12.0f, "Stop at SNR >= 12 dB", font);
    auto slScanHang = std::make_shared<Slider>(SIDEBAR_W - 40, 0.0f, 15.0f, 3.0f, "Resume 3 s after the signal", font);
    auto lblScan = std::make_shared<Label>("Scanner: off", font, 11, sf::Color(150, 150, 160));
    auto chkBookMarks = std::make_shared<Checkbox>("Show the bank's bookmarks on the spectrum", font,
                                                   settingsMgr.getBool("bookmark_markers", true));
    modBook->addWidget(std::make_shared<Label>("Bank (memory group):", font));
    modBook->addWidget(ddBank);
    modBook->addWidget(chkBookMarks);
    modBook->addWidget(std::make_shared<Label>("Name (bookmark or new bank):", font));
    modBook->addWidget(txtBookName);
    modBook->addWidget(rowBook);
    modBook->addWidget(rowBank);
    modBook->addWidget(rowBankIo);
    modBook->addWidget(bookList);
    modBook->addWidget(rowScan);
    modBook->addWidget(slScanThr);
    modBook->addWidget(slScanHang);
    modBook->addWidget(lblScan);
    modBook->addWidget(std::make_shared<Label>("Ticked bookmarks of the bank are scanned.", font, 11, sf::Color(130, 130, 140)));
    modBook->addWidget(std::make_shared<Label>("Tuning by hand stops the scanner.", font, 11, sf::Color(130, 130, 140)));
    // bank list: "All banks" + the banks; the list and the scanner show the chosen one
    auto refreshBanks = [&](const std::string& select) {
        std::vector<std::string> opts = {"All banks"};
        for (const auto& b : bookmarks.banks) opts.push_back(b);
        ddBank->setOptions(opts);
        int sel = 0;
        for (size_t i = 1; i < opts.size(); i++) if (opts[i] == select) sel = (int)i;
        ddBank->setSelection(sel);
        bookList->bankFilter = sel == 0 ? std::string() : opts[sel];
        btnBankDel->setEnabled(sel > 0 && opts[sel] != defaultBank());
        btnBankExport->setText(sel == 0 ? "Export all..." : "Export bank...");
        sidebar.recalculateLayout();
    };
    auto currentBank = [&]() { return bookList->bankFilter.empty() ? defaultBank() : bookList->bankFilter; };
    refreshBanks(settingsMgr.getString("bookmark_bank", ""));
    ddBank->onChange = [&](int idx) {
        refreshBanks(idx > 0 && idx < (int)ddBank->options.size() ? ddBank->options[idx] : std::string());
    };
    btnBankNew->onClick = [&]() {
        std::string n = txtBookName->getText();
        while (!n.empty() && n.back() == ' ') n.pop_back();
        if (n.empty()) { showPopup("Bookmarks", "Type the name of the new bank first."); return; }
        bookmarks.ensureBank(n);
        bookmarks.save();
        txtBookName->setText("");
        refreshBanks(n);
    };
    btnBankDel->onClick = [&]() {
        const std::string b = bookList->bankFilter;
        if (b.empty() || b == defaultBank()) return;
        bookmarks.removeBank(b);   // its bookmarks move to "General"
        refreshBanks(defaultBank());
    };
    scanner.eligible = [&](int i) { return bookList->shows(i); };
    // The digital voice cards show the name of the bookmark saved on their frequency
    // (the nearest one within 2.5 kHz; the chosen bank wins a tie)
    DigitalVoiceDecoder::channelLabel = [&](long long hz) -> std::string {
        if (hz <= 0) return std::string();
        int best = -1;
        long long bestD = 2500;
        for (int i = 0; i < (int)bookmarks.items.size(); i++) {
            const Bookmark& b = bookmarks.items[i];
            if (b.name.empty()) continue;
            const long long d = std::llabs(b.freqHz - hz);
            if (d < bestD || (d == bestD && best >= 0 && bookList->shows(i) && !bookList->shows(best))) { bestD = d; best = i; }
        }
        return best >= 0 ? bookmarks.items[best].name : std::string();
    };
    auto setScanUi = [&]() {
        btnScan->setText(scanner.running() ? "Stop scan" : "Scan");
        btnScan->setActive(scanner.running());
        btnScanSkip->setEnabled(scanner.running());
    };
    setScanUi();
    // Export: the chosen bank (or all of them) in the bookmarks.json format; import: adds its
    // banks and bookmarks (the same frequency in the same bank is replaced)
    btnBankExport->onClick = [&]() {
        const std::string bank = bookList->bankFilter;
        std::string fname = bank.empty() ? "bookmarks_all" : "bookmarks_" + bank;
        for (auto& c : fname) if (std::strchr("\\/:*?\"<>| ", c)) c = '_';
        const std::string path = saveJsonFileDialog(getDefaultExportPath(), fname + ".json");
        if (path.empty()) return;
        int n = 0;
        for (const auto& b : bookmarks.items) if (bank.empty() || b.bank == bank) n++;
        if (bookmarks.writeFile(path, bank))
            showPopup("Bookmarks", std::to_string(n) + " bookmarks exported to\n" + path);
        else
            showPopup("Bookmarks", "Could not write\n" + path);
    };
    btnBankImport->onClick = [&]() {
        const std::string path = openJsonFileDialog();
        if (path.empty()) return;
        if (scanner.running()) { scanner.stop(); setScanUi(); }
        const int n = bookmarks.importFile(path);
        if (n < 0) { showPopup("Bookmarks", "Not a bookmarks file:\n" + path); return; }
        bookList->selected = -1;
        bookList->active = -1;
        refreshBanks(bookList->bankFilter);
        showPopup("Bookmarks", std::to_string(n) + " bookmarks imported from\n" + path);
    };
    slScanThr->onChange = [&](float v) {
        scanner.thresholdDb = v;
        char b[48]; std::snprintf(b, sizeof(b), "Stop at SNR >= %.0f dB", v); slScanThr->setText(b);
    };
    slScanHang->onChange = [&](float v) {
        scanner.hangSec = v;
        char b[48]; std::snprintf(b, sizeof(b), "Resume %.0f s after the signal", v); slScanHang->setText(b);
    };
    bookList->onTune = [&](int i) {
        if (scanner.running()) { scanner.stop(); setScanUi(); }
        pendingBookmarkTune = i;
        if (i >= 0 && i < (int)bookmarks.items.size()) txtBookName->setText(bookmarks.items[i].name);
    };
    bookList->onChanged = [&]() { sidebar.recalculateLayout(); };
    btnBookAdd->onClick = [&]() {
        Bookmark b;
        b.freqHz = freqVFO.getFrequency();
        { std::lock_guard<std::mutex> l(cur->sd.mtx); b.mode = cur->sd.mode; }
        b.bandwidthHz = slBW->currentVal;
        b.name = txtBookName->getText();
        b.scan = true;
        b.bank = currentBank();
        if (b.freqHz <= 0) return;
        bookList->selected = bookmarks.add(b);
        bookList->ensureVisible(bookList->selected);
        txtBookName->setText("");
        sidebar.recalculateLayout();
    };
    btnBookSave->onClick = [&]() {
        const int i = bookList->selected;
        if (i < 0 || i >= (int)bookmarks.items.size()) { showPopup("Bookmarks", "Click a bookmark in the list first."); return; }
        Bookmark& b = bookmarks.items[i];
        b.name = txtBookName->getText();
        b.freqHz = freqVFO.getFrequency();
        { std::lock_guard<std::mutex> l(cur->sd.mtx); b.mode = cur->sd.mode; }
        b.bandwidthHz = slBW->currentVal;
        if (!bookList->bankFilter.empty()) b.bank = bookList->bankFilter;   // moves it to the chosen bank
        const long long f = b.freqHz;
        bookmarks.sort();
        bookmarks.save();
        const std::string bk = b.bank;
        for (size_t k = 0; k < bookmarks.items.size(); k++)
            if (bookmarks.items[k].freqHz == f && bookmarks.items[k].bank == bk) bookList->selected = (int)k;
    };
    scanner.tune = [&](int i) { pendingBookmarkTune = i; };
    btnScan->onClick = [&]() {
        if (scanner.running()) scanner.stop();
        else scanner.start(bookmarks.items, bookList->selected, scanClock.getElapsedTime().asSeconds());
        if (!scanner.running() && !bookmarks.items.empty()) {
            bool any = false;
            for (const auto& b : bookmarks.items) any = any || b.scan;
            if (!any) showPopup("Scanner", "Tick at least one bookmark for scanning.");
        }
        setScanUi();
    };
    btnScanSkip->onClick = [&]() { scanner.skip(bookmarks.items, scanClock.getElapsedTime().asSeconds()); };

    // --- Input signal switch (I/Q / Audio) ---
    applyInputType = [&](bool audioIn) {
        btnInIQ->setActive(!audioIn);
        btnInAudio->setActive(audioIn);
        if (audioIn == cur->inputIsAudio) return;
        Mode curMode;
        { std::lock_guard<std::mutex> l(cur->sd.mtx); curMode = cur->sd.mode; }
        if (audioIn) cur->modeBeforeAudio = curMode;
        cur->inputIsAudio = audioIn;
        { std::lock_guard<std::mutex> l(cur->sd.mtx); cur->sd.audioInput = audioIn; }
        if (audioIn) restoreMode(isDigitalMode(curMode) ? curMode : Mode::RAW);   // digital voice also decodes audio
        else restoreMode(cur->modeBeforeAudio);
    };

    // --- Decoders ---
    // Every decoder works the same way:
    //  - with I/Q input it listens to its own channel(s): the yellow bands with handles on the
    //    tuning bar (see modules/Core/DecoderChannels.h). It never changes the demodulator, so
    //    the tuner and the mode stay free for listening;
    //  - with audio input (Input signal switch) it decodes the input audio;
    //  - several decoders can run at once; each has a tab above the decoder panel.
    auto chkSSTV  = std::make_shared<Checkbox>("SSTV (slow-scan TV pictures)", font);
    auto chkWEFAX = std::make_shared<Checkbox>("WEFAX (HF weather fax)", font);
    auto chkISM   = std::make_shared<Checkbox>(ISMDecoder::available() ? "ISM (433/868 MHz sensors)"
                                                                       : "ISM (not in this build)", font);
    auto chkACARS = std::make_shared<Checkbox>("ACARS (aircraft, 131.x MHz)", font);
    auto chkADSB  = std::make_shared<Checkbox>("ADS-B (aircraft, 1090 MHz)", font);
    auto chkVDL2  = std::make_shared<Checkbox>("VDL2 (aircraft datalink, 136.975)", font);
    auto chkCW    = std::make_shared<Checkbox>("CW (Morse code)", font);
    auto chkNAVTEX = std::make_shared<Checkbox>("NAVTEX (518 / 490 kHz)", font);
    auto chkDSC   = std::make_shared<Checkbox>("DSC (VHF ch70 156.525, MF/HF)", font);
    auto chkAnalyzer = std::make_shared<Checkbox>("Signal analyzer (what is this signal?)", font);
    auto chkDV    = std::make_shared<Checkbox>(DigitalVoiceDecoder::available() ? "Digital voice (DMR, dPMR, D-STAR, YSF...)"
                                                                               : "Digital voice (not in this build)", font);
    auto chkWSJT  = std::make_shared<Checkbox>(WSJTDecoder::ft8Available() ? "FT8 / FT4 / WSPR (HF digital)"
                                                                           : "WSPR (FT8 / FT4 not in this build)", font);

    // The checkboxes; the modules are the shown receiver's (cur->decMods[i], nullptr = APRS)
    struct DecoderEntry { std::shared_ptr<Checkbox> chk; };
    std::vector<DecoderEntry> decoders = {
        {chkAprs}, {chkDV}, {chkAIS}, {chkSSTV}, {chkWEFAX}, {chkISM}, {chkACARS}, {chkVDL2}, {chkADSB}, {chkWSJT}, {chkCW}, {chkNAVTEX}, {chkDSC},
        {chkAnalyzer} };

    auto activateDecoderOn = [&](Receiver* r, size_t idx, bool on) {
        SDRModule* const mod = r->decMods[idx];
        std::lock_guard<std::mutex> l(r->sd.mtx);
        if (mod) {
            auto& act = r->sd.activeDecoders;
            act.erase(std::remove(act.begin(), act.end(), mod), act.end());
            if (on) {
                if (mod->channelSet()) mod->channelSet()->clear();   // placed again for the current band
                mod->enabled = true;
                act.push_back(mod);
            } else {
                mod->enabled = false;
            }
        } else {
            if (on) r->sd.aprsCh.clear();
            r->sd.aprsEnabled = on;
        }
        if (on) r->selectedTab = mod ? mod->name : std::string("APRS");   // show the one just switched on
    };
    auto activateDecoder = [&](size_t idx, bool on) { activateDecoderOn(cur, idx, on); };
    for (size_t i = 0; i < decoders.size(); i++) {
        decoders[i].chk->onToggle = [&, i](bool b) { activateDecoder(i, b); };
        modDecoders->addWidget(decoders[i].chk);
    }
    modDecoders->addWidget(std::make_shared<Label>("Several can run at once, each on its own channels", font, 11, sf::Color(150, 150, 160)));

    // The map window offers to switch on the decoder a satellite transmitter needs (SSTV, APRS)
    // on the receiver chosen for tracking in the map (its tab may be in the background)
    mapWindow.onEnableDecoder = [&](const std::string& name) {
        Receiver* r = cur;
        for (auto& rxp : receivers) if (rxp->id == satTracker.rxId) r = rxp.get();
        for (size_t i = 0; i < decoders.size(); i++) {
            DecoderEntry& d = decoders[i];
            const std::string n = r->decMods[i] ? r->decMods[i]->name : std::string("APRS");
            if (n != name) continue;
            if (r == cur) {
                if (!d.chk->checked) { d.chk->checked = true; d.chk->onToggle(true); }
            } else {
                bool on;
                { std::lock_guard<std::mutex> l(r->sd.mtx); on = r->decMods[i] ? r->decMods[i]->enabled : r->sd.aprsEnabled; }
                if (!on) activateDecoderOn(r, i, true);
            }
            r->selectedTab = n;
        }
    };

    auto modDisp = sidebar.addModule("Display");
    auto slMinDb = std::make_shared<Slider>(SIDEBAR_W - 40, -140.0f, -20.0f, -90.0f, "Min dB", font);
    slMinDb->onChange = [&](float v) { std::lock_guard<std::mutex> l(cur->sd.mtx); cur->sd.minDb = v; };
    modDisp->addWidget(slMinDb);

    auto slMaxDb = std::make_shared<Slider>(SIDEBAR_W - 40, -60.0f, 40.0f, 0.0f, "Max dB", font);
    slMaxDb->onChange = [&](float v) { std::lock_guard<std::mutex> l(cur->sd.mtx); cur->sd.maxDb = v; };
    modDisp->addWidget(slMaxDb);

    auto slZoom = std::make_shared<Slider>(SIDEBAR_W - 40, 1.0f, 16.0f, 1.0f, "Zoom", font);
    slZoom->onChange = [&](float v) {
        std::lock_guard<std::mutex> l(cur->sd.mtx);
        double oldZoom = cur->sd.zoomLevel;
        double oldVisibleFraction = 1.0 / oldZoom;
        double oldViewStart = cur->sd.viewCenterPct - oldVisibleFraction / 2.0;
        double tunerOffsetFromStart = cur->sd.tunedFreqPercent - oldViewStart;
        double tunerPosRatio = tunerOffsetFromStart / oldVisibleFraction;

        cur->sd.zoomLevel = v;

        double newVisibleFraction = 1.0 / v;
        double newViewStart = cur->sd.tunedFreqPercent - (tunerPosRatio * newVisibleFraction);
        cur->sd.viewCenterPct = newViewStart + newVisibleFraction / 2.0;

        if (cur->sd.viewCenterPct - newVisibleFraction/2.0 < 0.0) cur->sd.viewCenterPct = newVisibleFraction/2.0;
        if (cur->sd.viewCenterPct + newVisibleFraction/2.0 > 1.0) cur->sd.viewCenterPct = 1.0 - newVisibleFraction/2.0;
    };
    modDisp->addWidget(slZoom);

    // A copy of the zoom slider in the bottom right corner of the waterfall (on the status
    // bar, or on the decoder panel when one is open). Drives the sidebar slider.
    Slider slZoomBar(180, 1.0f, 16.0f, 1.0f, "Zoom", font);
    auto zoomBarText = [](float v) { char b[32]; std::snprintf(b, sizeof(b), "Zoom: %.1fx", v); return std::string(b); };
    slZoomBar.onChange = [&](float v) {
        slZoom->setValueSilent(v);
        if (slZoom->onChange) slZoom->onChange(slZoom->currentVal);
    };

    auto ddTheme = std::make_shared<Dropdown>(SIDEBAR_W - 40, 25.0f, font);
    ddTheme->setOptions(THEME_NAMES);
    ddTheme->setSelection(0);
    ddTheme->onChange = [&](int idx) {
        std::lock_guard<std::mutex> l(cur->sd.mtx);
        cur->sd.waterfallTheme = idx;
        Theme::setTheme(idx);
        freqVFO.updateStyle();
        btnTuningMode.updateStyle();
        btnPlay.updateStyle();
        btnMute.updateStyle();
        btnMap.updateStyle();
        volSlider.updateStyle();
        slZoomBar.updateStyle();
        timeSlider.updateStyle();
        sidebar.updateStyle();
    };
    modDisp->addWidget(std::make_shared<Label>("Waterfall Theme:", font));
    modDisp->addWidget(ddTheme);

    // UI scale selector (for HiDPI / tiny UI on Windows laptops)
    auto ddScale = std::make_shared<Dropdown>(SIDEBAR_W - 40, 25.0f, font);
    ddScale->setOptions(UI_SCALE_NAMES);
    ddScale->setSelection(0);
    ddScale->onChange = [&](int idx) {
        uiScaleIdx = idx;
        if (applyUiScale) applyUiScale();
    };
#ifdef __APPLE__
    modDisp->addWidget(std::make_shared<Label>("UI Scale (Cmd +/-/0):", font));
#else
    modDisp->addWidget(std::make_shared<Label>("UI Scale (Ctrl +/-/0):", font));
#endif
    modDisp->addWidget(ddScale);

    auto ddSnap = std::make_shared<Dropdown>(150.0f, 25.0f, font);
    ddSnap->setOptions(STEP_NAMES);
    ddSnap->setSelection(5);
    modDisp->addWidget(std::make_shared<Label>("Tuning Step:", font));
    modDisp->addWidget(ddSnap);

    auto modRec = sidebar.addModule("Recording");
    auto rowRecMode = std::make_shared<RowContainer>();
    auto btnRecAudio = std::make_shared<SdrButton>(80, 25, "Audio", font);
    btnRecAudio->setActive(true);
    auto btnRecIQ = std::make_shared<SdrButton>(80, 25, "Baseband", font);

    btnRecAudio->onClick = [&](){ btnRecAudio->setActive(true); btnRecIQ->setActive(false); std::lock_guard<std::mutex> l(cur->sd.mtx); cur->sd.recMode = RecMode::AUDIO; };
    btnRecIQ->onClick = [&](){ btnRecAudio->setActive(false); btnRecIQ->setActive(true); std::lock_guard<std::mutex> l(cur->sd.mtx); cur->sd.recMode = RecMode::BASEBAND; };
    rowRecMode->add(btnRecAudio);
    rowRecMode->add(btnRecIQ);
    modRec->addWidget(rowRecMode);

    auto lblRecPath = std::make_shared<Label>("Path: ...", font, 11, sf::Color(150,150,150));
    modRec->addWidget(lblRecPath);

    auto btnSetFolder = std::make_shared<SdrButton>(SIDEBAR_W - 40, 25, "Change Folder...", font);
    btnSetFolder->onClick = [&](){
        std::string p = selectFolderDialog();
        if(!p.empty()) {
            { std::lock_guard<std::mutex> l(cur->sd.mtx); cur->sd.recPath = p; }
            lblRecPath->setText("Path: " + truncatePath(p, 25));
        }
    };
    modRec->addWidget(btnSetFolder);

    auto btnRecToggle = std::make_shared<SdrButton>(SIDEBAR_W-40, 35, "START RECORDING", font);
    btnRecToggle->setColor(sf::Color(40, 40, 45));
    btnRecToggle->onClick = [&](){
        std::lock_guard<std::mutex> l(cur->sd.mtx);
        cur->sd.isRecording = !cur->sd.isRecording;
        if(cur->sd.isRecording) {
            btnRecToggle->setText("STOP RECORDING"); btnRecToggle->setColor(sf::Color(180, 50, 50));
        } else {
            btnRecToggle->setText("START RECORDING"); btnRecToggle->setColor(sf::Color(40, 40, 45));
        }
    };
    modRec->addWidget(btnRecToggle);

    const int MINI_W = 280;
    const int MINI_H = 120;
    std::vector<uint8_t> miniWaterfall(MINI_W * MINI_H * 4, 0);
    sf::Texture miniTex;
    (void)miniTex.resize({(unsigned int)MINI_W, (unsigned int)MINI_H});
    sf::Sprite miniSpr(miniTex);

    auto modInspector = sidebar.addModule("Signal Inspector");
    auto spacer = std::make_shared<Spacer>(130.0f);
    modInspector->addWidget(spacer);

    sf::Texture wTex;
    if(!wTex.resize({INTERNAL_WATERFALL_WIDTH, WATERFALL_HEIGHT_PX})) return 1;
    wTex.setSmooth(true);
    wTex.setRepeated(true);
    sf::Sprite wSpr(wTex);
    int currentWaterfallY = 0;

    LayoutState layout;
    sf::Vector2u lastSize = window.getSize();
    sf::Clock scaleRecheck;          // Auto UI scale: when it was last looked up
    bool scaleRecheckFocus = false;  // the window got the focus: look again (Linux)

    // w / h are LOGICAL sizes (window pixels divided by the UI scale)
    auto updateLayout = [&](int w, int h) {
        layout.winW = (float)w;
        layout.winH = (float)h;
        layout.sidebarX = layout.winW - SIDEBAR_W;
        layout.specW = layout.sidebarX;
        layout.specH = 250.0f;
        layout.waterfallH = layout.winH - TOP_BAR_H - layout.specH - layout.tuneBarH;
        if (layout.waterfallH < 100) layout.waterfallH = 100;

        topBar.setSize({layout.winW, (float)TOPBAR_DRAW_H});
        freqVFO.setPosition(20, 4);
        btnTuningMode.setPosition(370, 10);
        btnPlay.setPosition(320, 10);
        btnMap.setPosition(420, 10);
        volSlider.setPosition(layout.winW - 170, 10);
        btnMute.setPosition(layout.winW - 220, 10);
        sidebar.setGeometry(layout.sidebarX, TOP_BAR_H, layout.winH - TOP_BAR_H);
        timeSlider.setPosition(20, layout.winH - TIMELINE_H + 6.0f);
        timeSlider.setWidth(layout.specW - 40);

        wSpr.setPosition({0, TOP_BAR_H + layout.specH + layout.tuneBarH});
        float scaleX = layout.specW / (float)INTERNAL_WATERFALL_WIDTH;
        wSpr.setScale({scaleX, 1.0f});
        wSpr.setTextureRect(sf::IntRect({0, 0}, {INTERNAL_WATERFALL_WIDTH, (int)layout.waterfallH}));
    };

    // Applies the current UI scale: scaled view + layout computed in logical units
    applyUiScale = [&]() {
        sf::Vector2u sz = window.getSize();
        if (sz.x == 0 || sz.y == 0) return;
        float s = getUiScale();
        if (s < 0.5f) s = 0.5f;
        float lw = (float)sz.x / s;
        float lh = (float)sz.y / s;
        window.setView(sf::View(sf::FloatRect({0.f, 0.f}, {lw, lh})));
        updateLayout((int)lw, (int)lh);
    };

    // Restore saved scale before the first layout pass. Saved as a percentage (0 = Auto);
    // older versions saved the index into {Auto, 100, 125, 150, 175, 200, 250}.
    {
        int pct = settingsMgr.getInt("ui_scale_pct", -1);
        if (pct < 0) {
            static const int OLD[] = {0, 100, 125, 150, 175, 200, 250};
            pct = OLD[std::clamp(settingsMgr.getInt("ui_scale", 0), 0, 6)];
        }
        uiScaleIdx = 0;
        for (int i = 1; i < (int)UI_SCALE_VALUES.size(); i++)
            if (std::lround(UI_SCALE_VALUES[i] * 100.0f) == pct) uiScaleIdx = i;
    }
    ddScale->setSelection(uiScaleIdx);
    applyUiScale();

    for (auto& mod : sidebar.modules) {
        if (mod->title == "Source / Input") mod->isOpen = true;
        else mod->isOpen = settingsMgr.getBool("sidebar_" + mod->title, mod->isOpen);
    }

    int savedTheme = settingsMgr.getInt("theme", 0);
    ddTheme->setSelection(savedTheme);
    ddTheme->onChange(savedTheme);

    slMinDb->setValueSilent(settingsMgr.getFloat("min_db", -90.0f)); slMinDb->onChange(slMinDb->currentVal);
    slMaxDb->setValueSilent(settingsMgr.getFloat("max_db", 0.0f));   slMaxDb->onChange(slMaxDb->currentVal);
    slZoom->setValueSilent(settingsMgr.getFloat("zoom", 1.0f));      slZoom->onChange(slZoom->currentVal);

    ddSnap->setSelection(settingsMgr.getInt("step_index", 5));

    int savedMode = settingsMgr.getInt("mode_index", 1);
    Mode mEnum = (Mode)std::clamp(savedMode, 0, MODE_LAST);
    setMode(mEnum, buttonFor(mEnum));

    float savedBw = settingsMgr.getFloat("bandwidth", 12500.0f);
    slBW->setValueSilent(savedBw); slBW->onChange(savedBw);

    slScanThr->setValueSilent(settingsMgr.getFloat("scan_threshold", 12.0f)); slScanThr->onChange(slScanThr->currentVal);
    slScanHang->setValueSilent(settingsMgr.getFloat("scan_hang", 3.0f));     slScanHang->onChange(slScanHang->currentVal);

    float savedSq = settingsMgr.getFloat("squelch", -100.0f);
    slSq->setValueSilent(savedSq); slSq->onChange(savedSq);

    bool savedStereo = settingsMgr.getBool("stereo", false);
    chkStereo->checked = savedStereo;
    chkStereo->onToggle(savedStereo);

    cur->sstv->applySettings(settingsMgr.getBool("sstv_auto_slant", true), settingsMgr.getFloat("sstv_slant", 0.0f),
                           settingsMgr.getBool("sstv_auto_save", false));
    cur->wefax->applySettings(settingsMgr.getInt("wefax_lpm", 120), settingsMgr.getInt("wefax_ioc", 576),
                            settingsMgr.getBool("wefax_apt", true), settingsMgr.getFloat("wefax_slant", 0.0f),
                            settingsMgr.getBool("wefax_auto_save", false));

    bool savedRds = settingsMgr.getBool("rds", true);
    chkRds->checked = savedRds;
    chkRds->onToggle(savedRds);

    float savedGain = settingsMgr.getFloat("rf_gain", -1.0f);
    if (savedGain < 0) {
        chkAgc->checked = true;
        chkAgc->onToggle(true);
    } else {
        chkAgc->checked = false;
        chkAgc->onToggle(false);
        slGain->setValueSilent(savedGain);
        slGain->onChange(savedGain);
    }

    cur->sd.hw.fmNotch = settingsMgr.getBool("hw_fm_notch", false);
    cur->sd.hw.mwNotch = settingsMgr.getBool("hw_mw_notch", false);
    cur->sd.hw.biasT = settingsMgr.getBool("hw_bias_t", false);
    cur->sd.hw.directSampling = settingsMgr.getInt("hw_direct_samp", 0);
    cur->sd.hw.antennaIndex = settingsMgr.getInt("hw_antenna", 0);
    cur->sd.hw.ppm = std::clamp(settingsMgr.getInt("hw_ppm", 0), -200, 200);

    int savedSourceIdx = settingsMgr.getInt("source_type", 0);
    if (savedSourceIdx != 0) {
        ddSourceType->setSelection(savedSourceIdx);
        if (savedSourceIdx == 4) {
            cur->sourceType = 4;
            updateRadioControls(4);
            ddRate->setOptions(RTL_SAMPLE_RATES_TXT);
            ddRate->setSelection(2);
            refreshSourceUI();
        } else {
            quietSourceErrors = true;   // no "device not found" pop-ups right at startup
            ddSourceType->onChange(savedSourceIdx);
            quietSourceErrors = false;
        }
    }

    std::string savedRecPath = settingsMgr.getString("rec_path", "");
    if (savedRecPath.empty()) {
        savedRecPath = getDefaultRecordingPath();
    }
    { std::lock_guard<std::mutex> l(cur->sd.mtx); cur->sd.recPath = savedRecPath; }
    lblRecPath->setText("Path: " + truncatePath(savedRecPath, 25));

    sf::Clock interactionCooldown;
    bool isDraggingScale = false, isSpectrumDragging = false;
    bool isDraggingBW = false;
    float dragStartBW = 0.0f;
    float dragStartMouseX = 0.0f;
    float lastDragX = 0.0f;
    float aprsLogScrollOffset = 0.0f;
    bool isPanningView = false;

    // Dragging a decoder channel handle on the tuning bar (see DecoderChannels.h)
    struct ChannelDrag {
        ChannelSet* set = nullptr;
        int id = -1;
        float startX = 0.0f, x = 0.0f;
        bool moved = false;
        int snap = 0;   // 0: free, 1: known frequency, 2: the tuner
        long long freqHz = 0;
    } chDrag;

    // Dragging an edge of a decoder channel's yellow band (changes its width)
    struct ChannelEdgeDrag {
        ChannelSet* set = nullptr;
        int id = -1;
        float x = 0.0f;
        double widthHz = 0.0;
        bool snapTuner = false;   // on the tuner's band edge: release = take the tuner's width again
    } chEdge;

    bool showAprsModal = false;
    AprsLastPacket selectedAprsPacket;
    SdrButton btnModalClose(80, 30, "Close", font);
    SdrButton btnModalCopy(80, 30, "Copy", font);
    btnModalClose.onClick = [&]() { showAprsModal = false; };
    btnModalCopy.onClick = [&]() { sf::Clipboard::setString(selectedAprsPacket.raw); };

    // Band plan: resolved relative to the EXECUTABLE (works from Finder / .app bundle / any cwd)
    {
        std::string bpPath = findResourceFile("bandplan.json");
        if (!bpPath.empty()) {
            std::cout << "[INFO] Loading band plan from: " << bpPath << std::endl;
            globalBandPlan = loadBandPlan(bpPath);
        } else {
            std::cerr << "[Warning] bandplan.json not found." << std::endl;
        }
    }

    // --- Decoder tabs above the decoder panel (one per running decoder) ---
    // Every decoder keeps its own colour: the tab's mark and its channels on the spectrum.
    struct DecTab { std::string name; SDRModule* mod; ChannelSet* set; sf::Color color; size_t entry; };
    auto decoderColor = [](size_t entry) {
        static const sf::Color pal[] = { sf::Color(255, 210, 0), sf::Color(0, 210, 255), sf::Color(255, 110, 200),
                                         sf::Color(120, 230, 120), sf::Color(255, 150, 60), sf::Color(170, 150, 255),
                                         sf::Color(255, 255, 140), sf::Color(90, 240, 210), sf::Color(240, 120, 120),
                                         sf::Color(200, 200, 210), sf::Color(160, 220, 60) };
        return pal[entry % (sizeof(pal) / sizeof(pal[0]))];
    };
    auto collectTabs = [&](Receiver* r) {
        std::vector<DecTab> t;
        std::vector<SDRModule*> act;
        bool aprs;
        { std::lock_guard<std::mutex> l(r->sd.mtx); act = r->sd.activeDecoders; aprs = r->sd.aprsEnabled; }
        if (aprs) t.push_back({"APRS", nullptr, &r->sd.aprsCh, decoderColor(0), 0});
        for (SDRModule* m : act) {
            if (!m || !m->enabled) continue;
            size_t entry = 0;
            for (size_t i = 0; i < r->decMods.size(); i++) if (r->decMods[i] == m) entry = i;
            // the analyzer's channel is always orange (it is not a decoder of one signal)
            const sf::Color c = m == r->analyzer ? SignalAnalyzer::color() : decoderColor(entry);
            t.push_back({m->name, m, m->channelSet(), c, entry});
        }
        return t;
    };
    const float TAB_H = 22.0f;
    struct TabHit { sf::FloatRect tab, close; size_t entry; std::string name; };
    std::vector<TabHit> tabHits;              // where the tabs were drawn (for the clicks)
    sf::Clock decoderDropClock;               // time since the decoders last skipped a block
    bool decoderDropSeen = false;

    // =====================================================================
    //  Receiver tabs (below the top bar). The sidebar and the top bar show the
    //  receiver in `cur`; switching saves what only the widgets know (device
    //  list, sample rate list, gain ...) and loads the other receiver's state.
    //  The receivers not shown keep running: no waterfall, no speaker.
    // =====================================================================
    bool appMuted = false;
    struct RxAction { int kind = 0; Receiver* rx = nullptr; };   // 1 add, 2 close, 3 switch
    RxAction rxAction;
    struct RxTabHit { sf::FloatRect tab, close, speaker; Receiver* rx; };
    std::vector<RxTabHit> rxTabHits;
    sf::FloatRect rxAddHit;
    struct PipeTabHit { sf::FloatRect tab, close; int uid; };
    std::vector<PipeTabHit> pipeTabHits;
    sf::FloatRect pipeAddHit;
    int pipeDragUid = 0;             // a pipeline tab being dragged (to another place)
    float pipeDragX = 0.0f;
    bool pipeDragMoved = false;

    auto saveUi = [&](Receiver* r) {
        r->deviceOptions = ddDevice->options;
        r->deviceSel = ddDevice->selectedIndex;
        r->rateOptions = ddRate->options;
        r->rateSel = ddRate->selectedIndex;
        r->agc = chkAgc->checked;
        r->gain = slGain->currentVal;
        r->gainMin = slGain->minVal;
        r->gainMax = slGain->maxVal;
        r->hostText = txtHost->getText();
        r->uiSaved = true;
    };

    auto applyUi = [&](Receiver* r) {
        ddAudio->setSelection(audio.deviceOf(r->sd.audioStream));
        // source
        ddSourceType->setSelection(r->sourceType);
        updateRadioControls(r->sourceType);
        if (r->uiSaved) {
            ddDevice->setOptions(r->deviceOptions);
            ddDevice->setSelection(r->deviceSel);
            ddRate->setOptions(r->rateOptions);
            ddRate->setSelection(r->rateSel);
        } else {
            ddDevice->setOptions({"No Devices Found"});
            ddRate->setOptions({"None"});
        }
        txtHost->setText(r->hostText);
        btnConnect->setText(r->tcpConnected ? "Disconnect" : "Connect");
        btnConnect->setActive(r->tcpConnected);
        refreshSourceUI();
        btnInIQ->setActive(!r->inputIsAudio);
        btnInAudio->setActive(r->inputIsAudio);
        // gain (without sending it to the radio again)
        slGain->setLimits(r->gainMin, r->gainMax);
        slGain->setValueSilent(r->gain);
        chkAgc->checked = r->agc;
        slGain->setEnabled(!r->agc);
        if (!r->agc) {
            char b[48];
            if (r->sourceType == 1 || r->sourceType == 4) std::snprintf(b, sizeof(b), "RF Gain: %.1f dB", r->gain);
            else std::snprintf(b, sizeof(b), "RF Gain (dB): %d", (int)r->gain);
            slGain->setText(b);
        }
        // demodulator and display (kept in the receiver's SharedData)
        Mode m; float bw, sq, minDb, maxDb, zoom; bool st, rds, rec, play; RecMode rm; std::string recPath;
        double pct;
        int tsMask; bool muteEnc;
        {
            std::lock_guard<std::mutex> l(r->sd.mtx);
            tsMask = r->sd.digSlotMask; muteEnc = r->sd.digMuteEncrypted;
            m = r->sd.mode; bw = (float)r->sd.bandwidth; sq = r->sd.squelchThreshold;
            st = r->sd.stereoEnabled; rds = r->sd.rdsEnabled; minDb = r->sd.minDb; maxDb = r->sd.maxDb; zoom = r->sd.zoomLevel;
            rec = r->sd.isRecording; rm = r->sd.recMode; recPath = r->sd.recPath; play = r->sd.isPlaying;
            pct = r->sd.tunedFreqPercent;
        }
        restoreMode(m);
        if (m != Mode::OFF) { slBW->setValueSilent(bw); slBW->onChange(slBW->currentVal); }
        setTsUi(tsMask);
        chkMuteEnc->checked = muteEnc;
        slSq->setValueSilent(sq);
        chkStereo->checked = st;
        chkRds->checked = rds;
        slMinDb->setValueSilent(minDb);
        slMaxDb->setValueSilent(maxDb);
        slZoom->setValueSilent(zoom);
        // decoders
        for (size_t i = 0; i < decoders.size(); i++) {
            SDRModule* mod = r->decMods[i];
            std::lock_guard<std::mutex> l(r->sd.mtx);
            decoders[i].chk->checked = mod ? mod->enabled : r->sd.aprsEnabled;
        }
        // recording
        btnRecAudio->setActive(rm == RecMode::AUDIO);
        btnRecIQ->setActive(rm == RecMode::BASEBAND);
        lblRecPath->setText("Path: " + truncatePath(recPath, 25));
        btnRecToggle->setText(rec ? "STOP RECORDING" : "START RECORDING");
        btnRecToggle->setColor(rec ? sf::Color(180, 50, 50) : sf::Color(40, 40, 45));
        // top bar
        btnPlay.setText(play ? "||" : ">");
        btnPlay.setActive(play);
        btnTuningMode.setText(r->stickyCenter ? "CTR" : "FIX");
        btnTuningMode.setActive(r->stickyCenter);
        double rate = 2e6;
        {
            std::lock_guard<std::mutex> l(r->sd.sourceMtx);
            if (r->sd.source) rate = r->sd.source->getSampleRate();
        }
        freqVFO.setFrequency(r->centerFreq + (long long)std::llround((pct - 0.5) * rate));
        sidebar.recalculateLayout();
        sidebar.updateStyle();
    };

    // Clears the waterfall pictures (the new receiver's rows start from the top)
    auto clearWaterfalls = [&]() {
        std::vector<uint8_t> black((size_t)INTERNAL_WATERFALL_WIDTH * WATERFALL_HEIGHT_PX * 4, 0);
        for (size_t i = 3; i < black.size(); i += 4) black[i] = 255;
        wTex.update(black.data());
        std::fill(miniWaterfall.begin(), miniWaterfall.end(), 0);
        miniTex.update(miniWaterfall.data());
    };

    auto switchReceiver = [&](Receiver* to) {
        if (!to || to == cur) return;
        if (scanner.running()) { scanner.stop(); setScanUi(); }
        chDrag = ChannelDrag();
        chEdge = ChannelEdgeDrag();
        isDraggingScale = isSpectrumDragging = isDraggingBW = isPanningView = false;
        if (TextInput::currentFocus) TextInput::currentFocus->blur();
        if (Dropdown::currentActive) { Dropdown::currentActive->isOpen = false; Dropdown::currentActive = nullptr; }
        Receiver* from = cur;
        float vol; int theme;
        {
            std::lock_guard<std::mutex> l(from->sd.mtx);
            vol = from->sd.volume; theme = from->sd.waterfallTheme;
            from->sd.scanMute = false;
        }
        saveUi(from);
        from->sd.visible = false;   // its sound goes on (the speaker button on its tab decides)
        cur = to;
        {
            std::lock_guard<std::mutex> l(to->sd.mtx);
            to->sd.volume = vol;   // one speaker, one volume
            to->sd.waterfallTheme = theme;
            to->sd.newWaterfallData = false;
        }
        to->sd.visible = true;
        applyUi(to);
        clearWaterfalls();
    };

    auto addReceiver = [&]() {
        if (receivers.size() >= 8) { showPopup("Receivers", "Up to 8 receivers can run at once."); return; }
        Receiver* r = newReceiver();
        r->sourceType = 0;
        r->centerFreq = r->sd.centerFreq = 0;
        {
            // the new tab starts with the display settings of the current one
            std::lock_guard<std::mutex> a(cur->sd.mtx);
            std::lock_guard<std::mutex> b(r->sd.mtx);
            r->sd.minDb = cur->sd.minDb;
            r->sd.maxDb = cur->sd.maxDb;
            r->sd.recPath = cur->sd.recPath;
            r->sd.mode = Mode::NFM;
            r->sd.bandwidth = 12500.0;
        }
        switchReceiver(r);
    };

    auto closeReceiver = [&](Receiver* r) {
        if (receivers.size() <= 1 || !r) return;
        if (r == cur) {
            Receiver* other = nullptr;
            for (auto& x : receivers) if (x.get() != r) { other = x.get(); break; }
            switchReceiver(other);
        }
        const int stream = r->sd.audioStream;
        for (auto it = receivers.begin(); it != receivers.end(); ++it)
            if (it->get() == r) { receivers.erase(it); break; }   // stops its DSP thread and source
        audio.removeStream(stream);
    };

    auto drawRxTabs = [&]() {
        rxTabHits.clear();
        sf::RectangleShape bar({layout.winW, (float)RX_TAB_H});
        bar.setPosition({0.0f, (float)TOPBAR_DRAW_H});
        bar.setFillColor(sf::Color(26, 27, 32));
        window.draw(bar);
        const sf::Vector2f mp = window.mapPixelToCoords(sf::Mouse::getPosition(window));
        auto txt = [&](const std::string& str, float x, float y, sf::Color c) {
            sf::Text t(font, sf::String::fromUtf8(str.begin(), str.end()), 22);
            t.setScale({0.5f, 0.5f});
            t.setFillColor(c);
            t.setPosition({std::round(x), std::round(y)});
            window.draw(t);
            return t.getGlobalBounds().size.x;
        };
        float x = 6.0f;
        const float y = (float)TOPBAR_DRAW_H + 2.0f, h = (float)RX_TAB_H - 2.0f;
        for (auto& rxp : receivers) {
            Receiver* r = rxp.get();
            const bool sel = r == cur;
            bool playing, dec = false;
            {
                std::lock_guard<std::mutex> l(r->sd.mtx);
                playing = r->sd.isPlaying;
                dec = r->sd.aprsEnabled;
                for (SDRModule* m : r->sd.activeDecoders) dec = dec || (m && m->enabled);
            }
            const std::string title = r->title();
            sf::Text probe(font, sf::String::fromUtf8(title.begin(), title.end()), 22);
            const bool closable = receivers.size() > 1;
            const float tw = probe.getGlobalBounds().size.x * 0.5f + 48.0f + (closable ? 18.0f : 0.0f);
            sf::RectangleShape bg({tw, h});
            bg.setPosition({x, y});
            bg.setFillColor(sel ? sf::Color(19, 19, 21) : sf::Color(42, 44, 52));
            bg.setOutlineThickness(1.0f);
            bg.setOutlineColor(sel ? Theme::Accent : sf::Color(62, 62, 70));
            window.draw(bg);
            // state: green = playing, dot ring = decoders running
            sf::CircleShape led(4.0f);
            led.setPosition({x + 6.0f, y + h / 2.0f - 4.0f});
            led.setFillColor(playing ? sf::Color(80, 230, 110) : sf::Color(80, 80, 88));
            if (dec) { led.setOutlineThickness(1.5f); led.setOutlineColor(sf::Color(255, 210, 0)); }
            window.draw(led);
            // speaker: this receiver's sound on / off (crossed out = muted)
            const bool spk = r->sd.audioOut;
            const sf::FloatRect spkR({x + 15.0f, y}, {22.0f, h});
            {
                const sf::Color c = spkR.contains(mp) ? sf::Color::White : spk ? sf::Color(200, 200, 210) : sf::Color(110, 110, 118);
                const float sx = x + 18.0f, sy = y + h / 2.0f;
                sf::RectangleShape box({3.0f, 5.0f});
                box.setPosition({sx, sy - 2.5f});
                box.setFillColor(c);
                window.draw(box);
                sf::ConvexShape cone(4);
                cone.setPoint(0, {sx + 3.0f, sy - 2.5f}); cone.setPoint(1, {sx + 7.0f, sy - 6.0f});
                cone.setPoint(2, {sx + 7.0f, sy + 6.0f}); cone.setPoint(3, {sx + 3.0f, sy + 2.5f});
                cone.setFillColor(c);
                window.draw(cone);
                if (spk) {
                    for (float rr : {3.0f, 6.0f}) {   // sound waves
                        sf::VertexArray arc(sf::PrimitiveType::LineStrip, 5);
                        for (int k = 0; k < 5; k++) {
                            const float a = (-0.8f + 0.4f * k);
                            arc[k].position = {sx + 8.0f + rr * std::cos(a), sy + rr * std::sin(a)};
                            arc[k].color = c;
                        }
                        window.draw(arc);
                    }
                } else {
                    sf::VertexArray xx(sf::PrimitiveType::Lines, 4);
                    xx[0].position = {sx + 9.0f, sy - 3.0f}; xx[1].position = {sx + 14.0f, sy + 3.0f};
                    xx[2].position = {sx + 14.0f, sy - 3.0f}; xx[3].position = {sx + 9.0f, sy + 3.0f};
                    for (int k = 0; k < 4; k++) xx[k].color = sf::Color(240, 90, 90);
                    window.draw(xx);
                }
            }
            txt(title, x + 40.0f, y + 3.0f, sel ? sf::Color::White : sf::Color(180, 180, 190));
            sf::FloatRect closeR;
            if (closable) {
                closeR = sf::FloatRect({x + tw - 18.0f, y}, {18.0f, h});
                txt("x", x + tw - 13.0f, y + 2.0f, closeR.contains(mp) ? sf::Color(255, 120, 120) : sf::Color(130, 130, 140));
            }
            rxTabHits.push_back({sf::FloatRect({x, y}, {tw, h}), closeR, spkR, r});
            x += tw + 4.0f;
        }
        // the pipeline tabs (drag one sideways to put it elsewhere)
        pipeTabHits.clear();
        for (pl::Workspace* w : pl::Hub::get().list()) {
            const bool sel = pipeShown && w->uid == pipeUid;
            std::string title = "Pipeline: " + w->editor.title();
            if (title.size() > 42) title = title.substr(0, 40) + "..";
            sf::Text probe(font, sf::String::fromUtf8(title.begin(), title.end()), 22);
            const float tw = probe.getGlobalBounds().size.x * 0.5f + 40.0f;
            const bool dragged = pipeDragMoved && w->uid == pipeDragUid;
            sf::RectangleShape bg({tw, h});
            bg.setPosition({x, y});
            bg.setFillColor(dragged ? sf::Color(80, 60, 30) : sel ? sf::Color(19, 19, 21) : sf::Color(52, 42, 30));
            bg.setOutlineThickness(1.0f);
            bg.setOutlineColor(sel || dragged ? SignalAnalyzer::color() : sf::Color(110, 80, 40));
            window.draw(bg);
            // busy: the share of real time its thread needs (red when it cannot keep up)
            const double ld = w->load();
            if (ld > 0.5) {
                sf::RectangleShape bar({(tw - 8.0f) * (float)std::min(1.0, ld), 2.0f});
                bar.setPosition({x + 4.0f, y + h - 3.0f});
                bar.setFillColor(ld > 0.9 ? sf::Color(240, 80, 80) : sf::Color(240, 190, 60));
                window.draw(bar);
            }
            txt(title, x + 10.0f, y + 3.0f, sel ? sf::Color(255, 190, 110) : sf::Color(210, 170, 120));
            const sf::FloatRect closeR({x + tw - 18.0f, y}, {18.0f, h});
            txt("x", x + tw - 13.0f, y + 2.0f, closeR.contains(mp) ? sf::Color(255, 120, 120) : sf::Color(150, 130, 110));
            pipeTabHits.push_back({sf::FloatRect({x, y}, {tw, h}), closeR, w->uid});
            x += tw + 4.0f;
        }
        // "+": another receiver
        const float aw = 128.0f;
        rxAddHit = sf::FloatRect({x, y}, {aw, h});
        sf::RectangleShape add({aw, h});
        add.setPosition({x, y});
        add.setFillColor(rxAddHit.contains(mp) ? sf::Color(50, 55, 66) : sf::Color(32, 34, 40));
        add.setOutlineThickness(1.0f);
        add.setOutlineColor(sf::Color(62, 62, 70));
        window.draw(add);
        txt("+  Add receiver", x + 14.0f, y + 3.0f, Theme::Accent);
        x += aw + 4.0f;
        // "+ Pipeline": an empty pipeline tab (pick its receiver in the Channel input)
        const float pw = 104.0f;
        pipeAddHit = sf::FloatRect({x, y}, {pw, h});
        sf::RectangleShape padd({pw, h});
        padd.setPosition({x, y});
        padd.setFillColor(pipeAddHit.contains(mp) ? sf::Color(66, 52, 32) : sf::Color(40, 34, 28));
        padd.setOutlineThickness(1.0f);
        padd.setOutlineColor(sf::Color(110, 80, 40));
        window.draw(padd);
        txt("+  Pipeline", x + 14.0f, y + 3.0f, SignalAnalyzer::color());
    };

    // Smoothed values of the signal meter in the status bar
    float meterSnr = -1.0f;
    double meterSignal = -127.0;
    sf::Clock meterClock;

    // --- The other receivers of the last session (RX1 uses the settings above) ---
    {
        const int rxCount = std::clamp(settingsMgr.getInt("rx_count", 1), 1, 8);
        Receiver* first = cur;
        for (int i = 1; i < rxCount; i++) {
            const std::string k = "rx" + std::to_string(i + 1) + "_";
            Receiver* r = newReceiver();
            switchReceiver(r);
            long long f = 0;
            try { f = std::stoll(settingsMgr.getString(k + "frequency", "0")); } catch (...) {}
            r->centerFreq = f;
            { std::lock_guard<std::mutex> l(r->sd.mtx); r->sd.centerFreq = f; }
            r->hostText = settingsMgr.getString(k + "host", txtHost->getText());
            txtHost->setText(r->hostText);
            const int type = std::clamp(settingsMgr.getInt(k + "source_type", 0), 0, 4);
            std::string devHex = settingsMgr.getString(k + "device", "");
            if (!devHex.empty() && devHex[0] == 'x') devHex.erase(0, 1);   // "x": keeps it a string in the file
            const std::string devId = fromHex(devHex);
            const int rateIdx = std::max(0, settingsMgr.getInt(k + "rate", 0));
            r->sd.hw.ppm = std::clamp(settingsMgr.getInt(k + "ppm", 0), -200, 200);
            if (type == 4) {
                r->sourceType = 4;
                ddSourceType->setSelection(4);
                updateRadioControls(4);
                ddRate->setOptions(RTL_SAMPLE_RATES_TXT);
                ddRate->setSelection(std::min(rateIdx, (int)RTL_SAMPLE_RATES_TXT.size() - 1));
                refreshSourceUI();
            } else if (type >= 1 && type <= 3) {
                ddSourceType->setSelection(type);
                r->cachedDevices = type == 1 ? RtlSdrSource::getDeviceList()
                                 : type == 2 ? SdrPlaySource::getDeviceList() : AudioCaptureSource::getDeviceList();
                std::vector<std::string> names;
                int found = -1;
                for (size_t d = 0; d < r->cachedDevices.size(); d++) {
                    names.push_back(r->cachedDevices[d].name);
                    if (r->cachedDevices[d].id == devId) found = (int)d;
                }
                if (names.empty()) names.push_back("No Devices Found");
                ddDevice->setOptions(names);
                if (found >= 0) ddDevice->setSelection(found);
                r->sourceType = type;
                updateRadioControls(type);
                refreshSourceUI();
                // only the same device as last time (never another receiver's radio)
                if (found >= 0) {
                    quietSourceErrors = true;
                    doOpenSource(type, r->cachedDevices[found].id, rateIdx, "");
                    quietSourceErrors = false;
                }
            }
            const int m = settingsMgr.getInt(k + "mode", (int)Mode::NFM);
            restoreMode((Mode)std::clamp(m, 0, MODE_LAST));
            const float bw = settingsMgr.getFloat(k + "bandwidth", 12500.0f);
            slBW->setValueSilent(bw);
            slBW->onChange(slBW->currentVal);
        }
        switchReceiver(first);
        for (size_t i = 0; i < receivers.size(); i++) {
            Receiver& r = *receivers[i];
            r.sd.audioOut = settingsMgr.getBool("rx" + std::to_string(i + 1) + "_speaker", true);
            // the output device by name (stored hex: settings values lose spaces)
            const std::string dev = fromHex(settingsMgr.getString("rx" + std::to_string(i + 1) + "_audio_dev", ""));
            if (!dev.empty() && audio.deviceIndex(dev) > 0 && audio.setDevice(r.sd.audioStream, audio.deviceIndex(dev))) r.audioDevice = dev;
        }
        ddAudio->setSelection(audio.deviceOf(cur->sd.audioStream));
    }

    // --- The pipeline tabs of the last session (by chain name), and the one shown ---
    {
        const int n = std::clamp(settingsMgr.getInt("pipe_tabs", 0), 0, 32);
        for (int i = 0; i < n; i++) {
            const std::string name = fromHex(settingsMgr.getString("pipe_tab" + std::to_string(i), ""));
            if (!name.empty() && !pl::Hub::get().openIn(name, nullptr)) pl::Hub::get().add(name, "", &font);
        }
        const auto tabsNow = pl::Hub::get().list();
        const int sh = settingsMgr.getInt("pipe_shown", -1);
        if (sh >= 0 && sh < (int)tabsNow.size()) { pipeUid = tabsNow[sh]->uid; pipeShown = true; }
    }

    while (window.isOpen()) {
        // receiver tab actions from the last frame's clicks (done here, before anything of
        // this frame looks at `cur`)
        if (rxAction.kind != 0) {
            const RxAction a = rxAction;
            rxAction = {};
            if (a.kind == 1) { pipeShown = false; addReceiver(); }
            else if (a.kind == 2) {
                closeReceiver(a.rx);   // (a pipeline taking it shows "not available" on its input)
            }
            else if (a.kind == 3) switchReceiver(a.rx);
        }
        // A pipeline's Channel input took a receiver: its Analyzer (the orange channel) goes on
        {
            static sf::Clock wantClock;
            if (wantClock.getElapsedTime().asSeconds() > 0.3f) {
                wantClock.restart();
                for (int id : pl::Hub::get().wanted()) {
                    for (auto& rxp : receivers) {
                        Receiver* r = rxp.get();
                        if (r->id != id || !r->analyzer || r->analyzer->enabled) continue;
                        for (size_t i = 0; i < r->decMods.size(); i++) {
                            if (r->decMods[i] != r->analyzer) continue;
                            activateDecoderOn(r, i, true);
                            if (r == cur) decoders[i].chk->checked = true;
                        }
                    }
                }
            }
        }
        syncMacWindowSize(window);   // macOS window tiling resizes without telling SFML
        sf::Vector2u currSize = window.getSize();
        // Auto scale: look again now and then (the window moved to another monitor, the
        // system scale was changed). Windows / macOS: a cheap query, every second; Linux asks
        // the X server and gsettings, so only when the window was resized or got the focus.
        bool rescale = false;
        if (uiScaleIdx == 0) {
#if defined(__linux__)
            const bool look = currSize != lastSize || scaleRecheckFocus;
#else
            const bool look = currSize != lastSize || scaleRecheck.getElapsedTime().asSeconds() > 1.0f;
#endif
            if (look) { scaleRecheck.restart(); scaleRecheckFocus = false; rescale = detectAutoScale(); }
        }
        if ((currSize != lastSize || rescale) && currSize.x > 0 && currSize.y > 0) {
            applyUiScale();
            lastSize = currSize;
        }

        bool isHw = false;
        double hwSampleRate = 2e6;
        double prog = 0.0;
        float autoGainDb = -1.0f;
        float adcOverload = 0.0f;
        bool agcLimitedByRds = false;
        bool sourceReconnecting = false;
        {
            std::lock_guard<std::mutex> l(cur->sd.sourceMtx);
            if (cur->sd.source) {
                sourceReconnecting = cur->sd.source->isReconnecting();
                isHw = cur->sd.source->isHardware();
                hwSampleRate = cur->sd.source->getSampleRate();
                prog = cur->sd.source->getProgress();
                autoGainDb = cur->sd.source->getAutoGainDb();
                adcOverload = cur->sd.source->getOverloadFraction();
                agcLimitedByRds = cur->sd.source->isAgcLimited();
            }
        }
        if (!(hwSampleRate > 0.0)) hwSampleRate = 2e6;   // no source open: keep the scale math finite

        // Show the gain the software AGC has chosen next to the RF gain slider
        if (chkAgc->checked) {
            std::string agcText = "RF Gain: AGC";
            if (autoGainDb >= 0.0f) {
                char b[64];
                std::snprintf(b, sizeof(b), agcLimitedByRds ? "RF Gain: AGC (%.1f dB, tuned for RDS)" : "RF Gain: AGC (%.1f dB)", autoGainDb);
                agcText = b;
            }
            if (slGain->label.getString() != agcText) slGain->setText(agcText);
        }

        // RTL-TCP: detect that the server dropped the connection and reset the UI
        if (cur->tcpConnected) {
            bool lost = false;
            {
                std::lock_guard<std::mutex> l(cur->sd.sourceMtx);
                RtlTcpSource* t = dynamic_cast<RtlTcpSource*>(cur->sd.source.get());
                if (t && t->isConnectionLost()) lost = true;
            }
            if (lost) {
                doDisconnectTcp();
                showPopup("RTL-TCP", "Connection to the server was lost.");
            }
        }

        freqVFO.setEnabled(true);
        if (!ddSourceType->isOpen && !ddRate->isOpen && !ddDevice->isOpen) freqVFO.update(window);
        volSlider.update(window);

        if (std::abs(volSlider.currentVal - lastVolPos) > 0.0005f) {
            lastVolPos = volSlider.currentVal;
            volSlider.setText(volumeText(lastVolPos));
            for (auto& rxp : receivers) {   // one speaker, one volume for every receiver
                std::lock_guard<std::mutex> l(rxp->sd.mtx);
                rxp->sd.volume = volumeGain(lastVolPos);
            }
        }

        if (cur->sourceType == 0) {
            timeSlider.update(window);
            if (timeSlider.isDragging) {
                std::lock_guard<std::mutex> l(cur->sd.mtx);
                cur->sd.pendingSeekRequest = timeSlider.currentVal;
            } else {
                timeSlider.currentVal = prog;
                timeSlider.updateHandlePos();
            }
        }

        bool aprsOn = false;
        bool rdsOn = false;
        RdsInfo rdsInfo;
        double tunePct = 0.5;
        Mode mode = Mode::NFM;
        float currentZoom = 1.0f;
        int currentTheme = 0;
        double viewCenter = 0.5;

        {
            std::lock_guard<std::mutex> l(cur->sd.mtx);
            aprsOn = cur->sd.aprsEnabled;
            rdsOn = cur->sd.rdsEnabled;
            rdsInfo = cur->sd.rds;
            tunePct = cur->sd.tunedFreqPercent;
            mode = cur->sd.mode;
            currentZoom = cur->sd.zoomLevel;
            currentTheme = cur->sd.waterfallTheme;
            viewCenter = cur->sd.viewCenterPct;
        }

        slZoom->update(window);
        // the corner zoom slider: placed every frame (the timeline strip comes and goes)
        {
            const float zbTimelineH = (cur->sourceType == 0) ? TIMELINE_H : 0.0f;
            slZoomBar.setPosition(layout.specW - 180.0f - 20.0f, layout.winH - zbTimelineH - 40.0f + 4.0f);
            if (slZoomBar.isDragging && !sf::Mouse::isButtonPressed(sf::Mouse::Button::Left)) slZoomBar.isDragging = false;
            slZoomBar.update(window);
            if (!slZoomBar.isDragging) slZoomBar.setValueSilent(slZoom->currentVal);
            static float zbShown = -1.0f;
            if (std::abs(zbShown - slZoomBar.currentVal) > 0.01f) { zbShown = slZoomBar.currentVal; slZoomBar.setText(zoomBarText(zbShown)); }
        }

        double visibleFraction = 1.0 / currentZoom;
        double viewStartPct = viewCenter - visibleFraction / 2.0;
        double viewEndPct = viewCenter + visibleFraction / 2.0;
        if (viewStartPct < 0.0) { viewStartPct = 0.0; viewEndPct = visibleFraction; viewCenter = viewStartPct + visibleFraction/2.0; }
        if (viewEndPct > 1.0) { viewEndPct = 1.0; viewStartPct = 1.0 - visibleFraction; viewCenter = viewStartPct + visibleFraction/2.0; }
        double viewWidthPct = viewEndPct - viewStartPct;

        float tunerPosInView = (tunePct - viewStartPct) / viewWidthPct;
        float visualCenterX = tunerPosInView * layout.specW;
        float bwPct = (slBW->currentVal / hwSampleRate);
        float bwPixels = (bwPct / viewWidthPct) * layout.specW;

        // --- Running decoders: their tabs, the selected one's panel ---
        const std::vector<DecTab> tabs = collectTabs(cur);
        {
            bool found = false;
            for (const DecTab& t : tabs) if (t.name == cur->selectedTab) found = true;
            if (!found) cur->selectedTab = tabs.empty() ? std::string() : tabs.back().name;
        }
        const DecTab* selTab = nullptr;
        for (const DecTab& t : tabs) if (t.name == cur->selectedTab) selTab = &t;
        SDRModule* panelMod = selTab ? selTab->mod : nullptr;   // nullptr + selTab: the APRS tab
        const bool aprsShown = selTab && !selTab->mod;
        aprsOn = aprsOn && aprsShown;   // the APRS list is the APRS tab's panel
        const float timelineStripH = (cur->sourceType == 0) ? TIMELINE_H : 0.0f;
        const float aprsTopY = layout.winH - timelineStripH - 200.0f;

        // --- Decoder channels: coloured bands + handles on the tuning bar (all running decoders) ---
        auto setRunning = [&](ChannelSet* cs) {
            if (cur->inputIsAudio) return false;
            for (const DecTab& t : tabs) if (t.set == cs) return true;
            return false;
        };
        if (chDrag.set && !setRunning(chDrag.set)) chDrag = ChannelDrag();   // decoder switched off while dragging
        if (chEdge.set && !setRunning(chEdge.set)) chEdge = ChannelEdgeDrag();
        struct ChHandle { DecoderChannel ch; float x; float rawX; int edge; ChannelSet* set; sf::Color col; std::string dec; };
        std::vector<ChHandle> chHandles;
        const double vfoOffsetHz = (tunePct - 0.5) * hwSampleRate;
        const long long vfoFreqHz = cur->centerFreq + (long long)std::llround(vfoOffsetHz);
        auto offsetToX = [&](double off) {
            return (float)(((0.5 + off / hwSampleRate) - viewStartPct) / viewWidthPct * layout.specW);
        };
        auto xToFreq = [&](float x) {
            const double pct = viewStartPct + (x / layout.specW) * viewWidthPct;
            return cur->centerFreq + (long long)std::llround((pct - 0.5) * hwSampleRate);
        };
        const float tbTop = (float)TOP_BAR_H + layout.specH, tbBot = tbTop + layout.tuneBarH;
        if (!cur->inputIsAudio) {
            // the selected decoder's channels last: drawn on top, picked first when they overlap
            std::vector<const DecTab*> order;
            for (const DecTab& t : tabs) if (&t != selTab) order.push_back(&t);
            if (selTab) order.push_back(selTab);
            for (const DecTab* t : order) {
                if (!t->set) continue;
                for (const DecoderChannel& c : t->set->snapshot()) {
                    const double off = c.followVfo ? vfoOffsetHz : (double)(c.freqHz - cur->centerFreq);
                    const float x = offsetToX(off);
                    const int edge = x < 0.0f ? -1 : (x > layout.specW ? 1 : 0);
                    chHandles.push_back({c, std::clamp(x, 10.0f, layout.specW - 10.0f), x, edge, t->set, t->color, t->name});
                }
            }
        }
        auto handleAt = [&](sf::Vector2f m) -> int {
            if (m.y < tbTop - 3.0f || m.y > tbBot + 3.0f || m.x > layout.specW) return -1;
            int best = -1;
            float bestD = 10.0f;
            for (size_t i = 0; i < chHandles.size(); i++) {
                const float d = std::abs(chHandles[i].x - m.x);
                if (d <= bestD) { bestD = d; best = (int)i; }   // <=: the later (selected decoder) wins a tie
            }
            return best;
        };
        // Edges of the main tuner's band (grey) in pixels
        float tunerEdgeL = visualCenterX - bwPixels / 2.0f, tunerEdgeR = visualCenterX + bwPixels / 2.0f;
        if (mode == Mode::USB) { tunerEdgeL = visualCenterX; tunerEdgeR = visualCenterX + bwPixels; }
        if (mode == Mode::LSB) { tunerEdgeL = visualCenterX - bwPixels; tunerEdgeR = visualCenterX; }
        auto chCentreOffset = [&](const DecoderChannel& c) {
            return c.followVfo ? vfoOffsetHz : (double)(c.freqHz - cur->centerFreq);
        };
        // A draggable edge of a yellow band on the spectrum: index into chHandles, or -1.
        // Bands whose width is the tuner's have no own edges (the grey band's edges change both).
        // SSB channels: only the edge away from the dial frequency moves.
        auto edgeAt = [&](sf::Vector2f m) -> int {
            if (m.y < (float)TOP_BAR_H || m.y > (float)TOP_BAR_H + layout.specH || m.x > layout.specW) return -1;
            int best = -1;
            float bestD = 6.0f;
            for (size_t i = 0; i < chHandles.size(); i++) {
                const DecoderChannel& c = chHandles[i].ch;
                if (!chHandles[i].set->resizable || chHandles[i].edge != 0 || chHandles[i].set->widthFollowsTuner(c)) continue;
                const double off = chCentreOffset(c);
                if (c.sideband <= 0) { const float d = std::abs(offsetToX(c.lowOffset(off)) - m.x); if (d < bestD) { bestD = d; best = (int)i; } }
                if (c.sideband >= 0) { const float d = std::abs(offsetToX(c.highOffset(off)) - m.x); if (d < bestD) { bestD = d; best = (int)i; } }
            }
            return best;
        };
        auto updateEdgeDrag = [&](float mx) {
            const float x = std::clamp(mx, 0.0f, layout.specW);
            chEdge.x = x;
            const ChHandle* h = nullptr;
            for (const ChHandle& ch : chHandles) if (ch.ch.id == chEdge.id && ch.set == chEdge.set) h = &ch;
            if (!h) return;
            const DecoderChannel& c = h->ch;
            const double hzPerPx = (hwSampleRate * viewWidthPct) / layout.specW;
            const float cx = offsetToX(chCentreOffset(c));
            double w = c.sideband > 0 ? (x - cx) * hzPerPx
                     : c.sideband < 0 ? (cx - x) * hzPerPx
                                      : 2.0 * std::abs(x - cx) * hzPerPx;
            // on the grey band's edge: the tuner's width (only for channels that may follow it)
            chEdge.snapTuner = c.followVfo && chEdge.set->followTunerWidth &&
                               (std::abs(x - tunerEdgeL) < 6.0f || std::abs(x - tunerEdgeR) < 6.0f);
            if (chEdge.snapTuner) w = slBW->currentVal;
            else w = std::round(w / 100.0) * 100.0;
            w = std::clamp(w, 500.0, std::max(500.0, hwSampleRate * 0.9));
            chEdge.widthHz = w;
            chEdge.set->setChannelWidth(chEdge.id, w);
        };
        auto finishEdgeDrag = [&]() {
            if (chEdge.set && chEdge.snapTuner) chEdge.set->resetChannelWidth(chEdge.id);
            chEdge = ChannelEdgeDrag();
        };

        // Puts the tuner on a frequency (retunes the radio when it is outside the band)
        auto tuneVfoTo = [&](long long f) {
            const double halfBW = hwSampleRate / 2.0;
            if (isHw && (f < cur->centerFreq - halfBW || f > cur->centerFreq + halfBW)) {
                cur->centerFreq = f;
                {
                    std::lock_guard<std::mutex> l(cur->sd.mtx);
                    cur->sd.centerFreq = f;
                    cur->sd.tunedFreqPercent = 0.5;
                    cur->sd.viewCenterPct = 0.5;
                }
                cur->pendingCenterFreq = f;
                cur->debouncer.restart();
            } else {
                std::lock_guard<std::mutex> l(cur->sd.mtx);
                cur->sd.tunedFreqPercent = std::clamp(0.5 + (double)(f - cur->centerFreq) / hwSampleRate, 0.0, 1.0);
            }
            freqVFO.setFrequency(f);
        };
        auto updateChannelDrag = [&](float mx) {
            const float x = std::clamp(mx, 0.0f, layout.specW);
            chDrag.x = x;
            if (std::abs(x - chDrag.startX) > 3.0f) chDrag.moved = true;
            if (!chDrag.moved) return;
            // snap: the tuner first, then the known frequencies
            chDrag.snap = 0;
            chDrag.freqHz = xToFreq(x);
            float bestD = 10.0f;
            if (std::abs(visualCenterX - x) < bestD) { bestD = std::abs(visualCenterX - x); chDrag.snap = 2; chDrag.freqHz = vfoFreqHz; }
            if (chDrag.snap == 0 && cur->centerFreq > 0) {
                for (long long pf : chDrag.set->presets) {
                    const float px = offsetToX((double)(pf - cur->centerFreq));
                    if (std::abs(px - x) < bestD) { bestD = std::abs(px - x); chDrag.snap = 1; chDrag.freqHz = pf; }
                }
            }
            if (chDrag.snap == 0) {
                const long long step = STEP_VALUES[ddSnap->selectedIndex] > 0 ? STEP_VALUES[ddSnap->selectedIndex] : 100;
                chDrag.freqHz = (long long)std::llround((double)chDrag.freqHz / step) * step;
            }
            chDrag.set->moveTo(chDrag.id, chDrag.freqHz);
        };
        auto finishChannelDrag = [&]() {
            if (!chDrag.set) return;
            if (!chDrag.moved) {
                // a click: listen to the channel (tuner + the decoder's mode)
                for (const ChHandle& h : chHandles) {
                    if (h.ch.id != chDrag.id || h.set != chDrag.set) continue;
                    if (!h.ch.followVfo) tuneVfoTo(h.ch.freqHz);
                    if (!cur->inputIsAudio) restoreMode(h.ch.listenMode);
                }
            } else if (chDrag.snap == 2) {
                chDrag.set->followTuner(chDrag.id);
            }
            chDrag = ChannelDrag();
        };

        auto applySpectrumTuning = [&](float mouseX) {
            bool isHwLocal = false;
            double sr = 2e6;
            {
                std::lock_guard<std::mutex> l(cur->sd.sourceMtx);
                if (cur->sd.source) {
                    isHwLocal = cur->sd.source->isHardware();
                    sr = cur->sd.source->getSampleRate();
                }
            }
            if (!(sr > 0.0)) return;   // no source open (a file that failed to load): nothing to tune

            double clickViewPct = mouseX / layout.specW;
            double clickedAbsolutePct = viewStartPct + clickViewPct * viewWidthPct;

            double offsetHz = (clickedAbsolutePct - 0.5) * sr;
            long long clickedFreq = cur->centerFreq + (long long)offsetHz;

            long long step = STEP_VALUES[ddSnap->selectedIndex];
            if (step > 0) {
                clickedFreq = (long long)std::round((double)clickedFreq / step) * step;
                double newOffset = (double)(clickedFreq - cur->centerFreq);
                clickedAbsolutePct = 0.5 + (newOffset / sr);
            }

            if (cur->stickyCenter && isHwLocal) {
                cur->pendingCenterFreq = clickedFreq;
                cur->debouncer.restart();
                {
                    std::lock_guard<std::mutex> l(cur->sd.mtx);
                    cur->sd.tunedFreqPercent = 0.5;
                    cur->sd.viewCenterPct = 0.5;
                }
                freqVFO.setFrequency(clickedFreq);
            } else {
                {
                    std::lock_guard<std::mutex> l(cur->sd.mtx);
                    cur->sd.tunedFreqPercent = clickedAbsolutePct;
                }
                freqVFO.setFrequency(clickedFreq);
            }
        };

        for (auto& rxp : receivers) {   // every receiver: a tab may be left right after a change
            Receiver& r = *rxp;
            if (r.pendingRfGain > -900.0f && r.gainDebouncer.getElapsedTime().asMilliseconds() > GAIN_LATENCY_MS) {
                {
                    std::lock_guard<std::mutex> l(r.sd.sourceMtx);
                    if (r.sd.source && r.sd.source->isHardware()) r.sd.source->setGain((int)r.pendingRfGain);
                }
                r.pendingRfGain = -999.0f;
                r.gainDebouncer.restart();
            }
        }

        // Decoder panel at the bottom of the waterfall. With a file source the timeline
        // stays below it, so it can still be used.
        // the panel of the selected decoder (APRS: its list), the tab strip above it
        const float panelH = panelMod ? panelMod->panelHeight(layout.winH) : aprsShown ? 200.0f : 0.0f;
        const float panelTop = layout.winH - timelineStripH - panelH;
        const float tabTop = panelTop - TAB_H;
        const float decoderPanelTop = tabs.empty() ? layout.winH : tabTop;   // winH = no panel
        // everything below this is bottom UI (status bar / decoder panel / timeline): clicks
        // and the mouse wheel there must not tune
        const float bottomUiTop = std::min(decoderPanelTop, layout.winH - timelineStripH - 40.0f);
        // modules with a popup open get every event first
        std::vector<SDRModule*> runningMods;
        for (const DecTab& t : tabs) if (t.mod) runningMods.push_back(t.mod);

        // events held while a dialog was open come first
        auto nextEvent = [&]() -> std::optional<sf::Event> {
            if (!heldEvents.empty()) { const sf::Event e = heldEvents.front(); heldEvents.pop_front(); return e; }
            return window.pollEvent();
        };
        while (const std::optional<sf::Event> ev = nextEvent()) {
            if (ev->is<sf::Event::Closed>()) window.close();
            if (ev->is<sf::Event::FocusGained>()) scaleRecheckFocus = true;

            // UI scale from the keyboard: Ctrl (Cmd on macOS) + "+" / "-" one step, "0" Auto
            if (const auto* kp = ev->getIf<sf::Event::KeyPressed>()) {
#ifdef __APPLE__
                const bool mod = kp->system;
#else
                const bool mod = kp->control;
#endif
                using K = sf::Keyboard::Key;
                const bool up = kp->code == K::Equal || kp->code == K::Add;
                const bool down = kp->code == K::Hyphen || kp->code == K::Subtract;
                const bool reset = kp->code == K::Num0 || kp->code == K::Numpad0;
                if (mod && (up || down || reset)) {
                    int idx = 0;
                    if (!reset) {
                        const float s = getUiScale();
                        idx = uiScaleIdx;
                        if (up) { for (int i = 1; i < (int)UI_SCALE_VALUES.size(); i++) if (UI_SCALE_VALUES[i] > s + 0.01f) { idx = i; break; } }
                        else    { for (int i = (int)UI_SCALE_VALUES.size() - 1; i >= 1; i--) if (UI_SCALE_VALUES[i] < s - 0.01f) { idx = i; break; } }
                    }
                    if (idx != uiScaleIdx) {
                        uiScaleIdx = idx;
                        ddScale->setSelection(idx);
                        applyUiScale();
                    }
                    continue;
                }
            }

            // Global mouse-release catcher - prevents UI from getting stuck
            if (const auto* mr = ev->getIf<sf::Event::MouseButtonReleased>()) {
                if (mr->button == sf::Mouse::Button::Left && chDrag.set) {
                    finishChannelDrag();
                    continue;
                }
                if (mr->button == sf::Mouse::Button::Left && chEdge.set) {
                    finishEdgeDrag();
                    continue;
                }
                if (mr->button == sf::Mouse::Button::Left) {
                    isSpectrumDragging = false;
                    isDraggingScale = false;
                    isDraggingBW = false;
                }
                if (mr->button == sf::Mouse::Button::Right) {
                    isPanningView = false;
                }
            }

            if (showAprsModal) {
                btnModalClose.handleEvent(*ev, window);
                btnModalCopy.handleEvent(*ev, window);
                if (const auto* mb = ev->getIf<sf::Event::MouseButtonPressed>()) {
                    sf::Vector2f m = window.mapPixelToCoords(sf::Mouse::getPosition(window));
                    float mx = (layout.winW - 600) / 2;
                    float my = (layout.winH - 400) / 2;
                    sf::FloatRect modalRect({mx, my}, {600.f, 400.f});
                    if (!modalRect.contains(m)) showAprsModal = false;
                }
                continue;
            }

            // Popup window of a decoder (message details): it takes every event
            {
                SDRModule* modalMod = nullptr;
                for (SDRModule* m : runningMods) if (m->enabled && m->modalOpen()) modalMod = m;
                if (modalMod) {
                    modalMod->handleEvent(*ev, window);
                    continue;
                }
            }

            // Receiver tabs (below the top bar): switch / add / close
            if (const auto* mbr = ev->getIf<sf::Event::MouseButtonPressed>()) {
                const sf::Vector2f m = window.mapPixelToCoords(sf::Mouse::getPosition(window));
                if (m.y >= TOPBAR_DRAW_H && m.y < TOP_BAR_H) {
                    if (mbr->button == sf::Mouse::Button::Left) {
                        bool done = false;
                        if (rxAddHit.contains(m)) { rxAction = {1, nullptr}; done = true; }
                        else if (pipeAddHit.contains(m)) {
                            pipeUid = pl::Hub::get().add("", "", &font)->uid;
                            pipeShown = true;
                            done = true;
                        }
                        for (const PipeTabHit& h : pipeTabHits) {
                            if (done) break;
                            if (h.close.contains(m)) {
                                // the neighbour is shown when the shown one closes
                                if (pipeShown && pipeUid == h.uid) {
                                    int next = 0;
                                    for (size_t k = 0; k < pipeTabHits.size(); k++)
                                        if (pipeTabHits[k].uid == h.uid) next = k + 1 < pipeTabHits.size() ? pipeTabHits[k + 1].uid : k > 0 ? pipeTabHits[k - 1].uid : 0;
                                    pipeUid = next;
                                    pipeShown = next != 0;
                                }
                                pl::Hub::get().close(pl::Hub::get().byUid(h.uid));
                                done = true;
                            } else if (h.tab.contains(m)) {
                                pipeUid = h.uid;
                                pipeShown = true;
                                pipeDragUid = h.uid; pipeDragX = m.x; pipeDragMoved = false;
                                done = true;
                            }
                        }
                        if (!done) for (const RxTabHit& h : rxTabHits) {
                            if (h.speaker.contains(m)) {   // this receiver's sound on / off (works in the background)
                                const bool on = !h.rx->sd.audioOut;
                                h.rx->sd.audioOut = on;
                                if (!on) audio.clear(h.rx->sd.audioStream);
                                break;
                            }
                            if (h.close.size.x > 0 && h.close.contains(m)) { rxAction = {2, h.rx}; break; }
                            if (h.tab.contains(m)) { pipeShown = false; if (h.rx != cur) rxAction = {3, h.rx}; break; }
                        }
                    }
                    continue;
                }
            }

            // A pipeline tab dragged sideways: it moves past its neighbours
            if (pipeDragUid) {
                if (ev->is<sf::Event::MouseMoved>()) {
                    const sf::Vector2f m = window.mapPixelToCoords(sf::Mouse::getPosition(window));
                    if (std::fabs(m.x - pipeDragX) > 6.0f) pipeDragMoved = true;
                    if (pipeDragMoved) {
                        int from = -1, to = -1;
                        for (size_t k = 0; k < pipeTabHits.size(); k++) {
                            if (pipeTabHits[k].uid == pipeDragUid) from = (int)k;
                            const sf::FloatRect& t = pipeTabHits[k].tab;
                            if (m.x >= t.position.x && m.x < t.position.x + t.size.x) to = (int)k;
                        }
                        if (from >= 0 && to >= 0 && from != to) pl::Hub::get().move(from, to);
                    }
                    continue;
                }
                if (ev->is<sf::Event::MouseButtonReleased>()) { pipeDragUid = 0; pipeDragMoved = false; continue; }
            }

            // The pipeline workspace covers everything below the tabs: it gets the mouse there
            // and the keyboard (the top bar above it still works)
            if (pl::Workspace* ws = shownWs()) {
                bool mine = false;
                if (const auto* mb = ev->getIf<sf::Event::MouseButtonPressed>())
                    mine = window.mapPixelToCoords(mb->position).y >= TOP_BAR_H;
                else if (const auto* sc = ev->getIf<sf::Event::MouseWheelScrolled>())
                    mine = window.mapPixelToCoords(sc->position).y >= TOP_BAR_H;
                else if (ev->is<sf::Event::MouseMoved>() || ev->is<sf::Event::MouseButtonReleased>() ||
                         ev->is<sf::Event::KeyPressed>() || ev->is<sf::Event::KeyReleased>() || ev->is<sf::Event::TextEntered>())
                    mine = true;
                if (mine) {
                    ws->editor.handleEvent(*ev, window);
                    // moves / releases also reach the rest (drags of the top bar end properly)
                    if (!ev->is<sf::Event::MouseMoved>() && !ev->is<sf::Event::MouseButtonReleased>()) continue;
                    if (window.mapPixelToCoords(sf::Mouse::getPosition(window)).y >= TOP_BAR_H) continue;
                }
            }

            if (sidebar.handleEvent(*ev, window)) continue;

            // Decoder tabs: click = show its panel, x = switch the decoder off
            if (const auto* mbt = ev->getIf<sf::Event::MouseButtonPressed>()) {
                const sf::Vector2f m = window.mapPixelToCoords(sf::Mouse::getPosition(window));
                if (!tabs.empty() && m.y >= tabTop && m.y < tabTop + TAB_H && m.x < layout.specW) {
                    if (mbt->button == sf::Mouse::Button::Left) {
                        for (const TabHit& th : tabHits) {
                            if (th.close.contains(m)) {
                                DecoderEntry& d = decoders[th.entry];
                                d.chk->checked = false;
                                d.chk->onToggle(false);
                                break;
                            }
                            if (th.tab.contains(m)) { cur->selectedTab = th.name; break; }
                        }
                    }
                    continue;
                }
            }

            // The corner zoom slider sits on top of the status bar / decoder panel
            if (slZoomBar.handleEvent(*ev, window)) continue;

            // Decoder panel at the bottom of the waterfall (SSTV / WEFAX controls, picture scrolling)
            if (panelMod && panelMod->enabled && panelMod->handleEvent(*ev, window)) continue;

            // Decoder channel handles on the tuning bar: take the press before the scale drag
            if (const auto* mbp = ev->getIf<sf::Event::MouseButtonPressed>()) {
                if (mbp->button == sf::Mouse::Button::Left) {
                    const sf::Vector2f m = window.mapPixelToCoords(sf::Mouse::getPosition(window));
                    const int e = edgeAt(m);
                    if (e >= 0) {
                        chEdge = ChannelEdgeDrag();
                        chEdge.set = chHandles[e].set;
                        chEdge.id = chHandles[e].ch.id;
                        chEdge.x = m.x;
                        chEdge.widthHz = chHandles[e].ch.widthHz;
                        continue;
                    }
                    const int h = handleAt(m);
                    if (h >= 0) {
                        chDrag = ChannelDrag();
                        chDrag.set = chHandles[h].set;
                        chDrag.id = chHandles[h].ch.id;
                        chDrag.startX = chDrag.x = m.x;
                        continue;
                    }
                }
            }
            if (chDrag.set) {
                if (const auto* mmv = ev->getIf<sf::Event::MouseMoved>()) {
                    updateChannelDrag(window.mapPixelToCoords(mmv->position).x);
                    continue;
                }
            }
            if (chEdge.set) {
                if (const auto* mmv = ev->getIf<sf::Event::MouseMoved>()) {
                    updateEdgeDrag(window.mapPixelToCoords(mmv->position).x);
                    continue;
                }
            }

            bool topBarHandled = false;
            if (volSlider.handleEvent(*ev, window)) topBarHandled = true;
            if (slZoom->handleEvent(*ev, window)) topBarHandled = true;
            if (cur->sourceType == 0 && timeSlider.handleEvent(*ev, window)) topBarHandled = true;

            if (btnMute.isClicked(*ev, window)) {
                appMuted = !appMuted;
                btnMute.setActive(appMuted);
                audio.masterMute = appMuted;   // the whole speaker (every receiver)
                topBarHandled = true;
            }

            if (btnPlay.isClicked(*ev, window)) {
                bool s;
                {
                    std::lock_guard<std::mutex> l(cur->sd.mtx);
                    cur->sd.isPlaying = !cur->sd.isPlaying;
                    s = cur->sd.isPlaying;
                }
                if (s) {
                    btnPlay.setText("||"); btnPlay.setActive(true);
                    { std::lock_guard<std::mutex> l(cur->sd.sourceMtx); if (cur->sd.source) cur->sd.source->start(); }
                } else {
                    btnPlay.setText(">"); btnPlay.setActive(false);
                    audio.clear(cur->sd.audioStream);
                    { std::lock_guard<std::mutex> l(cur->sd.sourceMtx); if (cur->sd.source) cur->sd.source->stop(); }
                }
                topBarHandled = true;
            }

            if (btnMap.isClicked(*ev, window)) {
                mapWindow.openOrFocus();
                topBarHandled = true;
            }

            if (isHw && btnTuningMode.isClicked(*ev, window)) {
                cur->stickyCenter = !cur->stickyCenter;
                if(cur->stickyCenter) {
                    btnTuningMode.setText("CTR"); btnTuningMode.setActive(true);
                    { std::lock_guard<std::mutex> l(cur->sd.mtx); cur->sd.tunedFreqPercent = 0.5; }
                    cur->pendingCenterFreq = freqVFO.getFrequency();
                    cur->debouncer.restart();
                } else {
                    btnTuningMode.setText("FIX"); btnTuningMode.setActive(false);
                }
                topBarHandled = true;
            }

            if (freqVFO.handleEvent(*ev)) {
                topBarHandled = true;
                long long targetVFO = freqVFO.getFrequency();
                double halfBW = hwSampleRate / 2.0;
                double minF = (double)cur->centerFreq - halfBW;
                double maxF = (double)cur->centerFreq + halfBW;

                if (isHw && (cur->stickyCenter || targetVFO < minF || targetVFO > maxF)) {
                    cur->centerFreq = targetVFO;
                    {
                        std::lock_guard<std::mutex> l(cur->sd.mtx);
                        cur->sd.centerFreq = targetVFO;
                        cur->sd.tunedFreqPercent = 0.5;
                        cur->sd.viewCenterPct = 0.5;
                    }
                    cur->pendingCenterFreq = targetVFO;
                    cur->debouncer.restart();
                } else {
                    if (!isHw) {
                        if (targetVFO > maxF) targetVFO = (long long)maxF;
                        if (targetVFO < minF) targetVFO = (long long)minF;
                    }
                    double newOffset = (double)(targetVFO - cur->centerFreq);
                    {
                        std::lock_guard<std::mutex> l(cur->sd.mtx);
                        cur->sd.tunedFreqPercent = 0.5 + (newOffset / hwSampleRate);
                    }
                }
                if (targetVFO != freqVFO.getFrequency()) freqVFO.setFrequency(targetVFO);
            }

            if (topBarHandled) continue;

            const float apY = window.mapPixelToCoords(sf::Mouse::getPosition(window)).y;
            bool overAprs = (aprsOn && apY > aprsTopY && apY < aprsTopY + 200.0f);

            if (const auto* scroll = ev->getIf<sf::Event::MouseWheelScrolled>()) {
                if (overAprs) {
                    aprsLogScrollOffset += scroll->delta * 20.0f;
                    if (aprsLogScrollOffset > 0.0f) aprsLogScrollOffset = 0.0f;
                } else {
                    sf::Vector2f m = window.mapPixelToCoords(sf::Vector2i((int)scroll->position.x, (int)scroll->position.y));
                    if (m.y > TOP_BAR_H && m.y < bottomUiTop && m.x < layout.specW) {
                        long long step = STEP_VALUES[ddSnap->selectedIndex];
                        if (step == 0) step = 100;

                        long long current = freqVFO.getFrequency();
                        long long rawNext = current + (long long)(scroll->delta * step);

                        if (step > 0) {
                            long long remainder = rawNext % step;
                            if (remainder > step / 2) rawNext += (step - remainder);
                            else rawNext -= remainder;
                        }
                        if (rawNext < 0) rawNext = 0;

                        freqVFO.setFrequency(rawNext);

                        if (cur->stickyCenter && isHw) {
                            cur->centerFreq = rawNext;
                            cur->pendingCenterFreq = rawNext;
                            cur->debouncer.restart();
                            {
                                std::lock_guard<std::mutex> l(cur->sd.mtx);
                                cur->sd.centerFreq = rawNext;
                                cur->sd.tunedFreqPercent = 0.5;
                                cur->sd.viewCenterPct = 0.5;
                            }
                        } else {
                            double newOffset = (double)(rawNext - cur->centerFreq);
                            double clickPct = 0.5 + (newOffset / hwSampleRate);
                            clickPct = std::clamp(clickPct, 0.0, 1.0);
                            {
                                std::lock_guard<std::mutex> l(cur->sd.mtx);
                                cur->sd.tunedFreqPercent = clickPct;
                            }
                        }
                    }
                }
            }

            if (const auto* mb = ev->getIf<sf::Event::MouseButtonPressed>()) {
                sf::Vector2f m = window.mapPixelToCoords(sf::Mouse::getPosition(window));

                if (mb->button == sf::Mouse::Button::Right) {
                    bool inFreqScaleZone = (m.y > TOP_BAR_H && m.y < (TOP_BAR_H + layout.specH + layout.waterfallH) && m.x < layout.specW);
                    if (inFreqScaleZone && !overAprs) {
                        isPanningView = true;
                        lastDragX = m.x;
                    }
                }
                else if (mb->button == sf::Mouse::Button::Left) {
                    if (overAprs) {
                        float logX = layout.specW - 300;
                        float overlayY = aprsTopY;
                        float listTopY = overlayY + 25.0f;
                        if (m.x > logX) {
                            float relY = m.y - listTopY - aprsLogScrollOffset;
                            int index = (int)(relY / 16.0f);
                            std::lock_guard<std::mutex> l(cur->sd.mtx);
                            if (index >= 0 && index < cur->sd.aprsHistory.size()) {
                                selectedAprsPacket = cur->sd.aprsHistory[index];
                                showAprsModal = true;
                            }
                        }
                    }

                    bool overTimeline = (cur->sourceType == 0) && (timeSlider.isDragging || (m.y > layout.winH - TIMELINE_H));
                    if (m.y >= bottomUiTop) overTimeline = true;   // status bar / decoder panel: no tuning
                    bool inFreqScaleZone = (m.y >= TOP_BAR_H + layout.specH && m.y <= TOP_BAR_H + layout.specH + layout.tuneBarH && m.x < layout.specW);

                    float leftEdge = visualCenterX - bwPixels / 2.0f;
                    float rightEdge = visualCenterX + bwPixels / 2.0f;
                    if (mode == Mode::USB) { leftEdge = visualCenterX; rightEdge = visualCenterX + bwPixels; }
                    if (mode == Mode::LSB) { leftEdge = visualCenterX - bwPixels; rightEdge = visualCenterX; }
                    bool hitEdge = (std::abs(m.x - leftEdge) < 6 || std::abs(m.x - rightEdge) < 6) && (m.y > TOP_BAR_H && m.y < TOP_BAR_H + layout.specH);

                    if (hitEdge && !overAprs) {
                        isDraggingBW = true;
                        dragStartBW = slBW->currentVal;
                        dragStartMouseX = m.x;
                    }
                    else if (inFreqScaleZone && !overTimeline && !overAprs) {
                        isDraggingScale = true;
                        lastDragX = m.x;
                    }
                    else if (!overTimeline && !overAprs && m.x < layout.specW && m.y > TOP_BAR_H && m.y < (TOP_BAR_H + layout.specH + layout.waterfallH)) {
                        isSpectrumDragging = true;
                        applySpectrumTuning(m.x);
                    }
                }
            }
            else if (const auto* mm = ev->getIf<sf::Event::MouseMoved>()) {
                sf::Vector2f m = window.mapPixelToCoords(mm->position);

                float leftEdge = visualCenterX - bwPixels / 2.0f;
                float rightEdge = visualCenterX + bwPixels / 2.0f;
                if (mode == Mode::USB) { leftEdge = visualCenterX; rightEdge = visualCenterX + bwPixels; }
                if (mode == Mode::LSB) { leftEdge = visualCenterX - bwPixels; rightEdge = visualCenterX; }
                bool hitEdge = (std::abs(m.x - leftEdge) < 6 || std::abs(m.x - rightEdge) < 6) && (m.y > TOP_BAR_H && m.y < TOP_BAR_H + layout.specH);

                if ((hitEdge || isDraggingBW) && cursorSizeH) window.setMouseCursor(*cursorSizeH);

                if (isPanningView) {
                    float dx = lastDragX - m.x;
                    lastDragX = m.x;
                    double pctShift = (dx / layout.specW) * viewWidthPct;
                    std::lock_guard<std::mutex> l(cur->sd.mtx);
                    cur->sd.viewCenterPct += pctShift;
                    if (cur->sd.viewCenterPct - visibleFraction/2.0 < 0.0) cur->sd.viewCenterPct = visibleFraction/2.0;
                    if (cur->sd.viewCenterPct + visibleFraction/2.0 > 1.0) cur->sd.viewCenterPct = 1.0 - visibleFraction/2.0;
                }
                else if (isDraggingBW) {
                    float hzPerPx = (hwSampleRate * viewWidthPct) / layout.specW;
                    float dist = std::abs(m.x - visualCenterX);
                    float newBW = dist * hzPerPx * 2.0f;
                    if (mode == Mode::USB || mode == Mode::LSB) newBW = dist * hzPerPx;
                    newBW = std::clamp(newBW, slBW->minVal, slBW->maxVal);
                    slBW->setValueSilent(newBW);
                    slBW->onChange(newBW);
                }
                else if (isSpectrumDragging) applySpectrumTuning(m.x);

                if (isDraggingScale) {
                    static double dragAccumulator = 0.0;
                    float dx = lastDragX - m.x;
                    lastDragX = m.x;

                    double hzPerPx = (hwSampleRate * viewWidthPct) / layout.specW;
                    double exactShift = dx * hzPerPx + dragAccumulator;
                    long long shift = (long long)exactShift;
                    dragAccumulator = exactShift - shift;

                    if (isHw) {
                        long long nextCenter = cur->centerFreq + shift;
                        if (nextCenter < 0) nextCenter = 0;
                        cur->centerFreq = nextCenter;
                        {
                            std::lock_guard<std::mutex> l(cur->sd.mtx);
                            cur->sd.centerFreq = nextCenter;
                        }
                        cur->pendingCenterFreq = nextCenter;
                    } else {
                        double pctShift = (dx / layout.specW) * viewWidthPct;
                        std::lock_guard<std::mutex> l(cur->sd.mtx);
                        cur->sd.viewCenterPct = std::clamp(cur->sd.viewCenterPct + pctShift, visibleFraction/2.0, 1.0 - visibleFraction/2.0);
                    }
                }

                const bool overDecoderPanel = (m.y >= bottomUiTop);
                if (!overAprs && !overDecoderPanel && m.x >= 0 && m.x < layout.specW && m.y >= TOP_BAR_H && m.y < (TOP_BAR_H + layout.specH + layout.waterfallH)) {
                    std::lock_guard<std::mutex> l(cur->sd.mtx);
                    cur->sd.mouseX_spectrum = m.x;
                    cur->sd.mouseY_spectrum = m.y - TOP_BAR_H;
                } else {
                    std::lock_guard<std::mutex> l(cur->sd.mtx);
                    cur->sd.mouseX_spectrum = -1.0f;
                }
            }
        } // end of pollEvent loop

        // --- Scanner and bookmark tuning ---
        {
            float snr = 0.0f;
            unsigned long long seq = 0;
            { std::lock_guard<std::mutex> l(cur->sd.mtx); snr = cur->sd.tunerSnrDb; seq = cur->sd.tunerLevelSeq; }
            const bool fresh = seq != lastLevelSeq;
            lastLevelSeq = seq;
            // tuning by hand stops the scanner
            if (scanner.running() && pendingBookmarkTune < 0 && freqVFO.getFrequency() != scanner.tunedHz) { scanner.stop(); setScanUi(); }
            const bool wasRunning = scanner.running();
            // digital voice bookmarks: held while speech is decoded, not on a bare carrier
            int voice = -1;
            if (scanner.running() && scanner.current >= 0 && scanner.current < (int)bookmarks.items.size() &&
                isDigitalMode(bookmarks.items[scanner.current].mode)) {
                // (encrypted calls that are muted are skipped too)
                const dv::Status ds = cur->sd.digital->status();
                voice = 0;
                for (int sl = 0; sl < ds.nSlots && sl < 2; sl++)
                    if (ds.slot[sl].voice && !(ds.slot[sl].encrypted && chkMuteEnc->checked)) voice = 1;
            }
            scanner.update(bookmarks.items, scanClock.getElapsedTime().asSeconds(), snr, fresh, voice);
            if (wasRunning != scanner.running()) setScanUi();
            if (pendingBookmarkTune >= 0 && pendingBookmarkTune < (int)bookmarks.items.size()) {
                const Bookmark b = bookmarks.items[pendingBookmarkTune];
                tuneVfoTo(b.freqHz);
                if (!cur->inputIsAudio) {
                    restoreMode(b.mode);
                    if (b.mode != Mode::OFF) { slBW->setValueSilent(b.bandwidthHz); slBW->onChange(slBW->currentVal); }
                }
            }
            pendingBookmarkTune = -1;
            const bool mute = scanner.running() && scanner.muteWanted();
            { std::lock_guard<std::mutex> l(cur->sd.mtx); cur->sd.scanMute = mute; }
            bookList->active = scanner.running() ? scanner.current : -1;
            if (scanner.running()) bookList->ensureVisible(scanner.current);
            const std::string st = scanner.status(bookmarks.items);
            static std::string lastSt;
            if (st != lastSt) { lblScan->setText(st); lastSt = st; }
        }

        // --- Digital voice status under the demodulator buttons (5 times a second) ---
        if (grpDigital->shown && digitalUiClock.getElapsedTime().asMilliseconds() > 200) {
            digitalUiClock.restart();
            const dv::Status ds = cur->sd.digital->status();
            Mode dm;
            { std::lock_guard<std::mutex> l(cur->sd.mtx); dm = cur->sd.mode; }
            lblDigital->setText(digitalStatusText(ds, dm));
            bool enc = false, voice = false;
            for (int sl = 0; sl < ds.nSlots && sl < 2; sl++) { enc = enc || (ds.slot[sl].voice && ds.slot[sl].encrypted); voice = voice || ds.slot[sl].voice; }
            lblDigital->setColor(enc ? sf::Color(255, 140, 110) : voice ? sf::Color(140, 230, 150) : ds.sync ? sf::Color(200, 200, 210) : sf::Color(140, 140, 150));
        }

        // --- Satellite map window and frequency / Doppler tracking ---
        satCatalog.poll();
        mapWindow.frame();
        // decoder positions are stamped with the map's time (the recording's time for old files)
        positionBus().setClock(orbit::jdToUnix(mapWindow.trackerJd()));
        // ADS-B: the receiver for distances / surface positions, and the "Tune to 1090 MHz" button
        for (auto& rxp : receivers) rxp->adsb->setReceiver(satTracker.obs.latDeg, satTracker.obs.lonDeg);
        if (const long long want = cur->adsb->tuneRequest.exchange(0); want > 0) {
            if (isHw) {   // only the frequency; the sample rate stays the user's choice
                cur->centerFreq = want;
                {
                    std::lock_guard<std::mutex> l(cur->sd.mtx);
                    cur->sd.centerFreq = want;
                    cur->sd.tunedFreqPercent = 0.5;
                    cur->sd.viewCenterPct = 0.5;
                }
                cur->pendingCenterFreq = want;
                cur->debouncer.restart();
                freqVFO.setFrequency(want);
            }
        }
        {
            // The map's choices: every receiver (tab) and its running decoders
            auto rxDecTabs = [&](Receiver* r) { return r == cur ? tabs : collectTabs(r); };
            satTracker.receivers.clear();
            for (auto& rxp : receivers) {
                orbit::SatTracker::RxChoice c;
                c.id = rxp->id;
                c.name = rxp->title();
                for (const DecTab& t : rxDecTabs(rxp.get())) c.decoders.push_back(t.name);
                satTracker.receivers.push_back(std::move(c));
            }
            // Tracking runs on the chosen receiver, also while another tab is shown
            Receiver* R = nullptr;
            for (auto& rxp : receivers) if (rxp->id == satTracker.rxId) R = rxp.get();

            // The decoder whose channel follows the satellite: the one chosen in the map;
            // automatic: the one the transmitter needs (e.g. SSTV) when it runs, otherwise
            // the decoder in the receiver's selected tab
            std::string decName;
            ChannelSet* trackSet = nullptr;
            if (R) {
                const std::vector<DecTab> rt = rxDecTabs(R);
                const std::string hint = orbit::demodHintFor(satTracker.tx).decoder;
                const DecTab* t = nullptr;
                if (!satTracker.decoderName.empty()) {
                    for (const DecTab& x : rt) if (x.name == satTracker.decoderName) t = &x;
                } else {
                    for (const DecTab& x : rt) if (!hint.empty() && x.name == hint) t = &x;
                    if (!t) for (const DecTab& x : rt) if (x.name == R->selectedTab) t = &x;
                    if (!t && !rt.empty()) t = &rt.back();
                }
                if (t) { decName = t->name; trackSet = t->set; }
                if (R->inputIsAudio) trackSet = nullptr;
            }
            satTracker.activeDecoderName = decName;

            // real time, or the recording's / manual time set in the map's Time module
            const bool trackWants = satTracker.update(satCatalog, mapWindow.trackerJd());
            if (trackWants && !R)
                satTracker.applyStatus = "RX" + std::to_string(satTracker.rxId) + " is closed: choose a receiver";
            if (trackWants && R) {
                satTracker.applyStatus.clear();
                const bool shown = (R == cur);
                double rate = 2e6;
                bool rHw = false;
                {
                    std::lock_guard<std::mutex> l(R->sd.sourceMtx);
                    if (R->sd.source) { rate = R->sd.source->getSampleRate(); rHw = R->sd.source->isHardware(); }
                }
                if (rate <= 0) rate = 2e6;
                // Retunes the radio so `hz` lies in the received band, away from the DC spike;
                // the tuner (VFO) goes onto `hz` as well
                auto recentreOn = [&](long long hz) {
                    const long long c = hz + (long long)std::llround(rate * 0.25);
                    R->centerFreq = c;
                    {
                        std::lock_guard<std::mutex> l(R->sd.mtx);
                        R->sd.centerFreq = c;
                        R->sd.tunedFreqPercent = 0.5 + (double)(hz - c) / rate;
                        R->sd.viewCenterPct = 0.5;
                    }
                    R->pendingCenterFreq = c;
                    R->debouncer.restart();
                    if (shown) freqVFO.setFrequency(hz);
                };
                auto inBand = [&](long long hz, double width) {
                    return std::abs((double)(hz - R->centerFreq)) + width / 2.0 < rate * 0.45;
                };

                if (satTracker.target == orbit::TrackTarget::Tuner) {
                    // demodulator mode of the transmitter, once per tracking start
                    if (satTracker.setDemodMode && !satTracker.demodApplied && !R->inputIsAudio) {
                        const orbit::DemodHint h = orbit::demodHintFor(satTracker.tx);
                        if (shown) {
                            restoreMode(h.mode);
                            slBW->setValueSilent((float)h.bandwidthHz);
                            slBW->onChange(slBW->currentVal);
                        } else {   // the widgets show another receiver: set its SharedData (shown when its tab opens)
                            std::lock_guard<std::mutex> l(R->sd.mtx);
                            R->sd.mode = h.mode;
                            R->sd.bandwidth = h.bandwidthHz;
                        }
                    }
                    satTracker.demodApplied = true;
                    double pct;
                    { std::lock_guard<std::mutex> l(R->sd.mtx); pct = R->sd.tunedFreqPercent; }
                    const long long curHz = R->centerFreq + (long long)std::llround((pct - 0.5) * rate);
                    satTracker.noteRadioFrequency(curHz);
                    const long long want = satTracker.wantHz;
                    double bwNow;
                    { std::lock_guard<std::mutex> l(R->sd.mtx); bwNow = R->sd.bandwidth; }
                    if (std::llabs(want - curHz) >= 5 && trackClock.getElapsedTime().asMilliseconds() >= 100) {
                        if (rHw && !inBand(want, bwNow)) recentreOn(want);
                        else if (!rHw && !inBand(want, 0.0)) satTracker.applyStatus = "Outside the file's band";
                        else {
                            std::lock_guard<std::mutex> l(R->sd.mtx);
                            R->sd.tunedFreqPercent = 0.5 + (double)(want - R->centerFreq) / rate;
                            if (shown) freqVFO.setFrequency(want);
                        }
                        satTracker.lastCommandHz = want;
                        trackClock.restart();
                    } else if (satTracker.lastCommandHz == 0) satTracker.lastCommandHz = curHz;
                } else {
                    if (!trackSet) {
                        if (R->inputIsAudio) satTracker.applyStatus = "Audio input: decoder channels are not used";
                        else if (!satTracker.decoderName.empty()) satTracker.applyStatus = "Switch on " + satTracker.decoderName + " on RX" + std::to_string(R->id);
                        else satTracker.applyStatus = "Switch on a decoder (e.g. SSTV) on RX" + std::to_string(R->id);
                    } else {
                        const auto chans = trackSet->snapshot();
                        if (chans.empty()) satTracker.applyStatus = "Waiting for the decoder's channel...";
                        else {
                            const DecoderChannel& ch = chans.front();
                            if (!ch.followVfo) satTracker.noteRadioFrequency(ch.freqHz);
                            const long long want = satTracker.wantHz;
                            if ((ch.followVfo || std::llabs(want - ch.freqHz) >= 5) && trackClock.getElapsedTime().asMilliseconds() >= 100) {
                                trackSet->moveTo(ch.id, want);
                                if (rHw && !inBand(want, ch.widthHz)) recentreOn(want);
                                satTracker.lastCommandHz = want;
                                trackClock.restart();
                            }
                        }
                    }
                }
            }
        }

        for (auto& rxp : receivers) {
            Receiver& r = *rxp;
            if (r.pendingCenterFreq != 0 && r.debouncer.getElapsedTime().asMilliseconds() > TUNING_LATENCY_MS) {
                {
                    std::lock_guard<std::mutex> l(r.sd.sourceMtx);
                    if (r.sd.source && r.sd.source->isHardware()) {
                        r.sd.source->setCenterFrequency(r.pendingCenterFreq);
                        r.centerFreq = r.pendingCenterFreq;
                    }
                }
                {
                    std::lock_guard<std::mutex> l(r.sd.mtx);
                    r.sd.centerFreq = r.pendingCenterFreq;
                }
                r.pendingCenterFreq = 0;
                r.debouncer.restart();
            }
        }

        sidebar.update(window);

        bool showHand = false;
        sf::Vector2f mousePos = window.mapPixelToCoords(sf::Mouse::getPosition(window));

        if (sidebar.isMouseOver(window)) {
            if (sidebar.isAnyWidgetHovered(window)) showHand = true;
            if (slZoomBar.isMouseOver(window) || slZoomBar.isDragging) showHand = true;
        } else {
            if (freqVFO.isHovered) showHand = true;
            if (mousePos.y >= TOP_BAR_H + layout.specH - 10 && mousePos.y <= TOP_BAR_H + layout.specH + 20 && mousePos.x < layout.specW) showHand = true;
            if (volSlider.isMouseOver(window) || (cur->sourceType == 0 && timeSlider.isMouseOver(window))) showHand = true;
            if (btnPlay.isMouseOver(window) || btnMute.isMouseOver(window) || btnTuningMode.isMouseOver(window) || btnMap.isMouseOver(window)) showHand = true;
            if (aprsOn && mousePos.y > aprsTopY && mousePos.y < aprsTopY + 200.0f && mousePos.x < layout.specW) showHand = true;
            if (!tabs.empty() && mousePos.y >= tabTop && mousePos.y < tabTop + TAB_H && mousePos.x < layout.specW) showHand = true;
        }
        if (handleAt(mousePos) >= 0) showHand = true;
        if (showAprsModal) showHand = false;

        const bool overChEdge = chEdge.set || (!chDrag.set && !isDraggingBW && !showAprsModal && edgeAt(mousePos) >= 0);
        if ((isDraggingBW || (chDrag.set && chDrag.moved) || overChEdge) && cursorSizeH) { window.setMouseCursor(*cursorSizeH); }
        else if (showHand && cursorHand) { window.setMouseCursor(*cursorHand); }
        else if (cursorArrow) { window.setMouseCursor(*cursorArrow); }

        std::vector<uint8_t> newRow;
        bool hasData = false;
        {
            std::lock_guard<std::mutex> l(cur->sd.mtx);
            if (cur->sd.newInspectorData) {
                newRow = cur->sd.inspectorWaterfallRow;
                cur->sd.newInspectorData = false;
                hasData = true;
            }
        }
        if (hasData) {
            if (newRow.size() == MINI_W * 4) {
                if (MINI_H > 1) {
                    std::memmove(miniWaterfall.data() + MINI_W * 4, miniWaterfall.data(), (MINI_H - 1) * MINI_W * 4);
                }
                std::memcpy(miniWaterfall.data(), newRow.data(), MINI_W * 4);
                miniTex.update(miniWaterfall.data());
            } else {
                std::cerr << "[GPU Error] Inspector size mismatch!\n";
            }
        }

        std::vector<double> spectrum;
        std::vector<uint8_t> row;
        bool newRow2 = false;
        {
            std::lock_guard<std::mutex> lock(cur->sd.mtx);
            spectrum = cur->sd.fftSpectrum;
            if (cur->sd.newWaterfallData) {
                row = cur->sd.waterfallRow;
                cur->sd.newWaterfallData = false;
                newRow2 = true;
            }
        }

        if (newRow2) {
            currentWaterfallY = (currentWaterfallY - 1 + WATERFALL_HEIGHT_PX) % WATERFALL_HEIGHT_PX;
            wTex.update(row.data(), sf::Vector2u(INTERNAL_WATERFALL_WIDTH, 1), sf::Vector2u(0, (unsigned int)currentWaterfallY));
        }

        window.clear(sf::Color::Black);
        pl::Workspace* const wsShown = shownWs();
        if (pipeShown && !wsShown) pipeShown = false;   // (closed meanwhile)

        if (!wsShown) {   // (the receiver's view: not drawn at all while a pipeline tab covers it)
        long long cf = 0;
        if (cur->sd.source) cf = cur->centerFreq;

        // --- DEDICATED TUNING BAR ---
        sf::RectangleShape tuneBarBg({layout.specW, layout.tuneBarH});
        tuneBarBg.setPosition({0, (float)(TOP_BAR_H + layout.specH)});
        tuneBarBg.setFillColor(sf::Color(10, 10, 12));
        window.draw(tuneBarBg);

        drawGrid(window, font, 0, TOP_BAR_H, layout.specW, layout.specH, cf, hwSampleRate, viewStartPct, viewEndPct, slMinDb->currentVal, slMaxDb->currentVal);

        // --- BAND PLAN ---
        double startFreq = (double)cf + (viewStartPct - 0.5) * hwSampleRate;
        double endFreq = (double)cf + (viewEndPct - 0.5) * hwSampleRate;
        double visibleSpan = endFreq - startFreq;

        for (const auto& band : globalBandPlan) {
            if (band.endFreq > startFreq && band.startFreq < endFreq) {
                float startX = layout.specW * ((band.startFreq - startFreq) / visibleSpan);
                float endX = layout.specW * ((band.endFreq - startFreq) / visibleSpan);

                if (startX < 0) startX = 0;
                if (endX > layout.specW) endX = layout.specW;

                float widthX = endX - startX;

                if (widthX > 1.0f) {
                    sf::VertexArray gradient(sf::PrimitiveType::TriangleStrip, 4);
                    sf::Color topColor = band.color;
                    topColor.a = 35;
                    sf::Color bottomColor = band.color;
                    bottomColor.a = 0;

                    float topY = TOP_BAR_H;
                    float bottomY = TOP_BAR_H + layout.specH;

                    gradient[0] = sf::Vertex{sf::Vector2f(startX, topY), topColor};
                    gradient[1] = sf::Vertex{sf::Vector2f(endX, topY), topColor};
                    gradient[2] = sf::Vertex{sf::Vector2f(startX, bottomY), bottomColor};
                    gradient[3] = sf::Vertex{sf::Vector2f(endX, bottomY), bottomColor};
                    window.draw(gradient);

                    sf::RectangleShape bar({widthX, 4.0f});
                    bar.setPosition({startX, (float)TOP_BAR_H});
                    bar.setFillColor(band.color);
                    window.draw(bar);

                    if (widthX > 40.0f) {
                        sf::Text bText(font, band.name, 28);
                        bText.setScale({0.5f, 0.5f});
                        bText.setPosition({startX + 4, (float)TOP_BAR_H + 5});
                        bText.setFillColor(sf::Color(255, 255, 255, 200));
                        window.draw(bText);
                    }
                }
            }
        }

        std::vector<double> smoothedSpectrum = spectrum;
        for (size_t i = 1; i < spectrum.size() - 1; i++) {
            smoothedSpectrum[i] = (spectrum[i-1] + spectrum[i] * 2.0 + spectrum[i+1]) / 4.0;
        }

        sf::VertexArray fillArea(sf::PrimitiveType::TriangleStrip);
        sf::VertexArray outline(sf::PrimitiveType::LineStrip);

        sf::Color coreColor = Theme::Accent;
        sf::Color glowColor = Theme::Glow;
        sf::Color bottomColor = Theme::AccentDim;
        bottomColor.a = 20;

        int numPoints = spectrum.size();
        fillArea.resize(numPoints * 2);
        outline.resize(numPoints);

        for (int i = 0; i < numPoints; ++i) {
            float x = ((float)i / (numPoints - 1)) * layout.specW;
            float norm = (smoothedSpectrum[i] - slMinDb->currentVal) / (slMaxDb->currentVal - slMinDb->currentVal);
            float y = layout.specH - (norm * layout.specH);
            if (y < 0) y = 0; if (y > layout.specH) y = layout.specH;

            float topY = y + TOP_BAR_H;
            float bottomY = layout.specH + TOP_BAR_H;

            if (currentTheme == 4) {
                sf::Color dynColor = getHeatmap(norm, 4);
                coreColor = sf::Color(100, 200, 255, 255);
                glowColor = dynColor;
                glowColor.a = 150;
                bottomColor = dynColor;
                bottomColor.a = 20;
            }

            fillArea[2 * i]     = sf::Vertex{sf::Vector2f(x, topY), glowColor};
            fillArea[2 * i + 1] = sf::Vertex{sf::Vector2f(x, bottomY), bottomColor};
            outline[i]          = sf::Vertex{sf::Vector2f(x, topY), coreColor};
        }

        sf::RenderStates states;
        states.blendMode = sf::BlendAdd;
        window.draw(fillArea, states);
        window.draw(outline);

        wSpr.setTextureRect(sf::IntRect({0, currentWaterfallY}, {INTERNAL_WATERFALL_WIDTH, (int)layout.waterfallH}));
        float scaleX = layout.specW / (float)INTERNAL_WATERFALL_WIDTH;
        wSpr.setScale({scaleX, 1.0f});
        window.draw(wSpr);

        float mx = -1.0f;
        float my = -1.0f;
        {
            std::lock_guard<std::mutex> l(cur->sd.mtx);
            mx = cur->sd.mouseX_spectrum;
            my = cur->sd.mouseY_spectrum;
        }

        if (mx != -1.0f) {
            sf::Color guideColor(100, 100, 100);

            sf::VertexArray lineFFT(sf::PrimitiveType::Lines, 2);
            lineFFT[0].position = sf::Vector2f(mx, (float)TOP_BAR_H);
            lineFFT[0].color = guideColor;
            lineFFT[1].position = sf::Vector2f(mx, (float)layout.specH + TOP_BAR_H);
            lineFFT[1].color = guideColor;
            window.draw(lineFFT);

            // --- TOOLTIP ONLY IN THE LOWER PART OF THE FFT ---
            if (my > layout.specH - 50.0f && my <= layout.specH) {
                double hoverFreq = startFreq + (mx / layout.specW) * visibleSpan;
                int binIdx = std::clamp((int)((mx / layout.specW) * spectrum.size()), 0, (int)spectrum.size() - 1);
                float hoverPower = spectrum.empty() ? -127.0f : spectrum[binIdx];

                std::string freqFormat = freqVFO.formatWithDots((long long)hoverFreq) + " Hz";

                std::stringstream pwrStream;
                pwrStream << std::fixed << std::setprecision(2) << hoverPower << " dBm";

                std::string tooltipStr = freqFormat + "\nPower: " + pwrStream.str();

                sf::Text tooltipText(font, tooltipStr, 24);
                tooltipText.setScale({0.5f, 0.5f});
                tooltipText.setFillColor(sf::Color::White);

                sf::FloatRect textBounds = tooltipText.getGlobalBounds();
                sf::RectangleShape tooltipBg({textBounds.size.x + 16, textBounds.size.y + 12});
                tooltipBg.setFillColor(sf::Color(20, 20, 25, 230));
                tooltipBg.setOutlineColor(Theme::Accent);
                tooltipBg.setOutlineThickness(1.0f);

                float ttX = mx + 15.0f;
                float ttY = my + TOP_BAR_H + 15.0f;

                if (ttX + tooltipBg.getSize().x > layout.specW) {
                    ttX = mx - tooltipBg.getSize().x - 15.0f;
                }

                tooltipBg.setPosition({ttX, ttY});
                tooltipText.setPosition({ttX + 8, ttY + 4});

                window.draw(tooltipBg);
                window.draw(tooltipText);
            }

            if (my > (float)layout.specH) {
                sf::VertexArray lineWaterfall(sf::PrimitiveType::Lines, 2);
                lineWaterfall[0].position = sf::Vector2f(mx, (float)layout.specH + layout.tuneBarH + TOP_BAR_H);
                lineWaterfall[0].color = guideColor;
                lineWaterfall[1].position = sf::Vector2f(mx, (float)(layout.specH + layout.tuneBarH + layout.waterfallH + TOP_BAR_H));
                lineWaterfall[1].color = guideColor;
                window.draw(lineWaterfall);
            }
        }

        if (tunerPosInView >= 0.0f && tunerPosInView <= 1.0f) {
            sf::RectangleShape tunerRect;
            if (bwPixels < 2.0f) bwPixels = 2.0f;

            float rectX = visualCenterX - bwPixels / 2.0f;
            if (mode == Mode::USB) rectX += bwPixels / 2.0f;
            if (mode == Mode::LSB) rectX -= bwPixels / 2.0f;

            tunerRect.setSize({bwPixels, (float)layout.specH});
            tunerRect.setPosition({rectX, (float)TOP_BAR_H});
            tunerRect.setFillColor(mode == Mode::OFF ? sf::Color(50, 50, 50, 40) : sf::Color(200, 200, 200, 50));
            window.draw(tunerRect);

            sf::VertexArray centerLine(sf::PrimitiveType::Lines, 2);
            centerLine[0].position = sf::Vector2f(visualCenterX, (float)TOP_BAR_H);
            centerLine[0].color = sf::Color::Red;
            centerLine[1].position = sf::Vector2f(visualCenterX, (float)layout.specH + TOP_BAR_H);
            centerLine[1].color = sf::Color::Red;
            window.draw(centerLine);
        }

        // --- Bookmarks of the chosen bank: a thin line on the spectrum, the name near its bottom ---
        if (chkBookMarks->checked && cf > 0 && !cur->inputIsAudio && visibleSpan > 0.0) {
            const sf::Color lineCol(120, 200, 255, 110), textCol(160, 215, 255, 230);
            const float specTop = (float)TOP_BAR_H, specBot = specTop + (float)layout.specH;
            std::vector<float> rowEnd;   // labels in up to 3 rows, so neighbours don't overlap
            for (int i = 0; i < (int)bookmarks.items.size(); i++) {
                if (!bookList->shows(i)) continue;
                const Bookmark& b = bookmarks.items[i];
                if (b.freqHz < startFreq || b.freqHz > endFreq) continue;
                const float bx = std::round(layout.specW * (float)((b.freqHz - startFreq) / visibleSpan));
                sf::VertexArray ln(sf::PrimitiveType::Lines, 2);
                ln[0].position = {bx + 0.5f, specTop + 4.0f}; ln[0].color = lineCol;
                ln[1].position = {bx + 0.5f, specBot};        ln[1].color = lineCol;
                window.draw(ln);
                char fb[32];
                std::snprintf(fb, sizeof(fb), "%.4f", b.freqHz / 1e6);
                const std::string name = b.name.empty() ? std::string(fb) : b.name;
                sf::Text t(font, sf::String::fromUtf8(name.begin(), name.end()), 20);
                t.setScale({0.5f, 0.5f});
                const float tw = t.getGlobalBounds().size.x;
                const float lx = std::clamp(bx + 3.0f, 2.0f, std::max(2.0f, layout.specW - tw - 2.0f));
                int row = -1;
                for (size_t r = 0; r < rowEnd.size(); r++) if (rowEnd[r] <= lx) { row = (int)r; break; }
                if (row < 0) {
                    if (rowEnd.size() < 3) { row = (int)rowEnd.size(); rowEnd.push_back(0.0f); }
                    else continue;   // crowded: the line alone
                }
                rowEnd[row] = lx + tw + 6.0f;
                const float ly = specBot - 14.0f - 12.0f * (float)row;
                sf::RectangleShape tbg({tw + 4.0f, 12.0f});
                tbg.setPosition({lx - 2.0f, ly});
                tbg.setFillColor(sf::Color(10, 14, 20, 170));
                window.draw(tbg);
                t.setFillColor(textCol);
                t.setPosition({std::round(lx), std::round(ly - 1.0f)});
                window.draw(t);
            }
        }

        // --- Decoder channels: yellow bands (spectrum, waterfall) and handles (tuning bar) ---
        if (!chHandles.empty()) {
            const float specTop = (float)TOP_BAR_H;
            const float specBot = specTop + (float)layout.specH;
            const float wfTop = tbBot;
            const float wfBot = wfTop + (float)layout.waterfallH;
            auto withAlpha = [](sf::Color c, uint8_t a) { c.a = a; return c; };
            auto lighter = [](sf::Color c) { return sf::Color((uint8_t)std::min(255, c.r + 60), (uint8_t)std::min(255, c.g + 60), (uint8_t)std::min(255, c.b + 60)); };
            auto vline = [&](float x, float y0, float y1, sf::Color c) {
                sf::VertexArray ln(sf::PrimitiveType::Lines, 2);
                ln[0].position = {x, y0}; ln[0].color = c;
                ln[1].position = {x, y1}; ln[1].color = c;
                window.draw(ln);
            };
            auto label = [&](const std::string& str, float x, float y, sf::Color c, unsigned size = 20) {
                sf::Text t(font, sf::String::fromUtf8(str.begin(), str.end()), size);
                t.setScale({0.5f, 0.5f});
                t.setFillColor(c);
                t.setPosition({std::round(x), std::round(y)});
                window.draw(t);
                return t.getGlobalBounds().size.x;
            };
            auto mhzText = [](long long hz) { char b[32]; std::snprintf(b, sizeof(b), "%.3f", hz / 1e6); return std::string(b); };
            const int hovered = (chDrag.set || chEdge.set) ? -1 : handleAt(mousePos);
            const int hoveredEdge = (chDrag.set || chEdge.set || hovered >= 0) ? -1 : edgeAt(mousePos);

            // channel labels: below the band plan's name, in as many rows as they need not to overlap
            auto chTitle = [&](const ChHandle& h) {
                const DecoderChannel& c = h.ch;
                const bool numericName = !c.name.empty() && c.name[0] >= '0' && c.name[0] <= '9';
                const std::string cname = numericName ? h.dec + " " + c.name : c.name;
                const bool saysTuner = cname.size() >= 5 && cname.compare(cname.size() - 5, 5, "tuner") == 0;
                return cname + (c.followVfo ? (saysTuner ? std::string() : "  (tuner)") : numericName ? std::string() : "  " + mhzText(c.freqHz));
            };
            std::vector<int> labelRow(chHandles.size(), 0);
            {
                std::vector<std::pair<float, size_t>> order;
                for (size_t i = 0; i < chHandles.size(); i++) {
                    if (chHandles[i].edge != 0) continue;
                    const double off = chHandles[i].ch.followVfo ? vfoOffsetHz : (double)(chHandles[i].ch.freqHz - cur->centerFreq);
                    order.push_back({std::max(2.0f, offsetToX(chHandles[i].ch.lowOffset(off)) + 3.0f), i});
                }
                std::sort(order.begin(), order.end());
                std::vector<float> rowEnd;
                for (const auto& [lx, i] : order) {
                    const std::string t = chTitle(chHandles[i]);
                    sf::Text probe(font, sf::String::fromUtf8(t.begin(), t.end()), 20);
                    const float w = probe.getGlobalBounds().size.x * 0.5f + 6.0f;
                    int row = -1;
                    for (size_t r = 0; r < rowEnd.size(); r++) if (rowEnd[r] <= lx) { row = (int)r; break; }
                    if (row < 0) {
                        if (rowEnd.size() < 5) { row = (int)rowEnd.size(); rowEnd.push_back(0.0f); }
                        else row = (int)(std::min_element(rowEnd.begin(), rowEnd.end()) - rowEnd.begin());
                    }
                    rowEnd[row] = lx + w;
                    labelRow[i] = row;
                }
            }

            for (size_t i = 0; i < chHandles.size(); i++) {
                const ChHandle& h = chHandles[i];
                const DecoderChannel& c = h.ch;
                const bool dragging = (chDrag.set == h.set && chDrag.id == c.id) || (chEdge.set == h.set && chEdge.id == c.id);
                const double off = c.followVfo ? vfoOffsetHz : (double)(c.freqHz - cur->centerFreq);
                const sf::Color yellow = h.col;
                const sf::Color edgeCol = withAlpha(h.col, 210), fillCol = withAlpha(h.col, 38), wfEdgeCol = withAlpha(h.col, 110);
                // channels named by their frequency (ACARS, VDL2) get the decoder's name in front
                const std::string title = chTitle(h);

                if (h.edge == 0) {
                    // band on the spectrum, edges on the waterfall
                    float x0 = offsetToX(c.lowOffset(off)), x1 = offsetToX(c.highOffset(off));
                    if (x1 - x0 < 2.0f) { const float m = (x0 + x1) / 2.0f; x0 = m - 1.0f; x1 = m + 1.0f; }
                    const float cx0 = std::max(0.0f, x0), cx1 = std::min((float)layout.specW, x1);
                    if (cx1 > cx0) {
                        sf::RectangleShape band({cx1 - cx0, specBot - specTop});
                        band.setPosition({cx0, specTop});
                        band.setFillColor(dragging ? withAlpha(h.col, 70) : fillCol);
                        window.draw(band);
                    }
                    for (float x : {x0, x1}) {
                        if (x < 0.0f || x > layout.specW) continue;
                        vline(x, specTop, specBot, edgeCol);
                        vline(x, wfTop, wfBot, wfEdgeCol);
                    }
                    label(title, std::max(2.0f, x0 + 3.0f), specTop + 24.0f + 12.0f * (float)labelRow[i], edgeCol);

                    // handle: a yellow grip on the tuning bar at the channel frequency
                    const float hw = 12.0f, hh = layout.tuneBarH - 6.0f;
                    sf::RectangleShape grip({hw, hh});
                    grip.setPosition({std::round(h.x - hw / 2.0f), tbTop + 3.0f});
                    grip.setFillColor((dragging || (int)i == hovered) ? lighter(yellow) : yellow);
                    grip.setOutlineThickness(c.followVfo ? 2.0f : 1.0f);
                    grip.setOutlineColor(c.followVfo ? sf::Color::Red : sf::Color(60, 45, 0));
                    window.draw(grip);
                    for (float gx : {-2.0f, 2.0f}) vline(std::round(h.x + gx), tbTop + 7.0f, tbBot - 7.0f, sf::Color(90, 70, 0));
                } else {
                    // outside the visible spectrum: an arrow at the edge of the tuning bar
                    sf::ConvexShape arrow(3);
                    const float ax = h.edge < 0 ? 4.0f : layout.specW - 4.0f;
                    const float dir = h.edge < 0 ? 1.0f : -1.0f;
                    const float my = (tbTop + tbBot) / 2.0f;
                    arrow.setPoint(0, {ax, my});
                    arrow.setPoint(1, {ax + dir * 10.0f, my - 7.0f});
                    arrow.setPoint(2, {ax + dir * 10.0f, my + 7.0f});
                    arrow.setFillColor((int)i == hovered ? lighter(yellow) : yellow);
                    window.draw(arrow);
                    const std::string t = title + "  (outside the view)";
                    sf::Text probe(font, t, 20);
                    const float tw = probe.getGlobalBounds().size.x * 0.5f;
                    label(t, h.edge < 0 ? 18.0f : layout.specW - 18.0f - tw, tbTop + 6.0f, edgeCol);
                }
            }

            // what happens on release / click
            if (chDrag.set && chDrag.moved) {
                vline(chDrag.x, specTop, wfBot, sf::Color(255, 235, 120, 200));
                const std::string what = chDrag.snap == 2 ? "release: follow the tuner"
                                       : chDrag.snap == 1 ? mhzText(chDrag.freqHz) + " MHz  (known frequency)"
                                                          : mhzText(chDrag.freqHz) + " MHz";
                label(what, std::min(chDrag.x + 8.0f, layout.specW - 200.0f), tbTop - 15.0f, sf::Color(255, 235, 120), 22);
            } else if (chEdge.set) {
                char b[48];
                std::snprintf(b, sizeof(b), "width %.1f kHz", chEdge.widthHz / 1000.0);
                const std::string what = chEdge.snapTuner ? "release: the tuner's bandwidth" : std::string(b);
                label(what, std::min(chEdge.x + 8.0f, layout.specW - 200.0f), specTop + 28.0f, sf::Color(255, 235, 120), 22);
            } else if (hovered >= 0) {
                const ChHandle& h = chHandles[hovered];
                const std::string tip = h.dec + " " + h.ch.name + ":  drag = move,  drop on the tuner = follow it,  click = listen";
                label(tip, std::clamp(h.x - 60.0f, 4.0f, layout.specW - 380.0f), tbTop - 15.0f, sf::Color(255, 235, 120), 22);
            } else if (hoveredEdge >= 0) {
                const std::string tip = chHandles[hoveredEdge].ch.name + ":  drag the edge = channel width" +
                    std::string(chHandles[hoveredEdge].set->followTunerWidth && chHandles[hoveredEdge].ch.followVfo ? ",  drop on the grey edge = the tuner's width" : "");
                label(tip, std::clamp(mousePos.x + 10.0f, 4.0f, layout.specW - 420.0f), specTop + 28.0f, sf::Color(255, 235, 120), 22);
            }
        }

        // --- RDS PANEL (top right corner of the spectrum) ---
        if (rdsOn && mode == Mode::WFM && cur->sd.source) {
            const float pad = 10.0f;
            const float panelW = std::min(440.0f, layout.specW - 20.0f);
            const float panelX = layout.specW - panelW - 10.0f;
            const float panelY = (float)TOP_BAR_H + 14.0f;
            const float innerW = panelW - 2.0f * pad;
            auto utf8 = [](const std::string& s) { return sf::String::fromUtf8(s.begin(), s.end()); };
            auto makeText = [&](const sf::String& str, unsigned size, sf::Color col) {
                sf::Text t(font, str, size);
                t.setScale({0.5f, 0.5f});
                t.setFillColor(col);
                return t;
            };

            std::vector<sf::Text> rows;
            std::vector<float> rowGap;
            sf::Color dim(160, 160, 170);

            if (!rdsInfo.hasData) {
                bool rateOk = RDSDecoder::rateSupported(hwSampleRate);
                rows.push_back(makeText(rateOk ? (rdsInfo.synced ? "RDS: receiving..." : "RDS: searching...")
                                               : "RDS: sample rate too low (min. 125 kSps)", 26, dim));
                rowGap.push_back(0.0f);
            } else {
                std::string ps = rdsInfo.ps;
                sf::Text psText = makeText(utf8(ps), 52, sf::Color::White);
                psText.setStyle(sf::Text::Bold);
                rows.push_back(psText); rowGap.push_back(0.0f);

                std::string line2 = rdsInfo.pty >= 0 ? std::string(rdsPtyName(rdsInfo.pty)) : std::string("");
                if (rdsInfo.tp) line2 += "  |  TP";
                if (rdsInfo.ta) line2 += "  |  TA";
                if (!line2.empty()) { rows.push_back(makeText(line2, 26, Theme::Accent)); rowGap.push_back(6.0f); }

                // RadioText, word-wrapped to at most 3 lines
                if (!rdsInfo.radioText.empty()) {
                    std::vector<std::string> words;
                    std::stringstream ws(rdsInfo.radioText);
                    for (std::string w; ws >> w; ) words.push_back(w);
                    std::vector<std::string> lines;
                    std::string cur;
                    for (const auto& w : words) {
                        std::string trial = cur.empty() ? w : cur + " " + w;
                        if (cur.empty() || makeText(utf8(trial), 26, sf::Color::White).getGlobalBounds().size.x <= innerW) cur = trial;
                        else { lines.push_back(cur); cur = w; }
                    }
                    if (!cur.empty()) lines.push_back(cur);
                    if (lines.size() > 3) lines.resize(3);
                    for (size_t i = 0; i < lines.size(); i++) {
                        rows.push_back(makeText(utf8(lines[i]), 26, sf::Color(225, 225, 230)));
                        rowGap.push_back(i == 0 ? 6.0f : 1.0f);
                    }
                }

                std::string line4 = rdsInfo.clockTime;
                if (!line4.empty()) line4 += "     ";
                line4 += "Block errors: " + std::to_string((int)std::lround(rdsInfo.blockErrorRate * 100.0)) + "%";
                rows.push_back(makeText(line4, 22, dim)); rowGap.push_back(6.0f);
            }

            float contentH = 0.0f;
            for (size_t i = 0; i < rows.size(); i++) contentH += rowGap[i] + rows[i].getGlobalBounds().size.y + 4.0f;
            float panelH = contentH + 2.0f * pad;

            sf::RectangleShape rdsBg({panelW, panelH});
            rdsBg.setPosition({panelX, panelY});
            rdsBg.setFillColor(sf::Color(20, 20, 25, 225));
            rdsBg.setOutlineColor(Theme::Accent);
            rdsBg.setOutlineThickness(1.0f);
            window.draw(rdsBg);

            // Header: sync indicator + PI code
            sf::CircleShape syncDot(4.0f);
            syncDot.setFillColor(rdsInfo.synced ? sf::Color(60, 220, 90) : sf::Color(110, 110, 110));
            syncDot.setPosition({panelX + panelW - pad - 8.0f, panelY + pad + 2.0f});
            window.draw(syncDot);
            if (rdsInfo.hasData) {
                char piBuf[16];
                std::snprintf(piBuf, sizeof(piBuf), "PI %04X", rdsInfo.pi);
                sf::Text piText = makeText(piBuf, 26, dim);
                piText.setPosition({panelX + panelW - pad - 16.0f - piText.getGlobalBounds().size.x, panelY + pad - 2.0f});
                window.draw(piText);
            }

            float y = panelY + pad;
            for (size_t i = 0; i < rows.size(); i++) {
                y += rowGap[i];
                sf::FloatRect lb = rows[i].getLocalBounds();
                rows[i].setPosition({panelX + pad, y - lb.position.y * 0.5f});
                window.draw(rows[i]);
                y += rows[i].getGlobalBounds().size.y + 4.0f;
            }
        }

        if (aprsOn) {
            float aprsH = 200.0f;
            float overlayY = aprsTopY;

            sf::RectangleShape bg({layout.specW, aprsH});
            bg.setPosition({0, overlayY});
            bg.setFillColor(sf::Color(20, 20, 25, 240));
            bg.setOutlineColor(sf::Color::White);
            bg.setOutlineThickness(1);
            window.draw(bg);

            AprsLastPacket pkt;
            std::deque<AprsLastPacket> logs;
            {
                std::lock_guard<std::mutex> l(cur->sd.mtx);
                pkt = cur->sd.lastAprs;
                logs = cur->sd.aprsHistory;
            }

            float compassX = 60.0f;
            float compassY = overlayY + 70.0f;
            float compassR = 40.0f;

            sf::CircleShape compass(compassR);
            compass.setOrigin({compassR, compassR});
            compass.setPosition({compassX, compassY});
            compass.setFillColor(sf::Color::Transparent);
            compass.setOutlineColor(sf::Color(100,100,100));
            compass.setOutlineThickness(2);
            window.draw(compass);

            sf::Text dirT(font, "N", 24);
            dirT.setScale({0.5f, 0.5f});
            dirT.setFillColor(sf::Color::Yellow);
            dirT.setPosition({compassX - 4, compassY - compassR - 15});
            window.draw(dirT);

            sf::RectangleShape northTick({2, 6});
            northTick.setFillColor(sf::Color::Yellow);
            northTick.setPosition({compassX - 1, compassY - compassR});
            window.draw(northTick);

            if (pkt.course >= 0) {
                sf::VertexArray arrow(sf::PrimitiveType::Lines, 2);
                float rad = (pkt.course - 90.0f) * (3.14159f / 180.0f);
                arrow[0].position = sf::Vector2f(compassX, compassY);
                arrow[0].color = sf::Color::Red;
                arrow[1].position = sf::Vector2f(compassX + cos(rad)*compassR, compassY + sin(rad)*compassR);
                arrow[1].color = sf::Color::Red;
                window.draw(arrow);

                sf::Text crsTxt(font, std::to_string((int)pkt.course) + " deg", 24);
                crsTxt.setScale({0.5f, 0.5f});
                crsTxt.setPosition({compassX - 20, compassY + compassR + 5});
                window.draw(crsTxt);
            }

            float textX = compassX + compassR + 40.0f;
            float curY = overlayY + 20.0f;

            // which channel APRS decodes (same yellow line as in the other decoder panels)
            {
                const std::string chText = cur->inputIsAudio ? std::string("Input: audio") : cur->sd.aprsCh.describe();
                sf::Text chT(font, chText, 20);
                chT.setScale({0.5f, 0.5f});
                chT.setFillColor(sf::Color(255, 210, 0));
                chT.setPosition({textX, overlayY + 4.0f});
                window.draw(chT);
            }

            sf::Text lblCall(font, pkt.src.empty() ? "-- WAITING --" : pkt.src, 64);
            lblCall.setScale({0.5f, 0.5f});
            lblCall.setPosition({textX, curY});
            lblCall.setFillColor(sf::Color::Green);
            lblCall.setStyle(sf::Text::Bold);
            window.draw(lblCall);

            if (!pkt.dest.empty()) {
                sf::Text lblDest(font, "> " + pkt.dest, 40);
                lblDest.setScale({0.5f, 0.5f});
                lblDest.setPosition({textX + lblCall.getGlobalBounds().size.x + 15, curY + 12});
                lblDest.setFillColor(sf::Color(200,200,200));
                window.draw(lblDest);
            }

            curY += 40.0f;

            if (!pkt.coords.empty()) {
                sf::Text lblGPS(font, "GPS: " + pkt.coords, 36);
                lblGPS.setScale({0.5f, 0.5f});
                lblGPS.setPosition({textX, curY});
                lblGPS.setFillColor(sf::Color::Cyan);
                window.draw(lblGPS);
                curY += 25.0f;
            }

            if (!pkt.comment.empty()) {
                std::string wrappedMsg = wrapText(pkt.comment, font, 32, (layout.specW - 350.0f - textX) * 2.0f);
                sf::Text lblMsg(font, wrappedMsg, 32);
                lblMsg.setScale({0.5f, 0.5f});
                lblMsg.setPosition({textX, curY});
                lblMsg.setFillColor(sf::Color::Yellow);
                window.draw(lblMsg);
            }

            float logX = layout.specW - 300;
            float logW = 300;

            sf::RectangleShape divLine({2, aprsH});
            divLine.setPosition({logX, overlayY});
            divLine.setFillColor(sf::Color::White);
            window.draw(divLine);

            sf::Text logTitle(font, "PACKET HISTORY", 24);
            logTitle.setScale({0.5f, 0.5f});
            logTitle.setPosition({logX + 10, overlayY + 5});
            logTitle.setFillColor(sf::Color(150,150,150));
            window.draw(logTitle);

            float listTopY = overlayY + 25.0f;
            int i=0;
            for(const auto& logPkt : logs) {
                float yPos = listTopY + aprsLogScrollOffset + (i*16.0f);
                if (yPos > listTopY - 10 && yPos < layout.winH - 5) {
                    sf::FloatRect rowRect({logX, yPos}, {logW, 16.0f});
                    bool isHover = rowRect.contains(mousePos) && !showAprsModal;

                    if (isHover) {
                        sf::RectangleShape hl(sf::Vector2f(logW - 2, 16.0f));
                        hl.setPosition({logX + 1, yPos});
                        hl.setFillColor(sf::Color(50, 50, 60));
                        window.draw(hl);
                    }

                    std::string lShort = logPkt.timestamp + " " + logPkt.src + ">" + logPkt.dest;
                    if(lShort.length()>35) lShort=lShort.substr(0,32)+"...";

                    sf::Text l(font, lShort, 24);
                    l.setScale({0.5f, 0.5f});
                    l.setPosition({logX + 10, yPos});
                    if (isHover) l.setFillColor(sf::Color::White);
                    else l.setFillColor(sf::Color(200,200,200));
                    window.draw(l);
                }
                i++;
            }
        }

        // --- Decoder tabs (above the panel; drawn first: the panel's drop-down lists open over them) ---
        tabHits.clear();
        {
            // per receiver: switching to a tab that skipped blocks long ago is no new warning
            const unsigned long long drops = cur->sd.decoderDrops.load();
            if (drops > cur->seenDrops) { decoderDropClock.restart(); decoderDropSeen = true; }
            cur->seenDrops = drops;
        }
        if (!tabs.empty()) {
            sf::RectangleShape strip({layout.specW, TAB_H});
            strip.setPosition({0.0f, tabTop});
            strip.setFillColor(sf::Color(24, 25, 30, 250));
            window.draw(strip);
            auto tabText = [&](const std::string& str, float x, float y, sf::Color c) {
                sf::Text t(font, sf::String::fromUtf8(str.begin(), str.end()), 22);
                t.setScale({0.5f, 0.5f});
                t.setFillColor(c);
                t.setPosition({std::round(x), std::round(y)});
                window.draw(t);
                return t.getGlobalBounds().size.x;
            };
            float tx = 4.0f;
            for (const DecTab& t : tabs) {
                sf::Text probe(font, sf::String::fromUtf8(t.name.begin(), t.name.end()), 22);
                const float tw = probe.getGlobalBounds().size.x * 0.5f + 40.0f;
                const bool sel = (&t == selTab);
                sf::RectangleShape bg({tw, TAB_H - 3.0f});
                bg.setPosition({tx, tabTop + 3.0f});
                bg.setFillColor(sel ? sf::Color(20, 22, 27) : sf::Color(40, 42, 50));
                bg.setOutlineThickness(1.0f);
                bg.setOutlineColor(sel ? Theme::Accent : sf::Color(60, 60, 68));
                window.draw(bg);
                sf::RectangleShape mark({4.0f, TAB_H - 9.0f});
                mark.setPosition({tx + 5.0f, tabTop + 6.0f});
                mark.setFillColor(t.color);
                window.draw(mark);
                tabText(t.name, tx + 13.0f, tabTop + 5.0f, sel ? sf::Color::White : sf::Color(180, 180, 190));
                const sf::FloatRect closeR({tx + tw - 16.0f, tabTop + 3.0f}, {16.0f, TAB_H - 3.0f});
                const bool overClose = closeR.contains(mousePos);
                tabText("x", tx + tw - 12.0f, tabTop + 4.0f, overClose ? sf::Color(255, 120, 120) : sf::Color(130, 130, 140));
                tabHits.push_back({sf::FloatRect({tx, tabTop}, {tw, TAB_H}), closeR, t.entry, t.name});
                tx += tw + 3.0f;
            }
            if (decoderDropSeen && decoderDropClock.getElapsedTime().asSeconds() < 5.0f)
                tabText("Decoders can't keep up (CPU): some blocks were skipped. Switch some off or lower the sample rate.",
                        tx + 10.0f, tabTop + 5.0f, sf::Color(255, 140, 90));
        }

        SDRModule* decoderUI = panelMod;

        if (decoderUI && decoderUI->enabled) {
            float tableH = panelH;
            float tableY = panelTop;
            decoderUI->draw(window, 0, tableY, layout.specW, tableH);
        }
        else {
            // --- STATUS BAR WITH S-METER (hidden while decoders are running) ---
            float statusH = 40.0f;
            float statusY = layout.winH - statusH - timelineStripH;   // a file: above its timeline

            sf::RectangleShape statusBarBg({layout.specW, statusH});
            statusBarBg.setPosition({0, statusY});
            statusBarBg.setFillColor(sf::Color(30, 30, 35));
            statusBarBg.setOutlineThickness(1);
            statusBarBg.setOutlineColor(sf::Color(60, 60, 60));
            window.draw(statusBarBg);

            float ledX = 20.0f;
            auto drawLED = [&](std::string label, bool state, sf::Color colorOn) {
                sf::CircleShape led(5.0f);
                led.setPosition({ledX, statusY + 15.0f});
                led.setFillColor(state ? colorOn : sf::Color(50, 50, 50));
                window.draw(led);

                sf::Text txt(font, label, 20);
                txt.setScale({0.5f, 0.5f});
                txt.setPosition({ledX + 15.0f, statusY + 10.0f});
                txt.setFillColor(state ? sf::Color::White : sf::Color(150, 150, 150));
                window.draw(txt);
                ledX += 80.0f;
            };

            drawLED("PLAY", cur->sd.isPlaying, Theme::Accent);
            drawLED("REC", cur->sd.isRecording, sf::Color(255, 50, 50));
            drawLED("HW", isHw, sf::Color(50, 255, 100));

            // --- SIGNAL METER ---
            // The spectrum levels are relative to the ADC full scale (dBFS) and change with the
            // RF gain, so they can't be shown as calibrated dBm or S units. Instead the meter
            // shows how far the tuned signal is above the noise floor (SNR).
            const float meterW = 260.0f;
            const float meterX = (layout.specW / 2.0f) - (meterW / 2.0f);
            float snrDb = -1.0f;
            double signalDb = -127.0;
            const double viewW = viewEndPct - viewStartPct;
            if (!spectrum.empty() && viewW > 0.0 && tunePct >= viewStartPct && tunePct <= viewEndPct) {
                const int n = (int)spectrum.size();
                std::vector<double> sorted(spectrum.begin(), spectrum.end());
                const size_t q = sorted.size() / 5;   // noise floor: 20th percentile of the visible bins
                std::nth_element(sorted.begin(), sorted.begin() + q, sorted.end());
                const double floorDb = sorted[q];
                const double half = std::max(slBW->currentVal / hwSampleRate, 1.0 / n) / 2.0;
                int b0 = (int)(((tunePct - half) - viewStartPct) / viewW * n);
                int b1 = (int)(((tunePct + half) - viewStartPct) / viewW * n);
                b0 = std::clamp(b0, 0, n - 1);
                b1 = std::clamp(b1, b0, n - 1);
                for (int b = b0; b <= b1; b++) signalDb = std::max(signalDb, spectrum[b]);
                snrDb = (float)std::max(0.0, signalDb - floorDb);
            }
            // Smooth the readings (time constant ~0.8 s) so the meter doesn't jump around
            {
                const float dt = meterClock.restart().asSeconds();
                const float a = 1.0f - std::exp(-dt / 0.8f);
                if (snrDb < 0.0f) {
                    meterSnr = -1.0f;
                } else if (meterSnr < 0.0f) {
                    meterSnr = snrDb; meterSignal = signalDb;
                } else {
                    meterSnr += a * (snrDb - meterSnr);
                    meterSignal += a * (signalDb - meterSignal);
                }
                snrDb = meterSnr;
                signalDb = meterSignal;
            }

            sf::RectangleShape sBg({meterW, 8.0f});
            sBg.setPosition({meterX, statusY + 22.0f});
            sBg.setFillColor(sf::Color(20, 20, 22));
            window.draw(sBg);
            if (snrDb > 0.0f) {
                sf::RectangleShape sFill({meterW * std::clamp(snrDb / 60.0f, 0.0f, 1.0f), 8.0f});
                sFill.setPosition({meterX, statusY + 22.0f});
                sFill.setFillColor(Theme::AccentDim);
                window.draw(sFill);
            }
            for (int db = 10; db < 60; db += 10) {   // a tick every 10 dB
                sf::RectangleShape tick({1.0f, 8.0f});
                tick.setPosition({meterX + meterW * db / 60.0f, statusY + 22.0f});
                tick.setFillColor(sf::Color(70, 70, 75));
                window.draw(tick);
            }
            char meterTxt[64];
            if (snrDb >= 0.0f) std::snprintf(meterTxt, sizeof(meterTxt), "SNR %.0f dB   (%.0f dBFS)", snrDb, signalDb);
            else std::snprintf(meterTxt, sizeof(meterTxt), "SNR --");
            sf::Text sValTxt(font, meterTxt, 22);
            sValTxt.setScale({0.5f, 0.5f});
            sValTxt.setPosition({meterX, statusY + 5.0f});
            sValTxt.setFillColor(sf::Color(170, 170, 180));
            window.draw(sValTxt);

            // The ADC clips: the signal is distorted (weak signals and RDS suffer first)
            if (sourceReconnecting) {
                sf::Text rc(font, "DEVICE LOST - reconnecting...", 22);
                rc.setScale({0.5f, 0.5f});
                rc.setPosition({meterX + meterW + 16.0f, statusY + 14.0f});
                rc.setFillColor(sf::Color(255, 90, 90));
                window.draw(rc);
            } else if (adcOverload > 0.0002f) {
                sf::Text ov(font, "ADC OVERLOAD - lower the RF gain", 22);
                ov.setScale({0.5f, 0.5f});
                ov.setPosition({meterX + meterW + 16.0f, statusY + 14.0f});
                ov.setFillColor(sf::Color(255, 150, 60));
                window.draw(ov);
            }
        }

        slZoomBar.draw(window);

        if (cur->sourceType == 0) {   // the file's timeline on its own grey strip (with any decoder, also APRS)
            sf::RectangleShape strip({layout.specW, TIMELINE_H});
            strip.setPosition({0, layout.winH - TIMELINE_H});
            strip.setFillColor(sf::Color(30, 30, 35));
            strip.setOutlineThickness(1.0f);
            strip.setOutlineColor(sf::Color(60, 60, 60));
            window.draw(strip);
            timeSlider.draw(window);
        }
        sidebar.draw(window);
        }   // receiver's view

        // The pipeline workspace over everything below the tabs (the receivers keep running)
        if (wsShown) {
            char ld[48];
            std::snprintf(ld, sizeof(ld), "load %.0f %%", wsShown->load() * 100.0);
            wsShown->editor.draw(window, sf::FloatRect({0.0f, (float)TOP_BAR_H}, {layout.winW, layout.winH - TOP_BAR_H}), ld);
        }

        window.draw(topBar);
        drawRxTabs();
        freqVFO.draw(window);
        btnPlay.draw(window);
        if (isHw) btnTuningMode.draw(window);
        btnMap.setActive(mapWindow.isOpen());
        btnMap.draw(window);
        if (satTracker.active()) {
            // what is being tracked, next to the MAP button
            char tb[160];
            const orbit::LookAngles& la = satTracker.la;
            std::snprintf(tb, sizeof(tb), "%s %s", satTracker.mode == orbit::TrackMode::Doppler ? "DOPPLER" : "TRACK", satTracker.satName.c_str());
            sf::Text t1(font, tb, 26);
            t1.setScale({0.5f, 0.5f});
            t1.setStyle(sf::Text::Bold);
            t1.setFillColor(sf::Color(255, 160, 60));
            t1.setPosition({486.0f, 12.0f});
            window.draw(t1);
            if (la.ok) std::snprintf(tb, sizeof(tb), "%+.2f kHz   El %.0f\xC2\xB0  Az %.0f\xC2\xB0%s", satTracker.dopplerHz / 1000.0, la.elDeg, la.azDeg,
                                     satTracker.applyStatus.empty() ? "" : "   (!)");
            else std::snprintf(tb, sizeof(tb), "%s", satTracker.applyStatus.c_str());
            std::string s2 = tb;
            sf::Text t2(font, sf::String::fromUtf8(s2.begin(), s2.end()), 22);
            t2.setScale({0.5f, 0.5f});
            t2.setFillColor(la.ok && la.elDeg > 0 ? sf::Color(90, 255, 120) : sf::Color(170, 170, 180));
            t2.setPosition({486.0f, 30.0f});
            window.draw(t2);
        }
        btnMute.draw(window);
        volSlider.draw(window);

        if (showAprsModal) {
            float mw = 600;
            float mh = 400;
            float mx = (layout.winW - mw) / 2;
            float my = (layout.winH - mh) / 2;

            sf::RectangleShape modalBg({mw, mh});
            modalBg.setPosition({mx, my});
            modalBg.setFillColor(sf::Color(30, 30, 35));
            modalBg.setOutlineThickness(2);
            modalBg.setOutlineColor(Theme::Accent);
            window.draw(modalBg);

            sf::Text title(font, "Packet Details", 36);
            title.setScale({0.5f, 0.5f});
            title.setPosition({mx + 20, my + 15});
            title.setFillColor(Theme::Accent);
            window.draw(title);

            float cx = mx + 20;
            float cy = my + 60;

            auto drawField = [&](std::string label, std::string val, sf::Color c = sf::Color::White) {
                sf::Text l(font, label, 24);
                l.setScale({0.5f, 0.5f});
                l.setPosition({cx, cy});
                l.setFillColor(sf::Color(150,150,150));
                window.draw(l);

                sf::Text v(font, val.empty() ? "N/A" : val, 24);
                v.setScale({0.5f, 0.5f});
                v.setPosition({cx + 120, cy});
                v.setFillColor(c);
                window.draw(v);
                cy += 30;
            };

            drawField("Time:", selectedAprsPacket.timestamp);
            drawField("Source:", selectedAprsPacket.src, sf::Color::Green);
            drawField("Dest:", selectedAprsPacket.dest);
            drawField("Coords:", selectedAprsPacket.coords, sf::Color::Cyan);

            std::string crs = (selectedAprsPacket.course >= 0) ? std::to_string((int)selectedAprsPacket.course) + " deg" : "";
            drawField("Course:", crs);

            sf::Text rawL(font, "Raw Packet:", 24);
            rawL.setScale({0.5f, 0.5f});
            rawL.setPosition({cx, cy});
            rawL.setFillColor(sf::Color(150,150,150));
            window.draw(rawL);
            cy += 20;

            std::string wrappedRaw = wrapText(selectedAprsPacket.raw, font, 24, mw - 40);
            sf::Text rawV(font, wrappedRaw, 24);
            rawV.setScale({0.5f, 0.5f});
            rawV.setPosition({cx, cy});
            rawV.setFillColor(sf::Color::Yellow);
            window.draw(rawV);

            btnModalClose.setPosition(mx + mw - 100, my + mh - 50);
            btnModalClose.draw(window);

            btnModalCopy.setPosition(mx + 20, my + mh - 50);
            btnModalCopy.draw(window);
        }


        if (modInspector->isOpen && !wsShown) {
            float mx = modInspector->headerBg.getPosition().x + 10;
            float my = modInspector->headerBg.getPosition().y + 30;

            miniSpr.setPosition({(float)mx, (float)my});

            sf::RectangleShape border({(float)MINI_W + 2, (float)MINI_H + 2});
            border.setPosition({(float)mx - 1, (float)my - 1});
            border.setFillColor(sf::Color::Transparent);
            border.setOutlineColor(sf::Color(100, 100, 100));
            border.setOutlineThickness(1);
            window.draw(border);

            window.draw(miniSpr);

            sf::RectangleShape centerLine({2.0f, (float)MINI_H});
            centerLine.setPosition({mx + MINI_W / 2.0f - 1.0f, my});
            centerLine.setFillColor(sf::Color(255, 0, 0, 150));
            window.draw(centerLine);

            sf::Text infoTxt(font, "Bandwidth Scope (" + std::to_string((int)slBW->currentVal) + " Hz)", 14);
            infoTxt.setPosition({(float)mx, (float)my - 16});
            infoTxt.setScale({0.5f, 0.5f});
            infoTxt.setFillColor(sf::Color::Yellow);
        }

        // Popup window of a decoder, above everything else
        for (SDRModule* m : runningMods)
            if (m->enabled && m->modalOpen()) { m->drawModal(window); break; }
        window.display();
    }

    // RX1 is saved with the usual keys (the widgets must show it), the others as rxN_...
    if (!receivers.empty() && cur != receivers[0].get()) switchReceiver(receivers[0].get());
    SettingsManager saveMgr;
    saveMgr.set("rx_count", (int)receivers.size());
    // the pipeline tabs (their chains are saved in pipelines.json); then their threads stop,
    // before the speaker (AudioSink) and the receivers go
    {
        const auto tabsNow = pl::Hub::get().list();
        saveMgr.set("pipe_tabs", (int)tabsNow.size());
        int shownIdx = -1;
        for (size_t i = 0; i < tabsNow.size(); i++) {
            saveMgr.set("pipe_tab" + std::to_string(i), toHex(tabsNow[i]->editor.title()));
            if (pipeShown && tabsNow[i]->uid == pipeUid) shownIdx = (int)i;
        }
        saveMgr.set("pipe_shown", shownIdx);
        for (pl::Workspace* w : tabsNow) pl::Hub::get().close(w);
    }
    for (size_t i = 0; i < receivers.size(); i++) {
        saveMgr.set("rx" + std::to_string(i + 1) + "_speaker", receivers[i]->sd.audioOut.load());
        saveMgr.set("rx" + std::to_string(i + 1) + "_audio_dev", toHex(receivers[i]->audioDevice));
    }
    for (size_t i = 1; i < receivers.size(); i++) {
        Receiver* r = receivers[i].get();
        const std::string k = "rx" + std::to_string(i + 1) + "_";
        saveMgr.set(k + "source_type", r->sourceType);
        saveMgr.set(k + "frequency", std::to_string(r->centerFreq));
        const bool haveDev = r->deviceSel >= 0 && r->deviceSel < (int)r->cachedDevices.size();
        saveMgr.set(k + "device", haveDev ? "x" + toHex(r->cachedDevices[r->deviceSel].id) : std::string());
        saveMgr.set(k + "rate", r->rateSel);
        saveMgr.set(k + "host", r->hostText);
        saveMgr.set(k + "ppm", r->sd.hw.ppm);
        std::lock_guard<std::mutex> l(r->sd.mtx);
        saveMgr.set(k + "mode", (int)r->sd.mode);
        saveMgr.set(k + "bandwidth", (float)r->sd.bandwidth);
    }
    for (auto& mod : sidebar.modules) saveMgr.set("sidebar_" + mod->title, mod->isOpen);

    saveMgr.set("theme", ddTheme->selectedIndex);
    saveMgr.set("ui_scale_pct", uiScaleIdx == 0 ? 0 : (int)std::lround(UI_SCALE_VALUES[uiScaleIdx] * 100.0f));
    saveMgr.set("zoom", slZoom->currentVal);
    saveMgr.set("min_db", slMinDb->currentVal);
    saveMgr.set("max_db", slMaxDb->currentVal);
    saveMgr.set("step_index", ddSnap->selectedIndex);
    saveMgr.set("mode_index", (int)cur->sd.mode);
    saveMgr.set("bandwidth", slBW->currentVal);
    saveMgr.set("squelch", slSq->currentVal);
    saveMgr.set("scan_threshold", slScanThr->currentVal);
    saveMgr.set("scan_hang", slScanHang->currentVal);
    saveMgr.set("bookmark_bank", bookList->bankFilter);
    saveMgr.set("bookmark_markers", chkBookMarks->checked);
    DigitalVoiceDecoder::channelLabel = nullptr;   // it refers to this function's locals
    saveMgr.set("stereo", chkStereo->checked);
    saveMgr.set("rds", chkRds->checked);
    saveMgr.set("picture_path", pictureDirCustom);
    saveMgr.set("volume_pos", volSlider.currentVal);
    saveMgr.set("sstv_auto_slant", cur->sstv->getAutoSlant());
    saveMgr.set("sstv_slant", cur->sstv->getSlantPpm());
    saveMgr.set("sstv_auto_save", cur->sstv->getAutoSave());
    saveMgr.set("wefax_lpm", cur->wefax->getLpm());
    saveMgr.set("wefax_ioc", cur->wefax->getIoc());
    saveMgr.set("wefax_apt", cur->wefax->getAutoApt());
    saveMgr.set("wefax_slant", cur->wefax->getSlantPpm());
    saveMgr.set("wefax_auto_save", cur->wefax->getAutoSave());
    saveMgr.set("rf_gain", cur->sd.rfGain);
    saveMgr.set("source_type", ddSourceType->selectedIndex);
    saveMgr.set("hw_fm_notch", cur->sd.hw.fmNotch);
    saveMgr.set("hw_mw_notch", cur->sd.hw.mwNotch);
    saveMgr.set("hw_bias_t", cur->sd.hw.biasT);
    saveMgr.set("hw_direct_samp", cur->sd.hw.directSampling);
    saveMgr.set("hw_antenna", cur->sd.hw.antennaIndex);
    saveMgr.set("hw_ppm", cur->sd.hw.ppm);
    saveMgr.set("rtltcp_host", txtHost->getText());
    satTracker.saveSettings(saveMgr);
    mapWindow.saveSettings(saveMgr);

    {
        std::lock_guard<std::mutex> l(cur->sd.mtx);
        saveMgr.set("rec_path", cur->sd.recPath);
    }

    saveMgr.set("frequency", (double)cur->centerFreq);

    saveMgr.save(settingsPath);

    // Stop every receiver (its DSP thread, then the radio) while everything is still alive.
    // Leaving it to the destructors after main() returns would run when the driver libraries
    // may already be shutting down; the SDRplay API service doesn't cope well with a client
    // that disappears in the middle of a stream.
    cur = nullptr;
    receivers.clear();

    return 0;
}
