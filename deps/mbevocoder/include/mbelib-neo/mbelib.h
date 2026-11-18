/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file mbelib-neo/mbelib.h
 * @brief The mbelib-neo API on mbevocoder, source-compatible with mbelib-neo 2.6 (all but its
 *        TETRA ACELP decoder): drop-in for code written for mbelib-neo. All symbols carry the
 *        `mbe_` prefix.
 *
 * Decoding: keep one ::mbe_parms triplet per stream, call mbe_initMbeParms() once, then one
 * `mbe_process*` function per received frame; each call returns 160 samples of 8 kHz audio and
 * an ::mbe_process_result with error counts and flags. mbelib-neo 2.4's encoder functions are
 * here too (@ref neoencoder), on this library's encoders. The full encoder API with its options
 * and front end, the transcoder, tone frames and on-air layouts are in
 * `<mbevocoder/mbevocoder.h>`, which includes this header; the bit conventions and threading
 * rules are described there. `<mbelib/mbelib.h>` gives the same
 * decoder with its functions in their mbelib 1.x form, for code written against the original
 * mbelib.
 *
 * The decoder (synthesis, tone handling, erasure/mute logic) is the mbelib-neo 2.x decoder with
 * the AMBE-3000R-measured changes of `MBEVOCODER_CHIP_MEASURED` (see NOTICE.md).
 */
#ifndef MBELIB_NEO_MBELIB_H
#define MBELIB_NEO_MBELIB_H
#define MBELIB_NEO_PUBLIC_MBEBELIB_H   /**< mbelib-neo 2.6's include guard, for code that tests it. */

#include <mbelib-neo/version.h>

#include <stddef.h>
#include <stdint.h>

/* MBE_API: export/import decoration. Define `MBE_STATIC` when linking the static library on
 * Windows. A consumer may define it (and the deprecation macros) itself, as with mbelib-neo. */
#if !defined(MBE_API)
#  if (defined(_WIN32) || defined(__CYGWIN__)) && !defined(MBE_STATIC)
#    if defined(MBE_BUILDING)
#      define MBE_API __declspec(dllexport)
#    else
#      define MBE_API __declspec(dllimport)
#    endif
#  elif defined(__GNUC__) && __GNUC__ >= 4
#    define MBE_API __attribute__((visibility("default")))
#  else
#    define MBE_API
#  endif
#endif

/* MBE_DEPRECATED(msg) marks a declaration deprecated with a message where the compiler supports
 * it; MBE_DEPRECATED_FOR(sym) in favour of `sym`. */
#if !defined(MBE_DEPRECATED)
#  if defined(__GNUC__) || defined(__clang__)
#    define MBE_DEPRECATED(msg) __attribute__((deprecated(msg)))
#  elif defined(_MSC_VER)
#    define MBE_DEPRECATED(msg) __declspec(deprecated(msg))
#  else
#    define MBE_DEPRECATED(msg)
#  endif
#endif
#if !defined(MBE_DEPRECATED_FOR)
#  define MBE_DEPRECATED_FOR(sym) MBE_DEPRECATED("use " #sym " instead")
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================= */
/** @defgroup status Status codes, result reporting, soft bits
 *  mbelib-neo compatible. Functions return 0 or a non-negative count on success and one of
 *  the negative `MBE_STATUS_*` values on failure; a negative return means the outputs are
 *  unchanged or undefined and the stream state was not advanced.
 *  @{ */
/* ========================================================================= */

#define MBE_STATUS_INVALID_ARGUMENT (-1)   /**< NULL where a value is required, or an invalid enum / index. */
#define MBE_STATUS_INVALID_BITS     (-2)   /**< A bit array held a value other than 0 or 1. */

#define MBE_PROCESS_FLAG_SOFT_INPUT 0x0001u /**< The frame was decoded from soft bits. */
#define MBE_PROCESS_FLAG_C0_VALID   0x0002u /**< `c0_errors` is meaningful (a frame path was used). */
#define MBE_PROCESS_FLAG_C4_VALID   0x0004u /**< `c4_errors` is meaningful (IMBE frame path). */
#define MBE_PROCESS_FLAG_TONE       0x0010u /**< The frame was a tone frame and was rendered as one. */
#define MBE_PROCESS_FLAG_ERASURE    0x0020u /**< The frame was an erasure; comfort noise was output. */
#define MBE_PROCESS_FLAG_REPEAT     0x0040u /**< Too many errors: the previous frame's model was repeated. */
#define MBE_PROCESS_FLAG_MUTE       0x0080u /**< Repeats exhausted or error rate too high: output muted. */
#define MBE_PROCESS_FLAG_SILENCE    0x0100u /**< An AMBE+2 silence frame (fundamental index 124 or 125) was decoded and played; it does not become the prediction state. Not shown by mbe_formatProcessResult(). */
#define MBE_PROCESS_FLAG_C0_UNCORRECTABLE 0x0400u /**< Context (mbelib-neo 2.6.3): an AMBE frame decoder found a C0 word with four bit errors, which the extended Golay (24,12) code detects but cannot correct (`c0_errors` reads 4); passed on to the data path with the rest of the result. With `MBEVOCODER_CHIP_MEASURED` the frame plays as a lost frame and, on D-STAR, still becomes the prediction history, as on the AMBE-3000R. */
#define MBE_PROCESS_FLAG_PROVOICE   0x0200u /**< Context: the parameter bits come from an IMBE 7100x4400 (ProVoice) frame decode; set by the 7100x4400 decoders and passed on to the IMBE data path with the rest of the result. mbelib-neo plays a muted ProVoice frame as JMBE's comfort noise; mbevocoder mutes both IMBE rates alike. */

