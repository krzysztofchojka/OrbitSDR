/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file mbelib/mbelib.h
 * @brief The mbelib 1.x API on mbevocoder, for code written against the original mbelib.
 *
 * Includes the decoder API (`<mbelib-neo/mbelib.h>`) and gives the functions whose
 * signatures mbelib-neo 2.x changed their mbelib 1.x form:
 *
 *  - the `mbe_process*` functions take `int* errs, int* errs2, char* err_str` and a trailing
 *    `int uvquality` instead of an ::mbe_process_result;
 *  - mbe_synthesizeSpeech() / mbe_synthesizeSpeechf() take `uvquality`;
 *  - mbe_printVersion() exists.
 *
 * Every other function is the same in both APIs and is available unchanged. A translation unit
 * uses one decoder API or the other: this header maps the mbelib 1.x names onto exported
 * `mbe1_*` wrappers with macros, so once it is included those names take the 1.x arguments.
 * Code written for mbelib-neo includes `<mbelib-neo/mbelib.h>`; new code, and anything that
 * encodes or transcodes, includes `<mbevocoder/mbevocoder.h>`.
 *
 * Semantics follow mbelib 1.x:
 *  - Frame functions set `*errs` to the errors corrected in C0 and `*errs2` to the total.
 *  - Data functions read `*errs` / `*errs2` as the FEC stage's counts (they drive the repeat
 *    decision, as in mbelib 1.x).
 *  - `err_str` receives one `=` per error, then `E`, `T`, `R` or `M` for an erasure, tone,
 *    repeat or mute, NUL-terminated. Give it at least ::MBE_V1_ERR_STR_SIZE bytes (mbelib 1.x
 *    clients such as DSD use 64); it may be NULL.
 *  - `uvquality` is accepted and ignored: the unvoiced synthesis is mbelib-neo's FFT method,
 *    which has no quality setting.
 *
 * What is not provided: the mbelib 1.x ::mbe_parms fields `repeat` (now `repeatCount`) and
 * `un`; and the header name `<mbelib.h>` itself (add `<prefix>/include/mbelib` to the include
 * path to build unmodified sources that include it).
 */
#ifndef MBELIB_V1_MBELIB_H
#define MBELIB_V1_MBELIB_H

#include <mbelib-neo/mbelib.h>

/** Bytes the mbelib 1.x API writes at most into `err_str`, terminator included. */
#define MBE_V1_ERR_STR_SIZE 64

