#pragma once
#include <atomic>
#include <thread>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include "Globals.h"
#include "AudioSink.h"
#include "Demodulator.h"
#include "APRS_Decoder.h"
#include "modules/Core/AprsPosition.h"
#include "RDS_Decoder.h"
#include "AudioLeveler.h"
#include "RdsGainOptimizer.h"
#include <chrono>
#include "InspectorDSP.h"
#include "WavWriter.h"
#include "Settings.h"
#include "Utils.h"
#include "orbit/RecordingTime.h"

// =====================================================================
//  The decoders run in their own threads, one per decoder: the DSP thread
//  hands every block of I/Q (and the demodulated audio) over and goes on
//  with the waterfall and the speaker; the decoders work in parallel on
//  the other CPU cores. A decoder that can't keep up skips blocks
//  (counted in decoderDrops) instead of making the audio stutter.
// =====================================================================
class DecoderRunner {
public:
    struct Job {
        std::vector<Complex> iq;
        std::vector<float> mono;      // demodulated audio (48 kHz mono)
        double sr = 0, offset = 0, bw = 0, t = 0;
        long long centerHz = 0;
        bool reliable = true, audioIn = false, reset = false;
    };

    explicit DecoderRunner(SharedData& s) : shared(s) {}
    ~DecoderRunner() {
        for (auto& kv : workers) kv.second->shutdown();
    }

    // DSP thread: every running decoder gets the block (shared, not copied)
    void push(Job&& j) {
        std::vector<SDRModule*> mods;
        { std::lock_guard<std::mutex> l(shared.mtx); mods = shared.activeDecoders; }
        auto job = std::make_shared<const Job>(std::move(j));
        for (SDRModule* m : mods) {
            if (!m || !m->enabled) continue;
            auto& w = workers[m];
            if (!w) w = std::make_unique<Worker>(m, shared);
            w->push(job);
        }
    }

private:
    class Worker {
        SDRModule* mod;
        SharedData& shared;
        std::mutex m;
        std::condition_variable cv;
        std::deque<std::shared_ptr<const Job>> q;
        size_t queued = 0;            // samples waiting
        bool stop = false;
        bool pendingReset = false;    // a dropped block asked for a reset
        std::thread th;               // last: starts after the members above exist

        // The queue may hold this much I/Q. (A limit in blocks was wrong: radios often deliver
        // small blocks, and a short stall then "overflowed" with the CPU almost idle.)
        static constexpr double MAX_QUEUE_SEC = 1.5;

        void run() {
            for (;;) {
                std::shared_ptr<const Job> j;
                bool reset;
                {
                    std::unique_lock<std::mutex> l(m);
                    cv.wait(l, [this] { return stop || !q.empty(); });
                    if (stop) return;
                    j = std::move(q.front());
                    q.pop_front();
                    queued -= j->iq.size();
                    reset = j->reset || pendingReset;
                    pendingReset = false;
                }
                if (!mod->enabled) continue;
                if (reset) mod->reset();
                mod->setAudioInput(j->audioIn);
                mod->setStreamTime(j->t, j->reliable);
                mod->setRadioFrequency(j->centerHz);
                mod->setTunerBandwidth(j->bw);
                mod->processIQ(j->iq, j->sr, j->offset);
                if (!j->mono.empty()) mod->processAudio(j->mono);
            }
        }

    public:
        Worker(SDRModule* md, SharedData& s) : mod(md), shared(s), th([this] { run(); }) {}
        void push(const std::shared_ptr<const Job>& j) {
            {
                std::lock_guard<std::mutex> l(m);
                queued += j->iq.size();
                q.push_back(j);
                const double limit = std::max(j->sr, 1.0) * MAX_QUEUE_SEC;
                while (q.size() > 1 && (double)queued > limit) {   // really behind: drop the oldest
                    if (q.front()->reset) pendingReset = true;
                    queued -= q.front()->iq.size();
                    q.pop_front();
                    shared.decoderDrops++;
                }
            }
            cv.notify_one();
        }
        void shutdown() {
            { std::lock_guard<std::mutex> l(m); stop = true; }
            cv.notify_all();
            if (th.joinable()) th.join();
        }
    };

    SharedData& shared;
    std::map<SDRModule*, std::unique_ptr<Worker>> workers;   // DSP thread only
};

