#pragma once

// =====================================================================
//  Meshtastic: what is inside a LoRa packet of a Meshtastic node.
//
//  On air (firmware src/mesh/RadioInterface.h): a 16-byte header, little
//  endian, not encrypted:
//    to (4) - from (4) - packet id (4) - flags (hop limit 0..2, want ack 3,
//    via MQTT 4, hop start 5..7) - channel hash (1) - next hop (1) -
//    relay node (1, the last byte of the node that sent this copy)
//  then the payload, AES-CTR encrypted with the channel's key:
//    nonce = packet id (8 bytes, LE) + from (4, LE) + 4 zero bytes, the
//    last 4 bytes count the 16-byte blocks (big endian).
//  The key of the default channel is the well known "AQ==" key (PSK index
//  1); its name is the modem preset's ("LongFast" ...). The channel hash
//  is XOR(name bytes) ^ XOR(key bytes), so the decoder knows which key to
//  try. Direct messages between nodes with public keys (channel hash 0)
//  are encrypted per pair: they are shown, but not decrypted.
//  The plain text is a protobuf `Data` (portnum + payload): node info
//  (names, hardware), positions, texts, telemetry, traceroutes ...
//
//  Radio (MeshRadio.h): presets SF7..12 at 125 / 250 / 500 kHz, sync word
//  0x2B, a 16-symbol preamble, explicit header with CRC. The frequency of
//  a preset in a region: slot = djb2(preset name) % channels,
//  f = start + bw / 2 + slot x bw (EU_868 LongFast: 869.525 MHz).
// =====================================================================

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include <map>
#include <array>
#include <cmath>

