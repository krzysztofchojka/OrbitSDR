// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Unvoiced synthesis, TIA-102.BABA equations 117-126: LCG noise, windowed FFT, per-band
 * normalisation, inverse FFT and weighted overlap-add.
 *
 * From mbelib-neo 2.6.2's src/core/mbe_unvoiced_fft.c, which mbelib-neo 2.6.0 implemented
 * independently from the specification to replace the earlier implementation (its
 * legal/implementation-provenance.md). mbevocoder: the autocorrelation and forward-transform helpers,
 * which nothing here uses, are left out, and the band scaling takes an optional unvoiced amplitude
 * per harmonic (mixed excitation, mbe_synthesizeUnvoicedFFTWithNoiseAmp).
 */
#include "mbe_unvoiced_fft.h"
#include "mbe_compiler.h"

#include "pffft/pffft.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct mbe_fft_plan {
    PFFFT_Setup* setup;
    float* allocation;
    float* time;
    float* spectrum;
    float* shaped;
    float* work;
};

static MBE_THREAD_LOCAL uint32_t pending_noise_seed;
static MBE_THREAD_LOCAL int noise_seed_pending;

mbe_fft_plan*
mbe_fft_plan_alloc(void) {
    mbe_fft_plan* plan = calloc(1, sizeof(*plan));
    if (plan == NULL) {
        return NULL;
    }
    plan->setup = pffft_new_setup(MBE_FFT_SIZE, PFFFT_REAL);
    plan->allocation = plan->setup != NULL ? pffft_aligned_malloc(4u * MBE_FFT_SIZE * sizeof(float)) : NULL;
    if (plan->allocation == NULL) {
        mbe_fft_plan_free(plan);
        return NULL;
    }
    plan->time = plan->allocation;
    plan->spectrum = plan->time + MBE_FFT_SIZE;
    plan->shaped = plan->spectrum + MBE_FFT_SIZE;
    plan->work = plan->shaped + MBE_FFT_SIZE;
    return plan;
}

void
mbe_fft_plan_free(mbe_fft_plan* plan) {
    if (plan == NULL) {
        return;
    }
    if (plan->setup != NULL) {
        pffft_destroy_setup(plan->setup);
    }
    if (plan->allocation != NULL) {
        pffft_aligned_free(plan->allocation);
    }
    free(plan);
}

static uint32_t
usable_noise_seed(float seed) {
    uint32_t bits;
    memcpy(&bits, &seed, sizeof(bits));
    if (bits == UINT32_C(0x80000000)) {
        return 0;
    }
    /* Largest binary32 value below 2^32; reject before integer conversion. */
    if (bits > UINT32_C(0x4f7fffff)) {
        return (uint32_t)MBE_LCG_DEFAULT_SEED;
    }
    return (uint32_t)seed % 53125u;
}

void
mbe_generate_noise_lcg(float* buffer, int count, float* seed) {
    if (buffer == NULL || seed == NULL || count <= 0) {
        return;
    }
    uint32_t state = usable_noise_seed(*seed);
    for (int i = 0; i < count; ++i) {
        buffer[i] = (float)state;
        state = (171u * state + 11213u) % 53125u;
    }
    *seed = (float)state;
}

void
mbe_seedUnvoicedNoiseLcg(uint32_t seed) {
    if (seed == 0) {
        seed = UINT32_C(0x6d25357b);
    }
    pending_noise_seed = seed % 53125u;
    noise_seed_pending = 1;
}

void
mbe_generate_noise_with_overlap(float* buffer, float* seed, float* overlap) {
    if (buffer == NULL || seed == NULL || overlap == NULL) {
        return;
    }
    if (*seed < 0.0f) {
        memset(buffer, 0, MBE_FFT_SIZE * sizeof(float));
        memset(overlap, 0, MBE_NOISE_OVERLAP * sizeof(float));
        *seed = noise_seed_pending ? (float)pending_noise_seed : MBE_LCG_DEFAULT_SEED;
        noise_seed_pending = 0;
        return;
    }
    memmove(buffer, overlap, MBE_NOISE_OVERLAP * sizeof(float));
    mbe_generate_noise_lcg(buffer + MBE_NOISE_OVERLAP, MBE_FFT_SIZE - MBE_NOISE_OVERLAP, seed);
    memmove(overlap, buffer + MBE_FFT_SIZE - MBE_NOISE_OVERLAP, MBE_NOISE_OVERLAP * sizeof(float));
}

