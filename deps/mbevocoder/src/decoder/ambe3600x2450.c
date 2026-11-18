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
 * @brief AMBE 3600x2450 parameter decode, ECC, and synthesis hooks.
 */

#include "internal/config.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ambe3600x2450_const.h"
#include "ambe_common.h"
#include "ambe_pred.h"
#include "mbe_adaptive.h"
#include "mbe_compiler.h"
#include "mbe_result.h"
#include "mbe_tone.h"
#include "mbe_validation.h"
#include "mbelib-neo/mbelib.h"

/**
 * @brief Thread-local cache for AMBE DCT cosine coefficients.
 *
 * Pre-computes the cosine terms used in the Ri inverse DCT and per-block
 * inverse DCT loops to eliminate repeated cosf() calls per frame.
 *
 * Ri DCT: cosf(M_PI * (m-1) * (i-0.5) / 8) for m=1..8, i=1..8
 * Per-block IDCT: cosf(M_PI * (k-1) * (j-0.5) / ji) for ji=1..17, j=1..ji, k=1..ji
 */
struct ambe2450_dct_cache {
    int inited;
    float ri_cos[9][9];         /* [m][i] for m=1..8, i=1..8 (index 0 unused) */
    float idct_cos[18][18][18]; /* [ji][j][k] for ji=1..17, j=1..ji, k=1..ji */
};

static MBE_THREAD_LOCAL struct ambe2450_dct_cache ambe2450_cache = {0};

/**
 * @brief Initialize or return the thread-local AMBE 2450 DCT cache.
 *
 * Fills the cosine tables on first use. Because the cache is thread-local,
 * no locking is required.
 *
 * @return Pointer to the initialized cache.
 */
static struct ambe2450_dct_cache*
ambe2450_get_dct_cache(void) {
    if (ambe2450_cache.inited) {
        return &ambe2450_cache;
    }

    /* Fill Ri DCT cosine table: cosf(M_PI * (m-1) * (i-0.5) / 8) */
    for (int m = 1; m <= 8; m++) {
        for (int i = 1; i <= 8; i++) {
            ambe2450_cache.ri_cos[m][i] = cosf((M_PI * (float)(m - 1) * ((float)i - 0.5f)) / 8.0f);
        }
    }

    /* Fill per-block IDCT cosine table: cosf(M_PI * (k-1) * (j-0.5) / ji) */
    for (int ji = 1; ji <= 17; ji++) {
        for (int j = 1; j <= ji; j++) {
            for (int k = 1; k <= ji; k++) {
                ambe2450_cache.idct_cos[ji][j][k] = cosf((M_PI * (float)(k - 1) * ((float)j - 0.5f)) / (float)ji);
            }
        }
    }

    ambe2450_cache.inited = 1;
    return &ambe2450_cache;
}

static int
ambe2450_is_valid_tone_id(const char ambe_d[49]) {
    int id1 = 0;
    /* AMBE tone ID1 is U1[0..7] (bits 12..19 in ambe_d), not the full 12-bit U1 field. */
    for (int i = 12; i < 20; i++) {
        id1 = (id1 << 1) | (int)ambe_d[i];
    }

    return mbe_tone_id_is_valid(id1);
}



#if MBEVOCODER_CHIP_MEASURED
#include "ambe3600x2450_vuvmix.h"
#endif

/* Voicing: mbelib's table gives voiced/unvoiced per band; with MBEVOCODER_CHIP_MEASURED the
 * AMBE-3000R's measured voiced fraction and level offset per index and band are used as
 * well (mixed excitation, docs/decoder-quality.md). gain[l] receives the level offset. */
static void
ambe2450_decode_vuv(const char* ambe_d, mbe_parms* cur_mp, int L, float f0, int silence, int b0, float gain[57]) {
    (void)b0;
    int b1 = 0;
    b1 |= ambe_d[4] << 4;
    b1 |= ambe_d[5] << 3;
    b1 |= ambe_d[6] << 2;
    b1 |= ambe_d[7] << 1;
    b1 |= ambe_d[35];

    cur_mp->vmixValid = 0;
    for (int l = 1; l <= L; l++) {
        int jl = (int)((float)l * (float)16.0 * f0);
        gain[l] = 1.0f;
        if (silence == 0) {
#if MBEVOCODER_CHIP_MEASURED
            const float v = AmbeVuvMix[b1][jl];
            cur_mp->Vl[l] = (v >= 0.5f) ? 1 : 0;
            cur_mp->Vmix[l] = v;
            gain[l] = powf(10.0f, AmbeVuvGainDb[b1][jl] / 20.0f);
            cur_mp->vmixValid = 1;
#else
            cur_mp->Vl[l] = AmbeVuv[b1][jl];
#endif
        }
#ifdef AMBE_DEBUG
        fprintf(stderr, "jl[%i]:%i Vl[%i]:%i\n", l, jl, l, cur_mp->Vl[l]);
#endif
    }
#ifdef AMBE_DEBUG
    fprintf(stderr, "\nb0:%i w0:%f L:%i b1:%i\n", b0, cur_mp->w0, L, b1);
#endif
}

