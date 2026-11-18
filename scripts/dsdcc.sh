#!/bin/bash
# =====================================================================
#  DSDcc + mbevocoder (digital voice: DMR, dPMR, D-STAR, YSF, NXDN) - build
#  the vendored sources once
#
#  Usage (from the repository root, inside a build script):
#      . scripts/dsdcc.sh
#  Afterwards, if the library is ready:
#      DSDCC_OK=1
#      DSDCC_FLAGS="-DENABLE_DSDCC ..."   (pass to g++ when compiling)
#      DSDCC_LIB=".../libdsdcc.a"         (pass to g++ when linking)
#  On a compile error DSDCC_OK=0 and the flags are empty: OrbitSDR is then
#  built without the digital voice modes.
#
#  deps/dsdcc: DSDcc by Edouard Griffiths F4EXB (GPL v3), with OrbitSDR's
#  additions (see deps/dsdcc/README.md). deps/mbevocoder: the AMBE / IMBE
#  voice codecs (GPL v3, the decoder part of mbevocoder, see
#  deps/mbevocoder/README.md). Compiled into .cache/ (ignored by git) and
#  rebuilt only when a source file or the compiler changes.
#
#  Environment:
#      ORBITSDR_NO_VOCODER=1   leave the voice codec out: the frames are decoded and
#                              shown, the speech stays silent
#      ORBITSDR_NO_DIGITAL=1   leave the digital voice modes out entirely
#      ORBITSDR_CACHE          cache folder (default: <repo>/.cache)
#      CC / CXX                compilers (default: gcc / g++)
# =====================================================================

DSDCC_OK=0
DSDCC_FLAGS=""
DSDCC_LIB=""
DSDCC_VOCODER=0