namespace mesh {

// ---------------------------------------------------------------------
//  AES (128 / 256 bit keys). Meshtastic needs only encryption (CTR); MeshCore uses ECB, so
//  it needs the inverse cipher too.
// ---------------------------------------------------------------------
class Aes {
    std::array<uint8_t, 240> rk {};
    int rounds = 0;
    static const std::array<uint8_t, 256>& sbox() {
        static const std::array<uint8_t, 256> s = [] {
            // the multiplicative inverse in GF(2^8) and the affine map, built by walking
            // p over the field's generator 3 and q over its inverse
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
            // SubBytes + ShiftRows (the state is column major: s[row + 4 col])
            for (int c = 0; c < 4; c++) for (int row = 0; row < 4; row++) t[row + 4 * c] = S[s[row + 4 * ((c + row) & 3)]];
            if (r != rounds) {
                for (int c = 0; c < 4; c++) {   // MixColumns
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
            // InvShiftRows + InvSubBytes
            for (int c = 0; c < 4; c++) for (int row = 0; row < 4; row++) t[row + 4 * ((c + row) & 3)] = I[s[row + 4 * c]];
            for (int i = 0; i < 16; i++) t[i] ^= rk[r * 16 + i];
            if (r > 0) {
                for (int c = 0; c < 4; c++) {   // InvMixColumns
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
    // CTR: the last 4 bytes of the counter block count up (big endian), as in the firmware
    void ctr(const uint8_t nonce[16], const uint8_t* in, uint8_t* out, size_t n) const {
        uint8_t cnt[16], ks[16];
        std::memcpy(cnt, nonce, 16);
        for (size_t off = 0; off < n; off += 16) {
            encryptBlock(cnt, ks);
            for (size_t k = 0; k < 16 && off + k < n; k++) out[off + k] = (uint8_t)(in[off + k] ^ ks[k]);
            for (int k = 15; k >= 12; k--) if (++cnt[k] != 0) break;
        }
    }
};

// ---------------------------------------------------------------------
//  Protobuf reader (just the wire format: varint, fixed32/64, bytes)
// ---------------------------------------------------------------------
struct Pb {
    const uint8_t* p; const uint8_t* e;
    bool bad = false;
    Pb(const uint8_t* d, size_t n) : p(d), e(d + n) {}
    Pb(const std::string& s) : p((const uint8_t*)s.data()), e((const uint8_t*)s.data() + s.size()) {}
    bool more() const { return !bad && p < e; }
    uint64_t varint() {
        uint64_t v = 0;
        for (int sh = 0; sh < 64; sh += 7) {
            if (p >= e) { bad = true; return 0; }
            const uint8_t b = *p++;
            v |= (uint64_t)(b & 0x7F) << sh;
            if (!(b & 0x80)) return v;
        }
        bad = true;
        return 0;
    }
    uint32_t fixed32() {
        if (e - p < 4) { bad = true; p = e; return 0; }
        const uint32_t v = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
        p += 4;
        return v;
    }
    std::string bytes() {
        const uint64_t n = varint();
        if (bad || n > (uint64_t)(e - p)) { bad = true; p = e; return {}; }
        std::string s((const char*)p, (size_t)n);
        p += n;
        return s;
    }
    void skip(int wt) {
        switch (wt) {
        case 0: varint(); break;
        case 1: if (e - p < 8) bad = true; else p += 8; break;
        case 2: bytes(); break;
        case 5: fixed32(); break;
        default: bad = true; break;
        }
    }
    // the next field: number and wire type; false at the end or on garbage
    bool next(int& field, int& wt) {
        if (!more()) return false;
        const uint64_t k = varint();
        field = (int)(k >> 3); wt = (int)(k & 7);
        if (bad || field == 0) { bad = true; return false; }
        return true;
    }
    static float asFloat(uint32_t v) { float f; std::memcpy(&f, &v, 4); return f; }
    static int64_t zigzag(uint64_t v) { return (int64_t)(v >> 1) ^ -(int64_t)(v & 1); }
};

// ---------------------------------------------------------------------
//  Names of things
// ---------------------------------------------------------------------
inline const char* portName(int p) {
    switch (p) {
    case 0: return "unknown"; case 1: return "text"; case 2: return "remote hardware"; case 3: return "position";
    case 4: return "node info"; case 5: return "routing"; case 6: return "admin"; case 7: return "text (compressed)";
    case 8: return "waypoint"; case 9: return "audio"; case 10: return "detection sensor"; case 11: return "alert";
    case 12: return "key verification"; case 13: return "remote shell"; case 32: return "reply"; case 33: return "IP tunnel";
    case 34: return "paxcounter"; case 35: return "store & forward++"; case 36: return "node status"; case 37: return "mesh beacon";
    case 38: return "paging"; case 64: return "serial"; case 65: return "store & forward"; case 66: return "range test";
    case 67: return "telemetry"; case 68: return "ZPS"; case 69: return "simulator"; case 70: return "traceroute";
    case 71: return "neighbor info"; case 72: return "ATAK"; case 73: return "map report"; case 74: return "power stress";
    case 75: return "LoRaWAN bridge"; case 76: return "Reticulum"; case 77: return "Cayenne"; case 78: return "ATAK v2";
    case 79: return "LoRa OTA"; case 112: return "group alarm"; case 256: return "private"; case 257: return "ATAK forwarder";
    default: return "other";
    }
}

inline const char* roleName(int r) {
    static const char* n[] = {"client", "client mute", "router", "router client", "repeater", "tracker", "sensor", "TAK",
                              "client hidden", "lost and found", "TAK tracker", "router late", "client base"};
    return r >= 0 && r < 13 ? n[r] : "?";
}

inline std::string hwName(int h) {
    static const char* n[] = {
        "unset", "T-LoRa V2", "T-LoRa V1", "T-LoRa V2.1-1.6", "T-Beam", "Heltec V2.0", "T-Beam V0.7", "T-Echo", "T-LoRa V1.3",
        "RAK4631", "Heltec V2.1", "Heltec V1", "T-Beam S3 Core", "RAK11200", "Nano G1", "T-LoRa V2.1-1.8", "T-LoRa T3-S3",
        "Nano G1 Explorer", "Nano G2 Ultra", "LoRa Type", "WiPhone", "Wio WM1110", "RAK2560", "Heltec HRU-3601",
        "Heltec Wireless Bridge", "Station G1", "RAK11310", "Makerfabs Tracker", "Makerfabs", "CanaryOne", "RP2040 LoRa",
        "Station G2", "LoRa Relay V1", "T-Echo Plus", "PPR", "GenieBlocks", "nRF52", "Portduino (Linux)", "Android sim",
        "DIY V1", "nRF52840 PCA10059", "DR-DEV", "M5Stack", "Heltec V3", "Heltec WSL V3", "BetaFPV 2400", "BetaFPV 900 Nano",
        "RPi Pico", "Heltec Wireless Tracker", "Heltec Wireless Paper", "T-Deck", "T-Watch S3", "PiComputer S3", "Heltec HT62",
        "EByte ESP32-S3", "ESP32-S3 Pico", "Chatter 2", "Heltec Wireless Paper 1.0", "Heltec Wireless Tracker 1.0", "unPhone",
        "TD-LoRaC", "CDEBYTE EoRa-S3", "TWC Mesh V4", "nRF52 ProMicro DIY", "RadioMaster Bandit Nano", "Heltec Capsule Sensor V3",
        "Heltec Vision Master T190", "Heltec Vision Master E213", "Heltec Vision Master E290", "Heltec Mesh Node T114",
        "SenseCAP Indicator", "SenseCAP T1000-E", "RAK3172", "Wio-E5", "RadioMaster Bandit", "ME25LS01", "RP2040 Feather RFM95",
        "M5Stack Core Basic", "M5Stack Core2", "RPi Pico 2", "M5Stack CoreS3", "Seeed XIAO S3", "MS24SF1", "T-LoRa C6",
        "WisMesh Tap", "Routastic", "Mesh Tab", "MeshLink", "XIAO nRF52 kit", "ThinkNode M1", "ThinkNode M2", "T-ETH Elite",
        "Heltec Sensor Hub", "Muzi Base", "Heltec Mesh Pocket", "Seeed Solar Node", "NomadStar Meteor Pro", "CrowPanel", "Link 32",
        "Wio Tracker L1", "Wio Tracker L1 E-Ink", "Muzi R1 Neo", "T-Deck Pro", "T-LoRa Pager", "M5Stack", "WisMesh Tag",
        "RAK3312", "ThinkNode M5", "Heltec Mesh Solar", "T-Echo Lite", "Heltec V4", "M5Stack C6L", "M5Stack Cardputer Adv",
        "Heltec Wireless Tracker V2", "T-Watch Ultra", "ThinkNode M3", "WisMesh Tap V2", "RAK3401", "RAK6421", "ThinkNode M4",
        "ThinkNode M6", "MeshStick 1262", "T-Beam 1 W", "T5-S3 E-Paper Pro", "T-Beam BPF", "Mini E-Paper S3", "T-Display S3 Pro",
        "Heltec Mesh Node T096", "Mesh Tracker X1", "ThinkNode M7", "ThinkNode M8", "ThinkNode M9", "Heltec V4 R8",
        "Heltec Mesh Node T1", "Station G3", "T-Impulse Plus", "T-Echo Card", "Wio Tracker L2", "CrowPanel P4",
        "Heltec Mesh Tower V2", "Meshnology W10", "Heltec RC32", "Heltec RC52", "Heltec RCC6", "Wio Tracker L1 Pro 1 W",
        "Meshnology W12", "MeshPager X2", "T-Connect Pro", "Axiometa Genesis Mini", "Makerfabs Nomad Terminal", "ThinkNode MX",
        "Nodara", "Heltec Mesh Pocket V2"};
    if (h >= 0 && h < (int)(sizeof(n) / sizeof(n[0]))) return n[h];
    if (h == 254) return "virtual node";
    if (h == 255) return "private hardware";
    return "hw " + std::to_string(h);
}

// ---------------------------------------------------------------------
//  Modem presets and where they sit
// ---------------------------------------------------------------------
struct Preset { const char* name; int sf; double bwHz; int cr; };
inline const std::vector<Preset>& presets() {
    static const std::vector<Preset> p = {
        {"LongFast", 11, 250000, 5}, {"MediumFast", 9, 250000, 5}, {"MediumSlow", 10, 250000, 5}, {"ShortFast", 7, 250000, 5},
        {"ShortSlow", 8, 250000, 5}, {"ShortTurbo", 7, 500000, 5}, {"LongTurbo", 11, 500000, 8}, {"LongMod", 11, 125000, 8},
        {"LongSlow", 12, 125000, 8}};
    return p;
}

struct Region { const char* name; double startMHz, endMHz; };
inline const std::vector<Region>& regions() {
    // the regions whose band an RTL-SDR may sit in (MeshRadio.h); Europe first
    static const std::vector<Region> r = {
        {"EU_868", 869.4, 869.65}, {"EU_433", 433.0, 434.0}, {"UA_868", 868.0, 868.6}, {"UA_433", 433.0, 434.7},
        {"RU", 868.7, 869.2}, {"US", 902.0, 928.0}, {"ANZ", 915.0, 928.0}, {"ANZ_433", 433.05, 434.79}, {"IN", 865.0, 867.0},
        {"NZ_865", 864.0, 868.0}, {"KZ_433", 433.075, 434.775}, {"KZ_863", 863.0, 868.0}, {"MY_433", 433.0, 435.0},
        {"PH_433", 433.0, 434.7}, {"PH_868", 868.0, 869.4}, {"PH_915", 915.0, 918.0}, {"BR_902", 902.0, 907.5},
        {"JP", 920.5, 923.5}, {"KR", 920.0, 923.0}, {"TW", 920.0, 925.0}, {"CN", 470.0, 510.0}};
    return r;
}

inline uint32_t djb2(const std::string& s) {
    uint32_t h = 5381;
    for (unsigned char c : s) h = ((h << 5) + h) + c;
    return h;
}

// The default slot of a preset in a region (Hz), 0 if the preset does not fit there
inline long long defaultFrequency(const Region& g, const Preset& p) {
    const double bwMHz = p.bwHz / 1e6;
    const int n = (int)std::floor((g.endMHz - g.startMHz) / bwMHz + 1e-9);
    if (n < 1) return 0;
    const int slot = (int)(djb2(p.name) % (uint32_t)n);
    return std::llround((g.startMHz + bwMHz / 2.0 + slot * bwMHz) * 1e6);
}

// ---------------------------------------------------------------------
//  Channel keys
// ---------------------------------------------------------------------
inline const std::array<uint8_t, 16>& defaultPsk() {
    static const std::array<uint8_t, 16> k = {0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59, 0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01};
    return k;
}

struct ChannelKey {
    std::string name;
    std::vector<uint8_t> key;   // empty = no encryption
    uint8_t hash = 0;
    Aes aes;
};

inline uint8_t xorHash(const uint8_t* d, size_t n) { uint8_t h = 0; for (size_t i = 0; i < n; i++) h ^= d[i]; return h; }

// a channel from its name and PSK as the apps show it (base64; "AQ==" is the default key,
// one byte n means the default key with n - 1 added to its last byte, 0 none)
inline bool makeChannel(const std::string& name, const std::vector<uint8_t>& psk, ChannelKey& out) {
    out.name = name;
    std::vector<uint8_t> k = psk;
    if (k.size() == 1) {
        if (k[0] == 0) k.clear();
        else { const uint8_t add = (uint8_t)(k[0] - 1); k.assign(defaultPsk().begin(), defaultPsk().end()); k[15] = (uint8_t)(k[15] + add); }
    } else if (k.size() > 1 && k.size() < 16) k.resize(16, 0);
    else if (k.size() > 16 && k.size() < 32) k.resize(32, 0);
    else if (k.size() > 32) return false;
    out.key = k;
    out.hash = (uint8_t)(xorHash((const uint8_t*)name.data(), name.size()) ^ xorHash(k.data(), k.size()));
    if (!k.empty()) out.aes.setKey(k.data(), k.size());
    return true;
}

inline std::vector<uint8_t> base64Decode(const std::string& s) {
    std::vector<uint8_t> out;
    uint32_t acc = 0; int bits = 0;
    for (char c : s) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+' || c == '-') v = 62;
        else if (c == '/' || c == '_') v = 63;
        else continue;
        acc = acc << 6 | (uint32_t)v; bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back((uint8_t)(acc >> bits)); }
    }
    return out;
}

// ---------------------------------------------------------------------
//  Packets and nodes
// ---------------------------------------------------------------------
constexpr uint32_t BROADCAST = 0xFFFFFFFF;

inline std::string nodeId(uint32_t n) { char b[12]; std::snprintf(b, sizeof(b), "!%08x", n); return b; }

struct Node {
    uint32_t num = 0;
    std::string longName, shortName, hw, role;
    bool hasUser = false, licensed = false;
    bool hasPos = false;
    double lat = 0, lon = 0, altM = -1e9;
    int sats = -1, precisionBits = 0;
    double speedKmh = -1, trackDeg = -1;
    double posTime = 0;            // when the position came (decoder clock)
    int battery = -1;              // %, 101 = powered
    double voltage = -1, chUtil = -1, airUtil = -1;
    long long uptime = -1;
    double tempC = -1000, humidity = -1, pressure = -1;
    int hops = -1;                 // hops away (hop start - hop limit) of the last packet, -1 unknown
    double snr = -1000;            // of the last packet heard straight from it (hops 0)
    double lastSeen = 0, firstSeen = 0;
    int packets = 0;
    std::string lastText;
    // what the apps would show without a name: the last four hex digits
    std::string badge() const { if (!shortName.empty()) return shortName; char b[8]; std::snprintf(b, sizeof(b), "%04x", num & 0xFFFF); return b; }
    std::string label() const { return !longName.empty() ? longName : nodeId(num); }
};

// one line about a packet, for the messages list
struct Event {
    double t = 0;
    uint32_t from = 0, to = 0, id = 0;
    int port = -1;                 // -1: not decrypted
    std::string text;              // what it said
    bool isText = false;           // a text message (highlighted)
    int hops = -1;
    double snr = 0;
    std::string channel;           // the channel's name, or "#xx" (its hash)
};

class Decoder {
    std::vector<ChannelKey> keys;
    // packets already handled: (from, id) -> when. Relays send the same packet again.
    std::map<uint64_t, double> seen;

public:
    std::map<uint32_t, Node> nodes;
    std::vector<Event> events;     // new events since the caller took them
    long long packets = 0, decrypted = 0, encrypted = 0, duplicates = 0, notMesh = 0;

    Decoder() { setDefaultChannels(); }

    // the default key under every preset's name (whatever preset the mesh uses)
    void setDefaultChannels() {
        keys.clear();
        for (const auto& p : presets()) {
            ChannelKey k;
            if (makeChannel(p.name, {1}, k)) keys.push_back(k);
        }
    }
    // a channel of the user (name + base64 PSK); false if the key is not valid
    bool addChannel(const std::string& name, const std::string& pskBase64) {
        ChannelKey k;
        if (!makeChannel(name, base64Decode(pskBase64), k)) return false;
        for (auto& o : keys) if (o.name == name && o.key == k.key) return true;
        keys.push_back(k);
        return true;
    }
    size_t channelCount() const { return keys.size(); }

    void clear() { nodes.clear(); events.clear(); seen.clear(); packets = decrypted = encrypted = duplicates = notMesh = 0; }

    Node& node(uint32_t n, double t) {
        Node& d = nodes[n];
        if (d.num == 0) { d.num = n; d.firstSeen = t; }
        return d;
    }

    // One LoRa payload. t: the caller's clock (seconds), snr in dB. Returns false when it is no
    // Meshtastic packet (too short, or a header that cannot be one).
    bool handle(const std::vector<uint8_t>& f, double t, double snr) {
        if (f.size() < 16) { notMesh++; return false; }
        auto u32 = [&](size_t i) { return (uint32_t)f[i] | (uint32_t)f[i + 1] << 8 | (uint32_t)f[i + 2] << 16 | (uint32_t)f[i + 3] << 24; };
        const uint32_t to = u32(0), from = u32(4), id = u32(8);
        const uint8_t flags = f[12], chan = f[13];
        if (from == 0 || from == BROADCAST) { notMesh++; return false; }
        const int hopLimit = flags & 7, hopStart = flags >> 5;
        const int hops = hopStart ? std::max(0, hopStart - hopLimit) : -1;
        packets++;

        // every copy says the node is alive; the contents are read once
        Node& n = node(from, t);
        n.lastSeen = t;
        n.packets++;
        if (hops >= 0) n.hops = hops;
        if (hops == 0) n.snr = snr;
        const uint64_t key = (uint64_t)from << 32 | id;
        for (auto it = seen.begin(); it != seen.end();) { if (t - it->second > 600) it = seen.erase(it); else ++it; }
        if (seen.count(key)) { duplicates++; return true; }
        seen[key] = t;

        Event ev;
        ev.t = t; ev.from = from; ev.to = to; ev.id = id; ev.hops = hops; ev.snr = snr;
        char hb[8]; std::snprintf(hb, sizeof(hb), "#%02x", chan); ev.channel = hb;

        const uint8_t* enc = f.data() + 16;
        const size_t len = f.size() - 16;
        std::vector<uint8_t> plain(len);
        bool ok = false;
        for (const auto& k : keys) {
            if (k.hash != chan) continue;
            if (k.key.empty()) std::memcpy(plain.data(), enc, len);
            else {
                uint8_t nonce[16] = {};
                for (int i = 0; i < 4; i++) { nonce[i] = (uint8_t)(id >> (8 * i)); nonce[8 + i] = (uint8_t)(from >> (8 * i)); }
                k.aes.ctr(nonce, enc, plain.data(), len);
            }
            if (decodeData(plain, n, ev, t)) { ok = true; ev.channel = k.name; break; }
        }
        if (ok) decrypted++;
        else {
            encrypted++;
            ev.text = chan == 0 ? "encrypted direct message" : "encrypted (channel " + ev.channel + ", key unknown)";
        }
        events.push_back(ev);
        return true;
    }

private:
    // the Data message; false if it does not parse (wrong key)
    bool decodeData(const std::vector<uint8_t>& d, Node& n, Event& ev, double t) {
        Pb pb(d.data(), d.size());
        int field, wt, port = -1;
        std::string payload;
        uint32_t emoji = 0, replyId = 0;
        bool any = false;
        while (pb.next(field, wt)) {
            any = true;
            if (field == 1 && wt == 0) port = (int)pb.varint();
            else if (field == 2 && wt == 2) payload = pb.bytes();
            else if (field == 7 && wt == 5) replyId = pb.fixed32();
            else if (field == 8 && wt == 5) emoji = pb.fixed32();
            else if (field > 15 || (wt != 0 && wt != 1 && wt != 2 && wt != 5)) return false;
            else pb.skip(wt);
        }
        if (pb.bad || !any || port < 0 || port > 511) return false;
        ev.port = port;
        ev.text = portName(port);
        switch (port) {
        case 1: {   // text
            ev.isText = true;
            ev.text = payload;
            if (emoji) ev.text = "reacted " + payload;
            else if (replyId) ev.text = "(reply) " + payload;
            n.lastText = payload;
            break;
        }
        case 3: position(payload, n, ev, t); break;
        case 4: user(payload, n, ev); break;
        case 67: telemetry(payload, n, ev); break;
        case 70: {
            Pb r(payload);
            int hopsN = 0;
            while (r.next(field, wt)) { if (field == 1 && wt == 5) { r.fixed32(); hopsN++; } else if (field == 1 && wt == 2) hopsN += (int)r.bytes().size() / 4; else r.skip(wt); }
            ev.text = "traceroute (" + std::to_string(hopsN) + " hops so far)";
            break;
        }
        case 71: {
            Pb r(payload);
            int nb = 0;
            while (r.next(field, wt)) { if (field == 4 && wt == 2) { r.bytes(); nb++; } else r.skip(wt); }
            ev.text = "neighbor info: " + std::to_string(nb) + " neighbors";
            break;
        }
        case 5: ev.text = "routing (ack / error)"; break;
        default: break;
        }
        return true;
    }

    void position(const std::string& s, Node& n, Event& ev, double t) {
        Pb pb(s);
        int field, wt;
        bool haveLat = false, haveLon = false;
        int32_t lat = 0, lon = 0;
        while (pb.next(field, wt)) {
            if (field == 1 && wt == 5) { lat = (int32_t)pb.fixed32(); haveLat = true; }
            else if (field == 2 && wt == 5) { lon = (int32_t)pb.fixed32(); haveLon = true; }
            else if (field == 3 && wt == 0) n.altM = (double)(int32_t)pb.varint();
            else if (field == 15 && wt == 0) n.speedKmh = (double)pb.varint() * 3.6;   // m/s
            else if (field == 16 && wt == 0) n.trackDeg = (double)pb.varint() * 1e-5;
            else if (field == 19 && wt == 0) n.sats = (int)pb.varint();
            else if (field == 23 && wt == 0) n.precisionBits = (int)pb.varint();
            else pb.skip(wt);
        }
        char b[96];
        if (haveLat && haveLon && !(lat == 0 && lon == 0)) {
            n.hasPos = true; n.lat = lat * 1e-7; n.lon = lon * 1e-7; n.posTime = t;
            std::snprintf(b, sizeof(b), "position %.5f, %.5f", n.lat, n.lon);
            ev.text = b;
            if (n.altM > -1e8) { std::snprintf(b, sizeof(b), "  %.0f m", n.altM); ev.text += b; }
        } else ev.text = "position (empty)";
    }

    void user(const std::string& s, Node& n, Event& ev) {
        Pb pb(s);
        int field, wt;
        n.role = roleName(0);   // (proto3: the default, client, is not sent)
        while (pb.next(field, wt)) {
            if (field == 2 && wt == 2) n.longName = pb.bytes();
            else if (field == 3 && wt == 2) n.shortName = pb.bytes();
            else if (field == 5 && wt == 0) n.hw = hwName((int)pb.varint());
            else if (field == 6 && wt == 0) n.licensed = pb.varint() != 0;
            else if (field == 7 && wt == 0) n.role = roleName((int)pb.varint());
            else pb.skip(wt);
        }
        n.hasUser = true;
        ev.text = "node info: " + n.longName + " (" + n.shortName + ")" + (n.hw.empty() ? "" : ", " + n.hw);
    }

    void telemetry(const std::string& s, Node& n, Event& ev) {
        Pb pb(s);
        int field, wt;
        char b[160];
        ev.text = "telemetry";
        while (pb.next(field, wt)) {
            if (field == 2 && wt == 2) {   // device metrics
                Pb m(pb.bytes());
                while (m.next(field, wt)) {
                    if (field == 1 && wt == 0) n.battery = (int)m.varint();
                    else if (field == 2 && wt == 5) n.voltage = Pb::asFloat(m.fixed32());
                    else if (field == 3 && wt == 5) n.chUtil = Pb::asFloat(m.fixed32());
                    else if (field == 4 && wt == 5) n.airUtil = Pb::asFloat(m.fixed32());
                    else if (field == 5 && wt == 0) n.uptime = (long long)m.varint();
                    else m.skip(wt);
                }
                std::snprintf(b, sizeof(b), "device: battery %s, %.2f V, channel %.1f %%, air TX %.1f %%",
                              n.battery > 100 ? "powered" : n.battery >= 0 ? (std::to_string(n.battery) + " %").c_str() : "?",
                              n.voltage, n.chUtil, n.airUtil);
                ev.text = b;
            } else if (field == 3 && wt == 2) {   // environment
                Pb m(pb.bytes());
                while (m.next(field, wt)) {
                    if (field == 1 && wt == 5) n.tempC = Pb::asFloat(m.fixed32());
                    else if (field == 2 && wt == 5) n.humidity = Pb::asFloat(m.fixed32());
                    else if (field == 3 && wt == 5) n.pressure = Pb::asFloat(m.fixed32());
                    else m.skip(wt);
                }
                ev.text = "environment:";
                if (n.tempC > -999) { std::snprintf(b, sizeof(b), " %.1f \xC2\xB0""C", n.tempC); ev.text += b; }
                if (n.humidity >= 0) { std::snprintf(b, sizeof(b), " %.0f %%RH", n.humidity); ev.text += b; }
                if (n.pressure > 0) { std::snprintf(b, sizeof(b), " %.1f hPa", n.pressure); ev.text += b; }
            } else if (field == 5 && wt == 2) { pb.bytes(); ev.text = "telemetry: power"; }
            else if (field == 6 && wt == 2) { pb.bytes(); ev.text = "telemetry: local stats"; }
            else pb.skip(wt);
        }
    }
};

}  // namespace mesh
