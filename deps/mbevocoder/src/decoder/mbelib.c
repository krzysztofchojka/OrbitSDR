// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 *
 * Copyright (C) 2010 mbelib Author
 * GPG Key ID: 0xEA5EFE2C (9E7A 5527 9CDC EBF7 BF1B  D772 4F98 E863 EA5E FE2C)
 *
 * Portions were originally under the ISC license; this mbelib-neo
 * distribution is provided under GPL-2.0-or-later. See LICENSE for details.
 */

/**
 * @file
 * @brief Core MBE parameter utilities and speech/tone synthesis.
 *
 * @defgroup mbe_internal Internal Helpers
 * @brief Private helpers for voiced/unvoiced mixing, RNG, and SIMD paths.
 *
 * These functions and utilities are used internally by the library’s synthesis
 * implementation and are not part of the public API. Names and semantics may
 * change between releases. Where appropriate, helpers note the impact of
 * `MBEVOCODER_ENABLE_SIMD` on determinism and ordering.
 *
 * @{ 
 */

#include "internal/config.h"
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#ifdef MBE_DEBUG
#include <stdio.h>
#endif
#include "mbe_compiler.h"
#if defined(MBEVOCODER_ENABLE_SIMD)
#if defined(MBE_SIMD_TARGET_SSE2)
#include <emmintrin.h>
#endif
#if defined(MBE_SIMD_TARGET_NEON)
#include <arm_neon.h>
#endif
#endif

#include "ambe_common.h"
#include "mbe_adaptive.h"
#include "mbe_math.h"
#include "mbe_result.h"
#include "mbe_tone.h"
#include "mbe_unvoiced_fft.h"
#include "mbe_validation.h"
#include "mbelib-neo/mbelib.h"
#include "mbevocoder/version.h"
#include "mbelib_const.h"

#define MBE_TWO_PI (2.0f * (float)M_PI)

/* Thread-local PRNG state and helpers (xorshift32) */
static MBE_THREAD_LOCAL uint32_t mbe_rng_state = 0x12345678u;

/* Thread-local FFT plan for unvoiced synthesis */
static MBE_THREAD_LOCAL mbe_fft_plan* mbe_fft_plan_instance = NULL;


/**
 * @brief Get or create the thread-local FFT plan.
 * @return FFT plan for unvoiced synthesis, or NULL on allocation failure.
 */
static mbe_fft_plan*
mbe_get_fft_plan(void) {
    if (MBE_LIKELY(mbe_fft_plan_instance != NULL)) {
        return mbe_fft_plan_instance;
    }
    mbe_fft_plan_instance = mbe_fft_plan_alloc();
    return mbe_fft_plan_instance;
}

void
mbe_freeThreadResources(void) {
    /* mbevocoder: the plan is allocated on a thread's first synthesis and was never released, so
     * an application that starts and stops decoder threads leaked one per
     * thread. A later synthesis on the thread allocates a new one. */
    if (mbe_fft_plan_instance != NULL) {
        mbe_fft_plan_free(mbe_fft_plan_instance);
        mbe_fft_plan_instance = NULL;
    }
}

void
mbe_setThreadRngSeed(uint32_t seed) {
    if (seed == 0u) {
        seed = 0x6d25357bu; /* avoid zero state */
    }
    mbe_rng_state = seed;
    mbe_seedComfortNoiseRng(seed);
    mbe_seedUnvoicedNoiseLcg(seed);
}

/**
 * @brief Thread-local xorshift32 PRNG step.
 * @return New 32-bit pseudo-random state value (never 0).
 * @note Retained as a lightweight utility; current synthesis paths use
 *       codec-specific generators seeded via mbe_setThreadRngSeed().
 */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((unused))
#endif
static inline uint32_t
mbe_xorshift32(void) {
    uint32_t x = mbe_rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    mbe_rng_state = x ? x : 0x6d25357bu;
    return mbe_rng_state;
}

/*
 * Voiced-path helpers keep the oscillator recurrence scalar so the synthesis
 * order matches the historical implementation, then apply SIMD only to the
 * output accumulation. The paired helper lets the common prev+cur voiced case
 * touch the output buffer once per 4-sample block instead of twice.
 */
static inline void
mbe_fill_voiced_cos_block4(float* restrict cblk, float* restrict c, float* restrict s, float sd, float cd) {
    float cc = *c;
    float ss = *s;

    for (int k = 0; k < 4; ++k) {
        cblk[k] = cc;
        float cpn = (cc * cd) - (ss * sd);
        float spn = (ss * cd) + (cc * sd);
        cc = cpn;
        ss = spn;
    }

    *c = cc;
    *s = ss;
}

/**
 * @brief Add four voiced samples to the output using oscillator recurrence.
 *
 * Generates four consecutive cosine samples from the current oscillator state,
 * multiplies by window `W[0..3]` and gain `gain`, and accumulates into
 * `Ss[0..3]`. `gain` already includes the `2.0f` voiced multiplier of TIA-102.BABA equation 136.
 *
 * @param Ss Output sample pointer (adds to Ss[0..3]).
 * @param W  Window values for these four samples.
 * @param gain Voiced gain for this band/component, including the `2.0f` factor.
 * @param c  In/out cosine register.
 * @param s  In/out sine register.
 * @param sd sin(Δθ) per step.
 * @param cd cos(Δθ) per step.
 */
/** @internal @ingroup mbe_internal */
static inline void
mbe_add_voiced_block4(float* restrict Ss, const float* restrict W, float gain, float* restrict c, float* restrict s,
                      float sd, float cd) {
    float cblk[4];
    mbe_fill_voiced_cos_block4(cblk, c, s, sd, cd);

#if defined(MBEVOCODER_ENABLE_SIMD) && defined(MBE_SIMD_TARGET_SSE2)
    __m128 vC = _mm_loadu_ps(cblk);
    __m128 vW = _mm_loadu_ps(W);
    __m128 vA = _mm_set1_ps(gain);
    __m128 vS = _mm_loadu_ps(Ss);
    vS = _mm_add_ps(vS, _mm_mul_ps(_mm_mul_ps(vC, vW), vA));
    _mm_storeu_ps(Ss, vS);
#elif defined(MBEVOCODER_ENABLE_SIMD) && defined(MBE_SIMD_TARGET_NEON)
    float32x4_t vC = vld1q_f32(cblk);
    float32x4_t vW = vld1q_f32(W);
    float32x4_t vA = vdupq_n_f32(gain);
    float32x4_t vS = vld1q_f32(Ss);
    vS = vaddq_f32(vS, vmulq_f32(vmulq_f32(vC, vW), vA));
    vst1q_f32(Ss, vS);
#else
    Ss[0] += gain * W[0] * cblk[0];
    Ss[1] += gain * W[1] * cblk[1];
    Ss[2] += gain * W[2] * cblk[2];
    Ss[3] += gain * W[3] * cblk[3];
#endif
}

/**
 * @brief Add previous/current voiced contributions in one 4-sample block.
 *
 * This preserves the historical add order (`prev`, then `cur`) while avoiding
 * a second load/store of the output buffer when both voiced components are
 * active for the same harmonic.
 */
typedef struct mbe_voiced_block_component {
    const float* W;
    float gain;
    float* c;
    float* s;
    float sd;
    float cd;
} mbe_voiced_block_component;

/** @internal @ingroup mbe_internal */
static inline void
mbe_add_voiced_dual_block4(float* restrict Ss, const mbe_voiced_block_component* restrict prev,
                           const mbe_voiced_block_component* restrict cur) {
    float prev_cblk[4];
    float cur_cblk[4];

    mbe_fill_voiced_cos_block4(prev_cblk, prev->c, prev->s, prev->sd, prev->cd);
    mbe_fill_voiced_cos_block4(cur_cblk, cur->c, cur->s, cur->sd, cur->cd);

#if defined(MBEVOCODER_ENABLE_SIMD) && defined(MBE_SIMD_TARGET_SSE2)
    __m128 vS = _mm_loadu_ps(Ss);
    __m128 vPrev = _mm_mul_ps(_mm_mul_ps(_mm_loadu_ps(prev_cblk), _mm_loadu_ps(prev->W)), _mm_set1_ps(prev->gain));
    __m128 vCur = _mm_mul_ps(_mm_mul_ps(_mm_loadu_ps(cur_cblk), _mm_loadu_ps(cur->W)), _mm_set1_ps(cur->gain));
    vS = _mm_add_ps(vS, vPrev);
    vS = _mm_add_ps(vS, vCur);
    _mm_storeu_ps(Ss, vS);
#elif defined(MBEVOCODER_ENABLE_SIMD) && defined(MBE_SIMD_TARGET_NEON)
    float32x4_t vS = vld1q_f32(Ss);
    float32x4_t vPrev = vmulq_f32(vmulq_f32(vld1q_f32(prev_cblk), vld1q_f32(prev->W)), vdupq_n_f32(prev->gain));
    float32x4_t vCur = vmulq_f32(vmulq_f32(vld1q_f32(cur_cblk), vld1q_f32(cur->W)), vdupq_n_f32(cur->gain));
    vS = vaddq_f32(vS, vPrev);
    vS = vaddq_f32(vS, vCur);
    vst1q_f32(Ss, vS);
#else
    Ss[0] += prev->gain * prev->W[0] * prev_cblk[0];
    Ss[1] += prev->gain * prev->W[1] * prev_cblk[1];
    Ss[2] += prev->gain * prev->W[2] * prev_cblk[2];
    Ss[3] += prev->gain * prev->W[3] * prev_cblk[3];
    Ss[0] += cur->gain * cur->W[0] * cur_cblk[0];
    Ss[1] += cur->gain * cur->W[1] * cur_cblk[1];
    Ss[2] += cur->gain * cur->W[2] * cur_cblk[2];
    Ss[3] += cur->gain * cur->W[3] * cur_cblk[3];
#endif
}

/** @} */ /* end of mbe_internal */


/**
 * @brief Copy MBE parameter set from input to output.
 * @param source_mp      Source parameter set.
 * @param destination_mp Destination parameter set.
 *
 * Uses struct assignment for efficiency - the compiler generates optimal
 * code (typically a single memcpy-like operation) rather than many individual
 * field copies. The mbe_parms struct contains no pointers, so shallow copy
 * is correct and safe.
 */
void
mbe_moveMbeParms(const mbe_parms* source_mp, mbe_parms* destination_mp) {
    if (!source_mp || !destination_mp) {
        return;
    }
    *destination_mp = *source_mp;
}