#define MBE_MAX_FRAME_REPEATS       4        /**< Repeats before a stream mutes (::MBE_PROCESS_FLAG_MUTE). */
#define MBE_MUTING_THRESHOLD_IMBE   0.0875f  /**< Smoothed error-rate threshold for muting, IMBE. */
#define MBE_MUTING_THRESHOLD_AMBE   0.096f   /**< Smoothed error-rate threshold for muting, AMBE. */

/** Per-frame outcome of the FEC and decode stages. SDRangel displays `c0_errors` and `total_errors`. */
typedef struct mbe_process_result {
    int c0_errors;          /**< Errors corrected in the C0 (most protected) block. With `MBEVOCODER_CHIP_MEASURED` (default): an AMBE C0 word the extended Golay code detects as uncorrectable reads 4 (and the frame is repeated), and the AMBE and IMBE 7200 frame decoders' counts include the Golay parity bits (AMBE: full weight, D-STAR's C1 parity bit included, as mbelib-neo 2.6.3+). */
    int protected_errors;   /**< Errors corrected in the remaining protected blocks. */
    int c4_errors;          /**< IMBE only: errors corrected in the C4 Hamming block. */
    int total_errors;       /**< `c0_errors + protected_errors`. */
    unsigned flags;         /**< Combination of `MBE_PROCESS_FLAG_*`. */
} mbe_process_result;

/** A received bit with a confidence, for the `*Soft*` decode paths. */
typedef struct mbe_soft_bit {
    uint8_t bit;            /**< Hard decision, 0 or 1. */
    uint8_t reliability;    /**< 0 = no confidence .. 255 = certain. */
} mbe_soft_bit;

/** @return The library version as "major.minor.patch" (also ::MBELIB_VERSION). */
MBE_API const char* mbe_versionString(void);

/** Zero a result. The data-path `mbe_process*Data*` functions read the caller's result (the
 *  error counts and flags of a FEC stage run elsewhere), so initialise it, or pass NULL, before
 *  calling them with a result you did not get from a frame-path call. */
MBE_API void mbe_initProcessResult(mbe_process_result* result);
/** Render the legacy error bar into `str`: one `=` per corrected error, then `E`, `T`, `R`, `M`
 *  markers for the erasure, tone, repeat and mute flags. Always NUL-terminated when `size > 0`. */
MBE_API void mbe_formatProcessResult(char* str, size_t size, const mbe_process_result* result);

/** Make a soft bit from a hard decision and a reliability. */
MBE_API mbe_soft_bit mbe_softBitFromHard(int bit, uint8_t reliability);
/** Make a soft bit from a log-likelihood ratio: negative LLR means bit 1, magnitude (clamped
 *  to 255) is the reliability. */
MBE_API mbe_soft_bit mbe_softBitFromLlr(int16_t llr);
/** Convert `count` hard bits (0/1) to soft bits with one reliability. @return 0 or a status. */
MBE_API int mbe_softBitsFromHard(const char* bits, mbe_soft_bit* soft, size_t count, uint8_t reliability);
/** Convert `count` LLRs to soft bits (see mbe_softBitFromLlr()). @return 0 or a status. */
MBE_API int mbe_softBitsFromLlr(const int16_t* llr, mbe_soft_bit* soft, size_t count);

/** @} */

/* ========================================================================= */
/** @defgroup ecc Error correction
 *  Codeword bit arrays are LSB at index 0 (`in[22]` is the first Golay bit). The mbelib
 *  convention is kept: only the data bits are corrected and counted, parity bits are copied
 *  through. The soft variants search the nearest codeword by reliability-weighted distance
 *  and return the number of data bits that differ from the hard decision. Exception: with
 *  `MBEVOCODER_CHIP_MEASURED` (the default) the IMBE 7200x4400 frame decoders
 *  (mbe_decodeImbe7200x4400Frame() and its soft variant, and so the process functions) count
 *  every corrected Golay bit, parity included, as TIA-102.BABA's repeat and mute rules do; the
 *  functions in this group keep mbelib's count.
 *  @{ */
/* ========================================================================= */

/** Golay(23,12) on a 23-bit codeword in `*block`; replaced by its corrected 12 data bits. @return 0 or a status. */
MBE_API int mbe_checkGolayBlock(long int* block);
/** Golay(23,12): `in[22..0]` -> `out[22..0]`. @return Corrected data-bit count (0..3) or a status. */
MBE_API int mbe_golay2312(const char* in, char* out);
/** Soft-decision Golay(23,12). @return Data bits changed against the hard decision, or a status. */
MBE_API int mbe_golay2312Soft(const mbe_soft_bit* in, char* out);
/** Hamming(15,11), standard layout (data in `[14..4]`): `in[14..0]` -> `out[14..0]`. @return Corrected bits (0..1) or a status. */
MBE_API int mbe_hamming1511(const char* in, char* out);
/** Soft-decision Hamming(15,11). */
MBE_API int mbe_hamming1511Soft(const mbe_soft_bit* in, char* out);
/** Hamming(15,11) with the IMBE 7100x4400 (ProVoice) generator. */
MBE_API int mbe_7100x4400hamming1511(const char* in, char* out);
/** Soft-decision variant of mbe_7100x4400hamming1511(). */
MBE_API int mbe_7100x4400hamming1511Soft(const mbe_soft_bit* in, char* out);

/** @} */

