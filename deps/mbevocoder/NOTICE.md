# Third-party code and data

mbevocoder is licensed under GPL-3.0-or-later (see LICENSE). It incorporates or derives from:

| Origin | What | Licence |
|---|---|---|
| mbelib (Copyright 2010 mbelib Author, https://github.com/szechyjs/mbelib) | Golay and Hamming generator constants, parameter-bit field layouts, quantiser tables | ISC (permission notice in `LICENSES/ISC-mbelib.txt`) |
| mbelib-neo 2.1.0 and 2.6.2 (https://github.com/arancormonk/mbelib-neo) | The decoder: `src/decoder/*.c` and headers are vendored from 2.1.0's `src/core`, `src/ambe`, `src/imbe` and `src/internal` with the bit-layer functions removed. The parts mbelib-neo 2.6.0 re-implemented independently from TIA-102.BABA to settle their provenance (its `legal/implementation-provenance.md`: adaptive smoothing, unvoiced FFT synthesis and its noise generator, comfort noise, the initial, erasure and repeat-reset models, the tone oscillator, the tone-field check, the output limiter and 16-bit conversion) are taken from 2.6.2 instead (`mbe_adaptive.c`, `mbe_unvoiced_fft.c`, parts of `mbelib.c`, `ambe_common.c`, `ambe3600x2450.c`, `imbe7200x4400.c`). API shape (to 2.6, whose encoder functions `src/compat/neo_encoder.cpp` implements on our encoders), soft-decision decoding and error-count conventions | GPL-2.0-or-later |
| PFFFT (Julien Pommier) and FFTPACK (NCAR/UCAR) | `src/decoder/pffft/`, vendored via mbelib-neo | BSD-style (`LICENSES/BSD-PFFFT-FFTPACK.txt`, also `src/decoder/pffft/COPYING`) |
| dsdcc (Copyright Edouard Griffiths F4EXB, https://github.com/f4exb/dsdcc) | D-STAR, AMBE+2, YSF V/D type 2 and YSF VW on-air interleave tables | GPL-3.0-or-later |
| DSD (https://github.com/szechyjs/dsd) | P25 Phase 1 IMBE interleave table | GPL-2.0-or-later |
| OP25 (Copyright Max H. Parke KA1RBI, https://github.com/boatbod/op25) | P25 Phase 2 voice codeword interleave; AMBE+2 quantiser (`src/encoder/ambe_quant.cpp`, ported from `ambe_encoder.cc` and generalised to D-STAR) | GPL-3.0-or-later |
| IMBE vocoder, Project 25 IMBE Encoder/Decoder Fixed-Point implementation (Copyright 2009 Pavel Yazev, via OP25) | `src/analysis/imbe_vocoder/`, vendored with changes marked `mbevocoder`: the banner print removed, a thread-local RNG seed (`rand_gen.cc`), a named `IMBE_PARAM` struct, pitch-tracker changes (`pitch_est.cc`), the analysis split into stages with a model-domain entry `imbe_encode_model` and a model decoder (`encode.cc`, `decode.cc`, `imbe_vocoder.cc/.h`), the prediction below harmonic 1 from harmonic 1's amplitude in the amplitude quantiser (`sa_encode.cc`), a voicing-threshold scale, double-precision voicing energies and a level gate (`v_uv_det.cc`), the input rescale of the block-floating-point gain (`encode.cc`), and portability fixes (`basicop2.cc`, `globals.h`, `typedefs.h`) | GPL-3.0-or-later |

The encoder front end (`src/encoder/tonedetect.cpp`, `vad.cpp`, `noisesuppress.cpp`,
`echocancel.cpp`) is original code on published methods: Martin's minimum-statistics noise
estimate, Ephraim and Malah's decision-directed a priori SNR with a Wiener gain, the NLMS
adaptive filter with Geigel's double-talk test. No third-party code.

The install puts LICENSE, this file and `LICENSES/` under `share/doc/mbevocoder`. Test corpus sources and
their licences are listed in `corpus/SOURCES.md`.
