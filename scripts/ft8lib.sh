#!/bin/bash
# =====================================================================
#  ft8_lib (FT8 / FT4 decoder) - build the vendored sources once
#
#  Usage (from the repository root, inside a build script):
#      . scripts/ft8lib.sh
#  Afterwards, if the library is ready:
#      FT8LIB_OK=1
#      FT8LIB_FLAGS="-DENABLE_FT8LIB -I deps/ft8_lib"  (pass to g++ when compiling)
#      FT8LIB_LIB=".../libft8.a"                       (pass to g++ when linking)
#  On a compile error FT8LIB_OK=0 and the flags are empty: OrbitSDR is then
#  built without FT8 / FT4 (WSPR is OrbitSDR's own code and always works).
#
#  The sources are in deps/ft8_lib (MIT license). They are C, so they are
#  compiled with the C compiler into .cache/ (ignored by git) and rebuilt
#  only when a source file or the compiler changes.
#
#  Environment:
#      ORBITSDR_CACHE   cache folder (default: <repo>/.cache)
#      CC               C compiler (default: gcc, or cc if gcc is missing)
# =====================================================================

FT8LIB_OK=0
FT8LIB_FLAGS=""
FT8LIB_LIB=""

_ft8lib_main() {
    local repo cache cc triple src bdir
    repo="$(pwd)"
    src="$repo/deps/ft8_lib"
    cache="${ORBITSDR_CACHE:-$repo/.cache}"
    cc="${CC:-}"
    if [ -z "$cc" ]; then
        if command -v gcc >/dev/null 2>&1; then cc=gcc; else cc=cc; fi
    fi
    if [ ! -f "$src/ft8/decode.c" ]; then
        echo "[ft8_lib] WARNING: deps/ft8_lib is missing, building without FT8 / FT4"
        return 1
    fi
    triple="$($cc -dumpmachine 2>/dev/null || echo unknown)"
    bdir="$cache/ft8lib-build-$triple"
    mkdir -p "$bdir/obj" || return 1

    local files=("$src/ft8/constants.c" "$src/ft8/crc.c" "$src/ft8/decode.c" "$src/ft8/encode.c" "$src/ft8/ldpc.c"
                 "$src/ft8/message.c" "$src/ft8/text.c" "$src/common/monitor.c" "$src/fft/kiss_fft.c" "$src/fft/kiss_fftr.c"
                 "$src/compat.c")
    # quiet: the library prints its settings to stderr otherwise
    local cflags=(-O2 -pipe -std=gnu11 -w "-I$src" -Dstpcpy=ftx_stpcpy "-DLOG_PRINTF(...)=((void)0)" -D_USE_MATH_DEFINES)

    # rebuild when a source file or the compiler / flags change
    local stamp
    stamp="$cc|${cflags[*]}|$(cat "${files[@]}" "$src"/ft8/*.h "$src"/common/*.h "$src"/fft/*.h | cksum)"
    if [ -f "$bdir/libft8.a" ] && [ "$(cat "$bdir/stamp" 2>/dev/null)" = "$stamp" ]; then
        FT8LIB_LIB="$bdir/libft8.a"
        return 0
    fi

    echo "[ft8_lib] compiling the FT8 / FT4 library ..."
    rm -rf "$bdir/obj" "$bdir/libft8.a"
    mkdir -p "$bdir/obj" || return 1
    local f o
    for f in "${files[@]}"; do
        o="$bdir/obj/$(basename "$(dirname "$f")")_$(basename "$f" .c).o"
        $cc "${cflags[@]}" -c "$f" -o "$o" || {
            echo "[ft8_lib] WARNING: compiling $f failed, building without FT8 / FT4"
            return 1
        }
    done
    (cd "$bdir/obj" && ar rcs ../libft8.a *.o) || return 1
    echo "$stamp" > "$bdir/stamp"
    FT8LIB_LIB="$bdir/libft8.a"
    return 0
}

if _ft8lib_main; then
    FT8LIB_OK=1
    FT8LIB_FLAGS="-DENABLE_FT8LIB -I$(pwd)/deps/ft8_lib"
    echo "[ft8_lib] ready: $FT8LIB_LIB"
else
    FT8LIB_LIB=""
fi
unset -f _ft8lib_main