/* ========================================================================= */
/** @defgroup frames Frame level: ECC, descrambling and parameter-bit extraction per rate
 *  The `mbe_decode*Frame` functions do the whole job (C0 Golay, descramble, remaining ECC,
 *  extract the parameter bits) and fill an ::mbe_process_result; they return the total
 *  corrected error count or a status. The `mbe_ecc*`, `mbe_demodulate*` functions are the
 *  mbelib-neo stages they are built from, kept for callers that use them directly. The
 *  `mbe_dump*` functions print to stderr for debugging.
 *  @{ */
/* ========================================================================= */

/** @name AMBE 3600x2400 (D-STAR) */
/** @{ */
MBE_API void mbe_dumpAmbe2400Data(const char* ambe_d);   /**< Print the parameter bits to stderr. */
MBE_API void mbe_dumpAmbe3600x2400Frame(const char ambe_fr[4][24]);   /**< Print the frame bitplanes to stderr. */
/** Correct the C0 Golay(24,12) block in place. @return Errors corrected or a status. */
MBE_API int  mbe_eccAmbe3600x2400C0(char ambe_fr[4][24]);
/** Correct C1 and extract the 49 parameter bits (after mbe_eccAmbe3600x2400C0() and
 *  mbe_demodulateAmbe3600x2400Data()). @return Errors corrected in C1 or a status. */
MBE_API int  mbe_eccAmbe3600x2400Data(char ambe_fr[4][24], char* ambe_d);
/** Descramble C1 with the PN sequence seeded by C0 (in place). @return 0 or a status. */
MBE_API int  mbe_demodulateAmbe3600x2400Data(char ambe_fr[4][24]);
/** Full frame decode: FEC, descramble, extract `ambe_d`. `result` may be NULL. @return Total corrected errors or a status. */
MBE_API int  mbe_decodeAmbe3600x2400Frame(const char ambe_fr[4][24], char ambe_d[49], mbe_process_result* result);
/** Soft-decision variant of mbe_decodeAmbe3600x2400Frame(). */
MBE_API int  mbe_decodeAmbe3600x2400SoftFrame(const mbe_soft_bit ambe_fr[4][24], char ambe_d[49], mbe_process_result* result);
/** A frame as the 9 data bytes of a D-STAR DV frame (the sync word not included): the D-STAR
 *  on-air bit order, LSB first within each byte, as the GMSK modulator sends it (as
 *  mbe_ambeFrameToBits() with ::MBE_LAYOUT_DSTAR and mbe_packBits() LSB first). @return 0 or a status. */
MBE_API int  mbe_encodeDStarDVData(const char ambe_fr[4][24], unsigned char bytes9[9]);
/** Inverse of mbe_encodeDStarDVData(). @return 0 or a status. */
MBE_API int  mbe_decodeDStarDVData(const unsigned char bytes9[9], char ambe_fr[4][24]);
/** @} */

/** @name AMBE 3600x2450 (AMBE+2: DMR, P25 Phase 2, YSF DN, NXDN, dPMR)
 *  The frame structure is identical to 3600x2400; only the parameter bits mean different things. */
/** @{ */
MBE_API void mbe_dumpAmbe2450Data(const char* ambe_d);   /**< Print the parameter bits to stderr. */
MBE_API void mbe_dumpAmbe3600x2450Frame(const char ambe_fr[4][24]);   /**< Print the frame bitplanes to stderr. */
MBE_API int  mbe_eccAmbe3600x2450C0(char ambe_fr[4][24]);   /**< As mbe_eccAmbe3600x2400C0(), but with `MBEVOCODER_CHIP_MEASURED` (default) the count is full-weight, as in mbelib-neo 2.6 (TIA-102.BABA-1 5.5): every received bit, parity included, that differs from the corrected word; 4 when the word is detected uncorrectable. */
MBE_API int  mbe_eccAmbe3600x2450Data(char ambe_fr[4][24], char* ambe_d);   /**< As mbe_eccAmbe3600x2400Data(), but with `MBEVOCODER_CHIP_MEASURED` (default) the count is full-weight: every bit of the descrambled C1 word that differs from the corrected one. */
MBE_API int  mbe_demodulateAmbe3600x2450Data(char ambe_fr[4][24]);   /**< As mbe_demodulateAmbe3600x2400Data(). */
MBE_API int  mbe_decodeAmbe3600x2450Frame(const char ambe_fr[4][24], char ambe_d[49], mbe_process_result* result);   /**< As mbe_decodeAmbe3600x2400Frame(). */
MBE_API int  mbe_decodeAmbe3600x2450SoftFrame(const mbe_soft_bit ambe_fr[4][24], char ambe_d[49], mbe_process_result* result);   /**< As mbe_decodeAmbe3600x2400SoftFrame(). */
#define MBE_AMBE2450_FRAME_VOICE   0   /**< Voice frame: the only type that becomes the prediction state. */
#define MBE_AMBE2450_FRAME_SILENCE 1   /**< Silence frame (fundamental index 124 or 125): decoded and played, never the prediction state. */
#define MBE_AMBE2450_FRAME_ERASURE 2   /**< Erasure (index 120-123 or 126-127, or a tone frame whose index is unusable or whose redundant fields disagree): a repeat. */
#define MBE_AMBE2450_FRAME_TONE    7   /**< Tone frame with a usable tone index (255 is the silent tone). */
/** The type of an AMBE+2 frame from its parameter bits alone (TIA-102.BABA-1 4.1 and 7): a tone
 *  frame (u0's first six bits all 1) is recognised before the fundamental is read.
 *  @return An `MBE_AMBE2450_FRAME_*` value or a status. */
MBE_API int  mbe_classifyAmbe2450Frame(const char ambe_d[49]);
/** @} */