static void
ambe2450_decode_ri(const char* ambe_d, float Ri[9]) {
    float Gm[9];
    Gm[1] = 0;

    int b3 = 0;
    b3 |= ambe_d[12] << 8;
    b3 |= ambe_d[13] << 7;
    b3 |= ambe_d[14] << 6;
    b3 |= ambe_d[15] << 5;
    b3 |= ambe_d[16] << 4;
    b3 |= ambe_d[17] << 3;
    b3 |= ambe_d[18] << 2;
    b3 |= ambe_d[19] << 1;
#if MBEVOCODER_CHIP_MEASURED
    b3 |= ambe_d[43];   /* the AMBE-3000R's order of b3 / b4's low bits (src/params/fields.cpp) */
#else
    b3 |= ambe_d[40];
#endif
    Gm[2] = AmbePRBA24[b3][0];
    Gm[3] = AmbePRBA24[b3][1];
    Gm[4] = AmbePRBA24[b3][2];

    int b4 = 0;
    b4 |= ambe_d[20] << 6;
    b4 |= ambe_d[21] << 5;
    b4 |= ambe_d[22] << 4;
    b4 |= ambe_d[23] << 3;
#if MBEVOCODER_CHIP_MEASURED
    b4 |= ambe_d[40] << 2;
    b4 |= ambe_d[41] << 1;
    b4 |= ambe_d[42];
#else
    b4 |= ambe_d[41] << 2;
    b4 |= ambe_d[42] << 1;
    b4 |= ambe_d[43];
#endif
    Gm[5] = AmbePRBA58[b4][0];
    Gm[6] = AmbePRBA58[b4][1];
    Gm[7] = AmbePRBA58[b4][2];
    Gm[8] = AmbePRBA58[b4][3];

#ifdef AMBE_DEBUG
    fprintf(stderr, "b3: %i Gm[2]: %f Gm[3]: %f Gm[4]: %f b4: %i Gm[5]: %f Gm[6]: %f Gm[7]: %f Gm[8]: %f\n", b3, Gm[2],
            Gm[3], Gm[4], b4, Gm[5], Gm[6], Gm[7], Gm[8]);
#endif

    const struct ambe2450_dct_cache* cache = ambe2450_get_dct_cache();
    for (int i = 1; i <= 8; i++) {
        float sum = 0;
        for (int m = 1; m <= 8; m++) {
            int am = (m == 1) ? 1 : 2;
            sum = sum + ((float)am * Gm[m] * cache->ri_cos[m][i]);
        }
        Ri[i] = sum;
#ifdef AMBE_DEBUG
        fprintf(stderr, "R%i: %f ", i, Ri[i]);
#endif
    }
#ifdef AMBE_DEBUG
    fprintf(stderr, "\n");
#endif
}

static void
ambe2450_decode_cik(const char* ambe_d, int L, const float Ri[9], float Cik[5][18], int Ji[5]) {
    const float rconst = ((float)1 / ((float)2 * M_SQRT2));
    Cik[1][1] = (float)0.5 * (Ri[1] + Ri[2]);
    Cik[1][2] = rconst * (Ri[1] - Ri[2]);
    Cik[2][1] = (float)0.5 * (Ri[3] + Ri[4]);
    Cik[2][2] = rconst * (Ri[3] - Ri[4]);
    Cik[3][1] = (float)0.5 * (Ri[5] + Ri[6]);
    Cik[3][2] = rconst * (Ri[5] - Ri[6]);
    Cik[4][1] = (float)0.5 * (Ri[7] + Ri[8]);
    Cik[4][2] = rconst * (Ri[7] - Ri[8]);

    int b5 = 0;
    b5 |= ambe_d[24] << 4;
    b5 |= ambe_d[25] << 3;
    b5 |= ambe_d[26] << 2;
    b5 |= ambe_d[27] << 1;
    b5 |= ambe_d[44];

    int b6 = 0;
    b6 |= ambe_d[28] << 3;
    b6 |= ambe_d[29] << 2;
    b6 |= ambe_d[30] << 1;
    b6 |= ambe_d[45];

    int b7 = 0;
    b7 |= ambe_d[31] << 3;
    b7 |= ambe_d[32] << 2;
    b7 |= ambe_d[33] << 1;
    b7 |= ambe_d[46];

    int b8 = 0;
    b8 |= ambe_d[34] << 2;
    b8 |= ambe_d[47] << 1;
    b8 |= ambe_d[48];

    Ji[1] = AmbeLmprbl[L][0];
    Ji[2] = AmbeLmprbl[L][1];
    Ji[3] = AmbeLmprbl[L][2];
    Ji[4] = AmbeLmprbl[L][3];
#ifdef AMBE_DEBUG
    fprintf(stderr, "Ji[1]: %i Ji[2]: %i Ji[3]: %i Ji[4]: %i\n", Ji[1], Ji[2], Ji[3], Ji[4]);
    fprintf(stderr, "b5: %i b6: %i b7: %i b8: %i\n", b5, b6, b7, b8);
#endif

    for (int k = 3; k <= Ji[1]; k++) {
        if (k > 6) {
            Cik[1][k] = 0;
        } else {
            Cik[1][k] = AmbeHOCb5[b5][k - 3];
#ifdef AMBE_DEBUG
            fprintf(stderr, "C1,%i: %f ", k, Cik[1][k]);
#endif
        }
    }
    for (int k = 3; k <= Ji[2]; k++) {
        if (k > 6) {
            Cik[2][k] = 0;
        } else {
            Cik[2][k] = AmbeHOCb6[b6][k - 3];
#ifdef AMBE_DEBUG
            fprintf(stderr, "C2,%i: %f ", k, Cik[2][k]);
#endif
        }
    }
    for (int k = 3; k <= Ji[3]; k++) {
        if (k > 6) {
            Cik[3][k] = 0;
        } else {
            Cik[3][k] = AmbeHOCb7[b7][k - 3];
#ifdef AMBE_DEBUG
            fprintf(stderr, "C3,%i: %f ", k, Cik[3][k]);
#endif
        }
    }
    for (int k = 3; k <= Ji[4]; k++) {
        if (k > 6) {
            Cik[4][k] = 0;
        } else {
            Cik[4][k] = AmbeHOCb8[b8][k - 3];
#ifdef AMBE_DEBUG
            fprintf(stderr, "C4,%i: %f ", k, Cik[4][k]);
#endif
        }
    }
#ifdef AMBE_DEBUG
    fprintf(stderr, "\n");
#endif
}

