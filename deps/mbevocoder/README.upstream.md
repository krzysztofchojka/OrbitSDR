# mbevocoder

AMBE (3600x2400, D-STAR), AMBE+2 (3600x2450: DMR, P25 Phase 2, YSF DN, NXDN, dPMR) and
IMBE (7200x4400: P25 Phase 1, YSF VW, NXDN full rate) vocoder library in C++17 with a C API.
The API is `<mbevocoder/mbevocoder.h>`. It is source-compatible with mbelib-neo 2.6
(`<mbelib-neo/mbelib.h>`, encoder functions included; not its TETRA ACELP decoder) and its decoder part with the original mbelib 1.x
(`<mbelib/mbelib.h>`), so code written for either builds unchanged; the full encoder API with
its options and front end, the transcoder and tone frames need `<mbevocoder/mbevocoder.h>`.
Static library, CMake, Windows / macOS / Linux / Android. GPL-3.0-or-later.

Encoder features automatic tone detection, VAD/DTX with
silence frames and comfort noise for the gaps, a noise suppressor and an echo
canceller ([docs/frontend.md](docs/frontend.md)). 

Speed: encoder about 65x real time, decoder about 1600x on one desktop core 
([docs/benchmarks.md](docs/benchmarks.md)).

Start with the [user guide](docs/user-guide.md); the API reference is generated from the header
with `doxygen Doxyfile` (output in `build/doxygen/html`).

## How it compares

The reference is DVSI's **AMBE-3000R** vocoder chip, the hardware
implementation of these codecs (here in a ThumbDV / DV3000U USB dongle driven by the
`ambe3000` tool); every quality figure is a comparison with what it produces for the same input.

Decoding the frames the AMBE-3000R encoded from the 70-file speech corpus, scored
against the original speech and against the AMBE-3000R's own decode of the same frames
(`tools/quality/decoder_lineage.py`, `corpus/baselines/decoder_lineage.json`). The measures:

- **PESQ** (ITU-T P.862, narrowband): a model of how a listener would rate the decoded
  speech against the original, on a 1 (bad) to 4.5 (excellent) scale. Higher is better.
- **ESTOI** (extended short-time objective intelligibility): how much of the original's
  spectro-temporal structure survives, 0 to 1. Higher is better; tracks intelligibility
  rather than pleasantness.
- **LSD** (log-spectral distance): the mean dB difference between the short-time spectra of
  two signals, here our decoder's output and the AMBE-3000R's for the same frames, after
  level matching. Lower is better; 0 dB would be the same output.

| Decoder | AMBE+2 PESQ (higher better) | ESTOI (higher) | LSD vs AMBE-3000R (lower) | D-STAR PESQ (higher) | ESTOI (higher) | LSD vs AMBE-3000R (lower) |
|---|---|---|---|---|---|---|
| AMBE-3000R (hardware) | 2.94 | 0.810 | - | 3.00 | 0.805 | - |
| mbelib 1.3 | 2.57 | 0.732 | 8.16 dB | 1.77 | 0.561 | 11.41 dB |
| mbelib-neo 2.6.5 | 2.87 | 0.803 | 6.87 dB | 2.92 | 0.798 | 6.97 dB |
| **mbevocoder** | **2.97** | **0.803** | **6.97 dB** | **2.94** | **0.800** | **7.03 dB** |

On DVSI's own test vectors (DVSI's encoder's bits for 27 speech recordings, scored against the
input; [docs/decoder-quality.md](docs/decoder-quality.md), "DVSI's test vectors"; the vectors
themselves are DVSI's and are not redistributed). The AMBE-3000R's decode of them is
sample-identical to DVSI's published decode, so its row is DVSI's reference decoder:

| Decoder | D-STAR PESQ / ESTOI | AMBE+2 PESQ / ESTOI | IMBE PESQ / ESTOI |
|---|---|---|---|
| DVSI (AMBE-3000R; for IMBE DVSI's software) | 2.93 / 0.766 | 2.98 / 0.769 | 3.03 / 0.792 |
| mbelib 1.3 | 1.81 / 0.565 | 2.63 / 0.706 | 2.76 / 0.749 |
| mbelib-neo 2.6.5 | 2.85 / 0.764 | 2.86 / 0.767 | 2.91 / 0.786 |
| **mbevocoder** | **2.88 / 0.764** | **2.89 / 0.762** | **2.94 / 0.787** |

What mbevocoder has beyond mbelib 1.x and mbelib-neo, all behind the same `mbe_*` C API (mbelib-neo
has encoders too since 2.3 and 2.4; the first item compares them):

- Encoders for AMBE+2 (DMR, P25 Phase 2, NXDN, YSF DN, dPMR), D-STAR and IMBE (P25 Phase 1,
  YSF VW, NXDN full rate), level-calibrated to the AMBE-3000R, with output as parameter
  bits, FEC frames, on-air bits for every layout, or AMBE-3000R channel bytes. Through the
  AMBE-3000R's decoder the D-STAR encoder scores as the AMBE-3000R's own does (PESQ 3.01 against its 3.00 on
  the corpus) and the AMBE+2 encoder 3.04 against its 2.94. On DVSI's own test vectors all three encoders
  score above DVSI's encoder (D-STAR 3.11 against 2.93, AMBE+2 3.13 against 2.98, IMBE 3.05 against
  2.94), quiet microphones included ([docs/encoder-quality.md](docs/encoder-quality.md)).
  Through the AMBE-3000R's decoder mbelib-neo 2.6.5's AMBE+2 encoder scores lower (2.91 on the corpus
  against our 3.04); its D-STAR encoder (2.97 against 3.01) and its IMBE encoder (3.06 against 3.07
  through our decoder) are within error of ours.