void
mbe_renderMute(float* aout_buf, mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced) {
#if MBEVOCODER_CHIP_MEASURED
    /* The AMBE-3000R plays three repeats, fades the fourth to silence within the frame (about
     * -3 dB over that frame, a tail near -50 dB in the next) and stays silent until a frame
     * decodes; that frame comes out at its proper level, so the prediction state is not reset
     * (measured with LOST_FRAME on a steady passage, tools/quality/hil_errors.py --sweeps).
     * Here: the first mute frame is the repeated model faded out, later ones are silence, and
     * the model state stays as it was. */
    if (cur_mp->repeatCount <= MBE_MAX_FRAME_REPEATS) {
        /* render the repeated model once more (the synthesis itself mutes at the repeat limit,
         * so lower the count for the call) and fade it */
        const int count = cur_mp->repeatCount;
        cur_mp->repeatCount = MBE_MAX_FRAME_REPEATS - 1;
        mbe_moveMbeParms(cur_mp, prev_mp);
        float rm0 = mbe_spectralAmpEnhanceWithRm0(cur_mp);
        mbe_synthesizeSpeechWithPreEnhRm0f(aout_buf, cur_mp, prev_mp_enhanced, rm0);
        mbe_moveMbeParms(cur_mp, prev_mp_enhanced);
        cur_mp->repeatCount = prev_mp->repeatCount = prev_mp_enhanced->repeatCount = count;
        for (int n = 0; n < 160; n++) {
            aout_buf[n] *= (float)(160 - n) / 160.0f;
        }
        /* The output has faded to silence, so the synthesis' memory of the last frame must too:
         * the first frame decoded after the mute used to overlap the repeated model's voiced and
         * unvoiced tails at full level, 3-8 dB above the AMBE-3000R. The
         * prediction state (prev_mp) stays, as the AMBE-3000R's does. */
        for (int l = 0; l <= 56; l++) {
            prev_mp_enhanced->Ml[l] = 0.0f;
            prev_mp_enhanced->Vl[l] = 0;
        }
        prev_mp_enhanced->vmixValid = 0;
        memset(prev_mp_enhanced->previousUw, 0, sizeof(prev_mp_enhanced->previousUw));
        memset(prev_mp_enhanced->noiseOverlap, 0, sizeof(prev_mp_enhanced->noiseOverlap));
    } else {
        memset(aout_buf, 0, 160 * sizeof(float));
    }
#else
    mbe_synthesizeComfortNoisef(aout_buf);
    mbe_initAmbeParms_common(cur_mp, prev_mp, prev_mp_enhanced);
#endif
}

/* Lost frame: the framing layer knows a slot was not received (docs/decoder-quality.md,
 * "Bit errors"). The previous model is repeated three times, the fourth missing frame mutes
 * (the AMBE-3000R's count), and the mute holds, the repeat count kept past the limit, until a
 * frame decodes. Feeding the last frame's bits again instead would run the gain prediction on
 * them once more per frame (+2..3 dB each time). */
int
mbe_processLostFramef(float* aout_buf, mbe_process_result* result, mbe_parms* cur_mp, mbe_parms* prev_mp,
                      mbe_parms* prev_mp_enhanced) {
    mbe_process_result local_result;
    if (!aout_buf || !cur_mp || !prev_mp || !prev_mp_enhanced) {
        return MBE_STATUS_INVALID_ARGUMENT;
    }
    if (!result) {
        mbe_initProcessResult(&local_result);
        result = &local_result;
    } else {
        result->flags = 0u;
        result->c0_errors = result->c4_errors = result->protected_errors = result->total_errors = 0;
    }
    mbe_repeatFrameAudio(aout_buf, result, cur_mp, prev_mp, prev_mp_enhanced);
    return 0;
}

void
mbe_renderToneRepeat(float* aout_buf, mbe_process_result* result, mbe_parms* cur_mp) {
    char d[49];
    for (int i = 0; i < 49; i++) {
        d[i] = (char)((cur_mp->lastTone >> i) & 1u);
    }
    if (cur_mp->repeatCount < MBE_MAX_FRAME_REPEATS) {
        mbe_synthesizeTonef(aout_buf, d, cur_mp);
        return;
    }
    mbe_result_set_flag(result, MBE_PROCESS_FLAG_MUTE);
    if (cur_mp->repeatCount == MBE_MAX_FRAME_REPEATS) {
        mbe_synthesizeTonef(aout_buf, d, cur_mp);
        for (int n = 0; n < 160; n++) {
            aout_buf[n] *= (float)(160 - n) / 160.0f;
        }
        return;
    }
    memset(aout_buf, 0, 160 * sizeof(float));
}

void
mbe_repeatFrameAudio(float* aout_buf, mbe_process_result* result, mbe_parms* cur_mp, mbe_parms* prev_mp,
                     mbe_parms* prev_mp_enhanced) {
    mbe_repeatModel(cur_mp, prev_mp);
    if (cur_mp->repeatCount < MBE_MAX_FRAME_REPEATS + 1) {
        cur_mp->repeatCount++;
    }
    mbe_result_set_flag(result, MBE_PROCESS_FLAG_REPEAT);
#if MBEVOCODER_CHIP_MEASURED
    if (cur_mp->dstarTone != 0 && mbe_dstarRenderLostAfterTone(aout_buf, result, cur_mp, prev_mp, prev_mp_enhanced)) {
        return;
    }
    if (cur_mp->lastTone != 0u) {
        /* the frame last played was a tone: that tone is what repeats and fades */
        mbe_renderToneRepeat(aout_buf, result, cur_mp);
        prev_mp->repeatCount = prev_mp_enhanced->repeatCount = cur_mp->repeatCount;
        return;
    }
#endif
    if (cur_mp->repeatCount < MBE_MAX_FRAME_REPEATS) {
        mbe_moveMbeParms(cur_mp, prev_mp);
        float rm0 = mbe_spectralAmpEnhanceWithRm0(cur_mp);
        mbe_synthesizeSpeechWithPreEnhRm0f(aout_buf, cur_mp, prev_mp_enhanced, rm0);
        mbe_moveMbeParms(cur_mp, prev_mp_enhanced);
        return;
    }
    mbe_result_set_flag(result, MBE_PROCESS_FLAG_MUTE);
    const int count = cur_mp->repeatCount;
    mbe_renderMute(aout_buf, cur_mp, prev_mp, prev_mp_enhanced);
    /* stay muted on further lost frames (mbelib's mute resets the count; the count is what holds the
     * mute). Held at the first count past the limit: every count above it renders the same silence,
     * and an unbounded count overflowed after 2^31 lost frames. */
    const int held = count < MBE_MAX_FRAME_REPEATS + 1 ? count + 1 : MBE_MAX_FRAME_REPEATS + 1;
    cur_mp->repeatCount = prev_mp->repeatCount = prev_mp_enhanced->repeatCount = held;
}


int
mbe_processLostFrame(short* aout_buf, mbe_process_result* result, mbe_parms* cur_mp, mbe_parms* prev_mp,
                     mbe_parms* prev_mp_enhanced) {
    float float_buf[160];
    if (!aout_buf) {
        return MBE_STATUS_INVALID_ARGUMENT;
    }
    int ret = mbe_processLostFramef(float_buf, result, cur_mp, prev_mp, prev_mp_enhanced);
    if (ret < 0) {
        return ret;
    }
    mbe_floattoshort(float_buf, aout_buf);
    return ret;
}

void
mbe_keepModel(const mbe_parms* keep, mbe_parms* dst) {
    dst->w0 = keep->w0;
    dst->L = keep->L;
    dst->K = keep->K;
    dst->gamma = keep->gamma;
    dst->unvcScale = keep->unvcScale;
    dst->vmixValid = keep->vmixValid;
    dst->silenceFrame = keep->silenceFrame;
    memcpy(dst->Vl, keep->Vl, sizeof dst->Vl);
    memcpy(dst->Ml, keep->Ml, sizeof dst->Ml);
    memcpy(dst->log2Ml, keep->log2Ml, sizeof dst->log2Ml);
    memcpy(dst->Vmix, keep->Vmix, sizeof dst->Vmix);
}

void
mbe_repeatModel(mbe_parms* cur_mp, const mbe_parms* prev_mp) {
    if (!cur_mp || !prev_mp) {
        return;
    }
#if MBEVOCODER_CHIP_MEASURED
    mbe_parms model = *prev_mp;
    model.errorRate = cur_mp->errorRate;
    model.errorCountTotal = cur_mp->errorCountTotal;
    model.errorCount4 = cur_mp->errorCount4;
    model.mutingThreshold = cur_mp->mutingThreshold;
    model.swn = cur_mp->swn;
    model.tonePhase = cur_mp->tonePhase;
    model.noiseSeed = cur_mp->noiseSeed;
    memcpy(model.noiseOverlap, cur_mp->noiseOverlap, sizeof model.noiseOverlap);
    memcpy(model.hpfState, cur_mp->hpfState, sizeof model.hpfState);
    *cur_mp = model;
#else
    mbe_useLastMbeParms(cur_mp, prev_mp);
#endif
}

/**
 * @brief Replace current parameters with the last known parameters.
 * @param cur_mp  Destination parameter set to fill.
 * @param prev_mp Source parameter set from previous frame.
 *
 * Uses struct assignment for efficiency. See mbe_moveMbeParms() for details.
 */
void
mbe_useLastMbeParms(mbe_parms* cur_mp, const mbe_parms* prev_mp) {
    if (!cur_mp || !prev_mp) {
        return;
    }
#if MBEVOCODER_CHIP_MEASURED
    /* The output high-pass state is the synthesis', kept in cur_mp across frames; prev_mp holds a
     * copy taken before the last frame was synthesised, so a repeat restarted the filter from
     * the frame before. Keep the current state. */
    float hpf[4];
    memcpy(hpf, cur_mp->hpfState, sizeof hpf);
    *cur_mp = *prev_mp;
    memcpy(cur_mp->hpfState, hpf, sizeof hpf);
#else
    *cur_mp = *prev_mp;
#endif
}

/**
 * @brief Initialize MBE parameter state for decoding and synthesis.
 * @param cur_mp Output: current parameter state.
 * @param prev_mp Output: previous parameter state (reset to defaults).
 * @param prev_mp_enhanced Output: enhanced previous parameter state.
 */
/* The library's default voice model (pitch index 134), also what IMBE falls back to after
 * prolonged repeats. From mbelib-neo 2.6.2 (mbe_resetImbeModel, implemented independently in 2.6.0;
 * its legal/implementation-provenance.md). */
void
mbe_resetImbeModel(mbe_parms* mp) {
    if (mp == NULL) {
        return;
    }
    mp->w0 = (float)(4.0 * M_PI / (134.0 + 39.5));
    mp->L = (int)floor(0.9254 * floor(M_PI / (double)mp->w0 + 0.25));
    mp->K = 12;
    mp->gamma = 0.0f;
    for (int l = 0; l <= 56; ++l) {
        mp->Ml[l] = 1.0f;
        mp->Vl[l] = 0;
        mp->log2Ml[l] = 0.0f;
    }
    mp->swn = 0;
    mp->tonePhase = 0;
    mp->repeatCount = 0;
    mp->localEnergy = MBE_DEFAULT_LOCAL_ENERGY;
    mp->amplitudeThreshold = MBE_DEFAULT_AMPLITUDE_THRESHOLD;
    mp->mutingThreshold = MBE_MUTING_THRESHOLD_IMBE;
}

void
mbe_initMbeParms(mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced) {
    if (cur_mp == NULL || prev_mp == NULL || prev_mp_enhanced == NULL) {
        return;
    }
    mbe_parms initial;
    memset(&initial, 0, sizeof(initial));
    mbe_resetImbeModel(&initial);
    initial.noiseSeed = -1.0f;   /* the first noise buffer is silence, then the default seed */
    initial.unvcScale = 1.0f;    /* mbevocoder: mixed excitation off until a frame sets it */
    memcpy(cur_mp, &initial, sizeof(initial));
    memcpy(prev_mp, &initial, sizeof(initial));
    memcpy(prev_mp_enhanced, &initial, sizeof(initial));
}