static void
ambe2450_inverse_dct_tl(float Cik[5][18], const int Ji[5], float Tl[57]) {
    const struct ambe2450_dct_cache* cache = ambe2450_get_dct_cache();
    int l = 1;
    for (int i = 1; i <= 4; i++) {
        int ji = Ji[i];
        for (int j = 1; j <= ji; j++) {
            float sum = 0;
            for (int k = 1; k <= ji; k++) {
                int ak = (k == 1) ? 1 : 2;
#ifdef AMBE_DEBUG
                fprintf(stderr, "j: %i Cik[%i][%i]: %f ", j, i, k, Cik[i][k]);
#endif
                sum = sum + ((float)ak * Cik[i][k] * cache->idct_cos[ji][j][k]);
            }
            Tl[l] = sum;
#ifdef AMBE_DEBUG
            fprintf(stderr, "Tl[%i]: %f\n", l, Tl[l]);
#endif
            l++;
        }
    }
}

static void
ambe2450_update_spectral_amplitudes(mbe_parms* cur_mp, mbe_parms* prev_mp, const float Tl[57], float unvc) {
    int intkl[57] = {0};
    float flokl[57] = {0.0f};
    float deltal[57] = {0.0f};
    int prev_L = mbe_clamp_harmonic_count(prev_mp->L);
    cur_mp->L = mbe_clamp_harmonic_count(cur_mp->L);

    if (cur_mp->L > prev_L) {
        for (int l = prev_L + 1; l <= cur_mp->L; l++) {
            prev_mp->Ml[l] = prev_mp->Ml[prev_L];
            prev_mp->log2Ml[l] = prev_mp->log2Ml[prev_L];
        }
    }
    /* the previous frame's amplitude below harmonic 1 is harmonic 1's (mbelib); the IMBE
     * standard's zero is what the AMBE-3000R does not do here: it costs 0.03 / 0.2 dB against
     * its audio on AMBE+2 / D-STAR (docs/decoder-quality.md, "IMBE: the previous frame's
     * amplitude below harmonic 1") */
    prev_mp->log2Ml[0] = prev_mp->log2Ml[1];
    prev_mp->Ml[0] = prev_mp->Ml[1];

    float Sum43 = 0;
    for (int l = 1; l <= cur_mp->L; l++) {
        flokl[l] = ((float)prev_L / (float)cur_mp->L) * (float)l;
        intkl[l] = (int)(flokl[l]);
#ifdef AMBE_DEBUG
        fprintf(stderr, "flok%i: %f, intk%i: %i ", l, flokl[l], l, intkl[l]);
#endif
        deltal[l] = flokl[l] - (float)intkl[l];
#ifdef AMBE_DEBUG
        fprintf(stderr, "delta%i: %f ", l, deltal[l]);
#endif
        /* intkl[l] can reach 56; keep the upper interpolation tap inside log2Ml[57]. */
        int upper = intkl[l] + 1;
        if (upper > MBE_MAX_HARMONIC_BANDS) {
            upper = MBE_MAX_HARMONIC_BANDS;
        }
        Sum43 = Sum43 + ((((float)1 - deltal[l]) * prev_mp->log2Ml[intkl[l]]) + (deltal[l] * prev_mp->log2Ml[upper]));
    }
    Sum43 = ((AMBE2450_PRED / (float)cur_mp->L) * Sum43);
#ifdef AMBE_DEBUG
    fprintf(stderr, "\n");
    fprintf(stderr, "Sum43: %f\n", Sum43);
#endif

    float Sum42 = 0;
    for (int l = 1; l <= cur_mp->L; l++) {
        Sum42 += Tl[l];
    }
    Sum42 = Sum42 / (float)cur_mp->L;
    float BigGamma = cur_mp->gamma - (0.5f * log2f((float)cur_mp->L)) - Sum42;

    for (int l = 1; l <= cur_mp->L; l++) {
        int upper = intkl[l] + 1;
        if (upper > MBE_MAX_HARMONIC_BANDS) {
            upper = MBE_MAX_HARMONIC_BANDS;
        }
        float c1 = (AMBE2450_PRED * ((float)1 - deltal[l]) * prev_mp->log2Ml[intkl[l]]);
        float c2 = (AMBE2450_PRED * deltal[l] * prev_mp->log2Ml[upper]);
        cur_mp->log2Ml[l] = Tl[l] + c1 + c2 - Sum43 + BigGamma;
        if (cur_mp->Vl[l] == 1) {
            cur_mp->Ml[l] = exp2f(cur_mp->log2Ml[l]);
        } else {
            cur_mp->Ml[l] = unvc * exp2f(cur_mp->log2Ml[l]);
        }
#ifdef AMBE_DEBUG
        fprintf(stderr, "flokl[%i]: %f, intkl[%i]: %i ", l, flokl[l], l, intkl[l]);
        fprintf(stderr, "deltal[%i]: %f ", l, deltal[l]);
        fprintf(stderr, "prev_mp->log2Ml[%i]: %f\n", l, prev_mp->log2Ml[intkl[l]]);
        fprintf(stderr, "BigGamma: %f c1: %f c2: %f Sum43: %f Tl[%i]: %f log2Ml[%i]: %f Ml[%i]: %f\n", BigGamma, c1, c2,
                Sum43, l, Tl[l], l, cur_mp->log2Ml[l], l, cur_mp->Ml[l]);
#endif
    }
}