/** @name IMBE 7200x4400 (P25 Phase 1, YSF VW, NXDN full rate) */
/** @{ */
MBE_API void mbe_dumpImbe4400Data(const char* imbe_d);   /**< Print the parameter bits to stderr. */
MBE_API void mbe_dumpImbe7200x4400Data(const char* imbe_d);   /**< Print the parameter bits to stderr. */
MBE_API void mbe_dumpImbe7200x4400Frame(const char imbe_fr[8][23]);   /**< Print the frame to stderr. */
/** Correct the C0 Golay block in place. @return Errors corrected or a status. */
MBE_API int  mbe_eccImbe7200x4400C0(char imbe_fr[8][23]);
/** Correct the remaining Golay and Hamming blocks and extract the 88 parameter bits. @return Errors corrected or a status. */
MBE_API int  mbe_eccImbe7200x4400Data(char imbe_fr[8][23], char* imbe_d);
/** Descramble rows 1..6 with the PN sequence seeded by C0 (in place). @return 0 or a status. */
MBE_API int  mbe_demodulateImbe7200x4400Data(char imbe[8][23]);
/** Full frame decode. `result` may be NULL; `c4_errors` is set from the C4 Hamming block. @return Total corrected errors or a status. */
MBE_API int  mbe_decodeImbe7200x4400Frame(const char imbe_fr[8][23], char imbe_d[88], mbe_process_result* result);
/** Soft-decision variant of mbe_decodeImbe7200x4400Frame(). */
MBE_API int  mbe_decodeImbe7200x4400SoftFrame(const mbe_soft_bit imbe_fr[8][23], char imbe_d[88], mbe_process_result* result);
/** @} */

/** @name IMBE 7100x4400 (ProVoice)
 *  Decoded parameter bits are converted to the 7200x4400 layout, so the same
 *  mbe_decodeImbe4400Parms() / mbe_processImbe4400Data*() consume them. */
/** @{ */
MBE_API void mbe_dumpImbe7100x4400Data(const char* imbe_d);   /**< Print the parameter bits to stderr. */
MBE_API void mbe_dumpImbe7100x4400Frame(const char imbe_fr[7][24]);   /**< Print the frame to stderr. */
MBE_API int  mbe_eccImbe7100x4400C0(char imbe_fr[7][24]);   /**< Correct the C0 Golay block in place. */
MBE_API int  mbe_eccImbe7100x4400Data(char imbe_fr[7][24], char* imbe_d);   /**< Correct the remaining blocks and extract the 88 parameter bits (7100 order). */
MBE_API int  mbe_demodulateImbe7100x4400Data(char imbe[7][24]);   /**< Descramble in place. */
/** Reorder 7100x4400 parameter bits into the 7200x4400 order, in place. @return 0 or a status. */
MBE_API int  mbe_convertImbe7100to7200(char* imbe_d);
MBE_API int  mbe_decodeImbe7100x4400Frame(const char imbe_fr[7][24], char imbe_d[88], mbe_process_result* result);   /**< Full frame decode; `imbe_d` comes out in the 7200x4400 order. */
MBE_API int  mbe_decodeImbe7100x4400SoftFrame(const mbe_soft_bit imbe_fr[7][24], char imbe_d[88], mbe_process_result* result);   /**< Soft-decision variant of mbe_decodeImbe7100x4400Frame(). */
/** @} */

/** @name Frame assembly (in mbelib-neo from 2.4)
 *  Parameter bits -> FEC-protected, scrambled frame: the inverse of `mbe_decode*Frame` for an
 *  error-free frame. For 3600x2400, `ambe_d[24]` is the C1 Golay(24,12) parity position; it is
 *  computed here and the input value ignored. @return 0 or a status. */
/** @{ */
MBE_API int mbe_encodeAmbe3600x2400Frame(const char ambe_d[49], char ambe_fr[4][24]);   /**< D-STAR parameter bits -> frame. */
MBE_API int mbe_encodeAmbe3600x2450Frame(const char ambe_d[49], char ambe_fr[4][24]);   /**< AMBE+2 parameter bits -> frame. */
MBE_API int mbe_encodeImbe7200x4400Frame(const char imbe_d[88], char imbe_fr[8][23]);   /**< IMBE parameter bits -> frame. */
/** @} */

/** @} */

/* ========================================================================= */
/** @defgroup decoder Decoder state and synthesis
 *  mbelib-neo 2.x compatible. One ::mbe_parms triplet (current, previous, previous enhanced)
 *  per stream, initialised with mbe_initMbeParms() and never shared between threads. Each
 *  `mbe_process*` call consumes one frame and produces 160 samples at 8 kHz. The float
 *  variants are on the 16-bit range divided by 7: mbe_floattoshort() multiplies by 7 and clips
 *  at 95 % of full scale. Speech and tone frames decode at the AMBE-3000R's level, which is
 *  DVSI's decoder's (as mbelib-neo 2.3+); with `MBEVOCODER_CHIP_MEASURED` off, speech keeps
 *  mbelib-neo 2.1's level, about 16 dB louder.
 *  @{ */
/* ========================================================================= */

/** The MBE model of one frame plus the synthesis state carried between frames. Public for
 *  compatibility with mbelib; callers normally only pass triplets around. `Vl`, `Ml`, `log2Ml`,
 *  `PHIl`, `PSIl` are indexed 1..L.
 *
 *  Source-compatible with mbelib-neo 2.x, not binary-compatible: mbevocoder appends the mixed
 *  excitation, high-pass and silence-frame fields, so the struct is larger. Compile against this
 *  header; a binary built against mbelib-neo's (or any other size of) `mbe_parms` and linked to
 *  this library would have its struct overrun by mbe_initMbeParms() and the decoders. */
