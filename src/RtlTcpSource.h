#pragma once
#include <string>
#include <vector>
#include <complex>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <cerrno>
#include "RingBuffer.h"

#ifdef _WIN32
  #ifndef NOMINMAX
  #define NOMINMAX
  #endif
  #include <winsock2.h>   // musi być PRZED windows.h (patrz main.cpp)
  #include <ws2tcpip.h>
  typedef SOCKET sock_t;
  #define SOCK_INVALID INVALID_SOCKET
  #define SOCK_CLOSE closesocket
  #ifndef MSG_NOSIGNAL
  #define MSG_NOSIGNAL 0
  #endif
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <sys/select.h>
  typedef int sock_t;
  #define SOCK_INVALID (-1)
  #define SOCK_CLOSE ::close
  #ifndef MSG_NOSIGNAL
  #define MSG_NOSIGNAL 0
  #endif
#endif

// Klient protokołu rtl_tcp (https://github.com/osmocom/rtl-sdr -> rtl_tcp.c)
//  - po połączeniu serwer wysyła 12 bajtów: "RTL0" + tuner_type(BE32) + gain_count(BE32)
//  - komendy klient->serwer: 5 bajtów [cmd][param BE32]
//  - dane serwer->klient: IQ 8-bit unsigned, przeplot I,Q,I,Q...
class RtlTcpSource : public IQSource {
    enum Cmd : uint8_t {
        CMD_FREQ = 0x01, CMD_SAMPLERATE = 0x02, CMD_GAIN_MODE = 0x03,
        CMD_GAIN = 0x04, CMD_AGC_MODE = 0x08, CMD_DIRECT_SAMPLING = 0x09,
        CMD_BIAS_TEE = 0x0E   // tylko nowsze wersje rtl_tcp, starsze zignorują
    };

    sock_t sock = SOCK_INVALID;
    std::thread worker;
    std::atomic<bool> running {false};   // wątek odbiorczy żyje
    std::atomic<bool> active {false};    // play/pause (przy pauzie dane są wyrzucane)
    std::atomic<bool> connectionLost {false};
    std::mutex sendMtx;
    RingBuffer<Complex> ringBuffer;
    uint32_t sampleRate = 2048000;
    uint32_t tunerType = 0, gainCount = 0;
    std::atomic<long long> centerFreq {100000000};

    static const int BUF = 65536;

    // ---------- helpery sieciowe ----------
    static bool initSockets() {
#ifdef _WIN32
        static bool ok = []() { WSADATA w; return WSAStartup(MAKEWORD(2, 2), &w) == 0; }();
        return ok;
#else
        return true;
#endif
    }
    static void setNonBlocking(sock_t s, bool nb) {
#ifdef _WIN32
        u_long m = nb ? 1 : 0; ioctlsocket(s, FIONBIO, &m);
#else
        int f = fcntl(s, F_GETFL, 0);
        fcntl(s, F_SETFL, nb ? (f | O_NONBLOCK) : (f & ~O_NONBLOCK));
#endif
    }
    static void setRecvTimeout(sock_t s, int ms) {
#ifdef _WIN32
        DWORD t = (DWORD)ms;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof(t));
#else
        timeval tv; tv.tv_sec = ms / 1000; tv.tv_usec = (ms % 1000) * 1000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    }
    static bool wouldTimeout() {
#ifdef _WIN32
        int e = WSAGetLastError();
        return e == WSAETIMEDOUT || e == WSAEWOULDBLOCK;
#else
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
    }

    static sock_t connectWithTimeout(const std::string& host, const std::string& port, int timeoutMs) {
        addrinfo hints; std::memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0) return SOCK_INVALID;

        sock_t result = SOCK_INVALID;
        for (addrinfo* p = res; p; p = p->ai_next) {
            sock_t c = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
            if (c == SOCK_INVALID) continue;
            setNonBlocking(c, true);
            bool ok = (connect(c, p->ai_addr, (int)p->ai_addrlen) == 0);
            if (!ok) {
                fd_set wf; FD_ZERO(&wf); FD_SET(c, &wf);
                timeval tv; tv.tv_sec = timeoutMs / 1000; tv.tv_usec = (timeoutMs % 1000) * 1000;
                if (select((int)c + 1, nullptr, &wf, nullptr, &tv) > 0) {
                    int err = 0; socklen_t len = sizeof(err);
                    getsockopt(c, SOL_SOCKET, SO_ERROR, (char*)&err, &len);
                    ok = (err == 0);
                }
            }
            if (ok) { setNonBlocking(c, false); result = c; break; }
            SOCK_CLOSE(c);
        }
        freeaddrinfo(res);
        return result;
    }

    void sendCmd(uint8_t cmd, uint32_t param) {
        if (sock == SOCK_INVALID) return;
        uint8_t b[5] = { cmd, (uint8_t)(param >> 24), (uint8_t)(param >> 16),
                         (uint8_t)(param >> 8), (uint8_t)param };
        std::lock_guard<std::mutex> l(sendMtx);
        send(sock, (const char*)b, 5, MSG_NOSIGNAL);
    }

    void workerLoop() {
        std::vector<uint8_t> buf(BUF + 1);
        std::vector<Complex> conv(BUF / 2 + 1);
        size_t carry = 0;                       // ewentualny nieparzysty bajt z poprzedniego recv
        const double inv = 1.0 / 127.5;
        while (running) {
            int n = recv(sock, (char*)buf.data() + carry, BUF, 0);
            if (n > 0) {
                if (!active) { carry = 0; continue; }   // pauza: opróżniamy gniazdo
                size_t total = carry + (size_t)n;
                size_t samples = total / 2;
                for (size_t i = 0; i < samples; i++)
                    conv[i] = Complex((buf[2 * i] - 127.5) * inv, (buf[2 * i + 1] - 127.5) * inv);
                ringBuffer.push(conv.data(), samples);
                carry = total % 2;
                if (carry) buf[0] = buf[total - 1];
            } else if (n == 0) {
                connectionLost = true; break;           // serwer zamknął połączenie
            } else {
                if (wouldTimeout()) continue;
                connectionLost = true; break;
            }
        }
    }

