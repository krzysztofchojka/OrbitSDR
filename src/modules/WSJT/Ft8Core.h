#pragma once

// =====================================================================
//  FT8 / FT4 slot decoder on top of ft8_lib (Karlis Goba, MIT license,
//  vendored in deps/ft8_lib, built by scripts/ft8lib.sh).
//
//  Input: one slot of real USB audio at 12000 S/s starting at the slot
//  boundary (FT8 every 15 s, FT4 every 7.5 s, UTC).
//  Without ENABLE_FT8LIB (the library failed to build) available() is
//  false and decode() returns nothing.
// =====================================================================

#include <vector>
#include <string>
#include <mutex>
#include <cstring>
#include <cmath>
#include <algorithm>

#ifdef ENABLE_FT8LIB
extern "C" {
#include <ft8/decode.h>
#include <ft8/message.h>
#include <common/monitor.h>
}
#endif

namespace ftx {

enum class Proto { FT8, FT4 };

inline double slotSeconds(Proto p) { return p == Proto::FT8 ? 15.0 : 7.5; }

struct Decode {
    float freq = 0;       // audio Hz above the dial frequency
    float dt = 0;         // s from the slot start (FT8 nominally 0.5 s)
    float snr = 0;        // dB in 2500 Hz (estimate)
    std::string text;     // "CQ SP5ABC KO02"
};

#ifdef ENABLE_FT8LIB
// Callsign hash table: messages refer to calls heard before by a 10/12/22 bit hash.
// ft8_lib calls plain C functions, so the table is shared (guarded by a mutex).
namespace detail {
struct HashTable {
    static constexpr int SIZE = 1024;
    struct E { char call[12]; uint32_t hash; };   // 8 MSBs: age, 22 LSBs: hash
    E e[SIZE];
    int count = 0;
    std::mutex mtx;
    HashTable() { std::memset(e, 0, sizeof(e)); }
    void age(uint8_t maxAge) {
        for (auto& x : e) {
            if (!x.call[0]) continue;
            const uint8_t a = (uint8_t)(x.hash >> 24);
            if (a > maxAge) { x.call[0] = 0; x.hash = 0; count--; }
            else x.hash = (((uint32_t)a + 1u) << 24) | (x.hash & 0x3FFFFFu);
        }
    }
};
inline HashTable& table() { static HashTable t; return t; }

inline void saveHash(const char* callsign, uint32_t hash) {
    HashTable& t = table();
    const uint16_t h10 = (hash >> 12) & 0x3FFu;
    int idx = (h10 * 23) % HashTable::SIZE;
    for (int n = 0; n < HashTable::SIZE && t.e[idx].call[0]; n++) {
        if ((t.e[idx].hash & 0x3FFFFFu) == hash && !std::strcmp(t.e[idx].call, callsign)) {
            t.e[idx].hash &= 0x3FFFFFu;   // heard again: young
            return;
        }
        idx = (idx + 1) % HashTable::SIZE;
    }
    if (t.e[idx].call[0]) return;   // full
    t.count++;
    std::strncpy(t.e[idx].call, callsign, 11);
    t.e[idx].call[11] = 0;
    t.e[idx].hash = hash;
}

inline bool lookupHash(ftx_callsign_hash_type_t type, uint32_t hash, char* callsign) {
    HashTable& t = table();
    const uint8_t shift = type == FTX_CALLSIGN_HASH_10_BITS ? 12 : type == FTX_CALLSIGN_HASH_12_BITS ? 10 : 0;
    const uint16_t h10 = (hash >> (12 - shift)) & 0x3FFu;
    int idx = (h10 * 23) % HashTable::SIZE;
    for (int n = 0; n < HashTable::SIZE && t.e[idx].call[0]; n++) {
        if (((t.e[idx].hash & 0x3FFFFFu) >> shift) == hash) {
            std::strcpy(callsign, t.e[idx].call);
            return true;
        }
        idx = (idx + 1) % HashTable::SIZE;
    }
    callsign[0] = 0;
    return false;
}
} // namespace detail
#endif

inline bool available() {
#ifdef ENABLE_FT8LIB
    return true;
#else
    return false;
#endif
}

// Decodes one slot. Runs in the decoder's worker thread.
inline std::vector<Decode> decodeSlot(Proto proto, const std::vector<float>& audio, int sampleRate = 12000) {
    std::vector<Decode> out;
#ifdef ENABLE_FT8LIB
    const int kMinScore = 10, kMaxCandidates = 140, kLdpcIterations = 25, kMaxDecoded = 50;
    monitor_config_t cfg;
    cfg.f_min = 100.0f;
    cfg.f_max = 3100.0f;
    cfg.sample_rate = sampleRate;
    cfg.time_osr = 2;
    cfg.freq_osr = 2;
    cfg.protocol = proto == Proto::FT8 ? FTX_PROTOCOL_FT8 : FTX_PROTOCOL_FT4;
    monitor_t mon;
    monitor_init(&mon, &cfg);
    for (size_t pos = 0; pos + (size_t)mon.block_size <= audio.size() && mon.wf.num_blocks < mon.wf.max_blocks; pos += mon.block_size)
        monitor_process(&mon, audio.data() + pos);

    const ftx_waterfall_t* wf = &mon.wf;
    std::vector<ftx_candidate_t> cand(kMaxCandidates);
    const int n = ftx_find_candidates(wf, kMaxCandidates, cand.data(), kMinScore);

    // noise floor of the waterfall (median magnitude) for the SNR estimate
    float noiseDb = -120.0f;
    {
        const size_t total = (size_t)wf->num_blocks * wf->block_stride;
        if (total > 0) {
            std::vector<uint8_t> v(wf->mag, wf->mag + total);
            std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
            noiseDb = WF_ELEM_MAG(v[v.size() / 2]);
        }
    }

    std::vector<ftx_message_t> decoded;
    ftx_callsign_hash_interface_t hashIf = { detail::lookupHash, detail::saveHash };
    std::lock_guard<std::mutex> l(detail::table().mtx);
    for (int i = 0; i < n && (int)decoded.size() < kMaxDecoded; i++) {
        const ftx_candidate_t* c = &cand[i];
        ftx_message_t msg;
        ftx_decode_status_t st;
        if (!ftx_decode_candidate(wf, c, kLdpcIterations, &msg, &st)) continue;
        bool dupe = false;
        for (const auto& d : decoded)
            if (d.hash == msg.hash && !std::memcmp(d.payload, msg.payload, sizeof(msg.payload))) dupe = true;
        if (dupe) continue;
        decoded.push_back(msg);

        char text[FTX_MAX_MESSAGE_LENGTH + 8];
        ftx_message_offsets_t offs;
        if (ftx_message_decode(&msg, &hashIf, text, &offs) != FTX_MESSAGE_RC_OK) continue;
        Decode d;
        d.text = text;
        d.freq = (mon.min_bin + c->freq_offset + (float)c->freq_sub / wf->freq_osr) / mon.symbol_period;
        d.dt = (c->time_offset + (float)c->time_sub / wf->time_osr) * mon.symbol_period;
        // signal level: the strongest tone of the candidate's symbols against the noise floor.
        // The bin is one tone wide (6.25 Hz FT8, 20.8 Hz FT4); WSJT-X quotes SNR in 2500 Hz.
        float sigDb = -120.0f;
        {
            const int nsym = proto == Proto::FT8 ? FT8_NN : FT4_NN;
            const int ntones = proto == Proto::FT8 ? 8 : 4;
            double acc = 0.0;
            int cnt = 0;
            for (int s = 0; s < nsym; s++) {
                const int blk = c->time_offset + s;
                if (blk < 0 || blk >= wf->num_blocks) continue;
                const int base = blk * wf->block_stride + (c->time_sub * wf->freq_osr + c->freq_sub) * wf->num_bins + c->freq_offset;
                float best = -120.0f;
                for (int t = 0; t < ntones; t++) {
                    if (c->freq_offset + t >= wf->num_bins) break;
                    best = std::max(best, WF_ELEM_MAG(wf->mag[base + t]));
                }
                acc += std::pow(10.0, best / 10.0);
                cnt++;
            }
            if (cnt) sigDb = (float)(10.0 * std::log10(acc / cnt));
        }
        const float binHz = 1.0f / mon.symbol_period;
        const float sn = (float)(std::pow(10.0, (sigDb - noiseDb) / 10.0) - 1.0);
        d.snr = sn > 1e-3f ? 10.0f * std::log10(sn) + 10.0f * std::log10(binHz / 2500.0f) : -30.0f;
        d.snr = std::clamp(d.snr, -30.0f, 40.0f);
        out.push_back(d);
    }
    detail::table().age(10);
    monitor_free(&mon);
#else
    (void)proto; (void)audio; (void)sampleRate;
#endif
    return out;
}

} // namespace ftx