static int
ambe2450_decode_b0(const char* ambe_d) {
    int b0 = 0;
    b0 |= ambe_d[0] << 6;
    b0 |= ambe_d[1] << 5;
    b0 |= ambe_d[2] << 4;
    b0 |= ambe_d[3] << 3;
    b0 |= ambe_d[37] << 2;
    b0 |= ambe_d[38] << 1;
    b0 |= ambe_d[39];
    return b0;
}

#if !MBEVOCODER_CHIP_MEASURED
/* mbelib-neo 2.1's tone classification: the tone header (u0's top six bits all ones) and a check
 * over the redundant fields of TIA-102.BABA-1 7.2 Table 10: u3's last four bits zero, or u1's
 * trailing copy of ID(7..4) equal to the first. (From mbelib-neo 2.6.2, which implemented the check
 * independently in 2.6.0.) */
static int
ambe2450_tone_verified(const char* ambe_d) {
    for (int i = 0; i < 6; ++i) {
        if (ambe_d[i] != 1) {
            return 0;
        }
    }
    if ((ambe_d[45] | ambe_d[46] | ambe_d[47] | ambe_d[48]) == 0) {
        return 1;
    }
    for (int i = 0; i < 4; ++i) {
        if (ambe_d[12 + i] != ambe_d[20 + i]) {
            return 0;
        }
    }
    return 1;
}
#endif

#if MBEVOCODER_CHIP_MEASURED
static int
ambe2450_bits(const char* ambe_d, int first, int count) {
    int v = 0;
    for (int i = first; i < first + count; i++) {
        v = (v << 1) | (int)ambe_d[i];
    }
    return v;
}

/* A frame whose header (u0's top six bits) is all ones is a tone frame to the AMBE-3000R (a
 * silence frame's voicing bits never make it one). It plays the tone only when the four copies of
 * the tone ID agree: u1 = ID, ID[7:4]; u2 = ID[3:0], ID[7:1]; u3 = ID[0], ID, AD[0], 0000; any
 * other is a lost frame, whatever its fundamental index decodes as, and there is no gate on the
 * error counts as in JMBE's check (tools/quality/probe_ambe2450_frames.py, damaged tone frames). */
static int
ambe2450_tone_header(const char* ambe_d) {
    return ambe2450_bits(ambe_d, 0, 6) == 0x3f;
}

static int
ambe2450_tone_agrees(const char* ambe_d) {
    const int id1 = ambe2450_bits(ambe_d, 12, 8);
    const int id2 = ambe2450_bits(ambe_d, 20, 8);
    const int id3 = ambe2450_bits(ambe_d, 28, 8);
    const int id4 = ambe2450_bits(ambe_d, 36, 8);
    return (id1 == id2) && (id1 == id3) && (id1 == id4);
}

static uint64_t
ambe2450_pack_tone(const char* ambe_d) {
    uint64_t bits = 0;
    for (int i = 0; i < 49; i++) {
        bits |= (uint64_t)(ambe_d[i] & 1) << i;
    }
    return bits;
}
#endif

static void
ambe2450_set_silence_model(mbe_parms* cur_mp, int b0, int* L, float* f0, int* silence) {
    *silence = 1;
#if MBEVOCODER_CHIP_MEASURED
    /* mbevocoder: the AMBE-3000R renders a silence frame (fundamental index 124 or 125, the
     * encoder's DTX output) as 15 (14) unvoiced bands 250 Hz wide, w0 = 2 pi / 32, the model
     * mbelib uses for D-STAR silence frames; predicted from the last voice frame and
     * without advancing the prediction state (docs/frontend.md). JMBE's w0 of pi/32 * 2 pi
     * put the bands 2 kHz apart. */
    cur_mp->w0 = (float)(2.0 * M_PI) / 32.0f;
    *f0 = cur_mp->w0 / (float)(2.0 * M_PI);
#else
    /* JMBE AMBEFundamentalFrequency.W124/W125: constructor uses (frequency * 2*PI), where frequency is PI/32. */
    *f0 = (float)M_PI / 32.0f;
    cur_mp->w0 = *f0 * (float)(2.0 * M_PI);
#endif
    *L = (b0 == 124) ? 15 : 14; /* JMBE maps W124->L15, W125->L14 */
    cur_mp->L = *L;
    cur_mp->vmixValid = 0;
    for (int l = 1; l <= *L; l++) {
        cur_mp->Vl[l] = 0;
    }
}

