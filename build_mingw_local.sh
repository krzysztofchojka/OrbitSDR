#!/bin/bash

echo "------------------------------------------"
echo "Local OrbitSDR Compilation (MinGW64 / MSYS2 UCRT64)"
echo "------------------------------------------"

# 1. Install required tools and libraries in MSYS2
echo "Checking and installing system packages..."
pacman -S --needed --noconfirm \
    mingw-w64-ucrt-x86_64-gcc \
    mingw-w64-ucrt-x86_64-rtl-sdr \
    mingw-w64-ucrt-x86_64-make \
    unzip \
    curl \
    wget

# 2. Download and prepare SFML 3.0 for MinGW
if [ ! -d "sfml" ]; then
    echo "------------------------------------------"
    echo "Downloading SFML 3.0.0 (MinGW)..."
    curl -L -s -o sfml.zip https://github.com/SFML/SFML/releases/download/3.0.0/SFML-3.0.0-windows-gcc-14.2.0-mingw-64-bit.zip
    unzip -q sfml.zip
    mv SFML-3.0.0 sfml
    rm sfml.zip
fi

# --- Define and create the build directory ---
BUILD_DIR="build"
mkdir -p "$BUILD_DIR"

# 3. Path configuration
SRC_DIR="src"
OUT_FILE="$BUILD_DIR/orbitsdr.exe"
SFML_PATH="$(pwd)/sfml"
SDR_INC="$(pwd)/deps/sdrplay/include"
SDR_LIB="$(pwd)/deps/sdrplay/lib/windows"

if [ ! -f "$SDR_LIB/sdrplay_api.lib" ]; then
    echo "ERROR: sdrplay_api.lib not found in $SDR_LIB"
    exit 1
fi

# 4. Compilation
echo "------------------------------------------"
echo "Compiling OrbitSDR into /$BUILD_DIR folder..."
echo "------------------------------------------"
g++ -std=c++17 -O3 \
    -DENABLE_SDRPLAY \
    -D_USE_MATH_DEFINES \
    -I"$SFML_PATH/include" \
    -I"$SDR_INC" \
    "$SRC_DIR/main.cpp" \
    -o "$OUT_FILE" \
    -L"$SFML_PATH/lib" \
    -L"$SDR_LIB" \
    -lsfml-graphics -lsfml-window -lsfml-system -lsfml-audio \
    -lrtlsdr -lsdrplay_api \
    -lcomdlg32 -lole32

if [ $? -ne 0 ]; then
    echo "------------------------------------------"
    echo "COMPILATION ERROR"
    exit 1
fi

# 5. Copy DLL libraries to the build directory
echo "------------------------------------------"
echo "Copying DLL files to /$BUILD_DIR folder..."
echo "------------------------------------------"

cp "$SFML_PATH/bin/"*.dll "$BUILD_DIR/"

if [ -f "$SDR_LIB/sdrplay_api.dll" ]; then
    cp "$SDR_LIB/sdrplay_api.dll" "$BUILD_DIR/"
else
    echo "ERROR: sdrplay_api.dll not found in $SDR_LIB"
    exit 1
fi

# Bezpieczne kopiowanie WSZYSTKICH systemowych DLLi (w tym libwinpthread-1.dll)
echo "Searching for MinGW system DLL files..."
for dll in $(ldd "$OUT_FILE" | grep "/ucrt64" | awk '{print $3}'); do
    echo "Copying to build: $(basename "$dll")"
    cp "$dll" "$BUILD_DIR/"
done

# 6. OVERRIDE: Download latest RTL-SDR drivers for V4 support
echo "------------------------------------------"
echo "Attempting to download latest RTL-SDR V4 drivers from Osmocom..."
RTL_ZIP_URL="https://ftp.osmocom.org/binaries/windows/rtl-sdr/rtl-sdr-64bit-20260614.zip"
RTL_ZIP_DIR="rtl-sdr-64bit-20260614"

if curl -sSL -f -o rtlsdr_v4.zip "$RTL_ZIP_URL" || wget -q -O rtlsdr_v4.zip "$RTL_ZIP_URL"; then
    echo "Download successful. Injecting ONLY librtlsdr.dll into $BUILD_DIR..."
    unzip -q -o rtlsdr_v4.zip
    
    # Podmieniamy TYLKO jedną bibliotekę
    cp "$RTL_ZIP_DIR/librtlsdr.dll" "$BUILD_DIR/"
    
    # Cleanup
    rm -rf "$RTL_ZIP_DIR" rtlsdr_v4.zip
    echo "librtlsdr.dll (V4 support) successfully applied."
else
    echo "WARNING: Could not download RTL-SDR V4 drivers. Falling back to older system DLL."
    rm -f rtlsdr_v4.zip
fi

echo "------------------------------------------"
echo "DONE! The complete application is located in the folder: /$BUILD_DIR"
echo "------------------------------------------"