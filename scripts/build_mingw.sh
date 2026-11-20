#!/bin/bash

# Always work from the repository root (this script lives in scripts/)
cd "$(dirname "$0")/.." || exit 1

# --- CONFIGURATION ---
SRC_DIR="src"
OUT_FILE="orbitsdr.exe"

# Paths (assuming we are running the script from the main repository directory).
# SFML_PATH / RTLSDR_PATH may be given from outside (e.g. SFML from the MSYS2 package or built
# from source for Windows on ARM).
SFML_PATH="${SFML_PATH:-$(pwd)/sfml}"
SDR_INC="$(pwd)/deps/sdrplay/include"
SDR_LIB="$(pwd)/deps/sdrplay/lib/windows"

# --- Target: x64 (MSYS2 UCRT64, GCC) or ARM64 (MSYS2 CLANGARM64, Clang) ---
# Decided by the MSYS2 environment; ORBITSDR_ARCH=x64 / arm64 overrides it.
ARCH="${ORBITSDR_ARCH:-}"
if [ -z "$ARCH" ]; then
    case "${MSYSTEM:-}" in
        CLANGARM64) ARCH=arm64 ;;
        *) case "$(${CC:-gcc} -dumpmachine 2>/dev/null)" in aarch64*) ARCH=arm64 ;; *) ARCH=x64 ;; esac ;;
    esac
fi
PREFIX="${MINGW_PREFIX:-/ucrt64}"   # where the toolchain's own DLLs are (/ucrt64, /clangarm64)
if [ "$ARCH" = "arm64" ]; then
    # Clang toolchain: the dependency scripts (rtl_433, ft8_lib, DSDcc, TETRA) read CC / CXX
    export CC="${CC:-clang}" CXX="${CXX:-clang++}"
    STATIC_RT="-static-libstdc++"
    # the dependency scripts call "ar" (LLVM's is llvm-ar when no binutils name is installed)
    if ! command -v ar >/dev/null 2>&1 && command -v llvm-ar >/dev/null 2>&1; then ar() { llvm-ar "$@"; }; fi
else
    export CC="${CC:-gcc}" CXX="${CXX:-g++}"
    STATIC_RT="-static-libstdc++ -static-libgcc"
fi
WINDRES="${WINDRES:-windres}"
command -v "$WINDRES" >/dev/null 2>&1 || WINDRES=llvm-windres
RTL_FLAGS=""
if [ -n "${RTLSDR_PATH:-}" ]; then RTL_FLAGS="-I$RTLSDR_PATH/include"; RTL_LIBDIR="-L$RTLSDR_PATH/lib"; fi
# SFML Audio is not used; linked only when it is there (the prebuilt x64 SFML has it)
SFML_AUDIO=""
if ls "$SFML_PATH/lib/"libsfml-audio* >/dev/null 2>&1; then SFML_AUDIO="-lsfml-audio"; fi

echo "------------------------------------------"
echo "Compiling OrbitSDR (Windows $ARCH, $CXX)..."
echo "Working directory: $(pwd)"
echo "SFML Path: $SFML_PATH"
echo "SDR Path:  $SDR_LIB"
echo "------------------------------------------"

# The SDRplay API is loaded at run time (installed API first, the bundled
# sdrplay_api.dll as a fallback), so there is nothing to link against.

# --- rtl_433 (ISM decoder): downloaded and compiled once into .cache/ ---
. scripts/rtl433.sh
# --- ft8_lib (FT8 / FT4 decoder): vendored in deps/ft8_lib, compiled once into .cache/ ---
. scripts/ft8lib.sh
# --- DSDcc + mbelib (digital voice: DMR, dPMR, D-STAR, YSF, NXDN): vendored in deps/, compiled once into .cache/ ---
. scripts/dsdcc.sh
# --- TETRA metadata decoder (tetra-kit port): vendored in deps/tetra-kit, compiled once into .cache/ ---
. scripts/tetra.sh
# --- TETRA speech codec (ETSI EN 300 395-2): downloaded from etsi.org, checked and built once into .cache/ (not in the repo) ---
. scripts/tetracodec.sh

# --- COMPILATION ---
"$WINDRES" -I assets assets/resource.rc -O coff -o resource.res

