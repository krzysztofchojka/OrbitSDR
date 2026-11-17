# ft8_lib (vendored)

FT8 / FT4 decoder library by Kārlis Goba (YL3JG), MIT license (see `LICENSE`).

- Upstream: https://github.com/kgoba/ft8_lib
- Commit: `9fec6ca39886edbf96f4f5e71edc76da5074e871` (2025-08-24)
- Only the library parts are kept: `ft8/`, `fft/` (KISS FFT, BSD) and
  `common/monitor.*`. The demos, tests and audio / WAV helpers are left out.
- `compat.c` is OrbitSDR's: a portable `stpcpy()`.

Built by `scripts/ft8lib.sh` (sourced by the build scripts) into a static
library in `.cache/`. Used by `src/modules/WSJT/`.
