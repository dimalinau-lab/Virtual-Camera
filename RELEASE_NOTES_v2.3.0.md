# VirtualCamNative v2.3.0 — Seamless Hot-Swap Auto-Updater, D3D11 Optics & Audio DSP

## 🚀 What's New in v2.3.0

### 🖥️ Windows Desktop Client (`VirtualCamNative`)
- **Seamless Hot-Swap Auto-Updater (`VirtualCamNative_Update.zip`)**:
  - Lightning-fast in-place updater replacing full ~78 MB installer downloads with a lightweight **~0.89 MB ZIP package**.
  - Automatically queries the GitHub Releases API via secure WinHTTP HTTPS.
  - Downloads, verifies, and stages binaries and web UI assets into a temporary `_update/` folder.
  - Performs an atomic hot-swap and application restart in under **2 seconds** with zero user friction.
  - Retains graceful fallback to the full Inno Setup installer (`VirtualCamNative_Setup_v2.3.0.exe`) if chosen or required.
  - Dedicated settings card with auto-check toggle, manual check button, animated progress bar, and instant update modal.
- **Direct3D 11 (D3D11) Hardware Pipeline**:
  - GPU shader-accelerated color grading, 3D LUT matrix processing, and digital zoom with zero-copy DirectShow shared memory frame transfer.
- **Advanced Audio DSP Suite**:
  - Integrated RNNoise AI neural noise suppression.
  - Mechanical keyboard & mouse switch Transient De-Clicker (-16 dB).
  - Automatic Gain Control (AGC) with soft-knee limiter (+12 dB boost).
  - 3-Band Parametric Equalizer (120 Hz, 2.2 kHz, 7.5 kHz).
- **Minimalist Telemetry HUD Toggle**:
  - Live FPS, bitrate, battery temp, and resolution overlay.
  - Disabled by default on startup; toggleable via Director button, Settings drawer, or hotkey `<kbd>Ctrl</kbd> + <kbd>Shift</kbd> + <kbd>H</kbd>` with persistent state in `config.json`.
- **Trilingual UI & Triple Design System (v2.3.0)**:
  - English, Russian, and Ukrainian fully localized.
  - Version indicator badges updated across all 3 themes (Arcane Cyber, Motion Glass, Classic Aqua).

### 📱 Android Companion App (`ccamera`)
- **Version Bump v2.3.0 (versionCode 230)**:
  - Synchronized versioning with desktop client.
  - Flagship Samsung Galaxy punch-hole inset clearance and green camera indicator dot avoidance.
  - Floating PiP preview widget and real-time battery sensor telemetry.

---

## 📦 Assets Included
1. `VirtualCamNative_Setup_v2.3.0.exe` — Windows 10/11 Full Inno Setup Installer (~75 MB)
2. `VirtualCamNative_Update.zip` — Fast Hot-Swap binary update package (~0.89 MB)
3. `VirtualCam-v2.3.0.apk` — Android 8.0+ Client APK (versionCode 230, ~6.35 MB)
