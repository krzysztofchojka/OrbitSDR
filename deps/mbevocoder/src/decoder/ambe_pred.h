/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Spectral amplitude prediction coefficients shared by the decoders and the closed-loop
 * encoder (the encoder must form its residuals with the coefficient its decoder uses). */
#ifndef MBEVOCODER_AMBE_PRED_H
#define MBEVOCODER_AMBE_PRED_H

#include "internal/config.h"

/* AMBE+2 (3600x2450, 2450): 0.65, as in mbelib. */
#ifndef AMBE2450_PRED
#define AMBE2450_PRED ((float)0.65)
#endif

/* D-STAR (3600x2400): mbelib used 0.65 (the AMBE+2 value); the AMBE-3000R's D-STAR decoder
 * measures ~0.77 by step response and 0.79 minimises the spectral distance to the AMBE-3000R's
 * audio on the corpus (docs/decoder-quality.md). Both D-STAR values follow
 * MBEVOCODER_CHIP_MEASURED, which covers every measured D-STAR correction. */
#ifndef AMBE2400_PRED
#if MBEVOCODER_CHIP_MEASURED
#define AMBE2400_PRED ((float)0.79)
#else
#define AMBE2400_PRED ((float)0.65)   /* mbelib's value */
#endif
#endif

/* Gain (gamma) prediction: gamma = 2 (1 - c) dg[b2] + c gamma_prev, so the steady state is
 * 2 dg[b2] for every c. AMBE+2 uses c = 0.5 (mbelib, matches the AMBE-3000R's step response). The
 * AMBE-3000R's D-STAR decoder has no gain memory: a one-frame gain excursion is rendered in
 * full and is gone the next frame (tools/quality/probe_gain_step.py, docs/decoder-quality.md);
 * mbelib used 0.5 here too. */
#define AMBE2450_GAIN_PRED ((float)0.5)
#ifndef AMBE2400_GAIN_PRED
#if MBEVOCODER_CHIP_MEASURED
#define AMBE2400_GAIN_PRED ((float)0.0)
#else
#define AMBE2400_GAIN_PRED ((float)0.5)   /* mbelib's value */
#endif
#endif

#endif
