# DSDcc (vendored, with OrbitSDR additions)

Digital speech decoder library by Edouard Griffiths (F4EXB), GNU GPL v3
(see the headers of the source files; the original readme is `README.upstream.md`).
It decodes the frames of DMR, dPMR, D-STAR, YSF (System Fusion) and NXDN and
hands the voice frames to mbelib (`deps/mbelib`).

- Upstream: https://github.com/f4exb/dsdcc
- Commit: `f27b32d2df131ae3a376fe72d3fb880ae1f9ede1` (2025-09-20)
- Left out: `dsd_main.cpp` (the command line program), `serialdv/`, `testfec/`,
  `samples/`, `debian/`, the CMake files.

## OrbitSDR changes

- `export.h`, every `.cpp`: the diagnostics on `std::cerr` go to `DSDcc::nullLog()`
  (a stream that prints nothing); `dsd_logger.cpp` is quiet by default,
  `phaselock.cpp` no longer prints its settings.
- `dmr.h`, `dmr.cpp`, `dmr_ext.cpp` (new): per time slot `DSDDMR::SlotInfo`
  - the embedded LC is checked with its 5 bit checksum, and its FLCO is respected
    (talker alias and GPS blocks no longer overwrite the addresses),
  - service options: emergency, privacy (encrypted), broadcast, priority,
  - talker alias (7 bit, ISO 8 bit, UTF-8, UTF-16) and GPS info,
  - the data bursts are decoded (BPTC 196,96 with Hamming correction):
    voice LC header and terminator (Reed-Solomon 12,9 check), privacy (PI)
    header (algorithm id, key id), CSBK (opcode, feature set id), data header.
- `nxdn.h`, `nxdn.cpp`, `nxdnmessage.h`, `nxdnmessage.cpp`: cipher type and key id
  of a call.

Built by `scripts/dsdcc.sh` (sourced by the build scripts) into a static
library in `.cache/`. Used by `src/modules/Digital/`.
