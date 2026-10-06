# VirtualCamNative v2.4.1 - Smart Floating Window Control & Lifecycle Stability

## 🚀 What's New in v2.4.1

### 📱 Android Companion App (`Virtual-Camera-Android` / `ccamera`)
- **Smart Floating Window Control & Indicator**:
  - Added dedicated glassmorphic status pill and action button with real-time LED indicator (Emerald Green when active, Zinc Gray when disabled).
  - Users can easily toggle between working over other apps (floating PiP/overlay) or full background sleep.
- **Graceful Background Suspension & Zero Battery Drain**:
  - When floating mode is disabled, minimizing the app gracefully pauses the Camera2 hardware sensor and halts H.265 encoding.
  - The phone's green camera privacy LED immediately turns off, releasing the camera sensor and preserving battery life without killing the application.
- **Instant Stream Resume**:
  - Restoring the app instantly rebinds the camera session and resumes encoding in < 20 ms without any reloading glitches or black screens.
- **Samsung One UI & Android 14 Lifecycle Fix**:
  - Resolved a critical lifecycle issue where synthetic `onUserLeaveHint` / `onStop` transitions during app launch caused unintended shutdown loops on modern Samsung devices.
  - Eliminated task suicide logic, ensuring rock-solid stability upon application restart.

### 🖥️ Windows Desktop Client (`VirtualCamNative`)
- Retains all AVX2 SIMD 60 FPS interpolation, Universal Aspect Ratio framing, and Studio Optics features from v2.4.0.
- Fully compatible with the updated Android v2.4.1 companion app protocol.

---

## 📦 Assets Included
1. `VirtualCam-v2.4.1.apk` — Android 8.0+ Client APK (versionCode 241, ~6.66 MB)
2. `VirtualCamNative_Setup_v2.4.0.exe` — Windows 10/11 Full Inno Setup Installer (~75 MB)
3. `VirtualCamNative_Update.zip` — Fast Hot-Swap binary update package (~0.89 MB)
