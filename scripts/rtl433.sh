#!/bin/bash
# =====================================================================
#  rtl_433 (ISM band decoders) - download + build into a local cache
#
#  Usage (from the repository root, inside a build script):
#      . scripts/rtl433.sh
#  Afterwards, if the library is ready:
#      RTL433_OK=1
#      RTL433_FLAGS="-DENABLE_RTL433"     (pass to g++ when compiling main.cpp)
#      RTL433_LIB=".../librtl433.a -l..." (pass to g++ when linking, after the
#                                          sources; includes the system libs it needs)
#  On any failure (no network, compile error) RTL433_OK=0 and the flags are
#  empty: OrbitSDR is then built without the ISM decoder.
#
#  The rtl_433 sources are NOT part of this repository. A pinned commit is
#  downloaded once into .cache/ (ignored by git) and compiled once per
#  compiler; later builds reuse the cached library. Only the small bridge
#  (src/modules/ISM/rtl433_bridge.c) is recompiled when it changes.
#
#  Environment:
#      ORBITSDR_CACHE   cache folder (default: <repo>/.cache)
#      CC               C compiler (default: gcc, or cc if gcc is missing)
#      RTL433_DISABLE=1 skip rtl_433 completely
# =====================================================================

# Pinned rtl_433 version (master, needs r_flow.c with push_sdr_flow()).
# Change it here to update; the old cache entry simply stays unused.
RTL433_REF="0c85f7ad3566ff6aefd8aedad569d05023c0c70c"

RTL433_OK=0
RTL433_FLAGS=""
RTL433_LIB=""

