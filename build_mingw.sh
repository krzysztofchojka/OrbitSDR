#!/bin/bash

# --- CONFIGURATION ---
SRC_DIR="src"
OUT_FILE="orbitsdr.exe"

# Paths (assuming we are running the script from the main repository directory)
# $(pwd) returns the full path, which helps the compiler
SFML_PATH="$(pwd)/sfml"
SDR_INC="$(pwd)/deps/sdrplay/include"
SDR_LIB="$(pwd)/deps/sdrplay/lib/windows"

echo "------------------------------------------"
echo "Compiling OrbitSDR (MinGW64 / GCC)..."
echo "Working directory: $(pwd)"
echo "SFML Path: $SFML_PATH"
echo "SDR Path:  $SDR_LIB"
echo "------------------------------------------"

# Check if libraries exist to avoid weird linker errors
if [ ! -f "$SDR_LIB/sdrplay_api.lib" ]; then
    echo "ERROR: sdrplay_api.lib not found in $SDR_LIB"
    echo "Ensure that API/x64 files are in deps/sdrplay/lib/windows/"
    exit 1
fi

# --- COMPILATION ---
# -DENABLE_SDRPLAY       -> Enables SDRplay handling code in C++
# -lsdrplay_api          -> Links with the .lib file (MinGW finds it thanks to the -L flag)
# -lcomdlg32 -lole32     -> Required for file dialog windows on Windows
windres resource.rc -O coff -o resource.res

# Dodano 'resource.res \' poniżej
g++ -std=c++17 -O3 \
    -DENABLE_SDRPLAY \
    -D_USE_MATH_DEFINES \
    -Wno-macro-redefined \
    -I"$SFML_PATH/include" \
    -I"$SDR_INC" \
    "$SRC_DIR/main.cpp" \
    resource.res \
    -o "$OUT_FILE" \
    -L"$SFML_PATH/lib" \
    -L"$SDR_LIB" \
    -lsfml-graphics -lsfml-window -lsfml-system -lsfml-audio \
    -lrtlsdr -lsdrplay_api \
    -lcomdlg32 -lole32 \
    -static-libstdc++ -static-libgcc

if [ $? -ne 0 ]; then
    echo "COMPILATION ERROR"
    exit 1
fi

echo "------------------------------------------"
echo "Creating package (copying DLLs)..."
echo "------------------------------------------"

# 1. Copy SFML DLLs
cp "$SFML_PATH/bin/"*.dll .

# 2. Copy sdrplay_api.dll (CRITICAL FOR EXECUTION)
# The Windows app needs this file next to the .exe to run
if [ -f "$SDR_LIB/sdrplay_api.dll" ]; then
    echo "Copying sdrplay_api.dll..."
    cp "$SDR_LIB/sdrplay_api.dll" .
else
    echo "ERROR: sdrplay_api.dll not found in $SDR_LIB"
    exit 1
fi

# 3. Find and copy MSYS2 system DLLs (librtlsdr, libusb, winpthread, etc.)
# ldd shows which system libraries are missing
echo "Searching for system dependencies (MinGW)..."
for dll in $(ldd "$OUT_FILE" | grep "/ucrt64" | awk '{print $3}'); do
    echo "Copying: $dll"
    cp "$dll" .
done

echo "------------------------------------------"
echo "DONE! The orbitsdr.exe file and libraries are ready."
echo "------------------------------------------"