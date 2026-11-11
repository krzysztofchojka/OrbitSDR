#!/bin/bash

# --- CONFIG ---
SRC_DIR="src"
OUT_FILE="orbitsdr"
APP_NAME="OrbitSDR"
APP_BUNDLE="$APP_NAME.app"
CXX="g++"

# --- ARGUMENT PARSING ---
SDRPLAY_MODE="ask"

for arg in "$@"; do
  case $arg in
    --enable-sdrplay) SDRPLAY_MODE="yes"; shift ;;
    --disable-sdrplay) SDRPLAY_MODE="no"; shift ;;
  esac
done

# --- OS DETECTION ---
IS_MACOS=false
if [[ "$OSTYPE" == "darwin"* ]]; then
    echo "Detected: macOS"
    IS_MACOS=true
    BASE_INCLUDES=${INCLUDES:-"-I/opt/homebrew/include"}
    BASE_LIBS=${LIBS:-"-L/opt/homebrew/lib -lsfml-graphics -lsfml-window -lsfml-system -lrtlsdr"}
    EXTRAS="-framework CoreAudio -framework AudioToolbox -framework AudioUnit"
    SDRPLAY_INC="-I/usr/local/include"
    SDRPLAY_LIB=""   # loaded at run time (see src/SdrplayLoader.h)
else
    echo "Detected: Linux"
    BASE_INCLUDES=${INCLUDES:-""}
    BASE_LIBS=${LIBS:-"-lsfml-graphics -lsfml-window -lsfml-system -ldl -lpthread -lm -lrtlsdr"}
    EXTRAS=""
    SDRPLAY_INC="-I/usr/local/include"
    SDRPLAY_LIB="-ldl"   # the API is loaded at run time with dlopen (see src/SdrplayLoader.h)
fi

# --- COMPILATION ---
echo "------------------------------------------"
SDR_FLAGS=""
FINAL_INCLUDES="$BASE_INCLUDES"
FINAL_LIBS="$BASE_LIBS"

if [[ "$SDRPLAY_MODE" == "yes" ]]; then
    echo ">> Enabling SDRPlay..."
    SDR_FLAGS="-DENABLE_SDRPLAY"
    FINAL_INCLUDES="$FINAL_INCLUDES $SDRPLAY_INC"
    FINAL_LIBS="$FINAL_LIBS $SDRPLAY_LIB"
fi

FINAL_INCLUDES="$FINAL_INCLUDES $CXXFLAGS"
FINAL_LIBS="$FINAL_LIBS $LDFLAGS"

echo "Compiling binary..."
# Find all .cpp files in src/modules
MODULE_SOURCES=$(find src/modules -name "*.cpp")

echo "Compiling with modules: $MODULE_SOURCES"

$CXX -std=c++17 -O3 $SDR_FLAGS $FINAL_INCLUDES \
    $SRC_DIR/main.cpp $MODULE_SOURCES \
    -o $OUT_FILE $FINAL_LIBS $EXTRAS

if [ $? -ne 0 ]; then
    echo "COMPILATION FAILED."
    exit 1
fi
echo "Binary compiled successfully."

# --- BUNDLING & ICONS ---

