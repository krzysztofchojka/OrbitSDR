#pragma once
// =====================================================================
//  Crypto teaching blocks (included from Pipeline.h).
//
//  For labs that show, on your own made-up signals, how a message is
//  scrambled before it is sent and why the strength is all in the key:
//    - Key source: a key as bytes, fixed, counted (a loop over the key
//      space), random, or from a bank (a list) - the "memory of keys";
//    - Cipher: AES-128 / 256 (CTR or ECB), the strong cipher;
//    - the XOR bytes block (in PipelineLib.h) is the weak cipher: a
//      short repeating key, which the Brute force block below breaks by
//      trying every key and keeping the one whose result reads as text
//      (or matches a known start, or a CRC).
//  The point of the lab: a 1 - 3 byte key falls in seconds, and the
//  Brute force block prints how long the same search would take for a
//  real 128-bit key (longer than the age of the universe). A weak cipher
//  is worse than none: it only looks safe.
//
//  Nothing here attacks other people's transmissions - it works on the
//  frames made in the same pipeline. Breaking into someone else's radio
//  traffic is a crime; these blocks are a classroom, not a tool for it.
// =====================================================================

#include "../Core/CryptoLite.h"
#include <random>

// A key as a frame of bytes: fixed, a counter (loops over the key space), random, or a bank
class KeySource : public Block {
    std::mt19937 rng{std::random_device{}()};
    double t = 0.0;
    uint64_t counter = 0;
    size_t bankIdx = 0;
    std::vector<uint8_t> last;
    long long emitted = 0;
public:
    KeySource() {
        type = "keysrc"; title = "Key source"; category = "Crypto"; setOuts({{"key", PT::Frames}});
        addChoice("mode", "Keys", {"fixed", "counter (loop)", "random", "bank (list)"});
        addNum("bytes", "Key length", 1, 1, 32, 1, "bytes");
        addText("fixed", "Fixed key (hex)", "0x12");
        addText("bank", "Bank (one hex key per line)", "0x12\n0x37\n0xAB");
        addNum("every", "Every", 1.0, 0.01, 3600, 0.1, "s", 2);
        showOnly("bytes", "mode", {1, 2});
        showOnly("fixed", "mode", {0});
        showOnly("bank", "mode", {3});
    }
    std::string status() const override {
        if (last.empty()) return "no key yet";
        std::string s = "key " + crypto::toHex(last.data(), last.size());
        if (choice("mode") == 1) s += "   (" + std::to_string(counter) + ")";
        return s;
    }
    std::vector<std::string> bankKeys() const {
        std::vector<std::string> v;
        std::string line;
        std::istringstream in(text("bank"));
        while (std::getline(in, line)) { auto k = crypto::parseHex(line); if (!k.empty()) v.emplace_back(k.begin(), k.end()); }
        return v;
    }
    void reset() override { counter = 0; bankIdx = 0; t = 0.0; last.clear(); emitted = 0; }
    void process(const std::vector<const Stream*>&) override {
        Stream& o = out[0]; o.clear(); o.type = PT::Frames; o.rate = 0;
        t += tick;
        if (t < num("every")) return;
        t = 0.0;
        const int mode = choice("mode");
        const int n = std::clamp((int)num("bytes"), 1, 32);
        std::vector<uint8_t> key;
        if (mode == 0) {                                   // fixed
            key = crypto::parseHex(text("fixed"));
            if (key.empty()) key = {0};
        } else if (mode == 1) {                            // counter: big-endian, wrapping at 2^(8n)
            key.resize(n);
            for (int i = 0; i < n; i++) key[n - 1 - i] = (uint8_t)(counter >> (8 * i));
            const uint64_t span = n >= 8 ? ~0ull : (1ull << (8 * n));
            counter = (counter + 1) % (span ? span : 1);
        } else if (mode == 2) {                            // random
            key.resize(n);
            for (auto& b : key) b = (uint8_t)(rng() & 0xFF);
        } else {                                           // bank
            auto bank = bankKeys();
            if (bank.empty()) return;
            const std::string& k = bank[bankIdx % bank.size()];
            key.assign(k.begin(), k.end());
            bankIdx++;
        }
        last = key;
        emitted++;
        o.frames.push_back(key);
        o.newBurst = true;
    }
};

