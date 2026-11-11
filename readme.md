# OrbitSDR

**A fast, lightweight SDR receiver with built-in decoders. Build with Apple Silicon in mind, also runs on Windows and Linux.**

[![Build](https://github.com/krzysztofchojka/OrbitSDR/actions/workflows/build.yml/badge.svg)](https://github.com/krzysztofchojka/OrbitSDR/actions)
![Platforms](https://img.shields.io/badge/platform-macOS%20%7C%20Windows%20%7C%20Linux-lightgrey)

![OrbitSDR decoding ISM sensors and AIS while listening to FM broadcast](screenshot.png)

OrbitSDR is a C++17 / SFML receiver that stays small and responsive while decoding several digital modes at once. Plug in an SDRplay or an RTL-SDR, enable a decoder, and it tunes its own channel to the right frequency. You can keep listening to something else in the meantime.

- **Built for Apple Silicon (M1 and newer) and SDRplay.** That is the main development and test platform. No Homebrew or drivers needed for RTL-SDR, everything is bundled in the `.app`. For SDRPlay you have to install official [SDRPlay Api](https://sdrplay.com/hardware-api/).
- **Seven decoders built in:** APRS, AIS, ACARS, ISM sensors (rtl_433), RDS, SSTV and WEFAX.
- **Every decoder runs on its own channel.** Decode AIS, watch the 433 MHz sensors and listen to FM radio at the same time, all within one bandwidth.
- **Tested on real signals:** ships in harbours, weather stations and car tyre sensors in the city, APRS, SSTV, HF radiofax and RDS.

## Download

Get the latest build from the **[Releases page](https://github.com/krzysztofchojka/OrbitSDR/releases/latest)**.

| Platform | File | Notes |
|---|---|---|
| **macOS 15+ (Apple Silicon)** | `OrbitSDR-macOS.zip` | Primary platform. See [first launch on macOS](#first-launch-on-macos). |
| **Windows 10/11 x64** | `OrbitSDR-Setup-x.y.z-x64.exe` or `-portable.zip` | Installer or portable zip. |
| **Linux x64** | `orbitsdr_x.y.z_amd64.deb` or `.tar.gz` | Ubuntu 24.04 and newer (and other distributions with glibc 2.39+). |
| **Linux ARM64** | `orbitsdr_x.y.z_arm64.deb` or `.tar.gz` | Raspberry Pi 4/5 (64-bit Raspberry Pi OS Bookworm), other ARM boards. |

Development builds of every commit are in the [Actions](https://github.com/krzysztofchojka/OrbitSDR/actions) tab (you need a GitHub account to download them).

### First launch on macOS

The app is not notarized by Apple, so macOS blocks it the first time. You only have to allow it once:

1. Unzip `OrbitSDR-macOS.zip`, move `OrbitSDR.app` to **Applications** and open it.
2. macOS shows *"OrbitSDR" Not Opened*. Click **Done**.
3. Open **System Settings → Privacy & Security** and scroll down to the **Security** section.
4. Next to *"OrbitSDR" was blocked to protect your Mac*, click **Open Anyway** and confirm with your password or Touch ID.
5. Open OrbitSDR again and click **Open Anyway** in the dialog.

From then on OrbitSDR opens normally. Alternatively, you can remove the quarantine flag in Terminal:

```sh
xattr -dr com.apple.quarantine /Applications/OrbitSDR.app
```

## Supported sources

| Source | Details |
|---|---|
| **SDRplay** (RSP1A, RSP2, RSPdx, RSPduo...) | Antenna port selection, notch filters, Bias-T. Requires the [SDRplay API service](https://www.sdrplay.com/downloads/). |
| **RTL-SDR** | Standard and Direct Sampling (HF) modes, Bias-T. |
| **rtl_tcp** | Network RTL-SDR, e.g. a Raspberry Pi next to the antenna. |
| **Sound card** | Line-in / microphone as a baseband (I/Q) or audio source, e.g. the audio output of a scanner or transceiver. |
| **WAV files** | I/Q recordings (stereo) or demodulated audio (mono), 8/16/24-bit integer or 32-bit float, with a seekable timeline. |

## Decoders

| Decoder | Typical frequency | What you get |
|---|---|---|
| **AIS** | 161.975 / 162.025 MHz | Ships: name, call sign, type, position, speed, course. Both channels decoded at once. |
| **ISM** (rtl_433) | 433.92 / 868 / 315 / 915 MHz | About 300 device types: weather stations, thermometers, tyre pressure sensors (TPMS), door sensors, remotes, utility meters. |
| **ACARS** | 129–137 MHz | Aircraft datalink: registration, flight, label, message text. All ACARS channels in the bandwidth are decoded at once. |
| **APRS** | 144.800 MHz (EU) / 144.390 MHz (US) | AX.25 packets, messages and GPS positions. |
| **RDS** | FM broadcast | Station name, RadioText, PI, programme type, TP/TA, clock time. Also used to tune the WFM gain automatically. |
| **SSTV** | 145.800 MHz (ISS), HF | Robot 36/72, Martin M1/M2, Scottie S1/S2/DX, PD50–PD290. VIS auto-detection, slant correction, PNG export. |
| **WEFAX** | HF radiofax stations | Automatic start/stop, phasing, LPM/IOC selection, slant correction, PNG export. |

Decoded ISM, ACARS and APRS messages are logged to `ism.log`, `acars.log` and `aprs.log` in the settings folder. **Export** saves the messages of the current session to a file. Click a row in any decoder table to see all of its fields.

## Features

- **Demodulation:** AM, NFM, USB, LSB, WFM stereo, RAW.
- **Waterfall and spectrum:** drag to tune, zoom, axis panning, frequency snapping, band plan overlay.
- **Decoder channels:** each decoder has its own channel(s), shown as yellow bands on the spectrum with a handle on the tuning bar. When you enable a decoder its channels jump to the known frequencies inside the received band. Otherwise they follow the tuner.
  - Drag a handle to move a channel. It snaps to known frequencies and to the tuner.
  - Drop it on the tuner to make it follow the tuner.
  - Click it to listen to that channel.
  - An arrow at the edge of the tuning bar points to a channel that is outside the view.
- **I/Q or audio input:** for files and the sound card you choose whether the input is I/Q or demodulated audio. In audio mode, APRS, SSTV, WEFAX and ACARS decode the audio directly. ISM and AIS need I/Q from an SDR.
- **Recording:** demodulated audio or raw baseband I/Q to WAV.
- **Low sample rates:** software decimation, so even narrow bandwidths stay smooth.

<details>
<summary><b>ISM decoder details</b></summary>

- Decoding width of 250 kHz or 1 MHz.
- **Decoders: + extra** enables about 50 more rtl_433 decoders that are off by default. They only receive packets that no default decoder recognised. Their results are marked "(?)" because they can also match other devices.
- **Unknown: shown** lists every unrecognised packet with its guessed modulation, timing and bits.
- On RTL-SDR a slow "burst" AGC is used, so the gain never changes in the middle of a packet.

</details>

## Status and roadmap

OrbitSDR is under active development. macOS on Apple Silicon and Windows with SDRplay is the most tested setup. Linux and RTL-SDR builds are produced for every commit and work, but have had less testing.

- AIS, ISM, APRS, RDS, SSTV and WEFAX have been tested on real off-air signals.
- **ACARS has only been tested with recordings so far.** If you live near an airport, please try it and [report the result](https://github.com/krzysztofchojka/OrbitSDR/issues).
- More digital decoders are planned.

Bug reports, recordings of signals that don't decode, and feature requests are very welcome in [Issues](https://github.com/krzysztofchojka/OrbitSDR/issues). If OrbitSDR is useful to you, a ⭐ helps other people find it.

## Building from source

### macOS

```sh
brew install sfml librtlsdr
chmod +x build.sh
./build.sh            # asks whether to enable SDRplay support
```

### Linux (Ubuntu / Debian)

```sh
sudo apt update
sudo apt install build-essential libsfml-dev librtlsdr-dev zenity
chmod +x build.sh
./build.sh
```

You need SFML **3.0**. If your distribution ships SFML 2.x, build SFML 3 from source.

### Windows

Install [MSYS2](https://www.msys2.org/), open the **UCRT64** shell in the project folder and run:

```sh
./build_mingw_local.sh
```

The script installs the compiler and librtlsdr with `pacman`, downloads SFML 3.0 for MinGW and builds `orbitsdr.exe` with all DLLs next to it.

### SDRplay API

OrbitSDR does not link against the SDRplay API. It loads the API at run time: the version installed on your system first, and if that fails, the bundled copy (API 3.15). Without the API the program still starts, and only the SDRplay source is unavailable. Get the API from the [SDRplay downloads page](https://www.sdrplay.com/downloads/).

### rtl_433 (ISM decoder)

The rtl_433 sources are not stored in this repository. The build scripts call `scripts/rtl433.sh`, which downloads a pinned rtl_433 commit once into `.cache/` and compiles it into a static library. Later builds reuse it. You only need `curl` (or `wget`/`git`), `tar` and a C compiler.

- To update rtl_433, change `RTL433_REF` in `scripts/rtl433.sh`.
- Without network access and with an empty cache, OrbitSDR still builds, just without the ISM decoder.
- `RTL433_DISABLE=1` skips rtl_433 on purpose. `ORBITSDR_CACHE=/path` moves the cache.

## License and credits

OrbitSDR is licensed under the [GNU GPL v3](LICENSE).

It uses [SFML](https://www.sfml-dev.org/), [miniaudio](https://miniaud.io/), [librtlsdr](https://osmocom.org/projects/rtl-sdr), [rtl_433](https://github.com/merbanan/rtl_433) (GPL-2.0-or-later) and the [SDRplay API](https://www.sdrplay.com/).
