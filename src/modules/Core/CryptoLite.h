#pragma once

// =====================================================================
//  Small, self-contained crypto primitives for the pipelines (the Script
//  block's aes_* / sha256 / hmac functions, and any block that needs
//  them). Header only, namespace `crypto`, no dependency on the decoder
//  modules. The AES and SHA-256 are the same implementations used by the
//  Meshtastic / MeshCore decoders, checked against the FIPS-197 and
//  NIST test vectors; CryptoTest in the scratchpad re-checks them.
//
//  These are for the teaching labs (show how a signal is scrambled and
//  why a short key is weak) and for decoding real protocols that use
//  standard crypto. They are textbook implementations: correct, not
//  hardened (no constant-time guarantee), which is all a lab needs.
// =====================================================================

#include <cstdint>
#include <cstring>
#include <array>
#include <string>
#include <algorithm>

namespace crypto {

class Aes {
    std::array<uint8_t, 240> rk {};
    int rounds = 0;
    static const std::array<uint8_t, 256>& sbox() {
        static const std::array<uint8_t, 256> s = [] {
            std::array<uint8_t, 256> t {};
            uint8_t p = 1, q = 1;
            auto rotl = [](uint8_t x, int k) { return (uint8_t)((x << k) | (x >> (8 - k))); };
            do {
                p = (uint8_t)(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));
                q ^= (uint8_t)(q << 1); q ^= (uint8_t)(q << 2); q ^= (uint8_t)(q << 4);
                if (q & 0x80) q ^= 0x09;
                t[p] = (uint8_t)(q ^ rotl(q, 1) ^ rotl(q, 2) ^ rotl(q, 3) ^ rotl(q, 4) ^ 0x63);
            } while (p != 1);
            t[0] = 0x63;
            return t;
        }();
        return s;
    }
    static uint8_t xt(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }
    static const std::array<uint8_t, 256>& invSbox() {
        static const std::array<uint8_t, 256> s = [] {
            std::array<uint8_t, 256> t {};
            for (int i = 0; i < 256; i++) t[sbox()[i]] = (uint8_t)i;
            return t;
        }();
        return s;
    }
    static uint8_t mul(uint8_t a, uint8_t b) {
        uint8_t r = 0;
        while (b) { if (b & 1) r ^= a; a = xt(a); b >>= 1; }
        return r;
    }

public:
    // key: 16 bytes (AES-128) or 32 bytes (AES-256); false otherwise
    bool setKey(const uint8_t* key, size_t len) {
        if (len != 16 && len != 32) return false;
        const auto& S = sbox();
        const int nk = (int)len / 4;
        rounds = nk + 6;
        std::memcpy(rk.data(), key, len);
        uint8_t rcon = 1;
        for (int i = nk; i < 4 * (rounds + 1); i++) {
            uint8_t t[4];
            std::memcpy(t, &rk[(i - 1) * 4], 4);
            if (i % nk == 0) {
                const uint8_t a = t[0];
                t[0] = (uint8_t)(S[t[1]] ^ rcon); t[1] = S[t[2]]; t[2] = S[t[3]]; t[3] = S[a];
                rcon = xt(rcon);
            } else if (nk > 6 && i % nk == 4) {
                for (auto& v : t) v = S[v];
            }
            for (int k = 0; k < 4; k++) rk[i * 4 + k] = (uint8_t)(rk[(i - nk) * 4 + k] ^ t[k]);
        }
        return true;
    }
    void encryptBlock(const uint8_t in[16], uint8_t out[16]) const {
        const auto& S = sbox();
        uint8_t s[16];
        for (int i = 0; i < 16; i++) s[i] = (uint8_t)(in[i] ^ rk[i]);
        for (int r = 1; r <= rounds; r++) {
            uint8_t t[16];
            for (int c = 0; c < 4; c++) for (int row = 0; row < 4; row++) t[row + 4 * c] = S[s[row + 4 * ((c + row) & 3)]];
            if (r != rounds) {
                for (int c = 0; c < 4; c++) {
                    uint8_t* col = &t[4 * c];
                    const uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3], all = (uint8_t)(a0 ^ a1 ^ a2 ^ a3);
                    col[0] ^= (uint8_t)(all ^ xt((uint8_t)(a0 ^ a1)));
                    col[1] ^= (uint8_t)(all ^ xt((uint8_t)(a1 ^ a2)));
                    col[2] ^= (uint8_t)(all ^ xt((uint8_t)(a2 ^ a3)));
                    col[3] ^= (uint8_t)(all ^ xt((uint8_t)(a3 ^ a0)));
                }
            }
            for (int i = 0; i < 16; i++) s[i] = (uint8_t)(t[i] ^ rk[r * 16 + i]);
        }
        std::memcpy(out, s, 16);
    }
    void decryptBlock(const uint8_t in[16], uint8_t out[16]) const {
        const auto& I = invSbox();
        uint8_t s[16];
        for (int i = 0; i < 16; i++) s[i] = (uint8_t)(in[i] ^ rk[rounds * 16 + i]);
        for (int r = rounds - 1; r >= 0; r--) {
            uint8_t t[16];
            for (int c = 0; c < 4; c++) for (int row = 0; row < 4; row++) t[row + 4 * ((c + row) & 3)] = I[s[row + 4 * c]];
            for (int i = 0; i < 16; i++) t[i] ^= rk[r * 16 + i];
            if (r > 0) {
                for (int c = 0; c < 4; c++) {
                    uint8_t* col = &t[4 * c];
                    const uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                    col[0] = (uint8_t)(mul(a0, 14) ^ mul(a1, 11) ^ mul(a2, 13) ^ mul(a3, 9));
                    col[1] = (uint8_t)(mul(a0, 9) ^ mul(a1, 14) ^ mul(a2, 11) ^ mul(a3, 13));
                    col[2] = (uint8_t)(mul(a0, 13) ^ mul(a1, 9) ^ mul(a2, 14) ^ mul(a3, 11));
                    col[3] = (uint8_t)(mul(a0, 11) ^ mul(a1, 13) ^ mul(a2, 9) ^ mul(a3, 14));
                }
            }
            std::memcpy(s, t, 16);
        }
        std::memcpy(out, s, 16);
    }
    // AES-CTR (the counter is the last 4 bytes of the 16-byte nonce, big-endian). En- and
    // decryption are the same call.
    void ctr(const uint8_t nonce[16], const uint8_t* in, uint8_t* out, size_t n) const {
        uint8_t cnt[16], ks[16];
        std::memcpy(cnt, nonce, 16);
        for (size_t off = 0; off < n; off += 16) {
            encryptBlock(cnt, ks);
            for (size_t k = 0; k < 16 && off + k < n; k++) out[off + k] = (uint8_t)(in[off + k] ^ ks[k]);
            for (int k = 15; k >= 12; k--) if (++cnt[k] != 0) break;
        }
    }
    // ECB over whole 16-byte blocks; a short tail is left as it is (the caller pads)
    void ecbEncrypt(const uint8_t* in, uint8_t* out, size_t n) const { for (size_t o = 0; o + 16 <= n; o += 16) encryptBlock(in + o, out + o); }
    void ecbDecrypt(const uint8_t* in, uint8_t* out, size_t n) const { for (size_t o = 0; o + 16 <= n; o += 16) decryptBlock(in + o, out + o); }
};

