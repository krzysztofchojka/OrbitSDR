# DSDcc (vendored, with OrbitSDR additions)

Digital speech decoder library by Edouard Griffiths (F4EXB), GNU GPL v3
(see the headers of the source files; the original readme is `README.upstream.md`).
It decodes the frames of DMR, dPMR, D-STAR, YSF (System Fusion) and NXDN and
hands the voice frames to mbevocoder (`deps/mbevocoder`, its mbelib 1.x API).

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
- `dsd_mbe.h`, `dsd_mbe.cpp`: the voice gate. A sync pattern matched by chance in noise
  (about once a second over all patterns) started the decoding of frames that were noise:
  a crackle, and a call in the list. Each frame's FEC error count decides: noise always needs
  4 or more Golay corrections, a received signal mostly none. Frames wait (up to 4) until two
  of them have at most 2 errors, then all are played; 6 noise-like frames in a row or a quarter
  of a second without frames close the gate, and the codec starts afresh after a pause.
  `ysf.cpp` gives the V/D type 2 frames a quality (the triplets whose copies disagree).
- `dsd_decoder.h`, `.cpp`: a sample counter (the gate's clock), `voiceGateOpen()`, and a CRC
  score (`crcPassed()`, `getCrcScore()`) that `dmr_ext.cpp`, `dpmr.cpp`, `dstar.cpp`, `nxdn.cpp`
  and `ysf.cpp` raise when a CRC of the signalling checks (weighted by its length): OrbitSDR
  takes a signal as found only once its speech passes the gate or its CRCs check.
- `dmr.h`, `dmr_ext.cpp`: text messages (SMS): the data header (UDT, unconfirmed and
  confirmed packet data), the rate 1/2 blocks (CRC-9 of confirmed blocks, the CRC-16 of
  UDT, the packet CRC-32) and the text in UDT (7 bit, 8 bit, UTF-16), the DMR text service
  over IPv4 / UDP (port 5016, also with the compressed UDP header) and Motorola TMS (port
  4007). Ported from SDRangel 8.2's `dsdprotocoldmr.cpp` and `dsdpackettext.cpp` (GPL v3).
- `dsd_mbelib.h`: includes mbevocoder's `<mbelib/mbelib.h>`.
- `dsd_decoder.cpp`: dPMR with an inverted discriminator (inverted FS1, which is the
  FS4 pattern) is decoded too; upstream finds only the positive polarity, so receivers
  and recordings with the opposite FM sign gave no speech.
- `mbefec.cpp`, `dmr_ext.cpp`: thread safety (OrbitSDR runs the channels' decoders in
  parallel): `GolayMBE::mbe_checkGolayBlock` kept its working variables in `static`s shared
  by every instance, and `fixHamming` built its syndrome table lazily without a guard; now
  locals and a once-initialised `static const` table.

Built by `scripts/dsdcc.sh` (sourced by the build scripts) into a static
library in `.cache/`. Used by `src/modules/Digital/`.