static void
mbe_precompute_harmonic_cosines(float cos_tab[57], int L, float w0) {
    float s_step, c_step;
    mbe_sincosf(w0, &s_step, &c_step);
    float c = 1.0f, s = 0.0f;
    for (int l = 1; l <= L; ++l) {
        float cn = (c * c_step) - (s * s_step);
        float sn = (s * c_step) + (c * s_step);
        c = cn;
        s = sn;
        cos_tab[l] = c;
    }
}

static void
mbe_accumulate_spectral_energy(const mbe_parms* cur_mp, const float cos_tab[57], float* out_rm0, float* out_rm1) {
    const int L = cur_mp->L;
    float Rm0 = 0.0f;
    float Rm1 = 0.0f;
    int l;

#if defined(MBEVOCODER_ENABLE_SIMD)
#if defined(MBE_SIMD_TARGET_SSE2)
    if (L >= 4) {
        __m128 vRm0 = _mm_setzero_ps();
        __m128 vRm1 = _mm_setzero_ps();
        int l4;
        for (l4 = 1; l4 + 3 <= L; l4 += 4) {
            __m128 vMl = _mm_loadu_ps(&cur_mp->Ml[l4]);
            __m128 vCos = _mm_loadu_ps(&cos_tab[l4]);
            __m128 vMl2 = _mm_mul_ps(vMl, vMl);
            vRm0 = _mm_add_ps(vRm0, vMl2);
            vRm1 = _mm_add_ps(vRm1, _mm_mul_ps(vMl2, vCos));
        }
        __m128 shuf = _mm_shuffle_ps(vRm0, vRm0, _MM_SHUFFLE(2, 3, 0, 1));
        __m128 sums = _mm_add_ps(vRm0, shuf);
        shuf = _mm_movehl_ps(shuf, sums);
        sums = _mm_add_ss(sums, shuf);
        Rm0 = _mm_cvtss_f32(sums);

        shuf = _mm_shuffle_ps(vRm1, vRm1, _MM_SHUFFLE(2, 3, 0, 1));
        sums = _mm_add_ps(vRm1, shuf);
        shuf = _mm_movehl_ps(shuf, sums);
        sums = _mm_add_ss(sums, shuf);
        Rm1 = _mm_cvtss_f32(sums);

        for (l = l4; l <= L; l++) {
            const float Ml2 = cur_mp->Ml[l] * cur_mp->Ml[l];
            Rm0 += Ml2;
            Rm1 += Ml2 * cos_tab[l];
        }
    } else
#elif defined(MBE_SIMD_TARGET_NEON)
    if (L >= 4) {
        float32x4_t vRm0 = vdupq_n_f32(0.0f);
        float32x4_t vRm1 = vdupq_n_f32(0.0f);
        int l4;
        for (l4 = 1; l4 + 3 <= L; l4 += 4) {
            float32x4_t vMl = vld1q_f32(&cur_mp->Ml[l4]);
            float32x4_t vCos = vld1q_f32(&cos_tab[l4]);
            float32x4_t vMl2 = vmulq_f32(vMl, vMl);
            vRm0 = vaddq_f32(vRm0, vMl2);
            vRm1 = vmlaq_f32(vRm1, vMl2, vCos);
        }
        float32x2_t sum2 = vadd_f32(vget_low_f32(vRm0), vget_high_f32(vRm0));
        Rm0 = vget_lane_f32(vpadd_f32(sum2, sum2), 0);
        sum2 = vadd_f32(vget_low_f32(vRm1), vget_high_f32(vRm1));
        Rm1 = vget_lane_f32(vpadd_f32(sum2, sum2), 0);

        for (l = l4; l <= L; l++) {
            const float Ml2 = cur_mp->Ml[l] * cur_mp->Ml[l];
            Rm0 += Ml2;
            Rm1 += Ml2 * cos_tab[l];
        }
    } else
#endif
#endif
    {
        for (l = 1; l <= L; l++) {
            const float Ml2 = cur_mp->Ml[l] * cur_mp->Ml[l];
            Rm0 += Ml2;
            Rm1 += Ml2 * cos_tab[l];
        }
    }

    *out_rm0 = Rm0;
    *out_rm1 = Rm1;
}

/* Synthesis experiment knobs (tools/quality/synth_diag.py builds variants with these on the
 * compiler command line; the defaults are the shipped behaviour). */
#define MBE_HIGH_BAND_SHELF 1.25892541f   /* +2 dB, see mbe_spectralAmpEnhanceWithRm0 */
#ifndef MBE_EXP_ENHANCE
#define MBE_EXP_ENHANCE 1              /* 0: no spectral amplitude enhancement */
#endif
#ifndef MBE_EXP_UNVOICED_GAIN
#define MBE_EXP_UNVOICED_GAIN 1.0f     /* linear gain on the unvoiced synthesis */
#endif
#ifndef MBE_EXP_ENH_POWER
#define MBE_EXP_ENH_POWER 1.0f         /* exponent on the enhancement weight */
#endif
#ifndef MBE_EXP_ENH_MAX
#define MBE_EXP_ENH_MAX 1.2f           /* enhancement weight clamp */
#endif
#ifndef MBE_EXP_ENH_MIN
#define MBE_EXP_ENH_MIN 0.5f
#endif
#ifndef MBE_EXP_PHASE_SMOOTH
#define MBE_EXP_PHASE_SMOOTH MBE_SYNTH_PHASE_SMOOTH_DEFAULT   /* weight of the previous frame's regenerated phase (0..1) */
#endif
#ifndef MBE_EXP_UV_GEO
#define MBE_EXP_UV_GEO MBE_SYNTH_UV_GEO_DEFAULT   /* warp the unvoiced level ramp from linear to geometric */
#endif
/* Voiced synthesis between two frames (docs/decoder-quality.md, "Amplitude interpolation").
 * mbelib (the IMBE standard) interpolates harmonic amplitudes linearly across the frame, and
 * only the seven lowest harmonics keep phase continuity when the pitch is steady; the rest
 * are windowed oscillators. The AMBE-3000R evidently ramps amplitudes geometrically (straight
 * lines in dB): doing the same, for every harmonic voiced in both frames whose pitch moves by
 * less than half, takes PESQ on the AMBE-3000R's own frames from 2.72 to 2.80 (AMBE+2) and 2.86 to
 * 2.91 (D-STAR); warping the unvoiced level ramp (a linear crossfade in the WOLA) to a
 * geometric one adds 0.02, and smoothing the regenerated harmonic phases across frames
 * (0.8 of the previous frame's), which keeps the phase-continuity correction from bending
 * each harmonic's frequency by the frame-to-frame phase jump, another 0.02.
 * MBEVOCODER_CHIP_MEASURED=OFF restores mbelib's synthesis (bit-exact with mbelib-neo); the
 * MBE_EXP_* macros are the experiment knobs behind it. */
#if MBEVOCODER_CHIP_MEASURED
#define MBE_SYNTH_AMP_INTERP_DEFAULT 1
#define MBE_SYNTH_INTERP_MAXL_DEFAULT 57
#define MBE_SYNTH_INTERP_PITCH_DEFAULT 0.5f
#define MBE_SYNTH_UV_GEO_DEFAULT 1
#define MBE_SYNTH_PHASE_SMOOTH_DEFAULT 0.8f
#define MBE_SYNTH_HPF_DEFAULT 1
#else
#define MBE_SYNTH_AMP_INTERP_DEFAULT 0
#define MBE_SYNTH_INTERP_MAXL_DEFAULT 8
#define MBE_SYNTH_INTERP_PITCH_DEFAULT 0.1f
#define MBE_SYNTH_UV_GEO_DEFAULT 0
#define MBE_SYNTH_PHASE_SMOOTH_DEFAULT 0.0f
#define MBE_SYNTH_HPF_DEFAULT 0
#endif
#ifndef MBE_OUTPUT_SCALE
#define MBE_OUTPUT_SCALE 0.1529f   /* speech synthesis to the AMBE-3000R's level (below) */
#endif
#ifndef MBE_EXP_HPF
#define MBE_EXP_HPF MBE_SYNTH_HPF_DEFAULT   /* second-order 80 Hz high-pass on the synthesis output */
#endif
#ifndef MBE_EXP_AMP_INTERP
#define MBE_EXP_AMP_INTERP MBE_SYNTH_AMP_INTERP_DEFAULT      /* amplitude ramp 0 linear, 1 geometric, 2 smoothstep */
#endif
#ifndef MBE_EXP_INTERP_MAXL
#define MBE_EXP_INTERP_MAXL MBE_SYNTH_INTERP_MAXL_DEFAULT    /* harmonics below this use phase-continuous synthesis */
#endif
#ifndef MBE_EXP_INTERP_PITCH
#define MBE_EXP_INTERP_PITCH MBE_SYNTH_INTERP_PITCH_DEFAULT  /* ... when the pitch changes by less than this fraction */
#endif

static float
mbe_clamp_enhancement_weight(float w) {
    if (MBE_EXP_ENH_POWER != 1.0f) {
        w = powf(w, MBE_EXP_ENH_POWER);
    }
    if (w > MBE_EXP_ENH_MAX) {
        w = MBE_EXP_ENH_MAX;
    } else if (w < MBE_EXP_ENH_MIN) {
        w = MBE_EXP_ENH_MIN;
    }
    return w;
}

#if MBEVOCODER_CHIP_MEASURED
/*
 * mbevocoder, from mbelib-neo 2.6.3 (#160): the weights
 * W_l = sqrt(M_l) (0.96 pi (Rm0^2 + Rm1^2 - 2 Rm0 Rm1 cos(w0 l)) / (w0 Rm0 (Rm0^2 - Rm1^2)))^(1/4)
 * depend on the envelope's shape, not its level, and are evaluated with r = Rm1 / Rm0 and
 * M_l / sqrt(Rm0), which are at most 1: the terms in Rm0^3 overflowed float for loud models (a D-STAR
 * frame held at the largest gain and b3 built its amplitudes up to 2^45 and the output dropped out)
 * and underflow for quiet ones. Models whose peak lies beyond 2^60 or 2^-60, or whose weights float
 * cannot resolve, are enhanced in double precision (mbe_spectral_enhance_wide_range()).
 */
static int
mbe_spectral_needs_wide_range(const mbe_parms* cur_mp) {
    float peak = 0.0f;
    for (int l = 1; l <= cur_mp->L; l++) {
        const float magnitude = fabsf(cur_mp->Ml[l]);
        if (magnitude > peak) {
            peak = magnitude;
        }
    }
    return (peak > 0x1p60f) || (peak > 0.0f && peak < 0x1p-60f);
}

/* Whether float cannot resolve the weights: w0 (1 - r^2), or the numerator 1 + r^2 - 2 r cos(w0 l) of
 * a weighted harmonic, too close to 0 (decoded models stay above 2.5e-3 and 1.5e-3). */
