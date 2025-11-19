#!/bin/bash

# --- CONFIG ---
SRC_DIR="src"
OUT_FILE="orbitsdr"
CXX="g++"

# --- OS DETECTION ---
if [[ "$OSTYPE" == "darwin"* ]]; then
    echo "Detected: macOS"
    # Homebrew paths for Apple Silicon (M1/M2)
    INCLUDES="-I/opt/homebrew/include"
    LIBS="-L/opt/homebrew/lib -lsfml-graphics -lsfml-window -lsfml-system -lrtlsdr"
    EXTRAS="-framework CoreAudio -framework AudioToolbox -framework AudioUnit"
    # SDRPlay on macOS usually installs to /usr/local
    SDRPLAY_INC="-I/usr/local/include"
    SDRPLAY_LIB="-L/usr/local/lib -lsdrplay_api"
else
    echo "Detected: Linux"
    INCLUDES=""
    LIBS="-lsfml-graphics -lsfml-window -lsfml-system -ldl -lpthread -lm -lrtlsdr"
    EXTRAS=""
    SDRPLAY_INC="-I/usr/local/include"
    SDRPLAY_LIB="-lsdrplay_api"
fi

# --- SDRPLAY PROMPT ---
echo "------------------------------------------"
echo "Do you want to enable SDRPlay support (RSPdx/1A/2)?"
echo "Requires API 3.x driver installed."
read -p "Enable SDRPlay? [y/N]: " response

SDR_FLAGS=""
if [[ "$response" =~ ^([yY][eE][sS]|[yY])+$ ]]; then
    echo ">> Enabling SDRPlay..."
    SDR_FLAGS="-DENABLE_SDRPLAY"
    INCLUDES="$INCLUDES $SDRPLAY_INC"
    LIBS="$LIBS $SDRPLAY_LIB"
else
    echo ">> SDRPlay disabled (Dummy Mode)."
fi

echo "------------------------------------------"
echo "Compiling..."

$CXX -std=c++17 -O3 $SDR_FLAGS $INCLUDES $SRC_DIR/main.cpp -o $OUT_FILE $LIBS $EXTRAS

if [ $? -eq 0 ]; then
    echo "------------------------------------------"
    echo "SUCCESS! Run ./$OUT_FILE"
else
    echo "------------------------------------------"
    echo "COMPILATION FAILED."
    echo "If you selected SDRPlay, ensure 'sdrplay_api.h' exists"
    echo "in /usr/local/include or /opt/homebrew/include."
fi