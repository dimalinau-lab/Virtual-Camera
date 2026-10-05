# VirtualCamNative v2.4.0 - AVX2 60 FPS Interpolation, Unlocked Selfie 60 FPS & Universal Aspect Ratio

## 🚀 What's New in v2.4.0

### 🖥️ Windows Desktop Client (`VirtualCamNative`)
- **AVX2 SIMD 60 FPS Frame Interpolation (`blendNv12Buffer`)**:
  - Leverages hardware-accelerated AVX2 vector SIMD blending (`_mm256_avg_epu8`) to synthesize ultra-smooth intermediate frames in **< 0.03 ms** without CPU bottlenecks.
  - Automatically activates when 60 FPS is targeted on smartphones whose hardware HAL restricts camera preview to ~30 FPS, delivering smooth 60.0 FPS output into Discord, OBS Studio, Zoom, and Telegram.
  - EMA jitter filter (`smoothIntervalMs = smoothIntervalMs * 0.9f + frameIntervalMs * 0.1f`) prevents Wi-Fi packet jitter from fluttering interpolation.
  - Passes native 60 FPS streams through directly with zero synthetic blending.
- **Universal Aspect Ratio (Landscape & Portrait)**:
  - Added full Aspect Ratio support (4:3 Classic, 9:16 Phone/Shorts, 16:9 Widescreen) for **horizontal (landscape) stream orientation**.
  - Natural 1:1 distortion-free proportions: circles and faces remain geometrically accurate without horizontal squishing.
  - D3D11 hardware shader accelerated pillarbox masking (< 1.0 ms GPU render time).
  - Synchronized CPU fallback with automatic border blanking and buffer clearing on mode switches.
- **Hot-Swap In-Place Auto-Updater (`VirtualCamNative_Update.zip`)**:
  - High-efficiency ~0.89 MB ZIP payload for atomic updates without re-running full installers.

### 📱 Android Companion App (`ccamera`)
- **Version Bump v2.4.0 (versionCode 240)**:
  - Removed artificial 30 FPS ceiling on Front (Selfie) Camera across Android service and Web UI.
  - Enabled 60 FPS target and `[60, 60]` AE range support for both rear and selfie sensors.
  - Persistent 1080p resolution and camera facing state across app restarts (`SharedPreferences`).
  - Improved connection resilience and memory management.

---

## 📦 Assets Included
1. `VirtualCamNative_Setup_v2.4.0.exe` — Windows 10/11 Full Inno Setup Installer (~75 MB)
2. `VirtualCamNative_Update.zip` — Fast Hot-Swap binary update package (~0.89 MB)
3. `VirtualCam-v2.4.0.apk` — Android 8.0+ Client APK (versionCode 240, ~6.66 MB)
