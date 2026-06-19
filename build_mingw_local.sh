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
    curl

# 2. Download and prepare SFML 3.0 for MinGW
if [ ! -d "sfml" ]; then
    echo "------------------------------------------"
    echo "Downloading SFML 3.0.0 (MinGW)..."
    curl -L -s -o sfml.zip https://github.com/SFML/SFML/releases/download/3.0.0/SFML-3.0.0-windows-gcc-14.2.0-mingw-64-bit.zip
    unzip -q sfml.zip
    mv SFML-3.0.0 sfml
    rm sfml.zip
fi

# --- NEW: Define and create the build directory ---
BUILD_DIR="build"
mkdir -p "$BUILD_DIR"

# 3. Path configuration
SRC_DIR="src"
OUT_FILE="$BUILD_DIR/orbitsdr.exe"  # Output binary will be placed in the build directory
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

# Copy SFML DLLs
cp "$SFML_PATH/bin/"*.dll "$BUILD_DIR/"

# Copy SDRPlay DLL
if [ -f "$SDR_LIB/sdrplay_api.dll" ]; then
    cp "$SDR_LIB/sdrplay_api.dll" "$BUILD_DIR/"
else
    echo "ERROR: sdrplay_api.dll not found in $SDR_LIB"
    exit 1
fi

# Copy system dependencies (including rtl-sdr) directly to the build folder
echo "Searching for MinGW system DLL files..."
for dll in $(ldd "$OUT_FILE" | grep "/ucrt64" | awk '{print $3}'); do
    echo "Copying to build: $(basename "$dll")"
    cp "$dll" "$BUILD_DIR/"
done

echo "------------------------------------------"
echo "DONE! The complete application is located in the folder: /$BUILD_DIR"
echo "------------------------------------------"