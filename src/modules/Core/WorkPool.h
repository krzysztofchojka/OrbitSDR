#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#if defined(__APPLE__)
#include <pthread.h>
#include <sys/qos.h>
#endif

// =====================================================================
//  WorkPool - one process-wide pool of worker threads for the parts of
//  the decoders that can run in parallel (the channels of a multi-channel
//  decoder).
//
//  Every decoder already has its own thread (DecoderRunner, DSPWorker.h).
//  A decoder with many channels (Digital Voice with several TETRA / DMR
//  channels over a 6 MHz span) would still do all of them one after the
//  other on that one thread and fall behind while the other cores idle.
//  With the pool it hands its channels over as one batch:
//
//    - parallelFor(n, fn) runs fn(0..n-1) on the pool AND the calling
//      thread, and returns when all are done;
//    - items are handed out one at a time to whichever thread is free
//      (dynamic scheduling), so one expensive channel never holds up the
//      cheap ones; callers give the most expensive items first (longest
//      processing time first), which keeps all threads busy to the end;
//    - several decoders can submit batches at the same time: the pool
//      works them off in order of arrival, while each caller also works on
//      its own batch, so no decoder waits behind another one's work.
//
//  Size: the CPU's threads minus two (the DSP thread and the UI keep a
//  core), at least 1; ORBITSDR_THREADS overrides it (0 = no pool, every
//  batch runs on the caller).
//
//  fn must not throw and must not wait for a lock its caller holds.
// =====================================================================

class WorkPool {
public:
    static WorkPool& instance() {
        // never destroyed: decoder threads may still submit work while the process exits
        static WorkPool* p = new WorkPool();
        return *p;
    }

    size_t threadCount() const { return threads.size(); }

    void parallelFor(size_t n, const std::function<void(size_t)>& fn) {
        if (n == 0) return;
        if (n == 1 || threads.empty()) {
            for (size_t i = 0; i < n; i++) fn(i);
            return;
        }
        auto b = std::make_shared<Batch>();
        b->fn = &fn;
        b->n = n;
        {
            std::lock_guard<std::mutex> l(m);
            queue.push_back(b);
        }
        const size_t wake = std::min(n - 1, threads.size());
        for (size_t i = 0; i < wake; i++) cv.notify_one();

        work(*b);   // the caller takes part

        // the items the pool threads are still running
        std::unique_lock<std::mutex> l(b->m);
        b->cv.wait(l, [&] { return b->done.load() == b->n; });
    }

private:
    struct Batch {
        const std::function<void(size_t)>* fn = nullptr;   // valid until the caller returns
        size_t n = 0;
        std::atomic<size_t> next {0};
        std::atomic<size_t> done {0};
        std::mutex m;
        std::condition_variable cv;
    };

    std::mutex m;
    std::condition_variable cv;
    std::deque<std::shared_ptr<Batch>> queue;
    std::vector<std::thread> threads;

    static void work(Batch& b) {
        for (;;) {
            const size_t i = b.next.fetch_add(1);
            if (i >= b.n) return;
            try { (*b.fn)(i); } catch (...) {}
            if (b.done.fetch_add(1) + 1 == b.n) {
                std::lock_guard<std::mutex> l(b.m);
                b.cv.notify_all();
            }
        }
    }

    void loop() {
#if defined(__APPLE__)
        // like the decoder threads: user-initiated work, eligible for the performance cores
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#endif
        for (;;) {
            std::shared_ptr<Batch> b;
            {
                std::unique_lock<std::mutex> l(m);
                cv.wait(l, [&] { return !queue.empty(); });
                b = queue.front();
                if (b->next.load() >= b->n) {   // all handed out: no longer pending
                    queue.pop_front();
                    continue;
                }
            }
            work(*b);
            std::lock_guard<std::mutex> l(m);
            auto it = std::find(queue.begin(), queue.end(), b);
            if (it != queue.end()) queue.erase(it);
        }
    }

    WorkPool() {
        size_t n = 0;
        if (const char* e = std::getenv("ORBITSDR_THREADS")) {
            n = (size_t)std::max(0, std::atoi(e));
        } else {
            const unsigned hw = std::thread::hardware_concurrency();
            n = hw > 2 ? hw - 2 : 1;
        }
        n = std::min<size_t>(n, 32);
        for (size_t i = 0; i < n; i++) threads.emplace_back([this] { loop(); });
        for (auto& t : threads) t.detach();
    }
};

// What a channel processed on the pool reports into its decoder's shared state (a table of
// messages, the log): queued while the channel runs on a pool thread, run by the decoder's
// own thread after the join, in the order it happened. Outside a batch post() runs at once.
class DeferredCalls {
    bool active = false;
    std::vector<std::function<void()>> q;

public:
    template <typename F> void post(F&& f) {
        if (active) q.emplace_back(std::forward<F>(f));
        else f();
    }
    void begin() { active = true; }
    void end() { active = false; }
    void flush() {
        for (auto& f : q) f();
        q.clear();
    }
};
