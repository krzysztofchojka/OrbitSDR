// =====================================================================
//  OrbitSDR <-> rtl_433 bridge (C, built with the rtl_433 sources).
//
//  rtl_433 normally runs as its own program that reads a radio. Here only
//  its processing chain is used: OrbitSDR mixes and resamples the band of
//  interest and pushes the I/Q samples into push_sdr_flow(). Decoded
//  events arrive at a custom data output, which turns them into plain
//  key / label / value strings for the C++ side.
//
//  Optional extras:
//   - the decoders rtl_433 disables by default, registered with a much
//     lower priority, so they only see packages no default decoder took
//     (they are disabled because they also match other devices' signals),
//   - unknown signals: packages nobody decoded go through rtl_433's pulse
//     analyzer, which guesses the modulation and slices the bits.
// =====================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>

#include "rtl_433.h"
#include "r_private.h"
#include "r_api.h"
#include "r_flow.h"
#include "r_device.h"
#include "data.h"
#include "list.h"
#include "logger.h"
#include "pulse_detect.h"
#include "pulse_data.h"
#include "pulse_slicer.h"
#include "c_util.h"

// --- a quiet copy of rtl_433's pulse analyzer ---
// The original prints a long report to stderr for every package. The same code is
// compiled here with fprintf() turned into a no-op and its functions renamed, so it
// does not clash with the copy inside the library.
static int ism_quiet_fprintf(FILE *f, char const *fmt, ...)
{
    (void)f;
    (void)fmt;
    return 0;
}
#define fprintf ism_quiet_fprintf
#define pulse_analyzer ism_pulse_analyzer
#define pulse_analyzer_check ism_pulse_analyzer_check
#include "pulse_analyzer.c"
#undef fprintf
#undef pulse_analyzer
#undef pulse_analyzer_check

#include "rtl433_bridge.h"

#define ISM_MAX_FIELDS 64
#define ISM_VALUE_LEN 160
#define ISM_EXTRA_PRIORITY 100
#define ISM_MAX_CODES 4

struct ism_bridge {
    data_output_t output;   // must stay first: rtl_433 hands us this pointer
    r_cfg_t *cfg;
    ism_event_fn fn;
    void *user;
    int unknown;            // analyse undecoded packages
    // filled by the analyzer's bit output (see ism_capture_log)
    int cap_rows;
    char cap_codes[ISM_MAX_CODES][ISM_VALUE_LEN];
};

// ---------------------------------------------------------------------
//  value formatting
// ---------------------------------------------------------------------

static void format_value(char *dst, size_t len, data_type_t type, data_value_t v, char const *fmt);

static void format_array(char *dst, size_t len, data_array_t *arr)
{
    size_t pos = 0;
    dst[0] = '\0';
    pos += snprintf(dst + pos, len - pos, "[");
    for (int i = 0; i < arr->num_values && pos < len - 1; ++i) {
        char item[ISM_VALUE_LEN];
        data_value_t v;
        switch (arr->type) {
        case DATA_INT:    v.v_int = ((int *)arr->values)[i]; break;
        case DATA_DOUBLE: v.v_dbl = ((double *)arr->values)[i]; break;
        default:          v.v_ptr = ((void **)arr->values)[i]; break;
        }
        format_value(item, sizeof(item), arr->type, v, NULL);
        pos += snprintf(dst + pos, len - pos, "%s%s", i ? ", " : "", item);
    }
    if (pos < len - 1)
        snprintf(dst + pos, len - pos, "]");
}

static void format_value(char *dst, size_t len, data_type_t type, data_value_t v, char const *fmt)
{
    dst[0] = '\0';
    switch (type) {
    case DATA_INT:
        snprintf(dst, len, fmt && *fmt ? fmt : "%d", v.v_int);
        break;
    case DATA_DOUBLE:
        snprintf(dst, len, fmt && *fmt ? fmt : "%.3f", v.v_dbl);
        break;
    case DATA_STRING:
        snprintf(dst, len, fmt && *fmt ? fmt : "%s", v.v_ptr ? (char const *)v.v_ptr : "");
        break;
    case DATA_ARRAY:
        format_array(dst, len, (data_array_t *)v.v_ptr);
        break;
    case DATA_DATA: {
        // nested data: "key=value, key=value"
        size_t pos = 0;
        for (data_t *d = v.v_ptr; d && pos < len - 1; d = d->next) {
            char item[ISM_VALUE_LEN];
            format_value(item, sizeof(item), d->type, d->value, d->format);
            pos += snprintf(dst + pos, len - pos, "%s%s=%s", pos ? ", " : "", d->key, item);
        }
        break;
    }
    default:
        break;
    }
}