static int
ambe2450_setup_frame_model(const char* ambe_d, mbe_parms* cur_mp, int total_errors, int* b0, int* L, float* f0,
                           int* silence) {
    *silence = 0;

#if MBEVOCODER_CHIP_MEASURED
    (void)total_errors;
    if (ambe2450_tone_header(ambe_d)) {
        return ambe2450_tone_agrees(ambe_d) ? 7 : 2;
    }
#else
    /* JMBE-compatible tone classification:
     * - Tone if U0 tone check passes and either U3 check is zero or U1 high/low nibbles match.
     * - If total BER is known and >= 6, do not classify as tone (JMBE AMBEFrame). */
    if (ambe2450_tone_verified(ambe_d) && total_errors < 6) {
#ifdef AMBE_DEBUG
        fprintf(stderr, "Tone Frame\n");
#endif
        return 7;
    }
#endif

    *b0 = ambe2450_decode_b0(ambe_d);

    if ((*b0 >= 120) && (*b0 <= 123)) {
#ifdef AMBE_DEBUG
        fprintf(stderr, "Erasure Frame b0 = %d\n", *b0);
#endif
        return 2;
    }
    if ((*b0 == 124) || (*b0 == 125)) {
#ifdef AMBE_DEBUG
        fprintf(stderr, "Silence Frame\n");
#endif
        ambe2450_set_silence_model(cur_mp, *b0, L, f0, silence);
        return 0;
    }
    /* If the fundamental decodes as a tone but tone verification failed above,
     * treat this as erasure to match JMBE's fallback behavior. */
    if ((*b0 == 126) || (*b0 == 127)) {
#ifdef AMBE_DEBUG
        fprintf(stderr, "Unverified tone fundamental -> erasure\n");
#endif
        return 2;
    }
    if ((*b0 < 0) || (*b0 >= 120)) {
        return 2;
    }

#if MBEVOCODER_CHIP_MEASURED
    *f0 = AmbeW0Measured(*b0);
    *L = (int)AmbeLtableMeasured[*b0];
#else
    *f0 = AmbeW0table[*b0];
    *L = AmbeLtable[*b0];
#endif
    cur_mp->w0 = *f0 * (float)2 * M_PI;
    cur_mp->L = *L;
#if MBEVOCODER_CHIP_MEASURED
    /* Voicing indices 18-31 (all bands unvoiced, the AMBE-3000R's own encoder uses them on
     * a sixth of its unvoiced frames) are rendered by the AMBE-3000R on a band grid that
     * does not follow the fundamental index: the L bands span a fixed 3.58 kHz, w0 = 2.81 / L
     * (measured as the frequency scaling of their spectra against index 16 with the same
     * codes, at six fundamentals; docs/decoder-quality.md, "Unvoiced rows 18-31"). The
     * harmonic count, and so the shape decode, is the index's. */
    {
        const int b1 = (ambe_d[4] << 4) | (ambe_d[5] << 3) | (ambe_d[6] << 2) | (ambe_d[7] << 1) | ambe_d[35];
        if (b1 >= 18) {
            cur_mp->w0 = 2.81f / (float)*L;
            *f0 = cur_mp->w0 / ((float)2 * M_PI);
        }
    }
#endif
    return 0;
}

/**
 * @brief Internal AMBE 2450 parameter decode with optional tone BER gate.
 *
 * @param ambe_d  Demodulated AMBE parameter bits (49).
 * @param cur_mp  Output: current frame parameters.
 * @param prev_mp Input: previous frame parameters (for prediction).
 * @param total_errors Total frame error count for JMBE tone gating, or <0 to disable.
 * @return Tone index or 0 for voice; implementation-specific non-zero for special frames.
 */
static int
mbe_decodeAmbe2450ParmsInternal(const char* ambe_d, mbe_parms* cur_mp, mbe_parms* prev_mp, int total_errors) {

    int b0, b2, L = 0;
    float f0, Cik[5][18], Tl[57] = {0}, Ri[9];
    int silence = 0;
    int Ji[5];
    float deltaGamma;
    float unvc;
    int ret;

    if (!cur_mp || !prev_mp) {
        return MBE_STATUS_INVALID_ARGUMENT;
    }
    ret = mbe_validate_bits(ambe_d, 49u);
    if (ret < 0) {
        return ret;
    }

#ifdef AMBE_DEBUG
    fprintf(stderr, "\n");
#endif

    int special_frame = ambe2450_setup_frame_model(ambe_d, cur_mp, total_errors, &b0, &L, &f0, &silence);
    if (special_frame != 0) {
        return special_frame;
    }

    unvc = (float)0.2046 / sqrtf(cur_mp->w0);
#if MBEVOCODER_CHIP_MEASURED
    /* silence frames come out 13.8 dB (a further factor 0.2046) below the voice frame law on
     * the AMBE-3000R, measured against 18 preceding-frame/shape combinations (sd 0.4 dB) */
    if (silence) unvc *= (float)0.2046;
    cur_mp->silenceFrame = silence;
#endif
    cur_mp->unvcScale = unvc;

    // decode V/UV parameters
    float vuvGain[57];
    ambe2450_decode_vuv(ambe_d, cur_mp, L, f0, silence, b0, vuvGain);

    // decode gain vector
    // load b2 from ambe_d
    b2 = 0;
    b2 |= ambe_d[8] << 4;
    b2 |= ambe_d[9] << 3;
    b2 |= ambe_d[10] << 2;
    b2 |= ambe_d[11] << 1;
    b2 |= ambe_d[36];

    deltaGamma = AmbeDg[b2];
    cur_mp->gamma = deltaGamma + ((float)0.5 * prev_mp->gamma);
#ifdef AMBE_DEBUG
    fprintf(stderr, "b2: %i, deltaGamma: %f gamma: %f gamma-1: %f\n", b2, deltaGamma, cur_mp->gamma, prev_mp->gamma);
#endif

    // decode PRBA/HOC vectors and inverse DCT
    ambe2450_decode_ri(ambe_d, Ri);
    ambe2450_decode_cik(ambe_d, L, Ri, Cik, Ji);
    ambe2450_inverse_dct_tl(Cik, Ji, Tl);

    // determine log2Ml by applying ci,j to previous log2Ml
    ambe2450_update_spectral_amplitudes(cur_mp, prev_mp, Tl, unvc);
    // mixed excitation: the AMBE-3000R's level offset per voicing index and band
    if (cur_mp->vmixValid) {
        for (int l = 1; l <= L; l++) {
            cur_mp->Ml[l] *= vuvGain[l];
        }
    }

    return 0;
}