struct mbe_parameters {
    float w0;               /**< Fundamental, radians per sample. */
    int L;                  /**< Number of harmonics, 9..56. */
    int K;                  /**< IMBE: number of voicing bands. */
    int Vl[57];             /**< Voicing per harmonic, 1 = voiced. */
    float Ml[57];           /**< Linear harmonic amplitudes as synthesised. */
    float log2Ml[57];       /**< log2 amplitudes before the unvoiced scale, the prediction domain. */
    float PHIl[57];         /**< Harmonic phases. */
    float PSIl[57];         /**< Regenerated phase state. */
    float gamma;            /**< AMBE gain (log2), predicted from the previous frame. */
    uint32_t tonePhase;     /**< Tone synthesis phase accumulator. */
    int swn;                /**< Synthesis window / tone phase state. */

    /* === Adaptive smoothing state (Algorithms #111-116) === */
    float localEnergy;      /**< IIR-smoothed frame energy (algorithm #111). */
    int amplitudeThreshold; /**< Amplitude limit under errors (algorithm #115). */
    float errorRate;        /**< Smoothed bit error rate driving repeat/mute decisions. */
    int errorCountTotal;    /**< Corrected errors in the current frame. */
    int errorCount4;        /**< IMBE: errors in the C4 block of the current frame. */

    /* === Frame repeat/muting state === */
    int repeatCount;        /**< Consecutive repeated frames; muting at ::MBE_MAX_FRAME_REPEATS. */
    float mutingThreshold;  /**< ::MBE_MUTING_THRESHOLD_IMBE or ::MBE_MUTING_THRESHOLD_AMBE. */

    /* === FFT-based unvoiced synthesis state === */
    float previousUw[256];  /**< Overlap-add tail of the unvoiced synthesis. */
    float noiseSeed;        /**< Unvoiced noise generator state. */
    float noiseOverlap[96]; /**< Unvoiced noise overlap buffer. */

    /* === Mixed excitation (mbevocoder; docs/decoder-quality.md) === */
    float Vmix[57];         /**< Voiced energy fraction per harmonic, 0..1; `Vl` is `Vmix >= 0.5`. Valid when `vmixValid`. */
    float unvcScale;        /**< The unvoiced amplitude scale folded into `Ml` of unvoiced harmonics (1 for IMBE). */
    int vmixValid;          /**< Non-zero when `Vmix` describes this frame (AMBE decoders); otherwise `Vl` alone does. */
    float hpfState[4];      /**< Output high-pass state (MBEVOCODER_CHIP_MEASURED): x[n-1], x[n-2], y[n-1], y[n-2]. */
    int silenceFrame;       /**< Non-zero when the frame is an AMBE+2 silence frame (fundamental index 124/125, the encoder's DTX output): rendered as comfort noise predicted from the last voice frame, which stays the prediction state. */
    uint64_t lastTone;      /**< AMBE+2 (MBEVOCODER_CHIP_MEASURED): the parameter bits of the tone frame last played (bit i = `ambe_d[i]`), 0 after a voice frame; a repeat replays that tone, as the AMBE-3000R does. */
    int dstarTone;          /**< D-STAR (MBEVOCODER_CHIP_MEASURED): the tone the last frame left sounding, index | volume << 8 | 1 << 16 (| 1 << 17 when the error-rate mute silenced it), 0 for none; the next frame crossfades from it, and lost frames continue (dual tones) or end (single tones) it, as on the AMBE-3000R. */
    int historyL;           /**< D-STAR (MBEVOCODER_CHIP_MEASURED): the harmonic count of a prediction history other than the model in this state, 0 for none: a voice frame whose C0 word was uncorrectable plays as a lost frame (the previous model repeated) while the next frame predicts from its decoded amplitudes, as on the AMBE-3000R. */
    float historyLog2Ml[57];/**< D-STAR: the log2 amplitudes of that prediction history (`historyL` > 0). */
    float chipErrorRate;    /**< D-STAR (MBEVOCODER_CHIP_MEASURED): the AMBE-3000R's error-rate measure, r = 0.99 r + 0.001064 e over each frame's full-weight error count e (received C0 and C1 bits that differ from the corrected frame); the output mutes while r > 0.366. */
};

/** Alias for ::mbe_parameters, the name the API uses. */
typedef struct mbe_parameters mbe_parms;

/** Seed this thread's comfort-noise and unvoiced-noise generators (0 selects a fixed non-zero seed). */
MBE_API void mbe_setThreadRngSeed(uint32_t seed);
/** Release this thread's synthesis resources (an FFT plan of about 12 KB, allocated on the thread's
 *  first decode). Call it on a decoder thread before the thread ends; without it each thread that
 *  ever decoded leaves its plan behind. Safe to call at any time; a later decode on the thread
 *  allocates a new plan. */
