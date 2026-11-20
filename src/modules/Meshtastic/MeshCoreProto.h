#pragma once

// =====================================================================
//  MeshCore: what is inside a LoRa packet of a MeshCore node (another
//  LoRa mesh, github.com/meshcore-dev/MeshCore; checked against its
//  src/Packet.h, src/Mesh.cpp, src/Utils.cpp, src/helpers/*).
//
//  Radio: sync word 0x12; in Europe usually 869.618 MHz, 62.5 kHz, SF8,
//  CR 4/8 (the "EU/UK narrow" settings; heard in Gdynia), a 32-symbol
//  preamble at SF <= 8, 16 above.
//  Packet (no encryption of the header):
//    header (route type bits 0-1, payload type bits 2-5, version 6-7)
//    [transport codes, 4 bytes, for the route types 0 and 3]
//    path length (bits 0-5 how many hashes, 6-7 the hash size - 1)
//    path (the repeaters a flood packet went through, a hash each)
//    payload
//  Payloads: ADVERT = public key (32) + time (4) + signature (64) + app
//  data (flags, latitude / longitude x 1e6, name): a node announcing
//  itself, not encrypted. GRP_TXT = channel hash (1) + MAC (2) + AES-128
//  ECB ciphertext; the "Public" channel's key is well known, so its texts
//  ("name: message") can be read. Direct messages, requests and answers
//  are encrypted per pair of nodes (ECDH): only shown as such.
//  MAC: the first 2 bytes of HMAC-SHA256 over the ciphertext, the key
//  being the channel secret padded to 32 bytes. Channel hash: the first
//  byte of SHA-256 of the secret.
// =====================================================================

#include "MeshtasticCore.h"   // mesh::Aes, base64Decode
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include <map>
#include <array>
#include <cctype>
#include <algorithm>

namespace mcore {

// ---------------------------------------------------------------------
//  SHA-256 and HMAC-SHA256
// ---------------------------------------------------------------------
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

// ---------------------------------------------------------------------
//  Names
// ---------------------------------------------------------------------
enum { ROUTE_TRANSPORT_FLOOD = 0, ROUTE_FLOOD = 1, ROUTE_DIRECT = 2, ROUTE_TRANSPORT_DIRECT = 3 };
enum { PT_REQ = 0, PT_RESPONSE = 1, PT_TXT_MSG = 2, PT_ACK = 3, PT_ADVERT = 4, PT_GRP_TXT = 5, PT_GRP_DATA = 6,
       PT_ANON_REQ = 7, PT_PATH = 8, PT_TRACE = 9, PT_MULTIPART = 10, PT_CONTROL = 11, PT_RAW_CUSTOM = 15 };

inline const char* typeName(int t) {
    switch (t) {
    case PT_REQ: return "request"; case PT_RESPONSE: return "response"; case PT_TXT_MSG: return "direct message";
    case PT_ACK: return "ack"; case PT_ADVERT: return "advert"; case PT_GRP_TXT: return "group text";
    case PT_GRP_DATA: return "group data"; case PT_ANON_REQ: return "anonymous request"; case PT_PATH: return "path";
    case PT_TRACE: return "trace"; case PT_MULTIPART: return "multipart"; case PT_CONTROL: return "control";
    case PT_RAW_CUSTOM: return "custom";
    default: return "?";
    }
}
inline const char* nodeTypeName(int t) {
    switch (t) { case 1: return "companion"; case 2: return "repeater"; case 3: return "room server"; case 4: return "sensor"; default: return "?"; }
}

// ---------------------------------------------------------------------
//  Channels (group keys)
// ---------------------------------------------------------------------
struct Channel {
    std::string name;
    std::array<uint8_t, 32> secret {};   // 16 or 32 bytes used, the rest zero (it is the HMAC key)
    size_t len = 16;
    uint8_t hash = 0;
    mesh::Aes aes;
};

inline bool makeChannel(const std::string& name, const std::vector<uint8_t>& key, Channel& c) {
    if (key.size() != 16 && key.size() != 32) return false;
    c.name = name;
    c.secret.fill(0);
    std::memcpy(c.secret.data(), key.data(), key.size());
    c.len = key.size();
    c.hash = Sha256::of(c.secret.data(), c.len)[0];
    c.aes.setKey(c.secret.data(), 16);   // AES-128 with the first 16 bytes
    return true;
}
// a "#hashtag" channel: its secret is the first 16 bytes of SHA-256 of its name
inline bool hashtagChannel(const std::string& name, Channel& c) {
    const auto d = Sha256::of((const uint8_t*)name.data(), name.size());
    return makeChannel(name, std::vector<uint8_t>(d.begin(), d.begin() + 16), c);
}

inline std::string hex(const uint8_t* p, size_t n) {
    std::string s;
    char b[4];
    for (size_t i = 0; i < n; i++) { std::snprintf(b, sizeof(b), "%02x", p[i]); s += b; }
    return s;
}

// ---------------------------------------------------------------------
//  Nodes and the decoder
// ---------------------------------------------------------------------
struct Node {
    std::array<uint8_t, 32> key {};
    std::string name;
    int type = 0;
    bool hasPos = false;
    double lat = 0, lon = 0;
    int hops = -1;
    double snr = -1000;
    double lastSeen = 0, firstSeen = 0;
    uint32_t advertTime = 0;
    int packets = 0;
    std::string id() const { return hex(key.data(), 4); }
    std::string label() const { return name.empty() ? id() : name; }
    // the badge: a leading emoji (most MeshCore names start with one), else two letters
    std::string badge() const {
        if (name.empty()) return hex(key.data(), 2);
        const unsigned char c0 = (unsigned char)name[0];
        if (c0 >= 0xC0) {   // a non-ASCII character first: take it (and a following variation selector)
            size_t n = c0 >= 0xF0 ? 4 : c0 >= 0xE0 ? 3 : 2;
            if (name.size() >= n + 3 && (unsigned char)name[n] == 0xEF && (unsigned char)name[n + 1] == 0xB8) n += 3;
            return name.substr(0, std::min(n, name.size()));
        }
        std::string b;
        for (char ch : name) {
            if (std::isalnum((unsigned char)ch)) b += (char)std::toupper((unsigned char)ch);
            if (b.size() == 2) break;
        }
        return b.empty() ? hex(key.data(), 2) : b;
    }
    uint32_t rgb() const { return (uint32_t)key[0] << 16 | (uint32_t)key[1] << 8 | key[2]; }
};

struct Event {
    double t = 0;
    int type = -1;
    std::string from;          // the node (name), when known
    std::string text;
    bool isText = false;       // a text that could be read (highlighted)
    bool advert = false;       // node announced: `key` is its public key
    std::array<uint8_t, 32> key {};
    int hops = -1;
    double snr = 0;
};

class Decoder {
    std::vector<Channel> channels;
    std::map<std::string, double> seen;   // packet hash (type + payload) -> when; floods come again via repeaters

public:
    std::map<std::string, Node> nodes;    // by public key (hex)
    std::vector<Event> events;
    long long packets = 0, decoded = 0, encrypted = 0, duplicates = 0, notMesh = 0;

