#!/bin/bash

echo "------------------------------------------"
echo "Lokalna kompilacja OrbitSDR (MinGW64 / MSYS2 UCRT64)"
echo "------------------------------------------"

# 1. Instalacja wymaganych narzędzi i bibliotek w MSYS2
echo "Sprawdzanie i instalowanie pakietow systemowych..."
pacman -S --needed --noconfirm \
    mingw-w64-ucrt-x86_64-gcc \
    mingw-w64-ucrt-x86_64-rtl-sdr \
    mingw-w64-ucrt-x86_64-make \
    unzip \
    curl

# 2. Pobranie i przygotowanie SFML 3.0 dla MinGW
if [ ! -d "sfml" ]; then
    echo "------------------------------------------"
    echo "Pobieranie SFML 3.0.0 (MinGW)..."
    curl -L -s -o sfml.zip https://github.com/SFML/SFML/releases/download/3.0.0/SFML-3.0.0-windows-gcc-14.2.0-mingw-64-bit.zip
    unzip -q sfml.zip
    mv SFML-3.0.0 sfml
    rm sfml.zip
fi

# --- NOWOŚĆ: Definicja i tworzenie folderu build ---
BUILD_DIR="build"
mkdir -p "$BUILD_DIR"

# 3. Konfiguracja ścieżek
SRC_DIR="src"
OUT_FILE="$BUILD_DIR/orbitsdr.exe"  # Wyjściowy plik trafi do folderu build
SFML_PATH="$(pwd)/sfml"
SDR_INC="$(pwd)/deps/sdrplay/include"
SDR_LIB="$(pwd)/deps/sdrplay/lib/windows"

if [ ! -f "$SDR_LIB/sdrplay_api.lib" ]; then
    echo "BLAD: sdrplay_api.lib nie znaleziono w $SDR_LIB"
    exit 1
fi

# 4. Kompilacja
echo "------------------------------------------"
echo "Kompilacja OrbitSDR do folderu /$BUILD_DIR..."
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
    echo "BLAD KOMPILACJI"
    exit 1
fi

# 5. Kopiowanie bibliotek DLL do folderu build
echo "------------------------------------------"
echo "Kopiowanie plikow DLL do folderu /$BUILD_DIR..."
echo "------------------------------------------"

# Kopiowanie DLL z SFML
cp "$SFML_PATH/bin/"*.dll "$BUILD_DIR/"

# Kopiowanie DLL ze SDRPlay
if [ -f "$SDR_LIB/sdrplay_api.dll" ]; then
    cp "$SDR_LIB/sdrplay_api.dll" "$BUILD_DIR/"
else
    echo "BLAD: sdrplay_api.dll nie znaleziono w $SDR_LIB"
    exit 1
fi

# Kopiowanie zależnosci systemowych (w tym rtl-sdr) bezpośrednio do folderu build
echo "Wyszukiwanie systemowych plikow DLL z MinGW..."
for dll in $(ldd "$OUT_FILE" | grep "/ucrt64" | awk '{print $3}'); do
    echo "Kopiowanie do build: $(basename "$dll")"
    cp "$dll" "$BUILD_DIR/"
done

echo "------------------------------------------"
echo "GOTOWE! Kompletna aplikacja znajduje sie w folderze: /$BUILD_DIR"
echo "------------------------------------------"