static data_t const *find_key(data_t const *data, char const *key)
{
    for (data_t const *d = data; d; d = d->next)
        if (d->key && !strcmp(d->key, key))
            return d;
    return NULL;
}

// ---------------------------------------------------------------------
//  unknown signals
// ---------------------------------------------------------------------

// The analyzer reports the sliced bits through the decoder's log function
static void ism_capture_log(r_device *decoder, int level, data_t *data)
{
    (void)level;
    struct ism_bridge *b = decoder->output_ctx;
    data_t const *codes = find_key(data, "codes");
    if (b && codes && codes->type == DATA_ARRAY) {
        data_array_t const *arr = codes->value.v_ptr;
        if (arr && arr->type == DATA_STRING) {
            for (int i = 0; i < arr->num_values && b->cap_rows < ISM_MAX_CODES; ++i) {
                char const *s = ((char **)arr->values)[i];
                if (s && *s)
                    snprintf(b->cap_codes[b->cap_rows++], ISM_VALUE_LEN, "%s", s);
            }
        }
    }
    data_free(data);
}

static char const *modulation_name(unsigned m)
{
    switch (m) {
    case OOK_PULSE_PPM:                return "OOK PPM";
    case OOK_PULSE_PWM:                return "OOK PWM";
    case OOK_PULSE_MANCHESTER_ZEROBIT: return "OOK Manchester";
    case OOK_PULSE_PCM:                return "OOK PCM";
    case FSK_PULSE_PWM:                return "FSK PWM";
    case FSK_PULSE_MANCHESTER_ZEROBIT: return "FSK Manchester";
    case FSK_PULSE_PCM:                return "FSK PCM";
    default:                           return "unknown";
    }
}

static void analyse_unknown(struct ism_bridge *b, int fsk)
{
    struct dm_state *demod = b->cfg->demod;
    pulse_data_t const *src = fsk ? &demod->fsk_pulse_data : &demod->pulse_data;
    if (src->num_pulses < 8 || src->num_pulses > PD_MAX_PULSES)
        return;   // too short to be anything but noise

    static pulse_data_t pd;   // the analyzer changes the data, work on a copy
    pd = *src;
    r_device dev;
    memset(&dev, 0, sizeof(dev));
    dev.log_fn     = ism_capture_log;
    dev.output_ctx = b;
    b->cap_rows    = 0;
    ism_pulse_analyzer(&pd, fsk ? PULSE_DATA_FSK : PULSE_DATA_OOK, &dev);
    if (!dev.modulation || b->cap_rows == 0)
        return;

    // longest bit row: ignore fragments
    int longest = 0;
    for (int i = 0; i < b->cap_rows; ++i) {
        int n = 0;
        if (sscanf(b->cap_codes[i], "{%d}", &n) == 1 && n > longest)
            longest = n;
    }
    if (longest < 12)
        return;

    char vals[12][ISM_VALUE_LEN];
    char const *keys[12], *labels[12], *values[12];
    int n = 0;
#define ADD(k, l, ...) do { snprintf(vals[n], ISM_VALUE_LEN, __VA_ARGS__); keys[n] = k; labels[n] = l; values[n] = vals[n]; n++; } while (0)
    ADD("model", "Model", "Unknown");
    ADD("id", "Signal", "%s %db %.0f/%.0fus", modulation_name(dev.modulation), longest,
            dev.short_width, dev.long_width);
    ADD("modulation", "Modulation", "%s (guessed)", modulation_name(dev.modulation));
    ADD("timing", "Timing", "short %.0f us, long %.0f us, reset %.0f us",
            dev.short_width, dev.long_width, dev.reset_limit);
    {
        size_t pos = 0;
        vals[n][0] = '\0';
        for (int i = 0; i < b->cap_rows && pos < ISM_VALUE_LEN - 1; ++i)
            pos += snprintf(vals[n] + pos, ISM_VALUE_LEN - pos, "%s%s", i ? " " : "", b->cap_codes[i]);
        keys[n] = "codes"; labels[n] = "Codes"; values[n] = vals[n]; n++;
    }
    ADD("pulses", "Pulses", "%u", src->num_pulses);
    ADD("rssi", "RSSI", "%.1f dB", src->rssi_db);
    ADD("snr", "SNR", "%.1f dB", src->snr_db);
    ADD("noise", "Noise", "%.1f dB", src->noise_db);
    ADD("freq", "Freq", "%.3f MHz", (fsk ? src->freq1_hz : src->centerfreq_hz) / 1e6);
#undef ADD
    b->fn(b->user, n, keys, labels, values);
}

