/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file mbevocoder/mbevocoder.h
 * @brief mbevocoder: AMBE (3600x2400), AMBE+2 (3600x2450) and IMBE (7200x4400, 7100x4400)
 *        vocoder library: decoder, encoder and transcoder. All symbols carry the `mbe_` prefix.
 *
 * This is the library's API. It includes `<mbelib-neo/mbelib.h>`, the API source-compatible with
 * mbelib-neo 2.6 (the decoder, and mbelib-neo's encoder functions on our encoders), and adds the encoder, the transcoder, tone frames,
 * on-air layouts, frame assembly and integer-codeword ECC. Code written for mbelib-neo can keep
 * including `<mbelib-neo/mbelib.h>`; code written against the original mbelib includes
 * `<mbelib/mbelib.h>`, the same decoder with its functions in their mbelib 1.x form.
 *
 * See docs/user-guide.md for how the pieces fit together. In short:
 *
 *  - **Decoding** (mbelib-neo compatible): keep one ::mbe_parms triplet per stream, call
 *    mbe_initMbeParms() once, then one `mbe_process*` function per received frame; each call
 *    returns 160 samples of 8 kHz audio and an ::mbe_process_result with error counts and flags.
 *  - **Encoding**: create an ::mbe_encoder for a rate, feed it 160 samples per call and take
 *    the output as parameter bits, an FEC-protected frame, on-air bits for a protocol layout,
 *    or AMBE-3000R channel bytes.
 *  - **Transcoding**: an ::mbe_transcoder converts parameter bits between rates without audio
 *    in between.
 *  - **Tone frames**: DTMF, call-progress and single tones as frames, both directions.
 *
 * Bit conventions (inherited from mbelib):
 *  - "bits" arrays hold one bit per `char`, values exactly 0 or 1. Any other value is rejected
 *    with ::MBE_STATUS_INVALID_BITS.
 *  - AMBE frames are `char fr[4][24]` bitplanes: `fr[0][23..0]` = C0 (Golay 24,12; index 23 is
 *    the first transmitted / most significant bit, index 0 the overall parity bit),
 *    `fr[1][22..0]` = C1 (Golay 23,12, scrambled), `fr[2][10..0]` = C2, `fr[3][13..0]` = C3.
 *  - IMBE 7200x4400 frames are `char fr[8][23]`: rows 0..3 Golay(23,12), rows 4..6
 *    Hamming(15,11) in `[14..0]`, row 7 seven unprotected bits in `[6..0]`. IMBE 7100x4400
 *    (ProVoice) frames are `char fr[7][24]`.
 *  - AMBE parameter bits `ambe_d[49]` and IMBE parameter bits `imbe_d[88]` are in the order
 *    produced by the `mbe_ecc*Data` functions (C0 data, C1 data, C2, C3 ...).
 *  - Every function that takes pointers validates them and returns ::MBE_STATUS_INVALID_ARGUMENT
 *    for NULL where a value is required, so callers may pass NULL for outputs they do not use
 *    only where a function says so.
 *
 * Threading: any number of streams, one thread per stream at a time, no shared mutable state
 * between streams. Comfort-noise and unvoiced-noise generators are thread-local (inherited from
 * mbelib-neo), so two streams interleaved on one thread get different noise fill than each
 * would alone; mbe_setThreadRngSeed() makes that reproducible. A thread that decoded should call
 * mbe_freeThreadResources() before it ends.
 *
 * Provenance: the decoder (synthesis, tone handling, erasure/mute logic) is the mbelib-neo 2.x
 * decoder with the AMBE-3000R-measured changes of `MBEVOCODER_CHIP_MEASURED`; the bit layer
 * (ECC, scrambling, frame assembly, on-air interleavers, field packing), the encoder (analysis
 * stage from OP25's IMBE vocoder, AMBE quantiser ported from OP25's ambe_encoder), the
 * transcoder and the tone frames are new. See NOTICE.md.
 */
#ifndef MBEVOCODER_MBEVOCODER_H
#define MBEVOCODER_MBEVOCODER_H

#include <mbevocoder/version.h>
#include <mbelib-neo/mbelib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================= */
/** @addtogroup ecc
 *  @{ */
/** @name Integer-codeword ECC
 *  Data in the high bits, parity in the low bits. These report true corrected counts over the
 *  whole codeword, unlike the mbelib-compatible char-array functions (mbe_golay2312() and the
 *  others in `<mbelib-neo/mbelib.h>`).
 *  */
/** @{ */
MBE_API uint32_t mbe_golay2312Encode(uint32_t data12);           /**< 12 data bits -> 23-bit codeword. */
MBE_API uint32_t mbe_golay2412Encode(uint32_t data12);           /**< 12 data bits -> 24-bit codeword (23-bit << 1 | overall parity). */
MBE_API int      mbe_golay2312Decode(uint32_t* codeword23);       /**< Corrects in place. @return Errors corrected, 0..3. */
MBE_API int      mbe_golay2412Decode(uint32_t* codeword24);       /**< Corrects in place. @return 0..3: errors corrected (the parity bit included); 4: a weight-4 error pattern, which the extended code detects but cannot correct (the word is then a guess). */
MBE_API uint32_t mbe_hamming1511Encode(uint32_t data11);          /**< 11 data bits -> 15-bit codeword, standard layout. */
MBE_API int      mbe_hamming1511Decode(uint32_t* codeword15, uint32_t* data11); /**< Corrects in place, extracts data. @return Errors corrected, 0..1. */
MBE_API uint32_t mbe_7100x4400hamming1511Encode(uint32_t data11); /**< ProVoice generator. */
MBE_API int      mbe_7100x4400hamming1511Decode(uint32_t* codeword15, uint32_t* data11); /**< ProVoice generator. */
/** @} */

/** Pseudo-random scrambling sequence `x[n+1] = (173 x[n] + 13849) mod 65536`, `x[0] = 16 seed`.
 *  Writes `count` bits, `bits[i] = x[i+1] >> 15`. `seed12` is the 12-bit C0 data word. */
MBE_API void     mbe_pnSequence(uint32_t seed12, char* bits, unsigned count);
/** The first 24 bits of mbe_pnSequence() as an integer, MSB first. */
MBE_API uint32_t mbe_pn24(uint32_t seed12);
/** @} */

/* ========================================================================= */
/** @defgroup layouts On-air layouts
 *  Interleavers between the mbelib bitplane frames and each protocol's transmitted bit order
 *  (one bit per `char`, transmission order). Verified against dsdcc's tables and AMBE-3000R
 *  captures (docs/bit-order.md).
 *  @{ */
/* ========================================================================= */

/** Protocol bit orders for voice frames. */
typedef enum mbe_layout {
    MBE_LAYOUT_DSTAR = 0,   /**< D-STAR voice frame, 72 bits in transmission order. */
    MBE_LAYOUT_DMR,         /**< DMR / NXDN half rate / dPMR / YSF V/D type 1 AMBE+2, 72 bits (36 dibits). */
    MBE_LAYOUT_P25P2,       /**< P25 Phase 2 voice codeword, 72 bits (36 dibits). */
    MBE_LAYOUT_P25P1,       /**< P25 Phase 1 IMBE voice codeword, 144 bits (72 dibits). */
    MBE_LAYOUT_YSF_VW,      /**< YSF voice full rate IMBE, 144 bits (72 dibits) after de-whitening. */
    MBE_LAYOUT_COUNT
} mbe_layout;

/** @return A short name for the layout, or "invalid". */
MBE_API const char* mbe_layoutName(mbe_layout layout);
/** @return Number of on-air bits for a layout (72 or 144); 0 for an invalid layout. */
MBE_API unsigned mbe_layoutBits(mbe_layout layout);

/** @name AMBE: on-air bits <-> 4x24 bitplane frame (72-bit layouts) */
/** @{ */
MBE_API int mbe_ambeFrameFromBits(mbe_layout layout, const char* bits, char ambe_fr[4][24]);       /**< @return 0 or a status. */
MBE_API int mbe_ambeFrameToBits(mbe_layout layout, const char ambe_fr[4][24], char* bits);         /**< @return 0 or a status. */
MBE_API int mbe_ambeSoftFrameFromBits(mbe_layout layout, const mbe_soft_bit* bits, mbe_soft_bit ambe_fr[4][24]);   /**< Soft-bit variant of mbe_ambeFrameFromBits(). */
/** @} */

/** @name IMBE: on-air bits <-> 8x23 frame (144-bit layouts) */
/** @{ */
MBE_API int mbe_imbeFrameFromBits(mbe_layout layout, const char* bits, char imbe_fr[8][23]);   /**< On-air bits -> IMBE frame. @return 0 or a status. */
MBE_API int mbe_imbeFrameToBits(mbe_layout layout, const char imbe_fr[8][23], char* bits);   /**< IMBE frame -> on-air bits. @return 0 or a status. */
MBE_API int mbe_imbeSoftFrameFromBits(mbe_layout layout, const mbe_soft_bit* bits, mbe_soft_bit imbe_fr[8][23]);   /**< Soft-bit variant of mbe_imbeFrameFromBits(). */
/** @} */

/** YSF V/D type 2: 104 de-whitened payload bits (52 dibits, transmission order) carry 49 AMBE+2
 *  parameter bits, the first 27 with triple redundancy.
 *  @return The number of majority-vote disagreements (0..27) or a status. */
MBE_API int mbe_ysfVd2ToAmbe2450Data(const char bits[104], char ambe_d[49]);
/** Inverse of mbe_ysfVd2ToAmbe2450Data(); bit 103 is set to 0. @return 0 or a status. */
MBE_API int mbe_ambe2450DataToYsfVd2(const char ambe_d[49], char bits[104]);

/** Pack `count` bits into bytes; `msb_first` selects the bit direction within each byte. The
 *  AMBE-3000R emits D-STAR frames LSB first and AMBE+2 frames MSB first. */
MBE_API void mbe_packBits(const char* bits, unsigned count, uint8_t* bytes, int msb_first);
/** Inverse of mbe_packBits(). */
MBE_API void mbe_unpackBits(const uint8_t* bytes, unsigned count, char* bits, int msb_first);

/** DVSI raw 2450 (AMBE-3000R rate index 34, 49 bits) order <-> `ambe_d` parameter order. `bytes7`
 *  holds 49 bits MSB first followed by 7 zero bits. @return 0 or a status. */
/** @{ */
MBE_API int mbe_ambe2450DataToDvsi(const char ambe_d[49], uint8_t bytes7[7]);   /**< Parameter bits -> AMBE-3000R raw bytes. */
MBE_API int mbe_dvsiToAmbe2450Data(const uint8_t bytes7[7], char ambe_d[49]);   /**< AMBE-3000R raw bytes -> parameter bits. */
/** @} */

/** @} */

/* ========================================================================= */
/** @defgroup encoder Encoder
 *  One ::mbe_encoder per stream. Input is 160 samples at 8 kHz per call, 16-bit or float on
 *  the 16-bit scale; the encoder adds 189 samples of delay (239 for D-STAR; the frame returned
 *  for input samples `n..n+159` describes the speech around `n-189`), which puts our frames on
 *  the same 20 ms of audio as an AMBE-3000R's. Every output
 *  form (parameter bits, mbelib frame, on-air layout, AMBE-3000R bytes) is available from the same
 *  encoder; each call consumes one frame regardless of which output form is used.
 *  Levels are calibrated so that an AMBE-3000R decodes our frames at the same loudness as its
 *  own encoding of the same speech (docs/encoder-quality.md).
 *  @{ */
/* ========================================================================= */

#define MBE_STATUS_ENCODE_RANGE     (-3)   /**< The frame could not be coded (pitch or level outside the codec's range, or a tone the rate cannot carry). The encoder has consumed the input (its analysis and front-end history move on) but its prediction state has not advanced; the caller may repeat the previous frame for that slot. */

/** Codec rates. The `3600x` rates include FEC; the bare rates are the parameter bits only. */
typedef enum mbe_rate {
    MBE_RATE_AMBE3600x2400 = 0, /**< D-STAR. */
    MBE_RATE_AMBE3600x2450,     /**< AMBE+2 with FEC: DMR, P25 Phase 2, NXDN, YSF DN, dPMR. */
    MBE_RATE_AMBE2450,          /**< AMBE+2 parameter bits only (49) / DVSI raw order (AMBE-3000R rate 34). */
    MBE_RATE_IMBE7200x4400,     /**< IMBE with FEC: P25 Phase 1, YSF VW, NXDN full rate. */
    MBE_RATE_IMBE4400,          /**< IMBE parameter bits only (88). */
    MBE_RATE_COUNT
} mbe_rate;

/** Encoder options; get defaults from mbe_encoderDefaultOptions(). */
typedef struct mbe_encoder_options {
    float gain_adjust;          /**< AMBE rates only: subtracted from the log2 gain on top of the calibration (1.0 = -6 dB); 0 = none. */
    float amplitude_floor;      /**< AMBE rates only: floor for the integer analysis amplitudes; 0 = library default (2.0). */
    int limiter;                /**< 1: peak limiter on the input (-1 dBFS ceiling, instant attack, 300 ms release), for float input that may exceed full scale; 0 = off. */
    int tone_detect;            /**< AMBE rates: 1 = code single tones, DTMF, KNOX and call-progress tones as tone frames, as the AMBE-3000R does by default (the library default); 0 = off. */
    int dtx;                    /**< AMBE rates only: 1 = voice activity detection: silence frames in pauses and ::MBE_ENCODER_FLAG_VOICE_ACTIVE cleared; 0 = off (default). */
    int noise_suppress;         /**< 1 = noise suppression on the input; 0 = off (default). */
    int echo_cancel;            /**< 1 = echo cancellation against the reference given by mbe_encoderSetEchoReference(); 0 = off (default). */
    int reserved;               /**< Must be zero. */
    float voicing_threshold[8]; /**< AMBE only: per-band multipliers on the analysis voicing threshold (bands as the AMBE+2 voicing table: 0 = lowest); 0 = library default. */
} mbe_encoder_options;

/** Opaque encoder stream. */
typedef struct mbe_encoder mbe_encoder;

/** Fill `opts` with the defaults (no gain adjustment, default floor, limiter off, tone detection on, DTX, noise suppression and echo cancellation off). */
MBE_API void         mbe_encoderDefaultOptions(mbe_encoder_options* opts);

#define MBE_ENCODER_FLAG_VOICE_ACTIVE 0x0002   /**< The last frame carries speech (always set unless DTX classed it as silence). */
#define MBE_ENCODER_FLAG_TONE         0x8000   /**< The last frame is a tone frame (tone detection). */
/** Status of the last encoded frame, a combination of `MBE_ENCODER_FLAG_*` (the AMBE-3000R's ECMODE_OUT). @return The flags, 0 before the first frame. */
MBE_API int          mbe_encoderFrameFlags(const mbe_encoder* e);
/** Echo cancellation: the far-end samples the application played out during the same 20 ms as the next frame's input
 *  (the local decoder's output, time-aligned by the application to within the canceller's 32 ms span); call before
 *  each encode when `echo_cancel` is on. NULL, or no call, means silence was played. */
MBE_API int          mbe_encoderSetEchoReference(mbe_encoder* e, const short far_end[160]);
/** Create an encoder for `rate`; `opts` may be NULL for defaults. @return NULL if the rate is unsupported, `opts->reserved` is not zero, or memory is exhausted. */
MBE_API mbe_encoder* mbe_encoderCreate(mbe_rate rate, const mbe_encoder_options* opts);
/** Destroy an encoder (NULL is ignored). */
MBE_API void         mbe_encoderDestroy(mbe_encoder* e);
/** Restore the initial state (analysis history, prediction state, limiter). */
MBE_API void         mbe_encoderReset(mbe_encoder* e);
/** @return The encoder's rate, or ::MBE_RATE_COUNT for NULL. */
MBE_API mbe_rate     mbe_encoderRate(const mbe_encoder* e);
/** @return The algorithmic delay in samples: 141 of analysis look-ahead plus the frame alignment to the AMBE-3000R (48 for AMBE+2 and IMBE, 98 for D-STAR), so 189 / 239; a further 160 with `noise_suppress` on. */
MBE_API int          mbe_encoderDelaySamples(const mbe_encoder* e);
/** @return The parameter-bit count for a rate: 49 for AMBE, 88 for IMBE; 0 if invalid. */
MBE_API int          mbe_encoderParmBits(mbe_rate rate);

/** The unquantised MBE model of the last encoded frame, for diagnostics and parameter-domain
 *  tests: `w0` (radians/sample), `L`, `Vl[1..L]` and `Ml[1..L]` (linear amplitudes on the
 *  analysis scale), `log2Ml[1..L]`. Other fields are zero. @return 0 or a status. */
MBE_API int mbe_encoderAnalysis(const mbe_encoder* e, mbe_parms* model);
/** Voicing statistics of the last encoded frame, for tuning and diagnostics: per harmonic
 *  `1..L`, the analysis band's error ratio divided by its voicing threshold (below 1 means the
 *  analysis stage called the band voiced; 0 or negative threshold reports as a large value),
 *  and the amplitude estimates under the voiced and unvoiced hypotheses (analysis scale).
 *  `ratio[0]` receives the pitch tracker's error measure (about 0..1; above 0.55 the analysis
 *  stage distrusts the pitch). Any output may be NULL. @return L or a status. */
MBE_API int mbe_encoderVoicingStats(const mbe_encoder* e, float ratio[57], float sa_voiced[57], float sa_unvoiced[57]);

/** Encode one frame to parameter bits (`ambe_d[49]` / `imbe_d[88]`). `b`, if given, receives
 *  the quantiser indices (9 for AMBE: fundamental, voicing, gain, two PRBA and four HOC
 *  indices; the 8 IMBE codewords for IMBE). @return 0 or a status (on ::MBE_STATUS_ENCODE_RANGE
 *  the input is consumed but no frame is produced; repeat the previous frame for that slot). */
MBE_API int mbe_encoderEncodeParms(mbe_encoder* e, const short pcm[160], char* d, int* b);
/** Float-input variant of mbe_encoderEncodeParms(); samples on the 16-bit scale, clipped to it unless the limiter is on. */
MBE_API int mbe_encoderEncodeParmsf(mbe_encoder* e, const float pcm[160], char* d, int* b);
/** Encode one frame to an FEC-protected AMBE frame (AMBE rates only). @return 0 or a status. */
MBE_API int mbe_encoderEncodeAmbeFrame(mbe_encoder* e, const short pcm[160], char ambe_fr[4][24]);
/** Encode one frame to an FEC-protected IMBE frame (IMBE rates only). @return 0 or a status. */
MBE_API int mbe_encoderEncodeImbeFrame(mbe_encoder* e, const short pcm[160], char imbe_fr[8][23]);
/** Encode one frame to on-air bits for `layout` (72 or 144 bits, see mbe_layoutBits()). The
 *  layout must carry the rate's frame: ::MBE_LAYOUT_DSTAR for 3600x2400, ::MBE_LAYOUT_DMR or
 *  ::MBE_LAYOUT_P25P2 for 3600x2450 / 2450, the 144-bit layouts for IMBE; any other pairing is
 *  ::MBE_STATUS_INVALID_ARGUMENT. @return 0 or a status. */
MBE_API int mbe_encoderEncode(mbe_encoder* e, const short pcm[160], mbe_layout layout, char* bits);
/** Encode one frame to AMBE-3000R channel-packet bytes: 9 bytes (DMR layout, MSB first) for
 *  3600x2450, 9 bytes (D-STAR layout, LSB first) for 3600x2400, 7 bytes (raw order) for 2450.
 *  IMBE rates are not supported (::MBE_STATUS_INVALID_ARGUMENT). */
MBE_API int mbe_encoderEncodeDvsi(mbe_encoder* e, const short pcm[160], uint8_t* bytes);

/** @} */

/* ========================================================================= */
/** @defgroup fields Parameter fields
 *  Quantiser indices of an AMBE parameter word: `b[0]` fundamental, `b[1]` voicing, `b[2]`
 *  gain, `b[3..4]` PRBA vectors, `b[5..8]` higher-order coefficients. Widths 2450:
 *  7,5,5,9,7,5,4,4,3. Widths 2400: 7,4,6,9,7,4,4,4,3 (`b[8]` LSB is always 0 as mbelib
 *  decodes it; `ambe_d[24]` is the C1 parity, not data). AMBE+2 `b[3]` / `b[4]` follow the
 *  AMBE-3000R's order of their low bits (b4's three in `ambe_d[40..42]`, b3's in
 *  `ambe_d[43]`), as the decoder reads them; mbelib 1.x/2.x and the neo-compat build (every
 *  AMBE-3000R-measured option off) use mbelib's order (b3's in `ambe_d[40]`, b4's in
 *  `ambe_d[41..43]`), which makes decoders pick a neighbouring codebook row on DVSI frames
 *  (docs/bit-order.md). @return 0 or a status.
 *  @{ */
/* ========================================================================= */

MBE_API int mbe_unpackAmbe2450Fields(const char ambe_d[49], int b[9]);   /**< AMBE+2 parameter bits -> indices. */
MBE_API int mbe_packAmbe2450Fields(const int b[9], char ambe_d[49]);     /**< Indices -> AMBE+2 parameter bits; out-of-range indices are a status. */
MBE_API int mbe_unpackAmbe2400Fields(const char ambe_d[49], int b[9]);   /**< D-STAR parameter bits -> indices. */
MBE_API int mbe_packAmbe2400Fields(const int b[9], char ambe_d[49]);     /**< Indices -> D-STAR parameter bits (`ambe_d[24]` left for the frame encoder). */

/** @} */

/* ========================================================================= */
/** @defgroup tones Tone frames
 *  AMBE tone identifiers as the decoders interpret them: 5 = 156.25 Hz, 6 = 187.5 Hz,
 *  7..122 = 31.25 Hz x id (218.75..3812.5 Hz), 128..163 dual tones (128..143 DTMF 0-9 A-D * #
 *  in the order 0,1,..,9,A,B,C,D,*,#; 144..159 KNOX; 160..163 call progress: dial, ring, busy,
 *  reorder). `amplitude` is the 7-bit AD field, 0..127: an AMBE-3000R renders 127 as a
 *  full-scale sine and 0.71 dB less per step, and its encoder emits about 112 for a tone at
 *  -10 dBFS. Tone frames leave the voice prediction state untouched on both sides, so they can
 *  be interleaved with voice frames from the same encoder. Layouts, numbering and amplitude
 *  laws are AMBE-3000R-verified (docs/tone-frames.md).
 *  @{ */
/* ========================================================================= */

#define MBE_TONE_AMPLITUDE_DEFAULT 103   /**< mbelib's nominal D-STAR tone level (about -17 dBFS on the AMBE-3000R's law). */

/** Frequencies of a tone id. @return 1 and fills `f1`/`f2` (equal for single tones), or 0 if invalid. */
MBE_API int mbe_toneFrequencies(int tone_id, float* f1, float* f2);
/** Nearest single-tone id to `f1` (218.75..3812.5 Hz; 156.25 and 187.5 map to 5 and 6), or the
 *  dual-tone id for the pair (`f1`, `f2`) when `f2 > 0` differs from `f1`. @return The id or -1. */
MBE_API int mbe_toneIdFromFrequencies(float f1, float f2);
/** D-STAR numbers dual tones differently (128..143 DTMF as 128 + 4 x column + row over
 *  columns 1209/1336/1477/1633 Hz and rows 697/770/852/941 Hz; 144..147 dial, ring, busy,
 *  reorder; no KNOX tones). Single tones are the same. @return The D-STAR index or -1. */
MBE_API int mbe_toneIdToDstar(int tone_id);
/** Inverse of mbe_toneIdToDstar(). @return The AMBE+2 id or -1. */
MBE_API int mbe_toneIdFromDstar(int dstar_id);
/** Build the 49 parameter bits of an AMBE+2 tone frame. @return 0 or a status. */
MBE_API int mbe_encodeAmbe2450ToneData(int tone_id, int amplitude, char ambe_d[49]);
/** Build the 49 parameter bits of a D-STAR tone frame; `tone_id` is the AMBE+2 numbering.
 *  @return 0, ::MBE_STATUS_ENCODE_RANGE if D-STAR cannot carry the tone (KNOX), or a status. */
MBE_API int mbe_encodeAmbe2400ToneData(int tone_id, int amplitude, char ambe_d[49]);
/** A tone frame through the encoder's rate, FEC and a channel layout (as mbe_encoderEncode()).
 *  IMBE rates have no tone frames (::MBE_STATUS_INVALID_ARGUMENT). Does not consume audio. */
MBE_API int mbe_encoderTone(mbe_encoder* e, int tone_id, int amplitude, mbe_layout layout, char* bits);
/** A tone frame as AMBE-3000R channel bytes (as mbe_encoderEncodeDvsi()). */
MBE_API int mbe_encoderToneDvsi(mbe_encoder* e, int tone_id, int amplitude, uint8_t* bytes);

/** @} */

/* ========================================================================= */
/** @defgroup transcoder Parameter-domain transcoder
 *  Converts vocoder parameter bits between rates without synthesising audio: the source frame
 *  is decoded to the MBE model (fundamental, voicing, harmonic amplitudes) with the library's
 *  own parameter decoder and requantised for the target rate with the target decoder in the
 *  loop. Any source rate to any target rate; the AMBE rates use `ambe_d[49]` (after
 *  `mbe_decodeAmbe*Frame` / before `mbe_encodeAmbe*Frame`) and the IMBE rates `imbe_d[88]`.
 *  Between the two 2450 rates, from D-STAR to D-STAR, and between the two IMBE rates, voice and
 *  tone frames are copied unchanged (the return value still classifies the frame); erasures are replaced by a repeat
 *  frame like any other rate pair, so the output is always a usable frame.
 *  Tone frames are converted between the AMBE rates; gain is shifted between AMBE+2 and D-STAR
 *  so a bridged call keeps its loudness on real radios (docs/transcoder.md).
 *  @{ */
/* ========================================================================= */

/** Opaque transcoder stream (one per bridged call, in one direction). */
typedef struct mbe_transcoder mbe_transcoder;

#define MBE_TRANSCODE_VOICE    0   /**< A voice frame was produced. */
#define MBE_TRANSCODE_TONE     1   /**< The input was a tone frame; a target tone frame was produced. */
#define MBE_TRANSCODE_REPEAT   2   /**< The input was an erasure, invalid, or a tone the target cannot carry; the previous model was repeated (a quiet unvoiced frame if there is none yet). */

/** Create a transcoder. @return NULL if a rate is invalid or memory is exhausted. */
MBE_API mbe_transcoder* mbe_transcoderCreate(mbe_rate from, mbe_rate to);
/** Destroy a transcoder (NULL is ignored). */
MBE_API void mbe_transcoderDestroy(mbe_transcoder* t);
/** Restore the initial state of both the source decoder and the target quantiser. */
MBE_API void mbe_transcoderReset(mbe_transcoder* t);
MBE_API mbe_rate mbe_transcoderSourceRate(const mbe_transcoder* t);   /**< @return The source rate. */
MBE_API mbe_rate mbe_transcoderTargetRate(const mbe_transcoder* t);   /**< @return The target rate. */
/** Transcode one frame. `d_in` holds mbe_encoderParmBits(from) bits, `d_out` receives
 *  mbe_encoderParmBits(to) bits. @return An `MBE_TRANSCODE_*` value or a status. */
MBE_API int mbe_transcoderProcess(mbe_transcoder* t, const char* d_in, char* d_out);
/** The source decoder's model state after the last input frame (valid after ::MBE_TRANSCODE_VOICE /
 *  ::MBE_TRANSCODE_REPEAT), or NULL: the frame's decoded model, or after an AMBE+2 silence frame the
 *  last voice frame's, which the decoders keep as their prediction state across silence frames. */
MBE_API const mbe_parms* mbe_transcoderModel(const mbe_transcoder* t);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* MBEVOCODER_MBEVOCODER_H */
