# VirtualCamNative v2.2.0 — Samsung Flagships Optimization, HUD Telemetry Toggle & Audio DSP

## 🚀 What's New in v2.2.0

### 📱 Android Companion App (`ccamera`)
- **Samsung Galaxy & Modern Flagship Inset Clearance**:
  - Dynamically calculates status bar height, display punch-hole cutouts, and rounded corners using `WindowInsetsCompat`.
  - Fully calibrated for **Samsung Galaxy S22, S23, S24, S22/S23/S24 Ultra, Note, Nothing Phone, and Pixel** devices.
  - Badges ("В ЭФИРЕ", "32°C") never clip display edges or overlap with Samsung One UI green privacy camera dot indicators.
- **Glassmorphic Floating Action Bar**:
  - Quick action controls for lens flipping, floating PiP widget mode, and Eco Blackout.
- **Floating PIP & Multi-App Overlay Widget**:
  - Draggable preview overlay running above any other application with one-tap mic mute and lens flip.
- **Hardware Battery Temperature Monitor**:
  - Real-time battery sensor telemetry with severe overheating alerts (`🔥 ПЕРЕГРЕВ` at >= 42°C).

### 🖥️ Windows Desktop Client (`VirtualCamNative`)
- **HUD Telemetry Overlay Toggle**:
  - On-screen telemetry showing FPS, bitrate, battery temperature, and resolution.
  - **OFF by default on startup** to keep the workspace clean and unburdened; only visible when explicitly enabled by the user via the Director Bar button, Settings switch, or global hotkey `Ctrl + Shift + H`.
  - State immediately persists to `config.json`.
- **Advanced Audio DSP Suite**:
  - Studio Noise Gate with exponential decay.
  - RNNoise AI Neural denoiser for suppressing fan and keyboard noise while preserving vocal clarity.
  - Automatic Gain Control (AGC) and soft-knee limiter.
  - Mechanical keyboard and mouse switch De-Clicker.
  - 3-Band Parametric EQ (120 Hz Bass, 2.2 kHz Voice, 7.5 kHz Air).
- **Studio Optics 3D LUT Color Engine**:
  - D3D11 GPU-accelerated and OpenMP SIMD processing.
  - Brightness, Contrast, Saturation, and Color Temperature warmth controls.
  - 6 cinematic 3D LUT profiles (Neutral, Cine Teal & Orange, Golden Hour, Emerald Matrix, Noir B&W, Cyber Neon).
  - Digital Zoom (1.0x to 4.0x) with Pan (X, Y) framing.
- **Triple UI Skin System**:
  - Variant 1: Arcane Cyber (`index.html`).
  - Variant 2: Motion Glass (`index2.html`).
  - Variant 3: Classic Aqua (`index3.html`).

---

## 📦 Assets Included
1. `VirtualCamNative_Setup_v2.2.0.exe` — Windows 10/11 Full Installer (78 MB)
2. `VirtualCam-v2.2.0.apk` — Android 8.0+ Client APK (9.5 MB)