/**
 * @brief Decode AMBE 2450 parameters from demodulated bitstream.
 * @param ambe_d  Demodulated AMBE parameter bits (49).
 * @param cur_mp  Output: current frame parameters.
 * @param prev_mp Input: previous frame parameters (for prediction).
 * @return Tone index or 0 for voice; implementation-specific non-zero for tone frames.
 */
int
mbe_decodeAmbe2450Parms(const char* ambe_d, mbe_parms* cur_mp, mbe_parms* prev_mp) {
    return mbe_decodeAmbe2450ParmsInternal(ambe_d, cur_mp, prev_mp, -1);
}


static int
ambe2450_prepare_process(mbe_process_result* result, const char ambe_d[49], mbe_parms* cur_mp, mbe_parms* prev_mp,
                         mbe_parms* prev_mp_enhanced, int* total_errors, int* c0_errors, int* c0_errors_valid) {
    int ret;

    if (!cur_mp || !prev_mp || !prev_mp_enhanced || !total_errors || !c0_errors || !c0_errors_valid) {
        return MBE_STATUS_INVALID_ARGUMENT;
    }
    ret = mbe_result_resolve_total_errors(result, total_errors);
    if (ret < 0) {
        return ret;
    }
    ret = mbe_validate_bits(ambe_d, 49u);
    if (ret < 0) {
        return ret;
    }

    *c0_errors_valid = (result && ((result->flags & MBE_PROCESS_FLAG_C0_VALID) != 0u));
    *c0_errors = *c0_errors_valid ? result->c0_errors : 0;
    mbe_result_prepare_synthesis(result, *total_errors);

    /* JMBE AMBE starts from W124 defaults; normalize if caller used generic init. */
    mbe_ensureAmbeDefaults_common(cur_mp, prev_mp, prev_mp_enhanced);

    /* Set AMBE-specific muting threshold (9.6% vs IMBE's 8.75%). */
    cur_mp->mutingThreshold = MBE_MUTING_THRESHOLD_AMBE;

    cur_mp->errorCountTotal = *total_errors;
    cur_mp->errorCount4 = 0; /* AMBE has no Hamming cosets */
    cur_mp->errorRate = (0.95f * prev_mp->errorRate) + (0.001064f * (float)cur_mp->errorCountTotal);
    return 0;
}


static int
ambe2450_repeat_required(int total_errors, int c0_errors, int c0_errors_valid) {
    if (c0_errors_valid) {
#if MBEVOCODER_CHIP_MEASURED
        /* TIA-102.BABA-1 with full-weight counts: repeat when C0 is uncorrectable */
        (void)total_errors;
        return c0_errors >= 4;
#else
        /* mbelib-neo 2.1's repeat criteria when C0 errors are known */
        return ((c0_errors >= 4) || ((c0_errors >= 2) && (total_errors >= 6)));
#endif
    }

    /* Dataf callers pass parameter bits only (no C0 context); keep historical total-error fallback. */
    return (total_errors > 3);
}

static void
ambe2450_update_decode_state(int bad, int total_errors, int c0_errors, int c0_errors_valid, mbe_process_result* result,
                             mbe_parms* cur_mp, const mbe_parms* prev_mp) {
    if (bad == 2) {
        mbe_result_set_flag(result, MBE_PROCESS_FLAG_ERASURE);
        cur_mp->repeatCount = 0;
        /* JMBE erasure model: W120 defaults carried as previous frame state. */
        mbe_setAmbeErasureParms_common(cur_mp, prev_mp);
        return;
    }
    if (bad == 3 || bad == 7) {
        mbe_result_set_flag(result, MBE_PROCESS_FLAG_TONE);
        cur_mp->repeatCount = 0;
        return;
    }
    if (ambe2450_repeat_required(total_errors, c0_errors, c0_errors_valid)) {
        mbe_repeatModel(cur_mp, prev_mp);
        cur_mp->repeatCount++;
        mbe_result_set_flag(result, MBE_PROCESS_FLAG_REPEAT);
        return;
    }

    cur_mp->repeatCount = 0;
    cur_mp->lastTone = 0;   /* a voice or silence frame plays: repeats repeat it */
}

static void
ambe2450_synthesize_voice(float* aout_buf, mbe_process_result* result, mbe_parms* cur_mp, mbe_parms* prev_mp,
                          mbe_parms* prev_mp_enhanced) {
    if (cur_mp->repeatCount < MBE_MAX_FRAME_REPEATS) {
#if MBEVOCODER_CHIP_MEASURED
        /* a silence frame is predicted from the last voice frame every time: it does not
         * become the prediction state (the AMBE-3000R's level and spectrum stay put however many
         * silence frames follow a voice frame; docs/frontend.md) */
        mbe_parms keep;
        const int freeze = cur_mp->silenceFrame;
        if (freeze) mbe_moveMbeParms(prev_mp, &keep);
#endif
        mbe_moveMbeParms(cur_mp, prev_mp);
        float pre_enh_rm0 = mbe_spectralAmpEnhanceWithRm0(cur_mp);
        mbe_synthesizeSpeechWithPreEnhRm0f(aout_buf, cur_mp, prev_mp_enhanced, pre_enh_rm0);
        mbe_moveMbeParms(cur_mp, prev_mp_enhanced);
#if MBEVOCODER_CHIP_MEASURED
        if (freeze) mbe_keepModel(&keep, prev_mp);
#endif
        return;
    }

    mbe_result_set_flag(result, MBE_PROCESS_FLAG_MUTE);
    mbe_renderMute(aout_buf, cur_mp, prev_mp, prev_mp_enhanced);
}