MBE_API void mbe_freeThreadResources(void);
/** Initialise a stream's triplet. Call once per stream and again to restart it. */
MBE_API void mbe_initMbeParms(mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced);
/** Copy a parameter set (`*destination_mp = *source_mp`). */
MBE_API void mbe_moveMbeParms(const mbe_parms* source_mp, mbe_parms* destination_mp);
/** Repeat the previous frame's model into `cur_mp` (frame repeat on errors). */
MBE_API void mbe_useLastMbeParms(mbe_parms* cur_mp, const mbe_parms* prev_mp);
/** Spectral amplitude enhancement (IMBE algorithm #105), in place on `Ml`. */
MBE_API void mbe_spectralAmpEnhance(mbe_parms* cur_mp);
/** @return Non-zero when the error rate exceeds the stream's muting threshold. */
MBE_API int mbe_requiresMuting(const mbe_parms* mp);
/** @return Non-zero when `repeatCount` has reached ::MBE_MAX_FRAME_REPEATS. */
MBE_API int mbe_isMaxFrameRepeat(const mbe_parms* mp);
/** @return Non-zero when adaptive smoothing would change this frame. */
MBE_API int mbe_requiresAdaptiveSmoothing(const mbe_parms* mp);
/** Adaptive smoothing (algorithms #111-116): voicing and amplitude limiting under errors. */
MBE_API void mbe_applyAdaptiveSmoothing(mbe_parms* cur_mp, const mbe_parms* prev_mp);
/** Synthesise 160 samples from `cur_mp`, interpolating from `prev_mp` (the enhanced previous set). */
MBE_API void mbe_synthesizeSpeechf(float* aout_buf, mbe_parms* cur_mp, mbe_parms* prev_mp);
/** 16-bit variant of mbe_synthesizeSpeechf(). */
MBE_API void mbe_synthesizeSpeech(short* aout_buf, mbe_parms* cur_mp, mbe_parms* prev_mp);
MBE_API void mbe_synthesizeSilencef(float* aout_buf);        /**< 160 zero samples. */
MBE_API void mbe_synthesizeSilence(short* aout_buf);         /**< 16-bit variant. */
MBE_API void mbe_synthesizeComfortNoisef(float* aout_buf);   /**< 160 samples of low-level noise (erasures, mute). */
MBE_API void mbe_synthesizeComfortNoise(short* aout_buf);    /**< 16-bit variant. */
/** A frame the framing layer knows was not received (a missed slot, a failed sync): the previous frame's model is
 *  repeated three times, the fourth missing frame mutes (::MBE_MAX_FRAME_REPEATS) and the mute holds until a frame
 *  decodes (the AMBE-3000R's LOST_FRAME behaviour: a fade to silence on the fourth, the next decoded frame at its
 *  proper level; `MBEVOCODER_CHIP_MEASURED` off gives mbelib's comfort noise and state reset). Call it with the stream's
 *  triplet in place of the process function for the missing frame; `result` gets ::MBE_PROCESS_FLAG_REPEAT and,
 *  once muted, ::MBE_PROCESS_FLAG_MUTE. Do not feed the last frame's bits again instead: that reruns the gain
 *  prediction on them and the level climbs 2-3 dB per frame. Any rate. @return 0 or a status. */
MBE_API int mbe_processLostFramef(float* aout_buf, mbe_process_result* result, mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced);
MBE_API int mbe_processLostFrame(short* aout_buf, mbe_process_result* result, mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced);   /**< 16-bit variant. */
/** Comfort noise for a DTX gap (docs/frontend.md): 160 samples continuing the background the last frame
 *  described, for the 20 ms periods in which no frame arrives because the sender's DTX withheld it. The
 *  last rendered frame is repeated with every band unvoiced at its level (what an AMBE+2 silence frame or
 *  a D-STAR index-120 frame renders); the synthesis state moves on, the prediction state does not. Call it
 *  with the stream's triplet in place of the process function for the missing frame. */
MBE_API void mbe_synthesizeComfortNoiseFramef(float* aout_buf, mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced);
MBE_API void mbe_synthesizeComfortNoiseFrame(short* aout_buf, mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced);   /**< 16-bit variant. */
/** Render an AMBE+2 tone frame (`ambe_d` holds a tone frame, see @ref tones) into 160 samples.
 *  Amplitude follows the AMBE-3000R's law (part of `MBEVOCODER_CHIP_MEASURED`, default on): 0.71 dB per step
 *  of the 7-bit field, full scale at 127. */
MBE_API void mbe_synthesizeTonef(float* aout_buf, const char* ambe_d, mbe_parms* cur_mp);
/** Render a D-STAR tone frame. `ID1` is the D-STAR tone index (single tones 5..122, dual tones
 *  128..147 in D-STAR numbering); the volume is read from `ambe_d`. */
MBE_API void mbe_synthesizeTonefdstar(float* aout_buf, const char* ambe_d, mbe_parms* cur_mp, int ID1);
/** Convert 160 float samples to 16-bit: multiply by 7, clip at 95 % of full scale. */
MBE_API void mbe_floattoshort(const float* float_buf, short* aout_buf);

/** @name Parameter decode without synthesis
 *  Decode parameter bits into `cur_mp` using `prev_mp` for prediction. The caller advances the
 *  state afterwards with `mbe_moveMbeParms(cur_mp, prev_mp)`, as the `mbe_process*` functions
 *  do. Returns 0 for a voice frame; positive values flag special frames (AMBE+2: 2 erasure,
 *  7 tone; D-STAR: 5..122 single tone index, 3 dual tone or silence; IMBE: 1 invalid) and
 *  negative values are statuses. */
/** @{ */
MBE_API int mbe_decodeAmbe2400Parms(const char* ambe_d, mbe_parms* cur_mp, mbe_parms* prev_mp);   /**< D-STAR parameter bits -> model. */
MBE_API int mbe_decodeAmbe2450Parms(const char* ambe_d, mbe_parms* cur_mp, mbe_parms* prev_mp);   /**< AMBE+2 parameter bits -> model. */
MBE_API int mbe_decodeImbe4400Parms(const char* imbe_d, mbe_parms* cur_mp, mbe_parms* prev_mp);   /**< IMBE parameter bits (7200x4400 order) -> model. */
/** @} */