public:
    RtlTcpSource() : ringBuffer(4 * 1024 * 1024) {}
    ~RtlTcpSource() { close(); }

    // id = "host" albo "host:port" (domyślnie 1234)
    bool open(std::string id, uint32_t requestedRate = 0) override {
        close();
        if (!initSockets()) return false;

        std::string host = id, port = "1234";
        size_t c = id.rfind(':');
        if (c != std::string::npos) { host = id.substr(0, c); port = id.substr(c + 1); }
        auto trim = [](std::string& s) {
            while (!s.empty() && isspace((unsigned char)s.front())) s.erase(s.begin());
            while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
        };
        trim(host); trim(port);
        if (host.empty()) return false;
        if (port.empty()) port = "1234";

        sock = connectWithTimeout(host, port, 3000);
        if (sock == SOCK_INVALID) return false;

        int one = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
#ifdef SO_NOSIGPIPE
        setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, (const char*)&one, sizeof(one));
#endif
        // --- nagłówek 12 B ("RTL0") ---
        setRecvTimeout(sock, 500);
        uint8_t hdr[12]; int got = 0;
        auto t0 = std::chrono::steady_clock::now();
        while (got < 12 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3)) {
            int n = recv(sock, (char*)hdr + got, 12 - got, 0);
            if (n > 0) got += n;
            else if (n == 0) break;
            else if (!wouldTimeout()) break;
        }
        if (got < 12 || std::memcmp(hdr, "RTL0", 4) != 0) {
            SOCK_CLOSE(sock); sock = SOCK_INVALID; return false;
        }
        tunerType = ((uint32_t)hdr[4] << 24) | (hdr[5] << 16) | (hdr[6] << 8) | hdr[7];
        gainCount = ((uint32_t)hdr[8] << 24) | (hdr[9] << 16) | (hdr[10] << 8) | hdr[11];

        sampleRate = requestedRate > 0 ? requestedRate : 2048000;
        sendCmd(CMD_SAMPLERATE, sampleRate);
        sendCmd(CMD_FREQ, (uint32_t)centerFreq.load());
        sendCmd(CMD_GAIN_MODE, 0);   // auto
        sendCmd(CMD_AGC_MODE, 1);

        connectionLost = false;
        running = true;
        worker = std::thread(&RtlTcpSource::workerLoop, this);
        return true;
    }

    void close() override {
        running = false;
        active = false;
        if (sock != SOCK_INVALID) {
#ifdef _WIN32
            shutdown(sock, SD_BOTH);
#else
            shutdown(sock, SHUT_RDWR);
#endif
        }
        if (worker.joinable()) worker.join();
        if (sock != SOCK_INVALID) { SOCK_CLOSE(sock); sock = SOCK_INVALID; }
    }

    void start() override { ringBuffer.clear(); active = true; }
    void stop() override { active = false; }

    int read(Complex* buffer, int count) override { return (int)ringBuffer.pop(buffer, count); }
    double getSampleRate() override { return (double)sampleRate; }
    bool isHardware() override { return true; }
    bool isConnectionLost() const { return connectionLost; }

    void setCenterFrequency(long long hz) override {
        centerFreq = hz;
        sendCmd(CMD_FREQ, (uint32_t)hz);
        ringBuffer.clear();
    }

    void setGain(int db) override {
        if (db == -1) {
            sendCmd(CMD_GAIN_MODE, 0);
            sendCmd(CMD_AGC_MODE, 1);
        } else {
            sendCmd(CMD_GAIN_MODE, 1);
            sendCmd(CMD_AGC_MODE, 0);
            sendCmd(CMD_GAIN, (uint32_t)(db * 10));   // w 0.1 dB, jak w librtlsdr
        }
        ringBuffer.clear();
    }

    void setHardwareOption(std::string name, int value) override {
        if (name == "direct_sampling") sendCmd(CMD_DIRECT_SAMPLING, (uint32_t)value);
        else if (name == "bias_t")     sendCmd(CMD_BIAS_TEE, value ? 1 : 0);
    }

    std::vector<std::string> getAvailableSampleRatesText() override {
        return {"1.4 MSps", "1.8 MSps", "2.048 MSps", "2.4 MSps", "3.2 MSps"};
    }
    std::vector<uint32_t> getAvailableSampleRatesValues() override {
        return {1400000, 1800000, 2048000, 2400000, 3200000};
    }
};