// AES (the strong cipher). CTR en/decrypts in place (the same call both ways); ECB needs the
// data padded to 16 bytes. The key is the parameter, or the "key" input when one is wired.
class CipherBlock : public Block {
public:
    CipherBlock() {
        type = "cipher"; title = "Cipher (AES)"; category = "Crypto";
        ins = {{"frames", PT::Frames}, {"key", PT::Frames}};
        setOuts({{"frames", PT::Frames}});
        addChoice("algo", "Algorithm", {"AES-128 CTR", "AES-256 CTR", "AES-128 ECB", "AES-256 ECB"});
        addText("key", "Key (hex; 16 or 32 bytes)", "000102030405060708090A0B0C0D0E0F");
        addText("nonce", "CTR nonce (hex, 16 bytes)", "0");
        showOnly("nonce", "algo", {0, 1});
    }
    std::string status() const override { return warnKey; }
    std::string warnKey;
    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0]; o.clear(); o.type = PT::Frames;
        if (!in[0]) return;
        o.rate = in[0]->rate; o.newBurst = in[0]->newBurst;
        const int algo = choice("algo");
        const bool ctr = algo < 2;
        const size_t want = (algo == 1 || algo == 3) ? 32 : 16;
        std::vector<uint8_t> key = in[1] && !in[1]->frames.empty() ? in[1]->frames.back() : crypto::parseHex(text("key"));
        if (key.size() != want) { warnKey = "key must be " + std::to_string(want) + " bytes (" + std::to_string(key.size()) + " given)"; return; }
        warnKey = ctr ? "AES CTR" : "AES ECB";
        crypto::Aes aes; aes.setKey(key.data(), key.size());
        uint8_t nonce[16] = {};
        if (ctr) { auto nv = crypto::parseHex(text("nonce")); for (size_t i = 0; i < nv.size() && i < 16; i++) nonce[i] = nv[i]; }
        for (const auto& f : in[0]->frames) {
            std::vector<uint8_t> g = f;
            if (ctr) aes.ctr(nonce, f.data(), g.data(), f.size());
            else { g.resize((f.size() + 15) / 16 * 16, 0); aes.ecbEncrypt(f.data(), g.data(), g.size()); }   // ECB: this block only encrypts
            o.frames.push_back(std::move(g));
        }
    }
};

// Break a short repeating-XOR key: try every key of the chosen length, keep the one whose
// result passes a test (reads as text / starts with a known string / CRC-16 checks). The work
// is spread over the passes so the UI stays live, with progress, rate and the time the same
// search would take for longer keys.
class XorBrute : public Block {
    std::vector<uint8_t> ct;        // the ciphertext being worked on
    uint64_t tried = 0, total = 0;
    bool done = false, found = false, emittedBest = false;
    std::vector<uint8_t> foundKey, plain;
    double bestScore = -1e18;       // the best "readable text" candidate so far
    std::vector<uint8_t> bestKey, bestPlain;
    double rate = 0.0;              // keys per second (smoothed)
    std::chrono::steady_clock::time_point t0;