- The AMBE-3000R's encoder front end: automatic tone detection, VAD/DTX with silence frames and
  comfort noise for the gaps, noise suppression, echo cancellation, and `ECMODE_OUT`-style
  frame flags ([docs/frontend.md](docs/frontend.md)).
- A parameter-domain transcoder between every rate (D-STAR <-> AMBE+2 <-> IMBE), no audio
  in between, AMBE-3000R-verified levels ([docs/transcoder.md](docs/transcoder.md)).
- Tone frames: encoding (DTMF, KNOX, call progress, single tones) as well as decoding, with
  the AMBE-3000R's numbering and amplitude laws on both rates; D-STAR dual tones rendered
  ([docs/tone-frames.md](docs/tone-frames.md)).
- Decoding as the hardware was measured to do it: D-STAR tables, mixed-excitation voicing,
  geometric amplitude ramps and smoothed phases, an 80 Hz output high-pass, the silence-frame
  model, and the AMBE-3000R's output level (DVSI's, within 0.2 dB), speech and tones alike; the
  float output unclipped.
- The complete bit layer as public API: Golay/Hamming, PN scrambler, FEC and interleave for
  all four codecs, on-air layouts, pack/unpack of every quantiser field.
- Error handling measured against the AMBE-3000R: `mbe_processLostFramef` for frames the
  framing layer knows were lost (three repeats, a fade, no level jump on recovery), and
  frames whose Golay word is detectably uncorrectable, AMBE+2 erasures and unusable tone frames
  are repeated as the AMBE-3000R repeats them rather than decoded as garbage or played as noise;
  PESQ under 0.5-5 % BER, bursts and lost frames within the clean-frame gap of the
  AMBE-3000R's own decoder ([docs/decoder-quality.md](docs/decoder-quality.md)).

## Build

```bash
cmake --preset release
cmake --build --preset release
ctest --preset release
```