/** @name Data paths: parameter bits -> 160 samples
 *  For callers that ran the FEC themselves (SDRangel's YSF V/D 2 and VW). `result` carries the
 *  FEC stage's error counts and flags *in* (they drive repeat and mute decisions) and the
 *  synthesis flags *out*; initialise it with mbe_initProcessResult() or pass NULL. The
 *  return value is the total error count (from `result`) or a status. */
/** @{ */
MBE_API int mbe_processAmbe2400Dataf(float* aout_buf, mbe_process_result* result, const char ambe_d[49], mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced);   /**< D-STAR parameter bits -> 160 float samples. */
MBE_API int mbe_processAmbe2400Data(short* aout_buf, mbe_process_result* result, const char ambe_d[49], mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced);   /**< 16-bit output variant. */
MBE_API int mbe_processAmbe2450Dataf(float* aout_buf, mbe_process_result* result, const char ambe_d[49], mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced);   /**< AMBE+2 parameter bits -> 160 float samples. */
MBE_API int mbe_processAmbe2450Data(short* aout_buf, mbe_process_result* result, const char ambe_d[49], mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced);   /**< 16-bit output variant. */
MBE_API int mbe_processImbe4400Dataf(float* aout_buf, mbe_process_result* result, const char imbe_d[88], mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced);   /**< IMBE parameter bits -> 160 float samples. */
MBE_API int mbe_processImbe4400Data(short* aout_buf, mbe_process_result* result, const char imbe_d[88], mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced);   /**< 16-bit output variant. */
/** @} */

/** @name Frame paths: FEC-protected frame -> 160 samples
 *  The usual entry points: FEC, parameter decode and synthesis in one call. `ambe_d` /
 *  `imbe_d` receive the parameter bits; `result` (may be NULL) receives error counts and flags.
 *  The return value is the total corrected error count or a status. */
/** @{ */
MBE_API int mbe_processAmbe3600x2400Framef(float* aout_buf, mbe_process_result* result, const char ambe_fr[4][24], char ambe_d[49], mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced);   /**< D-STAR frame -> 160 float samples. */
MBE_API int mbe_processAmbe3600x2400Frame(short* aout_buf, mbe_process_result* result, const char ambe_fr[4][24], char ambe_d[49], mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced);   /**< 16-bit output variant. */
MBE_API int mbe_processAmbe3600x2400SoftFramef(float* aout_buf, mbe_process_result* result, const mbe_soft_bit ambe_fr[4][24], char ambe_d[49], mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced);   /**< D-STAR soft-bit frame -> 160 float samples. */
MBE_API int mbe_processAmbe3600x2400SoftFrame(short* aout_buf, mbe_process_result* result, const mbe_soft_bit ambe_fr[4][24], char ambe_d[49], mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced);   /**< 16-bit output variant. */
MBE_API int mbe_processAmbe3600x2450Framef(float* aout_buf, mbe_process_result* result, const char ambe_fr[4][24], char ambe_d[49], mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced);   /**< AMBE+2 frame -> 160 float samples. */
MBE_API int mbe_processAmbe3600x2450Frame(short* aout_buf, mbe_process_result* result, const char ambe_fr[4][24], char ambe_d[49], mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced);   /**< 16-bit output variant. */
MBE_API int mbe_processAmbe3600x2450SoftFramef(float* aout_buf, mbe_process_result* result, const mbe_soft_bit ambe_fr[4][24], char ambe_d[49], mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced);   /**< AMBE+2 soft-bit frame -> 160 float samples. */
MBE_API int mbe_processAmbe3600x2450SoftFrame(short* aout_buf, mbe_process_result* result, const mbe_soft_bit ambe_fr[4][24], char ambe_d[49], mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced);   /**< 16-bit output variant. */
MBE_API int mbe_processImbe7200x4400Framef(float* aout_buf, mbe_process_result* result, const char imbe_fr[8][23], char imbe_d[88], mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced);   /**< IMBE 7200x4400 frame -> 160 float samples. */
MBE_API int mbe_processImbe7200x4400Frame(short* aout_buf, mbe_process_result* result, const char imbe_fr[8][23], char imbe_d[88], mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced);   /**< 16-bit output variant. */
MBE_API int mbe_processImbe7200x4400SoftFramef(float* aout_buf, mbe_process_result* result, const mbe_soft_bit imbe_fr[8][23], char imbe_d[88], mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced);   /**< IMBE 7200x4400 soft-bit frame -> 160 float samples. */
MBE_API int mbe_processImbe7200x4400SoftFrame(short* aout_buf, mbe_process_result* result, const mbe_soft_bit imbe_fr[8][23], char imbe_d[88], mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced);   /**< 16-bit output variant. */
MBE_API int mbe_processImbe7100x4400Framef(float* aout_buf, mbe_process_result* result, const char imbe_fr[7][24], char imbe_d[88], mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced);   /**< IMBE 7100x4400 (ProVoice) frame -> 160 float samples. */
MBE_API int mbe_processImbe7100x4400Frame(short* aout_buf, mbe_process_result* result, const char imbe_fr[7][24], char imbe_d[88], mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced);   /**< 16-bit output variant. */
MBE_API int mbe_processImbe7100x4400SoftFramef(float* aout_buf, mbe_process_result* result, const mbe_soft_bit imbe_fr[7][24], char imbe_d[88], mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced);   /**< IMBE 7100x4400 soft-bit frame -> 160 float samples. */
MBE_API int mbe_processImbe7100x4400SoftFrame(short* aout_buf, mbe_process_result* result, const mbe_soft_bit imbe_fr[7][24], char imbe_d[88], mbe_parms* cur_mp, mbe_parms* prev_mp, mbe_parms* prev_mp_enhanced);   /**< 16-bit output variant. */
/** @} */