class Sha256 {
    uint32_t h[8];
    uint8_t buf[64];
    size_t bl = 0;
    uint64_t total = 0;
    static uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void block(const uint8_t* p) {
        static const uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
            0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
            0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
            0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
            0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
        for (int i = 16; i < 64; i++) {
            const uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
        for (int i = 0; i < 64; i++) {
            const uint32_t t1 = k + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
            const uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
    }

public:
    Sha256() {
        static const uint32_t H0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        std::memcpy(h, H0, sizeof(h));
    }
    void update(const uint8_t* p, size_t n) {
        total += n;
        while (n) {
            const size_t k = std::min(n, 64 - bl);
            std::memcpy(buf + bl, p, k);
            bl += k; p += k; n -= k;
            if (bl == 64) { block(buf); bl = 0; }
        }
    }
    std::array<uint8_t, 32> digest() {
        const uint64_t bits = total * 8;
        const uint8_t one = 0x80, zero = 0;
        update(&one, 1);
        while (bl != 56) update(&zero, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
        update(len, 8);
        std::array<uint8_t, 32> out {};
        for (int i = 0; i < 8; i++) for (int k = 0; k < 4; k++) out[4 * i + k] = (uint8_t)(h[i] >> (24 - 8 * k));
        return out;
    }
    static std::array<uint8_t, 32> of(const uint8_t* p, size_t n) { Sha256 s; s.update(p, n); return s.digest(); }
};

inline std::array<uint8_t, 32> hmacSha256(const uint8_t* key, size_t klen, const uint8_t* msg, size_t n) {
    uint8_t k[64] = {};
    if (klen > 64) { auto d = Sha256::of(key, klen); std::memcpy(k, d.data(), 32); }
    else std::memcpy(k, key, klen);
    uint8_t ip[64], op[64];
    for (int i = 0; i < 64; i++) { ip[i] = (uint8_t)(k[i] ^ 0x36); op[i] = (uint8_t)(k[i] ^ 0x5c); }
    Sha256 a; a.update(ip, 64); a.update(msg, n);
    const auto inner = a.digest();
    Sha256 b; b.update(op, 64); b.update(inner.data(), 32);
    return b.digest();
}

// hex <-> bytes (for key parameters written as "0x12 AB 3F" or "12AB3F")
inline std::vector<uint8_t> parseHex(const std::string& s) {
    std::vector<uint8_t> v;
    int hi = -1;
    for (size_t i = 0; i < s.size(); i++) {
        const char c = s[i];
        if (c == '0' && i + 1 < s.size() && (s[i + 1] == 'x' || s[i + 1] == 'X')) { i++; continue; }
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else { hi = -1; continue; }   // a separator ends the current byte
        if (hi < 0) hi = d; else { v.push_back((uint8_t)(hi << 4 | d)); hi = -1; }
    }
    return v;
}
inline std::string toHex(const uint8_t* p, size_t n) {
    static const char* h = "0123456789ABCDEF";
    std::string s;
    for (size_t i = 0; i < n; i++) { if (i) s += ' '; s += h[p[i] >> 4]; s += h[p[i] & 15]; }
    return s;
}

}  // namespace crypto