Presets: `debug`, `release`, `asan-ubsan`, `tsan`, `strict-fp`, `neo-compat` (every
AMBE-3000R-measured option off: mbelib-neo 2.1's output to float rounding, for verification only), `vs2026`, `android-arm64`,
`android-armv7`, `android-x86_64` (the Android presets need `ANDROID_NDK_HOME`, e.g. NDK 27,
and build the library only; add `-DMBEVOCODER_BUILD_TESTS=ON -DMBEVOCODER_BUILD_TOOLS=ON` for the rest).

## Using it

```c
#include <mbevocoder/mbevocoder.h>   /* decode-only code may keep <mbelib-neo/mbelib.h>,
                                       or <mbelib/mbelib.h> for code written for mbelib 1.x */

/* decode: mbelib-neo's API */
mbe_parms cur, prev, enh;
mbe_initMbeParms(&cur, &prev, &enh);
float pcm[160];
mbe_processAmbe3600x2450Framef(pcm, NULL, ambe_fr, ambe_d, &cur, &prev, &enh);

/* encode */
mbe_encoder_options opts;
mbe_encoderDefaultOptions(&opts);          /* tone_detect on; dtx, noise_suppress, echo_cancel off */
mbe_encoder* enc = mbe_encoderCreate(MBE_RATE_AMBE3600x2450, &opts);
char bits[72];
mbe_encoderEncode(enc, pcm16, MBE_LAYOUT_DMR, bits);
int flags = mbe_encoderFrameFlags(enc);    /* MBE_ENCODER_FLAG_TONE, MBE_ENCODER_FLAG_VOICE_ACTIVE */
mbe_encoderDestroy(enc);
```

Rates: `MBE_RATE_AMBE3600x2400` (D-STAR), `MBE_RATE_AMBE3600x2450` (AMBE+2), `MBE_RATE_AMBE2450`
(raw), `MBE_RATE_IMBE7200x4400`, `MBE_RATE_IMBE4400`; layouts for D-STAR, DMR, P25 Phase 1
and 2, YSF VW and V/D 2; `mbe_encoderEncodeDvsi` gives AMBE-3000R channel bytes and
`mbe_transcoder*` converts frames between rates. The [user guide](docs/user-guide.md) has the rest.

## Tools

- `ambe3000`: drives an AMBE-3000R dongle (ThumbDV / DV3000U) as a reference encoder and
  decoder. `ambe3000 --port COM3 identify`, `... --rate dstar|ambe2450|raw2450 loopback in.wav out.dvsi out.wav`;
  `--ecmode 0x1000` turns the AMBE-3000R's tone detector on (`0x0800` DTX, `0x0040` noise suppression;
  `--ctl 150001` adds the AMBE-3000R's `ECMODE_OUT` per frame as a `.cmode` file), `--ctl HEX` sends
  any control packet.
- `mbe-cli encode|decode|parms|frbits|frpack|dpack|dunpack|frames|transcode|tone`: WAV to frames (AMBE+2, D-STAR, raw 2450,
  IMBE 4400/7200; `--dtx`, `--noise-suppress`, `--echo-cancel far.wav`, `--no-tone-detect`,
  `--flags`, `--fe-out` for the front end), frames to WAV, quantiser-index dumps, frame synthesis
  from indices or parameter bits for probing the AMBE-3000R, frame text to channel bytes,
  parameter-domain transcoding between rates, and tone frame generation.
- `neo-decode`: the same with an external mbelib-neo build, plus `--dump` / `--synth` to
  produce the parity references in `testdata/neo` (configure with `MBELIB_NEO_INCLUDE_DIRS`
  and `MBELIB_NEO_LIBRARY`; build mbelib-neo without fast-math for exact references).
- `tools/quality/compare.py`, `encoder_compare.py`, `hil_encoder.py`, `hil_transcode.py`,
  `imbe_baseline.py`, `jmbe_crosscheck.py`: decoder, encoder and transcoder quality against the AMBE-3000R (and our IMBE frames through JMBE), in the parameter
  and audio domains; the HIL ones decode our frames on the AMBE-3000R and write listening sets.
  `perceptual.py`: PESQ and ESTOI for the same chains (`pip install numpy scipy pesq pystoi`);
  `synth_diag.py`, `synth_experiment.py`, `perceptual_frames.py`: our synthesis against the AMBE-3000R's
  on the same frames, per frame.
  `frontend_oracle.py`, `decoder_mismatch.py`: oracle and frame-by-frame diagnosis of the
  encoder and decoder against the AMBE-3000R. `probe_f0.py`, `probe_gain.py`, `probe_gain_step.py`,
  `probe_vuv.py`, `probe_vuv_rows.py`, `probe_vuv_mix.py`, `dstar_lsweep.py`: parameter-by-parameter probing of the AMBE-3000R's decoder;
  `probe_tone_detect.py`, `probe_dtx.py`, `probe_ns.py`: the AMBE-3000R's tone detector, VAD/DTX and
  noise suppressor frame by frame, with `--ours` for the library's. `hil_errors.py`: bit-error
  behaviour against the AMBE-3000R; `encoder_errors.py`: our encoders' frames and the reference
  encoders' under the same channel errors; `decoder_lineage_all.py`: the decoder lineage for IMBE
  and under errors; `rate_ladder.py`: encoder variants on any rate over four input framings;
  `imbe_reference_compare.py`: IMBE against the OpenBLIP25 encoder; `dvsi_decoders.py`,
  `dvsi_encoders.py` (with the drivers in `dvsi_harness`): decoders and encoders on DVSI's own test
  vectors, which are fetched locally and never redistributed; `neo_encoders.py`: mbelib-neo's
  encoders (2.4 and later) against ours through the AMBE-3000R; `probe_ambe2450_frames.py`: AMBE+2
  erasures, tone frames and heavy errors on the AMBE-3000R against our decoder;
  `probe_dstar_frames.py`: which D-STAR codes the AMBE-3000R plays as lost frames. What they need
  (builds, test data, the AMBE-3000R, external decoders) is in [tools/quality/README.md](tools/quality/README.md).
- `tools/quality/listening.py`, `listening_step5.py`, `listening_fricatives.py`, `listening_imbe.py`,
  `listening_pauses.py`: listening sets (chains per file and rate, level matched), blind A/B pairs
  with a hidden key and a results sheet, and the reveal.
- `mbelib1-decode` (with `-DMBELIB1_SOURCE_DIR=<mbelib 1.x checkout>`): mbelib 1.x built from
  its sources for `tools/quality/decoder_lineage.py`, the three-decoder comparison above
  (mbelib-neo is scored with `neodec` from `tools/quality/dvsi_harness`, built against an mbelib-neo
  checkout).
- `tools/corpus/*.py`: fetch the speech sources, prepare the 8 kHz test corpus, capture goldens.

## Test data

Small goldens (AMBE-3000R-encoded frames) live in `corpus/golden`. The corpus audio and AMBE-3000R-decoded
PCM are fetched into `testdata/` from a release bundle (`-DMBEVOCODER_FETCH_TESTDATA=ON`) or
regenerated locally with the tools. See `corpus/SOURCES.md` for licences.
