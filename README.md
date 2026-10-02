# VirtualCamNative (PC Client & Driver) 🚀

> **Ultra-Low-Latency, High-Performance DirectShow & Media Foundation Virtual Camera & Microphone for Windows powered by Android hardware acceleration.**

<p align="center">
  <b>Language / Мова / Язык:</b><br>
  <a href="#virtualcamnative-pc-client--driver-">English</a> • 
  <a href="#-virtualcamnative-руководство-на-русском">Русский</a> • 
  <a href="#-virtualcamnative-посібник-українською">Українська</a>
</p>

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg?style=for-the-badge&logo=cplusplus)](https://isocpp.org/)
[![DirectShow](https://img.shields.io/badge/Driver-DirectShow%20%2F%20MF-orange.svg?style=for-the-badge)](https://docs.microsoft.com/en-us/windows/win32/directshow/directshow)
[![FFmpeg](https://img.shields.io/badge/Decoder-FFmpeg%20HEVC-green.svg?style=for-the-badge&logo=ffmpeg)](https://ffmpeg.org/)
[![Windows](https://img.shields.io/badge/Windows-10%2F11-0078D6.svg?style=for-the-badge&logo=windows)](https://www.microsoft.com/)
[![Android App](https://img.shields.io/badge/Companion-Virtual--Camera--Android-3DDC84.svg?style=for-the-badge&logo=android)](https://github.com/dimalinau-lab/Virtual-Camera-Android)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg?style=for-the-badge)](LICENSE)

**VirtualCamNative** is an open-source, high-efficiency desktop client and system driver written in pure **C++20**. Serving as a modern, lightweight alternative to proprietary solutions like DroidCam and Iriun, it receives hardware-accelerated **HEVC (H.265)** video and raw 48 kHz PCM audio over USB or Wi-Fi. The stream is converted on-the-fly and fed into native **DirectShow / Media Foundation virtual camera and microphone devices**, providing a plug-and-play experience in **Discord, OBS Studio, Zoom, Telegram, and WebRTC browsers** with ultra-low latency.

---

## 🚀 What's New in v2.2.0

- 🔄 **In-App Auto-Update System**: One-click updates powered by the GitHub Releases API with background chunked downloading, download progress telemetry, update notification pill/modal, and silent installer launch.
- 🎛️ **Advanced Audio DSP Suite**: Integrated RNNoise AI neural noise suppression, mechanical keyboard & mouse switch Transient De-Clicker (-16 dB), Automatic Gain Control (AGC) with soft-knee limiter (+12 dB boost), and 3-Band Parametric Equalizer (120 Hz, 2.2 kHz, 7.5 kHz).
- 🎮 **Direct3D 11 (D3D11) Hardware Pipeline**: GPU shader-accelerated color grading, 3D LUT matrix processing, and digital zoom with zero-copy DirectShow shared memory frame transfer.
- 🎭 **AI Neural Background Engine**: Real-time virtual Bokeh blur with adjustable radius/softness, Chroma Green Screen replacement, and Dark Studio stage mode.
- 📊 **Minimalist Telemetry HUD Toggle**: On-screen live FPS, bitrate, battery temp, and resolution overlay. **Disabled by default on startup**, toggleable via Director button, Settings drawer, or hotkey `<kbd>Ctrl</kbd> + <kbd>Shift</kbd> + <kbd>H</kbd>` with persistent state in `config.json`.
- 📷 **Dual-Camera Multi-Cam Switching**: Instant channel switching between two phones on the local network (`/api/multicam`).

---

## 🌟 Key Features

### 🎥 Video & Rendering Pipeline
- ⚡ **Ultra-Low-Latency Pipeline (~10–15 ms)**  
  Single-pass color conversion (Fast Bilinear BGRA to NV12) with zero dynamic allocations on the critical rendering path.
- 🎯 **Smooth 60 FPS via SIMD AVX2 Frame Blending**  
  Hardware-accelerated pixel averaging (`_mm256_avg_epu8`) synthesizing smooth 60 FPS motion in ~0.2 ms with minimal CPU impact.
- 🛡️ **Anti-Bufferbloat Socket Management**  
  Non-blocking queue inspection via `ioctlsocket(FIONREAD)` with proactive frame-dropping safeguards to eliminate accumulated streaming lag over USB and Wi-Fi.
- 🔄 **Sensor-Aware Geometric Transformation**  
  Direct portrait transposition resolving sensor orientation issues for both back and front cameras with hardware mirroring and 180° inversion.
- 🎥 **Dual Virtual Driver Architecture**  
  A native COM filter (`NativeMFVirtualCam.dll`) feeding both camera frames and microphone audio through synchronized **Windows Shared Memory** (Memory-Mapped Files + Win32 Events).

### 🎨 Studio Optics & 3D LUT Color Engine
- 🎮 **Direct3D 11 GPU Acceleration (v2.2.0)**  
  Hardware shader execution of color correction matrix and 3D LUT grading directly on GPU with zero CPU overhead.
- 🔍 **Physical Lens Switching & Digital Framing**  
  - Direct hardware optical lens selection: **0.5x Ultra-Wide**, **1x Wide**, and **2x / 3x Telephoto** via Camera2 `CONTROL_ZOOM_RATIO`.
  - Digital Zoom (1.0x to 4.0x) with real-time Pan $(X, Y)$ framing.
- 🎞️ **Real-Time Color Matrix & 3D LUT Grading**  
  Zero-copy pixel grading running on the rendering pipeline:
  - **Brightness** ($-100$ to $+100$) & **Contrast** ($50\%$ to $200\%$).
  - **Saturation** ($0\%$ to $200\%$) & **Color Temperature** ($-50$ Warm to $+50$ Cool).
  - Built-in cinematic 3D LUT film profiles: *Neutral*, *Cine Teal & Orange*, *Golden Hour*, *Emerald Matrix*, *Noir B&W*, *Cyber Neon*.
- 🎭 **AI Neural Background Engine (v2.2.0)**  
  Real-time background manipulation: Bokeh Blur (custom radius/softness), Green Screen chroma replacement, and Dark Studio lighting.

### 🎙️ Advanced Audio Suite
- ⏱️ **Lip-Sync Compensation Delay (0–500 ms)**  
  Circular ring-buffer delaying PCM audio samples to achieve frame-perfect audio-video synchronization.
- 🔇 **Studio Noise Gate DSP**  
  Real-time suppression of keyboard clicking and cooling fan rumble with adjustable RMS threshold, instantaneous attack, and smooth exponential decay.
- 🤖 **RNNoise AI Neural Noise Suppression (v2.2.0)**  
  Deep-learning neural voice filter eliminating constant air conditioning, PC fans, and ambient background rumble while preserving vocal clarity.
- ⌨️ **Transient Switch De-Clicker (v2.2.0)**  
  High-slew transient detector suppressing sharp mechanical keyboard clicks and mouse switches by up to -16 dB.
- 🎚️ **Automatic Gain Control (AGC) & Soft Limiter (v2.2.0)**  
  Dynamic speech leveling with up to +12 dB boost for quiet microphones, protected by a soft-knee limiter against clipping distortion.
- 🎛️ **3-Band Parametric Equalizer (v2.2.0)**  
  Studio biquad filters: Low-shelf (120 Hz, Bass body), Peaking (2.2 kHz, Vocal clarity), and High-shelf (7.5 kHz, Air brilliance), adjustable from -15 dB to +15 dB.
- 🎛️ **WASAPI Virtual Cable Redirection**  
  Direct integration with **VB-Audio CABLE** and DirectShow capture filter for crystal-clear 48 kHz 16-bit stereo transmission.

### 🎭 Troll FX Distortion Suite
Built-in real-time stream distortion executed directly in the rendering loop without latency penalties:
- **Nuclear Flashbang / Overexposure:** Exponential bloom turning light sources into blinding flares.
- **144p Pixelate:** Dynamic block downscaling (144p / ATM security camera look).
- **VHS Glitch:** Randomized horizontal scanline tearing and color jitter.
- **90s Bitcrush:** 16-bit retro color palette quantization.
- **5 FPS Slideshow:** Simulated heavy packet loss and network stutter.

### 🖥️ Desktop UX & Triple Design System
- 🎨 **3 Switchable Design Systems (`config.json` persistent)**:
  - **Variant 1 — Arcane Cyber (`index.html`):** Chamfered futuristic HUD with gothic accents and neon glow.
  - **Variant 2 — Motion Glass (`index2.html`):** Modern Bento layout with Emil Kowalski spring physics, 1px borders, and frosted glassmorphism.
  - **Variant 3 — Classic Aqua (`index3.html`):** Mac OS X Aqua pinstripes, brushed aluminum textures, gel buttons, and drop shadows.
- 🌐 **Full Trilingual Localization:** English (EN), Ukrainian (UK), and Russian (RU).
- 📥 **System Tray & Clean Window Management:**
  - Configurable **Close to Tray** (`close_to_tray`): minimize to system notification area instead of closing, keeping virtual camera live.
  - Diagnostic Terminal Console hidden by default (`SW_HIDE`), toggleable via Settings UI and tray context menu.
- 📊 **Minimalist Telemetry HUD Toggle (v2.2.0):**
  - Real-time on-screen telemetry showing FPS, bitrate, battery temperature, and resolution.
  - **Disabled by default on startup** to keep the workspace clean; toggleable on-demand via the Director pill button, Settings drawer, or hotkey `<kbd>Ctrl</kbd> + <kbd>Shift</kbd> + <kbd>H</kbd>`. State persists in `config.json`.
- ⌨️ **Global Win32 System Hotkeys:**
  - <kbd>Ctrl</kbd> + <kbd>Shift</kbd> + <kbd>M</kbd> : Mute / Unmute Microphone
  - <kbd>Ctrl</kbd> + <kbd>Shift</kbd> + <kbd>B</kbd> : Privacy Shield (Blackout shutter)
  - <kbd>Ctrl</kbd> + <kbd>Shift</kbd> + <kbd>C</kbd> : Flip Camera (Front / Back)
  - <kbd>Ctrl</kbd> + <kbd>Shift</kbd> + <kbd>H</kbd> : Toggle HUD Telemetry Overlay (FPS / Bitrate / Temp)
  - <kbd>Ctrl</kbd> + <kbd>Shift</kbd> + <kbd>T</kbd> : Toggle Troll FX Distortion

---

## 📊 Comparison with Existing Solutions

| Feature | **VirtualCamNative (C++)** | DroidCam | Iriun Cam |
| :--- | :---: | :---: | :---: |
| **Language & Runtime** | **Native C++20 (Zero-bloat)** | C++ / C# | C++ / Objective-C |
| **Video Codec** | **HEVC / H.265 (Hardware)** | H.264 / MJPEG | H.264 / HEVC |
| **Frame Rate** | **Smooth 60 FPS (AVX2)** | Limited (Paid) | Limited |
| **Optics & 3D LUT** | **Yes (0.5x/1x/2x, Color DSP, 6 LUTs)** | No | No |
| **Lip-Sync & Noise Gate** | **Yes (0–500ms Delay + Gate DSP)** | No | No |
| **Troll FX & Shaders** | **Yes (Flashbang, 144p, Glitch, Bitcrush)** | No | No |
| **Modular UI Themes** | **Yes (Cyber, Bento Glass, Aqua)** | Fixed UI | Fixed UI |
| **System Tray & Hotkeys** | **Yes (Background stream + Hotkeys)** | Basic | Basic |
| **Latency (USB)** | **~10–15 ms** | ~40–70 ms | ~30–50 ms |
| **Bufferbloat Prevention** | **Yes (Automatic Queue Guard)** | No | Limited |
| **Wi-Fi Pairing** | **Auto-Discovery (UDP :8888)** | Manual IP Entry | mDNS / Bonjour |
| **License** | **100% Free & Open Source (MIT)** | Proprietary (Freemium) | Proprietary (Watermarked) |

---

## 📐 System Architecture

```
 +---------------------------------------------------------------------------------+
 |                        ANDROID CLIENT (HARDWARE SOURCE)                         |
 |           ( https://github.com/dimalinau-lab/Virtual-Camera-Android )           |
 |                                                                                 |
 | [ Camera2 Source (0.5x/1x/2x) ] ---> [ MediaCodec H.265 ] ---> [ TCP :8554 ]    |
 | [ AudioRecord (Noise Suppressed) ] -> [ Raw 48kHz PCM ]   ---> [ TCP :8555 ]    |
 | [ NanoHTTPD :8080 ] <------------- REST & Pairing <------- [ UDP Beacon :8888 ] |
 +----------------------------------------|----------------------------------------+
                                          | TCP Video/Audio / HTTP REST / UDP Beacon
                                          v
 +---------------------------------------------------------------------------------+
 |                        VIRTUALCAMNATIVE PC CLIENT (C++20)                       |
 |                                                                                 |
 |  [ DeviceDiscoveryService ] ───► [ Device Selector Dropdown ]                   |
 |  [ TcpReceiver ]                  [ AudioReceiver (Ring-Buffer + Noise Gate) ]  |
 |         │                                                                       |
 |         ▼ (Anti-Bufferbloat Queue Guard)                                        |
 |  [ NvdecDecoder (FFmpeg Low-Delay) ]                                            |
 |         │                                                                       |
 |         ▼                                                                       |
 |  [ 64x64 Block Rotator ] ───► [ Studio Optics & 3D LUT Color Engine ]           |
 |         │                                   │                                   |
 |         ▼                                   ▼                                   |
 |  [ Local MJPEG Preview :8000 ]     [ Win32 Shared Memory MMF ]                  |
 |         │                                   │                                   |
 |         ▼                                   │                                   |
 |  [ WebView2 Desktop GUI ]                   │ Frame Buffer + Sync Events        |
 |  (System Tray + Hotkeys)                    │                                   |
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

### Installation Options
- 📦 **Recommended (One-Click Setup):** Download and run **`VirtualCamNative_Setup_v2.2.0.exe`** from [Releases](https://github.com/dimalinau-lab/Virtual-Camera/releases). The installer automatically installs the runtime, bundles the app, and registers the virtual camera COM driver filter (`NativeMFVirtualCam.dll`).
- 🛠️ **Manual / Portable:** Unpack the portable archive and run `regsvr32.exe /s bin\NativeMFVirtualCam.dll` as Administrator.

### Prerequisites
- **Operating System:** Windows 10 or Windows 11 (64-bit).
- **Companion App:** [Virtual-Camera-Android](https://github.com/dimalinau-lab/Virtual-Camera-Android) installed on your smartphone (or download `VirtualCam-v2.2.0.apk`).
- **Visual C++ Redistributable:** 2015–2022 (x64) *(included in Inno Setup)*.
- **VB-Audio Cable:** *(Optional, for system microphone input routing)*.

### USB Connection (Lowest Latency & Maximum Stability)
1. Enable **USB Debugging** on your phone (*Settings -> Developer Options -> USB Debugging*).
2. Connect your phone to your PC via a USB cable.
3. Launch `VirtualCamNative.exe`.
4. Click **USB Connect** — ports `8080`, `8554`, and `8555` are mapped automatically via ADB.

### Wi-Fi Connection (Wireless)
1. Ensure your PC and smartphone are connected to the same Wi-Fi network (5 GHz recommended).
2. Launch `VirtualCamNative.exe`. Active devices are discovered automatically and displayed in the top selector.
3. Select your device from the dropdown menu and click **Wi-Fi Connect**.
4. If connecting for the first time, tap **Allow** on the confirmation prompt on your smartphone screen.

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
| `/api/status` | `GET` | - | Returns active connection details, device name, and camera status. |
| `/api/telemetry` | `GET` | - | Returns real-time metrics: `{"fps": 60.0, "bitrate": "10 Mbps", "codec": "H.265"}`. |
| `/api/phone/set_config` | `POST` | `{"fps": 60, "bitrate": 10000000, "resolution": "1080p"}` | Dynamically changes resolution, target FPS, and bitrate. |
| `/api/phone/lens` | `GET`/`POST` | `?lens=0.5x \| 1x \| 2x` | Switches physical camera sensor optics on the smartphone. |
| `/api/optics/color` | `GET`/`POST` | `?brightness=0&contrast=100&saturation=100&temp=0&preset=0` | Live studio color correction & 3D LUT selection. |
| `/api/optics/zoom` | `GET`/`POST` | `?zoom=1.0&pan_x=0.0&pan_y=0.0` | Digital zoom and pan framing. |
| `/api/optics/get` | `GET` | - | Returns current studio optics state. |
| `/api/optics/reset` | `GET`/`POST` | - | Resets zoom and color grading to defaults. |
| `/api/audio_delay` | `GET`/`POST` | `?ms=50` | Sets Lip-Sync compensation delay (0 to 500 ms). |
| `/api/noise_gate` | `GET`/`POST` | `?enabled=1&threshold=0.015` | Configures studio Noise Gate DSP. |
| `/api/audio_ai_noise` | `GET`/`POST` | `?enabled=1` | RNNoise AI neural background and fan noise suppression. |
| `/api/audio_agc` | `GET`/`POST` | `?enabled=1` | Automatic Gain Control & soft-knee limiter (+12 dB max boost). |
| `/api/audio_declicker` | `GET`/`POST` | `?enabled=1` | Suppresses mechanical keyboard & mouse transient clicks (-16 dB). |
| `/api/audio_eq` | `GET`/`POST` | `?low=0&mid=0&high=0` | 3-band parametric equalizer (Low 120Hz, Mid 2.2kHz, High 7.5kHz, ±15 dB). |
| `/api/bg_effect` | `GET`/`POST` | `?mode=0..3&radius=1..25&softness=0.02..0.5` | AI neural background effects (0: Off, 1: Bokeh Blur, 2: Green Screen, 3: Dark Studio). |
| `/api/multicam` | `GET`/`POST` | `?channel=1\|2&ip1=...&ip2=...` | Fast dual-camera channel switching between devices. |
| `/api/app_settings` | `GET`/`POST` | `?close_to_tray=1&show_console=0&show_hud_stats=0` | Live app preferences: tray minimize, console visibility, and HUD telemetry overlay. |
| `/api/update/check` | `GET` | - | Queries GitHub Releases API for the latest available desktop release. |
| `/api/update/download` | `POST` | - | Triggers asynchronous background download of installer from GitHub. |
| `/api/update/status` | `GET` | - | Returns live download progress, speed, and status (`idle`, `downloading`, `ready`, `error`). |
| `/api/update/install` | `POST` | - | Launches installer with `/SILENT` switch and gracefully exits application. |
| `/api/get_config` | `GET` | - | Reads full JSON configuration file. |
| `/api/save_file` | `POST` | `{ ... }` | Persists user settings to `config.json`. |
| `/api/troll` | `POST` | `?fps_5=1&pixelate=8&glitch=1&bitcrush=1&overexposure=1` | Live Troll FX distortion parameters. |
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
   git clone https://github.com/dimalinau-lab/Virtual-Camera.git
   cd Virtual-Camera
   ```
2. Generate build tree via CMake:
   ```cmd
   cmake -B out/build/x64-release -S . -G "Ninja" -DCMAKE_BUILD_TYPE=Release
   ```
3. Compile the solution in Release mode:
   ```cmd
   cmake --build out/build/x64-release --config Release
   ```
4. Output binaries and frontend assets are copied to `bin/`.

---

## 📦 Installer Compilation (Inno Setup)

An Inno Setup script is included to generate a self-contained installer:
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

---
---

# 🇷🇺 VirtualCamNative (Руководство на русском)

> **Высокопроизводительная виртуальная камера и микрофон DirectShow & Media Foundation для Windows со сверхнизкой задержкой, использующая аппаратное ускорение Android.**

**VirtualCamNative** — это клиент и системный драйвер для Windows с открытым исходным кодом, написанный на чистом **C++20**. Являясь современной и легковесной альтернативой проприетарным решениям (DroidCam, Iriun), он принимает аппаратный видеопоток **HEVC (H.265)** и несжатый 48 кГц PCM-звук по кабелю USB или сети Wi-Fi. Поток на лету декодируется и передается в нативный **COM-драйвер виртуальной камеры и микрофона (DirectShow / Media Foundation)**, обеспечивая работу в **Discord, OBS Studio, Zoom, Telegram и WebRTC-браузерах** со сверхнизкой задержкой (~10–15 мс).

---

## 🚀 Что нового в версии v2.2.0

- 🔄 **Встроенная система автообновления**: Обновление в один клик через GitHub Releases API с фоновой загрузкой по частям, отображением скорости и прогресса, модальным окном со списком изменений и тихой автоматической установкой.
- 🎛️ **Пакет студийной обработки звука (Audio DSP Suite)**: Интегрированное нейросетевое шумоподавление RNNoise AI, динамический подавитель механических щелчков клавиатуры и мыши De-Clicker (-16 dB), автоматическая регулировка уровня (AGC) с лимитером soft-knee (+12 dB усиления) и 3-полосный параметрический эквалайзер (120 Гц, 2.2 кГц, 7.5 кГц).
- 🎮 **Аппаратный D3D11-конвейер Studio Optics**: Шейдерная обработка цветовой матрицы, 3D LUT профилей и цифрового зума силами видеокарты Direct3D 11 с передачей кадра в DirectShow без лишних копирований.
- 🎭 **Нейросетевые фоновые эффекты (AI Background Engine)**: Виртуальное размытие фона Bokeh Blur с настраиваемым радиусом и мягкостью краев, зеленый экран (Chroma Key) и темная сцена (Dark Studio).
- 📊 **Минималистичный переключаемый оверлей телеметрии HUD**: Отображение FPS, битрейта, температуры батареи смартфона и разрешения. **Выключен по умолчанию при старте**, активируется кнопкой в Director Bar, в панели настроек или хоткеем `<kbd>Ctrl</kbd> + <kbd>Shift</kbd> + <kbd>H</kbd>` с сохранением в `config.json`.
- 📷 **Быстрое переключение камер Multi-Cam**: Мгновенный выбор между двумя подключенными смартфонами в локальной сети (`/api/multicam`).

---

## 🌟 Ключевые возможности

### 🎥 Видеоконвейер и рендеринг
- ⚡ **Сверхнизкая задержка (~10–15 мс по USB)**  
  Однопроходная цветовая конвертация (быстрый билинейный BGRA в NV12) без динамических выделений памяти на критическом пути рендеринга.
- 🎯 **Плавные 60 FPS через AVX2 SIMD Frame Blending**  
  Аппаратное усреднение соседних пикселей (`_mm256_avg_epu8`) синтезирует плавный видеоряд 60 FPS за ~0.2 мс практически без нагрузки на CPU.
- 🛡️ **Защита от накопления задержки (Anti-Bufferbloat)**  
  Неблокирующая инспекция сокета через `ioctlsocket(FIONREAD)` с упреждающим сбросом устаревших кадров исключает накопление задержки потока при сетевых флуктуациях.
- 🔄 **Коррекция ориентации сенсора и переворот на 180°**  
  Прямое блочное транспонирование кадра 64×64 с поддержкой аппаратного зеркалирования и разворота на 180° для настольных креплений.
- 🎥 **Архитектура виртуального драйвера DirectShow / MF**  
  Нативный COM-фильтр (`NativeMFVirtualCam.dll`) передает видеокадры и аудиосемплы через кольцевой буфер **Windows Shared Memory** (Memory-Mapped Files + события синхронизации Win32).

### 🎨 Студийная оптика Studio Optics & 3D LUT
- 🎮 **Аппаратное D3D11 ускорение (v2.2.0)**  
  Вычисление цветовой матрицы и интерполяция 3D LUT исполняются пиксельными шейдерами DirectX 11 с нулевой нагрузкой на процессор.
- 🔍 **Переключение физических объективов камеры смартфона**  
  - Аппаратное переключение модулей: **0.5x Сверхширокоугольный**, **1x Основной**, **2x / 3x Телеобъектив** через Camera2 API (`CONTROL_ZOOM_RATIO`).
  - Цифровой зум (от 1.0x до 4.0x) с динамическим панорамированием $(X, Y)$.
- 🎞️ **Аппаратная цветокоррекция и кинематографические 3D LUT**  
  Обработка пикселей в реальном времени прямо на конвейере вывода:
  - **Яркость** (от $-100$ до $+100$) и **Контрастность** (от $50\%$ до $200\%$).
  - **Насыщенность** (от $0\%$ до $200\%$) и **Цветовая температура** (от $-50$ теплая до $+50$ холодная).
  - Встроенные кинематографические профили: *Neutral*, *Cine Teal & Orange*, *Golden Hour*, *Emerald Matrix*, *Noir B&W*, *Cyber Neon*.
- 🎭 **Нейросетевые эффекты фона (v2.2.0)**  
  Размытие заднего плана (Bokeh Blur), хромакей (Green Screen) и затемнение студии (Dark Studio).

### 🎙️ Студийный звуковой тракт
- ⏱️ **Компенсация задержки Lip-Sync (0–500 мс)**  
  Кольцевой буфер задержки PCM-семплов для достижения попиксельной синхронизации звука с артикуляцией губ.
- 🔇 **Студийный гейт шума (DSP Noise Gate)**  
  Подавление шума клавиатуры и гула кулеров с настраиваемым порогом RMS, мгновенной атакой и плавным экспоненциальным затуханием.
- 🤖 **Нейросетевой фильтр RNNoise AI (v2.2.0)**  
  Интеллектуальное подавление непрерывного гула вентиляторов ПК, кондиционеров и бытового шума без искажения тембра голоса.
- ⌨️ **Подавитель щелчков клавиатуры De-Clicker (v2.2.0)**  
  Детектор резких фронтов, глушащий удары по механическим свитчам и клики мыши до -16 dB.
- 🎚️ **Автоматическая регулировка уровня (AGC) и лимитер (v2.2.0)**  
  Выравнивание громкости тихих микрофонов с гейном до +12 dB и мягким компрессионным лимитером от клиппинга.
- 🎛️ **3-полосный параметрический эквалайзер (v2.2.0)**  
  Студийные биквадратные фильтры: Низ (120 Гц, теплота), Середина (2.2 кГц, разборчивость речи), Верх (7.5 кГц, воздух), от -15 dB до +15 dB.
- 🎛️ **Перенаправление в виртуальный аудиокабель WASAPI**  
  Прямая интеграция с **VB-Audio CABLE** и DirectShow Capture Filter для передачи звука 48 кГц 16-бит стерео в любые приложения.

### 🎭 Пакет шейдерных эффектов искажения (Troll FX)
Эффекты реального времени, исполняемые на CPU/SIMD без задержки:
- **Nuclear Flashbang:** Экспоненциальный засвет с эффектом светошумовой гранаты.
- **144p Pixelate:** Динамическое пиксельное огрубление картинки под камеру наблюдения.
- **VHS Glitch:** Горизонтальные разрывы строк и артефакты аналоговой кассеты.
- **90s Bitcrush:** Квантование цвета в 16-битную палитру ретро-консолей.
- **5 FPS Slideshow:** Эмуляция жестких сетевых потерь пакетов и зависания.

### 🖥️ Три графических интерфейса и сохранение конфигурации
- 🎨 **3 переключаемых стиля UI (`config.json`)**:
  - **Вариант 1 — Arcane Cyber (`index.html`):** Скошенные футуристичные панели, неоновая подсветка и темная палитра.
  - **Вариант 2 — Motion Glass (`index2.html`):** Современный Bento-дизайн с физикой пружин Эмиля Ковальски и матовым стеклом.
  - **Вариант 3 — Classic Aqua (`index3.html`):** Ретро-стиль Mac OS X Aqua, полосатый фон, металлические текстуры и гелевые кнопки.
- 💾 **Надежная система сохранения настроек (`ConfigManager`)**:
  - Автоматическое асинхронное сохранение всех 21 параметров (звук, оптика, битрейт, тема, FPS, переворот 180°, трей) при изменении в UI.
  - Атомарная запись через `.tmp` файл предотвращает повреждение конфигурации при неожиданном закрытии программы.
- 📥 **Работа в системном трее и скрытие консоли**:
  - Опция **Сворачивать в трей** (`close_to_tray`): окно закрывается в трей, стриминг продолжается без прерываний.
  - Терминал диагностики скрыт по умолчанию (`SW_HIDE`), переключается из настроек и контекстного меню трея.
- 📊 **Переключаемый оверлей телеметрии HUD (v2.2.0)**:
  - Живой показ FPS, битрейта, температуры аккумулятора и разрешения прямо на экране.
  - **Выключен по умолчанию при запуске**, чтобы не перегружать пользователя. Включается в панели режимов, в окне настроек или хоткеем `<kbd>Ctrl</kbd> + <kbd>Shift</kbd> + <kbd>H</kbd>` с сохранением в `config.json`.
- ⌨️ **Глобальные горячие клавиши Windows**:
  - <kbd>Ctrl</kbd> + <kbd>Shift</kbd> + <kbd>M</kbd> : Отключить / Включить микрофон
  - <kbd>Ctrl</kbd> + <kbd>Shift</kbd> + <kbd>B</kbd> : Шторка приватности (Blackout)
  - <kbd>Ctrl</kbd> + <kbd>Shift</kbd> + <kbd>C</kbd> : Переключить камеру (Фронтальная / Основная)
  - <kbd>Ctrl</kbd> + <kbd>Shift</kbd> + <kbd>H</kbd> : Показать / скрыть телеметрию HUD (FPS, градусы, битрейт)
  - <kbd>Ctrl</kbd> + <kbd>Shift</kbd> + <kbd>T</kbd> : Переключить эффект искажения (Troll FX)

---

## 🚀 Быстрый запуск

### Способы установки
- 📦 **Рекомендуемый (в один клик):** Скачайте и запустите **`VirtualCamNative_Setup_v2.2.0.exe`** из раздела [Releases](https://github.com/dimalinau-lab/Virtual-Camera/releases). Инсталлятор автоматически установит библиотеки Visual C++ Redistributable и зарегистрирует COM-фильтр виртуальной камеры (`NativeMFVirtualCam.dll`).
- 🛠️ **Портативная версия:** Распакуйте архив в удобную папку и выполните разовую регистрацию `regsvr32.exe /s bin\NativeMFVirtualCam.dll` от имени администратора.

### Системные требования
- **ОС:** Windows 10 или Windows 11 (64-бит).
- **Клиент для телефона:** Установленное приложение [Virtual-Camera-Android](https://github.com/dimalinau-lab/Virtual-Camera-Android) (или готовый файл `VirtualCam-v2.2.0.apk`).
- **Visual C++ Redistributable:** 2015–2022 (x64) *(встроен в инсталлятор)*.
- **VB-Audio Cable:** *(Опционально, для перенаправления звука микрофона)*.

### Подключение по USB (Рекомендуется для минимальной задержки)
1. Включите **Отладку по USB** на смартфоне (*Настройки -> Для разработчиков -> Отладка по USB*).
2. Подключите смартфон к ПК качественным кабелем USB.
3. Запустите `VirtualCamNative.exe`.
4. Нажмите кнопку **USB Connect** — порты `8080`, `8554` и `8555` пробросятся автоматически через ADB.

### Беспроводное подключение по Wi-Fi
1. Убедитесь, что ПК и смартфон подключены к одной Wi-Fi сети (рекомендуется 5 ГГц).
2. Запустите `VirtualCamNative.exe`. Приложение автоматически обнаружит телефон в сети через UDP-маяк.
3. Выберите устройство из выпадающего списка сверху и нажмите **Wi-Fi Connect**.
4. При первом подключении подтвердите сопряжение на экране смартфона.

---

## 🛠️ Сборка из исходников

```cmd
:: Клонирование репозитория
git clone https://github.com/dimalinau-lab/Virtual-Camera.git
cd Virtual-Camera

:: Генерация проекта CMake в Release x64
cmake -B out/build/x64-release -S . -G "Ninja" -DCMAKE_BUILD_TYPE=Release

:: Компиляция
cmake --build out/build/x64-release --config Release
```

Готовые исполняемые файлы и ресурсы интерфейса автоматически копируются в директорию `bin/`.

---

## 🔧 Ручная регистрация COM-драйвера

```cmd
:: Регистрация виртуальной камеры и микрофона (от имени Администратора)
regsvr32.exe /s bin\NativeMFVirtualCam.dll

:: Удаление регистрации драйвера
regsvr32.exe /u /s bin\NativeMFVirtualCam.dll
```

---
---

# 🇺🇦 VirtualCamNative (Посібник українською)

> **Високопродуктивна віртуальна камера та мікрофон DirectShow & Media Foundation для Windows із наднизькою затримкою та апаратним прискоренням Android.**

**VirtualCamNative** — це нативний клієнт і системний драйвер для Windows з відкритим вихідним кодом, створений на базі **C++20**. Він є сучасною високопродуктивною альтернативою комерційним програмам (DroidCam, Iriun) і транслює апаратний відеопотік **HEVC (H.265)** та нестиснений звук 48 кГц PCM через USB-кабель або мережу Wi-Fi. Потік декодується на льоту та передається безпосередньо у віртуальний драйвер **DirectShow / Media Foundation**, що забезпечує миттєву роботу в **Discord, OBS Studio, Zoom, Telegram та браузерах** із затримкою всього ~10–15 мс.

---

## 🚀 Що нового у версії v2.2.0

- 🔄 **Вбудована система автооновлення**: Оновлення в один клік через GitHub Releases API з фоновим завантаженням частинами, відображенням швидкості та прогресу, модальним вікном зі списком змін та тихою автоматичною інсталяцією.
- 🎛️ **Студійний звуковий процесор (Audio DSP Suite)**: Нейромережеве шумозаглушення RNNoise AI, динамічний фільтр механічних клацань клавіатури та миші De-Clicker (-16 dB), автоматичне регулювання гучності (AGC) з м'яким лімітером (+12 dB підсилення) та 3-смуговий параметричний еквалайзер (120 Гц, 2.2 кГц, 7.5 кГц).
- 🎮 **Апаратний D3D11 конвеєр Studio Optics**: Шейдерна обробка кольорових матриць, кінематографічних 3D LUT та цифрового зуму силами графічного процесора Direct3D 11 без навантаження на CPU.
- 🎭 **Нейромережеві ефекти заднього плану (AI Background Engine)**: Реалістичне розмиття Bokeh Blur із тонким налаштуванням радіусу та м'якості контуру, хромакей (Green Screen) та затемнена сцена (Dark Studio).
- 📊 **Мінімалістичний оверлей телеметрії HUD, що приховується**: Відображення FPS, бітрейту, температури акумулятора смартфона та роздільної здатності. **Вимкнений за замовчуванням при запуску**, вмикається кнопкою на панелі, у вікні налаштувань або хоткеєм `<kbd>Ctrl</kbd> + <kbd>Shift</kbd> + <kbd>H</kbd>` зі збереженням у `config.json`.
- 📷 **Миттєве перемикання камер Multi-Cam**: Швидкий вибір між двома активними смартфонами в локальній мережі (`/api/multicam`).

---

## 🌟 Головні переваги та можливості

### 🎥 Відеоконвеєр та рендеринг
- ⚡ **Наднизька затримка (~10–15 мс через USB)**  
  Однопрохідна колірна конвертація BGRA в NV12 без виділення динамічної пам'яті під час обробки кадрів.
- 🎯 **Плавні 60 FPS завдяки SIMD AVX2 Frame Blending**  
  Апаратне усереднення сусідніх кадрів за допомогою векторних інструкцій `_mm256_avg_epu8` синтезує плавний рух 60 FPS за ~0.2 мс без навантаження на процесор.
- 🛡️ **Захист від накопичення буфера (Anti-Bufferbloat)**  
  Неблокуючий моніторинг черги сокета через `ioctlsocket(FIONREAD)` зі своєчасним відкиданням застарілих кадрів, що запобігає появі лагів під час мережевих коливань.
- 🔄 **Геометрична трансформація та поворот на 180°**  
  Блокове транспонування кадру (64×64) із підтримкою апаратного дзеркалювання та розвороту на 180° для штативів та кронштейнів.
- 🎥 **Віртуальний драйвер DirectShow / Media Foundation**  
  Нативний COM-модуль (`NativeMFVirtualCam.dll`) передає відео та звук через спільну пам'ять **Windows Shared Memory** (Memory-Mapped Files + події синхронізації Win32).

### 🎨 Студійна оптика Studio Optics та 3D LUT
- 🎮 **Апаратне прискорення Direct3D 11 (v2.2.0)**  
  Обробка колірних матриць та інтерполяція 3D LUT виконується піксельними шейдерами GPU з нульовим навантаженням на центральний процесор.
- 🔍 **Перемикання фізичних об'єктивів смартфона**  
  - Пряме апаратне керування сенсорами: **0.5x Надширококутний**, **1x Основний**, **2x / 3x Телеоб'єктив** через Camera2 API (`CONTROL_ZOOM_RATIO`).
  - Цифровий зум (від 1.0x до 4.0x) із можливістю панорамування $(X, Y)$.
- 🎞️ **Апаратна корекція кольору та 3D LUT профілі**  
  Обробка кольору в реальному часі безпосередньо у відеоконвеєрі:
  - **Яскравість** (від $-100$ до $+100$) та **Контрастність** (від $50\%$ до $200\%$).
  - **Насиченість** (від $0\%$ до $200\%$) та **Колірна температура** (від $-50$ тепла до $+50$ холодна).
  - Вбудовані кінематографічні профілі: *Neutral*, *Cine Teal & Orange*, *Golden Hour*, *Emerald Matrix*, *Noir B&W*, *Cyber Neon*.
- 🎭 **Нейромережеві ефекти заднього плану (v2.2.0)**  
  Розмиття фону (Bokeh Blur), заміна зеленого фону (Green Screen) та студійне затемнення (Dark Studio).

### 🎙️ Професійний аудіотракт
- ⏱️ **Синхронізація Lip-Sync (0–500 мс)**  
  Кільцевий буфер затримки PCM-аудіо для досягнення ідеальної синхронізації звуку з рухом губ на відео.
- 🔇 **Студійний нойз-гейт (DSP Noise Gate)**  
  Фільтрація клацання клавіатури та шуму кулерів із регульованим RMS-порогом, миттєвою атакою та плавним експоненційним затуханням.
- 🤖 **Нейрофільтр шумопоглинання RNNoise AI (v2.2.0)**  
  Глибоке придушення постійного фонового шуму комп'ютерних кулерів і кондиціонерів без спотворення тембру голосу.
- ⌨️ **Фільтр клацань De-Clicker (v2.2.0)**  
  Аналізатор сплесків, що знижує гучність клацання механічних перемикачів клавіатури та миші до -16 dB.
- 🎚️ **Автоматичне регулювання гучності (AGC) та компресор (v2.2.0)**  
  Компенсація тихих мікрофонів з посиленням до +12 dB та м'яким лімітером для уникнення спотворень.
- 🎛️ **3-смуговий параметричний еквалайзер (v2.2.0)**  
  Студійні biquad-фільтри: Низ (120 Гц, бас), Середина (2.2 кГц, розбірливість мови), Верх (7.5 кГц, яскравість/повітря), від -15 dB до +15 dB.
- 🎛️ **Інтеграція з віртуальним кабелем WASAPI**  
  Пряма сумісність із **VB-Audio CABLE** та DirectShow Capture Filter для чистої передачі звуку 48 кГц 16-біт стерео.

### 🎭 Набір ефектів спотворення (Troll FX)
- **Nuclear Flashbang:** Експоненційний ефект світлошумової гранати із засліпленням.
- **144p Pixelate:** Динамічна пікселізація кадру під стиль старих камер спостереження.
- **VHS Glitch:** Горизонтальні розриви рядків та аналогові артефакти відеокасети.
- **90s Bitcrush:** Квантування палітри кольорів у ретро-стиль 16-бітних приставок.
- **5 FPS Slideshow:** Імітація значної втрати мережевих пакетів і підвисання картинки.

### 🖥️ Три стилі інтерфейсу та збереження налаштувань
- 🎨 **3 змінні теми оформлення (`config.json`)**:
  - **Варіант 1 — Arcane Cyber (`index.html`):** Футуристичний неоновий HUD зі скошеними гранями.
  - **Варіант 2 — Motion Glass (`index2.html`):** Сучасний Bento-дизайн з пружною анімацією Еміля Ковальскі та матовим склом.
  - **Варіант 3 — Classic Aqua (`index3.html`):** Автентичний ретро-стиль Mac OS X Aqua з полосатим фоном і гелевими кнопками.
- 💾 **Надійна синхронізація конфігурації (`ConfigManager`)**:
  - Автоматичне збереження усіх 21 параметрів у фоновому режимі при зміні повзунків чи перемикачів.
  - Безпечний атомарний запис через `.tmp` файл, що запобігає пошкодженню налаштувань у разі вимкнення ПК.
- 📥 **Робота в системному треї та прихована консоль**:
  - Опція **Згортати в трей** (`close_to_tray`): програма ховається в системну область сповіщень, відеопотік не переривається.
  - Вікно консолі діагностики приховане за замовчуванням (`SW_HIDE`), перемикається з налаштувань та меню трею.
- 📊 **Оверлей телеметрії HUD, що приховується (v2.2.0)**:
  - Відображення FPS, бітрейту, температури батареї та роздільної здатності.
  - **Вимкнений за замовчуванням при запуску**. Вмикається за бажанням кнопкою в панелі режимів, перемикачем у налаштуваннях або хоткеєм `<kbd>Ctrl</kbd> + <kbd>Shift</kbd> + <kbd>H</kbd>` зі збереженням у `config.json`.
- ⌨️ **Глобальні гарячі клавіші Windows**:
  - <kbd>Ctrl</kbd> + <kbd>Shift</kbd> + <kbd>M</kbd> : Увімкнути / вимкнути мікрофон
  - <kbd>Ctrl</kbd> + <kbd>Shift</kbd> + <kbd>B</kbd> : Шторка приватності (Повний блекаут)
  - <kbd>Ctrl</kbd> + <kbd>Shift</kbd> + <kbd>C</kbd> : Перемкнути камеру (Фронтальна / Основна)
  - <kbd>Ctrl</kbd> + <kbd>Shift</kbd> + <kbd>H</kbd> : Показати / сховати телеметрію (FPS, градуси, бітрейт)
  - <kbd>Ctrl</kbd> + <kbd>Shift</kbd> + <kbd>T</kbd> : Перемкнути ефект спотворення (Troll FX)

---

## 🚀 Швидкий старт

### Варіанти встановлення
- 📦 **Рекомендований (в один клік):** Завантажте та запустіть **`VirtualCamNative_Setup_v2.2.0.exe`** з розділу [Releases](https://github.com/dimalinau-lab/Virtual-Camera/releases). Інсталятор автоматично встановить бібліотеки Visual C++ Redistributable та зареєструє COM-драйвер віртуальної камери (`NativeMFVirtualCam.dll`).
- 🛠️ **Портативна версія:** Розпакуйте архів та виконайте разову реєстрацію `regsvr32.exe /s bin\NativeMFVirtualCam.dll` від імені адміністратора.

### Системні вимоги
- **ОС:** Windows 10 або Windows 11 (64-біт).
- **Мобільний додаток:** Встановлений [Virtual-Camera-Android](https://github.com/dimalinau-lab/Virtual-Camera-Android) на телефоні (або файл `VirtualCam-v2.2.0.apk`).
- **Visual C++ Redistributable:** 2015–2022 (x64) *(входить до інсталятора)*.
- **VB-Audio Cable:** *(Опціонально, для маршрутизації звуку мікрофона)*.

### Підключення через USB (Найкраща якість та стабільність)
1. Увімкніть **Налагодження через USB** на телефоні (*Налаштування -> Для розробників -> Налагодження через USB*).
2. Підключіть смартфон до ПК якісним USB-кабелем.
3. Запустіть `VirtualCamNative.exe`.
4. Натисніть **USB Connect** — порти `8080`, `8554` та `8555` будуть перенаправлені автоматично через ADB.

### Бездротове підключення через Wi-Fi
1. Переконайтеся, що ПК та смартфон підключені до однієї мережі Wi-Fi (бажано 5 ГГц).
2. Запустіть `VirtualCamNative.exe`. Смартфон виявиться автоматично через UDP-маяк.
3. Оберіть знайдений пристрій у верхньому списку та натисніть **Wi-Fi Connect**.
4. Під час першого з'єднання натисніть **Дозволити** на екрані телефона.

---

## 🛠️ Компіляція з вихідного коду

```cmd
:: Клонування репозиторію
git clone https://github.com/dimalinau-lab/Virtual-Camera.git
cd Virtual-Camera

:: Створення build-конфігурації CMake в режимі Release x64
cmake -B out/build/x64-release -S . -G "Ninja" -DCMAKE_BUILD_TYPE=Release

:: Збирання проекту
cmake --build out/build/x64-release --config Release
```

Вихідні бінарні файли та ресурси інтерфейсу будуть скомпільовані в папку `bin/`.

---

## 🔧 Реєстрація віртуального COM-драйвера

```cmd
:: Реєстрація віртуальної камери та мікрофона (запуск від імені Адміністратора)
regsvr32.exe /s bin\NativeMFVirtualCam.dll

:: Видалення реєстрації драйвера
regsvr32.exe /u /s bin\NativeMFVirtualCam.dll
```

---

## 📄 Ліцензія

Проект розповсюджується під вільною ліцензією **MIT**. Дивіться файл [`LICENSE`](LICENSE) для отримання детальної інформації.