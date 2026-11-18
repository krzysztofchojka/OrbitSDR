// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 *
 * Internal header for adaptive smoothing (TIA-102.BABA equations 111-116) and comfort noise.
 */

#ifndef MBEINT_MBE_ADAPTIVE_H
#define MBEINT_MBE_ADAPTIVE_H

#include <stdint.h>

#include "mbelib-neo/mbelib.h"

/* Constants from TIA-102.BABA sections 8 and 9 (equations 111-116) */

/** Default local energy value (equation 111). */
#define MBE_DEFAULT_LOCAL_ENERGY        75000.0f

/** Minimum local energy threshold (equation 111). */
#define MBE_MIN_LOCAL_ENERGY            10000.0f

/** Energy smoothing coefficient alpha (equation 111). */
#define MBE_ENERGY_SMOOTH_ALPHA         0.95f

/** Energy smoothing coefficient beta (equation 111). */
#define MBE_ENERGY_SMOOTH_BETA          0.05f

/** Default amplitude threshold (equation 115). */
#define MBE_DEFAULT_AMPLITUDE_THRESHOLD 20480

/** Error rate threshold for smoothing entry (equation 112). */
#define MBE_ERROR_THRESHOLD_ENTRY       0.0125f

/** Low error rate threshold (equation 112). */
#define MBE_ERROR_THRESHOLD_LOW         0.005f

/** Adaptive gain constant (equation 112). */
#define MBE_ADAPTIVE_GAIN               45.255f

/** Adaptive exponent constant (equation 112). */
#define MBE_ADAPTIVE_EXPONENT           277.26f

/** Alternative adaptive multiplier (equation 112). */
#define MBE_ADAPTIVE_ALT                1.414f

/** Amplitude penalty per error (equation 115). */
#define MBE_AMPLITUDE_PENALTY_PER_ERROR 300

/** Amplitude base constant (equation 115). */
#define MBE_AMPLITUDE_BASE              6000

/**
 * @brief Apply adaptive smoothing with caller-supplied pre-enhancement RM0.
 *
 * TIA-102.BABA equation 111 uses local energy derived from pre-enhanced spectral
 * amplitudes. Internal decode/synthesize paths pass that value explicitly so
 * the handoff cannot retain caller stack addresses.
 *
 * @param cur_mp Current frame parameters (modified in-place).
 * @param prev_mp Previous frame parameters (for local energy).
 * @param rm0 Sum of squared pre-enhancement amplitudes.
 */
void mbe_applyAdaptiveSmoothingWithRm0(mbe_parms* cur_mp, const mbe_parms* prev_mp, float rm0);

/**
 * @brief Apply spectral amplitude enhancement and return pre-enhancement RM0.
 * @param cur_mp In/out parameter set to enhance.
 * @return Sum of squared amplitudes before enhancement.
 */
float mbe_spectralAmpEnhanceWithRm0(mbe_parms* cur_mp);

/**
 * @brief Synthesize speech using a captured pre-enhancement RM0 value.
 * @param aout_buf Output buffer of 160 float samples.
 * @param cur_mp Current frame parameters.
 * @param prev_mp Previous enhanced frame parameters.
 * @param rm0 Sum of squared pre-enhancement amplitudes.
 */
void mbe_synthesizeSpeechWithPreEnhRm0f(float* aout_buf, mbe_parms* cur_mp, mbe_parms* prev_mp, float rm0);

/**
 * @brief Seed the comfort-noise RNG used by mbe_synthesizeComfortNoisef().
 *
 * Uses Java Random-compatible 48-bit state initialization.
 *
 * @param seed Thread-local seed value (0 maps to a fixed non-zero default).
 */
void mbe_seedComfortNoiseRng(uint32_t seed);

/**
 * @brief Load the previous frame's model into cur_mp for a frame repeat.
 *
 * MBEVOCODER_CHIP_MEASURED: the model (and repeatCount, which the caller increments) comes from
 * prev_mp, while cur_mp keeps this frame's error state (errorRate, error counts, muting
 * threshold: the error-rate recursion and the mute tests see every frame, TIA-102.BABA 7.7) and
 * its synthesis state (noise generator and overlap, tone phase, output high-pass), which must run
 * on rather than replay the previous frame's (as mbelib-neo 2.3, mbe_repeat_load_model). Off:
 * mbe_useLastMbeParms(), a copy of the whole of prev_mp, as mbelib-neo 2.1.
 */
void mbe_repeatModel(mbe_parms* cur_mp, const mbe_parms* prev_mp);

/** The library's default voice model (pitch index 134), with no change to the stream's continuity
 *  state; IMBE's model after prolonged repeats, and the initial model. */
void mbe_resetImbeModel(mbe_parms* mp);

#endif /* MBEINT_MBE_ADAPTIVE_H */
