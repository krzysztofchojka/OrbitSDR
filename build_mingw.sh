#!/bin/bash

# --- KONFIGURACJA ---
SRC_DIR="src"
OUT_FILE="orbitsdr.exe"
# Zakładamy, że SFML jest rozpakowany w folderze 'sfml' w bieżącym katalogu
SFML_PATH="$(pwd)/sfml"

echo "------------------------------------------"
echo "Kompilacja OrbitSDR (MinGW64 / GCC)..."
echo "------------------------------------------"

# FIX:
# 1. -D_USE_MATH_DEFINES -> Naprawia błąd "M_PI was not declared".
# 2. -Wno-macro-redefined -> Wycisza ostrzeżenie o NOMINMAX.
# 3. Dodano flagi systemowe Windows: -lcomdlg32 -lole32 (dla NativeDialogs.h)

g++ -std=c++17 -O3 -D_USE_MATH_DEFINES -Wno-macro-redefined \
    -I"$SFML_PATH/include" \
    "$SRC_DIR/main.cpp" \
    -o "$OUT_FILE" \
    -L"$SFML_PATH/lib" \
    -lsfml-graphics -lsfml-window -lsfml-system -lsfml-audio \
    -lrtlsdr -lcomdlg32 -lole32 \
    -static-libstdc++ -static-libgcc

if [ $? -ne 0 ]; then
    echo "BŁĄD KOMPILACJI"
    exit 1
fi

echo "------------------------------------------"
echo "Tworzenie paczki (kopiowanie DLL)..."
echo "------------------------------------------"

# 1. Kopiuj DLL-ki SFML
cp "$SFML_PATH/bin/"*.dll .

# 2. Znajdź i skopiuj DLL-ki systemowe (librtlsdr, libusb, itp.)
# Używamy sprytnego polecenia 'ldd', żeby znaleźć wszystko, czego exe potrzebuje
for dll in $(ldd "$OUT_FILE" | grep "/ucrt64" | awk '{print $3}'); do
    echo "Kopiowanie: $dll"
    cp "$dll" .
done

echo "------------------------------------------"
echo "GOTOWE! Pliki w bieżącym katalogu."
echo "------------------------------------------"