float
mbe_synthesisWindow(int n) {
    /* Annex I's taper samples, rounded separately to binary32. */
    static const float taper[51] = {0.00f, 0.02f, 0.04f, 0.06f, 0.08f, 0.10f, 0.12f, 0.14f, 0.16f, 0.18f, 0.20f,
                                    0.22f, 0.24f, 0.26f, 0.28f, 0.30f, 0.32f, 0.34f, 0.36f, 0.38f, 0.40f, 0.42f,
                                    0.44f, 0.46f, 0.48f, 0.50f, 0.52f, 0.54f, 0.56f, 0.58f, 0.60f, 0.62f, 0.64f,
                                    0.66f, 0.68f, 0.70f, 0.72f, 0.74f, 0.76f, 0.78f, 0.80f, 0.82f, 0.84f, 0.86f,
                                    0.88f, 0.90f, 0.92f, 0.94f, 0.96f, 0.98f, 1.00f};
    if (n <= -105 || n >= 105) {
        return 0.0f;
    }
    if (n < 0) {
        n = -n;
    }
    if (n <= 55) {
        return 1.0f;
    }
    return taper[105 - n];
}

/* mbevocoder: the synthesis window over the 256-point buffer and the weighted overlap-add's two
 * windows over a 160-sample frame, tabulated once per thread from mbe_synthesisWindow() (the same
 * values, so the same output, without a window evaluation per sample). */
struct window_tables {
    int ready;
    float analysis[MBE_FFT_SIZE];
    float previous[160];
    float current[160];
    float weight[160];
};

static MBE_THREAD_LOCAL struct window_tables window_tables;

static const struct window_tables*
get_window_tables(void) {
    struct window_tables* t = &window_tables;
    if (!t->ready) {
        for (int i = 0; i < MBE_FFT_SIZE; ++i) {
            t->analysis[i] = mbe_synthesisWindow(i - MBE_FFT_SIZE / 2);
        }
        for (int n = 0; n < 160; ++n) {
            t->previous[n] = mbe_synthesisWindow(n);
            t->current[n] = mbe_synthesisWindow(n - 160);
            t->weight[n] = t->previous[n] * t->previous[n] + t->current[n] * t->current[n];
        }
        t->ready = 1;
    }
    return t;
}

static void
wola_combine_frame(float* output, const float* prevUw, const float* currUw) {
    const struct window_tables* t = get_window_tables();
    for (int n = 0; n < 160; ++n) {
        float sample = 0.0f;
        if (t->previous[n] > 0.0f) {
            sample += t->previous[n] * prevUw[128 + n];
        }
        if (t->current[n] > 0.0f) {
            sample += t->current[n] * currUw[128 + n - 160];
        }
        if (t->weight[n] > 0.0f) {
            output[n] += sample / t->weight[n];
        }
    }
}

void
mbe_wola_combine(float* output, const float* prevUw, const float* currUw, int N) {
    if (output == NULL || prevUw == NULL || currUw == NULL || N <= 0 || N > MBE_FFT_SIZE) {
        return;
    }
    for (int n = 0; n < N; ++n) {
        const float previous_window = mbe_synthesisWindow(n);
        const float current_window = mbe_synthesisWindow(n - N);
        const float weight = previous_window * previous_window + current_window * current_window;
        float sample = 0.0f;
        if (previous_window > 0.0f) {
            sample += previous_window * prevUw[128 + n];
        }
        if (current_window > 0.0f) {
            sample += current_window * currUw[128 + n - N];
        }
        if (weight > 0.0f) {
            output[n] += sample / weight;
        }
    }
}

static int
valid_unvoiced_harmonics(const mbe_parms* mp) {
    return mp != NULL && mp->L >= 1 && mp->L <= 56;
}

static int
valid_unvoiced_model(const mbe_parms* mp) {
    if (!valid_unvoiced_harmonics(mp)) {
        return 0;
    }
    uint32_t bits;
    memcpy(&bits, &mp->w0, sizeof(bits));
    return bits <= UINT32_C(0x7f7fffff) || bits == UINT32_C(0x80000000);
}