// ---------------------------------------------------------------------
//  custom data output
// ---------------------------------------------------------------------

static void R_API_CALLCONV ism_output_print(data_output_t *output, data_t *data)
{
    struct ism_bridge *b = (struct ism_bridge *)output;
    if (!b->fn || !data)
        return;

    // Device events always carry a "model" field. A package nobody decoded arrives
    // as raw pulse data ("mod" + "pulses", only with unknown signals enabled).
    if (!find_key(data, "model")) {
        data_t const *mod = find_key(data, "mod");
        if (b->unknown && mod && find_key(data, "pulses") && mod->type == DATA_STRING)
            analyse_unknown(b, mod->value.v_ptr && !strcmp(mod->value.v_ptr, "FSK"));
        return;
    }

    static char values[ISM_MAX_FIELDS][ISM_VALUE_LEN];
    char const *keys[ISM_MAX_FIELDS];
    char const *labels[ISM_MAX_FIELDS];
    char const *vals[ISM_MAX_FIELDS];
    int n = 0;
    int extra = 0;
    for (data_t *d = data; d && n < ISM_MAX_FIELDS - 1; d = d->next) {
        format_value(values[n], ISM_VALUE_LEN, d->type, d->value, d->format);
        keys[n]   = d->key ? d->key : "";
        labels[n] = d->pretty_key && *d->pretty_key ? d->pretty_key : keys[n];
        vals[n]   = values[n];
        if (d->key && !strcmp(d->key, "protocol") && d->type == DATA_INT) {
            int num = d->value.v_int;
            if (num >= 1 && num <= (int)b->cfg->num_r_devices && b->cfg->devices[num - 1].disabled)
                extra = 1;
        }
        n++;
    }
    if (extra) {
        snprintf(values[n], ISM_VALUE_LEN, "decoder disabled by default in rtl_433: may be a false match");
        keys[n] = "note"; labels[n] = "Note"; vals[n] = values[n];
        n++;
    }
    b->fn(b->user, n, keys, labels, vals);
}

static void R_API_CALLCONV ism_output_free(data_output_t *output)
{
    (void)output;   // owned by ism_bridge, freed in ism_destroy()
}

// rtl_433 log messages: only errors go to the console
static void ism_log_handler(log_level_t level, char const *src, char const *msg, void *userdata)
{
    (void)userdata;
    if (level <= LOG_ERROR)
        fprintf(stderr, "[rtl_433] %s: %s\n", src ? src : "", msg ? msg : "");
}

// ---------------------------------------------------------------------
//  public interface
// ---------------------------------------------------------------------

