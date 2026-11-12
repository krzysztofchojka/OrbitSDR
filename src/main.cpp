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

    {
        std::lock_guard<std::mutex> lock(sourceMtx);
        currentSource = std::make_shared<FileSource>();
        if (currentSource->open("None")) {}
    }

    AudioSink audio;
    std::vector<std::string> deviceNames;
    for (const auto& dev : audio.availableDevices) deviceNames.push_back(dev.name);
    audio.initDevice(0, (int)AUDIO_RATE);
    audio.start();

    SharedData sharedData;
    sharedData.aprsHistory = loadLastLogLinesAsPackets(getAprsLogFilePath(), 20);
    sharedData.centerFreq = savedFreq;

    std::atomic<bool> dspRunning {true};
    std::thread dspThread(dspWorker, std::ref(dspRunning), std::ref(sharedData), std::ref(audio));

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
    const std::vector<std::string> UI_SCALE_NAMES  = {"Auto (system)", "100%", "125%", "150%", "175%", "200%", "250%"};
    const std::vector<float>       UI_SCALE_VALUES = {0.0f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f};
    int uiScaleIdx = 0;
    float autoUiScale = 1.0f;
    #ifdef _WIN32
    autoUiScale = detectWindowsUiScale((void*)window.getNativeHandle());
    std::cout << "[INFO] Detected Windows UI scale: " << autoUiScale << std::endl;
    #endif
    auto getUiScale = [&]() -> float {
        int idx = std::clamp(uiScaleIdx, 0, (int)UI_SCALE_VALUES.size() - 1);
        if (idx == 0) return autoUiScale;
        return UI_SCALE_VALUES[idx];
    };
    std::function<void()> applyUiScale; // assigned after updateLayout is defined

    // --- RTL-TCP connection state ---
    bool tcpConnected = false;
    std::string tcpAddr;

    std::vector<std::unique_ptr<SDRModule>> modules;
    auto aisMod = std::make_unique<AISDecoder>();
    ModuleContext ctx = { (float)AUDIO_RATE, font };
    aisMod->init(ctx);
    modules.push_back(std::move(aisMod));
    AISDecoder* ptrAIS = (AISDecoder*)modules[0].get();

    // Image decoders save their pictures into the picture folder: the one chosen by the
    // user, or OrbitSDR inside the system Pictures folder
    std::string pictureDirCustom = settingsMgr.getString("picture_path", "");
    std::string pictureDirDefault = getDefaultPicturesPath();
    auto pictureDir = [&]() { return pictureDirCustom.empty() ? pictureDirDefault : pictureDirCustom; };
    auto setPictureDir = [&](const std::string& d) { pictureDirCustom = d; };
    auto sstvMod = std::make_unique<SSTVModule>();
    sstvMod->init(ctx);
    sstvMod->saveDir = pictureDir;
    sstvMod->setSaveDir = setPictureDir;
    SSTVModule* ptrSSTV = sstvMod.get();
    modules.push_back(std::move(sstvMod));
    auto wefaxMod = std::make_unique<WEFAXModule>();
    wefaxMod->init(ctx);
    wefaxMod->saveDir = pictureDir;
    wefaxMod->setSaveDir = setPictureDir;
    WEFAXModule* ptrWEFAX = wefaxMod.get();
    modules.push_back(std::move(wefaxMod));
    auto ismMod = std::make_unique<ISMDecoder>();
    ismMod->init(ctx);
    ismMod->log.setPath(getIsmLogFilePath());
    ismMod->exportDir = getDefaultExportPath();
    ISMDecoder* ptrISM = ismMod.get();
    auto acarsMod = std::make_unique<ACARSDecoder>();
    acarsMod->init(ctx);
    acarsMod->log.setPath(getAcarsLogFilePath());
    acarsMod->exportDir = getDefaultExportPath();
    ACARSDecoder* ptrACARS = acarsMod.get();
    modules.push_back(std::move(acarsMod));
    modules.push_back(std::move(ismMod));

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
    bool stickyCenterMode = false;

    SdrButton btnPlay(40, 40, ">", font);
    btnPlay.setColor(sf::Color(116, 57, 57));
    SdrButton btnMute(40, 40, "M", font);

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
    auto btnAM  = std::make_shared<SdrButton>(36, 25, "AM", font);
    auto btnWFM = std::make_shared<SdrButton>(36, 25, "WFM", font);
    auto btnUSB = std::make_shared<SdrButton>(36, 25, "USB", font);
    auto btnLSB = std::make_shared<SdrButton>(36, 25, "LSB", font);
    auto btnRAW = std::make_shared<SdrButton>(42, 25, "RAW", font);
    auto btnOFF = std::make_shared<SdrButton>(36, 25, "OFF", font);

    auto rowModes = std::make_shared<RowContainer>();
    rowModes->add(btnNFM); rowModes->add(btnAM); rowModes->add(btnWFM);
    rowModes->add(btnUSB); rowModes->add(btnLSB); rowModes->add(btnRAW); rowModes->add(btnOFF);

    auto slBW = std::make_shared<Slider>(SIDEBAR_W - 40, 1000.0f, 200000.0f, 12500.0f, "Bandwidth (Hz)", font);

    // Input signal type: I/Q from a radio, or audio that is already demodulated (sound card,
    // a receiver's audio output, audio recordings). Set automatically when a source is opened,
    // the user can change it. Every demodulator and decoder follows this switch.
    bool inputIsAudio = false;
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

    int currentSourceType = 0;
    sf::Clock gainDebouncer;
    float pendingRfGain = -999.0f;

    slGain->onChange = [&](float v) {
        std::lock_guard<std::mutex> l(sharedData.mtx);
        sharedData.rfGain = v;
        std::stringstream ss;
        ss << std::fixed << std::setprecision(1);
        if (currentSourceType == 1 || currentSourceType == 4) {
            ss << "RF Gain: " << v << " dB";
        } else {
            ss << "RF Gain (dB): " << (int)v;
        }
        slGain->setText(ss.str());
        pendingRfGain = v;
    };

    chkAgc->onToggle = [&](bool b) {
        if (!b) {
            // Switching to manual: carry on from the gain the AGC had chosen, and show it as manual
            float agcGain = -1.0f;
            {
                std::lock_guard<std::mutex> l(sourceMtx);
                if (currentSource) agcGain = currentSource->getAutoGainDb();
            }
            if (agcGain >= 0.0f) slGain->setValueSilent(agcGain);
            slGain->onChange(slGain->currentVal);   // updates the label and queues the gain
        }
        std::lock_guard<std::mutex> l(sharedData.mtx);
        sharedData.rfGain = b ? -1.0f : slGain->currentVal;
        slGain->setEnabled(!b);
        pendingRfGain = b ? -1.0f : slGain->currentVal;
    };
    slGain->setEnabled(false);

    auto lblAntenna = std::make_shared<Label>("Antenna (SDRPlay):", font);
    auto ddAntenna = std::make_shared<Dropdown>(SIDEBAR_W - 40, 25.0f, font);
    ddAntenna->setOptions({"Antenna A / Default", "Antenna B", "Antenna C / Hi-Z"});
    ddAntenna->onChange = [&](int idx) {
        hwState.antennaIndex = idx;
        std::lock_guard<std::mutex> l(sourceMtx);
        if(currentSource) currentSource->setHardwareOption("antenna", idx);
    };

    auto lblDirectSamp = std::make_shared<Label>("Direct Sampling (RTL):", font);
    auto ddDirectSamp = std::make_shared<Dropdown>(SIDEBAR_W - 40, 25.0f, font);
    ddDirectSamp->setOptions({"Off (Default)", "I-ADC", "Q-ADC"});
    ddDirectSamp->onChange = [&](int idx) {
        hwState.directSampling = idx;
        std::lock_guard<std::mutex> l(sourceMtx);
        if(currentSource) currentSource->setHardwareOption("direct_sampling", idx);
    };

    auto rowFilters = std::make_shared<RowContainer>();
    auto chkFmNotch = std::make_shared<Checkbox>("FM Notch", font);
    chkFmNotch->onToggle = [&](bool b) {
        hwState.fmNotch = b;
        std::lock_guard<std::mutex> l(sourceMtx);
        if(currentSource) currentSource->setHardwareOption("fm_notch", b?1:0);
    };
    auto chkMwNotch = std::make_shared<Checkbox>("MW Notch", font);
    chkMwNotch->onToggle = [&](bool b) {
        hwState.mwNotch = b;
        std::lock_guard<std::mutex> l(sourceMtx);
        if(currentSource) currentSource->setHardwareOption("mw_notch", b?1:0);
    };
    rowFilters->add(chkFmNotch);
    rowFilters->add(chkMwNotch);

    auto chkBiasT = std::make_shared<Checkbox>("Bias-T Power", font);
    chkBiasT->onToggle = [&](bool b) {
        hwState.biasT = b;
        std::lock_guard<std::mutex> l(sourceMtx);
        if(currentSource) currentSource->setHardwareOption("bias_t", b?1:0);
    };

    auto updateRadioControls = [&](int sourceIdx) {
        modRadio->widgets.clear();
        if (sourceIdx != 0) {
            modRadio->addWidget(slGain);
            modRadio->addWidget(chkAgc);
        }
        if (sourceIdx == 1 || sourceIdx == 4) {
            modRadio->addWidget(lblDirectSamp);
            modRadio->addWidget(ddDirectSamp);
            modRadio->addWidget(chkBiasT);
            ddDirectSamp->setSelection(hwState.directSampling);
            chkBiasT->checked = hwState.biasT;
        } else if (sourceIdx == 2) {
            modRadio->addWidget(lblAntenna);
            modRadio->addWidget(ddAntenna);
            modRadio->addWidget(rowFilters);
            modRadio->addWidget(chkBiasT);
            ddAntenna->setSelection(hwState.antennaIndex);
            chkFmNotch->checked = hwState.fmNotch;
            chkMwNotch->checked = hwState.mwNotch;
            chkBiasT->checked = hwState.biasT;
        }
        sidebar.recalculateLayout();
        sidebar.updateStyle();
    };
    updateRadioControls(0);

    long long currentCenterFreq = savedFreq;
    long long pendingCenterFreq = 0;

    // Startup restores the last source silently; errors are only explained when the user
    // picks a source or refreshes the device list
    bool quietSourceErrors = false;

    std::function<void(int, std::string, int, std::string)> doOpenSource = [&](int sourceIdx, std::string deviceID, int rateIdx, std::string pathOverride) {
        {
            std::lock_guard<std::mutex> lock(sourceMtx);
            if (currentSource) {
                currentSource->stop();
                currentSource->close();
                currentSource = nullptr;
            }
        }
        // Whatever happens next, we are not connected to an RTL-TCP server any more
        tcpConnected = false;
        btnConnect->setText("Connect");
        btnConnect->setActive(false);

        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        std::shared_ptr<IQSource> newSource;
        uint32_t targetRate = 0;
        bool success = false;
        currentSourceType = sourceIdx;
        updateRadioControls(sourceIdx);
        if (sourceIdx != 0 && sourceIdx != 3 && applyInputType) applyInputType(false);   // radios: always I/Q

        if (sourceIdx == 0) {
            newSource = std::make_shared<FileSource>();
            std::string path = pathOverride.empty() ? "None" : pathOverride;
            success = newSource->open(path);
            {
                std::lock_guard<std::mutex> l(sharedData.mtx);
                sharedData.currentFilename = path;
            }
            std::string fileName = path.substr(path.find_last_of("/\\") + 1);
            ddRate->setOptions({fileName, "[ Change File... ]"});
            ddRate->setSelection(0);

            long long f = 0;
            std::regex freqRegex(R"(_(\d+)Hz_)");
            std::smatch match;
            if (std::regex_search(path, match, freqRegex)) {
                try { f = std::stoll(match[1]); } catch(...) {}
            }
            currentCenterFreq = f;
            freqVFO.setFrequency(f);
            { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.centerFreq = f; }
        }
        else if (sourceIdx == 1) {
            targetRate = (rateIdx < RTL_RATES_VAL.size()) ? RTL_RATES_VAL[rateIdx] : 2048000;
            auto rtl = std::make_shared<RtlSdrSource>();
            ddRate->setOptions(rtl->getAvailableSampleRatesText());
            ddRate->setSelection(rateIdx);
            success = rtl->open(deviceID, targetRate);
            newSource = rtl;

            if (currentCenterFreq < 1000000) currentCenterFreq = 97800000;
            freqVFO.setFrequency(currentCenterFreq);
            newSource->setCenterFrequency(currentCenterFreq);
            {
                std::lock_guard<std::mutex> l(sharedData.mtx);
                sharedData.tunedFreqPercent = 0.5;
                sharedData.viewCenterPct = 0.5;
                sharedData.centerFreq = currentCenterFreq;
            }
            slGain->setLimits(0.0f, 49.6f);
            slGain->setText("RF Gain: AGC");
            newSource->setHardwareOption("direct_sampling", hwState.directSampling);
            newSource->setHardwareOption("bias_t", hwState.biasT ? 1 : 0);
        }
        else if (sourceIdx == 2) {
            targetRate = (rateIdx < SDRPLAY_RATES_VAL.size()) ? SDRPLAY_RATES_VAL[rateIdx] : 2000000;
            auto sdr = std::make_shared<SdrPlaySource>();
            ddRate->setOptions(sdr->getAvailableSampleRatesText());
            ddRate->setSelection(rateIdx);
            success = sdr->open(deviceID, targetRate);
            newSource = sdr;

            if (currentCenterFreq < 1000000) currentCenterFreq = 97800000;
            freqVFO.setFrequency(currentCenterFreq);
            newSource->setCenterFrequency(currentCenterFreq);
            {
                std::lock_guard<std::mutex> l(sharedData.mtx);
                sharedData.tunedFreqPercent = 0.5;
                sharedData.viewCenterPct = 0.5;
                sharedData.centerFreq = currentCenterFreq;
            }
            slGain->setLimits(0.0f, 50.0f);
            chkAgc->checked = true;
            slGain->setEnabled(false);
            sharedData.rfGain = -1.0f;

            newSource->setHardwareOption("antenna", hwState.antennaIndex);
            newSource->setHardwareOption("fm_notch", hwState.fmNotch ? 1 : 0);
            newSource->setHardwareOption("mw_notch", hwState.mwNotch ? 1 : 0);
            newSource->setHardwareOption("bias_t", hwState.biasT ? 1 : 0);
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
            currentCenterFreq = 0;
            freqVFO.setFrequency(0);
            {
                std::lock_guard<std::mutex> l(sharedData.mtx);
                sharedData.tunedFreqPercent = 0.5;
                sharedData.viewCenterPct = 0.5;
                sharedData.centerFreq = 0;
            }
        }
        else if (sourceIdx == 4) {
            targetRate = (rateIdx < (int)RTL_RATES_VAL.size()) ? RTL_RATES_VAL[rateIdx] : 2048000;
            auto tcp = std::make_shared<RtlTcpSource>();
            ddRate->setOptions(tcp->getAvailableSampleRatesText());
            ddRate->setSelection(rateIdx);
            success = tcp->open(deviceID, targetRate);   // deviceID = "host:port"
            newSource = tcp;

            if (currentCenterFreq < 1000000) currentCenterFreq = 97800000;
            freqVFO.setFrequency(currentCenterFreq);
            newSource->setCenterFrequency(currentCenterFreq);
            {
                std::lock_guard<std::mutex> l(sharedData.mtx);
                sharedData.tunedFreqPercent = 0.5;
                sharedData.viewCenterPct = 0.5;
                sharedData.centerFreq = currentCenterFreq;
            }
            slGain->setLimits(0.0f, 49.6f);
            slGain->setText("RF Gain: AGC");
            newSource->setHardwareOption("direct_sampling", hwState.directSampling);
            newSource->setHardwareOption("bias_t", hwState.biasT ? 1 : 0);
        }

        if (!success && sourceIdx != 0) {
            if (!quietSourceErrors) showPopup("Error", "Could not open device.");
            return;
        }

        {
            std::lock_guard<std::mutex> lock(sourceMtx);
            currentSource = newSource;
        }
        // Sound card and mono WAV files are audio, everything else is I/Q (the user can change it)
        if (applyInputType) applyInputType(newSource && newSource->isAudio());

        // RTL-TCP connected successfully -> the button turns into "Disconnect"
        if (sourceIdx == 4) {
            tcpConnected = true;
            tcpAddr = deviceID;
            btnConnect->setText("Disconnect");
            btnConnect->setActive(true);
        }

        {
            std::lock_guard<std::mutex> lock(sharedData.mtx);
            sharedData.isPlaying = false;
            sharedData.isRecording = false;
        }
        audio.stop();
        btnPlay.setText(">");
        btnPlay.setActive(false);
        audio.clear();
    };

    // Cleanly drops the RTL-TCP connection and leaves the app in an idle (no source) state
    std::function<void()> doDisconnectTcp = [&]() {
        {
            std::lock_guard<std::mutex> lock(sourceMtx);
            if (currentSource) {
                currentSource->stop();
                currentSource->close();
            }
            currentSource = std::make_shared<FileSource>();   // idle placeholder, reads nothing
        }
        {
            std::lock_guard<std::mutex> lock(sharedData.mtx);
            sharedData.isPlaying = false;
            sharedData.isRecording = false;
        }
        audio.stop();
        btnPlay.setText(">");
        btnPlay.setActive(false);
        audio.clear();

        tcpConnected = false;
        btnConnect->setText("Connect");
        btnConnect->setActive(false);
    };

    std::vector<SDRDeviceItem> cachedDevices;

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
            { std::lock_guard<std::mutex> l(sharedData.mtx); fname = sharedData.currentFilename; }
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
    // May refill cachedDevices when the user lets us start the API service.
    auto explainNoSdrplay = [&]() {
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
                cachedDevices = SdrPlaySource::getDeviceList();
            }
            if (cachedDevices.empty() && answer == IDYES)
                showPopup("SDRplay", "No SDRplay device found yet.\n\nIf the service didn't start, open 'Services' (services.msc), select 'SDRplay API Service' and click Start - or restart the computer.");
        } else if (svc == "missing") {
            showPopup("SDRPlay Info", "The SDRplay API is not installed.\n\nIf you want to use SDRPlay, please install the official API from:\nhttps://www.sdrplay.com/downloads/");
        } else {
            showPopup("SDRPlay Info", "No SDRPlay device found.\n\nCheck the USB connection and close other programs that use the device (e.g. SDRuno or SDRconnect).");
        }