static int
band_edge(float frequency) {
    if (frequency >= 128.0f) {
        return 128;
    }
    return (int)ceilf(frequency);
}

static float
band_power(const float* spectrum, int lower, int upper) {
    float power = 0.0f;
    for (int k = lower; k < upper; ++k) {
        const float real = spectrum[2u * (size_t)k];
        const float imag = k == 0 ? 0.0f : spectrum[2u * (size_t)k + 1u];
        power += real * real + imag * imag;
    }
    return power;
}

/* amp (mbevocoder, mixed excitation): the unvoiced amplitude per harmonic, 0 for none; NULL
 * takes Ml of the harmonics with Vl == 0 */
static void
shape_unvoiced(mbe_fft_plan* fft, const mbe_parms* mp, const float* amp) {
    memset(fft->shaped, 0, MBE_FFT_SIZE * sizeof(float));
    const float spacing = MBE_256_OVER_2PI * mp->w0;
    for (int l = 1; l <= mp->L; ++l) {
        if (amp != NULL ? !(amp[l] > 0.0f) : mp->Vl[l] != 0) {
            continue;
        }
        const int lower = band_edge(((float)l - 0.5f) * spacing);
        const int upper = band_edge(((float)l + 0.5f) * spacing);
        const float power = band_power(fft->spectrum, lower, upper);
        if (upper <= lower || power <= 1.0e-10f) {
            continue;
        }
        const float scale = (MBE_UNVOICED_SCALE_COEFF * (amp != NULL ? amp[l] : mp->Ml[l]))
                            / sqrtf(power / (float)(upper - lower));
        for (int k = lower; k < upper; ++k) {
            fft->shaped[2u * (size_t)k] = fft->spectrum[2u * (size_t)k] * scale;
            if (k != 0) {
                fft->shaped[2u * (size_t)k + 1u] = fft->spectrum[2u * (size_t)k + 1u] * scale;
            }
        }
    }
}

void
mbe_synthesizeUnvoicedFFTWithNoiseAmp(float* output, mbe_parms* cur_mp, const mbe_parms* prev_mp, mbe_fft_plan* plan,
                                      const float* noise_buffer, const float* amp) {
    if (output == NULL || !valid_unvoiced_model(cur_mp) || !valid_unvoiced_harmonics(prev_mp) || plan == NULL
        || noise_buffer == NULL) {
        return;
    }
    const struct window_tables* windows = get_window_tables();
    for (int i = 0; i < MBE_FFT_SIZE; ++i) {
        plan->time[i] = noise_buffer[i] * windows->analysis[i];
    }
    pffft_transform_ordered(plan->setup, plan->time, plan->spectrum, plan->work, PFFFT_FORWARD);
    shape_unvoiced(plan, cur_mp, amp);
    pffft_transform_ordered(plan->setup, plan->shaped, plan->time, plan->work, PFFFT_BACKWARD);
    for (int i = 0; i < MBE_FFT_SIZE; ++i) {
        plan->time[i] *= 1.0f / MBE_FFT_SIZE;
    }
    wola_combine_frame(output, prev_mp->previousUw, plan->time);
    memcpy(cur_mp->previousUw, plan->time, sizeof(cur_mp->previousUw));
}

void
mbe_synthesizeUnvoicedFFTWithNoise(float* output, mbe_parms* cur_mp, const mbe_parms* prev_mp, mbe_fft_plan* plan,
                                   const float* noise_buffer) {
    mbe_synthesizeUnvoicedFFTWithNoiseAmp(output, cur_mp, prev_mp, plan, noise_buffer, NULL);
}

void
mbe_synthesizeUnvoicedFFT(float* output, mbe_parms* cur_mp, const mbe_parms* prev_mp, mbe_fft_plan* plan) {
    if (output == NULL || !valid_unvoiced_model(cur_mp) || !valid_unvoiced_harmonics(prev_mp) || plan == NULL) {
        return;
    }
    float noise[MBE_FFT_SIZE];
    mbe_generate_noise_with_overlap(noise, &cur_mp->noiseSeed, cur_mp->noiseOverlap);
    mbe_synthesizeUnvoicedFFTWithNoise(output, cur_mp, prev_mp, plan, noise);
}
