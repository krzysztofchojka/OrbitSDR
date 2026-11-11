#pragma once
#include <atomic>
#include <thread>
#include <mutex>
#include "Globals.h"
#include "AudioSink.h"
#include "Demodulator.h"
#include "APRS_Decoder.h"
#include "RDS_Decoder.h"
#include "AudioLeveler.h"
#include "RdsGainOptimizer.h"
#include <chrono>
#include "InspectorDSP.h"
#include "WavWriter.h"
#include "Settings.h"
#include "Utils.h"

inline void dspWorker(std::atomic<bool>& running, SharedData& shared, AudioSink& audio) {
    #ifdef _WIN32
        // Stops Windows 11 from moving the DSP thread onto slow E-cores
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    #endif
    Demodulator demod(2000000, AUDIO_RATE);
    APRSDecoder aprsDecoder(AUDIO_RATE);
    InspectorDSP inspector;
    RDSDecoder rdsDecoder;
    AudioLeveler leveler;
    RdsGainOptimizer rdsGainOpt;
    bool rdsGainOptActive = false;
    IQSource* lastSrc = nullptr;
    const auto dspStart = std::chrono::steady_clock::now();
    long long rdsRefFreqHz = 0;    // tuned frequency the current RDS data belongs to
    bool rdsWasActive = false;

    aprsDecoder.onMessage = [&](std::string msg) {
        std::string ts = getTimestamp();
        std::string fullLog = ts + " " + msg;
        std::ofstream logFile(getAprsLogFilePath(), std::ios::app);
        if (logFile.is_open()) {
            logFile << fullLog << "\n";
            logFile.close();
        }
        AprsLastPacket pkt;
        parseAprsData(fullLog, pkt);
        std::lock_guard<std::mutex> l(shared.mtx);
        shared.aprsHistory.push_back(pkt);
        if (shared.aprsHistory.size() > 500) shared.aprsHistory.pop_front();
        shared.lastAprs = pkt;
    };

    double lastSampleRate = 0;
    std::vector<Complex> iqBuffer;
    std::vector<double> winFunc = makeWindow(FFT_SIZE);
    std::vector<double> localFftHistory(INTERNAL_WATERFALL_WIDTH, -100.0);
    std::vector<Complex> fftBuffer(FFT_SIZE, {0,0});
    int fftBufferIdx = 0;
    float smoothedRssi = -100.0f;
    WavWriter recorder;
    sf::Clock waterfallTimer;

    int squelchHold = 0;

    while (running) {
        std::shared_ptr<IQSource> src = nullptr;
        {
            std::lock_guard<std::mutex> lock(sourceMtx);
            src = currentSource;
        }

        if (!src) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        double targetFreqPct, bw;
        float vol;
        bool muted;
        Mode mode;
        bool play, aprsActive;
        float minDb, maxDb;
        bool doRecord;
        RecMode rMode;
        std::string rPath;
        float sqThr;
        bool stereo;
        bool rdsOn;
        long long centerHz;
        double seekReq = -1.0;
        int themeID = 0;
        float currentZoom = 1.0f;
        double viewCenter = 0.5;
        SDRModule* activeModule = nullptr;

        {
            std::lock_guard<std::mutex> lock(shared.mtx);
            seekReq = shared.pendingSeekRequest;
            shared.pendingSeekRequest = -1.0;
            float rawPct = shared.tunedFreqPercent;
            if (std::isnan(rawPct) || std::isinf(rawPct)) rawPct = 0.5f;
            targetFreqPct = std::clamp(rawPct, 0.0f, 1.0f);
            bw = shared.bandwidth;
            vol = shared.volume;
            muted = shared.isMuted;
            mode = shared.mode;
            play = shared.isPlaying;
            minDb = shared.minDb;
            maxDb = shared.maxDb;
            doRecord = shared.isRecording;
            rMode = shared.recMode;
            rPath = shared.recPath;
            aprsActive = shared.aprsEnabled;
            sqThr = shared.squelchThreshold;
            stereo = shared.stereoEnabled;
            rdsOn = shared.rdsEnabled;
            centerHz = shared.centerFreq;
            themeID = shared.waterfallTheme;
            currentZoom = shared.zoomLevel;
            viewCenter = shared.viewCenterPct;
            activeModule = shared.activeDecoder;
        }

        bool justSeeked = false;
        if (seekReq >= 0.0 && src->isSeekable()) {
            src->seek(seekReq);
            demod.clear();
            audio.clear();
            justSeeked = true;
        }

        if (!play && !justSeeked) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        if (doRecord && !recorder.active) {
            long long currentCenterHz;
            {
                std::lock_guard<std::mutex> l(shared.mtx);
                currentCenterHz = shared.centerFreq;
            }
            char timeBuf[32];
            std::time_t now = std::time(nullptr);
            std::strftime(timeBuf, sizeof(timeBuf), "%Y%m%d_%H%M%S", std::localtime(&now));
            std::string filename, freqLabel;

            if (rMode == RecMode::AUDIO) {
                double offset = (targetFreqPct - 0.5) * src->getSampleRate();
                long long tunedHz = currentCenterHz + (long long)offset;
                freqLabel = "_" + std::to_string(tunedHz / 1000) + "kHz";
                filename = (rPath.empty() ? "." : rPath) + "/rec_" + std::string(timeBuf) + freqLabel + "_audio.wav";
                recorder.start(filename, (int)AUDIO_RATE, 2);
            } else {
                freqLabel = "_" + std::to_string(currentCenterHz) + "Hz";
                filename = (rPath.empty() ? "." : rPath) + "/rec_" + std::string(timeBuf) + freqLabel + "_IQ.wav";
                recorder.start(filename, (int)src->getSampleRate(), 2);
            }

            {
                std::lock_guard<std::mutex> l(shared.mtx);
                shared.recStatus = "REC: " + filename;
            }
        } else if (!doRecord && recorder.active) {
            recorder.stop();
            {
                std::lock_guard<std::mutex> l(shared.mtx);
                shared.recStatus = "Saved.";
            }
        }

        if (play && !src->isHardware() && !justSeeked) {
            size_t safeLevel = 24000;
            while (audio.getBufferedCount() > safeLevel && running && shared.isPlaying) {
                {
                    std::lock_guard<std::mutex> l(shared.mtx);
                    if (!shared.isPlaying) break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }

        double sr = src->getSampleRate();
        if (sr != lastSampleRate) {
            demod = Demodulator(sr, AUDIO_RATE);
            lastSampleRate = sr;
        }

        int chunkSize = std::clamp((int)(sr / 20.0), 4096, 262144);
        if (iqBuffer.size() != chunkSize) iqBuffer.resize(chunkSize);

        // RDS runs only in WFM mode. Its data belongs to one station, so it is
        // cleared whenever the receiver moves to a different frequency.
        const bool rdsActive = rdsOn && mode == Mode::WFM && RDSDecoder::rateSupported(sr);
        demod.mpxTapEnabled = rdsActive;
        if (rdsActive) {
            long long tunedHz = centerHz + (long long)((targetFreqPct - 0.5) * sr);
            rdsDecoder.setSampleRate(sr);
            if (!rdsWasActive || std::llabs(tunedHz - rdsRefFreqHz) > 25000) {
                rdsDecoder.reset();
                rdsRefFreqHz = tunedHz;
                rdsGainOpt.reset();               // new station: start again from the plain AGC
                src->setAgcMaxStep(-1);
            }
        }
        rdsWasActive = rdsActive;

        // Burst decoders (ISM) need a gain that stays put during a packet
        src->setBurstAgc(activeModule && activeModule->enabled && activeModule->wantsBurstAgc());

        int readCount = src->read(iqBuffer.data(), chunkSize);
        if (src->didLoop()) {
            demod.clear();
            SDRModule* activeMod = nullptr;
            {
                std::lock_guard<std::mutex> l(shared.mtx);
                activeMod = shared.activeDecoder;
            }
            if (activeMod) activeMod->reset();
        }

        if (readCount > 0) {
            if (recorder.active && rMode == RecMode::BASEBAND) {
                std::vector<float> rawFloat(readCount * 2);
                for(int i=0; i<readCount; i++) {
                    rawFloat[i*2] = (float)iqBuffer[i].real();
                    rawFloat[i*2+1] = (float)iqBuffer[i].imag();
                }
                recorder.write(rawFloat.data(), rawFloat.size());
            }

            for(int i = 0; i < readCount; i++) {
                fftBuffer[fftBufferIdx] = iqBuffer[i];
                fftBufferIdx = (fftBufferIdx + 1) % FFT_SIZE;
            }

            double freqOffset = (targetFreqPct - 0.5) * sr;
            std::vector<Complex> chunkToProcess(iqBuffer.begin(), iqBuffer.begin() + readCount);
            inspector.process(chunkToProcess, sr, freqOffset, bw, shared);

            if (play) {
                if (activeModule && activeModule->enabled) {
                    activeModule->setRadioFrequency(centerHz);
                    activeModule->processIQ(chunkToProcess, sr, freqOffset);
                }
                auto audioData = demod.process(chunkToProcess, freqOffset, bw, mode, stereo);
                if (rdsActive && !demod.mpxTap.empty()) rdsDecoder.process(demod.mpxTap.data(), demod.mpxTap.size());

                // RDS assisted gain: while the software AGC runs, tune its gain limit for the
                // lowest RDS block error rate (see RdsGainOptimizer)
                if (src.get() != lastSrc) { rdsGainOpt.reset(); lastSrc = src.get(); }   // another radio
                const int agcStep = src->getAgcStep();
                if (rdsActive && agcStep >= 0) {
                    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - dspStart).count();
                    const RdsInfo ri = rdsDecoder.getInfo();
                    src->setAgcMaxStep(rdsGainOpt.update(now, ri.synced, ri.blockErrorRate, agcStep, src->getAgcStepCount()));
                    rdsGainOptActive = true;
                } else if (rdsGainOptActive) {
                    rdsGainOpt.reset();
                    src->setAgcMaxStep(-1);
                    rdsGainOptActive = false;
                }

                if (aprsActive) {
                    std::vector<float> mono;
                    mono.reserve(audioData.size()/2);
                    for(size_t i=0; i<audioData.size(); i+=2) mono.push_back(audioData[i]);
                    aprsDecoder.process(mono);
                }

                if (activeModule && activeModule->enabled) {
                    std::vector<float> mono;
                    mono.reserve(audioData.size()/2);
                    for(size_t i=0; i<audioData.size(); i+=2) mono.push_back(audioData[i]);
                    activeModule->processAudio(mono);
                }

                static float squelchEnv = 1.0f;
                if (sqThr > -99.0f) {
                    if (smoothedRssi < sqThr) {
                        squelchEnv *= 0.9f; // Soft close (fade-out)
                    } else {
                        squelchEnv = 1.0f;  // Instant open
                    }
                    if (squelchEnv < 0.001f) squelchEnv = 0.0f; // True digital silence on the speaker
                    
                    // Multiply the samples by the envelope
                    for (auto& s : audioData) {
                        s *= squelchEnv;
                    }
                }

                // Similar loudness for all modes (speaker only, the decoders above got the raw audio)
                leveler.process(audioData);

                float finalVol = muted ? 0.0f : vol;
                for (auto& s : audioData) {
                    if (std::isnan(s) || std::isinf(s)) s = 0.0f;
                    else s = std::clamp(s * finalVol, -1.0f, 1.0f);
                }
                audio.pushSamples(audioData);
                if (recorder.active && rMode == RecMode::AUDIO) recorder.write(audioData.data(), audioData.size());
            }

            if (waterfallTimer.getElapsedTime().asMilliseconds() > 33) {
                std::vector<Complex> fftData(FFT_SIZE);
                for(int i=0; i<FFT_SIZE; i++) fftData[i] = fftBuffer[(fftBufferIdx + i) % FFT_SIZE] * winFunc[i];
                fft(fftData);

                double visibleFraction = 1.0 / currentZoom;
                double viewStartPct = viewCenter - visibleFraction / 2.0;
                double viewEndPct = viewCenter + visibleFraction / 2.0;

                if (viewStartPct < 0.0) {
                    viewStartPct = 0.0;
                    viewEndPct = visibleFraction;
                }
                if (viewEndPct > 1.0) {
                    viewEndPct = 1.0;
                    viewStartPct = 1.0 - visibleFraction;
                }

                // Waterfall - use float precision so partial bins at the FFT edges aren't cut off
                float exactStartBin = viewStartPct * FFT_SIZE;
                float exactEndBin = viewEndPct * FFT_SIZE;
                float exactVisibleBins = exactEndBin - exactStartBin;
                float alpha = 1.0f;

                std::vector<uint8_t> tempRow(INTERNAL_WATERFALL_WIDTH * 4);
                for (int x = 0; x < INTERNAL_WATERFALL_WIDTH; x++) {
                    float binFloat = exactStartBin + ((float)x / INTERNAL_WATERFALL_WIDTH) * exactVisibleBins;
                    int bIdx = std::clamp((int)binFloat, 0, FFT_SIZE - 1);
                    int shiftedIdx = (bIdx + FFT_SIZE / 2) % FFT_SIZE;
                    float mag = std::abs(fftData[shiftedIdx]) / FFT_SIZE;
                    float db = 20.0f * std::log10(mag + 1e-12f);
                    
                    localFftHistory[x] = localFftHistory[x] * (1.0f - alpha) + db * alpha;
                    float norm = (localFftHistory[x] - minDb) / (maxDb - minDb);
                    sf::Color c = getHeatmap(norm, themeID);
                    
                    int px = x * 4;
                    tempRow[px] = c.r;
                    tempRow[px + 1] = c.g;
                    tempRow[px + 2] = c.b;
                    tempRow[px + 3] = 255;
                }

                int tunerBin = std::clamp((int)(targetFreqPct * FFT_SIZE), 0, FFT_SIZE - 1);
                int shiftedTuner = (tunerBin + FFT_SIZE / 2) % FFT_SIZE;
                float tMag = std::abs(fftData[shiftedTuner]) / FFT_SIZE;
                smoothedRssi = smoothedRssi * 0.8f + (20.0f * std::log10(tMag + 1e-12f)) * 0.2f;

                {
                    std::unique_lock<std::mutex> lock(shared.mtx, std::try_to_lock);
                    if (lock.owns_lock()) {
                        std::vector<double> visibleSpectrum(INTERNAL_WATERFALL_WIDTH);
                        for(int i=0; i<INTERNAL_WATERFALL_WIDTH; i++) visibleSpectrum[i] = localFftHistory[i];
                        shared.fftSpectrum = visibleSpectrum;
                        shared.waterfallRow = tempRow;
                        shared.newWaterfallData = true;
                        shared.rds = rdsActive ? rdsDecoder.getInfo() : RdsInfo();
                        waterfallTimer.restart();
                    }
                }
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    if (recorder.active) recorder.stop();
}