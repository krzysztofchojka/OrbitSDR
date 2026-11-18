// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Internal helpers shared by AMBE 3600x2400 and 3600x2450 paths: parameter-state
 *        defaults, erasure and repeat models (the frame ECC is in src/bits).
 */

#include "internal/config.h"
#include "ambe_common.h"
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "mbe_adaptive.h"
#include "mbelib-neo/mbelib.h"

/* The AMBE reset and erasure models. From mbelib-neo 2.6.2, which implemented them independently in
 * 2.6.0 (its legal/implementation-provenance.md); mbevocoder: the mixed-excitation fields. */
void
mbe_initAmbeParms_common(mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced) {
    if (cur_mp == NULL || prev_mp == NULL || prev_mp_enhanced == NULL) {
        return;
    }
    mbe_parms initial;
    memset(&initial, 0, sizeof(initial));
    /* the silence frame's model (index 124): w0 = (pi / 32) * 2 pi, 15 harmonics, all unvoiced */
    initial.w0 = (float)((M_PI / 32.0) * (2.0 * M_PI));
    initial.L = 15;
    for (int l = 0; l <= 56; ++l) {
        initial.Ml[l] = 1.0f;
    }
    initial.noiseSeed = -1.0f;
    initial.localEnergy = MBE_DEFAULT_LOCAL_ENERGY;
    initial.amplitudeThreshold = MBE_DEFAULT_AMPLITUDE_THRESHOLD;
    initial.mutingThreshold = MBE_MUTING_THRESHOLD_AMBE;
    initial.unvcScale = 1.0f;
#if MBEVOCODER_CHIP_MEASURED
    initial.chipErrorRate = prev_mp->chipErrorRate;   /* D-STAR's mute measure is the stream's */
#endif
    memcpy(cur_mp, &initial, sizeof(initial));
    memcpy(prev_mp, &initial, sizeof(initial));
    memcpy(prev_mp_enhanced, &initial, sizeof(initial));
}

void
mbe_setAmbeErasureParms_common(mbe_parms* mp, const mbe_parms* state_src) {
    if (mp == NULL) {
        return;
    }
    if (state_src != NULL) {
        memmove(mp->PHIl, state_src->PHIl, sizeof(mp->PHIl));
        memmove(mp->PSIl, state_src->PSIl, sizeof(mp->PSIl));
        mp->noiseSeed = state_src->noiseSeed;
        memmove(mp->noiseOverlap, state_src->noiseOverlap, sizeof(mp->noiseOverlap));
        memmove(mp->previousUw, state_src->previousUw, sizeof(mp->previousUw));
    }
    mp->w0 = 0.0f;
    mp->L = 9;
    mp->K = 0;
    mp->gamma = 0.0f;
    mp->swn = 0;
    mp->tonePhase = 0;
    for (int l = 0; l <= 56; ++l) {
        mp->Ml[l] = 1.0f;
        mp->Vl[l] = 0;
        mp->log2Ml[l] = 0.0f;
    }
    mp->localEnergy = MBE_DEFAULT_LOCAL_ENERGY;
    mp->amplitudeThreshold = MBE_DEFAULT_AMPLITUDE_THRESHOLD;
#if MBEVOCODER_CHIP_MEASURED
    /* the erasure model is all unvoiced: no stale mixed-excitation table or silence
     * flag; the output high-pass keeps its state for continuity */
    memset(mp->Vmix, 0, sizeof(mp->Vmix));
    mp->unvcScale = 1.0f;
    mp->vmixValid = 0;
    mp->silenceFrame = 0;
    mp->lastTone = 0;
#endif
}

void
mbe_ensureAmbeDefaults_common(mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced) {
    if (!prev_mp) {
        return;
    }

    if (fabsf(prev_mp->mutingThreshold - MBE_MUTING_THRESHOLD_AMBE) > 1e-6f) {
        mbe_initAmbeParms_common(cur_mp, prev_mp, prev_mp_enhanced);
    }
}
