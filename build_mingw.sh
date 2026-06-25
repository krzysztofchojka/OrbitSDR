#!/bin/bash

# --- CONFIGURATION ---
SRC_DIR="src"
OUT_FILE="orbitsdr.exe"

# Paths (assuming we are running the script from the main repository directory)
SFML_PATH="$(pwd)/sfml"
SDR_INC="$(pwd)/deps/sdrplay/include"
SDR_LIB="$(pwd)/deps/sdrplay/lib/windows"

echo "------------------------------------------"
echo "Compiling OrbitSDR (MinGW64 / GCC)..."
echo "Working directory: $(pwd)"
echo "SFML Path: $SFML_PATH"
echo "SDR Path:  $SDR_LIB"
echo "------------------------------------------"

if [ ! -f "$SDR_LIB/sdrplay_api.lib" ]; then
    echo "ERROR: sdrplay_api.lib not found in $SDR_LIB"
    echo "Ensure that API/x64 files are in deps/sdrplay/lib/windows/"
    exit 1
fi

# --- COMPILATION ---
windres resource.rc -O coff -o resource.res

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

# 2. Copy sdrplay_api.dll
if [ -f "$SDR_LIB/sdrplay_api.dll" ]; then
    echo "Copying sdrplay_api.dll..."
    cp "$SDR_LIB/sdrplay_api.dll" .
else
    echo "ERROR: sdrplay_api.dll not found in $SDR_LIB"
    exit 1
fi

# 3. Find and copy MSYS2 system DLLs (safely copies ALL required system files)
echo "Searching for system dependencies (MinGW)..."
for dll in $(ldd "$OUT_FILE" | grep "/ucrt64" | awk '{print $3}'); do
    echo "Copying: $dll"
    cp "$dll" .
done

# 4. OVERRIDE: Download latest RTL-SDR drivers for V4 support
echo "------------------------------------------"
echo "Attempting to download latest RTL-SDR V4 drivers from Osmocom..."
RTL_ZIP_URL="https://ftp.osmocom.org/binaries/windows/rtl-sdr/rtl-sdr-64bit-20260614.zip"
RTL_ZIP_DIR="rtl-sdr-64bit-20260614"

if curl -sSL -f -o rtlsdr_v4.zip "$RTL_ZIP_URL" || wget -q -O rtlsdr_v4.zip "$RTL_ZIP_URL"; then
    echo "Download successful. Injecting ONLY librtlsdr.dll..."
    unzip -q -o rtlsdr_v4.zip
    
    # Podmieniamy TYLKO jedną bibliotekę, resztę zostawiamy systemową!
    cp "$RTL_ZIP_DIR/librtlsdr.dll" .
    
    # Cleanup
    rm -rf "$RTL_ZIP_DIR" rtlsdr_v4.zip
    echo "librtlsdr.dll (V4 support) successfully applied."
else
    echo "WARNING: Could not download RTL-SDR V4 drivers. Falling back to older system DLL."
    rm -f rtlsdr_v4.zip
fi

echo "Copying bandplan.json..."
cp bandplan.json . 2>/dev/null || echo "WARNING: bandplan.json not found!"

echo "------------------------------------------"
echo "DONE! The orbitsdr.exe file and libraries are ready."
echo "------------------------------------------"