ism_bridge *ism_create(uint32_t sample_rate, int flags, ism_event_fn fn, void *user)
{
    struct ism_bridge *b = calloc(1, sizeof(*b));
    if (!b)
        return NULL;
    b->fn      = fn;
    b->user    = user;
    b->unknown = (flags & ISM_UNKNOWN_SIGNALS) != 0;

    b->output.output_print = ism_output_print;
    b->output.output_free  = ism_output_free;
    b->output.log_level    = 0;   // no device debug logs

    r_cfg_t *cfg = r_create_cfg();
    if (!cfg) {
        free(b);
        return NULL;
    }
    b->cfg = cfg;

    cfg->verbosity          = LOG_WARNING;
    cfg->conversion_mode    = CONVERT_SI;    // metric units (C, km/h, mm, hPa, kPa)
    cfg->report_time        = REPORT_TIME_OFF;
    cfg->report_meta        = 1;             // modulation, frequency, RSSI, SNR
    cfg->report_protocol    = 1;
    cfg->report_description = 0;
    cfg->samp_rate          = sample_rate;
    cfg->center_frequency   = 433920000;

    r_logger_set_log_handler(ism_log_handler, NULL);

    struct dm_state *demod = cfg->demod;
    demod->sample_size      = 4;            // CS16
    demod->use_mag_est      = 1;
    demod->auto_level       = 1;            // follow the noise floor (independent of the RF gain)
    demod->samp_rate        = sample_rate;
    demod->center_frequency = cfg->center_frequency;
    demod->fsk_pulse_detect_mode = cfg->fsk_pulse_detect_mode;
    demod->raw_handler      = &cfg->raw_handler;
    demod->verbosity        = cfg->verbosity;
    demod->raw_mode         = b->unknown ? 2 : 0;   // 2: report the packages nobody decoded

    list_push(&cfg->output_handler, &b->output);

    register_all_protocols(cfg, 0);   // the default decoders, like rtl_433 without -R

    // The decoders rtl_433 disables by default: last, so they never take a package
    // away from a default decoder (several weather sensors run at priority 10 and only
    // get a package when nothing at priority 0 decoded it)
    if (flags & ISM_EXTRA_DECODERS) {
        for (unsigned i = 0; i < cfg->num_r_devices; i++) {
            if (cfg->devices[i].disabled != 1)
                continue;
            register_protocol(cfg, &cfg->devices[i], NULL);
            r_device *p = demod->r_devs.elems[demod->r_devs.len - 1];
            p->priority += ISM_EXTRA_PRIORITY;
        }
    }

    // FSK decoders need the FM demodulator
    for (void **iter = demod->r_devs.elems; iter && *iter; ++iter) {
        r_device *r_dev = *iter;
        if (r_dev->modulation >= FSK_DEMOD_MIN_VAL && r_dev->modulation < LORA_DEMOD_MIN_VAL) {
            demod->enable_FM_demod = 1;
            break;
        }
    }
    if (demod->lora_fm_demod) {
        unsigned sf;
        uint32_t bw;
        unsigned sync;
        if (get_lora_params(&demod->r_devs, &sf, &bw, &sync))
            demod->enable_FM_demod = 1;
    }

    pulse_detect_set_levels(demod->pulse_detect, demod->use_mag_est, demod->level_limit,
            demod->min_level, demod->min_snr, demod->detect_verbosity);
    reset_sdr_flow(cfg);
    return b;
}

void ism_destroy(ism_bridge *b)
{
    if (!b)
        return;
    if (b->cfg) {
        r_free_cfg(b->cfg);   // also calls ism_output_free() for our output
        free(b->cfg);
    }
    free(b);
}

void ism_reset(ism_bridge *b)
{
    if (!b || !b->cfg || !b->cfg->demod)
        return;
    reset_sdr_flow(b->cfg);
}

void ism_set_frequency(ism_bridge *b, uint32_t hz)
{
    if (!b || !b->cfg || !b->cfg->demod)
        return;
    b->cfg->center_frequency         = hz;
    b->cfg->demod->center_frequency  = hz;
}

int ism_push_cs16(ism_bridge *b, const int16_t *iq, uint32_t n_samples)
{
    if (!b || !b->cfg || !b->cfg->demod || !iq || !n_samples)
        return 0;
    if (n_samples > MAXIMAL_BUF_LENGTH)
        n_samples = MAXIMAL_BUF_LENGTH;
    return push_sdr_flow(b->cfg, (unsigned char *)iq, n_samples * 4);
}

int ism_protocols_enabled(const ism_bridge *b)
{
    return (b && b->cfg && b->cfg->demod) ? (int)b->cfg->demod->r_devs.len : 0;
}

int ism_protocols_total(const ism_bridge *b)
{
    return (b && b->cfg) ? (int)b->cfg->num_r_devices : 0;
}

void ism_get_stats(const ism_bridge *b, ism_stats *out)
{
    memset(out, 0, sizeof(*out));
    if (!b || !b->cfg || !b->cfg->demod)
        return;
    struct dm_state const *demod = b->cfg->demod;
    out->ook_packages     = demod->total_frames_ook;
    out->fsk_packages     = demod->total_frames_fsk;
    out->decoded_packages = demod->total_frames_events;
    out->noise_db         = demod->noise_level;
    out->level_db         = demod->min_level_auto;
}