_dsdcc_main() {
    local repo cache cc cxx triple dsrc msrc bdir vocoder
    repo="$(pwd)"
    dsrc="$repo/deps/dsdcc"
    msrc="$repo/deps/mbevocoder"
    cache="${ORBITSDR_CACHE:-$repo/.cache}"
    cc="${CC:-}"; cxx="${CXX:-}"
    if [ -z "$cc" ]; then if command -v gcc >/dev/null 2>&1; then cc=gcc; else cc=cc; fi; fi
    if [ -z "$cxx" ]; then if command -v g++ >/dev/null 2>&1; then cxx=g++; else cxx=c++; fi; fi
    if [ "${ORBITSDR_NO_DIGITAL:-0}" = "1" ]; then
        echo "[dsdcc] digital voice left out (ORBITSDR_NO_DIGITAL=1)"
        return 1
    fi
    if [ ! -f "$dsrc/dsd_decoder.cpp" ]; then
        echo "[dsdcc] WARNING: deps/dsdcc is missing, building without digital voice"
        return 1
    fi
    vocoder=1
    if [ "${ORBITSDR_NO_VOCODER:-0}" = "1" ] || [ ! -f "$msrc/src/decoder/mbelib.c" ]; then vocoder=0; fi

    triple="$($cxx -dumpmachine 2>/dev/null || echo unknown)"
    bdir="$cache/dsdcc-build-$triple-v$vocoder-mbev"
    mkdir -p "$bdir/obj" || return 1

    local cxxflags=(-O2 -pipe -std=c++17 -w -Ddsdcc_STATIC "-I$dsrc")
    # mbevocoder: the options its CMakeLists.txt sets (the AMBE-3000R-measured decoder)
    local mdefs=(-DMBE_STATIC -DMBE_BUILDING -DMBEVOCODER_CHIP_MEASURED=1 -DMBEVOCODER_FLOAT_CLIP=0
                 -DMBEVOCODER_ENABLE_SIMD=1 -D_USE_MATH_DEFINES
                 "-I$msrc/include" "-I$msrc/src" "-I$msrc/src/decoder" "-I$msrc/src/decoder/pffft")
    local cflags=(-O2 -pipe -std=gnu11 -w "${mdefs[@]}")
    local mcxxflags=(-O2 -pipe -std=c++17 -w "${mdefs[@]}")
    if [ "$vocoder" = "1" ]; then cxxflags+=(-DDSD_USE_MBELIB -DMBE_STATIC "-I$msrc/include"); fi

    local dfiles=() mfiles=() mxfiles=() f
    for f in "$dsrc"/*.cpp; do dfiles+=("$f"); done
    if [ "$vocoder" = "1" ]; then
        for f in "$msrc"/src/decoder/*.c "$msrc"/src/decoder/pffft/*.c; do mfiles+=("$f"); done
        for f in "$msrc"/src/*.cpp "$msrc"/src/*/*.cpp; do mxfiles+=("$f"); done
    fi

    local stamp
    stamp="$cc|$cxx|${cxxflags[*]}|${cflags[*]}|${mcxxflags[*]}|$(cat "${dfiles[@]}" "$dsrc"/*.h ${mfiles[@]+"${mfiles[@]}"} ${mxfiles[@]+"${mxfiles[@]}"} | cksum)"
    if [ -f "$bdir/libdsdcc.a" ] && [ "$(cat "$bdir/stamp" 2>/dev/null)" = "$stamp" ]; then
        DSDCC_LIB="$bdir/libdsdcc.a"
        DSDCC_VOCODER=$vocoder
        return 0
    fi

    echo "[dsdcc] compiling the digital voice library (DSDcc$( [ "$vocoder" = "1" ] && echo " + mbevocoder")) ..."
    rm -rf "$bdir/obj" "$bdir/libdsdcc.a"
    mkdir -p "$bdir/obj" || return 1
    # in parallel: one job per source file, as many at once as there are CPUs
    local jobs pids=() o fail=0
    jobs="$(nproc 2>/dev/null || echo 4)"
    for f in "${dfiles[@]}"; do
        o="$bdir/obj/d_$(basename "$f" .cpp).o"
        $cxx "${cxxflags[@]}" -c "$f" -o "$o" &
        pids+=($!)
        if [ "${#pids[@]}" -ge "$jobs" ]; then wait "${pids[0]}" || fail=1; pids=("${pids[@]:1}"); fi
    done
    for f in ${mfiles[@]+"${mfiles[@]}"}; do
        o="$bdir/obj/m_$(basename "$f" .c).o"
        $cc "${cflags[@]}" -c "$f" -o "$o" &
        pids+=($!)
        if [ "${#pids[@]}" -ge "$jobs" ]; then wait "${pids[0]}" || fail=1; pids=("${pids[@]:1}"); fi
    done
    for f in ${mxfiles[@]+"${mxfiles[@]}"}; do
        o="$bdir/obj/mx_$(basename "$(dirname "$f")")_$(basename "$f" .cpp).o"
        $cxx "${mcxxflags[@]}" -c "$f" -o "$o" &
        pids+=($!)
        if [ "${#pids[@]}" -ge "$jobs" ]; then wait "${pids[0]}" || fail=1; pids=("${pids[@]:1}"); fi
    done
    for p in ${pids[@]+"${pids[@]}"}; do wait "$p" || fail=1; done
    if [ "$fail" != "0" ]; then
        echo "[dsdcc] WARNING: compiling the library failed, building without digital voice"
        return 1
    fi
    (cd "$bdir/obj" && ar rcs ../libdsdcc.a *.o) || return 1
    echo "$stamp" > "$bdir/stamp"
    DSDCC_LIB="$bdir/libdsdcc.a"
    DSDCC_VOCODER=$vocoder
    return 0
}

if _dsdcc_main; then
    DSDCC_OK=1
    DSDCC_FLAGS="-DENABLE_DSDCC -Ddsdcc_STATIC -I$(pwd)/deps/dsdcc"
    if [ "$DSDCC_VOCODER" = "1" ]; then
        DSDCC_FLAGS="$DSDCC_FLAGS -DDSD_USE_MBELIB -DMBE_STATIC -I$(pwd)/deps/mbevocoder/include"
        echo "[dsdcc] ready (with the voice codec): $DSDCC_LIB"
    else
        DSDCC_FLAGS="$DSDCC_FLAGS -DORBITSDR_NO_VOCODER"
        echo "[dsdcc] ready (WITHOUT the voice codec): $DSDCC_LIB"
    fi
else
    DSDCC_LIB=""
fi
unset -f _dsdcc_main
