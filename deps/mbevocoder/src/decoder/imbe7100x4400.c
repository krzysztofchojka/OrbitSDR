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
 * @brief IMBE 7100x4400 parameter decode, ECC, and synthesis hooks.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "imbe4400_internal.h"
#include "mbe_bitpack.h"
#include "mbe_result.h"
#include "mbelib-neo/mbelib.h"


/**
 * @brief Process a complete IMBE 7100x4400 frame into float PCM.
 * @param aout_buf Output buffer of 160 float samples.
 * @param result   Optional output status populated by decode and synthesis.
 * @param imbe_fr  Input frame as 7x24 bitplanes.
 * @param imbe_d   Scratch/output parameter bits (88).
 * @param cur_mp,prev_mp,prev_mp_enhanced Parameter state as per Dataf variant.
 */
int
mbe_processImbe7100x4400Framef(float* aout_buf, mbe_process_result* result, const char imbe_fr[7][24], char imbe_d[88],
                               mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced) {
    mbe_process_result local_result;
    int ret;
    if (!result) {
        result = &local_result;
    }
    ret = mbe_decodeImbe7100x4400Frame(imbe_fr, imbe_d, result);
    if (ret < 0) {
        return ret;
    }
    return mbe_processImbe4400Dataf_internal(aout_buf, result, imbe_d, cur_mp, prev_mp, prev_mp_enhanced);
}

int
mbe_processImbe7100x4400SoftFramef(float* aout_buf, mbe_process_result* result, const mbe_soft_bit imbe_fr[7][24],
                                   char imbe_d[88], mbe_parms* cur_mp, mbe_parms* prev_mp,
                                   mbe_parms* prev_mp_enhanced) {
    mbe_process_result local_result;
    int ret;
    if (!result) {
        result = &local_result;
    }
    ret = mbe_decodeImbe7100x4400SoftFrame(imbe_fr, imbe_d, result);
    if (ret < 0) {
        return ret;
    }
    return mbe_processImbe4400Dataf_internal(aout_buf, result, imbe_d, cur_mp, prev_mp, prev_mp_enhanced);
}

int
mbe_processImbe7100x4400SoftFrame(short* aout_buf, mbe_process_result* result, const mbe_soft_bit imbe_fr[7][24],
                                  char imbe_d[88], mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced) {
    float float_buf[160];
    int ret;
    if (!aout_buf) {
        return MBE_STATUS_INVALID_ARGUMENT;
    }
    ret = mbe_processImbe7100x4400SoftFramef(float_buf, result, imbe_fr, imbe_d, cur_mp, prev_mp, prev_mp_enhanced);
    if (ret < 0) {
        return ret;
    }
    mbe_floattoshort(float_buf, aout_buf);
    return ret;
}

/**
 * @brief Process a complete IMBE 7100x4400 frame into 16-bit PCM.
 * @see mbe_processImbe7100x4400Framef for details.
 */
int
mbe_processImbe7100x4400Frame(short* aout_buf, mbe_process_result* result, const char imbe_fr[7][24], char imbe_d[88],
                              mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced) {

    float float_buf[160];
    int ret;
    if (!aout_buf) {
        return MBE_STATUS_INVALID_ARGUMENT;
    }
    ret = mbe_processImbe7100x4400Framef(float_buf, result, imbe_fr, imbe_d, cur_mp, prev_mp, prev_mp_enhanced);
    if (ret < 0) {
        return ret;
    }
    mbe_floattoshort(float_buf, aout_buf);
    return ret;
}