static int
mbe_spectral_float_unresolved(const mbe_parms* cur_mp, const float cos_tab[57], float Rm0, float Rm1) {
    if (!(Rm0 > 0.0f) || !(cur_mp->w0 > 0.0f)) {
        return 0;
    }
    const float r = Rm1 / Rm0;
    if (!(((float)1 - (r * r)) > 0x1p-14f)) {
        return 1;
    }
    for (int l = 1; l <= cur_mp->L; l++) {
        const float spread = ((float)1 + (r * r)) - ((float)2 * r * cos_tab[l]);
        if ((8 * l) > cur_mp->L && cur_mp->Ml[l] != 0.0f && !(spread > 0x1p-12f)) {
            return 1;
        }
    }
    return 0;
}

/* The enhancement (weights and renormalisation) in double precision, cosines included, amplitudes
 * held to the float range; returns Rm0, held to it too. As |r| reaches 1 the weight tends to
 * sqrt(M_l / sqrt(Rm0)) (0.96 pi / w0)^(1/4), used where no positive denominator is left. */
static float
mbe_spectral_enhance_wide_range(mbe_parms* cur_mp) {
    const int L = cur_mp->L;
    double rm0 = 0.0, rm1 = 0.0, sum = 0.0;
    double enhanced[57], cosines[57];
    for (int l = 1; l <= L; l++) {
        const double power = (double)cur_mp->Ml[l] * (double)cur_mp->Ml[l];
        cosines[l] = cos((double)cur_mp->w0 * (double)l);
        rm0 += power;
        rm1 += power * cosines[l];
    }
    const int weighted = (rm0 > 0.0) && (cur_mp->w0 > 0.0f) && MBE_EXP_ENHANCE;
    const double r = weighted ? rm1 / rm0 : 0.0;
    const int singular = weighted && !((double)cur_mp->w0 * (1.0 - (r * r)) > 0.0);
    for (int l = 1; l <= L; l++) {
        const double ml = (double)cur_mp->Ml[l];
        enhanced[l] = ml;
        if (weighted && ml != 0.0 && (8 * l) > L) {
            const double spread = singular ? 1.0 : fmax((1.0 + (r * r)) - (2.0 * r * cosines[l]), 0.0);
            const double shape = (0.96 * M_PI) / (singular ? (double)cur_mp->w0 : (double)cur_mp->w0 * (1.0 - (r * r)));
            const double w = sqrt(ml / sqrt(rm0)) * sqrt(sqrt(shape * spread));
            enhanced[l] = ml * (double)mbe_clamp_enhancement_weight((float)w);
        }
        sum += enhanced[l] * enhanced[l];
    }
    const double gamma = (weighted && sum > 0.0) ? sqrt(rm0 / sum) : 1.0;
    for (int l = 1; l <= L; l++) {
        const double value = enhanced[l] * gamma;
        cur_mp->Ml[l] = (value > (double)FLT_MAX) ? FLT_MAX : (value < -(double)FLT_MAX ? -FLT_MAX : (float)value);
    }
    return (rm0 > (double)FLT_MAX) ? FLT_MAX : (float)rm0;
}
#endif

static void
mbe_apply_spectral_weights(mbe_parms* cur_mp, const float cos_tab[57], float Rm0, float Rm1) {
    float Wl[57];
    const int L = cur_mp->L;

#if MBEVOCODER_CHIP_MEASURED
    /* A repeated erasure model has w0 = 0, so every cosine is 1, Rm1 = Rm0 and the weight is 0/0:
     * leave such a model unweighted. */
    if (!(Rm0 > 0.0f) || !(cur_mp->w0 > 0.0f)) {
        return;
    }
    const float r = Rm1 / Rm0;
    if (!((float)1 - (r * r) > 0.0f)) {
        return;
    }
    const float shape = ((float)0.96 * (float)M_PI) / (cur_mp->w0 * ((float)1 - (r * r)));
    const float inv_sqrt_rm0 = (float)1 / sqrtf(Rm0);
#else
    float R2m0 = Rm0 * Rm0;
    float R2m1 = Rm1 * Rm1;
#endif

    for (int l = 1; l <= L; l++) {
        if (cur_mp->Ml[l] != 0.0f) {
            const float cos_w0l = cos_tab[l];
#if MBEVOCODER_CHIP_MEASURED
            const float spread = fmaxf(((float)1 + (r * r)) - ((float)2 * r * cos_w0l), 0.0f);
            Wl[l] = sqrtf(cur_mp->Ml[l] * inv_sqrt_rm0) * sqrtf(sqrtf(shape * spread));
#else
            Wl[l] = sqrtf(cur_mp->Ml[l])
                    * sqrtf(sqrtf(((float)0.96 * (float)M_PI * ((R2m0 + R2m1) - ((float)2 * Rm0 * Rm1 * cos_w0l)))
                                  / (cur_mp->w0 * Rm0 * (R2m0 - R2m1))));
#endif

            if ((8 * l) <= L) {
                /* no-op for low harmonics */
            } else {
                cur_mp->Ml[l] = mbe_clamp_enhancement_weight(Wl[l]) * cur_mp->Ml[l];
            }
        }
    }
}

static float
mbe_sum_spectral_magnitudes_squared(const mbe_parms* cur_mp) {
    const int L = cur_mp->L;
    float sum = 0.0f;
    int l;

#if defined(MBEVOCODER_ENABLE_SIMD)
#if defined(MBE_SIMD_TARGET_SSE2)
    if (L >= 4) {
        __m128 vsum = _mm_setzero_ps();
        __m128 sign_mask = _mm_set1_ps(-0.0f);
        int l4;
        for (l4 = 1; l4 + 3 <= L; l4 += 4) {
            __m128 vMl = _mm_loadu_ps(&cur_mp->Ml[l4]);
            vMl = _mm_andnot_ps(sign_mask, vMl);
            vsum = _mm_add_ps(vsum, _mm_mul_ps(vMl, vMl));
        }
        __m128 shuf = _mm_shuffle_ps(vsum, vsum, _MM_SHUFFLE(2, 3, 0, 1));
        __m128 sums = _mm_add_ps(vsum, shuf);
        shuf = _mm_movehl_ps(shuf, sums);
        sums = _mm_add_ss(sums, shuf);
        sum = _mm_cvtss_f32(sums);

        for (l = l4; l <= L; l++) {
            float M = cur_mp->Ml[l];
            if (M < 0.0f) {
                M = -M;
            }
            sum += M * M;
        }
    } else
#elif defined(MBE_SIMD_TARGET_NEON)
    if (L >= 4) {
        float32x4_t vsum = vdupq_n_f32(0.0f);
        int l4;
        for (l4 = 1; l4 + 3 <= L; l4 += 4) {
            float32x4_t vMl = vld1q_f32(&cur_mp->Ml[l4]);
            vMl = vabsq_f32(vMl);
            vsum = vmlaq_f32(vsum, vMl, vMl);
        }
        float32x2_t sum2 = vadd_f32(vget_low_f32(vsum), vget_high_f32(vsum));
        sum = vget_lane_f32(vpadd_f32(sum2, sum2), 0);

        for (l = l4; l <= L; l++) {
            float M = cur_mp->Ml[l];
            if (M < 0.0f) {
                M = -M;
            }
            sum += M * M;
        }
    } else
#endif
#endif
    {
        for (l = 1; l <= L; l++) {
            float M = cur_mp->Ml[l];
            if (M < 0.0f) {
                M = -M;
            }
            sum += M * M;
        }
    }

    return sum;
}

static void
mbe_scale_spectral_magnitudes(mbe_parms* cur_mp, float gamma) {
    const int L = cur_mp->L;
    int l;

#if defined(MBEVOCODER_ENABLE_SIMD)
#if defined(MBE_SIMD_TARGET_SSE2)
    if (L >= 4) {
        __m128 vgamma = _mm_set1_ps(gamma);
        int l4;
        for (l4 = 1; l4 + 3 <= L; l4 += 4) {
            __m128 vMl = _mm_loadu_ps(&cur_mp->Ml[l4]);
            vMl = _mm_mul_ps(vMl, vgamma);
            _mm_storeu_ps(&cur_mp->Ml[l4], vMl);
        }
        for (l = l4; l <= L; l++) {
            cur_mp->Ml[l] = gamma * cur_mp->Ml[l];
        }
    } else
#elif defined(MBE_SIMD_TARGET_NEON)
    if (L >= 4) {
        float32x4_t vgamma = vdupq_n_f32(gamma);
        int l4;
        for (l4 = 1; l4 + 3 <= L; l4 += 4) {
            float32x4_t vMl = vld1q_f32(&cur_mp->Ml[l4]);
            vMl = vmulq_f32(vMl, vgamma);
            vst1q_f32(&cur_mp->Ml[l4], vMl);
        }
        for (l = l4; l <= L; l++) {
            cur_mp->Ml[l] = gamma * cur_mp->Ml[l];
        }
    } else
#endif
#endif
    {
        for (l = 1; l <= L; l++) {
            cur_mp->Ml[l] = gamma * cur_mp->Ml[l];
        }
    }
}

/**
 * @brief Apply spectral amplitude enhancement to the current parameters.
 * @param cur_mp In/out parameter set to enhance.
 *
 * Uses SIMD optimizations for accumulation and scaling loops when available.
 */
#if MBEVOCODER_CHIP_MEASURED
static void mbe_apply_high_band_shelf(mbe_parms* cur_mp);
#endif

float
mbe_spectralAmpEnhanceWithRm0(mbe_parms* cur_mp) {

    float Rm0, Rm1;
    float cos_tab[57];

    if (MBE_UNLIKELY(!cur_mp || !mbe_harmonic_count_is_valid(cur_mp->L))) {
        return 0.0f;
    }

    mbe_precompute_harmonic_cosines(cos_tab, cur_mp->L, cur_mp->w0);
#if MBEVOCODER_CHIP_MEASURED
    if (MBE_UNLIKELY(mbe_spectral_needs_wide_range(cur_mp))) {
        Rm0 = mbe_spectral_enhance_wide_range(cur_mp);
        mbe_apply_high_band_shelf(cur_mp);
        return Rm0;
    }
#endif
    mbe_accumulate_spectral_energy(cur_mp, cos_tab, &Rm0, &Rm1);

    if (!MBE_EXP_ENHANCE) {
        return Rm0;
    }
#if MBEVOCODER_CHIP_MEASURED
    if (MBE_UNLIKELY(mbe_spectral_float_unresolved(cur_mp, cos_tab, Rm0, Rm1))) {
        Rm0 = mbe_spectral_enhance_wide_range(cur_mp);
        mbe_apply_high_band_shelf(cur_mp);
        return Rm0;
    }
#endif
    mbe_apply_spectral_weights(cur_mp, cos_tab, Rm0, Rm1);

    float sum = mbe_sum_spectral_magnitudes_squared(cur_mp);
    float gamma = (sum == 0.0f) ? 1.0f : sqrtf(Rm0 / sum);
    mbe_scale_spectral_magnitudes(cur_mp, gamma);
#if MBEVOCODER_CHIP_MEASURED
    mbe_apply_high_band_shelf(cur_mp);
#endif

    return Rm0;
}

