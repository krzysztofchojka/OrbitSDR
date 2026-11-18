# mbelib (vendored)

AMBE / IMBE voice codec library (decoding), ISC license (see `COPYRIGHT`;
the original readme is `README.md`).

- Upstream: https://github.com/szechyjs/mbelib
- Commit: `9a04ed5c78176a9965f3d43f7aa1b1f5330e771f` (2019-05-28)
- Only the library sources are kept (no tests, CMake or packaging files).
  Not modified.

Patent notice (from the mbelib authors): the speech codecs implemented here
may be covered by patents in some countries; whether you may use them depends
on where you live. Builds can leave mbelib out with `./build.sh --no-vocoder`
(or `ORBITSDR_NO_VOCODER=1` for any build script): the digital voice decoders
then still show every call, only without the speech.

Compiled into the same static library as DSDcc by `scripts/dsdcc.sh`.