_rtl433_main() {
    if [ "${RTL433_DISABLE:-0}" = "1" ]; then
        echo "[rtl_433] disabled (RTL433_DISABLE=1)"
        return 1
    fi

    local repo cache cc
    repo="$(pwd)"
    cache="${ORBITSDR_CACHE:-$repo/.cache}"
    cc="${CC:-}"
    if [ -z "$cc" ]; then
        if command -v gcc >/dev/null 2>&1; then cc=gcc; else cc=cc; fi
    fi

    local short="${RTL433_REF:0:12}"
    local src="$cache/rtl_433-$short"
    local triple
    triple="$($cc -dumpmachine 2>/dev/null || echo unknown)"
    local bdir="$cache/rtl_433-$short-build-$triple"
    local bridge_src="$repo/src/modules/ISM/rtl433_bridge.c"

    mkdir -p "$cache" || return 1

    # --- 1. download the sources once ---
    if [ ! -f "$src/include/r_flow.h" ]; then
        echo "[rtl_433] downloading sources ($short) into $cache ..."
        local tmp="$cache/rtl_433-$short.tmp"
        rm -rf "$tmp" "$cache/rtl_433-$short.tar.gz"
        mkdir -p "$tmp"
        local url="https://github.com/merbanan/rtl_433/archive/$RTL433_REF.tar.gz"
        if { curl -sSLf -o "$cache/rtl_433-$short.tar.gz" "$url" || wget -q -O "$cache/rtl_433-$short.tar.gz" "$url"; } \
            && tar -xzf "$cache/rtl_433-$short.tar.gz" -C "$tmp"; then
            rm -rf "$src"
            mv "$tmp"/rtl_433-* "$src"
        elif command -v git >/dev/null 2>&1; then
            echo "[rtl_433] archive download failed, trying git ..."
            rm -rf "$tmp" && mkdir -p "$tmp"
            git -C "$tmp" init -q && git -C "$tmp" remote add origin https://github.com/merbanan/rtl_433.git \
                && git -C "$tmp" fetch -q --depth 1 origin "$RTL433_REF" && git -C "$tmp" checkout -q FETCH_HEAD \
                && rm -rf "$src" && mv "$tmp" "$src"
        fi
        rm -rf "$tmp" "$cache/rtl_433-$short.tar.gz"
        if [ ! -f "$src/include/r_flow.h" ]; then
            echo "[rtl_433] WARNING: download failed, building without the ISM decoder"
            return 1
        fi
    fi

    # --- 2. compile the library once per compiler ---
    local cflags="-O2 -pipe -DNDEBUG -std=gnu11 -w -I$src/include"
    case "$triple" in
        *mingw*|*windows*|*cygwin*) cflags="$cflags -D__USE_MINGW_ANSI_STDIO -D_USE_MATH_DEFINES -D_WIN32_WINNT=0x0600" ;;
        *) cflags="$cflags -D_GNU_SOURCE" ;;
    esac
    cflags="$cflags ${RTL433_EXTRA_CFLAGS:-}"
    local stamp="$RTL433_REF|$cc|$cflags"

    mkdir -p "$bdir/obj" || return 1
    if [ ! -f "$bdir/librtl433_core.a" ] || [ "$(cat "$bdir/stamp" 2>/dev/null)" != "$stamp" ]; then
        echo "[rtl_433] compiling the decoder library (only on the first build) ..."
        rm -f "$bdir/obj/"*.o "$bdir/librtl433_core.a"
        local list="$bdir/sources.txt"
        : > "$list"
        for f in "$src"/src/*.c "$src"/src/devices/*.c; do
            case "$(basename "$f")" in
                rtl_433.c) continue ;;   # the command line program (main)
            esac
            echo "$f" >> "$list"
        done
        local jobs
        jobs="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
        # object names carry the folder so devices/foo.c and foo.c never clash
        export _R433_CC="$cc" _R433_CFLAGS="$cflags" _R433_OBJ="$bdir/obj"
        if ! tr '\n' '\0' < "$list" | xargs -0 -n 1 -P "$jobs" sh -c '
                f="$1"; d="$(basename "$(dirname "$f")")"; o="$_R433_OBJ/${d}_$(basename "$f" .c).o"
                $_R433_CC $_R433_CFLAGS -c "$f" -o "$o" || { echo "[rtl_433] failed: $f"; exit 255; }' sh; then
            echo "[rtl_433] WARNING: compiling rtl_433 failed, building without the ISM decoder"
            return 1
        fi
        # short relative names: the full paths of ~340 objects are too long for Windows
        (cd "$bdir/obj" && ar rcs ../librtl433_core.a *.o) || return 1
        echo "$stamp" > "$bdir/stamp"
    fi

    # --- 3. the bridge (our code) + final library, rebuilt when the bridge changes ---
    if [ ! -f "$bdir/librtl433.a" ] || [ "$bridge_src" -nt "$bdir/librtl433.a" ] \
            || [ "$bdir/librtl433_core.a" -nt "$bdir/librtl433.a" ]; then
        # -I src: the bridge includes a quiet copy of src/pulse_analyzer.c
        $cc $cflags -I"$src/src" -c "$bridge_src" -o "$bdir/rtl433_bridge.o" || {
            echo "[rtl_433] WARNING: compiling the bridge failed, building without the ISM decoder"
            return 1
        }
        cp "$bdir/librtl433_core.a" "$bdir/librtl433.a.tmp" \
            && ar rs "$bdir/librtl433.a.tmp" "$bdir/rtl433_bridge.o" >/dev/null \
            && mv "$bdir/librtl433.a.tmp" "$bdir/librtl433.a" || return 1
    fi

    # the library + the system libraries it needs (sockets for the unused network outputs)
    case "$triple" in
        *mingw*|*windows*|*cygwin*) RTL433_LIB="$bdir/librtl433.a -lws2_32 -lnetapi32" ;;
        *) RTL433_LIB="$bdir/librtl433.a -lm" ;;
    esac
    return 0
}

if _rtl433_main; then
    RTL433_OK=1
    RTL433_FLAGS="-DENABLE_RTL433"
    echo "[rtl_433] ready: $RTL433_LIB"
fi
unset -f _rtl433_main