#if MBEVOCODER_CHIP_MEASURED
static void
mbe_apply_high_band_shelf(mbe_parms* cur_mp) {
    {   /* High-band shelf: with the same bits, our synthesis rendered 2.7-3.9 kHz 1.4-2.5 dB below
         * DVSI's decoder (= the AMBE-3000R) relative to 200-1000 Hz, on every rate. +2 dB from
         * 3.1 to 3.7 kHz, ramped in from 2.7 kHz and out by 3.9 kHz, brings the long-term spectrum
         * of DVSI's 27 speech vectors within 0.3-0.6 dB rms of DVSI's decode (1.2-1.5 before);
         * PESQ unchanged (docs/decoder-quality.md, "High-band shelf"; mbelib-neo 2.5 found the
         * gap too). After the renormalisation, so it raises the band rather than the rest. */
        const float hz1 = cur_mp->w0 * (8000.0f / (2.0f * (float)M_PI));
        for (int l = 1; l <= cur_mp->L; l++) {
            const float hz = hz1 * (float)l;
            float t = 0.0f;
            if (hz > 2700.0f && hz < 3900.0f) {
                t = hz < 3100.0f ? (hz - 2700.0f) / 400.0f : hz <= 3700.0f ? 1.0f : (3900.0f - hz) / 200.0f;
            }
            cur_mp->Ml[l] *= 1.0f + ((MBE_HIGH_BAND_SHELF - 1.0f) * t);
        }
    }
}
#endif

void
mbe_spectralAmpEnhance(mbe_parms* cur_mp) {
    (void)mbe_spectralAmpEnhanceWithRm0(cur_mp);
}

/* Output headroom: 95 % of the 16-bit full scale, on the float scale (x 7 in mbe_floattoshort). */
#define MBE_AUDIO_SOFT_CLIP_FLOAT ((32767.0f * 0.95f) / 7.0f)

#if MBEVOCODER_FLOAT_CLIP
/* From mbelib-neo 2.6.2 (implemented independently in 2.6.0): NaN passes through. */
static float
mbe_clipFloatSample(float sample) {
    uint32_t bits;
    uint32_t limit_bits;
    const float limit = MBE_AUDIO_SOFT_CLIP_FLOAT;
    memcpy(&bits, &sample, sizeof(bits));
    memcpy(&limit_bits, &limit, sizeof(limit_bits));
    /* Volatile integer classification remains observable under finite-math assumptions. */
    const volatile uint32_t magnitude = bits & UINT32_C(0x7fffffff);
    if (magnitude > limit_bits && magnitude <= UINT32_C(0x7f800000)) {
        return (bits & UINT32_C(0x80000000)) != 0 ? -limit : limit;
    }
    return sample;
}

static void
mbe_clipFloatBuffer(float* samples, int count) {
    for (int i = 0; i < count; ++i) {
        samples[i] = mbe_clipFloatSample(samples[i]);
    }
}
#endif

#ifndef DISABLE_AMBE_TONES
/* Tone oscillator: a 32-bit phase accumulator per component (from mbelib-neo 2.6.2, implemented
 * independently in 2.6.0). */
static uint32_t
mbe_tonePhaseStep(float frequency) {
    const double fraction = fmod((double)frequency, 8000.0) / 8000.0;
    return (uint32_t)(uint64_t)floor(fraction * 4294967296.0 + 0.5);
}

static float
mbe_toneSampleFromPhase(uint32_t phase) {
    const float angle = (float)((double)phase * (2.0 * M_PI / 4294967296.0) - M_PI / 2.0);
    return sinf(angle);
}

/* mbevocoder: tone amplitude laws measured on an AMBE-3000R (docs/tone-frames.md). The AMBE-3000R
 * renders an AMBE+2 tone frame at 0.71 dB per step of the 7-bit AD field, full scale at 127,
 * and a D-STAR tone frame at 0.354 dB per step of its 8-bit volume field, full scale at
 * about 236.5; dual tones carry each component at the full single-tone amplitude.
 * mbelib scaled AD linearly to 1/7 of full scale. MBEVOCODER_CHIP_MEASURED=0 restores that. */
/* Full scale in the float domain: mbe_floattoshort scales by 7 and clips at 95 %. */
#define MBE_TONE_FULL_SCALE_FLOAT MBE_AUDIO_SOFT_CLIP_FLOAT

static float
mbe_toneGainFromAmplitudeId(int amplitude_id) {
#if MBEVOCODER_CHIP_MEASURED
    if (amplitude_id < 0) {
        return 0.0f;
    }
    float db = 0.71f * (float)(amplitude_id - 127) + 0.9f;
    if (db > 0.0f) {
        db = 0.0f;
    }
    return MBE_TONE_FULL_SCALE_FLOAT * powf(10.0f, db / 20.0f);
#else
    return (((amplitude_id < 0) ? 0.0f : (float)amplitude_id) / 127.0f) * MBE_AUDIO_SOFT_CLIP_FLOAT;
#endif
}

static float
mbe_toneGainFromDstarVolume(int volume) {
#if MBEVOCODER_CHIP_MEASURED
    /* the law runs on above full scale: the AMBE-3000R clips the tone there (mbe_renderDstarTone) */
    const float db = 0.354f * ((float)volume - 236.5f) + 0.4f;
    return MBE_TONE_FULL_SCALE_FLOAT * powf(10.0f, db / 20.0f);
#else
    (void)volume;
    return mbe_toneGainFromAmplitudeId(103); /* JMBE nominal D-STAR tone amplitude */
#endif
}

static void
mbe_renderTonef(float* aout_buf, mbe_parms* cur_mp, float freq1, float freq2, float gain) {
    if (!aout_buf || !cur_mp || freq1 <= 0.0f) {
        mbe_synthesizeSilencef(aout_buf);
        return;
    }

    const int dual_tone = (freq2 > 0.0f) && (fabsf(freq2 - freq1) > 1e-6f);
#if MBEVOCODER_CHIP_MEASURED
    const float component = gain;   /* the AMBE-3000R sums two full-amplitude components */
#else
    const float component = 0.5f * gain;
#endif
    const uint32_t step1 = mbe_tonePhaseStep(freq1);
    const uint32_t step2 = dual_tone ? mbe_tonePhaseStep(freq2) : 0u;
    uint32_t phase1;
    memcpy(&phase1, &cur_mp->swn, sizeof(phase1));
    uint32_t phase2 = cur_mp->tonePhase;

    for (int n = 0; n < 160; n++) {
        phase1 += step1;
        float s1 = mbe_toneSampleFromPhase(phase1);

        if (dual_tone) {
            phase2 += step2;
            float s2 = mbe_toneSampleFromPhase(phase2);
            aout_buf[n] = (component * s1) + (component * s2);
        } else {
            aout_buf[n] = gain * s1;
        }
    }

    memcpy(&cur_mp->swn, &phase1, sizeof(phase1));
    cur_mp->tonePhase = phase2;
}

/* D-STAR dual-tone numbering (AMBE-3000R-verified, docs/tone-frames.md): 128..143 DTMF as
 * 128 + 4 * column + row over columns 1209/1336/1477/1633 Hz and rows 697/770/852/941 Hz,
 * 144 dial, 145 ring, 146 busy, 147 reorder; nothing above 147 (the AMBE-3000R repeats the previous
 * frame: ambe3600x2400.c, ambe2400_measured_tone_action). */
static int
mbe_dstarDualToneFreqs(int index, float* freq1, float* freq2) {
    static const float cols[4] = {1209.0f, 1336.0f, 1477.0f, 1633.0f};
    static const float rows[4] = {697.0f, 770.0f, 852.0f, 941.0f};
    static const float cp[4][2] = {{350.0f, 440.0f}, {440.0f, 480.0f}, {480.0f, 620.0f}, {350.0f, 490.0f}};
    if (index >= 128 && index <= 143) {
        *freq1 = cols[(index - 128) >> 2];
        *freq2 = rows[(index - 128) & 3];
        return 1;
    }
    if (index >= 144 && index <= 147) {
        *freq1 = cp[index - 144][0];
        *freq2 = cp[index - 144][1];
        return 1;
    }
    return 0;
}
#endif

/**
 * @brief Synthesize a tone frame into 160 float samples at 8 kHz.
 * @param aout_buf Output buffer of 160 float samples.
 * @param ambe_d   AMBE parameter bits (49) providing tone indices.
 * @param cur_mp   Current parameter set (tone synthesis state).
 */
void
mbe_synthesizeTonef(float* aout_buf, const char* ambe_d, mbe_parms* cur_mp) {
#ifdef DISABLE_AMBE_TONES // generate silence if tones disabled
    (void)ambe_d;
    (void)cur_mp;
    mbe_synthesizeSilencef(aout_buf);
    return;
#else
    int i;

    if (!cur_mp || mbe_validate_bits(ambe_d, 49u) < 0) {
        mbe_synthesizeSilencef(aout_buf);
        return;
    }

    int u0, u1, u2, u3;
    u0 = u1 = u2 = u3 = 0;

    for (i = 0; i < 12; i++) {
        u0 = u0 << 1;
        u0 = u0 | (int)ambe_d[i];
    }

    for (i = 12; i < 24; i++) {
        u1 = u1 << 1;
        u1 = u1 | (int)ambe_d[i];
    }

    for (i = 24; i < 35; i++) {
        u2 = u2 << 1;
        u2 = u2 | (int)ambe_d[i];
    }

    for (i = 35; i < 49; i++) {
        u3 = u3 << 1;
        u3 = u3 | (int)ambe_d[i];
    }

    int AD, ID0, ID1, ID2, ID3, ID4;
    AD = ((u0 & 0x3f) << 1) + ((u3 >> 4) & 0x1);
    ID0 = 0;
    ID1 = ((u1 & 0xfff) >> 4);
    ID2 = ((u1 & 0xf) << 4) + ((u2 >> 7) & 0xf);
    ID3 = ((u2 & 0x7f) << 1) + ((u2 >> 13) & 0x1);
    ID4 = ((u3 & 0x1fe0) >> 5);

    float freq1, freq2;
    (void)ID0;
    (void)ID2;
    (void)ID3; /* parsed for potential validation */
    (void)ID4;

    if (!mbe_tone_lookup_freqs(ID1, &freq1, &freq2)) {
        mbe_synthesizeSilencef(aout_buf);
        return;
    }

    mbe_renderTonef(aout_buf, cur_mp, freq1, freq2, mbe_toneGainFromAmplitudeId(AD));
#endif
}

/**
 * @brief Synthesize a D-STAR style tone into 160 float samples.
 * @param aout_buf Output buffer of 160 float samples.
 * @param ambe_d   AMBE parameter bits (49).
 * @param cur_mp   Current parameter set.
 * @param ID1      Tone index selector.
 */
#ifndef DISABLE_AMBE_TONES
/* A D-STAR tone index's frequencies (single tones carry freq2 = freq1); 0 for an index with none. */
static int
mbe_dstarToneFreqs(int index, float* freq1, float* freq2) {
    *freq1 = *freq2 = 0.0f;
    if (index == 5) {
        *freq1 = *freq2 = 156.25f;
    } else if (index == 6) {
        *freq1 = *freq2 = 187.5f;
    } else if (index >= 7 && index <= 122) {
        *freq1 = *freq2 = 31.25f * (float)index;
    } else if (!mbe_dstarDualToneFreqs(index, freq1, freq2)) {
        return 0;
    }
    return 1;
}

