#pragma once

#include "PipelineEditor.h"
#include <thread>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <set>

// =====================================================================
//  Pipeline workspaces: the pipeline tabs next to the receiver tabs.
//
//  Each workspace is one pipeline (a saved chain) with its editor and its
//  own thread, so several chains run side by side on several cores.
//  Its Channel input blocks pick a receiver ("RX1", "RX2" ... or none);
//  the receivers publish their orange Analyzer channel through the Hub
//  (SignalAnalyzer::processIQ) to every workspace that takes them.
//
//  Thread of a workspace: waits for samples (or 20 ms), takes everything
//  waiting for its inputs, then runs the pipeline once. Generators are
//  paced by the first input that brought samples (its sample count), or
//  by the wall clock when no input brings any. An input more than a second
//  behind drops its oldest samples (counted, shown on the input block).
//
//  Lock order: Hub::m -> Workspace::fm; Pipeline::mtx is never held with
//  either of them.
// =====================================================================

namespace pl {

class Workspace {
public:
    const int uid;
    Pipeline pipe;
    PipelineEditor editor {pipe};

    explicit Workspace(int u) : uid(u) { th = std::thread([this] { loop(); }); }
    ~Workspace() {
        { std::lock_guard<std::mutex> l(fm); stopping = true; }
        cv.notify_all();
        if (th.joinable()) th.join();
    }

    // Does an input of this workspace take receiver `rx`? (any thread)
    bool takes(int rx) { std::lock_guard<std::mutex> l(fm); return want.count(rx) > 0; }
    std::set<int> taking() { std::lock_guard<std::mutex> l(fm); return want; }

    // A receiver's channel samples (its DSP thread): to every input that takes it
    void feed(int rx, const CF* x, size_t n, double rate, const std::string& info) {
        {
            std::lock_guard<std::mutex> l(fm);
            for (auto& kv : feeds) {
                Feed& f = kv.second;
                if (f.rx != rx) continue;
                if (f.rate != rate) { f.q.clear(); f.queued = 0; f.rate = rate; }
                f.q.emplace_back(x, x + n);
                f.queued += n;
                f.info = info;
                // more than a second waiting (this pipeline is too slow): the oldest go
                while (f.q.size() > 1 && (double)f.queued > rate) {
                    f.queued -= f.q.front().size();
                    f.q.pop_front();
                    f.dropped++;
                }
            }
        }
        cv.notify_one();
    }

    // How long the last run took and how much signal it stood for (the load, for the tab)
    double load() const { return loadAvg.load(); }

private:
    struct Feed {
        int rx = -1;
        std::deque<std::vector<CF>> q;
        size_t queued = 0;
        double rate = 0.0;
        std::string info;
        long long dropped = 0;
    };
    std::mutex fm;
    std::condition_variable cv;
    bool stopping = false;
    std::map<int, Feed> feeds;      // by input block id
    std::set<int> want;             // the receivers its inputs take
    std::thread th;
    std::atomic<double> loadAvg {0.0};

    struct Got { std::vector<CF> x; double rate = 0.0; std::string info; long long dropped = 0; int rx = -1; };

    void loop() {
        using clk = std::chrono::steady_clock;
        auto lastInput = clk::now() - std::chrono::seconds(10), lastRun = clk::now();
        std::map<int, Got> got;
        for (;;) {
            bool any = false;
            {
                std::unique_lock<std::mutex> l(fm);
                cv.wait_for(l, std::chrono::milliseconds(20), [&] {
                    if (stopping) return true;
                    for (const auto& kv : feeds) if (!kv.second.q.empty()) return true;
                    return false;
                });
                if (stopping) return;
                for (auto it = got.begin(); it != got.end();) it = feeds.count(it->first) ? std::next(it) : got.erase(it);
                for (auto& kv : feeds) {
                    Feed& f = kv.second;
                    Got& g = got[kv.first];
                    g.x.clear();
                    for (auto& c : f.q) g.x.insert(g.x.end(), c.begin(), c.end());
                    f.q.clear();
                    f.queued = 0;
                    g.rate = f.rate; g.info = f.info; g.dropped = f.dropped; g.rx = f.rx;
                    any = any || !g.x.empty();
                }
            }
            const auto now = clk::now();
            double tick = -1.0;
            if (!any) {
                // no signal: the generators run by the wall clock (after a short wait, so a
                // receiver between two chunks does not count as stopped)
                if (now - lastInput < std::chrono::milliseconds(100)) continue;
                tick = std::min(std::chrono::duration<double>(now - lastRun).count(), 0.25);
            } else {
                lastInput = now;
            }
            lastRun = now;

            std::map<int, int> subs;   // input block -> receiver
            std::set<int> rxs;
            const auto t0 = clk::now();
            {
                std::lock_guard<std::mutex> l(pipe.mtx);
                for (auto& bp : pipe.nodes) {
                    if (!bp->isInput) continue;
                    InputBlock* in = static_cast<InputBlock*>(bp.get());
                    const int rx = in->rxId();
                    subs[in->id] = rx;
                    if (rx > 0) rxs.insert(rx);
                    Stream& s = in->out[0];
                    s.clear();
                    s.type = PT::IQ;
                    const auto it = got.find(in->id);
                    const bool mine = it != got.end() && it->second.rx == rx;
                    s.rate = mine ? it->second.rate : 0.0;
                    if (mine) s.iq.swap(it->second.x);
                    // the first input with samples paces the generators
                    if (tick < 0.0 && !s.iq.empty() && s.rate > 0) tick = s.iq.size() / s.rate;
                    if (rx <= 0) in->info = "no receiver: sources (generators) only";
                    else if (!mine || it->second.info.empty()) in->info = "RX" + std::to_string(rx) + ": waiting for its signal (playing?)";
                    else {
                        in->info = "RX" + std::to_string(rx) + ": " + it->second.info;
                        if (it->second.dropped > 0) in->info += "  (too slow: " + std::to_string(it->second.dropped) + " skipped)";
                    }
                }
                if (tick < 0.0) tick = 0.0;
                pipe.run(tick);
            }
            // how busy: run time / signal time (smoothed)
            const double took = std::chrono::duration<double>(clk::now() - t0).count();
            if (tick > 0.0) loadAvg = loadAvg.load() * 0.9 + 0.1 * (took / tick);

            std::lock_guard<std::mutex> l(fm);
            for (auto it = feeds.begin(); it != feeds.end();) it = subs.count(it->first) ? std::next(it) : feeds.erase(it);
            for (const auto& kv : subs) {
                Feed& f = feeds[kv.first];
                if (f.rx != kv.second) { f = Feed(); f.rx = kv.second; }
            }
            want = rxs;
        }
    }
};

// All the workspaces, in tab order
class Hub {
public:
    static Hub& get() { static Hub h; return h; }