#ifdef __cplusplus
extern "C" {
#endif

/** @defgroup mbelib1 mbelib 1.x API
 *  The decoder functions in their mbelib 1.x form (see `<mbelib/mbelib.h>`). Call them by their
 *  mbelib 1.x names; the `mbe1_` names are what those map to.
 *  @{ */
MBE_API void mbe1_processAmbe2400Dataf(float* aout_buf, int* errs, int* errs2, char* err_str, char ambe_d[49], mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced, int uvquality);   /**< `mbe_processAmbe2400Dataf` in its mbelib 1.x form. */
MBE_API void mbe1_processAmbe2400Data(short* aout_buf, int* errs, int* errs2, char* err_str, char ambe_d[49], mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced, int uvquality);   /**< `mbe_processAmbe2400Data` in its mbelib 1.x form. */
MBE_API void mbe1_processAmbe3600x2400Framef(float* aout_buf, int* errs, int* errs2, char* err_str, char ambe_fr[4][24], char ambe_d[49], mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced, int uvquality);   /**< `mbe_processAmbe3600x2400Framef` in its mbelib 1.x form. */
MBE_API void mbe1_processAmbe3600x2400Frame(short* aout_buf, int* errs, int* errs2, char* err_str, char ambe_fr[4][24], char ambe_d[49], mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced, int uvquality);   /**< `mbe_processAmbe3600x2400Frame` in its mbelib 1.x form. */
MBE_API void mbe1_processAmbe2450Dataf(float* aout_buf, int* errs, int* errs2, char* err_str, char ambe_d[49], mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced, int uvquality);   /**< `mbe_processAmbe2450Dataf` in its mbelib 1.x form. */
MBE_API void mbe1_processAmbe2450Data(short* aout_buf, int* errs, int* errs2, char* err_str, char ambe_d[49], mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced, int uvquality);   /**< `mbe_processAmbe2450Data` in its mbelib 1.x form. */
MBE_API void mbe1_processAmbe3600x2450Framef(float* aout_buf, int* errs, int* errs2, char* err_str, char ambe_fr[4][24], char ambe_d[49], mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced, int uvquality);   /**< `mbe_processAmbe3600x2450Framef` in its mbelib 1.x form. */
MBE_API void mbe1_processAmbe3600x2450Frame(short* aout_buf, int* errs, int* errs2, char* err_str, char ambe_fr[4][24], char ambe_d[49], mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced, int uvquality);   /**< `mbe_processAmbe3600x2450Frame` in its mbelib 1.x form. */
MBE_API void mbe1_processImbe4400Dataf(float* aout_buf, int* errs, int* errs2, char* err_str, char imbe_d[88], mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced, int uvquality);   /**< `mbe_processImbe4400Dataf` in its mbelib 1.x form. */
MBE_API void mbe1_processImbe4400Data(short* aout_buf, int* errs, int* errs2, char* err_str, char imbe_d[88], mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced, int uvquality);   /**< `mbe_processImbe4400Data` in its mbelib 1.x form. */
MBE_API void mbe1_processImbe7200x4400Framef(float* aout_buf, int* errs, int* errs2, char* err_str, char imbe_fr[8][23], char imbe_d[88], mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced, int uvquality);   /**< `mbe_processImbe7200x4400Framef` in its mbelib 1.x form. */
MBE_API void mbe1_processImbe7200x4400Frame(short* aout_buf, int* errs, int* errs2, char* err_str, char imbe_fr[8][23], char imbe_d[88], mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced, int uvquality);   /**< `mbe_processImbe7200x4400Frame` in its mbelib 1.x form. */
MBE_API void mbe1_processImbe7100x4400Framef(float* aout_buf, int* errs, int* errs2, char* err_str, char imbe_fr[7][24], char imbe_d[88], mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced, int uvquality);   /**< `mbe_processImbe7100x4400Framef` in its mbelib 1.x form. */
MBE_API void mbe1_processImbe7100x4400Frame(short* aout_buf, int* errs, int* errs2, char* err_str, char imbe_fr[7][24], char imbe_d[88], mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced, int uvquality);   /**< `mbe_processImbe7100x4400Frame` in its mbelib 1.x form. */
MBE_API void mbe1_synthesizeSpeechf(float* aout_buf, mbe_parms* cur_mp, mbe_parms* prev_mp, int uvquality);   /**< `mbe_synthesizeSpeechf` in its mbelib 1.x form. */
MBE_API void mbe1_synthesizeSpeech(short* aout_buf, mbe_parms* cur_mp, mbe_parms* prev_mp, int uvquality);   /**< `mbe_synthesizeSpeech` in its mbelib 1.x form. */
/** Copy the library version ("3.0.0") into `str` (mbelib 1.x: at least 32 bytes). */
MBE_API void mbe_printVersion(char* str);
/** @} */

#ifdef __cplusplus
}
#endif

/* The mbelib 1.x names (described in the file comment; hidden from the API reference). */
/** @cond */
#define mbe_processAmbe2400Dataf        mbe1_processAmbe2400Dataf
#define mbe_processAmbe2400Data         mbe1_processAmbe2400Data
#define mbe_processAmbe3600x2400Framef  mbe1_processAmbe3600x2400Framef
#define mbe_processAmbe3600x2400Frame   mbe1_processAmbe3600x2400Frame
#define mbe_processAmbe2450Dataf        mbe1_processAmbe2450Dataf
#define mbe_processAmbe2450Data         mbe1_processAmbe2450Data
#define mbe_processAmbe3600x2450Framef  mbe1_processAmbe3600x2450Framef
#define mbe_processAmbe3600x2450Frame   mbe1_processAmbe3600x2450Frame
#define mbe_processImbe4400Dataf        mbe1_processImbe4400Dataf
#define mbe_processImbe4400Data         mbe1_processImbe4400Data
#define mbe_processImbe7200x4400Framef  mbe1_processImbe7200x4400Framef
#define mbe_processImbe7200x4400Frame   mbe1_processImbe7200x4400Frame
#define mbe_processImbe7100x4400Framef  mbe1_processImbe7100x4400Framef
#define mbe_processImbe7100x4400Frame   mbe1_processImbe7100x4400Frame
#define mbe_synthesizeSpeechf           mbe1_synthesizeSpeechf
#define mbe_synthesizeSpeech            mbe1_synthesizeSpeech
/** @endcond */

#endif /* MBELIB_V1_MBELIB_H */