void
mbe_renderDstarTone(float* aout_buf, mbe_parms* cur_mp, int index, int volume, int from_volume) {
    float freq1, freq2;
    if (!aout_buf || !cur_mp || !mbe_dstarToneFreqs(index, &freq1, &freq2)) {
        mbe_synthesizeSilencef(aout_buf);
        return;
    }
    mbe_renderTonef(aout_buf, cur_mp, freq1, freq2, 1.0f);
    /* a dual tone's level stops rising at about volume 238 (-2.6 dB RMS, the sum clipped) */
    if (freq2 != freq1) {
        volume = volume > 238 ? 238 : volume;
        from_volume = from_volume > 238 ? 238 : from_volume;
    }
    const float g1 = mbe_toneGainFromDstarVolume(volume);
    const float g0 = from_volume >= 0 ? mbe_toneGainFromDstarVolume(from_volume) : g1;
    /* the AMBE-3000R's tone peaks reach 16-bit full scale at about volume 236 and clip there above it
     * (single tone 1250 Hz at volume 255: -1.0 dB RMS; tools/quality/probe_dstar_neo265.py) */
    const float limit = 32767.0f / 7.0f;
    for (int n = 0; n < 160; n++) {
        float v = aout_buf[n] * (n < MBE_DSTAR_TONE_FADE ? g0 + (g1 - g0) * (float)n / (float)MBE_DSTAR_TONE_FADE : g1);
        aout_buf[n] = v > limit ? limit : (v < -limit ? -limit : v);
    }
}
#endif

void
mbe_synthesizeTonefdstar(float* aout_buf, const char* ambe_d, mbe_parms* cur_mp, int ID1) {
#ifdef DISABLE_AMBE_TONES // generate silence if tones disabled
    (void)ambe_d;
    (void)cur_mp;
    (void)ID1;
    mbe_synthesizeSilencef(aout_buf);
    return;
#else
    float freq1 = 0, freq2 = 0;
    if (!cur_mp || !mbe_dstarToneFreqs(ID1, &freq1, &freq2)) {
        mbe_synthesizeSilencef(aout_buf);
        return;
    }
    /* mbevocoder: the D-STAR tone frame carries an 8-bit volume in ambe_d[12..16],[44],[45],[17]
     * (mbelib parsed it for debugging only and rendered every tone at AD 103). */
    int volume = 103 * 2 - 17;
    if (ambe_d && mbe_validate_bits(ambe_d, 49u) == 0) {
        volume = ((int)ambe_d[12] << 7) | ((int)ambe_d[13] << 6) | ((int)ambe_d[14] << 5) | ((int)ambe_d[15] << 4)
                 | ((int)ambe_d[16] << 3) | ((int)ambe_d[44] << 2) | ((int)ambe_d[45] << 1) | (int)ambe_d[17];
    }
#if MBEVOCODER_CHIP_MEASURED
    mbe_renderDstarTone(aout_buf, cur_mp, ID1, volume, -1);
#else
    mbe_renderTonef(aout_buf, cur_mp, freq1, freq2, mbe_toneGainFromDstarVolume(volume));
#endif
#endif
}

/**
 * @brief Write 160 float samples of silence.
 * @param aout_buf Output buffer of 160 float samples.
 */
void
mbe_synthesizeSilencef(float* aout_buf) {
    if (aout_buf == NULL) {
        return;
    }
    memset(aout_buf, 0, 160 * sizeof(*aout_buf));
}

/**
 * @brief Write 160 16-bit samples of silence.
 * @param aout_buf Output buffer of 160 16-bit samples.
 */
void
mbe_synthesizeSilence(short* aout_buf) {
    if (aout_buf == NULL) {
        return;
    }
    memset(aout_buf, 0, 160 * sizeof(*aout_buf));
}

/* Regenerated voiced phase (US 5,701,390 Eqs. 7-9): odd 1/m edge-detection
 * kernel over the enhanced, smoothed log2 magnitudes rendered by synthesis. */
#define MBE_PHASE_KERNEL_HALF_LEN 19
#ifndef MBE_EXP_PHASE_SCALE
#define MBE_EXP_PHASE_SCALE 0.44f      /* regenerated-phase kernel scale (experiment knob) */
#endif
#define MBE_PHASE_KERNEL_SCALE    MBE_EXP_PHASE_SCALE
#define MBE_PHASE_EXTRAP_SLOPE    0.72f
#define MBE_PHASE_MIN_MAGNITUDE   1e-6f

static int
mbe_should_mute_speech(const mbe_parms* cur_mp) {
    int mute_on_error_rate = (fabsf(cur_mp->mutingThreshold - MBE_MUTING_THRESHOLD_AMBE) > 1e-6f);
    return mbe_isMaxFrameRepeat(cur_mp) || (mute_on_error_rate && mbe_requiresMuting(cur_mp));
}

static void
mbe_regenerate_harmonic_phases(const mbe_parms* cur_mp, float phi[57]) {
    const int D = MBE_PHASE_KERNEL_HALF_LEN;
    const int L = cur_mp->L; /* Validated by the synthesis core. */
    /* 1 / (2k + 1) as float, written out: MSVC's /fp:strict (MBEVOCODER_STRICT_FP) does not fold
     * float divisions in a static initializer. Each literal is the correctly rounded quotient
     * (1.0f / 3 etc.), so the values are bit-identical to the divisions. */
    static const float inv_odd[10] = {
        1.0f, 0.333333343f, 0.200000003f, 0.142857149f, 0.111111112f,
        0.0909090936f, 0.0769230798f, 0.0666666701f, 0.0588235296f, 0.0526315793f,
    };
    float B[56 + 2 * MBE_PHASE_KERNEL_HALF_LEN + 1];
    B[D] = 0.0f;
    for (int l = 1; l <= L; ++l) {
        B[D + l] = log2f(fmaxf(cur_mp->Ml[l], MBE_PHASE_MIN_MAGNITUDE));
    }
    /* Define the tail for all 56 phases, including harmonics fading out. */
    for (int l = L + 1; l <= 56 + D; ++l) {
        B[D + l] = B[D + L] - MBE_PHASE_EXTRAP_SLOPE * (float)(l - L);
    }
    for (int l = 1; l <= D; ++l) {
        B[D - l] = B[D + l];
    }
    for (int l = 1; l <= 56; ++l) {
        float acc = 0.0f;
        for (int k = 0; k < 10; ++k) {
            int m = 2 * k + 1;
            acc += (B[D + l + m] - B[D + l - m]) * inv_odd[k];
        }
        phi[l] = MBE_PHASE_KERNEL_SCALE * acc;
    }
}

static int
mbe_reconcile_speech_model_lengths(mbe_parms* cur_mp, mbe_parms* prev_mp) {
    if (cur_mp->L > prev_mp->L) {
        int maxl = cur_mp->L;
        for (int l = prev_mp->L + 1; l <= maxl; l++) {
            prev_mp->Ml[l] = 0.0f;
            prev_mp->Vl[l] = 1;
        }
        return maxl;
    }

    int maxl = prev_mp->L;
    for (int l = cur_mp->L + 1; l <= maxl; l++) {
        cur_mp->Ml[l] = 0.0f;
        cur_mp->Vl[l] = 1;
    }
    return maxl;
}

/**
 * PSIl is integrated linear phase (pulse position). PHIl is the phase at the
 * end of the frame: PSIl plus the regenerated spectral-envelope phase.
 */
static void
mbe_update_speech_phases(mbe_parms* cur_mp, mbe_parms* prev_mp, int N) {
    const float cw0 = cur_mp->w0;
    const float pw0 = prev_mp->w0;
    float phi[57];
    /* (The IMBE standard's own phase model, pitch-integrated phases for the lowest quarter of
     * the harmonics and random offsets above scaled by the unvoiced fraction as JMBE renders
     * them, was tried for IMBE streams in place of the regenerated phases: 0.02 MOS worse
     * on our own frames, docs/decoder-quality.md.) */
    mbe_regenerate_harmonic_phases(cur_mp, phi);

    for (int l = 1; l <= 56; l++) {
        float prev_psil_wrapped = fmodf(prev_mp->PSIl[l], MBE_TWO_PI);
        if (prev_psil_wrapped < 0.0f) {
            prev_psil_wrapped += MBE_TWO_PI;
        }
        prev_mp->PSIl[l] = prev_psil_wrapped;

        cur_mp->PSIl[l] = prev_psil_wrapped + ((pw0 + cw0) * ((float)(l * N) / 2.0f));
        if (MBE_EXP_PHASE_SMOOTH > 0.0f) {
            /* smooth the regenerated phase across frames (prev's is PHIl - PSIl, wrapped) */
            float prevPhi = prev_mp->PHIl[l] - prev_mp->PSIl[l];
            prevPhi -= MBE_TWO_PI * floorf((prevPhi + (float)M_PI) / MBE_TWO_PI);
            float d = phi[l] - prevPhi;
            d -= MBE_TWO_PI * floorf((d + (float)M_PI) / MBE_TWO_PI);
            phi[l] = prevPhi + (1.0f - MBE_EXP_PHASE_SMOOTH) * d;
        }
        cur_mp->PHIl[l] = cur_mp->PSIl[l] + phi[l];
    }
}

/* Mixed excitation (docs/decoder-quality.md): a harmonic with voiced fraction v is rendered
 * as a voiced component of amplitude sqrt(v) M and a noise component of amplitude
 * sqrt(1 - v) M, where M is the amplitude in the respective domain (Ml carries the unvoiced
 * scale for harmonics whose Vl is 0). Frames without Vmix fall back to Vl exactly. */
static void
mbe_split_excitation(const mbe_parms* mp, float voiced[57], float unvoiced[57]) {
    const int L = mp->L;
    for (int l = 1; l <= 56; l++) {
        float m = (l <= L) ? mp->Ml[l] : 0.0f;
        if (!mp->vmixValid || mp->Vmix[l] >= 1.0f || mp->Vmix[l] <= 0.0f) {
            voiced[l] = (mp->Vl[l] == 1) ? m : 0.0f;
            unvoiced[l] = (mp->Vl[l] == 1) ? 0.0f : m;
        } else {
            const float v = mp->Vmix[l];
            const float unvc = (mp->unvcScale > 0.0f) ? mp->unvcScale : 1.0f;
            const float base = (mp->Vl[l] == 1) ? m : m / unvc;   /* voiced-domain amplitude */
            voiced[l] = base * sqrtf(v);
            unvoiced[l] = base * unvc * sqrtf(1.0f - v);
        }
    }
    voiced[0] = unvoiced[0] = 0.0f;
}

