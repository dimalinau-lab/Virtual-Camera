# VirtualCamNative (PC Client & Driver) 🚀

> **Ultra-Low-Latency, High-Performance DirectShow & Media Foundation Virtual Camera for Windows powered by Android hardware acceleration.**

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg?style=for-the-badge&logo=cplusplus)](https://isocpp.org/)
[![DirectShow](https://img.shields.io/badge/Driver-DirectShow%20%2F%20MF-orange.svg?style=for-the-badge)](https://docs.microsoft.com/en-us/windows/win32/directshow/directshow)
[![FFmpeg](https://img.shields.io/badge/Decoder-FFmpeg%20HEVC-green.svg?style=for-the-badge&logo=ffmpeg)](https://ffmpeg.org/)
[![Windows](https://img.shields.io/badge/Windows-10%2F11-0078D6.svg?style=for-the-badge&logo=windows)](https://www.microsoft.com/)
[![Android App](https://img.shields.io/badge/Companion-Virtual--Camera--Android-3DDC84.svg?style=for-the-badge&logo=android)](https://github.com/dimalinau-lab/Virtual-Camera-Android)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg?style=for-the-badge)](LICENSE)

**VirtualCamNative** is an open-source, ultra-low-latency desktop client and virtual camera driver engineered in pure **C++20**. Designed as a high-performance replacement for commercial tools like DroidCam and Iriun, it receives hardware-accelerated **HEVC (H.265)** streams over USB or Wi-Fi, decodes them via FFmpeg, and exposes a native **DirectShow / Media Foundation virtual camera device** recognized across **Discord, OBS Studio, Zoom, Telegram, and WebRTC browsers** with **~7–10ms glass-to-glass latency**.

---

## 🌟 Key Features

- ⚡ **Ultra-Low Latency Pipeline (~7–10 ms)**  
  Fast single-pass color conversion using optimized scaling routines (`SWS_POINT`) and zero unnecessary heap allocations in the hot rendering path.
- 🛡️ **Anti-Bufferbloat Socket Management**  
  Monitors network queues via non-blocking inspection (`ioctlsocket(FIONREAD)`) and purges stale NAL units to eliminate accumulated streaming delay over Wi-Fi/USB.
- 🎥 **System-Wide DirectShow & Media Foundation Driver**  
  A native C++ filter (`NativeMFVirtualCam.dll`) feeding frames through synchronized **Windows Shared Memory** (Memory Mapped Files + Win32 Events) directly into consuming applications.
- 🚀 **Hardware HEVC / H.265 Decoding**  
  FFmpeg hardware acceleration pipeline configured for minimum decoding overhead and extremely low CPU consumption.
- 🛰️ **Seamless Dual Connectivity**  
  - **USB Mode:** Automatic ADB port forwarding (`tcp:8080`, `tcp:8554`) without manual command-line configuration.  
  - **Wi-Fi Mode:** Zero-config auto-discovery via background **UDP Beacon Scanner** (`255.255.255.255:8888`).
- 🔄 **Cache-Friendly 90° Frame Rotation**  
  Custom 32x32 block-based transformation algorithm (`transformPortraitFrame`) executing portrait-to-landscape adaptation in **~0.25 ms**.
- 🎛️ **Modern Desktop GUI**  
  Built with **Microsoft Edge WebView2**, supporting dynamic camera switching, hardware blackout control, bitrate/FPS updates, and a local MJPEG preview stream.

---

## 📊 Comparison with Existing Solutions

| Feature | **VirtualCamNative (C++)** | DroidCam | Iriun Cam |
| :--- | :---: | :---: | :---: |
| **Language & Runtime** | **Native C++20 (No Python/Java)** | C++ / C# | C++ / Objective-C |
| **Video Codec** | **HEVC / H.265 (Hardware)** | H.264 / MJPEG | H.264 / HEVC |
| **Latency (USB)** | **~7–10 ms** | ~40–70 ms | ~30–50 ms |
| **Driver Implementation** | **DirectShow & Media Foundation** | DirectShow | DirectShow |
| **Network Bufferbloat Dropping** | **Yes (Automatic Queue Guard)** | No | Limited |
| **Wi-Fi Pairing** | **Auto-Discovery (UDP :8888)** | Manual IP Entry | mDNS / Bonjour |
| **License** | **100% Free & Open Source (MIT)** | Proprietary (Freemium) | Proprietary (Watermarked) |

---

## 📐 System Architecture

```
 +-------------------------------------------------------------------------------+
 |                        ANDROID CLIENT (HARDWARE SOURCE)                       |
 |             ( https://github.com/dimalinau-lab/Virtual-Camera-Android )        |
 |                                                                               |
 | [ CameraX Source ] ---> [ MediaCodec H.265 ] ---> [ Raw TCP Server :8554 ]    |
 |                                                                               |
 | [ NanoHTTPD :8080 ] <--- REST Commands --- [ UDP Discovery Beacon :8888 ]     |
 +-------------------------------------|-----------------------------------------+
                                       | TCP Raw NALU / HTTP REST / UDP Beacon
                                       v
 +-------------------------------------------------------------------------------+
 |                        VIRTUALCAMNATIVE PC CLIENT (C++20)                     |
 |                                                                               |
 |  [ TcpReceiver ]                                                              |
 |         │                                                                     |
 |         ▼ (Anti-Bufferbloat Queue Guard)                                      |
 |  [ NvdecDecoder (FFmpeg) ]                                                    |
 |         │                                                                     |
 |         ▼                                                                     |
 |  [ Block Rotator (0.25ms) / SWS_POINT ] ---> [ Local MJPEG Stream :8000 ]     |
 |         │                                                 │                   |
 |         ▼                                                 ▼                   |
 |  [ Win32 Shared Memory MMF ]                      [ WebView2 GUI ]            |
 +-------------------------------------|-----------------------------------------+
                                       | Frame Buffer Pointer + Win32 Event
                                       v
 +-------------------------------------------------------------------------------+
 |                  NATIVE VIRTUAL CAMERA DRIVER (DIRECTSHOW / MF)               |
 |                                                                               |
 |  [ NativeMFVirtualCam.dll ] <--- Reads Shared Memory Frame Buffer             |
 +-------------------------------------|-----------------------------------------+
                                       | DirectShow Graph Capture Pin
                                       v
 +-------------------------------------------------------------------------------+
 |                            CONSUMING APPLICATIONS                             |
 |                                                                               |
 |  Discord  |  OBS Studio  |  Zoom  |  Telegram  |  WebRTC Browsers             |
 +-------------------------------------------------------------------------------+
```

---

## 🚀 Quick Start

### Prerequisites
- **Operating System:** Windows 10 or Windows 11 (64-bit).
- **Companion App:** [Virtual-Camera-Android](https://github.com/dimalinau-lab/Virtual-Camera-Android) installed and running on your smartphone.
- **ADB Tools:** Android platform tools (`adb.exe` included in `redist/` or via Android SDK).

### USB Connection (Lowest Latency)
1. Enable **USB Debugging** on your smartphone (*Settings -> Developer Options -> USB Debugging*).
2. Connect your phone to your PC via a USB cable.
3. Launch `VirtualCamNative.exe`.
4. Click **Connect via USB** — ports `8080` and `8554` will be forwarded automatically.

### Wi-Fi Connection (Wireless)
1. Ensure both your PC and phone are connected to the same local network subnet.
2. Launch `VirtualCamNative.exe`.
3. Click **Connect via Wi-Fi** — the client scans for UDP beacons on port `8888` and connects automatically.

---

## 🌐 Local & Remote REST Control API

The desktop client exposes a local HTTP API on port `8000` while proxying control actions to the Android device on port `8080`:

| Endpoint | Method | Request Body / Query | Description |
| :--- | :---: | :--- | :--- |
| `/api/connect_adb` | `POST` | - | Sets up ADB port forwards (`8080`, `8554`) and initiates the USB streaming pipeline. |
| `/api/connect` | `POST` | `{"ip": "192.168.1.X"}` *(Optional)* | Discovers phone via UDP (or connects to explicit IP) and starts Wi-Fi stream. |
| `/api/disconnect` | `POST` | - | Terminates video streaming threads and notifies the device. |
| `/api/status` | `GET` | - | Proxies device connection status, active camera, and orientation. |
| `/api/telemetry` | `GET` | - | Returns live telemetry: `{"fps": 30.0, "bitrate": "Active", "codec": "H.265"}`. |
| `/api/phone/action/(.*)` | `POST` | `{"action": "switch_camera" \| "toggle_torch" \| "toggle_blackout"}` | Dispatches hardware commands directly to the phone. |
| `/api/orientation` | `GET` | `?mode=vertical \| horizontal` | Switches rendering projection without resetting encoder or socket connections. |
| `/stream` | `GET` | - | Multipart MJPEG video preview feed for the desktop application window. |

---

## 🛠️ Building from Source

### Requirements
- **IDE:** Visual Studio 2022 (Desktop development with C++ v143, C++20 standard).
- **Windows SDK:** 10.0.22000.0 or higher.
- **Libraries:** FFmpeg 6.x / 7.x Development Libraries (`libavcodec`, `libavutil`, `libswscale`).
- **UI Framework:** Microsoft.Web.WebView2 NuGet package.

### Build Steps
1. Clone the repository:
   ```bash
   git clone https://github.com/dimalinau-lab/Virtual-Camera.git
   cd Virtual-Camera
   ```
2. Open `VirtualCamNative.sln` in Visual Studio 2022.
3. Set build configuration to **Release | x64**.
4. Build Solution: <kbd>Ctrl</kbd> + <kbd>Shift</kbd> + <kbd>B</kbd>.

### Virtual Camera DLL Registration
The application attempts self-registration on startup. To register or unregister the filter manually from an elevated Command Prompt (Administrator):

```cmd
:: Register the virtual camera driver
regsvr32.exe /s bin\Release\NativeMFVirtualCam.dll

:: Unregister the virtual camera driver
regsvr32.exe /u /s bin\Release\NativeMFVirtualCam.dll
```

---

## 🔧 Troubleshooting

<details>
<summary><b>1. DirectShow Driver / Camera Filter not showing in Discord or OBS</b></summary>
<br>

Ensure `NativeMFVirtualCam.dll` has been registered with Administrator permissions:
```cmd
regsvr32.exe "%cd%\NativeMFVirtualCam.dll"
```
Completely close and reopen Discord, OBS, or your browser after registration.
</details>

<details>
<summary><b>2. Frame rate stutters or drops on Wi-Fi</b></summary>
<br>

- Use a 5 GHz Wi-Fi band; 2.4 GHz channels suffer from heavy packet retransmissions.
- The built-in **Anti-Bufferbloat** mechanism automatically drops stale frames if socket queues exceed 64 KB, keeping video real-time.
</details>

<details>
<summary><b>3. USB ADB Connection Fails</b></summary>
<br>

Check that ADB detects your device:
```cmd
adb devices
```
If the device list is empty, verify USB Debugging is enabled and your device's USB driver is installed properly on Windows.
</details>

---

## 📱 Companion Android App

For the mobile streamer component, see the companion repository:  
👉 **[Virtual-Camera-Android on GitHub](https://github.com/dimalinau-lab/Virtual-Camera-Android)**

---

## 📄 License

This project is licensed under the **MIT License**. See the [`LICENSE`](LICENSE) file for details.