if [ "$IS_MACOS" = true ]; then
    echo "------------------------------------------"
    echo "Generating macOS Bundle: $APP_BUNDLE"

    mkdir -p "$APP_BUNDLE/Contents/MacOS"
    mkdir -p "$APP_BUNDLE/Contents/Resources"
    mkdir -p "$APP_BUNDLE/Contents/Frameworks"

    cp "$OUT_FILE" "$APP_BUNDLE/Contents/MacOS/$APP_NAME"
    chmod +x "$APP_BUNDLE/Contents/MacOS/$APP_NAME"

    # --- Generate the .icns icon from icon.png ---
    if [ -f "icon.png" ]; then
        echo "Generating AppIcon.icns from icon.png..."
        
        # Also copy the PNG into Resources so SFML can load it at runtime (for the Dock)
        cp "icon.png" "$APP_BUNDLE/Contents/Resources/"

        # Create a temporary iconset folder
        ICONSET_DIR="OrbitSDR.iconset"
        mkdir -p "$ICONSET_DIR"

        # Generate the sizes macOS requires (using sips)
        sips -z 16 16     icon.png --out "${ICONSET_DIR}/icon_16x16.png" > /dev/null
        sips -z 32 32     icon.png --out "${ICONSET_DIR}/icon_16x16@2x.png" > /dev/null
        sips -z 32 32     icon.png --out "${ICONSET_DIR}/icon_32x32.png" > /dev/null
        sips -z 64 64     icon.png --out "${ICONSET_DIR}/icon_32x32@2x.png" > /dev/null
        sips -z 128 128   icon.png --out "${ICONSET_DIR}/icon_128x128.png" > /dev/null
        sips -z 256 256   icon.png --out "${ICONSET_DIR}/icon_128x128@2x.png" > /dev/null
        sips -z 256 256   icon.png --out "${ICONSET_DIR}/icon_256x256.png" > /dev/null
        sips -z 512 512   icon.png --out "${ICONSET_DIR}/icon_256x256@2x.png" > /dev/null
        sips -z 512 512   icon.png --out "${ICONSET_DIR}/icon_512x512.png" > /dev/null
        sips -z 1024 1024 icon.png --out "${ICONSET_DIR}/icon_512x512@2x.png" > /dev/null

        # Convert the iconset folder into an .icns file
        iconutil -c icns "$ICONSET_DIR" -o "$APP_BUNDLE/Contents/Resources/AppIcon.icns"
        
        # Clean up
        rm -rf "$ICONSET_DIR"
    else
        echo "WARNING: icon.png not found! App will have generic icon."
    fi

    # Copy bandplan.json into Resources
    if [ -f "bandplan.json" ]; then
        cp "bandplan.json" "$APP_BUNDLE/Contents/Resources/"
    fi

    # Fix libraries linking
    # Bundled SDRplay API: only a fallback, the installed API is used first (loaded at run time)
    if [[ "$SDRPLAY_MODE" == "yes" ]] && [ -f "/usr/local/lib/libsdrplay_api.so.3" ]; then
        cp "/usr/local/lib/libsdrplay_api.so.3" "$APP_BUNDLE/Contents/Frameworks/"
    fi

    # Create Info.plist (includes the CFBundleIconFile entry)
    cat > "$APP_BUNDLE/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>$APP_NAME</string>
    <key>CFBundleIdentifier</key>
    <string>com.orbitsdr.app</string>
    <key>CFBundleName</key>
    <string>$APP_NAME</string>
    <key>CFBundleIconFile</key>
    <string>AppIcon</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>NSHighResolutionCapable</key>
    <true/>
    <key>NSMicrophoneUsageDescription</key>
    <string>OrbitSDR requires audio access to use Line-In and Sound Card sources.</string>
</dict>
</plist>
EOF

    # Copy fonts if available
    if ls *.ttf >/dev/null 2>&1; then cp *.ttf "$APP_BUNDLE/Contents/MacOS/"; fi

    # Signing with Entitlements
    echo "Signing app..."

    # Generate the Entitlements file for microphone access
    cat > entitlements.plist <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>com.apple.security.device.audio-input</key>
    <true/>
</dict>
</plist>
EOF

    if [ -d "$APP_BUNDLE/Contents/Frameworks" ]; then
        codesign --force --sign - "$APP_BUNDLE/Contents/Frameworks/"* 2>/dev/null
    fi

    # Sign the app with the Entitlements
    codesign --force --deep --entitlements entitlements.plist --sign - "$APP_BUNDLE"

    # Clean up
    rm entitlements.plist
    echo "Bundle created: $APP_BUNDLE"

else
    # --- LINUX LOGIC ---
    # On Linux just copy the png next to the binary so the C++ code can load it
    if [ -f "icon.png" ]; then
        cp "icon.png" "icon.png" # Dummy copy just to be explicit, logic handled in C++
    fi
    # Copy bandplan.json
    if [ -f "bandplan.json" ]; then
        cp "bandplan.json" "bandplan.json"
    fi
    echo "SUCCESS! Run ./$OUT_FILE"
fi