    // ---- UI thread ----
    // A new tab with the saved chain `chain` ("" or not saved: a new empty chain) taking
    // receiver `rx` (e.g. "RX2"; "" = keep what the chain has)
    Workspace* add(const std::string& chain, const std::string& rx, const sf::Font* font) {
        auto w = std::make_unique<Workspace>(nextUid++);
        Workspace* p = w.get();
        p->editor.setFont(font);
        p->editor.chainInUse = [this, p](const std::string& name) {
            for (Workspace* o : list()) if (o != p && o->editor.title() == name) return true;
            return false;
        };
        bool opened = false;
        if (!chain.empty() && !openIn(chain, nullptr)) opened = p->editor.openChain(chain);
        if (!opened) p->editor.newChain(chain.empty() ? "Pipeline" : chain);
        if (!rx.empty()) p->editor.setSource(rx);
        std::lock_guard<std::mutex> l(m);
        tabs.push_back(std::move(w));
        return p;
    }
    void close(Workspace* w) {
        if (!w) return;
        w->editor.saveIfChanged();
        std::unique_ptr<Workspace> gone;
        {
            std::lock_guard<std::mutex> l(m);
            for (auto it = tabs.begin(); it != tabs.end(); ++it)
                if (it->get() == w) { gone = std::move(*it); tabs.erase(it); break; }
        }
        // (its thread stops here, outside the lock)
    }
    // Tab `from` moved to position `to`
    void move(int from, int to) {
        std::lock_guard<std::mutex> l(m);
        if (from < 0 || from >= (int)tabs.size() || to < 0 || to >= (int)tabs.size() || from == to) return;
        auto w = std::move(tabs[from]);
        tabs.erase(tabs.begin() + from);
        tabs.insert(tabs.begin() + to, std::move(w));
    }
    std::vector<Workspace*> list() {
        std::lock_guard<std::mutex> l(m);
        std::vector<Workspace*> v;
        for (auto& t : tabs) v.push_back(t.get());
        return v;
    }
    Workspace* byUid(int uid) {
        std::lock_guard<std::mutex> l(m);
        for (auto& t : tabs) if (t->uid == uid) return t.get();
        return nullptr;
    }
    // the tab that has chain `name` open (other than `except`), or nullptr
    Workspace* openIn(const std::string& name, Workspace* except) {
        for (Workspace* w : list()) if (w != except && w->editor.title() == name) return w;
        return nullptr;
    }
    // the first tab that takes receiver `rx`, or nullptr
    Workspace* taking(int rx) {
        for (Workspace* w : list()) if (w->takes(rx)) return w;
        return nullptr;
    }
    void saveAll() { for (Workspace* w : list()) w->editor.saveIfChanged(); }

    // ---- any thread ----
    // Does any workspace take receiver `rx`? (its Analyzer channel is cut out only then,
    // or while the Analyzer's own panel is shown)
    bool wants(int rx) {
        std::lock_guard<std::mutex> l(m);
        for (auto& t : tabs) if (t->takes(rx)) return true;
        return false;
    }
    std::set<int> wanted() {
        std::lock_guard<std::mutex> l(m);
        std::set<int> s;
        for (auto& t : tabs) for (int r : t->taking()) s.insert(r);
        return s;
    }
    // A receiver's channel samples (its DSP thread)
    void publish(int rx, const CF* x, size_t n, double rate, const std::string& info) {
        std::lock_guard<std::mutex> l(m);
        for (auto& t : tabs) if (t->takes(rx)) t->feed(rx, x, n, rate, info);
    }

private:
    std::mutex m;
    std::vector<std::unique_ptr<Workspace>> tabs;
    int nextUid = 1;
};

} // namespace pl