static void
mbe_render_voiced_interpolated(float* aout_buf, const mbe_parms* cur_mp, const mbe_parms* prev_mp, int l, int N,
                               float cw0, float pw0, float pw0l, float curAmp, float prevAmp) {
    float* Ss = aout_buf;
    float deltaphil = cur_mp->PHIl[l] - prev_mp->PHIl[l] - (((pw0 + cw0) * (float)(l * N)) / 2.0f);
    float deltawl = (1.0f / (float)N)
                    * (deltaphil - (2.0f * (float)M_PI * floorf((deltaphil + (float)M_PI) / (2.0f * (float)M_PI))));

    if (MBE_EXP_AMP_INTERP == 1 && prevAmp > 1e-6f && curAmp > 1e-6f) {
        /* The geometric ramp a_n = prevAmp (curAmp / prevAmp)^(n / N) and the quadratic phase
         * theta_n = phi + b n + c n^2 by recurrences in double precision: a running product, and
         * a rotation whose step itself rotates by 2c. The direct form called powf and cosf for
         * every sample of every harmonic (all of them are phase-continuous in the measured
         * synthesis), four times the cost of mbelib-neo's synthesis; the two
         * agree to about 1e-5 of the amplitude, closer than the float phase of the direct form. */
        const double b = (double)pw0l + (double)deltawl;
        const double c = ((double)cw0 - (double)pw0) * (double)l / (double)(2 * N);
        const double q = pow((double)curAmp / (double)prevAmp, 1.0 / (double)N);
        double a = (double)prevAmp;
        double zr = cos((double)prev_mp->PHIl[l]), zi = sin((double)prev_mp->PHIl[l]);
        double wr = cos(b + c), wi = sin(b + c);           /* theta_1 - theta_0 */
        const double ur = cos(2.0 * c), ui = sin(2.0 * c); /* the step's own rotation */
        for (int n = 0; n < N; n++) {
            Ss[n] += (float)(2.0 * a * zr);
            const double nr = zr * wr - zi * wi, ni = zr * wi + zi * wr;
            const double vr = wr * ur - wi * ui, vi = wr * ui + wi * ur;
            zr = nr; zi = ni; wr = vr; wi = vi;
            a *= q;
        }
        return;
    }

    for (int n = 0; n < N; n++) {
        float thetaln = prev_mp->PHIl[l] + ((pw0l + deltawl) * (float)n) + (((cw0 - pw0) * (float)(l * n * n)) / (float)(2 * N));
        float aln;
        if (MBE_EXP_AMP_INTERP == 2) {
            float t = (float)n / (float)N;
            t = t * t * (3.0f - 2.0f * t);
            aln = prevAmp + t * (curAmp - prevAmp);
        } else {
            aln = prevAmp + (((float)n / (float)N) * (curAmp - prevAmp));
        }
        *Ss += 2.0f * aln * cosf(thetaln);
        Ss++;
    }
}

static void
mbe_render_voiced_windowed(float* aout_buf, const mbe_parms* cur_mp, const mbe_parms* prev_mp, int l, int N, float cw0l,
                           float pw0l, int cur_voiced, int prev_voiced, float curAmp, float prevAmp) {
    float* Ss = aout_buf;

    if (prev_voiced && cur_voiced) {
        const float gain_prev = 2.0f * prevAmp;
        float sd_prev, cd_prev;
        mbe_sincosf(pw0l, &sd_prev, &cd_prev);
        float s_prev, c_prev;
        mbe_sincosf(prev_mp->PHIl[l], &s_prev, &c_prev);

        const float gain_cur = 2.0f * curAmp;
        float sd_cur, cd_cur;
        mbe_sincosf(cw0l, &sd_cur, &cd_cur);
        float s_cur, c_cur;
        mbe_sincosf(cur_mp->PHIl[l] - (cw0l * (float)N), &s_cur, &c_cur);

        for (int n = 0; n < N; n += 4) {
            const mbe_voiced_block_component prev_component = {Ws + n + N, gain_prev, &c_prev,
                                                               &s_prev,    sd_prev,   cd_prev};
            const mbe_voiced_block_component cur_component = {Ws + n, gain_cur, &c_cur, &s_cur, sd_cur, cd_cur};
            mbe_add_voiced_dual_block4(Ss, &prev_component, &cur_component);
            Ss += 4;
        }
    } else if (prev_voiced) {
        const float gain_prev = 2.0f * prevAmp;
        float sd_prev, cd_prev;
        mbe_sincosf(pw0l, &sd_prev, &cd_prev);
        float s_prev, c_prev;
        mbe_sincosf(prev_mp->PHIl[l], &s_prev, &c_prev);

        for (int n = 0; n < N; n += 4) {
            mbe_add_voiced_block4(Ss, Ws + n + N, gain_prev, &c_prev, &s_prev, sd_prev, cd_prev);
            Ss += 4;
        }
    } else if (cur_voiced) {
        const float gain_cur = 2.0f * curAmp;
        float sd_cur, cd_cur;
        mbe_sincosf(cw0l, &sd_cur, &cd_cur);
        float s_cur, c_cur;
        mbe_sincosf(cur_mp->PHIl[l] - (cw0l * (float)N), &s_cur, &c_cur);

        for (int n = 0; n < N; n += 4) {
            mbe_add_voiced_block4(Ss, Ws + n, gain_cur, &c_cur, &s_cur, sd_cur, cd_cur);
            Ss += 4;
        }
    }
}

static void
mbe_render_voiced_speech(float* aout_buf, const mbe_parms* cur_mp, const mbe_parms* prev_mp, int maxl, int N,
                         const float curAmp[57], const float prevAmp[57]) {
    const float cw0 = cur_mp->w0;
    const float pw0 = prev_mp->w0;

    for (int l = 1; l <= maxl; l++) {
        float cw0l = cw0 * (float)l;
        float pw0l = pw0 * (float)l;
        /* a harmonic takes part in the voiced synthesis when it has a voiced component; with
         * binary voicing that is exactly Vl == 1 (the amplitude 0 of a reconciled length is
         * still rendered, as before) */
        int cur_voiced = cur_mp->vmixValid ? (curAmp[l] > 0.0f || (cur_mp->Vl[l] == 1 && l > cur_mp->L)) : (cur_mp->Vl[l] == 1);
        int prev_voiced = prev_mp->vmixValid ? (prevAmp[l] > 0.0f || (prev_mp->Vl[l] == 1 && l > prev_mp->L)) : (prev_mp->Vl[l] == 1);

        if (cur_voiced || prev_voiced) {
            int use_interpolation = (l < MBE_EXP_INTERP_MAXL) && cur_voiced && prev_voiced
                                    && (fabsf(cw0 - pw0) < (MBE_EXP_INTERP_PITCH * cw0));
            if (use_interpolation) {
                mbe_render_voiced_interpolated(aout_buf, cur_mp, prev_mp, l, N, cw0, pw0, pw0l, cur_voiced ? curAmp[l] : 0.0f,
                                               prev_voiced ? prevAmp[l] : 0.0f);
            } else {
                mbe_render_voiced_windowed(aout_buf, cur_mp, prev_mp, l, N, cw0l, pw0l, cur_voiced, prev_voiced, curAmp[l], prevAmp[l]);
            }
        }
    }
}

static void
mbe_synthesizeSpeechCore(float* aout_buf, mbe_parms* cur_mp, mbe_parms* prev_mp, int has_pre_enh_rm0,
                         float pre_enh_rm0) {

    const int N = 160;

    if (MBE_UNLIKELY(!aout_buf)) {
        return;
    }
    if (MBE_UNLIKELY(!cur_mp || !prev_mp || !mbe_harmonic_count_is_valid(cur_mp->L)
                     || !mbe_harmonic_count_is_valid(prev_mp->L))) {
        mbe_synthesizeSilencef(aout_buf);
        return;
    }
#if MBEVOCODER_CHIP_MEASURED
    if (prev_mp->repeatCount >= MBE_MAX_FRAME_REPEATS && cur_mp->repeatCount == 0) {
        /* The first frame decoded after a mute: the AMBE-3000R plays it at its full level from
         * the start. The synthesis' previous frame is the faded repeat (mbe_renderMute clears its
         * tails), so start this frame steady at its own model instead of ramping up from silence
         * (hil_errors.py --sweeps). */
        prev_mp->w0 = cur_mp->w0;
        prev_mp->L = cur_mp->L;
        memcpy(prev_mp->Ml, cur_mp->Ml, sizeof prev_mp->Ml);
        memcpy(prev_mp->Vl, cur_mp->Vl, sizeof prev_mp->Vl);
        memcpy(prev_mp->Vmix, cur_mp->Vmix, sizeof prev_mp->Vmix);
        prev_mp->vmixValid = cur_mp->vmixValid;
        prev_mp->unvcScale = cur_mp->unvcScale;
    }
#endif

    /* TIA-102.BABA equations 111-116: advance the smoothing state before the mute decision, so a
     * muted frame keeps its parameter progression. */
    if (has_pre_enh_rm0) {
        mbe_applyAdaptiveSmoothingWithRm0(cur_mp, prev_mp, pre_enh_rm0);
    } else {
        mbe_applyAdaptiveSmoothing(cur_mp, prev_mp);
    }

    /* The shared core mutes on the repeat count, and on IMBE's error-rate threshold; the AMBE
     * paths apply their own error-rate mutes. */
    if (mbe_should_mute_speech(cur_mp)) {
        /* Muted frames output comfort noise while preserving model progression. */
        mbe_synthesizeComfortNoisef(aout_buf);
        return;
    }

    /* Algorithm #117: generate the unvoiced noise buffer first, preserving
     * per-stream overlap and LCG consumption independently of voiced phase. */
    float noise_buffer[256];
    mbe_generate_noise_with_overlap(noise_buffer, &cur_mp->noiseSeed, cur_mp->noiseOverlap);

    /* Initialize output buffer to zero */
    memset(aout_buf, 0, N * sizeof(float));

    /* eq 128 and 129: Handle different L values between frames */
    int maxl = mbe_reconcile_speech_model_lengths(cur_mp, prev_mp);

    /* Mixed excitation: voiced and unvoiced amplitude per harmonic for both frames. */
    float curV[57], curU[57], prevV[57], prevU[57];
    mbe_split_excitation(cur_mp, curV, curU);
    mbe_split_excitation(prev_mp, prevV, prevU);

    /* Integrate pulse position and regenerate phase from the spectral envelope. */
    mbe_update_speech_phases(cur_mp, prev_mp, N);

    /* Synthesize voiced components
     * Use phase/amplitude interpolation (Algorithms #134-138) for low harmonics
     * when pitch is stable, otherwise use windowed oscillator approach
     */
    /* Voiced components (Algorithms #134-138 for the low harmonics, windowed oscillators
     * above), then the unvoiced components from the same per-frame noise buffer
     * (equations 117-126); the unvoiced gain and geometric-ramp knobs render them separately. */
    mbe_fft_plan* plan = mbe_get_fft_plan();
    const float* uamp = cur_mp->vmixValid ? curU : NULL;
    if (MBE_EXP_UNVOICED_GAIN == 1.0f && !MBE_EXP_UV_GEO) {
        mbe_render_voiced_speech(aout_buf, cur_mp, prev_mp, maxl, N, curV, prevV);
        if (plan) {
            mbe_synthesizeUnvoicedFFTWithNoiseAmp(aout_buf, cur_mp, prev_mp, plan, noise_buffer, uamp);
        }
    } else {
        float voicedBuf[160], uvBuf[160];
        memset(voicedBuf, 0, sizeof voicedBuf);
        memset(uvBuf, 0, sizeof uvBuf);
        mbe_render_voiced_speech(voicedBuf, cur_mp, prev_mp, maxl, N, curV, prevV);
        if (plan) {
            mbe_synthesizeUnvoicedFFTWithNoiseAmp(uvBuf, cur_mp, prev_mp, plan, noise_buffer, uamp);
        }
        for (int n = 0; n < N; n++) {
            uvBuf[n] *= MBE_EXP_UNVOICED_GAIN;
        }
        if (MBE_EXP_UV_GEO) {
            /* the WOLA ramps the unvoiced level linearly between the frames' unvoiced
             * amplitudes; rescale it to a geometric ramp of the total unvoiced amplitude */
            float ep = 0.0f, ec = 0.0f;
            for (int l = 1; l <= 56; l++) { ep += prevU[l] * prevU[l]; ec += curU[l] * curU[l]; }
            const float ap = sqrtf(ep), ac = sqrtf(ec);
            if (ap > 1e-6f && ac > 1e-6f) {
                for (int n = 0; n < N; n++) {
                    const float t = (float)n / (float)N;
                    const float lin = ap + t * (ac - ap);
                    const float geo = ap * powf(ac / ap, t);
                    uvBuf[n] *= geo / lin;
                }
            }
        }
        for (int n = 0; n < N; n++) {
            aout_buf[n] = voicedBuf[n] + uvBuf[n];
        }
    }

    if (MBE_EXP_HPF) {
        /* Output high-pass (docs/decoder-quality.md, "Low frequencies"): the AMBE-3000R's
         * output has no energy below about 50 Hz and rolls off under 100 Hz, where ours
         * carried the noise generator's leakage and low fundamentals as a rumble that made
         * pauses 5 dB louder than the AMBE-3000R's. Second-order Butterworth, 80 Hz (-3 dB), -9 dB
         * at 50 Hz, -14 dB at 35 Hz. State persists in cur_mp across frames. */
        static const float b0 = 0.95654323f, b1 = -1.91308645f, b2 = 0.95654323f;
        static const float a1 = -1.91119707f, a2 = 0.91497583f;
        float x1 = cur_mp->hpfState[0], x2 = cur_mp->hpfState[1], y1 = cur_mp->hpfState[2], y2 = cur_mp->hpfState[3];
        for (int n = 0; n < N; n++) {
            const float x0 = aout_buf[n];
            const float y0 = b0 * x0 + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = x0; y2 = y1; y1 = y0;
            aout_buf[n] = y0;
        }
        cur_mp->hpfState[0] = x1; cur_mp->hpfState[1] = x2; cur_mp->hpfState[2] = y1; cur_mp->hpfState[3] = y2;
    }

#if MBEVOCODER_CHIP_MEASURED
    /* Output level: the AMBE-3000R's, which is DVSI's (docs/decoder-quality.md, "Output level"):
     * mbelib's synthesis put speech 16.3 dB above it (1.7-1.8 dB below it after mbe-cli's former
     * x0.125, on the corpus and on DVSI's vectors alike), where the 16-bit conversion clipped
     * loud speech and tone frames, already at the AMBE-3000R's level, played 18 dB below the
     * speech around them. mbelib-neo 2.3 moved to DVSI's level the same way. */
    for (int n = 0; n < N; n++) {
        aout_buf[n] *= MBE_OUTPUT_SCALE;
    }
#endif
#if MBEVOCODER_FLOAT_CLIP
    /* mbelib-neo's float-path clipping at 95 % of full scale on the 1/7 scale. Off by
     * default: at mbelib's level it clipped 2-3 % of the samples of normal speech before the
     * caller's own gain could bring it down. mbe_floattoshort still clips. */
    mbe_clipFloatBuffer(aout_buf, N);
#endif
}

