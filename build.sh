#!/bin/bash

# --- CONFIG ---
SRC_DIR="src"
OUT_FILE="orbitsdr"
CXX="g++"

# --- ARGUMENT PARSING ---
# Default state is to ask the user
SDRPLAY_MODE="ask"

# Check for command line arguments
for arg in "$@"; do
  case $arg in
    --enable-sdrplay)
      SDRPLAY_MODE="yes"
      shift
      ;;
    --disable-sdrplay)
      SDRPLAY_MODE="no"
      shift
      ;;
  esac
done

# --- OS DETECTION ---
if [[ "$OSTYPE" == "darwin"* ]]; then
    echo "Detected: macOS"
    # Homebrew paths for Apple Silicon (M1/M2)
    # We allow overriding via external environment variables if set
    BASE_INCLUDES=${INCLUDES:-"-I/opt/homebrew/include"}
    BASE_LIBS=${LIBS:-"-L/opt/homebrew/lib -lsfml-graphics -lsfml-window -lsfml-system -lrtlsdr"}
    EXTRAS="-framework CoreAudio -framework AudioToolbox -framework AudioUnit"
    
    SDRPLAY_INC="-I/usr/local/include"
    SDRPLAY_LIB="-L/usr/local/lib -lsdrplay_api"
else
    echo "Detected: Linux"
    BASE_INCLUDES=${INCLUDES:-""}
    BASE_LIBS=${LIBS:-"-lsfml-graphics -lsfml-window -lsfml-system -ldl -lpthread -lm -lrtlsdr"}
    EXTRAS=""
    
    SDRPLAY_INC="-I/usr/local/include"
    SDRPLAY_LIB="-lsdrplay_api"
fi

# --- SDRPLAY LOGIC ---
echo "------------------------------------------"

if [[ "$SDRPLAY_MODE" == "ask" ]]; then
    echo "Do you want to enable SDRPlay support (RSPdx/1A/2)?"
    echo "Requires API 3.x driver installed."
    read -p "Enable SDRPlay? [y/N]: " response
    if [[ "$response" =~ ^([yY][eE][sS]|[yY])+$ ]]; then
        SDRPLAY_MODE="yes"
    else
        SDRPLAY_MODE="no"
    fi
fi

SDR_FLAGS=""
FINAL_INCLUDES="$BASE_INCLUDES"
FINAL_LIBS="$BASE_LIBS"

if [[ "$SDRPLAY_MODE" == "yes" ]]; then
    echo ">> Enabling SDRPlay..."
    SDR_FLAGS="-DENABLE_SDRPLAY"
    FINAL_INCLUDES="$FINAL_INCLUDES $SDRPLAY_INC"
    FINAL_LIBS="$FINAL_LIBS $SDRPLAY_LIB"
else
    echo ">> SDRPlay disabled (Dummy Mode)."
fi

# Append any flags passed from CI environment (CXXFLAGS / LDFLAGS)
FINAL_INCLUDES="$FINAL_INCLUDES $CXXFLAGS"
FINAL_LIBS="$FINAL_LIBS $LDFLAGS"

echo "------------------------------------------"
echo "Compiling..."
# Debug output to see exact command
echo "$CXX -std=c++17 -O3 $SDR_FLAGS $FINAL_INCLUDES $SRC_DIR/main.cpp -o $OUT_FILE $FINAL_LIBS $EXTRAS"

$CXX -std=c++17 -O3 $SDR_FLAGS $FINAL_INCLUDES $SRC_DIR/main.cpp -o $OUT_FILE $FINAL_LIBS $EXTRAS

if [ $? -eq 0 ]; then
    echo "------------------------------------------"
    echo "SUCCESS! Run ./$OUT_FILE"
else
    echo "------------------------------------------"
    echo "COMPILATION FAILED."
    if [[ "$SDRPLAY_MODE" == "yes" ]]; then
        echo "If you selected SDRPlay, ensure 'sdrplay_api.h' exists"
        echo "in /usr/local/include or /opt/homebrew/include."
    fi
    exit 1
fi