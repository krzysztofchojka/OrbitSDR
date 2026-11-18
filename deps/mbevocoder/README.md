# mbevocoder (vendored, decoder part)

AMBE (3600x2400, D-STAR), AMBE+2 (3600x2450: DMR, dPMR, NXDN, YSF) and IMBE (7200x4400)
voice codec library by Jon Beniston M7RCE, GPL-3.0-or-later (see `LICENSE`, `NOTICE.md` and
`LICENSES/`; the original readme is `README.upstream.md`). It is the codec SDRangel uses from
version 8.2 on, in place of mbelib. Its decoder derives from mbelib and mbelib-neo, decodes as
DVSI's AMBE-3000R chip was measured to (PESQ 2.97 against mbelib's 2.57 on AMBE+2, 2.94
against 1.77 on D-STAR), and repeats or fades out frames it cannot correct instead of playing
them as noise.

- Upstream: https://github.com/srcejon/mbevocoder
- Tag `v3.0.0`, commit `c6e08b6368018e5c3ed52f3716f0115dfb3c5b3e` (2026-11-02)
- Kept: `include/`, and from `src/` the decoder (`decoder/`, `compat/mbelib1.cpp`, `bits/`,
  `core/`, `internal/`, `version.cpp`). Left out: the encoders, the transcoder, the IMBE
  analysis, `compat/neo_encoder.cpp`, `params/`, the tests, tools, corpus and CMake files.
- `include/mbevocoder/version.h` is written by hand from `version.h.in` (3.0.0).
- Built with the options of its `CMakeLists.txt` (`MBEVOCODER_CHIP_MEASURED=1`,
  `MBEVOCODER_FLOAT_CLIP=0`, SIMD on) by `scripts/dsdcc.sh`, into the same static library
  as DSDcc, which calls it through its mbelib 1.x API (`<mbelib/mbelib.h>`). Not modified.

Patent notice: the speech codecs implemented here may be covered by patents in some
countries; whether you may use them depends on where you live. Builds can leave the codec
out with `./build.sh --no-vocoder` (or `ORBITSDR_NO_VOCODER=1` for any build script): the
digital voice decoders then still show every call, only without the speech.
