#pragma once

// =====================================================================
//  Minimal HTTPS GET for the satellite tracker.
//   - Windows: WinINet (part of the system, link with -lwininet)
//   - macOS / Linux: the system curl binary (always there on macOS,
//     a dependency of the .deb package on Linux)
//  Requests run one at a time on a worker thread (HttpQueue), so the
//  UI never blocks and the servers never see bursts from us.
// =====================================================================

#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <functional>
#include <atomic>
#include <memory>
#include <cstdio>
#include <cctype>

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #include <wininet.h>
#endif

namespace orbit {

struct HttpResult {
    int status = 0;          // HTTP status, 0 = no connection
    std::string body;        // raw bytes
    std::string error;
    bool ok() const { return status >= 200 && status < 300; }
};

inline std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

// Synchronous GET. `headers` are "Name: value" lines.
inline HttpResult httpGet(const std::string& url, const std::vector<std::string>& headers = {}, int timeoutSec = 30) {
    HttpResult res;
#ifdef _WIN32
    HINTERNET net = InternetOpenA("OrbitSDR", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!net) { res.error = "InternetOpen failed"; return res; }
    DWORD to = (DWORD)timeoutSec * 1000;
    InternetSetOptionA(net, INTERNET_OPTION_CONNECT_TIMEOUT, &to, sizeof(to));
    InternetSetOptionA(net, INTERNET_OPTION_RECEIVE_TIMEOUT, &to, sizeof(to));
    InternetSetOptionA(net, INTERNET_OPTION_SEND_TIMEOUT, &to, sizeof(to));
    std::string hdr;
    for (const auto& h : headers) hdr += h + "\r\n";
    HINTERNET h = InternetOpenUrlA(net, url.c_str(), hdr.empty() ? NULL : hdr.c_str(), hdr.empty() ? 0 : (DWORD)-1,
                                   INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI, 0);
    if (!h) {
        char b[64]; std::snprintf(b, sizeof(b), "connection failed (error %lu)", (unsigned long)GetLastError());
        res.error = b;
        InternetCloseHandle(net);
        return res;
    }
    DWORD code = 0, len = sizeof(code);
    if (HttpQueryInfoA(h, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &code, &len, NULL)) res.status = (int)code;
    else res.status = 200;   // non-HTTP URL (unlikely)
    char buf[16384];
    DWORD got = 0;
    while (InternetReadFile(h, buf, sizeof(buf), &got) && got > 0) res.body.append(buf, got);
    InternetCloseHandle(h);
    InternetCloseHandle(net);
#else
    // URL and headers are passed in single quotes: strip quotes we did not put there
    auto q = [](const std::string& s) {
        std::string o = "'";
        for (char c : s) { if (c == '\'') o += "'\\''"; else o += c; }
        return o + "'";
    };
    const char* curlBin = "curl";
    if (FILE* f = std::fopen("/usr/bin/curl", "rb")) { std::fclose(f); curlBin = "/usr/bin/curl"; }
    std::string cmd = std::string(curlBin) + " -s -L --compressed --max-time " + std::to_string(timeoutSec) +
                      " -A 'OrbitSDR' -w '\\n__HTTP_STATUS__%{http_code}'";
    for (const auto& hd : headers) cmd += " -H " + q(hd);
    cmd += " " + q(url) + " 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) { res.error = "curl not available"; return res; }
    char buf[16384];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), p)) > 0) res.body.append(buf, n);
    int rc = pclose(p);
    const std::string tag = "\n__HTTP_STATUS__";
    size_t pos = res.body.rfind(tag);
    if (pos != std::string::npos) {
        res.status = std::atoi(res.body.c_str() + pos + tag.size());
        res.body.resize(pos);
    }
    if (res.status == 0) res.error = rc != 0 ? "no connection (curl exit " + std::to_string(rc) + ")" : "no response";
#endif
    if (!res.ok() && res.error.empty()) res.error = "HTTP " + std::to_string(res.status);
    return res;
}

// One worker thread, requests in order. Callbacks run on the UI thread from poll().
// The state is shared with the worker, so the queue can be destroyed (app exit) while a
// request is still in flight: the detached worker finishes it and quits.
class HttpQueue {
public:
    using Callback = std::function<void(const HttpResult&)>;

    HttpQueue() : st(std::make_shared<State>()) {
        std::shared_ptr<State> s = st;
        std::thread([s] { run(s); }).detach();
    }
    ~HttpQueue() {
        { std::lock_guard<std::mutex> l(st->mtx); st->stopping = true; st->pending.clear(); }
        st->cv.notify_all();
    }
    HttpQueue(const HttpQueue&) = delete;
    HttpQueue& operator=(const HttpQueue&) = delete;

    // `urgent` requests (a search the user is waiting for) go before queued background downloads
    void get(const std::string& url, std::vector<std::string> headers, Callback cb, bool urgent = false) {
        {
            std::lock_guard<std::mutex> l(st->mtx);
            Req r{url, std::move(headers), std::move(cb)};
            if (urgent) st->pending.push_front(std::move(r)); else st->pending.push_back(std::move(r));
        }
        st->cv.notify_all();
    }

    // UI thread: deliver finished requests
    void poll() {
        std::deque<std::pair<Callback, HttpResult>> ready;
        { std::lock_guard<std::mutex> l(st->mtx); ready.swap(st->done); }
        for (auto& r : ready) if (r.first) r.first(r.second);
    }

    // (answers waiting for poll() count too: their callbacks may queue the next request)
    bool busy() const { std::lock_guard<std::mutex> l(st->mtx); return st->active || !st->pending.empty() || !st->done.empty(); }

private:
    struct Req { std::string url; std::vector<std::string> headers; Callback cb; };
    struct State {
        std::mutex mtx;
        std::condition_variable cv;
        std::deque<Req> pending;
        std::deque<std::pair<Callback, HttpResult>> done;
        bool stopping = false;
        bool active = false;
    };
    std::shared_ptr<State> st;

    static void run(std::shared_ptr<State> s) {
        for (;;) {
            Req r;
            {
                std::unique_lock<std::mutex> l(s->mtx);
                s->cv.wait(l, [&] { return s->stopping || !s->pending.empty(); });
                if (s->stopping) return;
                r = std::move(s->pending.front());
                s->pending.pop_front();
                s->active = true;
            }
            HttpResult res = httpGet(r.url, r.headers);
            {
                std::lock_guard<std::mutex> l(s->mtx);
                s->active = false;
                if (s->stopping) return;
                s->done.push_back({std::move(r.cb), std::move(res)});
            }
        }
    }
};

} // namespace orbit