/** @} */

/* ========================================================================= */
/** @defgroup neoencoder Encoders, mbelib-neo's API
 *  mbelib-neo 2.4's encoder functions, on this library's encoders (the full encoder API, with
 *  options, on-air layouts and the front end, is ::mbe_encoder in `<mbevocoder/mbevocoder.h>`).
 *  One context per stream, 160 samples of 8 kHz audio per call: float on mbelib-neo's scale
 *  (nominal +-1) or 16-bit. Initialise with the `Alloc` function and mbe_initMbeParms(), and
 *  after every frame call `mbe_moveMbeParms(cur_mp, prev_mp)`; `cur_mp` receives the model the
 *  decoder decodes from the emitted bits given `prev_mp`, which is only read. Tone frames are sent
 *  as the AMBE-3000R sends them; a tone frame leaves the prediction history alone, so `cur_mp` is
 *  then a copy of `prev_mp`. Like mbelib-neo's encoders these never send silence or erasure frames.
 *  Unlike mbelib-neo's (about 10 ms), the parameters lag the audio by mbe_encoderDelaySamples():
 *  189 samples, 239 for D-STAR, so two frames of zeros flush the tail. A frame with a non-finite
 *  sample or one beyond +-2^20, or a `prev_mp` whose levels are not finite or beyond +-2^20 (or a
 *  log2 amplitude above 120), is ::MBE_STATUS_INVALID_ARGUMENT, and so are NULL pointers and
 *  `cur_mp == prev_mp`. To restart a stream, call the `Reset` function and mbe_initMbeParms().
 *  @{ */
/* ========================================================================= */

typedef struct mbe_ambe2400_encoder mbe_ambe2400_encoder;   /**< D-STAR (AMBE 3600x2400) encoder context. */
typedef struct mbe_ambe2450_encoder mbe_ambe2450_encoder;   /**< AMBE+2 (2450 parameter bits) encoder context. */
typedef struct mbe_imbe4400_encoder mbe_imbe4400_encoder;   /**< IMBE (4400 parameter bits) encoder context. */

MBE_API mbe_ambe2400_encoder* mbe_ambe2400EncoderAlloc(void);   /**< @return A new context, or NULL if memory is exhausted. */
MBE_API void mbe_ambe2400EncoderReset(mbe_ambe2400_encoder* enc);   /**< Restore the initial state (NULL is ignored). */
MBE_API void mbe_ambe2400EncoderFree(mbe_ambe2400_encoder* enc);    /**< Free a context (NULL is ignored). */
/** Encode one frame to D-STAR parameter bits. @return 0 or a status. */
MBE_API int mbe_encodeAmbe2400Parms(mbe_ambe2400_encoder* enc, const float* samples, char ambe_d[49], mbe_parms* cur_mp, const mbe_parms* prev_mp);
MBE_API int mbe_encodeAmbe2400ParmsShort(mbe_ambe2400_encoder* enc, const short* samples, char ambe_d[49], mbe_parms* cur_mp, const mbe_parms* prev_mp);   /**< 16-bit input variant. */

MBE_API mbe_ambe2450_encoder* mbe_ambe2450EncoderAlloc(void);   /**< @return A new context, or NULL if memory is exhausted. */
MBE_API void mbe_ambe2450EncoderReset(mbe_ambe2450_encoder* enc);   /**< Restore the initial state (NULL is ignored). */
MBE_API void mbe_ambe2450EncoderFree(mbe_ambe2450_encoder* enc);    /**< Free a context (NULL is ignored). */
/** Encode one frame to AMBE+2 parameter bits. @return ::MBE_AMBE2450_FRAME_VOICE, ::MBE_AMBE2450_FRAME_TONE or a status. */
MBE_API int mbe_encodeAmbe2450Parms(mbe_ambe2450_encoder* enc, const float* samples, char ambe_d[49], mbe_parms* cur_mp, const mbe_parms* prev_mp);
MBE_API int mbe_encodeAmbe2450ParmsShort(mbe_ambe2450_encoder* enc, const short* samples, char ambe_d[49], mbe_parms* cur_mp, const mbe_parms* prev_mp);   /**< 16-bit input variant. */

MBE_API mbe_imbe4400_encoder* mbe_imbe4400EncoderAlloc(void);   /**< @return A new context, or NULL if memory is exhausted. */
MBE_API void mbe_imbe4400EncoderReset(mbe_imbe4400_encoder* enc);   /**< Restore the initial state (NULL is ignored). */
MBE_API void mbe_imbe4400EncoderFree(mbe_imbe4400_encoder* enc);    /**< Free a context (NULL is ignored). */
/** Encode one frame to IMBE parameter bits (7200x4400 order). @return 0 or a status. */
MBE_API int mbe_encodeImbe4400Parms(mbe_imbe4400_encoder* enc, const float* samples, char imbe_d[88], mbe_parms* cur_mp, const mbe_parms* prev_mp);
MBE_API int mbe_encodeImbe4400ParmsShort(mbe_imbe4400_encoder* enc, const short* samples, char imbe_d[88], mbe_parms* cur_mp, const mbe_parms* prev_mp);   /**< 16-bit input variant. */

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* MBELIB_NEO_MBELIB_H */