    Decoder() {
        Channel c;
        if (makeChannel("Public", mesh::base64Decode("izOH6cXN6mrJ5e26oRXNcg=="), c)) channels.push_back(c);
    }
    bool addChannel(const std::string& name, const std::string& keyBase64) {
        Channel c;
        if (!name.empty() && name[0] == '#' && keyBase64.empty()) { if (!hashtagChannel(name, c)) return false; }
        else if (!makeChannel(name, mesh::base64Decode(keyBase64), c)) return false;
        channels.push_back(c);
        return true;
    }
    void clear() { nodes.clear(); events.clear(); seen.clear(); packets = decoded = encrypted = duplicates = notMesh = 0; }

    // the node whose public key starts with these bytes (direct messages name nodes by a
    // 1..3 byte hash, the first bytes of the key); "" when none or more than one
    std::string whoIs(const uint8_t* h, size_t n) const {
        const Node* f = nullptr;
        for (const auto& kv : nodes) if (std::memcmp(kv.second.key.data(), h, n) == 0) { if (f) return ""; f = &kv.second; }
        return f ? f->label() : "";
    }

    // One LoRa payload; false: it is no MeshCore packet
    bool handle(const std::vector<uint8_t>& f, double t, double snr) {
        if (f.size() < 3) { notMesh++; return false; }
        const int route = f[0] & 3, type = (f[0] >> 2) & 15, ver = f[0] >> 6;
        if (ver != 0) { notMesh++; return false; }
        size_t i = 1;
        if (route == ROUTE_TRANSPORT_FLOOD || route == ROUTE_TRANSPORT_DIRECT) i += 4;
        if (i >= f.size()) { notMesh++; return false; }
        const int pl = f[i++];
        const int hashSize = (pl >> 6) + 1, count = pl & 63;
        if (hashSize == 4 || count * hashSize > 64) { notMesh++; return false; }
        const size_t pathBytes = (size_t)count * hashSize;
        if (i + pathBytes >= f.size()) { notMesh++; return false; }
        const uint8_t* path = f.data() + i;
        i += pathBytes;
        const uint8_t* p = f.data() + i;
        const size_t n = f.size() - i;
        packets++;
        const bool flood = route == ROUTE_FLOOD || route == ROUTE_TRANSPORT_FLOOD;
        const int hops = flood && type != PT_TRACE ? count : -1;   // a flood packet's path: the repeaters so far

        // one packet, heard again through other repeaters: read once (the hash as the firmware's)
        Sha256 hs;
        const uint8_t tb = (uint8_t)type;
        hs.update(&tb, 1);
        if (type == PT_TRACE) { const uint8_t pl2[2] = {(uint8_t)pl, 0}; hs.update(pl2, 2); }
        hs.update(p, n);
        const auto hd = hs.digest();
        const std::string key = hex(hd.data(), 8);
        for (auto it = seen.begin(); it != seen.end();) { if (t - it->second > 600) it = seen.erase(it); else ++it; }
        const bool dup = seen.count(key) != 0;
        seen[key] = t;

        Event ev;
        ev.t = t; ev.type = type; ev.hops = hops; ev.snr = snr;
        char b[160];
        switch (type) {
        case PT_ADVERT: {
            if (n < 32 + 4 + 64 + 1) { notMesh++; return false; }
            std::string k = hex(p, 32);
            Node& nd = nodes[k];
            if (nd.packets == 0) { std::memcpy(nd.key.data(), p, 32); nd.firstSeen = t; }
            nd.lastSeen = t;
            nd.packets++;
            if (hops >= 0) nd.hops = dup && nd.hops >= 0 ? std::min(nd.hops, hops) : hops;   // the shortest way it came
            if (hops == 0) nd.snr = snr;
            if (dup) { duplicates++; return true; }
            std::memcpy(&nd.advertTime, p + 32, 4);
            const uint8_t* a = p + 100;
            const size_t an = n - 100;
            const int flags = a[0];
            size_t j = 1;
            nd.type = flags & 15;
            if ((flags & 0x10) && j + 8 <= an) {
                int32_t la, lo;
                std::memcpy(&la, a + j, 4); std::memcpy(&lo, a + j + 4, 4);
                j += 8;
                nd.hasPos = !(la == 0 && lo == 0);
                nd.lat = la / 1e6; nd.lon = lo / 1e6;
            }
            if (flags & 0x20) j += 2;
            if (flags & 0x40) j += 2;
            if ((flags & 0x80) && j < an) {
                std::string nm((const char*)a + j, an - j);
                while (!nm.empty() && nm.back() == 0) nm.pop_back();
                nd.name = nm;
            }
            ev.advert = true;
            ev.key = nd.key;
            ev.from = nd.label();
            std::snprintf(b, sizeof(b), "advert: %s", nodeTypeName(nd.type));
            ev.text = b;
            if (nd.hasPos) { std::snprintf(b, sizeof(b), ", %.5f, %.5f", nd.lat, nd.lon); ev.text += b; }
            decoded++;
            break;
        }
        case PT_GRP_TXT: case PT_GRP_DATA: {
            if (dup) { duplicates++; return true; }
            if (n < 3 + 16) { notMesh++; return false; }
            const uint8_t ch = p[0];
            const uint8_t* mac = p + 1;
            const uint8_t* ct = p + 3;
            const size_t cn = (n - 3) / 16 * 16;
            bool ok = false;
            for (const auto& c : channels) {
                if (c.hash != ch) continue;
                const auto m = hmacSha256(c.secret.data(), 32, ct, cn);
                if (m[0] != mac[0] || m[1] != mac[1]) continue;
                std::vector<uint8_t> pt(cn);
                for (size_t k = 0; k < cn; k += 16) c.aes.decryptBlock(ct + k, pt.data() + k);
                if (type == PT_GRP_TXT && cn >= 6) {
                    std::string s((const char*)pt.data() + 5, cn - 5);
                    s = s.substr(0, s.find('\0'));
                    const size_t colon = s.find(": ");
                    if (colon != std::string::npos && colon < 40) { ev.from = s.substr(0, colon); ev.text = s.substr(colon + 2); }
                    else ev.text = s;
                    ev.isText = true;
                    ev.text = "[" + c.name + "] " + ev.text;
                } else ev.text = "group data on " + c.name;
                ok = true;
                break;
            }
            if (ok) decoded++;
            else { encrypted++; std::snprintf(b, sizeof(b), "%s on a private channel (#%02x)", typeName(type), ch); ev.text = b; }
            break;
        }
        case PT_TXT_MSG: case PT_REQ: case PT_RESPONSE: case PT_PATH: {
            if (dup) { duplicates++; return true; }
            encrypted++;
            if (n >= 2) {
                const std::string d = whoIs(p, 1), s = whoIs(p + 1, 1);
                std::snprintf(b, sizeof(b), "%s %s > %s (encrypted)", typeName(type), s.empty() ? hex(p + 1, 1).c_str() : s.c_str(),
                              d.empty() ? hex(p, 1).c_str() : d.c_str());
                ev.text = b;
            } else ev.text = typeName(type);
            break;
        }
        case PT_ANON_REQ: {
            if (dup) { duplicates++; return true; }
            encrypted++;
            const std::string d = n >= 1 ? whoIs(p, 1) : "";
            ev.text = std::string("anonymous request to ") + (d.empty() ? (n ? hex(p, 1) : std::string("?")) : d) + " (login / encrypted)";
            break;
        }
        default:
            if (dup) { duplicates++; return true; }
            decoded++;
            std::snprintf(b, sizeof(b), "%s, %zu bytes", typeName(type), n);
            ev.text = b;
            if (type == PT_TRACE) { std::snprintf(b, sizeof(b), " (%d hops)", count); ev.text += b; }
            break;
        }
        (void)path;
        events.push_back(ev);
        return true;
    }
};

}  // namespace mcore
