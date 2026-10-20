#!/bin/bash
# Builds a .deb from the staged Linux bundle folder.
#
# Usage: packaging/build_deb.sh <stage_dir> <arch> <version> <output.deb>
#   stage_dir : folder containing OrbitSDR, libs/, bandplan.json, icon.png ...
#   arch      : amd64 | arm64
#   version   : e.g. 1.0.0 or 0.0.0+git1a2b3c4
set -euo pipefail

STAGE="$1"
ARCH="$2"
VERSION="$3"
OUT="$4"

MAINTAINER="Krzysztof Chojka <kontakt@kriss.waw.pl>"
HOMEPAGE="https://github.com/krzysztofchojka/OrbitSDR"

PKG="deb_build/orbitsdr_${VERSION}_${ARCH}"
rm -rf deb_build
mkdir -p "$PKG/DEBIAN" \
         "$PKG/opt/orbitsdr" \
         "$PKG/usr/bin" \
         "$PKG/usr/share/applications" \
         "$PKG/usr/share/pixmaps" \
         "$PKG/usr/share/doc/orbitsdr"

# --- application files -> /opt/orbitsdr ---
cp -a "$STAGE"/. "$PKG/opt/orbitsdr/"
rm -f "$PKG/opt/orbitsdr/OrbitSDR_Run.sh"      # portable launcher, not needed in the package

# --- launcher in PATH ---
cat > "$PKG/usr/bin/orbitsdr" << 'EOF'
#!/bin/sh
APP=/opt/orbitsdr
export LD_LIBRARY_PATH="$APP/libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$APP/OrbitSDR" "$@"
EOF

# --- menu entry + icon ---
if [ -f "$STAGE/icon.png" ]; then
    cp "$STAGE/icon.png" "$PKG/usr/share/pixmaps/orbitsdr.png"
fi

cat > "$PKG/usr/share/applications/orbitsdr.desktop" << 'EOF'
[Desktop Entry]
Type=Application
Name=OrbitSDR
Comment=Lightweight multiplatform SDR receiver
Exec=orbitsdr
Icon=orbitsdr
Terminal=false
Categories=HamRadio;AudioVideo;Audio;
Keywords=sdr;radio;rtl-sdr;sdrplay;aprs;ais;
EOF

# --- license ---
if [ -f LICENSE ]; then
    cp LICENSE "$PKG/usr/share/doc/orbitsdr/copyright"
fi

# --- permissions ---
find "$PKG" -type d -exec chmod 755 {} +
find "$PKG" -type f -exec chmod 644 {} +
chmod 755 "$PKG/usr/bin/orbitsdr" "$PKG/opt/orbitsdr/OrbitSDR"

# --- control file ---
INSTALLED_SIZE=$(du -sk "$PKG" | cut -f1)

cat > "$PKG/DEBIAN/control" << EOF
Package: orbitsdr
Version: ${VERSION}
Section: hamradio
Priority: optional
Architecture: ${ARCH}
Depends: libc6, libstdc++6, libgl1, libx11-6, libxrandr2, libxcursor1, libxi6, libfreetype6, libudev1, libusb-1.0-0, zenity
Recommends: rtl-sdr
Installed-Size: ${INSTALLED_SIZE}
Maintainer: ${MAINTAINER}
Homepage: ${HOMEPAGE}
Description: Lightweight multiplatform SDR receiver
 OrbitSDR is a lightweight software defined radio application with a
 waterfall display, AM/NFM/WFM/SSB demodulation, baseband recording and
 playback, and built-in APRS and AIS decoders.
 .
 Supports RTL-SDR, RTL-TCP, sound card input and SDRplay devices.
 SDRplay hardware additionally requires the official SDRplay API service
 from https://www.sdrplay.com/api/ to be installed.
EOF

dpkg-deb --root-owner-group --build "$PKG" "$OUT"

echo "---- package info ----"
dpkg-deb --info "$OUT"
echo "---- package contents (top level) ----"
dpkg-deb --contents "$OUT" | head -40
