// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Internal helpers shared by the AMBE 3600x2400 and 3600x2450 decoders: parameter-state
 *        defaults, erasure and repeat models, and the mute (the frame ECC is in src/bits).
 */

#ifndef MBELIB_NEO_INTERNAL_AMBE_COMMON_H
#define MBELIB_NEO_INTERNAL_AMBE_COMMON_H

#include "mbelib-neo/mbelib.h"

/* The C0 / C1 error correction and demodulation that mbelib-neo kept here are in src/bits
 * (ambe_frame.cpp), behind the public mbe_eccAmbe* / mbe_decodeAmbe*Frame functions. */

/**
 * @brief Initialize AMBE parameter state to the library's AMBE defaults.
 *
 * The AMBE decode paths start from the silence frame's model (index 124:
 * w0 = (pi / 32) * 2 pi, L = 15) with unvoiced bands and unit spectral
 * amplitudes, as mbelib-neo does (and JMBE did).
 *
 * @param cur_mp  Output current parameter state.
 * @param prev_mp Output previous parameter state.
 * @param prev_mp_enhanced Output enhanced previous parameter state.
 */
void mbe_initAmbeParms_common(mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced);
/* Restore the model fields (fundamental, harmonics, voicing, amplitudes, gain) of `keep` into
 * `dst`, leaving dst's synthesis and error state as they are: silence frames do not advance
 * the AMBE-3000R's prediction state (docs/frontend.md). */
void mbe_keepModel(const mbe_parms* keep, mbe_parms* dst);
/* Mute after the repeats are exhausted, and reset the model state for the next good frame.
 * MBEVOCODER_CHIP_MEASURED: as the AMBE-3000R does it, the last repeat faded to silence within the
 * frame and silence after; otherwise mbelib's comfort noise. `cur_mp` holds the repeated
 * model on entry. (docs/decoder-quality.md, "Bit errors") */
void mbe_renderMute(float* aout_buf, mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced);
/* A frame with nothing to decode, as the AMBE-3000R plays a lost frame: the previous model repeated
 * (mbe_repeatModel), the fourth in a row faded to silence and silence after until a frame decodes.
 * Sets MBE_PROCESS_FLAG_REPEAT (and _MUTE); `result` is not otherwise touched. */
void mbe_repeatFrameAudio(float* aout_buf, mbe_process_result* result, mbe_parms* cur_mp, mbe_parms* prev_mp,
                          mbe_parms* prev_mp_enhanced);
/* MBEVOCODER_CHIP_MEASURED, AMBE+2: a repeat whose last played frame was a tone (`cur_mp->lastTone`)
 * replays that tone: in full for repeats 1-3, faded within the fourth, silence after (the
 * AMBE-3000R's lost-frame schedule, measured with damaged tone frames). */
void mbe_renderToneRepeat(float* aout_buf, mbe_process_result* result, mbe_parms* cur_mp);

/* MBEVOCODER_CHIP_MEASURED, D-STAR tones (docs/decoder-quality.md, "D-STAR tones"): the AMBE-3000R
 * moves into and out of a tone, and between two volumes of one tone, over the first
 * MBE_DSTAR_TONE_FADE samples of a frame. */
#define MBE_DSTAR_TONE_FADE 80

/* Render D-STAR tone `index` at `volume`, continuing the oscillators' phases in cur_mp; with
 * from_volume >= 0 the level moves from that volume's over the first MBE_DSTAR_TONE_FADE samples.
 * Silence for an index with no tone. */
void mbe_renderDstarTone(float* aout_buf, mbe_parms* cur_mp, int index, int volume, int from_volume);

/* D-STAR: a lost frame (cur_mp already the repeated model, its repeatCount advanced) right after a
 * tone frame plays what the AMBE-3000R plays there; returns 0 when no tone is held (cur_mp->dstarTone),
 * for the voice-model repeat. */
int mbe_dstarRenderLostAfterTone(float* aout_buf, mbe_process_result* result, mbe_parms* cur_mp, mbe_parms* prev_mp,
                                 mbe_parms* prev_mp_enhanced);

/**
 * @brief Set AMBE parameters to the erasure model (index 120..123).
 *
 * w0 = 0,
 * L=9, unvoiced bands, unit amplitudes, zero gain term. Keeps phase and
 * unvoiced overlap/noise state from `state_src` to preserve synthesizer
 * continuity behavior.
 *
 * @param mp Target parameter set to rewrite.
 * @param state_src Source state for phase/noise continuity (may be NULL).
 */
void mbe_setAmbeErasureParms_common(mbe_parms* mp, const mbe_parms* state_src);

/**
 * @brief Ensure AMBE parameter state is initialized with AMBE defaults.
 *
 * If the previous state does not appear to be AMBE-initialized, this
 * routine resets all three state structs via mbe_initAmbeParms_common().
 *
 * @param cur_mp  In/out current parameter state.
 * @param prev_mp In/out previous parameter state.
 * @param prev_mp_enhanced In/out enhanced previous parameter state.
 */
void mbe_ensureAmbeDefaults_common(mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced);

#endif /* MBELIB_NEO_INTERNAL_AMBE_COMMON_H */