inline void dspWorker(std::atomic<bool>& running, SharedData& shared, AudioSink& audio) {
    #ifdef _WIN32
        // Stops Windows 11 from moving the DSP thread onto slow E-cores
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    #endif
    Demodulator demod(2000000, AUDIO_RATE);
    APRSDecoder aprsDecoder(AUDIO_RATE);
    std::unique_ptr<Demodulator> aprsDemod;   // APRS channel (I/Q input), see aprsChannels()
    double aprsDemodRate = 0.0;
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
        // the station on the satellite map
        PosReport pos;
        if (aprspos::parse(msg, pos)) positionBus().report(pos);
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

    float squelchEnv = 1.0f;   // per receiver (several DSP threads run at once)
    double paceSamples = -1.0; // background file playback: samples played since paceStart
    auto paceStart = std::chrono::steady_clock::now();
    std::vector<float> fftMags(FFT_SIZE), fftSorted(FFT_SIZE);
    DecoderRunner decoders(shared);   // the decoders' thread (stops when dspWorker returns)
    bool decodersReset = false;

    // Stream time for the slot based decoders (see SDRModule::setStreamTime)
    std::string streamTimePath;
    double streamTimeStart = 0.0;
    bool streamTimeReliable = true;
    auto streamTimeOf = [&](IQSource* s, int samples, double rate, bool& reliable) -> double {
        const std::string path = s->getFilePath();
        const double blockSec = rate > 0 ? samples / rate : 0.0;
        if (path.empty()) {
            reliable = true;
            const double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
            return now - blockSec;
        }
        const double dur = s->getDurationSec();
        if (path != streamTimePath) {
            streamTimePath = path;
            const orbit::RecordingStart rs = orbit::recordingStart(path, dur);
            streamTimeStart = rs.unix;
            streamTimeReliable = rs.unix > 0 && !rs.guess;
        }
        reliable = streamTimeReliable;
        return streamTimeStart + s->getProgress() * dur - blockSec;
    };

    while (running) {
        std::shared_ptr<IQSource> src = nullptr;
        {
            std::lock_guard<std::mutex> lock(shared.sourceMtx);
            src = shared.source;
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
        bool anyDecoder = false;
        bool burstAgc = false;
        bool audioIn = false;

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
            for (SDRModule* m : shared.activeDecoders)
                if (m && m->enabled) { anyDecoder = true; burstAgc = burstAgc || m->wantsBurstAgc(); }
            audioIn = shared.audioInput;
        }

        bool justSeeked = false;
        if (seekReq >= 0.0 && src->isSeekable()) {
            src->seek(seekReq);
            demod.clear();
            audio.clear(shared.audioStream);
            justSeeked = true;
            paceSamples = -1.0;
        }

        if (!play && !justSeeked) {
            paceSamples = -1.0;
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

        const bool toSpeaker = shared.audioOut;
        if (play && !src->isHardware() && !justSeeked && !toSpeaker && !src->getFilePath().empty()) {
            // a file in a background tab: nothing drains the speaker buffer, play it by the clock
            const auto nowT = std::chrono::steady_clock::now();
            if (paceSamples < 0.0) { paceSamples = 0.0; paceStart = nowT; }
            const double ahead = paceSamples / std::max(1.0, src->getSampleRate()) -
                                 std::chrono::duration<double>(nowT - paceStart).count();
            if (ahead > 0.15) std::this_thread::sleep_for(std::chrono::milliseconds((int)((ahead - 0.1) * 1000)));
        } else paceSamples = -1.0;
        if (play && !src->isHardware() && !justSeeked && toSpeaker) {
            size_t safeLevel = 24000;
            while (audio.getBufferedCount(shared.audioStream) > safeLevel && running && shared.isPlaying) {
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

        // Input set to Audio (Source / Input switch): the samples already are audio, so
        // only RAW makes sense (any other demodulator would turn the audio into noise)
        const bool audioSource = audioIn;
        if (audioSource) mode = Mode::RAW;

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
        src->setBurstAgc(burstAgc);

        int readCount = src->read(iqBuffer.data(), chunkSize);
        if (src->didLoop()) {
            demod.clear();
            decodersReset = true;   // the decoders start again with the next block
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

            if (paceSamples >= 0.0) paceSamples += readCount;
            const bool shown = shared.visible;   // background tabs: no waterfall, spectrum, inspector
            if (shown) for(int i = 0; i < readCount; i++) {
                fftBuffer[fftBufferIdx] = iqBuffer[i];
                fftBufferIdx = (fftBufferIdx + 1) % FFT_SIZE;
            }

            double freqOffset = (targetFreqPct - 0.5) * sr;
            if (audioSource) freqOffset = 0.0;   // audio: nothing to tune, keep it unshifted
            std::vector<Complex> chunkToProcess(iqBuffer.begin(), iqBuffer.begin() + readCount);
            if (shown) inspector.process(chunkToProcess, sr, freqOffset, bw, shared);

            if (play) {
                // the decoders' block (their thread takes it after the demodulated audio is added below)
                DecoderRunner::Job decJob;
                if (anyDecoder) {
                    decJob.t = streamTimeOf(src.get(), readCount, sr, decJob.reliable);
                    decJob.iq = chunkToProcess;
                    decJob.sr = sr;
                    decJob.offset = freqOffset;
                    decJob.bw = bw;
                    decJob.centerHz = centerHz;
                    decJob.audioIn = audioSource;
                    decJob.reset = decodersReset;
                    decodersReset = false;
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
                    // Audio input: the input audio itself. I/Q: the own APRS channel (NFM)
                    const std::vector<float>* src = &audioData;
                    std::vector<float> chAudio;
                    if (!audioSource) {
                        const auto rc = shared.aprsCh.update(centerHz, sr, freqOffset);
                        if (!rc.empty() && rc[0].inBand) {
                            if (!aprsDemod || aprsDemodRate != sr) {
                                aprsDemod = std::make_unique<Demodulator>(sr, AUDIO_RATE);
                                aprsDemodRate = sr;
                            }
                            chAudio = aprsDemod->process(chunkToProcess, rc[0].offsetHz, rc[0].ch.widthHz, Mode::NFM, false);
                        }
                        src = &chAudio;
                    }
                    std::vector<float> mono;
                    mono.reserve(src->size()/2);
                    for(size_t i=0; i<src->size(); i+=2) mono.push_back((*src)[i]);
                    if (!mono.empty()) aprsDecoder.process(mono);
                }

                if (anyDecoder) {
                    decJob.mono.reserve(audioData.size() / 2);
                    for (size_t i = 0; i < audioData.size(); i += 2) decJob.mono.push_back(audioData[i]);
                    decoders.push(std::move(decJob));
                }

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

                bool scanMuted = false;
                { std::lock_guard<std::mutex> l(shared.mtx); scanMuted = shared.scanMute; }
                float finalVol = (muted || scanMuted) ? 0.0f : vol;
                for (auto& s : audioData) {
                    if (std::isnan(s) || std::isinf(s)) s = 0.0f;
                    else s = std::clamp(s * finalVol, -1.0f, 1.0f);
                }
                if (toSpeaker) audio.pushSamples(audioData, shared.audioStream);
                if (recorder.active && rMode == RecMode::AUDIO) recorder.write(audioData.data(), audioData.size());
            }

            if (shown && waterfallTimer.getElapsedTime().asMilliseconds() > 33) {
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

                // Tuner band level for the scanner (full FFT, so it works at any zoom)
                float tunerSnr = 0.0f, tunerPeak = -150.0f;
                {
                    std::vector<float>& mags = fftMags;
                    for (int i = 0; i < FFT_SIZE; i++) {
                        const int sh = (i + FFT_SIZE / 2) % FFT_SIZE;   // index 0 = lowest frequency
                        mags[i] = 20.0f * std::log10(std::abs(fftData[sh]) / FFT_SIZE + 1e-12f);
                    }
                    double lo = -bw / 2.0, hi = bw / 2.0;
                    if (mode == Mode::USB) { lo = 0.0; hi = bw; }
                    else if (mode == Mode::LSB) { lo = -bw; hi = 0.0; }
                    const double binHz = sr / FFT_SIZE;
                    const int c = (int)(targetFreqPct * FFT_SIZE);
                    const int b0 = std::clamp(c + (int)std::floor(lo / binHz), 0, FFT_SIZE - 1);
                    const int b1 = std::clamp(c + (int)std::ceil(hi / binHz), b0, FFT_SIZE - 1);
                    for (int b = b0; b <= b1; b++) tunerPeak = std::max(tunerPeak, mags[b]);
                    std::vector<float>& sorted = fftSorted;
                    sorted = mags;
                    std::nth_element(sorted.begin(), sorted.begin() + FFT_SIZE / 5, sorted.end());
                    tunerSnr = std::max(0.0f, tunerPeak - sorted[FFT_SIZE / 5]);
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
                        shared.tunerSnrDb = tunerSnr;
                        shared.tunerPeakDb = tunerPeak;
                        shared.tunerLevelSeq++;
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