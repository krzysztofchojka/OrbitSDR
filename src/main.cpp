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

// Extracted Modules
#include "Globals.h"
#include "Utils.h"
#include "WavWriter.h"
#include "InspectorDSP.h"
#include "DSPWorker.h"

// Custom Decoders
#include "modules/Core/SDRModule.h"
#include "modules/AIS/AIS_Decoder.h"

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
    sf::RenderWindow window(sf::VideoMode::getDesktopMode(), "OrbitSDR", sf::Style::Default, sf::State::Windowed, settings);
    window.setFramerateLimit(60);

    sf::Image icon;
    if (!icon.loadFromFile("icon.png") && !icon.loadFromFile("../Resources/icon.png")) {}
    else {
        window.setIcon(icon);
    }

    sf::Font font;
    if (!font.openFromFile("/System/Library/Fonts/Helvetica.ttc") && !font.openFromFile("C:/Windows/Fonts/arial.ttf") && !font.openFromFile("arial.ttf")) {
        std::cerr << "Font not found!\n";
    }

    std::vector<std::unique_ptr<SDRModule>> modules;
    auto aisMod = std::make_unique<AISDecoder>();
    ModuleContext ctx = { (float)AUDIO_RATE, font };
    aisMod->init(ctx);
    modules.push_back(std::move(aisMod));
    AISDecoder* ptrAIS = (AISDecoder*)modules[0].get();

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

    Slider volSlider(150, 0.0f, 4.0f, 1.0f, "Volume", font);
    Slider timeSlider(100, 0.0f, 1.0f, 0.0f, "Timeline", font);

    Sidebar sidebar(SIDEBAR_W, font);

    auto modDecoders = sidebar.addModule("Decoders");
    auto chkAprs = std::make_shared<Checkbox>("Enable APRS (144.800)", font);
    auto btnCopyAprs = std::make_shared<SdrButton>(200, 25, "Copy Last Packet", font);
    auto chkAIS = std::make_shared<Checkbox>("Enable AIS (162.025)", font);

    auto btnNFM = std::make_shared<SdrButton>(36, 25, "NFM", font);
    auto btnAM = std::make_shared<SdrButton>(36, 25, "AM", font);
    auto btnWFM = std::make_shared<SdrButton>(36, 25, "WFM", font);
    auto btnUSB = std::make_shared<SdrButton>(36, 25, "USB", font);
    auto btnLSB = std::make_shared<SdrButton>(36, 25, "LSB", font);
    auto btnRAW = std::make_shared<SdrButton>(42, 25, "RAW", font);
    auto btnOFF = std::make_shared<SdrButton>(36, 25, "OFF", font);

    auto rowModes = std::make_shared<RowContainer>();
    rowModes->add(btnNFM);
    rowModes->add(btnAM);
    rowModes->add(btnWFM);
    rowModes->add(btnUSB);
    rowModes->add(btnLSB);
    rowModes->add(btnRAW);
    rowModes->add(btnOFF);
    auto slBW = std::make_shared<Slider>(SIDEBAR_W - 40, 1000.0f, 200000.0f, 12500.0f, "Bandwidth (Hz)", font);

    auto modSource = sidebar.addModule("Source / Input");
    auto ddSourceType = std::make_shared<Dropdown>(SIDEBAR_W - 40, 25.0f, font);
    ddSourceType->setOptions({"File Source", "RTL-SDR", "SDRPlay", "Sound Card (Line-In)"});
    modSource->addWidget(std::make_shared<Label>("Source Type:", font));
    modSource->addWidget(ddSourceType);

    auto lblDevice = std::make_shared<Label>("Select Device:", font);
    auto ddDevice = std::make_shared<Dropdown>(SIDEBAR_W - 40, 25.0f, font);
    auto btnRefresh = std::make_shared<SdrButton>(SIDEBAR_W - 40, 25, "Refresh Device List", font);

    auto ddRate = std::make_shared<Dropdown>(SIDEBAR_W - 40, 25.0f, font);
    ddRate->setOptions({"None"});

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
        if (currentSourceType == 1) {
            ss << "RF Gain: " << v << " dB";
        } else {
            ss << "RF Gain (dB): " << (int)v;
        }
        slGain->setText(ss.str());
        pendingRfGain = v;
    };

    chkAgc->onToggle = [&](bool b) {
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
        if (sourceIdx == 1) {
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

    std::function<void(int, std::string, int, std::string)> doOpenSource = [&](int sourceIdx, std::string deviceID, int rateIdx, std::string pathOverride) {
        {
            std::lock_guard<std::mutex> lock(sourceMtx);
            if (currentSource) {
                currentSource->stop();
                currentSource->close();
                currentSource = nullptr;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        std::shared_ptr<IQSource> newSource;
        uint32_t targetRate = 0;
        bool success = false;
        currentSourceType = sourceIdx;

        updateRadioControls(sourceIdx);

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
                try {
                    f = std::stoll(match[1]);
                } catch(...) {}
            }
            currentCenterFreq = f;
            freqVFO.setFrequency(f);
            {
                std::lock_guard<std::mutex> l(sharedData.mtx);
                sharedData.centerFreq = f;
            }

        } else if (sourceIdx == 1) {
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

        } else if (sourceIdx == 2) {
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

        } else if (sourceIdx == 3) {
            targetRate = 48000;
            auto audioIn = std::make_shared<AudioCaptureSource>();
            ddRate->setOptions(audioIn->getAvailableSampleRatesText());
            ddRate->setSelection(0);
            success = audioIn->open(deviceID, targetRate);
            newSource = audioIn;

            currentCenterFreq = 0;
            freqVFO.setFrequency(currentCenterFreq);
            {
                std::lock_guard<std::mutex> l(sharedData.mtx);
                sharedData.tunedFreqPercent = 0.5;
                sharedData.viewCenterPct = 0.5;
                sharedData.centerFreq = 0;
            }
            slGain->setEnabled(false);
            chkAgc->checked = false;
        }

        if (!success && sourceIdx != 0) {
            showPopup("Error", "Could not open device.");
            return;
        }

        {
            std::lock_guard<std::mutex> lock(sourceMtx);
            currentSource = newSource;
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

    std::vector<SDRDeviceItem> cachedDevices;

    std::function<void()> refreshSourceUI = [&]() {
        modSource->widgets.clear();
        modSource->addWidget(std::make_shared<Label>("Source Type:", font));
        modSource->addWidget(ddSourceType);

        if (ddSourceType->selectedIndex == 0) {
            modSource->addWidget(std::make_shared<Label>("Loaded File:", font));
            std::string fname;
            {
                std::lock_guard<std::mutex> l(sharedData.mtx);
                fname = sharedData.currentFilename;
            }
            if (fname == "None" || fname.empty()) {
                ddRate->setOptions({"[ Select File... ]"});
            } else {
                std::string shortName = fname.substr(fname.find_last_of("/\\") + 1);
                ddRate->setOptions({shortName, "[ Change File... ]"});
            }
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

    ddSourceType->onChange = [&](int idx) {
        if (idx == 0) {
            std::string path = openFileDialog();
            if (!path.empty()) {
                doOpenSource(0, "", 0, path);
                refreshSourceUI();
            } else refreshSourceUI();
        } else {
            cachedDevices.clear();
            std::vector<std::string> names;
            if (idx == 1) cachedDevices = RtlSdrSource::getDeviceList();
            else if (idx == 2) cachedDevices = SdrPlaySource::getDeviceList();
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
        else if (idx == 2) cachedDevices = SdrPlaySource::getDeviceList();
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

    ddRate->onChange = [&](int idx) {
        if (ddSourceType->selectedIndex == 0) {
            std::string currentOpt = "";
            if(idx >= 0 && idx < ddRate->options.size()) currentOpt = ddRate->options[idx];
            if (currentOpt == "[ Select File... ]" || currentOpt == "[ Change File... ]") {
                std::string path = openFileDialog();
                if (!path.empty()) doOpenSource(0, "", 0, path);
                else refreshSourceUI();
            }
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
    slSq->onChange = [&](float v) {
        std::lock_guard<std::mutex> l(sharedData.mtx);
        sharedData.squelchThreshold = v;
    };
    modDemod->addWidget(slSq);

    auto chkStereo = std::make_shared<Checkbox>("Stereo (WFM only)", font);
    chkStereo->onToggle = [&](bool b) {
        std::lock_guard<std::mutex> l(sharedData.mtx);
        sharedData.stereoEnabled = b;
    };
    modDemod->addWidget(chkStereo);

    Mode previousMode = Mode::NFM;

    auto setMode = [&](Mode m, SdrButton* me) {
        if (chkAprs->checked) {
            chkAprs->checked = false;
            {
                std::lock_guard<std::mutex> l(sharedData.mtx);
                sharedData.aprsEnabled = false;
            }
        }

        if (chkAIS->checked) {
            chkAIS->checked = false;
            {
                std::lock_guard<std::mutex> l(sharedData.mtx);
                sharedData.activeDecoder = nullptr;
                ptrAIS->enabled = false;
            }
        }

        rowModes->setEnabled(true);
        btnNFM->setActive(false);
        btnAM->setActive(false);
        btnWFM->setActive(false);
        btnUSB->setActive(false);
        btnLSB->setActive(false);
        btnRAW->setActive(false);
        btnOFF->setActive(false);
        me->setActive(true);

        std::lock_guard<std::mutex> l(sharedData.mtx);
        sharedData.mode = m;

        if (m == Mode::WFM) {
            slBW->setLimits(50000, 200000);
            slBW->setValueSilent(180000);
            sharedData.bandwidth = 180000;
            slBW->setText("Bandwidth: 180 kHz");
        } else if (m == Mode::RAW) {
            slBW->setLimits(1000, 200000);
            slBW->setValueSilent(48000);
            sharedData.bandwidth = 48000;
            slBW->setText("Bandwidth: 48.0 kHz");
        } else if (m == Mode::NFM || m == Mode::AM) {
            slBW->setLimits(4000, 40000);
            slBW->setValueSilent(12500);
            sharedData.bandwidth = 12500;
            slBW->setText("Bandwidth: 12.5 kHz");
        } else if (m == Mode::OFF) {

        } else {
            slBW->setLimits(1000, 10000);
            slBW->setValueSilent(3000);
            sharedData.bandwidth = 3000;
            slBW->setText("Bandwidth: 3 kHz");
        }
    };

    btnNFM->onClick = [&](){ setMode(Mode::NFM, btnNFM.get()); };
    btnAM->onClick = [&](){ setMode(Mode::AM, btnAM.get()); };
    btnWFM->onClick = [&](){ setMode(Mode::WFM, btnWFM.get()); };
    btnUSB->onClick = [&](){ setMode(Mode::USB, btnUSB.get()); };
    btnLSB->onClick = [&](){ setMode(Mode::LSB, btnLSB.get()); };
    btnRAW->onClick = [&](){ setMode(Mode::RAW, btnRAW.get()); };
    btnOFF->onClick = [&](){ setMode(Mode::OFF, btnOFF.get()); };

    chkAprs->onToggle = [&](bool b) {
        {
            std::lock_guard<std::mutex> l(sharedData.mtx);
            sharedData.aprsEnabled = b;
        }
        rowModes->setEnabled(!b);

        if(b && chkAIS->checked) {
            chkAIS->checked = false;
            {
                std::lock_guard<std::mutex> l(sharedData.mtx);
                sharedData.activeDecoder = nullptr;
                ptrAIS->enabled = false;
            }
        }

        if (b) {
            {
                std::lock_guard<std::mutex> l(sharedData.mtx);
                previousMode = sharedData.mode;
            }
            bool isAudioIn = (currentSourceType == 3);

            btnNFM->setActive(!isAudioIn);
            btnAM->setActive(false);
            btnWFM->setActive(false);
            btnUSB->setActive(false);
            btnLSB->setActive(false);
            btnRAW->setActive(isAudioIn);
            btnOFF->setActive(false);

            {
                std::lock_guard<std::mutex> l(sharedData.mtx);
                sharedData.mode = isAudioIn ? Mode::RAW : Mode::NFM;
                sharedData.bandwidth = isAudioIn ? 48000.0f : 12500.0f;
            }

            if (isAudioIn) {
                slBW->setLimits(1000, 200000);
                slBW->setValueSilent(48000);
                slBW->setText("Bandwidth: 48.0 kHz");
            } else {
                slBW->setLimits(4000, 40000);
                slBW->setValueSilent(12500);
                slBW->setText("Bandwidth: 12.5 kHz");
            }
        } else {
            if(previousMode == Mode::NFM) setMode(Mode::NFM, btnNFM.get());
            else if(previousMode == Mode::AM) setMode(Mode::AM, btnAM.get());
            else if(previousMode == Mode::WFM) setMode(Mode::WFM, btnWFM.get());
            else if (previousMode == Mode::RAW) setMode(Mode::RAW, btnRAW.get());
            else if(previousMode == Mode::USB) setMode(Mode::USB, btnUSB.get());
            else if(previousMode == Mode::LSB) setMode(Mode::LSB, btnLSB.get());
            else setMode(Mode::OFF, btnOFF.get());
        }
    };

    modDecoders->addWidget(chkAprs);

    btnCopyAprs->onClick = [&](){
        std::lock_guard<std::mutex> l(sharedData.mtx);
        sf::Clipboard::setString(sharedData.lastAprs.raw);
    };
    modDecoders->addWidget(btnCopyAprs);

    chkAIS->onToggle = [&](bool b) {
        std::lock_guard<std::mutex> l(sharedData.mtx);
        if(b && chkAprs->checked) {
            chkAprs->checked = false;
            sharedData.aprsEnabled = false;
        }

        rowModes->setEnabled(!b);

        if (b) {
            sharedData.activeDecoder = ptrAIS;
            ptrAIS->enabled = true;
            previousMode = sharedData.mode;

            bool isAudioIn = (currentSourceType == 3);
            sharedData.mode = isAudioIn ? Mode::RAW : Mode::NFM;
            sharedData.bandwidth = isAudioIn ? 48000.0f : 25000.0f;

            btnNFM->setActive(!isAudioIn);
            btnAM->setActive(false);
            btnWFM->setActive(false);
            btnUSB->setActive(false);
            btnLSB->setActive(false);
            btnRAW->setActive(isAudioIn);
            btnOFF->setActive(false);

            if (isAudioIn) {
                slBW->setLimits(1000, 200000);
                slBW->setValueSilent(48000);
                slBW->setText("Bandwidth: 48.0 kHz");
            } else {
                slBW->setLimits(4000, 40000);
                slBW->setValueSilent(25000);
                slBW->setText("Bandwidth: 25.0 kHz");
            }
        } else {
            sharedData.activeDecoder = nullptr;
            ptrAIS->enabled = false;
        }
    };

    auto aisUiRestore = [&](bool b) {
        if(!b) {
            if(previousMode == Mode::NFM) setMode(Mode::NFM, btnNFM.get());
            else if(previousMode == Mode::AM) setMode(Mode::AM, btnAM.get());
            else if(previousMode == Mode::WFM) setMode(Mode::WFM, btnWFM.get());
            else if (previousMode == Mode::RAW) setMode(Mode::RAW, btnRAW.get());
            else if(previousMode == Mode::USB) setMode(Mode::USB, btnUSB.get());
            else if(previousMode == Mode::LSB) setMode(Mode::LSB, btnLSB.get());
            else setMode(Mode::OFF, btnOFF.get());
        }
    };

    auto originalAisToggle = chkAIS->onToggle;
    chkAIS->onToggle = [=](bool b) {
        originalAisToggle(b);
        aisUiRestore(b);
    };

    modDecoders->addWidget(chkAIS);

    auto modDisp = sidebar.addModule("Display");

    auto slMinDb = std::make_shared<Slider>(SIDEBAR_W - 40, -140.0f, -20.0f, -90.0f, "Min dB", font);
    slMinDb->onChange = [&](float v) {
        std::lock_guard<std::mutex> l(sharedData.mtx);
        sharedData.minDb = v;
    };
    modDisp->addWidget(slMinDb);

    auto slMaxDb = std::make_shared<Slider>(SIDEBAR_W - 40, -60.0f, 40.0f, 0.0f, "Max dB", font);
    slMaxDb->onChange = [&](float v) {
        std::lock_guard<std::mutex> l(sharedData.mtx);
        sharedData.maxDb = v;
    };
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

    btnRecAudio->onClick = [&](){
        btnRecAudio->setActive(true);
        btnRecIQ->setActive(false);
        std::lock_guard<std::mutex> l(sharedData.mtx);
        sharedData.recMode = RecMode::AUDIO;
    };

    btnRecIQ->onClick = [&](){
        btnRecAudio->setActive(false);
        btnRecIQ->setActive(true);
        std::lock_guard<std::mutex> l(sharedData.mtx);
        sharedData.recMode = RecMode::BASEBAND;
    };

    rowRecMode->add(btnRecAudio);
    rowRecMode->add(btnRecIQ);
    modRec->addWidget(rowRecMode);

    auto lblRecPath = std::make_shared<Label>("Path: ...", font, 11, sf::Color(150,150,150));
    modRec->addWidget(lblRecPath);

    auto btnSetFolder = std::make_shared<SdrButton>(SIDEBAR_W - 40, 25, "Change Folder...", font);
    btnSetFolder->onClick = [&](){
        std::string p = selectFolderDialog();
        if(!p.empty()) {
            {
                std::lock_guard<std::mutex> l(sharedData.mtx);
                sharedData.recPath = p;
            }
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
            btnRecToggle->setText("STOP RECORDING");
            btnRecToggle->setColor(sf::Color(180, 50, 50));
        } else {
            btnRecToggle->setText("START RECORDING");
            btnRecToggle->setColor(sf::Color(40, 40, 45));
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

    std::vector<std::uint8_t> waterfall(INTERNAL_WATERFALL_WIDTH * WATERFALL_HEIGHT_PX * 4, 0);
    sf::Texture wTex;
    if(!wTex.resize({INTERNAL_WATERFALL_WIDTH, WATERFALL_HEIGHT_PX})) return 1;
    wTex.setSmooth(true);
    sf::Sprite wSpr(wTex);

    LayoutState layout;
    sf::Vector2u lastSize = window.getSize();

    auto updateLayout = [&](int w, int h) {
        layout.winW = (float)w;
        layout.winH = (float)h;
        layout.sidebarX = layout.winW - SIDEBAR_W;
        layout.specW = layout.sidebarX;
        layout.specH = 250.0f;
        layout.waterfallH = layout.winH - TOP_BAR_H - layout.specH;

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

        wSpr.setPosition({0, TOP_BAR_H + layout.specH});
        float scaleX = layout.specW / (float)INTERNAL_WATERFALL_WIDTH;
        wSpr.setScale({scaleX, 1.0f});
        wSpr.setTextureRect(sf::IntRect({0, 0}, {INTERNAL_WATERFALL_WIDTH, (int)layout.waterfallH}));
    };

    updateLayout(window.getSize().x, window.getSize().y);


    for (auto& mod : sidebar.modules) {
        if (mod->title == "Source / Input") mod->isOpen = true;
        else mod->isOpen = settingsMgr.getBool("sidebar_" + mod->title, mod->isOpen);
    }

    int savedTheme = settingsMgr.getInt("theme", 0);
    ddTheme->setSelection(savedTheme);
    ddTheme->onChange(savedTheme);

    slMinDb->setValueSilent(settingsMgr.getFloat("min_db", -90.0f));
    slMinDb->onChange(slMinDb->currentVal);

    slMaxDb->setValueSilent(settingsMgr.getFloat("max_db", 0.0f));
    slMaxDb->onChange(slMaxDb->currentVal);

    slZoom->setValueSilent(settingsMgr.getFloat("zoom", 1.0f));
    slZoom->onChange(slZoom->currentVal);

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
    slBW->setValueSilent(savedBw);
    slBW->onChange(savedBw);

    float savedSq = settingsMgr.getFloat("squelch", -100.0f);
    slSq->setValueSilent(savedSq);
    slSq->onChange(savedSq);

    bool savedStereo = settingsMgr.getBool("stereo", false);
    chkStereo->checked = savedStereo;
    chkStereo->onToggle(savedStereo);

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
        refreshSourceUI();
    }

    std::string savedRecPath = settingsMgr.getString("rec_path", "");
    if (savedRecPath.empty()) {
        savedRecPath = getDefaultRecordingPath();
    }
    {
        std::lock_guard<std::mutex> l(sharedData.mtx);
        sharedData.recPath = savedRecPath;
    }
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

    bool showAprsModal = false;
    AprsLastPacket selectedAprsPacket;
    SdrButton btnModalClose(80, 30, "Close", font);
    SdrButton btnModalCopy(80, 30, "Copy", font);
    btnModalClose.onClick = [&]() { showAprsModal = false; };
    btnModalCopy.onClick = [&]() { sf::Clipboard::setString(selectedAprsPacket.raw); };

    while (window.isOpen()) {
        sf::Vector2u currSize = window.getSize();
        if (currSize != lastSize && currSize.x > 0 && currSize.y > 0) {
            sf::FloatRect visibleArea({0.f, 0.f}, {(float)currSize.x, (float)currSize.y});
            window.setView(sf::View(visibleArea));
            updateLayout(currSize.x, currSize.y);
            lastSize = currSize;
        }

        bool isHw = false;
        double hwSampleRate = 2e6;
        double prog = 0.0;

        {
            std::lock_guard<std::mutex> l(sourceMtx);
            if (currentSource) {
                isHw = currentSource->isHardware();
                hwSampleRate = currentSource->getSampleRate();
                prog = currentSource->getProgress();
            }
        }

        freqVFO.setEnabled(true);
        if (!ddSourceType->isOpen && !ddRate->isOpen && !ddDevice->isOpen)
            freqVFO.update(window);

        volSlider.update(window);
        if (std::abs(volSlider.currentVal - sharedData.volume) > 0.01f) {
            std::lock_guard<std::mutex> l(sharedData.mtx);
            sharedData.volume = volSlider.currentVal;
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
        double tunePct = 0.5;
        Mode mode = Mode::NFM;
        float currentZoom = 1.0f;
        int currentTheme = 0;
        double viewCenter = 0.5;

        {
            std::lock_guard<std::mutex> l(sharedData.mtx);
            aprsOn = sharedData.aprsEnabled;
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

        if (viewStartPct < 0.0) {
            viewStartPct = 0.0;
            viewEndPct = visibleFraction;
            viewCenter = viewStartPct + visibleFraction/2.0;
        }
        if (viewEndPct > 1.0) {
            viewEndPct = 1.0;
            viewStartPct = 1.0 - visibleFraction;
            viewCenter = viewStartPct + visibleFraction/2.0;
        }

        double viewWidthPct = viewEndPct - viewStartPct;

        float tunerPosInView = (tunePct - viewStartPct) / viewWidthPct;
        float visualCenterX = tunerPosInView * layout.specW;

        float bwPct = (slBW->currentVal / hwSampleRate);
        float bwPixels = (bwPct / viewWidthPct) * layout.specW;

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

        while (const std::optional<sf::Event> ev = window.pollEvent()) {
            if (ev->is<sf::Event::Closed>()) window.close();

            // Nadrzędny chwytacz zwolnienia przycisku myszy - zapobiega blokowaniu UI
            if (const auto* mr = ev->getIf<sf::Event::MouseButtonReleased>()) {
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

            if (sidebar.handleEvent(*ev, window)) continue;

            bool topBarHandled = false;

            if (volSlider.handleEvent(*ev, window)) topBarHandled = true;
            if (slZoom->handleEvent(*ev, window)) topBarHandled = true;
            if (currentSourceType == 0 && timeSlider.handleEvent(*ev, window)) topBarHandled = true;

            if (btnMute.isClicked(*ev, window)) {
                static bool m = false;
                m = !m;
                if(m) btnMute.setActive(true);
                else btnMute.setActive(false);
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
                    btnPlay.setText("||");
                    btnPlay.setActive(true);
                    audio.start();
                    {
                        std::lock_guard<std::mutex> l(sourceMtx);
                        if (currentSource) currentSource->start();
                    }
                } else {
                    btnPlay.setText(">");
                    btnPlay.setActive(false);
                    audio.stop();
                    {
                        std::lock_guard<std::mutex> l(sourceMtx);
                        if (currentSource) currentSource->stop();
                    }
                }
                topBarHandled = true;
            }

            if (isHw && btnTuningMode.isClicked(*ev, window)) {
                stickyCenterMode = !stickyCenterMode;
                if(stickyCenterMode) {
                    btnTuningMode.setText("CTR");
                    btnTuningMode.setActive(true);
                    {
                        std::lock_guard<std::mutex> l(sharedData.mtx);
                        sharedData.tunedFreqPercent = 0.5;
                    }
                    pendingCenterFreq = freqVFO.getFrequency();
                    debouncer.restart();
                } else {
                    btnTuningMode.setText("FIX");
                    btnTuningMode.setActive(false);
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
                } else if (mb->button == sf::Mouse::Button::Left) {
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
                    bool inFreqScaleZone = (m.y >= TOP_BAR_H + layout.specH - 10 && m.y <= TOP_BAR_H + layout.specH + 20 && m.x < layout.specW);

                    float leftEdge = visualCenterX - bwPixels / 2.0f;
                    float rightEdge = visualCenterX + bwPixels / 2.0f;
                    if (mode == Mode::USB) { leftEdge = visualCenterX; rightEdge = visualCenterX + bwPixels; }
                    if (mode == Mode::LSB) { leftEdge = visualCenterX - bwPixels; rightEdge = visualCenterX; }

                    bool hitEdge = (std::abs(m.x - leftEdge) < 6 || std::abs(m.x - rightEdge) < 6) && (m.y > TOP_BAR_H && m.y < TOP_BAR_H + layout.specH);

                    if (hitEdge && !overAprs) {
                        isDraggingBW = true;
                        dragStartBW = slBW->currentVal;
                        dragStartMouseX = m.x;
                    } else if (inFreqScaleZone && !overTimeline && !overAprs) {
                        isDraggingScale = true;
                        lastDragX = m.x;
                    } else if (!overTimeline && !overAprs && m.x < layout.specW && m.y > TOP_BAR_H && m.y < (TOP_BAR_H + layout.specH + layout.waterfallH)) {
                        isSpectrumDragging = true;
                        applySpectrumTuning(m.x);
                    }
                }
            } else if (const auto* mm = ev->getIf<sf::Event::MouseMoved>()) {
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
                } else if (isDraggingBW) {
                    float hzPerPx = (hwSampleRate * viewWidthPct) / layout.specW;
                    float dist = std::abs(m.x - visualCenterX);
                    float newBW = dist * hzPerPx * 2.0f;
                    if (mode == Mode::USB || mode == Mode::LSB) newBW = dist * hzPerPx;
                    newBW = std::clamp(newBW, slBW->minVal, slBW->maxVal);
                    slBW->setValueSilent(newBW);
                    slBW->onChange(newBW);
                } else if (isSpectrumDragging) applySpectrumTuning(m.x);

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
                        
                        currentCenterFreq = nextCenter; // Szybki update dla wizualizacji
                        { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.centerFreq = nextCenter; }
                        
                        // Kolejkowanie skoku do urządzenia (debouncer)
                        pendingCenterFreq = nextCenter;
                    } else {
                        double pctShift = (dx / layout.specW) * viewWidthPct;
                        std::lock_guard<std::mutex> l(sharedData.mtx);
                        sharedData.viewCenterPct = std::clamp(sharedData.viewCenterPct + pctShift, visibleFraction/2.0, 1.0 - visibleFraction/2.0);
                    }
                }

                if (!overAprs && m.x >= 0 && m.x < layout.specW && m.y >= TOP_BAR_H && m.y < (TOP_BAR_H + layout.specH + layout.waterfallH)) {
                    std::lock_guard<std::mutex> l(sharedData.mtx);
                    sharedData.mouseX_spectrum = m.x;
                    sharedData.mouseY_spectrum = m.y - TOP_BAR_H;
                } else {
                    std::lock_guard<std::mutex> l(sharedData.mtx);
                    sharedData.mouseX_spectrum = -1.0f;
                }
            }
        } // <- Koniec bezpiecznie zamkniętej pętli pollEvent()

        if (pendingCenterFreq != 0 && debouncer.getElapsedTime().asMilliseconds() > TUNING_LATENCY_MS) {
            std::lock_guard<std::mutex> l(sourceMtx);
            if (currentSource && currentSource->isHardware()) {
                currentSource->setCenterFrequency(pendingCenterFreq);
                currentCenterFreq = pendingCenterFreq;
            }
            { std::lock_guard<std::mutex> l(sharedData.mtx); sharedData.centerFreq = pendingCenterFreq; }
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

        if (showAprsModal) showHand = false;

        if (isDraggingBW && cursorSizeH) {
            window.setMouseCursor(*cursorSizeH);
        } else if (showHand && cursorHand) {
            window.setMouseCursor(*cursorHand);
        } else if (cursorArrow) {
            window.setMouseCursor(*cursorArrow);
        }

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
            std::copy_backward(waterfall.begin(), waterfall.end() - INTERNAL_WATERFALL_WIDTH * 4, waterfall.end());
            std::copy(row.begin(), row.end(), waterfall.begin());
            wTex.update(waterfall.data());
        }

        window.clear(sf::Color::Black);

        long long cf = 0;
        if (currentSource) cf = currentCenterFreq;

        drawGrid(window, font, 0, TOP_BAR_H, layout.specW, layout.specH, cf, hwSampleRate, viewStartPct, viewEndPct, slMinDb->currentVal, slMaxDb->currentVal);

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
            if (y < 0) y = 0;
            if (y > layout.specH) y = layout.specH;

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

            fillArea[2 * i] = sf::Vertex{sf::Vector2f(x, topY), glowColor};
            fillArea[2 * i + 1] = sf::Vertex{sf::Vector2f(x, bottomY), bottomColor};
            outline[i] = sf::Vertex{sf::Vector2f(x, topY), coreColor};
        }

        sf::RenderStates states;
        states.blendMode = sf::BlendAdd;
        window.draw(fillArea, states);
        window.draw(outline);

        wSpr.setTextureRect(sf::IntRect({0, 0}, {INTERNAL_WATERFALL_WIDTH, (int)layout.waterfallH}));
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

            if (my > (float)layout.specH) {
                sf::VertexArray lineWaterfall(sf::PrimitiveType::Lines, 2);
                lineWaterfall[0].position = sf::Vector2f(mx, (float)layout.specH + TOP_BAR_H);
                lineWaterfall[0].color = guideColor;
                lineWaterfall[1].position = sf::Vector2f(mx, (float)(layout.specH + layout.waterfallH + TOP_BAR_H));
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
            float tableH = 250.0f;
            float tableY = layout.winH - tableH;
            decoderUI->draw(window, 0, tableY, layout.specW, tableH);
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

        window.display();
    }

    SettingsManager saveMgr;
    for (auto& mod : sidebar.modules) saveMgr.set("sidebar_" + mod->title, mod->isOpen);
    saveMgr.set("theme", ddTheme->selectedIndex);
    saveMgr.set("zoom", slZoom->currentVal);
    saveMgr.set("min_db", slMinDb->currentVal);
    saveMgr.set("max_db", slMaxDb->currentVal);
    saveMgr.set("step_index", ddSnap->selectedIndex);
    saveMgr.set("mode_index", (int)sharedData.mode);
    saveMgr.set("bandwidth", slBW->currentVal);
    saveMgr.set("squelch", slSq->currentVal);
    saveMgr.set("stereo", chkStereo->checked);
    saveMgr.set("rf_gain", sharedData.rfGain);
    saveMgr.set("source_type", ddSourceType->selectedIndex);
    saveMgr.set("hw_fm_notch", hwState.fmNotch);
    saveMgr.set("hw_mw_notch", hwState.mwNotch);
    saveMgr.set("hw_bias_t", hwState.biasT);
    saveMgr.set("hw_direct_samp", hwState.directSampling);
    saveMgr.set("hw_antenna", hwState.antennaIndex);
    {
        std::lock_guard<std::mutex> l(sharedData.mtx);
        saveMgr.set("rec_path", sharedData.recPath);
    }
    saveMgr.set("frequency", (double)currentCenterFreq);
    saveMgr.save(settingsPath);

    dspRunning = false;
    if (dspThread.joinable()) dspThread.join();

    return 0;
}