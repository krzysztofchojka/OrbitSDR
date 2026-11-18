#!/bin/bash
# =====================================================================
#  TETRA downlink decoder (tetra-kit port) - build the vendored sources
#  once into .cache/.
#
#  Usage (from the repository root, inside a build script):
#      . scripts/tetra.sh
#  Afterwards, if the library is ready:
#      TETRA_OK=1
#      TETRA_FLAGS="-DENABLE_TETRA -I.../deps/tetra-kit/decoder"   (compile)
#      TETRA_LIB=".../libtetra.a"                                  (link)
#  On a compile error TETRA_OK=0 and the flags are empty: OrbitSDR is then
#  built without TETRA metadata decoding (the pi/4-DQPSK lock indicator
#  still works, it does not need this library).
#
#  deps/tetra-kit: the decoder stack of tetra-kit by Larry (GPL v3), with
#  OrbitSDR's in-process adaptations (see deps/tetra-kit/ORBITSDR_NOTICE.md).
#  main.cc (its UDP/getopt entry point) is excluded; everything else builds.
#
#  Environment:
#      ORBITSDR_NO_TETRA=1   leave TETRA metadata decoding out
#      ORBITSDR_NO_DIGITAL=1  leave the digital modes out entirely
#      ORBITSDR_CACHE         cache folder (default: <repo>/.cache)
#      CXX                    compiler (default: g++)
# =====================================================================

TETRA_OK=0
TETRA_FLAGS=""
TETRA_LIB=""

_tetra_main() {
    local repo cache cxx triple src bdir
    repo="$(pwd)"
    src="$repo/deps/tetra-kit/decoder"
    cache="${ORBITSDR_CACHE:-$repo/.cache}"
    cxx="${CXX:-}"
    if [ -z "$cxx" ]; then if command -v g++ >/dev/null 2>&1; then cxx=g++; else cxx=c++; fi; fi

    if [ "${ORBITSDR_NO_DIGITAL:-0}" = "1" ]; then
        echo "[tetra] TETRA decoding left out (ORBITSDR_NO_DIGITAL=1)"; return 1
    fi
    if [ "${ORBITSDR_NO_TETRA:-0}" = "1" ]; then
        echo "[tetra] TETRA decoding left out (ORBITSDR_NO_TETRA=1)"; return 1
    fi
    if [ ! -f "$src/decoder.cc" ]; then
        echo "[tetra] WARNING: deps/tetra-kit is missing, building without TETRA decoding"; return 1
    fi

    triple="$($cxx -dumpmachine 2>/dev/null || echo unknown)"
    bdir="$cache/tetra-build-$triple"
    mkdir -p "$bdir/obj" || return 1

    local inc=(-I"$src" -I"$src/common" -I"$src/mac" -I"$src/mle" -I"$src/mm"
               -I"$src/cmce" -I"$src/cmce/sds" -I"$src/llc" -I"$src/sndcp"
               -I"$src/uplane" -I"$src/wiremsg")
    local cxxflags=(-O2 -pipe -std=c++17 -w "${inc[@]}")

    local files=() f
    while IFS= read -r f; do files+=("$f"); done < <(find "$src" -name '*.cc' ! -name 'main.cc' | sort)

    local stamp
    stamp="$cxx|${cxxflags[*]}|$(find "$src" \( -name '*.cc' -o -name '*.h' \) ! -name 'main.cc' -exec cat {} + | cksum)"
    if [ -f "$bdir/libtetra.a" ] && [ "$(cat "$bdir/stamp" 2>/dev/null)" = "$stamp" ]; then
        TETRA_LIB="$bdir/libtetra.a"; return 0
    fi

    echo "[tetra] compiling the TETRA decoder library ..."
    rm -rf "$bdir/obj" "$bdir/libtetra.a"; mkdir -p "$bdir/obj" || return 1
    local jobs pids=() o p fail=0
    jobs="$(nproc 2>/dev/null || echo 4)"
    for f in "${files[@]}"; do
        o="$bdir/obj/$(basename "$(dirname "$f")")_$(basename "$f" .cc).o"
        $cxx "${cxxflags[@]}" -c "$f" -o "$o" &
        pids+=($!)
        if [ "${#pids[@]}" -ge "$jobs" ]; then wait "${pids[0]}" || fail=1; pids=("${pids[@]:1}"); fi
    done
    for p in ${pids[@]+"${pids[@]}"}; do wait "$p" || fail=1; done
    if [ "$fail" != "0" ]; then
        echo "[tetra] WARNING: compiling the library failed, building without TETRA decoding"; return 1
    fi
    (cd "$bdir/obj" && ar rcs ../libtetra.a *.o) || return 1
    echo "$stamp" > "$bdir/stamp"
    TETRA_LIB="$bdir/libtetra.a"
    return 0
}

if _tetra_main; then
    TETRA_OK=1
    TETRA_FLAGS="-DENABLE_TETRA -I$(pwd)/deps/tetra-kit/decoder"
    echo "[tetra] ready: $TETRA_LIB"
else
    TETRA_LIB=""
fi
unset -f _tetra_main
