#!/bin/bash
# =====================================================================
#  TETRA speech codec (ACELP, ETSI EN 300 395-2) - download, check, patch
#  and build the DECODER part once into .cache/.
#
#  The codec is NOT part of the OrbitSDR repository: like osmo-tetra, the
#  build fetches ETSI's own reference source (en_30039502v010301p0.zip)
#  from etsi.org, verifies its MD5, and compiles it locally. Nothing of the
#  ETSI code is committed. Whether a build that contains it may be
#  redistributed is up to whoever distributes it - check ETSI's terms.
#
#  Usage (from the repository root, inside a build script):
#      . scripts/tetracodec.sh
#  Afterwards, if the library is ready:
#      TETRACODEC_OK=1
#      TETRACODEC_FLAGS="-DENABLE_TETRA_CODEC"   (compile)
#      TETRACODEC_LIB=".../libtetracodec.a"      (link)
#  Without network access, or on any error, TETRACODEC_OK=0: OrbitSDR is
#  then built without TETRA speech (all metadata still works).
#
#  Fixes applied to the reference source (the same as osmo-tetra's
#  etsi_codec-patches): Word16 / Word32 as int16_t / int32_t (the original
#  `long` is 64 bit on Linux / macOS), and round() renamed etsi_round()
#  (it clashes with the C library).
#
#  Environment:
#      ORBITSDR_NO_TETRA_CODEC=1  leave TETRA speech out
#      ORBITSDR_NO_TETRA=1 / ORBITSDR_NO_DIGITAL=1  leave TETRA out entirely
#      ORBITSDR_CACHE             cache folder (default: <repo>/.cache)
#      CC                         compiler (default: gcc)
# =====================================================================

TETRACODEC_OK=0
TETRACODEC_FLAGS=""
TETRACODEC_LIB=""

_tetracodec_main() {
    local repo cache cc triple zipdir zip url md5exp md5 src bdir
    repo="$(pwd)"
    cache="${ORBITSDR_CACHE:-$repo/.cache}"
    cc="${CC:-}"
    if [ -z "$cc" ]; then if command -v gcc >/dev/null 2>&1; then cc=gcc; else cc=cc; fi; fi
    for v in ORBITSDR_NO_DIGITAL ORBITSDR_NO_TETRA ORBITSDR_NO_TETRA_CODEC; do
        if [ "${!v:-0}" = "1" ]; then echo "[tetracodec] TETRA speech left out ($v=1)"; return 1; fi
    done

    url="https://www.etsi.org/deliver/etsi_en/300300_300399/30039502/01.03.01_60/en_30039502v010301p0.zip"
    md5exp="a8115fe68ef8f8cc466f4192572a1e3e"
    zipdir="$cache/etsi"
    zip="$zipdir/en_30039502v010301p0.zip"
    mkdir -p "$zipdir" || return 1

    _tc_md5() {
        if command -v md5sum >/dev/null 2>&1; then md5sum "$1" | awk '{print $1}'
        elif command -v md5 >/dev/null 2>&1; then md5 -q "$1"
        else echo "no-md5-tool"; fi
    }

    if [ ! -f "$zip" ] || [ "$(_tc_md5 "$zip")" != "$md5exp" ]; then
        echo "[tetracodec] downloading the ETSI TETRA codec reference source ..."
        rm -f "$zip"
        if command -v curl >/dev/null 2>&1; then
            curl -fsSL -A "Mozilla/5.0 (compatible; build script)" -o "$zip.part" "$url" || { rm -f "$zip.part"; }
        elif command -v wget >/dev/null 2>&1; then
            wget -q -U "Mozilla/5.0 (compatible; build script)" -O "$zip.part" "$url" || { rm -f "$zip.part"; }
        fi
        if [ ! -f "$zip.part" ]; then
            echo "[tetracodec] WARNING: download failed (offline?), building without TETRA speech"; return 1
        fi
        mv "$zip.part" "$zip"
    fi
    md5="$(_tc_md5 "$zip")"
    if [ "$md5" != "$md5exp" ]; then
        echo "[tetracodec] WARNING: checksum mismatch ($md5), building without TETRA speech"; rm -f "$zip"; return 1
    fi

    triple="$($cc -dumpmachine 2>/dev/null || echo unknown)"
    bdir="$cache/tetracodec-build-$triple"
    src="$bdir/src/c-code"
    local cflags=(-O2 -pipe -std=gnu99 -w "-I$src")
    local stamp="$cc|${cflags[*]}|$md5|v1"
    if [ -f "$bdir/libtetracodec.a" ] && [ "$(cat "$bdir/stamp" 2>/dev/null)" = "$stamp" ]; then
        TETRACODEC_LIB="$bdir/libtetracodec.a"; return 0
    fi

    echo "[tetracodec] building the TETRA speech decoder ..."
    rm -rf "$bdir"; mkdir -p "$bdir/src" "$bdir/obj" || return 1
    if ! command -v unzip >/dev/null 2>&1; then
        echo "[tetracodec] WARNING: unzip not found, building without TETRA speech"; return 1
    fi
    (cd "$bdir/src" && unzip -q -L -o "$zip" 'c-code/*' 2>/dev/null || unzip -q -L -o "$zip") || return 1
    [ -f "$src/sdec_tet.c" ] || { echo "[tetracodec] WARNING: unexpected zip layout"; return 1; }

    # osmo-tetra's fix_64bit + round_private, done with sed
    local h
    for h in "$src/source.h" "$src/channel.h"; do
        sed -i.bak -e 's/^typedef[[:space:]]*short[[:space:]]*Word16;/typedef int16_t Word16;/' \
                   -e 's/^typedef[[:space:]]*long[[:space:]]*Word32;/typedef int32_t Word32;/' \
                   -e 's/^#define TYPEDEF_H/#define TYPEDEF_H\
#include <stdint.h>/' "$h" || return 1
    done
    local f
    for f in "$src"/*.c "$src"/*.h; do
        sed -i.bak -E 's/(^|[^A-Za-z0-9_])round\(/\1etsi_round(/g' "$f" || return 1
    done
    rm -f "$src"/*.bak

    # decoder only: channel decoder + speech decoder (no encoders, no main programs)
    local files=(cdec_tet.c sub_cd.c sdec_tet.c sub_sc_d.c sub_dsp.c fbas_tet.c fexp_tet.c fmat_tet.c tetra_op.c)
    local fail=0
    for f in "${files[@]}"; do
        $cc "${cflags[@]}" -c "$src/$f" -o "$bdir/obj/${f%.c}.o" || fail=1
    done
    if [ "$fail" != "0" ]; then
        echo "[tetracodec] WARNING: compiling the codec failed, building without TETRA speech"; return 1
    fi
    (cd "$bdir/obj" && ar rcs ../libtetracodec.a *.o) || return 1
    echo "$stamp" > "$bdir/stamp"
    TETRACODEC_LIB="$bdir/libtetracodec.a"
    return 0
}

if _tetracodec_main; then
    TETRACODEC_OK=1
    TETRACODEC_FLAGS="-DENABLE_TETRA_CODEC"
    echo "[tetracodec] ready: $TETRACODEC_LIB"
else
    TETRACODEC_LIB=""
fi
unset -f _tetracodec_main _tc_md5 2>/dev/null
