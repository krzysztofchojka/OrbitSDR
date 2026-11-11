#pragma once
// =====================================================================
//  Small C interface between OrbitSDR and the rtl_433 decoder library.
//  Implemented in rtl433_bridge.c, which is compiled together with the
//  rtl_433 sources by scripts/rtl433.sh (it needs rtl_433's private headers).
// =====================================================================

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ism_bridge ism_bridge;

// One decoded event: n fields, each with a key ("temperature_C"), a label for
// display ("Temperature") and the formatted value ("21.4 C"). The strings are
// only valid during the call.
typedef void (*ism_event_fn)(void* user, int n, const char* const* keys,
                             const char* const* labels, const char* const* values);

// Flags for ism_create
enum {
    // also the ~50 decoders rtl_433 disables by default; they run after all default
    // decoders (only on packages nobody else decoded) and their messages get a "note"
    ISM_EXTRA_DECODERS = 1,
    // packages nobody decoded: guess the modulation and slice the bits (model "Unknown")
    ISM_UNKNOWN_SIGNALS = 2,
};

// sample_rate: the rate of the samples given to ism_push_cs16 (250000 or 1000000)
ism_bridge* ism_create(uint32_t sample_rate, int flags, ism_event_fn fn, void* user);
void ism_destroy(ism_bridge* b);

// Forget the signal in progress (e.g. after a frequency change)
void ism_reset(ism_bridge* b);

// Frequency of the samples (only used for the reported "freq" field)
void ism_set_frequency(ism_bridge* b, uint32_t hz);

// Interleaved I/Q int16 samples; returns the number of decoded events
int ism_push_cs16(ism_bridge* b, const int16_t* iq, uint32_t n_samples);

// Number of registered device decoders / total known protocols
int ism_protocols_enabled(const ism_bridge* b);
int ism_protocols_total(const ism_bridge* b);

// Statistics since ism_create: signals the pulse detector found (OOK / FSK packages),
// how many of them gave a decoded message, and the current noise floor in dB.
typedef struct ism_stats {
    unsigned ook_packages, fsk_packages, decoded_packages;
    float noise_db, level_db;
} ism_stats;
void ism_get_stats(const ism_bridge* b, ism_stats* out);

#ifdef __cplusplus
}
#endif
