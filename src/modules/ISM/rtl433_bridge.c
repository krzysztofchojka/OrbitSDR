// =====================================================================
//  OrbitSDR <-> rtl_433 bridge (C, built with the rtl_433 sources).
//
//  rtl_433 normally runs as its own program that reads a radio. Here only
//  its processing chain is used: OrbitSDR mixes and resamples the band of
//  interest and pushes the I/Q samples into push_sdr_flow(). Decoded
//  events arrive at a custom data output, which turns them into plain
//  key / label / value strings for the C++ side.
// =====================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtl_433.h"
#include "r_private.h"
#include "r_api.h"
#include "r_flow.h"
#include "r_device.h"
#include "data.h"
#include "list.h"
#include "logger.h"
#include "pulse_detect.h"

#include "rtl433_bridge.h"

#define ISM_MAX_FIELDS 64
#define ISM_VALUE_LEN 160

struct ism_bridge {
    data_output_t output;   // must stay first: rtl_433 hands us this pointer
    r_cfg_t *cfg;
    ism_event_fn fn;
    void *user;
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

// ---------------------------------------------------------------------
//  custom data output
// ---------------------------------------------------------------------

static void R_API_CALLCONV ism_output_print(data_output_t *output, data_t *data)
{
    struct ism_bridge *b = (struct ism_bridge *)output;
    if (!b->fn || !data)
        return;

    // Only device events: they always carry a "model" field (log messages don't)
    int is_event = 0;
    for (data_t *d = data; d; d = d->next) {
        if (d->key && !strcmp(d->key, "model")) {
            is_event = 1;
            break;
        }
    }
    if (!is_event)
        return;

    static char values[ISM_MAX_FIELDS][ISM_VALUE_LEN];
    char const *keys[ISM_MAX_FIELDS];
    char const *labels[ISM_MAX_FIELDS];
    char const *vals[ISM_MAX_FIELDS];
    int n = 0;
    for (data_t *d = data; d && n < ISM_MAX_FIELDS; d = d->next) {
        format_value(values[n], ISM_VALUE_LEN, d->type, d->value, d->format);
        keys[n]   = d->key ? d->key : "";
        labels[n] = d->pretty_key && *d->pretty_key ? d->pretty_key : keys[n];
        vals[n]   = values[n];
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

ism_bridge *ism_create(uint32_t sample_rate, int all_decoders, ism_event_fn fn, void *user)
{
    struct ism_bridge *b = calloc(1, sizeof(*b));
    if (!b)
        return NULL;
    b->fn   = fn;
    b->user = user;

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

    list_push(&cfg->output_handler, &b->output);

    register_all_protocols(cfg, all_decoders ? 1 : 0);   // 0: the defaults, like rtl_433 without -R

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