"$CXX" -std=c++17 -O3 \
    -DENABLE_SDRPLAY \
    -D_USE_MATH_DEFINES \
    -Wno-macro-redefined \
    $RTL433_FLAGS \
    $FT8LIB_FLAGS \
    $DSDCC_FLAGS \
    $TETRA_FLAGS \
    $TETRACODEC_FLAGS \
    -I"$SFML_PATH/include" \
    -I"$SDR_INC" \
    $RTL_FLAGS \
    "$SRC_DIR/main.cpp" \
    resource.res \
    $RTL433_LIB \
    $FT8LIB_LIB \
    $DSDCC_LIB \
    $TETRA_LIB \
    $TETRACODEC_LIB \
    -o "$OUT_FILE" \
    -L"$SFML_PATH/lib" \
    ${RTL_LIBDIR:-} \
    -lcomdlg32 -lole32 -lws2_32 -lwininet \
    -lsfml-graphics -lsfml-window -lsfml-system $SFML_AUDIO \
    -lrtlsdr \
    -lcomdlg32 -lole32 -lgdi32 -luser32 \
    $STATIC_RT

if [ $? -ne 0 ]; then
    echo "COMPILATION ERROR"
    exit 1
fi

echo "------------------------------------------"
echo "Creating package (copying DLLs)..."
echo "------------------------------------------"

# 1. Copy SFML DLLs (ARM64: only the ones the program imports, step 3; SFML_PATH may be the
#    whole MSYS2 prefix there)
if [ "$ARCH" != "arm64" ]; then cp "$SFML_PATH/bin/"*.dll .; fi

# 2. Copy sdrplay_api.dll (fallback when the SDRplay API isn't installed or can't be used).
#    The bundled DLL is x64: an ARM64 program cannot load it, so on ARM64 only an installed
#    ARM64 SDRplay API (if SDRplay offers one) is used.
if [ "$ARCH" = "arm64" ]; then
    echo "ARM64: sdrplay_api.dll (x64) not bundled"
elif [ -f "$SDR_LIB/sdrplay_api.dll" ]; then
    echo "Copying sdrplay_api.dll..."
    cp "$SDR_LIB/sdrplay_api.dll" .
else
    echo "ERROR: sdrplay_api.dll not found in $SDR_LIB"
    exit 1
fi

# 3. Find and copy MSYS2 system DLLs (safely copies ALL required system files)
echo "Searching for system dependencies (MinGW)..."
if [ "$ARCH" = "arm64" ]; then
    # (ldd runs the program; under the x64 MSYS2 runtime on ARM64 Windows it may not see an ARM64
    # process.) The imports are read from the files instead, recursively.
    OBJDUMP="$(command -v llvm-objdump || command -v objdump)"
    copy_deps() {
        local f="$1" name
        for name in $("$OBJDUMP" -p "$f" 2>/dev/null | awk '/DLL Name:/ {print $3}'); do
            [ -f "./$name" ] && continue
            for dir in "$PREFIX/bin" "$SFML_PATH/bin" ${RTLSDR_PATH:+"$RTLSDR_PATH/bin"}; do
                if [ -f "$dir/$name" ]; then echo "Copying: $dir/$name"; cp "$dir/$name" .; copy_deps "./$name"; break; fi
            done
        done
    }
    copy_deps "$OUT_FILE"
else
    for dll in $(ldd "$OUT_FILE" | grep "$PREFIX" | awk '{print $3}'); do
        echo "Copying: $dll"
        cp "$dll" .
    done
fi

# 4. OVERRIDE: Download latest RTL-SDR drivers for V4 support (x64 builds only: these DLLs are x64)
echo "------------------------------------------"
if [ "$ARCH" = "arm64" ]; then
    echo "ARM64: keeping the librtlsdr.dll built for ARM64"
else
echo "Attempting to download latest RTL-SDR V4 drivers from Osmocom..."
RTL_ZIP_URL="https://ftp.osmocom.org/binaries/windows/rtl-sdr/rtl-sdr-64bit-20260614.zip"
RTL_ZIP_DIR="rtl-sdr-64bit-20260614"

if curl -sSL -f -o rtlsdr_v4.zip "$RTL_ZIP_URL" || wget -q -O rtlsdr_v4.zip "$RTL_ZIP_URL"; then
    echo "Download successful. Injecting ONLY librtlsdr.dll..."
    unzip -q -o rtlsdr_v4.zip
    
    # Replace ONLY this one library, keep the rest from the system!
    cp "$RTL_ZIP_DIR/librtlsdr.dll" .
    
    # Cleanup
    rm -rf "$RTL_ZIP_DIR" rtlsdr_v4.zip
    echo "librtlsdr.dll (V4 support) successfully applied."
else
    echo "WARNING: Could not download RTL-SDR V4 drivers. Falling back to older system DLL."
    rm -f rtlsdr_v4.zip
fi
fi


echo "------------------------------------------"
echo "DONE! The orbitsdr.exe file and libraries are ready."
echo "------------------------------------------"