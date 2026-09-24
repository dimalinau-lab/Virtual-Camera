# VirtualCamNative (PC Client & Driver) 🚀

> **Ultra-Low-Latency, High-Performance DirectShow & Media Foundation Virtual Camera & Microphone for Windows powered by Android hardware acceleration.**

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg?style=for-the-badge&logo=cplusplus)](https://isocpp.org/)
[![DirectShow](https://img.shields.io/badge/Driver-DirectShow%20%2F%20MF-orange.svg?style=for-the-badge)](https://docs.microsoft.com/en-us/windows/win32/directshow/directshow)
[![FFmpeg](https://img.shields.io/badge/Decoder-FFmpeg%20HEVC-green.svg?style=for-the-badge&logo=ffmpeg)](https://ffmpeg.org/)
[![Windows](https://img.shields.io/badge/Windows-10%2F11-0078D6.svg?style=for-the-badge&logo=windows)](https://www.microsoft.com/)
[![Android App](https://img.shields.io/badge/Companion-Virtual--Camera--Android-3DDC84.svg?style=for-the-badge&logo=android)](https://github.com/dimalinau-lab/Virtual-Camera-Android)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg?style=for-the-badge)](LICENSE)

**VirtualCamNative** is an open-source, high-efficiency desktop client and system driver written in pure **C++20**. Serving as a modern, lightweight alternative to proprietary solutions like DroidCam and Iriun, it receives hardware-accelerated **HEVC (H.265)** video and raw 48 kHz PCM audio over USB or Wi-Fi. The stream is converted on-the-fly and fed into native **DirectShow / Media Foundation virtual camera and microphone devices**, providing a plug-and-play experience in **Discord, OBS Studio, Zoom, Telegram, and WebRTC browsers** with ultra-low latency.

---

## 🌟 Key Features

- ⚡ **Ultra-Low-Latency Hot Pipeline (~10–15 ms)**  
  Single-pass color conversion (Fast Bilinear BGRA to NV12) with zero dynamic allocations on the critical rendering path.
- 🎯 **Smooth 60 FPS via SIMD AVX2 Frame Blending**  
  Hardware-accelerated pixel averaging (`_mm256_avg_epu8`) synthesizing smooth 60 FPS motion in ~0.2 ms with minimal CPU impact.
- 🎭 **Troll FX & Live Filter Suite**  
  Built-in real-time stream distortion suite executed directly in the rendering loop without latency penalties:
  - **Nuclear Flashbang / Overexposure:** Exponential bloom turning light sources and lamps into blinding flares.
  - **144p Pixelate:** Dynamic block downscaling (low, medium, ATM-grade compression).
  - **VHS Glitch:** Randomized horizontal scanline tearing and color jitter.
  - **90s Bitcrush:** 16-bit retro color palette quantization.
  - **5 FPS Slideshow:** Simulated heavy packet loss and network stutter.
- 🎨 **Multi-Skin Dual UI Engine (Persistent State)**  
  Full support for interchangeable frontend layouts with automatic startup state persistence via `config.json`:
  - **Arcane Cyber (`index.html`):** Chamfered futuristic HUD with gothic accents and neon glow.
  - **Motion UI (`index2.html`):** Modern, floating glass dock layout with smooth Anime.js transitions.
- 🎙️ **Virtual Microphone Integration (48 kHz PCM)**  
  Low-latency WASAPI pipeline feeding phone audio directly into system apps via **VB-Audio Cable** and DirectShow capture filter.
- 🛡️ **Anti-Bufferbloat Socket Management**  
  Non-blocking queue inspection via `ioctlsocket(FIONREAD)` with proactive frame-dropping safeguards to eliminate accumulated streaming lag over USB and Wi-Fi.
- 🔒 **PIN-Free Device Discovery & Trust Architecture**  
  Continuous background UDP scanner (`DeviceDiscoveryService`) coupled with an explicit one-tap pairing modal dialog on the mobile screen (`/api/pair` / `/api/unpair`), backed by local trust caches.
- 🎥 **Dual Virtual Driver Architecture**  
  A native COM filter (`NativeMFVirtualCam.dll`) feeding both camera frames and microphone audio through synchronized **Windows Shared Memory** (Memory-Mapped Files + Win32 Events).
- 🔄 **Sensor-Aware Geometric Transformation**  
  Direct portrait transposition resolving sensor orientation issues for both back and front cameras with hardware mirroring.

---

## 📊 Comparison with Existing Solutions

| Feature | **VirtualCamNative (C++)** | DroidCam | Iriun Cam |
| :--- | :---: | :---: | :---: |
| **Language & Runtime** | **Native C++20 (No Java/C#)** | C++ / C# | C++ / Objective-C |
| **Video Codec** | **HEVC / H.265 (Hardware)** | H.264 / MJPEG | H.264 / HEVC |
| **60 FPS Support** | **Yes (AVX2 Interpolation)** | Limited (Paid) | Limited |
| **Troll FX & Shaders** | **Yes (Flashbang, 144p, Glitch)** | No | No |
| **Modular UI Skins** | **Yes (Cyberpunk & Minimal Glass)** | Fixed UI | Fixed UI |
| **Integrated Audio** | **Yes (48 kHz WASAPI / DirectShow)** | Yes (Driver-based) | Yes |
| **Multi-Device Selection** | **Yes (Dropdown Discovery)** | Manual / Single | Limited |
| **Pairing & Access Control** | **Yes (On-Screen Authorization)** | PIN / None | None |
| **Latency (USB)** | **~10–15 ms** | ~40–70 ms | ~30–50 ms |
| **Bufferbloat Prevention** | **Yes (Automatic Queue Guard)** | No | Limited |
| **Wi-Fi Pairing** | **Auto-Discovery (UDP :8888)** | Manual IP Entry | mDNS / Bonjour |
| **License** | **100% Free & Open Source (MIT)** | Proprietary (Freemium) | Proprietary (Watermarked) |

---

## 📐 System Architecture

```
 +---------------------------------------------------------------------------------+
 |                        ANDROID CLIENT (HARDWARE SOURCE)                         |
 |              ( [https://github.com/dimalinau-lab/Virtual-Camera-Android](https://github.com/dimalinau-lab/Virtual-Camera-Android) )        |
 |                                                                                 |
 | [ CameraX Source ] ---> [ MediaCodec H.265 ]  ---> [ Raw TCP Server :8554 ]     |
 | [ AudioRecord ]    ---> [ Raw 48kHz PCM ]     ---> [ Audio TCP Server :8555 ]   |
 | [ NanoHTTPD :8080 ] <--- REST & Pairing ------ [ UDP Discovery Beacon :8888 ]   |
 +----------------------------------------|----------------------------------------+
                                          | TCP Video/Audio / HTTP REST / UDP Beacon
                                          v
 +---------------------------------------------------------------------------------+
 |                        VIRTUALCAMNATIVE PC CLIENT (C++20)                       |
 |                                                                                 |
 |  [ DeviceDiscoveryService ] ───► [ Device Selector Dropdown ]                   |
 |  [ TcpReceiver ]               [ AudioReceiver (WASAPI / VB-Cable) ]            |
 |         │                                                                       |
 |         ▼ (Anti-Bufferbloat Queue Guard)                                        |
 |  [ NvdecDecoder (FFmpeg Low-Delay) ]                                            |
 |         │                                                                       |
 |         ▼                                                                       |
 |  [ 64x64 Block Rotator ] ───► [ AVX2 NV12 Blender (60 FPS) ]                    |
 |         │                                   │                                   |
 |         ▼                                   ▼                                   |
 |  [ Local MJPEG Preview :8000 ]     [ Win32 Shared Memory MMF ]                  |
 |         │                                   │                                   |
 |         ▼                                   │                                   |
 |  [ WebView2 Desktop GUI ]                   │ Frame Buffer + Sync Events        |
 +---------------------------------------------|-----------------------------------+
                                               v
 +---------------------------------------------------------------------------------+
 |                NATIVE VIRTUAL DRIVERS (DIRECTSHOW & MEDIA FOUNDATION)           |
 |                                                                                 |
 |  [ NativeMFVirtualCam.dll ]                                                     |
 |    ├── Video Capture Filter ("Native High-Speed Cam")                           |
 |    └── Audio Capture Filter ("VirtualCam Native Microphone")                    |
 +----------------------------------------|----------------------------------------+
                                          | DirectShow Capture Pins
                                          v
 +---------------------------------------------------------------------------------+
 |                              CONSUMING APPLICATIONS                             |
 |                                                                                 |
 |    Discord    |    OBS Studio    |    Zoom    |    Telegram    |    Browsers    |
 +---------------------------------------------------------------------------------+
```

---

## 🚀 Quick Start

### Prerequisites
- **Operating System:** Windows 10 or Windows 11 (64-bit).
- **Companion App:** [Virtual-Camera-Android](https://github.com/dimalinau-lab/Virtual-Camera-Android) installed on your smartphone.
- **Visual C++ Redistributable:** 2015–2022 (x64).
- **VB-Audio Cable:** *(Optional, bundled with installer for system microphone redirection)*.

### USB Connection (Lowest Latency & Highest Stability)
1. Enable **USB Debugging** on your phone (*Settings -> Developer Options -> USB Debugging*).
2. Connect your phone to your PC via a USB cable.
3. Launch `VirtualCamNative.exe`.
4. Click **USB Connect** — ports `8080`, `8554`, and `8555` are mapped automatically via ADB.

### Wi-Fi Connection (Wireless)
1. Ensure your PC and smartphone are connected to the same Wi-Fi network (5 GHz recommended).
2. Launch `VirtualCamNative.exe`. Active devices are discovered automatically and displayed in the top selector.
3. Select your device from the dropdown menu and click **Wi-Fi Connect**.
4. If connecting for the first time, tap **Allow** on the confirmation dialog prompt displayed on your smartphone screen.

---

## 🌐 REST Control & Telemetry API

The embedded HTTP server running on port `8000` provides local status endpoints and forwards configuration commands to the Android device:

| Endpoint | Method | Payload / Query | Description |
| :--- | :---: | :--- | :--- |
| `/api/devices` | `GET` | - | Returns active discovered devices: `[{"id":"...","name":"...","ip":"...","port":8080}]`. |
| `/api/pair_device` | `POST` | `?ip=192.168.1.X` | Triggers a confirmation dialog prompt on the phone screen and acquires a trust token. |
| `/api/connect_adb` | `POST` | - | Forwards ADB ports (`8080`, `8554`, `8555`) and begins streaming over USB. |
| `/api/connect` | `POST` | `{"ip": "192.168.1.X", "auth_token": "..."}` | Validates auth token and initiates Wi-Fi stream. |
| `/api/disconnect` | `POST` | - | Safely shuts down video/audio reception sockets. |
| `/api/status` | `GET` | - | Returns active connection details, device name, and camera lens. |
| `/api/telemetry` | `GET` | - | Returns real-time metrics: `{"fps": 60.0, "bitrate": "10 Mbps", "codec": "H.265"}`. |
| `/api/phone/set_config` | `POST` | `{"fps": 60, "bitrate": 10000000, "resolution": "1080p"}` | Dynamically changes resolution, target FPS, and bitrate. |
| `/api/phone/action/(.*)` | `POST` | `{"action": "switch_camera" \| "toggle_torch" \| "toggle_blackout" \| "toggle_mic_mute"}` | Dispatches hardware controls to the device. |
| `/api/audio_volume` | `GET` | `?val=1.0` | Adjusts software microphone gain (0.0 to 2.0). |
| `/api/orientation` | `GET` | `?mode=vertical \| horizontal` | Changes composition layout and canvas cropping in real time. |
| `/stream` | `GET` | - | Multipart MJPEG stream for client preview rendering. |

---

## 🛠️ Building from Source

### Requirements
- **CMake:** Version 3.20 or newer.
- **Compiler:** Microsoft Visual C++ (MSVC) v143 (Visual Studio 2022) with C++20 support.
- **FFmpeg:** Shared 64-bit build (version 6.x or 7.x) including `avcodec`, `avutil`, `swscale`.
- **OpenMP:** Enabled for SIMD and multi-core spatial transformations.

### Build Steps (CMake)
1. Clone the repository:
   ```bash
   git clone [https://github.com/dimalinau-lab/Virtual-Camera.git](https://github.com/dimalinau-lab/Virtual-Camera.git)
   cd Virtual-Camera
   ```
2. Generate build tree via CMake (adjust `FFMPEG_ROOT` to match your local installation):
   ```cmd
   cmake -B build -S . -G "Visual Studio 17 2022" -A x64 -DFFMPEG_ROOT="D:/ffmpeg-master-latest-win64-gpl-shared"
   ```
3. Compile the solution in Release mode:
   ```cmd
   cmake --build build --config Release
   ```
4. Compiled binaries and assets (`index.html`, `icon.ico`, dependencies) are automatically copied to `bin/`.

---

## 📦 Installer Compilation (Inno Setup)

An Inno Setup script is included to generate a silent, self-contained installer:
1. Open `installer.iss` in **Inno Setup Compiler**.
2. Verify the `#define` source paths match your workspace directory.
3. Click **Compile** (<kbd>Ctrl</kbd> + <kbd>F9</kbd>).  
The output executable bundles the VC++ Redistributable, VB-Cable driver setup, COM DLL, and GUI assets.

---

## 🔧 Driver Registration

The application automatically registers required filters upon first run when executed with administrator privileges. To register or remove the COM driver manually:

```cmd
:: Register virtual camera and microphone driver (Run as Administrator)
regsvr32.exe /s bin\NativeMFVirtualCam.dll

:: Unregister driver
regsvr32.exe /u /s bin\NativeMFVirtualCam.dll
```

---

## 📱 Companion Android App

For the mobile streamer component, see the companion repository:  
👉 **[Virtual-Camera-Android on GitHub](https://github.com/dimalinau-lab/Virtual-Camera-Android)**

---

## 📄 License

This project is licensed under the **MIT License**. See the [`LICENSE`](LICENSE) file for details.
