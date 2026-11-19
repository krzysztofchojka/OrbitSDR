# tetra-kit (vendored) — provenance

This directory holds the **decoder** stack of tetra-kit, vendored into OrbitSDR as the
basis for OrbitSDR's TETRA downlink decoder (SYSINFO / network info, SDS, LIP).

- Upstream:   https://gitlab.com/larryth/tetra-kit
- Commit:     7306a08c2dc753de636aaea8187e6ac5a3a99d02 (2024-03-03)
- License:    GNU GPL v3 (see LICENSE) — compatible with OrbitSDR's GPLv3 stack
              (DSDcc, mbevocoder).

## What was taken

Only `decoder/` (the pure C++ TETRA protocol stack: lower MAC channel coding,
MAC / LLC / MLE / MM / CMCE / SDS / LIP). The `phy/` GNU Radio pi/4-DQPSK flowgraph,
`codec/` and `recorder/` are NOT vendored: OrbitSDR supplies its own native
pi/4-DQPSK demodulator (src/modules/Digital/Tetra/TetraPhy.h) that produces the same
one-bit-per-symbol stream the decoder's `TetraDecoder::rxSymbol()` expects, and an
in-process reporting sink that replaces the decoder's UDP/JSON `Report`.

## OrbitSDR adaptations (tracked separately, not applied in-place yet)

- Replace the POSIX/UDP transport in `main.cc` / `common/report.*` with an in-process
  callback sink filling a `tetra::Status` struct (consumed by the Digital Voice panel).
- Guard the few POSIX-only includes so the stack builds under MSVC/MinGW (Windows).
- Build the `decoder/**/*.cc` into `.cache/` via `scripts/tetra.sh`, mirroring DSDcc.
- `mac/mac.cc` SYSINFO + `common/tetracell.*`: keep the cell's air interface security
  (D-MLE-SYSINFO "Air interface encryption service" bit and the extended services
  broadcast Security information element, EN 300 392-7 A.8.77) for the UI.
- `mac/mac.cc`: the file-scope `curBurstType` (only used in a debug log line) is
  `thread_local`: OrbitSDR runs the decoders of several TETRA channels in parallel.