    static uint16_t crc16(const uint8_t* p, size_t n) {   // CRC-16/CCITT-FALSE
        uint16_t c = 0xFFFF;
        for (size_t i = 0; i < n; i++) { c ^= (uint16_t)p[i] << 8; for (int b = 0; b < 8; b++) c = (c & 0x8000) ? (c << 1) ^ 0x1021 : c << 1; }
        return c;
    }
    // how text-like the bytes are: letters and spaces count most, other printable a little,
    // anything unprintable is a strong penalty (so real text beats printable-looking garbage)
    static double textScore(const std::vector<uint8_t>& g) {
        if (g.empty()) return -1e18;
        double s = 0;
        for (uint8_t b : g) {
            if (b == ' ' || (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z')) s += 2;
            else if ((b >= '0' && b <= '9') || b == '\t' || b == '\n' || b == '\r') s += 1;
            else if (b >= 0x20 && b < 0x7F) s += 0.2;    // punctuation
            else s -= 6;                                  // control / high bytes: not text
        }
        return s / g.size();
    }
    // a definitive test (known start / CRC): the one right key passes. "" for readable text,
    // which is scored across all keys instead.
    bool definitive(const std::vector<uint8_t>& g) const {
        if (choice("test") == 1) {
            const std::string pre = text("prefix");
            return !pre.empty() && g.size() >= pre.size() && std::equal(pre.begin(), pre.end(), g.begin());
        }
        if (choice("test") == 2) {
            if (g.size() < 3) return false;
            return crc16(g.data(), g.size() - 2) == ((uint16_t)g[g.size() - 2] << 8 | g[g.size() - 1]);
        }
        return false;
    }
    void restart() { tried = 0; done = false; found = false; emittedBest = false; foundKey.clear(); plain.clear();
                     bestScore = -1e18; bestKey.clear(); bestPlain.clear(); rate = 0.0; t0 = std::chrono::steady_clock::now(); }

public:
    XorBrute() {
        type = "xorbrute"; title = "Brute force (XOR key)"; category = "Crypto";
        ins = {{"ciphertext", PT::Frames}}; setOuts({{"plaintext", PT::Frames}});
        addNum("bytes", "Key length to try", 2, 1, 4, 1, "bytes");
        addChoice("test", "Recognise plaintext by", {"readable text", "known start", "CRC-16/CCITT"});
        addText("prefix", "Known start", "");
        showOnly("prefix", "test", {1});
    }
    void reset() override { ct.clear(); restart(); }
    std::string status() const override {
        if (ct.empty()) return "waiting for ciphertext";
        if (found) return "found key " + crypto::toHex(foundKey.data(), foundKey.size());
        if (done) return "no key passed the test";
        char b[64]; std::snprintf(b, sizeof(b), "trying keys: %.1f %%", total ? 100.0 * tried / total : 0.0);
        return b;
    }
    std::vector<std::string> buttons() const override { return {"Restart"}; }
    void onButton(int) override { restart(); }
    sf::Vector2f viewSize() const override { return {440.0f, 150.0f}; }

    static std::string humanTime(double s) {
        if (s < 1) return "< 1 s";
        const char* u[] = {"s", "min", "h", "days", "years"};
        double v[] = {s, s / 60, s / 3600, s / 86400, s / 31557600.0};
        int i = 0; while (i < 4 && v[i + 1] >= 1) i++;
        char b[48];
        if (i == 4 && v[4] > 1e6) { std::snprintf(b, sizeof(b), "%.1e years", v[4]); return b; }
        std::snprintf(b, sizeof(b), "%.1f %s", v[i], u[i]);
        return b;
    }

    void process(const std::vector<const Stream*>& in) override {
        Stream& o = out[0]; o.clear(); o.type = PT::Frames;
        if (in[0]) { o.rate = in[0]->rate; if (!in[0]->frames.empty()) { ct = in[0]->frames.back(); restart(); } }
        const int n = std::clamp((int)num("bytes"), 1, 4);
        total = 1; for (int i = 0; i < n; i++) total *= 256;
        if (ct.empty() || done) return;
        const bool scoring = choice("test") == 0;   // readable text: keep the best over the whole space
        // a budget of real time per pass, so the UI stays responsive on any key length
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(60);
        std::vector<uint8_t> g(ct.size());
        while (tried < total && !done) {
            // the key is `tried` as n big-endian bytes
            uint8_t key[4];
            for (int i = 0; i < n; i++) key[n - 1 - i] = (uint8_t)(tried >> (8 * i));
            for (size_t i = 0; i < ct.size(); i++) g[i] = ct[i] ^ key[i % n];
            tried++;
            if (scoring) {
                const double sc = textScore(g);
                if (sc > bestScore) { bestScore = sc; bestKey.assign(key, key + n); bestPlain = g; }
            } else if (definitive(g)) {
                found = true; done = true; foundKey.assign(key, key + n); plain = g;
                o.frames.push_back(g); o.newBurst = true; break;
            }
            if ((tried & 0x3FFF) == 0 && std::chrono::steady_clock::now() > deadline) break;
        }
        if (tried >= total) {
            done = true;
            if (scoring && !bestKey.empty()) { found = true; foundKey = bestKey; plain = bestPlain; }
        }
        // the best readable candidate is emitted once the whole space has been scored
        if (scoring && done && found && !emittedBest) { emittedBest = true; o.frames.push_back(plain); o.newBurst = true; }
        const double el = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (el > 0) rate = 0.8 * rate + 0.2 * (tried / el);
    }

    void drawView(const Draw& d, const sf::FloatRect& r) override {
        d.rect(r, sf::Color(6, 10, 14), sf::Color(50, 60, 75));
        float y = r.position.y + 5;
        auto line = [&](const std::string& s, sf::Color c) { d.text(s, r.position.x + 6, y, 11, c); y += 15.0f; };
        if (ct.empty()) { line("waiting for ciphertext on the input", sf::Color(150, 150, 160)); return; }
        const int n = std::clamp((int)num("bytes"), 1, 4);
        char b[128];
        // progress bar
        const float bw = r.size.x - 12, bx = r.position.x + 6;
        d.rect({{bx, y}, {bw, 12}}, sf::Color(20, 24, 30), sf::Color(70, 80, 95));
        const double frac = total ? (double)tried / total : 0;
        d.rect({{bx, y}, {(float)(bw * frac), 12}}, found ? sf::Color(90, 200, 120) : sf::Color(90, 150, 220));
        y += 18.0f;
        std::snprintf(b, sizeof(b), "%d-byte key: tried %llu of %llu  (%.1f %%)", n, (unsigned long long)tried, (unsigned long long)total, 100.0 * frac);
        line(b, sf::Color(220, 220, 225));
        std::snprintf(b, sizeof(b), "%.0f keys/s", rate);
        line(b, sf::Color(150, 160, 175));
        if (found) {
            line("FOUND key: " + crypto::toHex(foundKey.data(), foundKey.size()), sf::Color(120, 240, 150));
            std::string txt; for (uint8_t c : plain) txt += (c >= 0x20 && c < 0x7F) ? (char)c : '.';
            if (txt.size() > 52) txt = txt.substr(0, 51) + ".";
            line("plaintext: " + txt, sf::Color(200, 230, 200));
        } else if (done) {
            line("no key passed the test (wrong length or test?)", sf::Color(240, 140, 120));
        } else if (rate > 0) {
            line("this length ETA: " + humanTime((total - tried) / rate), sf::Color(200, 200, 110));
            // what a real key would cost at this rate
            std::snprintf(b, sizeof(b), "a 128-bit key at this rate: %s", humanTime(std::pow(2.0, 128) / rate).c_str());
            line(b, sf::Color(255, 180, 90));
        }
    }
};