static void
ambe2450_synthesize_tone(float* aout_buf, const char ambe_d[49], mbe_parms* cur_mp, mbe_parms* prev_mp,
                         mbe_parms* prev_mp_enhanced) {
    if (ambe2450_is_valid_tone_id(ambe_d)) {
        mbe_synthesizeTonef(aout_buf, ambe_d, cur_mp);
        return;
    }
    if (!mbe_isMaxFrameRepeat(prev_mp)) {
        mbe_parms synth_mp;

        /* JMBE invalid-tone behavior reuses the prior VOICE model while still advancing synth state. */
        mbe_moveMbeParms(prev_mp_enhanced, &synth_mp);
        mbe_synthesizeSpeechf(aout_buf, &synth_mp, prev_mp_enhanced);
        mbe_moveMbeParms(&synth_mp, prev_mp_enhanced);
        return;
    }

    mbe_synthesizeComfortNoisef(aout_buf);
    mbe_initAmbeParms_common(cur_mp, prev_mp, prev_mp_enhanced);
}

static void
ambe2450_synthesize_erasure(float* aout_buf, const mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced) {
    /* JMBE erasure behavior: synthesize white noise and keep ERASURE
     * parameters as the previous-frame context for recovery. */
    mbe_synthesizeComfortNoisef(aout_buf);
    mbe_moveMbeParms(cur_mp, prev_mp);
    mbe_moveMbeParms(cur_mp, prev_mp_enhanced);
}

static void
ambe2450_synthesize_frame(float* aout_buf, mbe_process_result* result, const char ambe_d[49], int bad,
                          mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced) {
    if (bad == 0) {
        ambe2450_synthesize_voice(aout_buf, result, cur_mp, prev_mp, prev_mp_enhanced);
        return;
    }
    if (bad == 7) {
        ambe2450_synthesize_tone(aout_buf, ambe_d, cur_mp, prev_mp, prev_mp_enhanced);
        return;
    }
    if (bad == 2) {
        ambe2450_synthesize_erasure(aout_buf, cur_mp, prev_mp, prev_mp_enhanced);
        return;
    }

    mbe_synthesizeComfortNoisef(aout_buf);
    mbe_initAmbeParms_common(cur_mp, prev_mp, prev_mp_enhanced);
}

static int
mbe_processAmbe2450Dataf_internal(float* aout_buf, mbe_process_result* result, const char ambe_d[49], mbe_parms* cur_mp,
                                  mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced) {
    int bad;
    int c0_errors;
    int c0_errors_valid;
    int total_errors;
    int ret;

    if (!aout_buf || !cur_mp || !prev_mp || !prev_mp_enhanced) {
        return MBE_STATUS_INVALID_ARGUMENT;
    }
    ret = ambe2450_prepare_process(result, ambe_d, cur_mp, prev_mp, prev_mp_enhanced, &total_errors, &c0_errors,
                                   &c0_errors_valid);
    if (ret < 0) {
        return ret;
    }

    bad = mbe_decodeAmbe2450ParmsInternal(ambe_d, cur_mp, prev_mp, total_errors);
    if (bad < 0) {
        return bad;
    }

#if MBEVOCODER_CHIP_MEASURED
    /* The AMBE-3000R plays an erasure (fundamental index 120-123, 126-127 without the tone header,
     * a tone frame whose redundant fields disagree) and a tone frame whose index it cannot play
     * (the silent 255 included) as it plays a lost frame: the last model repeated three times, the
     * fourth faded and silence after, with no reset of the prediction state, so the next voice
     * frame decodes at its level. JMBE's comfort noise and W120 reset left the frames after an
     * erasure 20-30 dB low while the prediction recovered (tools/quality/probe_ambe2450_frames.py;
     * TIA-102.BABA-1 7.3 and mbelib-neo 2.3 repeat erasures too). */
    if ((bad == 2) || ((bad == 7) && !ambe2450_is_valid_tone_id(ambe_d))) {
        mbe_result_set_flag(result, MBE_PROCESS_FLAG_ERASURE);
        if (bad == 7) {
            mbe_result_set_flag(result, MBE_PROCESS_FLAG_TONE);
        }
        mbe_repeatFrameAudio(aout_buf, result, cur_mp, prev_mp, prev_mp_enhanced);
        return result ? result->total_errors : total_errors;
    }
    /* A frame the FEC says to repeat repeats the frame last played, and when that was a tone the
     * tone is what plays (with the lost-frame schedule): a tone frame with an uncorrectable C0
     * included. */
    if (((bad == 7) || (cur_mp->lastTone != 0u)) && ambe2450_repeat_required(total_errors, c0_errors, c0_errors_valid)) {
        if (bad == 7) {
            mbe_result_set_flag(result, MBE_PROCESS_FLAG_TONE);
        }
        mbe_repeatFrameAudio(aout_buf, result, cur_mp, prev_mp, prev_mp_enhanced);
        return result ? result->total_errors : total_errors;
    }
#endif
    const int b0 = ambe2450_decode_b0(ambe_d);
    const int silence = (bad == 0) && ((b0 == 124) || (b0 == 125));
    ambe2450_update_decode_state(bad, total_errors, c0_errors, c0_errors_valid, result, cur_mp, prev_mp);
    ambe2450_synthesize_frame(aout_buf, result, ambe_d, bad, cur_mp, prev_mp, prev_mp_enhanced);
#if MBEVOCODER_CHIP_MEASURED
    if (bad == 7) {
        /* the tone played: it is what a following lost frame repeats */
        cur_mp->lastTone = prev_mp->lastTone = prev_mp_enhanced->lastTone = ambe2450_pack_tone(ambe_d);
        prev_mp->repeatCount = prev_mp_enhanced->repeatCount = 0;
    }
    /* TIA-102.BABA-1's error-rate mute (silence while the model state moves on): with the
     * full-weight counts and the standard's threshold it mutes what the AMBE-3000R mutes, 98 % of
     * frames alike under steady 4-12 % bit errors (tools/quality/probe_ambe2450_frames.py) */
    if (cur_mp->errorRate > cur_mp->mutingThreshold) {
        memset(aout_buf, 0, 160 * sizeof(float));
        mbe_result_set_flag(result, MBE_PROCESS_FLAG_MUTE);
    }
#endif
    /* mbelib-neo 2.4: a silence frame that was played (not replaced by a repeat or a mute) */
    if (silence && result && (result->flags & (MBE_PROCESS_FLAG_REPEAT | MBE_PROCESS_FLAG_MUTE)) == 0u) {
        mbe_result_set_flag(result, MBE_PROCESS_FLAG_SILENCE);
    }
    return result ? result->total_errors : total_errors;
}

