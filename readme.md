# OrbitSDR

A lightweight, high-performance SDR receiver built with C++17 and SFML. 

## ⚠️ Current Status: Very Early Access (Pre-Alpha)

Please note that this project is in a very early stage of development. It is currently tested primarily on **macOS** (Apple Silicon) but should also work on Linux and Windows.

![screenshot](screenshot.png)

## ✨ Features

- **Baseband Player:** Play raw IQ `.wav` files with a seekable timeline slider to analyze recordings (for now only 16-bit PCM is supported).
- **Integrated APRS:** Built-in AX.25 packet decoder with a live dashboard for messages and GPS telemetry (more digital modes planned).
- **Recording:** Record both demodulated Audio or raw Baseband IQ data.
- **Smooth UX:** Waterfall with drag-to-tune, axis panning, and frequency snapping.
- **Demodulation:** Supports AM, NFM, LSB/USB, and WFM Stereo.
- **Hardware Support:** Works with RTL-SDR and SDRPlay (API 3.x).

### To-Do / Known Limitations

- **RTL-SDR:** Direct Sampling mode (HF) is not yet implemented.
- **SDRPlay:** Basic support only (no antenna selection or notch filters yet).
- **Decoders:** Currently only APRS (AX.25) is supported, more digital decoders will be added in future updates.
- **Files:** Add support for 32-bit Float baseband recordings.

## 🍎 Building on macOS
### 1. Install Dependencies (Homebrew):
```sh
brew install sfml
brew install librtlsdr
```
### 2. Build
```sh
chmod +x build.sh
./build.sh
```
*The script will ask if you want to link SDRPlay libraries.*

## 🐧 Building on Linux
### 1. Install Dependencies (Ubuntu/Debian):
```sh
sudo apt update
# Note: Ensure you install SFML 3.0. If your repo has 2.x, build SFML from source.
sudo apt install build-essential libsfml-dev librtlsdr-dev zenity
```
### 2. Build
```sh
chmod +x build.sh
./build.sh
```
*The script will ask if you want to link SDRPlay libraries.*