#else
        showPopup("SDRPlay Info", "No SDRPlay device found or SDRPlay API Service is not installed.\n\nIf you want to use SDRPlay, please install the official API from:\nhttps://www.sdrplay.com/downloads/");
#endif
    };

    ddSourceType->onChange = [&](int idx) {
        // Switching away from RTL-TCP while connected: drop the connection first
        if (idx != 4 && tcpConnected) doDisconnectTcp();

        if (idx == 0) {
            std::string path = openFileDialog();
            if (!path.empty()) {
                doOpenSource(0, "", 0, path);
                refreshSourceUI();
            } else refreshSourceUI();
        }
        else if (idx == 4) {
            if (!tcpConnected) {
                ddRate->setOptions(RTL_SAMPLE_RATES_TXT);
                ddRate->setSelection(2);
            }
            refreshSourceUI();   // shows host:port, connection is made only by the Connect button
        } else {
            cachedDevices.clear();
            std::vector<std::string> names;

            if (idx == 1) cachedDevices = RtlSdrSource::getDeviceList();
            else if (idx == 2) {
                cachedDevices = SdrPlaySource::getDeviceList();
                if (cachedDevices.empty() && !quietSourceErrors) explainNoSdrplay();
            }
            else if (idx == 3) cachedDevices = AudioCaptureSource::getDeviceList();

            for(auto& d : cachedDevices) names.push_back(d.name);
            if (names.empty()) names.push_back("No Devices Found");
            ddDevice->setOptions(names);

            refreshSourceUI();

            if (!cachedDevices.empty()) {
                doOpenSource(idx, cachedDevices[0].id, 0, "");
            }
        }
    };

    ddDevice->onChange = [&](int idx) {
        if (idx >= 0 && idx < cachedDevices.size()) {
            doOpenSource(ddSourceType->selectedIndex, cachedDevices[idx].id, ddRate->selectedIndex, "");
        }
    };

    btnRefresh->onClick = [&]() {
        {
            std::lock_guard<std::mutex> lock(sourceMtx);
            if (currentSource) {
                currentSource->stop();
                currentSource->close();
                currentSource = nullptr;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        int idx = ddSourceType->selectedIndex;
        if (idx == 1) cachedDevices = RtlSdrSource::getDeviceList();
        else if (idx == 2) {
            cachedDevices = SdrPlaySource::getDeviceList();
            if (cachedDevices.empty()) explainNoSdrplay();
        }
        else if (idx == 3) cachedDevices = AudioCaptureSource::getDeviceList();

        std::vector<std::string> names;
        for(auto& d : cachedDevices) names.push_back(d.name);

        if (names.empty()) {
            names.push_back("No Devices Found");
            ddDevice->setOptions(names);
        } else {
            ddDevice->setOptions(names);
            ddDevice->setSelection(0);
            doOpenSource(idx, cachedDevices[0].id, ddRate->selectedIndex, "");
        }
    };

    // Connect <-> Disconnect toggle
    btnConnect->onClick = [&]() {
        if (tcpConnected) {
            doDisconnectTcp();
            return;
        }
        std::string addr = txtHost->getText();
        if (addr.empty()) return;
        doOpenSource(4, addr, ddRate->selectedIndex, "");
    };

    // Enter in the host field only CONNECTS (it must never disconnect an active session)
    txtHost->onSubmit = [&](const std::string&) {
        if (!tcpConnected && btnConnect->onClick) btnConnect->onClick();
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
            if (tcpConnected && !tcpAddr.empty()) doOpenSource(4, tcpAddr, idx, "");
        } else if (ddSourceType->selectedIndex != 0 && !cachedDevices.empty()) {
            doOpenSource(ddSourceType->selectedIndex, cachedDevices[ddDevice->selectedIndex].id, idx, "");
        }
    };

    refreshSourceUI();

    auto modAudio = sidebar.addModule("Audio Output");
    auto ddAudio = std::make_shared<Dropdown>(SIDEBAR_W - 40, 25.0f, font);
    ddAudio->setOptions(deviceNames);
    ddAudio->onChange = [&](int idx) {
        audio.stop();
        audio.initDevice(idx, (int)AUDIO_RATE);
        if (sharedData.isPlaying) audio.start();
    };
    modAudio->addWidget(ddAudio);

    auto modDemod = sidebar.addModule("Demodulator");
    modDemod->addWidget(rowModes);

    slBW->onChange = [&](float v) {
        std::lock_guard<std::mutex> l(sharedData.mtx);
        sharedData.bandwidth = v;
        std::string txt = "Bandwidth: " + (v >= 1000 ? std::to_string((int)(v/1000)) + " kHz" : std::to_string((int)v) + " Hz");
        slBW->setText(txt);
    };
    slBW->onChange(12500.0f);
    modDemod->addWidget(slBW);

    auto slSq = std::make_shared<Slider>(SIDEBAR_W - 40, -100.0f, 0.0f, -100.0f, "Squelch (dB)", font);
    slSq->onChange = [&](float v) { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.squelchThreshold = v; };
    modDemod->addWidget(slSq);

    auto chkStereo = std::make_shared<Checkbox>("Stereo (WFM only)", font);
    chkStereo->onToggle = [&](bool b) { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.stereoEnabled = b; };
    modDemod->addWidget(chkStereo);

    auto chkRds = std::make_shared<Checkbox>("RDS (WFM only)", font);
    chkRds->onToggle = [&](bool b) { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.rdsEnabled = b; };
    modDemod->addWidget(chkRds);

    // --- Demodulator mode buttons (only for listening: the decoders have their own channels) ---
    auto setMode = [&](Mode m, SdrButton* me) {
        if (inputIsAudio && m != Mode::RAW && m != Mode::OFF) { m = Mode::RAW; me = btnRAW.get(); }

        rowModes->setEnabled(true);
        btnNFM->setActive(false); btnAM->setActive(false); btnWFM->setActive(false);
        btnUSB->setActive(false); btnLSB->setActive(false); btnRAW->setActive(false); btnOFF->setActive(false);
        me->setActive(true);
        if (inputIsAudio) {   // audio input: the other demodulators make no sense
            btnNFM->setEnabled(false); btnAM->setEnabled(false); btnWFM->setEnabled(false);
            btnUSB->setEnabled(false); btnLSB->setEnabled(false);
        }

        std::lock_guard<std::mutex> l(sharedData.mtx);
        sharedData.mode = m;

        if (m == Mode::WFM) {
            slBW->setLimits(50000, 200000); slBW->setValueSilent(180000);
            sharedData.bandwidth = 180000; slBW->setText("Bandwidth: 180 kHz");
        } else if (m == Mode::RAW) {
            slBW->setLimits(1000, 200000); slBW->setValueSilent(48000);
            sharedData.bandwidth = 48000; slBW->setText("Bandwidth: 48.0 kHz");
        } else if (m == Mode::NFM || m == Mode::AM) {
            slBW->setLimits(4000, 40000); slBW->setValueSilent(12500);
            sharedData.bandwidth = 12500; slBW->setText("Bandwidth: 12.5 kHz");
        } else if (m == Mode::OFF) {
        } else {
            slBW->setLimits(1000, 10000); slBW->setValueSilent(3000);
            sharedData.bandwidth = 3000; slBW->setText("Bandwidth: 3 kHz");
        }
    };

    btnNFM->onClick = [&](){ setMode(Mode::NFM, btnNFM.get()); };
    btnAM->onClick  = [&](){ setMode(Mode::AM, btnAM.get()); };
    btnWFM->onClick = [&](){ setMode(Mode::WFM, btnWFM.get()); };
    btnUSB->onClick = [&](){ setMode(Mode::USB, btnUSB.get()); };
    btnLSB->onClick = [&](){ setMode(Mode::LSB, btnLSB.get()); };
    btnRAW->onClick = [&](){ setMode(Mode::RAW, btnRAW.get()); };
    btnOFF->onClick = [&](){ setMode(Mode::OFF, btnOFF.get()); };

    auto restoreMode = [&](Mode m) {
        if (m == Mode::NFM) setMode(Mode::NFM, btnNFM.get());
        else if (m == Mode::AM) setMode(Mode::AM, btnAM.get());
        else if (m == Mode::WFM) setMode(Mode::WFM, btnWFM.get());
        else if (m == Mode::RAW) setMode(Mode::RAW, btnRAW.get());
        else if (m == Mode::USB) setMode(Mode::USB, btnUSB.get());
        else if (m == Mode::LSB) setMode(Mode::LSB, btnLSB.get());
        else setMode(Mode::OFF, btnOFF.get());
    };

    // --- Input signal switch (I/Q / Audio) ---
    Mode modeBeforeAudio = Mode::NFM;
    applyInputType = [&](bool audioIn) {
        btnInIQ->setActive(!audioIn);
        btnInAudio->setActive(audioIn);
        if (audioIn == inputIsAudio) return;
        Mode cur;
        { std::lock_guard<std::mutex> l(sharedData.mtx); cur = sharedData.mode; }
        if (audioIn) modeBeforeAudio = cur;
        inputIsAudio = audioIn;
        { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.audioInput = audioIn; }
        if (audioIn) setMode(Mode::RAW, btnRAW.get());
        else restoreMode(modeBeforeAudio);
    };

    // --- Decoders ---
    // Every decoder works the same way:
    //  - with I/Q input it listens to its own channel(s): the yellow bands with handles on the
    //    tuning bar (see modules/Core/DecoderChannels.h). It never changes the demodulator, so
    //    the tuner and the mode stay free for listening;
    //  - with audio input (Input signal switch) it decodes the input audio;
    //  - one decoder runs at a time.
    auto chkSSTV  = std::make_shared<Checkbox>("SSTV (slow-scan TV pictures)", font);
    auto chkWEFAX = std::make_shared<Checkbox>("WEFAX (HF weather fax)", font);
    auto chkISM   = std::make_shared<Checkbox>(ISMDecoder::available() ? "ISM (433/868 MHz sensors)"
                                                                       : "ISM (not in this build)", font);
    auto chkACARS = std::make_shared<Checkbox>("ACARS (aircraft, 131.x MHz)", font);

    struct DecoderEntry { std::shared_ptr<Checkbox> chk; SDRModule* mod; };   // mod == nullptr: APRS
    std::vector<DecoderEntry> decoders = {
        {chkAprs, nullptr}, {chkAIS, ptrAIS}, {chkSSTV, ptrSSTV},
        {chkWEFAX, ptrWEFAX}, {chkISM, ptrISM}, {chkACARS, ptrACARS} };

    auto activateDecoder = [&](size_t idx, bool on) {
        DecoderEntry& d = decoders[idx];
        std::lock_guard<std::mutex> l(sharedData.mtx);
        if (d.mod) {
            if (on) {
                if (d.mod->channelSet()) d.mod->channelSet()->clear();   // placed again for the current band
                sharedData.activeDecoder = d.mod;
                d.mod->enabled = true;
            } else {
                if (sharedData.activeDecoder == d.mod) sharedData.activeDecoder = nullptr;
                d.mod->enabled = false;
            }
        } else {
            if (on) aprsChannels().clear();
            sharedData.aprsEnabled = on;
        }
    };
    for (size_t i = 0; i < decoders.size(); i++) {
        decoders[i].chk->onToggle = [&, i](bool b) {
            if (b) {
                for (size_t j = 0; j < decoders.size(); j++) {
                    if (j != i && decoders[j].chk->checked) { decoders[j].chk->checked = false; activateDecoder(j, false); }
                }
            }
            activateDecoder(i, b);
        };
        modDecoders->addWidget(decoders[i].chk);
    }
    modDecoders->addWidget(std::make_shared<Label>("Each decoder has its own channel (yellow)", font, 11, sf::Color(150, 150, 160)));

    auto modDisp = sidebar.addModule("Display");
    auto slMinDb = std::make_shared<Slider>(SIDEBAR_W - 40, -140.0f, -20.0f, -90.0f, "Min dB", font);
    slMinDb->onChange = [&](float v) { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.minDb = v; };
    modDisp->addWidget(slMinDb);

    auto slMaxDb = std::make_shared<Slider>(SIDEBAR_W - 40, -60.0f, 40.0f, 0.0f, "Max dB", font);
    slMaxDb->onChange = [&](float v) { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.maxDb = v; };
    modDisp->addWidget(slMaxDb);

    auto slZoom = std::make_shared<Slider>(SIDEBAR_W - 40, 1.0f, 16.0f, 1.0f, "Zoom", font);
    slZoom->onChange = [&](float v) {
        std::lock_guard<std::mutex> l(sharedData.mtx);
        double oldZoom = sharedData.zoomLevel;
        double oldVisibleFraction = 1.0 / oldZoom;
        double oldViewStart = sharedData.viewCenterPct - oldVisibleFraction / 2.0;
        double tunerOffsetFromStart = sharedData.tunedFreqPercent - oldViewStart;
        double tunerPosRatio = tunerOffsetFromStart / oldVisibleFraction;

        sharedData.zoomLevel = v;

        double newVisibleFraction = 1.0 / v;
        double newViewStart = sharedData.tunedFreqPercent - (tunerPosRatio * newVisibleFraction);
        sharedData.viewCenterPct = newViewStart + newVisibleFraction / 2.0;

        if (sharedData.viewCenterPct - newVisibleFraction/2.0 < 0.0) sharedData.viewCenterPct = newVisibleFraction/2.0;
        if (sharedData.viewCenterPct + newVisibleFraction/2.0 > 1.0) sharedData.viewCenterPct = 1.0 - newVisibleFraction/2.0;
    };
    modDisp->addWidget(slZoom);

    auto ddTheme = std::make_shared<Dropdown>(SIDEBAR_W - 40, 25.0f, font);
    ddTheme->setOptions(THEME_NAMES);
    ddTheme->setSelection(0);
    ddTheme->onChange = [&](int idx) {
        std::lock_guard<std::mutex> l(sharedData.mtx);
        sharedData.waterfallTheme = idx;
        Theme::setTheme(idx);
        freqVFO.updateStyle();
        btnTuningMode.updateStyle();
        btnPlay.updateStyle();
        btnMute.updateStyle();
        volSlider.updateStyle();
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
    modDisp->addWidget(std::make_shared<Label>("UI Scale:", font));
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

    btnRecAudio->onClick = [&](){ btnRecAudio->setActive(true); btnRecIQ->setActive(false); std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.recMode = RecMode::AUDIO; };
    btnRecIQ->onClick = [&](){ btnRecAudio->setActive(false); btnRecIQ->setActive(true); std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.recMode = RecMode::BASEBAND; };
    rowRecMode->add(btnRecAudio);
    rowRecMode->add(btnRecIQ);
    modRec->addWidget(rowRecMode);

    auto lblRecPath = std::make_shared<Label>("Path: ...", font, 11, sf::Color(150,150,150));
    modRec->addWidget(lblRecPath);

    auto btnSetFolder = std::make_shared<SdrButton>(SIDEBAR_W - 40, 25, "Change Folder...", font);
    btnSetFolder->onClick = [&](){
        std::string p = selectFolderDialog();
        if(!p.empty()) {
            { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.recPath = p; }
            lblRecPath->setText("Path: " + truncatePath(p, 25));
        }
    };
    modRec->addWidget(btnSetFolder);

    auto btnRecToggle = std::make_shared<SdrButton>(SIDEBAR_W-40, 35, "START RECORDING", font);
    btnRecToggle->setColor(sf::Color(40, 40, 45));
    btnRecToggle->onClick = [&](){
        std::lock_guard<std::mutex> l(sharedData.mtx);
        sharedData.isRecording = !sharedData.isRecording;
        if(sharedData.isRecording) {
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

    // w / h are LOGICAL sizes (window pixels divided by the UI scale)
    auto updateLayout = [&](int w, int h) {
        layout.winW = (float)w;
        layout.winH = (float)h;
        layout.sidebarX = layout.winW - SIDEBAR_W;
        layout.specW = layout.sidebarX;
        layout.specH = 250.0f;
        layout.waterfallH = layout.winH - TOP_BAR_H - layout.specH - layout.tuneBarH;
        if (layout.waterfallH < 100) layout.waterfallH = 100;

        topBar.setSize({layout.winW, (float)TOP_BAR_H});
        freqVFO.setPosition(20, 4);
        btnTuningMode.setPosition(370, 10);
        btnPlay.setPosition(320, 10);
        volSlider.setPosition(layout.winW - 170, 10);
        btnMute.setPosition(layout.winW - 220, 10);
        sidebar.setGeometry(layout.sidebarX, TOP_BAR_H, layout.winH - TOP_BAR_H);
        timeSlider.setPosition(20, layout.winH - 30);
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

    // Restore saved scale before the first layout pass
    uiScaleIdx = std::clamp(settingsMgr.getInt("ui_scale", 0), 0, (int)UI_SCALE_VALUES.size() - 1);
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
    Mode mEnum = (Mode)savedMode;
    if (mEnum == Mode::NFM) setMode(Mode::NFM, btnNFM.get());
    else if (mEnum == Mode::AM) setMode(Mode::AM, btnAM.get());
    else if (mEnum == Mode::WFM) setMode(Mode::WFM, btnWFM.get());
    else if (mEnum == Mode::USB) setMode(Mode::USB, btnUSB.get());
    else if (mEnum == Mode::LSB) setMode(Mode::LSB, btnLSB.get());
    else if (mEnum == Mode::RAW) setMode(Mode::RAW, btnRAW.get());
    else setMode(Mode::OFF, btnOFF.get());

    float savedBw = settingsMgr.getFloat("bandwidth", 12500.0f);
    slBW->setValueSilent(savedBw); slBW->onChange(savedBw);

    float savedSq = settingsMgr.getFloat("squelch", -100.0f);
    slSq->setValueSilent(savedSq); slSq->onChange(savedSq);

    bool savedStereo = settingsMgr.getBool("stereo", false);
    chkStereo->checked = savedStereo;
    chkStereo->onToggle(savedStereo);

    ptrSSTV->applySettings(settingsMgr.getBool("sstv_auto_slant", true), settingsMgr.getFloat("sstv_slant", 0.0f),
                           settingsMgr.getBool("sstv_auto_save", false));
    ptrWEFAX->applySettings(settingsMgr.getInt("wefax_lpm", 120), settingsMgr.getInt("wefax_ioc", 576),
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

    hwState.fmNotch = settingsMgr.getBool("hw_fm_notch", false);
    hwState.mwNotch = settingsMgr.getBool("hw_mw_notch", false);
    hwState.biasT = settingsMgr.getBool("hw_bias_t", false);
    hwState.directSampling = settingsMgr.getInt("hw_direct_samp", 0);
    hwState.antennaIndex = settingsMgr.getInt("hw_antenna", 0);

    int savedSourceIdx = settingsMgr.getInt("source_type", 0);
    if (savedSourceIdx != 0) {
        ddSourceType->setSelection(savedSourceIdx);
        if (savedSourceIdx == 4) {
            currentSourceType = 4;
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
    { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.recPath = savedRecPath; }
    lblRecPath->setText("Path: " + truncatePath(savedRecPath, 25));

    sf::Clock debouncer;
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

    // Smoothed values of the signal meter in the status bar
    float meterSnr = -1.0f;
    double meterSignal = -127.0;
    sf::Clock meterClock;

    while (window.isOpen()) {
        sf::Vector2u currSize = window.getSize();
        if (currSize != lastSize && currSize.x > 0 && currSize.y > 0) {
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
            std::lock_guard<std::mutex> l(sourceMtx);
            if (currentSource) {
                sourceReconnecting = currentSource->isReconnecting();
                isHw = currentSource->isHardware();
                hwSampleRate = currentSource->getSampleRate();
                prog = currentSource->getProgress();
                autoGainDb = currentSource->getAutoGainDb();
                adcOverload = currentSource->getOverloadFraction();
                agcLimitedByRds = currentSource->isAgcLimited();
            }
        }

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
        if (tcpConnected) {
            bool lost = false;
            {
                std::lock_guard<std::mutex> l(sourceMtx);
                RtlTcpSource* t = dynamic_cast<RtlTcpSource*>(currentSource.get());
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
            std::lock_guard<std::mutex> l(sharedData.mtx);
            sharedData.volume = volumeGain(lastVolPos);
        }

        if (currentSourceType == 0) {
            timeSlider.update(window);
            if (timeSlider.isDragging) {
                std::lock_guard<std::mutex> l(sharedData.mtx);
                sharedData.pendingSeekRequest = timeSlider.currentVal;
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
            std::lock_guard<std::mutex> l(sharedData.mtx);
            aprsOn = sharedData.aprsEnabled;
            rdsOn = sharedData.rdsEnabled;
            rdsInfo = sharedData.rds;
            tunePct = sharedData.tunedFreqPercent;
            mode = sharedData.mode;
            currentZoom = sharedData.zoomLevel;
            currentTheme = sharedData.waterfallTheme;
            viewCenter = sharedData.viewCenterPct;
        }

        slZoom->update(window);

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

        // --- Decoder channels: yellow bands + handles on the tuning bar ---
        ChannelSet* chSet = nullptr;
        {
            std::lock_guard<std::mutex> l(sharedData.mtx);
            if (!inputIsAudio) {
                if (sharedData.aprsEnabled) chSet = &aprsChannels();
                else if (sharedData.activeDecoder && sharedData.activeDecoder->enabled) chSet = sharedData.activeDecoder->channelSet();
            }
        }
        if (chDrag.set && chDrag.set != chSet) chDrag = ChannelDrag();   // decoder switched off while dragging
        if (chEdge.set && chEdge.set != chSet) chEdge = ChannelEdgeDrag();
        struct ChHandle { DecoderChannel ch; float x; float rawX; int edge; };
        std::vector<ChHandle> chHandles;
        const double vfoOffsetHz = (tunePct - 0.5) * hwSampleRate;
        const long long vfoFreqHz = currentCenterFreq + (long long)std::llround(vfoOffsetHz);
        auto offsetToX = [&](double off) {
            return (float)(((0.5 + off / hwSampleRate) - viewStartPct) / viewWidthPct * layout.specW);
        };
        auto xToFreq = [&](float x) {
            const double pct = viewStartPct + (x / layout.specW) * viewWidthPct;
            return currentCenterFreq + (long long)std::llround((pct - 0.5) * hwSampleRate);
        };
        const float tbTop = (float)TOP_BAR_H + layout.specH, tbBot = tbTop + layout.tuneBarH;
        if (chSet) {
            for (const DecoderChannel& c : chSet->snapshot()) {
                const double off = c.followVfo ? vfoOffsetHz : (double)(c.freqHz - currentCenterFreq);
                const float x = offsetToX(off);
                const int edge = x < 0.0f ? -1 : (x > layout.specW ? 1 : 0);
                chHandles.push_back({c, std::clamp(x, 10.0f, layout.specW - 10.0f), x, edge});
            }
        }
        auto handleAt = [&](sf::Vector2f m) -> int {
            if (m.y < tbTop - 3.0f || m.y > tbBot + 3.0f || m.x > layout.specW) return -1;
            int best = -1;
            float bestD = 10.0f;
            for (size_t i = 0; i < chHandles.size(); i++) {
                const float d = std::abs(chHandles[i].x - m.x);
                if (d < bestD) { bestD = d; best = (int)i; }
            }
            return best;
        };
        // Edges of the main tuner's band (grey) in pixels
        float tunerEdgeL = visualCenterX - bwPixels / 2.0f, tunerEdgeR = visualCenterX + bwPixels / 2.0f;
        if (mode == Mode::USB) { tunerEdgeL = visualCenterX; tunerEdgeR = visualCenterX + bwPixels; }
        if (mode == Mode::LSB) { tunerEdgeL = visualCenterX - bwPixels; tunerEdgeR = visualCenterX; }
        auto chCentreOffset = [&](const DecoderChannel& c) {
            return c.followVfo ? vfoOffsetHz : (double)(c.freqHz - currentCenterFreq);
        };
        // A draggable edge of a yellow band on the spectrum: index into chHandles, or -1.
        // Bands whose width is the tuner's have no own edges (the grey band's edges change both).
        // SSB channels: only the edge away from the dial frequency moves.
        auto edgeAt = [&](sf::Vector2f m) -> int {
            if (!chSet || !chSet->resizable || m.y < (float)TOP_BAR_H || m.y > (float)TOP_BAR_H + layout.specH || m.x > layout.specW) return -1;
            int best = -1;
            float bestD = 6.0f;
            for (size_t i = 0; i < chHandles.size(); i++) {
                const DecoderChannel& c = chHandles[i].ch;
                if (chHandles[i].edge != 0 || chSet->widthFollowsTuner(c)) continue;
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
            for (const ChHandle& ch : chHandles) if (ch.ch.id == chEdge.id) h = &ch;
            if (!h) return;
            const DecoderChannel& c = h->ch;
            const double hzPerPx = (hwSampleRate * viewWidthPct) / layout.specW;
            const float cx = offsetToX(chCentreOffset(c));
            double w = c.sideband > 0 ? (x - cx) * hzPerPx
                     : c.sideband < 0 ? (cx - x) * hzPerPx
                                      : 2.0 * std::abs(x - cx) * hzPerPx;
            // on the grey band's edge: the tuner's width (only for channels that may follow it)
            chEdge.snapTuner = c.followVfo && chSet->followTunerWidth &&
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
            if (isHw && (f < currentCenterFreq - halfBW || f > currentCenterFreq + halfBW)) {
                currentCenterFreq = f;
                {
                    std::lock_guard<std::mutex> l(sharedData.mtx);
                    sharedData.centerFreq = f;
                    sharedData.tunedFreqPercent = 0.5;
                    sharedData.viewCenterPct = 0.5;
                }
                pendingCenterFreq = f;
                debouncer.restart();
            } else {
                std::lock_guard<std::mutex> l(sharedData.mtx);
                sharedData.tunedFreqPercent = std::clamp(0.5 + (double)(f - currentCenterFreq) / hwSampleRate, 0.0, 1.0);
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
            if (chDrag.snap == 0 && currentCenterFreq > 0) {
                for (long long pf : chDrag.set->presets) {
                    const float px = offsetToX((double)(pf - currentCenterFreq));
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
                    if (h.ch.id != chDrag.id) continue;
                    if (!h.ch.followVfo) tuneVfoTo(h.ch.freqHz);
                    if (!inputIsAudio) restoreMode(h.ch.listenMode);
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
                std::lock_guard<std::mutex> l(sourceMtx);
                if (currentSource) {
                    isHwLocal = currentSource->isHardware();
                    sr = currentSource->getSampleRate();
                }
            }

            double clickViewPct = mouseX / layout.specW;
            double clickedAbsolutePct = viewStartPct + clickViewPct * viewWidthPct;

            double offsetHz = (clickedAbsolutePct - 0.5) * sr;
            long long clickedFreq = currentCenterFreq + (long long)offsetHz;

            long long step = STEP_VALUES[ddSnap->selectedIndex];
            if (step > 0) {
                clickedFreq = (long long)std::round((double)clickedFreq / step) * step;
                double newOffset = (double)(clickedFreq - currentCenterFreq);
                clickedAbsolutePct = 0.5 + (newOffset / sr);
            }

            if (stickyCenterMode && isHwLocal) {
                pendingCenterFreq = clickedFreq;
                debouncer.restart();
                {
                    std::lock_guard<std::mutex> l(sharedData.mtx);
                    sharedData.tunedFreqPercent = 0.5;
                    sharedData.viewCenterPct = 0.5;
                }
                freqVFO.setFrequency(clickedFreq);
            } else {
                {
                    std::lock_guard<std::mutex> l(sharedData.mtx);
                    sharedData.tunedFreqPercent = clickedAbsolutePct;
                }
                freqVFO.setFrequency(clickedFreq);
            }
        };

        if (pendingRfGain > -900.0f && gainDebouncer.getElapsedTime().asMilliseconds() > GAIN_LATENCY_MS) {
            {
                std::lock_guard<std::mutex> l(sourceMtx);
                if (currentSource && currentSource->isHardware()) {
                    currentSource->setGain((int)pendingRfGain);
                }
            }
            pendingRfGain = -999.0f;
            gainDebouncer.restart();
        }

        // Decoder panel at the bottom of the waterfall. With a file source the timeline
        // stays below it, so it can still be used.
        const float timelineStripH = (currentSourceType == 0) ? 40.0f : 0.0f;
        float decoderPanelTop = layout.winH;   // winH = no panel
        {
            SDRModule* dm = nullptr;
            { std::lock_guard<std::mutex> l(sharedData.mtx); dm = sharedData.activeDecoder; }
            if (dm && dm->enabled) decoderPanelTop = layout.winH - dm->panelHeight(layout.winH) - timelineStripH;
        }

        while (const std::optional<sf::Event> ev = window.pollEvent()) {
            if (ev->is<sf::Event::Closed>()) window.close();

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

            // Popup window of the active decoder (message details): it takes every event
            {
                SDRModule* modalMod = nullptr;
                { std::lock_guard<std::mutex> l(sharedData.mtx); modalMod = sharedData.activeDecoder; }
                if (modalMod && modalMod->enabled && modalMod->modalOpen()) {
                    modalMod->handleEvent(*ev, window);
                    continue;
                }
            }

            if (sidebar.handleEvent(*ev, window)) continue;

            // Decoder panel at the bottom of the waterfall (SSTV / WEFAX controls, picture scrolling)
            {
                SDRModule* panelMod = nullptr;
                { std::lock_guard<std::mutex> l(sharedData.mtx); panelMod = sharedData.activeDecoder; }
                if (panelMod && panelMod->enabled && panelMod->handleEvent(*ev, window)) continue;
            }

            // Decoder channel handles on the tuning bar: take the press before the scale drag
            if (const auto* mbp = ev->getIf<sf::Event::MouseButtonPressed>()) {
                if (mbp->button == sf::Mouse::Button::Left) {
                    const sf::Vector2f m = window.mapPixelToCoords(sf::Mouse::getPosition(window));
                    const int e = edgeAt(m);
                    if (e >= 0 && chSet) {
                        chEdge = ChannelEdgeDrag();
                        chEdge.set = chSet;
                        chEdge.id = chHandles[e].ch.id;
                        chEdge.x = m.x;
                        chEdge.widthHz = chHandles[e].ch.widthHz;
                        continue;
                    }
                    const int h = handleAt(m);
                    if (h >= 0 && chSet) {
                        chDrag = ChannelDrag();
                        chDrag.set = chSet;
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
            if (currentSourceType == 0 && timeSlider.handleEvent(*ev, window)) topBarHandled = true;

            if (btnMute.isClicked(*ev, window)) {
                static bool m = false;
                m = !m;
                if(m) btnMute.setActive(true); else btnMute.setActive(false);
                std::lock_guard<std::mutex> l(sharedData.mtx);
                sharedData.isMuted = m;
                topBarHandled = true;
            }

            if (btnPlay.isClicked(*ev, window)) {
                bool s;
                {
                    std::lock_guard<std::mutex> l(sharedData.mtx);
                    sharedData.isPlaying = !sharedData.isPlaying;
                    s = sharedData.isPlaying;
                }
                if (s) {
                    btnPlay.setText("||"); btnPlay.setActive(true);
                    audio.start();
                    { std::lock_guard<std::mutex> l(sourceMtx); if (currentSource) currentSource->start(); }
                } else {
                    btnPlay.setText(">"); btnPlay.setActive(false);
                    audio.stop();
                    { std::lock_guard<std::mutex> l(sourceMtx); if (currentSource) currentSource->stop(); }
                }
                topBarHandled = true;
            }

            if (isHw && btnTuningMode.isClicked(*ev, window)) {
                stickyCenterMode = !stickyCenterMode;
                if(stickyCenterMode) {
                    btnTuningMode.setText("CTR"); btnTuningMode.setActive(true);
                    { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.tunedFreqPercent = 0.5; }
                    pendingCenterFreq = freqVFO.getFrequency();
                    debouncer.restart();
                } else {
                    btnTuningMode.setText("FIX"); btnTuningMode.setActive(false);
                }
                topBarHandled = true;
            }

            if (freqVFO.handleEvent(*ev)) {
                topBarHandled = true;
                long long targetVFO = freqVFO.getFrequency();
                double halfBW = hwSampleRate / 2.0;
                double minF = (double)currentCenterFreq - halfBW;
                double maxF = (double)currentCenterFreq + halfBW;

                if (isHw && (stickyCenterMode || targetVFO < minF || targetVFO > maxF)) {
                    currentCenterFreq = targetVFO;
                    {
                        std::lock_guard<std::mutex> l(sharedData.mtx);
                        sharedData.centerFreq = targetVFO;
                        sharedData.tunedFreqPercent = 0.5;
                        sharedData.viewCenterPct = 0.5;
                    }
                    pendingCenterFreq = targetVFO;
                    debouncer.restart();
                } else {
                    if (!isHw) {
                        if (targetVFO > maxF) targetVFO = (long long)maxF;
                        if (targetVFO < minF) targetVFO = (long long)minF;
                    }
                    double newOffset = (double)(targetVFO - currentCenterFreq);
                    {
                        std::lock_guard<std::mutex> l(sharedData.mtx);
                        sharedData.tunedFreqPercent = 0.5 + (newOffset / hwSampleRate);
                    }
                }
                if (targetVFO != freqVFO.getFrequency()) freqVFO.setFrequency(targetVFO);
            }

            if (topBarHandled) continue;

            bool overAprs = (aprsOn && window.mapPixelToCoords(sf::Mouse::getPosition(window)).y > (layout.winH - 200));

            if (const auto* scroll = ev->getIf<sf::Event::MouseWheelScrolled>()) {
                if (overAprs) {
                    aprsLogScrollOffset += scroll->delta * 20.0f;
                    if (aprsLogScrollOffset > 0.0f) aprsLogScrollOffset = 0.0f;
                } else {
                    sf::Vector2f m = window.mapPixelToCoords(sf::Vector2i((int)scroll->position.x, (int)scroll->position.y));
                    if (m.y > TOP_BAR_H && m.x < layout.specW) {
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

                        if (stickyCenterMode && isHw) {
                            currentCenterFreq = rawNext;
                            pendingCenterFreq = rawNext;
                            debouncer.restart();
                            {
                                std::lock_guard<std::mutex> l(sharedData.mtx);
                                sharedData.centerFreq = rawNext;
                                sharedData.tunedFreqPercent = 0.5;
                                sharedData.viewCenterPct = 0.5;
                            }
                        } else {
                            double newOffset = (double)(rawNext - currentCenterFreq);
                            double clickPct = 0.5 + (newOffset / hwSampleRate);
                            clickPct = std::clamp(clickPct, 0.0, 1.0);
                            {
                                std::lock_guard<std::mutex> l(sharedData.mtx);
                                sharedData.tunedFreqPercent = clickPct;
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
                        float overlayY = layout.winH - 200.0f;
                        float listTopY = overlayY + 25.0f;
                        if (m.x > logX) {
                            float relY = m.y - listTopY - aprsLogScrollOffset;
                            int index = (int)(relY / 16.0f);
                            std::lock_guard<std::mutex> l(sharedData.mtx);
                            if (index >= 0 && index < sharedData.aprsHistory.size()) {
                                selectedAprsPacket = sharedData.aprsHistory[index];
                                showAprsModal = true;
                            }
                        }
                    }

                    bool overTimeline = (currentSourceType == 0) && (timeSlider.isDragging || (m.y > layout.winH - 40));
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
                    std::lock_guard<std::mutex> l(sharedData.mtx);
                    sharedData.viewCenterPct += pctShift;
                    if (sharedData.viewCenterPct - visibleFraction/2.0 < 0.0) sharedData.viewCenterPct = visibleFraction/2.0;
                    if (sharedData.viewCenterPct + visibleFraction/2.0 > 1.0) sharedData.viewCenterPct = 1.0 - visibleFraction/2.0;
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
                        long long nextCenter = currentCenterFreq + shift;
                        if (nextCenter < 0) nextCenter = 0;
                        currentCenterFreq = nextCenter;
                        {
                            std::lock_guard<std::mutex> l(sharedData.mtx);
                            sharedData.centerFreq = nextCenter;
                        }
                        pendingCenterFreq = nextCenter;
                    } else {
                        double pctShift = (dx / layout.specW) * viewWidthPct;
                        std::lock_guard<std::mutex> l(sharedData.mtx);
                        sharedData.viewCenterPct = std::clamp(sharedData.viewCenterPct + pctShift, visibleFraction/2.0, 1.0 - visibleFraction/2.0);
                    }
                }

                const bool overDecoderPanel = (m.y >= decoderPanelTop && m.y < layout.winH - timelineStripH);
                if (!overAprs && !overDecoderPanel && m.x >= 0 && m.x < layout.specW && m.y >= TOP_BAR_H && m.y < (TOP_BAR_H + layout.specH + layout.waterfallH)) {
                    std::lock_guard<std::mutex> l(sharedData.mtx);
                    sharedData.mouseX_spectrum = m.x;
                    sharedData.mouseY_spectrum = m.y - TOP_BAR_H;
                } else {
                    std::lock_guard<std::mutex> l(sharedData.mtx);
                    sharedData.mouseX_spectrum = -1.0f;
                }
            }
        } // end of pollEvent loop

        if (pendingCenterFreq != 0 && debouncer.getElapsedTime().asMilliseconds() > TUNING_LATENCY_MS) {
            std::lock_guard<std::mutex> l(sourceMtx);
            if (currentSource && currentSource->isHardware()) {
                currentSource->setCenterFrequency(pendingCenterFreq);
                currentCenterFreq = pendingCenterFreq;
            }
            {
                std::lock_guard<std::mutex> l(sharedData.mtx);
                sharedData.centerFreq = pendingCenterFreq;
            }
            pendingCenterFreq = 0;
            debouncer.restart();
        }

        sidebar.update(window);

        bool showHand = false;
        sf::Vector2f mousePos = window.mapPixelToCoords(sf::Mouse::getPosition(window));

        if (sidebar.isMouseOver(window)) {
            if (sidebar.isAnyWidgetHovered(window)) showHand = true;
        } else {
            if (freqVFO.isHovered) showHand = true;
            if (mousePos.y >= TOP_BAR_H + layout.specH - 10 && mousePos.y <= TOP_BAR_H + layout.specH + 20 && mousePos.x < layout.specW) showHand = true;
            if (volSlider.isMouseOver(window) || (currentSourceType == 0 && timeSlider.isMouseOver(window))) showHand = true;
            if (btnPlay.isMouseOver(window) || btnMute.isMouseOver(window) || btnTuningMode.isMouseOver(window)) showHand = true;
            if (aprsOn && mousePos.y > (layout.winH - 200) && mousePos.x < layout.specW) showHand = true;
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
            std::lock_guard<std::mutex> l(sharedData.mtx);
            if (sharedData.newInspectorData) {
                newRow = sharedData.inspectorWaterfallRow;
                sharedData.newInspectorData = false;
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
            std::lock_guard<std::mutex> lock(sharedData.mtx);
            spectrum = sharedData.fftSpectrum;
            if (sharedData.newWaterfallData) {
                row = sharedData.waterfallRow;
                sharedData.newWaterfallData = false;
                newRow2 = true;
            }
        }

        if (newRow2) {
            currentWaterfallY = (currentWaterfallY - 1 + WATERFALL_HEIGHT_PX) % WATERFALL_HEIGHT_PX;
            wTex.update(row.data(), sf::Vector2u(INTERNAL_WATERFALL_WIDTH, 1), sf::Vector2u(0, (unsigned int)currentWaterfallY));
        }

        window.clear(sf::Color::Black);

        long long cf = 0;
        if (currentSource) cf = currentCenterFreq;

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
            std::lock_guard<std::mutex> l(sharedData.mtx);
            mx = sharedData.mouseX_spectrum;
            my = sharedData.mouseY_spectrum;
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

        // --- Decoder channels: yellow bands (spectrum, waterfall) and handles (tuning bar) ---
        if (!chHandles.empty()) {
            const float specTop = (float)TOP_BAR_H;
            const float specBot = specTop + (float)layout.specH;
            const float wfTop = tbBot;
            const float wfBot = wfTop + (float)layout.waterfallH;
            const sf::Color yellow(255, 210, 0);
            const sf::Color edgeCol(255, 210, 0, 210), fillCol(255, 210, 0, 38), wfEdgeCol(255, 210, 0, 110);
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

            for (size_t i = 0; i < chHandles.size(); i++) {
                const ChHandle& h = chHandles[i];
                const DecoderChannel& c = h.ch;
                const bool dragging = (chDrag.set && chDrag.id == c.id) || (chEdge.set && chEdge.id == c.id);
                const double off = c.followVfo ? vfoOffsetHz : (double)(c.freqHz - currentCenterFreq);
                const std::string title = c.name + (c.followVfo ? "  (tuner)" : "  " + mhzText(c.freqHz));

                if (h.edge == 0) {
                    // band on the spectrum, edges on the waterfall
                    float x0 = offsetToX(c.lowOffset(off)), x1 = offsetToX(c.highOffset(off));
                    if (x1 - x0 < 2.0f) { const float m = (x0 + x1) / 2.0f; x0 = m - 1.0f; x1 = m + 1.0f; }
                    const float cx0 = std::max(0.0f, x0), cx1 = std::min((float)layout.specW, x1);
                    if (cx1 > cx0) {
                        sf::RectangleShape band({cx1 - cx0, specBot - specTop});
                        band.setPosition({cx0, specTop});
                        band.setFillColor(dragging ? sf::Color(255, 210, 0, 70) : fillCol);
                        window.draw(band);
                    }
                    for (float x : {x0, x1}) {
                        if (x < 0.0f || x > layout.specW) continue;
                        vline(x, specTop, specBot, edgeCol);
                        vline(x, wfTop, wfBot, wfEdgeCol);
                    }
                    label(title, std::max(2.0f, x0 + 3.0f), specTop + 3.0f + 12.0f * (float)(i % 2), edgeCol);

                    // handle: a yellow grip on the tuning bar at the channel frequency
                    const float hw = 12.0f, hh = layout.tuneBarH - 6.0f;
                    sf::RectangleShape grip({hw, hh});
                    grip.setPosition({std::round(h.x - hw / 2.0f), tbTop + 3.0f});
                    grip.setFillColor((dragging || (int)i == hovered) ? sf::Color(255, 235, 120) : yellow);
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
                    arrow.setFillColor((int)i == hovered ? sf::Color(255, 235, 120) : yellow);
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
                const std::string tip = h.ch.name + ":  drag = move,  drop on the tuner = follow it,  click = listen";
                label(tip, std::clamp(h.x - 60.0f, 4.0f, layout.specW - 380.0f), tbTop - 15.0f, sf::Color(255, 235, 120), 22);
            } else if (hoveredEdge >= 0) {
                const std::string tip = chHandles[hoveredEdge].ch.name + ":  drag the edge = channel width" +
                    std::string(chSet->followTunerWidth && chHandles[hoveredEdge].ch.followVfo ? ",  drop on the grey edge = the tuner's width" : "");
                label(tip, std::clamp(mousePos.x + 10.0f, 4.0f, layout.specW - 420.0f), specTop + 28.0f, sf::Color(255, 235, 120), 22);
            }
        }

        // --- RDS PANEL (top right corner of the spectrum) ---
        if (rdsOn && mode == Mode::WFM && currentSource) {
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
            float overlayY = layout.winH - aprsH;

            sf::RectangleShape bg({layout.specW, aprsH});
            bg.setPosition({0, overlayY});
            bg.setFillColor(sf::Color(20, 20, 25, 240));
            bg.setOutlineColor(sf::Color::White);
            bg.setOutlineThickness(1);
            window.draw(bg);

            AprsLastPacket pkt;
            std::deque<AprsLastPacket> logs;
            {
                std::lock_guard<std::mutex> l(sharedData.mtx);
                pkt = sharedData.lastAprs;
                logs = sharedData.aprsHistory;
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
                const std::string chText = inputIsAudio ? std::string("Input: audio") : aprsChannels().describe();
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

        SDRModule* decoderUI = nullptr;
        {
            std::lock_guard<std::mutex> l(sharedData.mtx);
            decoderUI = sharedData.activeDecoder;
        }

        if (decoderUI && decoderUI->enabled) {
            float tableH = decoderUI->panelHeight(layout.winH);
            float tableY = layout.winH - tableH - timelineStripH;
            if (timelineStripH > 0.0f) {   // background for the timeline below the panel
                sf::RectangleShape strip({layout.specW, timelineStripH});
                strip.setPosition({0, layout.winH - timelineStripH});
                strip.setFillColor(sf::Color(30, 30, 35));
                window.draw(strip);
            }
            decoderUI->draw(window, 0, tableY, layout.specW, tableH);
        }
        else {
            // --- STATUS BAR WITH S-METER (hidden while decoders are running) ---
            float statusH = 40.0f;
            float statusY = layout.winH - statusH;

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

            drawLED("PLAY", sharedData.isPlaying, Theme::Accent);
            drawLED("REC", sharedData.isRecording, sf::Color(255, 50, 50));
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

        if (currentSourceType == 0) timeSlider.draw(window);
        sidebar.draw(window);

        window.draw(topBar);
        freqVFO.draw(window);
        btnPlay.draw(window);
        if (isHw) btnTuningMode.draw(window);
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


        if (modInspector->isOpen) {
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

        // Popup window of the active decoder, above everything else
        {
            SDRModule* modalMod = nullptr;
            { std::lock_guard<std::mutex> l(sharedData.mtx); modalMod = sharedData.activeDecoder; }
            if (modalMod && modalMod->enabled && modalMod->modalOpen()) modalMod->drawModal(window);
        }
        window.display();
    }

    SettingsManager saveMgr;
    for (auto& mod : sidebar.modules) saveMgr.set("sidebar_" + mod->title, mod->isOpen);

    saveMgr.set("theme", ddTheme->selectedIndex);
    saveMgr.set("ui_scale", uiScaleIdx);
    saveMgr.set("zoom", slZoom->currentVal);
    saveMgr.set("min_db", slMinDb->currentVal);
    saveMgr.set("max_db", slMaxDb->currentVal);
    saveMgr.set("step_index", ddSnap->selectedIndex);
    saveMgr.set("mode_index", (int)sharedData.mode);
    saveMgr.set("bandwidth", slBW->currentVal);
    saveMgr.set("squelch", slSq->currentVal);
    saveMgr.set("stereo", chkStereo->checked);
    saveMgr.set("rds", chkRds->checked);
    saveMgr.set("picture_path", pictureDirCustom);
    saveMgr.set("volume_pos", volSlider.currentVal);
    saveMgr.set("sstv_auto_slant", ptrSSTV->getAutoSlant());
    saveMgr.set("sstv_slant", ptrSSTV->getSlantPpm());
    saveMgr.set("sstv_auto_save", ptrSSTV->getAutoSave());
    saveMgr.set("wefax_lpm", ptrWEFAX->getLpm());
    saveMgr.set("wefax_ioc", ptrWEFAX->getIoc());
    saveMgr.set("wefax_apt", ptrWEFAX->getAutoApt());
    saveMgr.set("wefax_slant", ptrWEFAX->getSlantPpm());
    saveMgr.set("wefax_auto_save", ptrWEFAX->getAutoSave());
    saveMgr.set("rf_gain", sharedData.rfGain);
    saveMgr.set("source_type", ddSourceType->selectedIndex);
    saveMgr.set("hw_fm_notch", hwState.fmNotch);
    saveMgr.set("hw_mw_notch", hwState.mwNotch);
    saveMgr.set("hw_bias_t", hwState.biasT);
    saveMgr.set("hw_direct_samp", hwState.directSampling);
    saveMgr.set("hw_antenna", hwState.antennaIndex);
    saveMgr.set("rtltcp_host", txtHost->getText());

    {
        std::lock_guard<std::mutex> l(sharedData.mtx);
        saveMgr.set("rec_path", sharedData.recPath);
    }

    saveMgr.set("frequency", (double)currentCenterFreq);

    saveMgr.save(settingsPath);

    dspRunning = false;
    if (dspThread.joinable()) dspThread.join();

    // Release the radio while everything is still alive. Leaving it to the destructor of
    // the global source would run after main() returns, when the driver libraries may
    // already be shutting down; the SDRplay API service doesn't cope well with a client
    // that disappears in the middle of a stream.
    {
        std::lock_guard<std::mutex> l(sourceMtx);
        if (currentSource) {
            currentSource->stop();
            currentSource->close();
            currentSource = nullptr;
        }
    }

    return 0;
}