void
mbe_synthesizeSpeechWithPreEnhRm0f(float* aout_buf, mbe_parms* cur_mp, mbe_parms* prev_mp, float rm0) {
    mbe_synthesizeSpeechCore(aout_buf, cur_mp, prev_mp, 1, rm0);
}

void
mbe_synthesizeSpeechf(float* aout_buf, mbe_parms* cur_mp, mbe_parms* prev_mp) {
    mbe_synthesizeSpeechCore(aout_buf, cur_mp, prev_mp, 0, 0.0f);
}

/**
 * @brief Comfort noise for a DTX gap: the background the last frame described, continued.
 *
 * The last rendered frame (prev_mp_enhanced) is repeated with every band unvoiced at its
 * level, which is what an AMBE+2 silence frame or a D-STAR index-120 frame renders; the
 * synthesis state moves on so a later voice frame joins cleanly, and the prediction state
 * (prev_mp) stays as it is, as it does over the AMBE-3000R's silence frames.
 */
void
mbe_synthesizeComfortNoiseFramef(float* aout_buf, mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced) {
    if (MBE_UNLIKELY(!aout_buf || !cur_mp || !prev_mp || !prev_mp_enhanced)) {
        return;
    }
    mbe_parms keep;
    mbe_moveMbeParms(prev_mp, &keep);
    mbe_moveMbeParms(prev_mp_enhanced, cur_mp);
    for (int l = 1; l <= cur_mp->L && l < 57; l++) {
        if (cur_mp->Vl[l]) {
            /* a voiced harmonic's amplitude carries no unvoiced scale: apply it, as the
             * decoder does when a band is coded unvoiced */
            cur_mp->Ml[l] *= cur_mp->unvcScale > 0.0f ? cur_mp->unvcScale : 1.0f;
        }
        cur_mp->Vl[l] = 0;
        cur_mp->Vmix[l] = 0.0f;
    }
    cur_mp->repeatCount = 0;
    mbe_moveMbeParms(cur_mp, prev_mp);
    mbe_synthesizeSpeechf(aout_buf, cur_mp, prev_mp_enhanced);
    mbe_moveMbeParms(cur_mp, prev_mp_enhanced);
    mbe_keepModel(&keep, prev_mp);
}

/** 16-bit variant of mbe_synthesizeComfortNoiseFramef(). */
void
mbe_synthesizeComfortNoiseFrame(short* aout_buf, mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced) {
    float float_buf[160];

    if (MBE_UNLIKELY(!aout_buf || !cur_mp || !prev_mp || !prev_mp_enhanced)) {
        return;
    }
    mbe_synthesizeComfortNoiseFramef(float_buf, cur_mp, prev_mp, prev_mp_enhanced);
    mbe_floattoshort(float_buf, aout_buf);
}

/**
 * @brief Synthesize one speech frame into 160 16-bit samples at 8 kHz.
 * @param aout_buf Output buffer of 160 16-bit samples.
 * @param cur_mp   Current parameter set.
 * @param prev_mp  Previous parameter set.
 */
void
mbe_synthesizeSpeech(short* aout_buf, mbe_parms* cur_mp, mbe_parms* prev_mp) {
    float float_buf[160];

    if (MBE_UNLIKELY(!aout_buf)) {
        return;
    }
    mbe_synthesizeSpeechf(float_buf, cur_mp, prev_mp);
    mbe_floattoshort(float_buf, aout_buf);
}

/* 16-bit output: x 7, limited to 95 % of full scale (+-31128.65), NaN as 0. From mbelib-neo 2.6.2's
 * mbe_pcm_convert.c, which mbelib-neo 2.6.0 implemented independently. mbevocoder: with
 * MBEVOCODER_CHIP_MEASURED it rounds to the nearest step (half away from zero) where mbelib truncated
 * towards zero, a dead zone of two steps around zero that distorts quiet output (DVSI's quietest
 * test vectors lost 0.1-0.3 PESQ to it, docs/decoder-quality.md "DVSI's test vectors"); the
 * neo-compat build keeps the truncation. */
#if MBEVOCODER_CHIP_MEASURED
/* the AMBE-3000R's output reaches 16-bit full scale (its loud tones clip there); mbelib limited at 95 % */
#define MBE_PCM_LIMIT   32767.0f
#define MBE_SHORT_ROUND 1
#else
#define MBE_PCM_LIMIT   (32767.0f * 0.95f)
#define MBE_SHORT_ROUND 0
#endif

#if defined(MBEVOCODER_ENABLE_SIMD) && defined(MBE_SIMD_TARGET_SSE2)
static __m128
mbe_scaled_pcm4(const float* src) {
    __m128 value = _mm_mul_ps(_mm_loadu_ps(src), _mm_set1_ps(7.0f));
    value = _mm_and_ps(value, _mm_cmpord_ps(value, value));
    value = _mm_max_ps(value, _mm_set1_ps(-MBE_PCM_LIMIT));
    value = _mm_min_ps(value, _mm_set1_ps(MBE_PCM_LIMIT));
#if MBE_SHORT_ROUND
    value = _mm_add_ps(value, _mm_or_ps(_mm_and_ps(value, _mm_set1_ps(-0.0f)), _mm_set1_ps(0.5f)));
#endif
    return value;
}

static void
mbe_convert_pcm(const float* src, short* dest) {
    for (int i = 0; i < 160; i += 8) {
        const __m128i low = _mm_cvttps_epi32(mbe_scaled_pcm4(src + i));
        const __m128i high = _mm_cvttps_epi32(mbe_scaled_pcm4(src + i + 4));
        _mm_storeu_si128((__m128i*)(void*)(dest + i), _mm_packs_epi32(low, high));
    }
}
#elif defined(MBEVOCODER_ENABLE_SIMD) && defined(MBE_SIMD_TARGET_NEON)
static int32x4_t
mbe_scaled_pcm4(const float* src) {
    float32x4_t value = vmulq_n_f32(vld1q_f32(src), 7.0f);
    value = vreinterpretq_f32_u32(vandq_u32(vreinterpretq_u32_f32(value), vceqq_f32(value, value)));
    value = vmaxq_f32(value, vdupq_n_f32(-MBE_PCM_LIMIT));
    value = vminq_f32(value, vdupq_n_f32(MBE_PCM_LIMIT));
#if MBE_SHORT_ROUND
    {
        const uint32x4_t sign = vandq_u32(vreinterpretq_u32_f32(value), vdupq_n_u32(0x80000000u));
        value = vaddq_f32(value, vreinterpretq_f32_u32(vorrq_u32(sign, vreinterpretq_u32_f32(vdupq_n_f32(0.5f)))));
    }
#endif
    return vcvtq_s32_f32(value);
}

static void
mbe_convert_pcm(const float* src, short* dest) {
    for (int i = 0; i < 160; i += 8) {
        const int16x4_t low = vqmovn_s32(mbe_scaled_pcm4(src + i));
        const int16x4_t high = vqmovn_s32(mbe_scaled_pcm4(src + i + 4));
        vst1q_s16(dest + i, vcombine_s16(low, high));
    }
}
#else
static void
mbe_convert_pcm(const float* src, short* dest) {
    for (int i = 0; i < 160; ++i) {
        float value = src[i] * 7.0f;
        if (isnan(value)) {
            value = 0.0f;
        } else if (value > MBE_PCM_LIMIT) {
            value = MBE_PCM_LIMIT;
        } else if (value < -MBE_PCM_LIMIT) {
            value = -MBE_PCM_LIMIT;
        }
#if MBE_SHORT_ROUND
        value += (value < 0.0f) ? -0.5f : 0.5f;
#endif
        dest[i] = (short)value;
    }
}
#endif

/**
 * @brief Convert 160 float samples to clipped/scaled 16-bit PCM.
 * @param float_buf Input 160 float samples.
 * @param aout_buf  Output 160 16-bit samples.
 */
void
mbe_floattoshort(const float* float_buf, short* aout_buf) {
    if (float_buf == NULL || aout_buf == NULL) {
        return;
    }
    mbe_convert_pcm(float_buf, aout_buf);
}