/**
 * @brief Process AMBE 2450 parameters into 160 float samples at 8 kHz.
 * @param aout_buf Output buffer of 160 float samples.
 * @param result   Optional in/out status context.
 * @param ambe_d   Demodulated parameter bits (49).
 * @param cur_mp   In/out: current frame parameters (may be enhanced).
 * @param prev_mp  In/out: previous frame parameters.
 * @param prev_mp_enhanced In/out: enhanced previous parameters for continuity.
 */
int
mbe_processAmbe2450Dataf(float* aout_buf, mbe_process_result* result, const char ambe_d[49], mbe_parms* cur_mp,
                         mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced) {
    mbe_process_result local_result;

    if (!result) {
        mbe_initProcessResult(&local_result);
        result = &local_result;
    }
    return mbe_processAmbe2450Dataf_internal(aout_buf, result, ambe_d, cur_mp, prev_mp, prev_mp_enhanced);
}

int
mbe_processAmbe2450Data(short* aout_buf, mbe_process_result* result, const char ambe_d[49], mbe_parms* cur_mp,
                        mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced) {
    float float_buf[160];
    int ret;
    if (!aout_buf) {
        return MBE_STATUS_INVALID_ARGUMENT;
    }
    ret = mbe_processAmbe2450Dataf(float_buf, result, ambe_d, cur_mp, prev_mp, prev_mp_enhanced);
    if (ret < 0) {
        return ret;
    }
    mbe_floattoshort(float_buf, aout_buf);
    return ret;
}

/**
 * @brief Process a complete AMBE 3600x2450 frame into float PCM.
 * @param aout_buf Output buffer of 160 float samples.
 * @param result   Optional output status populated by decode and synthesis.
 * @param ambe_fr  Input frame as 4x24 bitplanes.
 * @param ambe_d   Scratch/output parameter bits (49).
 * @param cur_mp,prev_mp,prev_mp_enhanced Parameter state as per Dataf variant.
 */
int
mbe_processAmbe3600x2450Framef(float* aout_buf, mbe_process_result* result, const char ambe_fr[4][24], char ambe_d[49],
                               mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced) {
    mbe_process_result local_result;
    int ret;
    if (!result) {
        result = &local_result;
    }
    ret = mbe_decodeAmbe3600x2450Frame(ambe_fr, ambe_d, result);
    if (ret < 0) {
        return ret;
    }
    return mbe_processAmbe2450Dataf(aout_buf, result, ambe_d, cur_mp, prev_mp, prev_mp_enhanced);
}

int
mbe_processAmbe3600x2450SoftFramef(float* aout_buf, mbe_process_result* result, const mbe_soft_bit ambe_fr[4][24],
                                   char ambe_d[49], mbe_parms* cur_mp, mbe_parms* prev_mp,
                                   mbe_parms* prev_mp_enhanced) {
    mbe_process_result local_result;
    int ret;
    if (!result) {
        result = &local_result;
    }
    ret = mbe_decodeAmbe3600x2450SoftFrame(ambe_fr, ambe_d, result);
    if (ret < 0) {
        return ret;
    }
    return mbe_processAmbe2450Dataf(aout_buf, result, ambe_d, cur_mp, prev_mp, prev_mp_enhanced);
}

int
mbe_processAmbe3600x2450SoftFrame(short* aout_buf, mbe_process_result* result, const mbe_soft_bit ambe_fr[4][24],
                                  char ambe_d[49], mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced) {
    float float_buf[160];
    int ret;
    if (!aout_buf) {
        return MBE_STATUS_INVALID_ARGUMENT;
    }
    ret = mbe_processAmbe3600x2450SoftFramef(float_buf, result, ambe_fr, ambe_d, cur_mp, prev_mp, prev_mp_enhanced);
    if (ret < 0) {
        return ret;
    }
    mbe_floattoshort(float_buf, aout_buf);
    return ret;
}

/**
 * @brief Process a complete AMBE 3600x2450 frame into 16-bit PCM.
 * @see mbe_processAmbe3600x2450Framef for details.
 */
int
mbe_processAmbe3600x2450Frame(short* aout_buf, mbe_process_result* result, const char ambe_fr[4][24], char ambe_d[49],
                              mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced) {
    float float_buf[160];
    int ret;

    if (!aout_buf) {
        return MBE_STATUS_INVALID_ARGUMENT;
    }
    ret = mbe_processAmbe3600x2450Framef(float_buf, result, ambe_fr, ambe_d, cur_mp, prev_mp, prev_mp_enhanced);
    if (ret < 0) {
        return ret;
    }
    mbe_floattoshort(float_buf, aout_buf);
    return ret;
}
