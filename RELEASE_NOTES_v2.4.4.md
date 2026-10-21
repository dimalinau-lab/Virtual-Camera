# VirtualCamNative v2.4.4 - Portrait Video Plane Decoupling & Green Artifact Elimination

## 🚀 What's New in v2.4.4

### 🖥️ Windows Desktop Client (`VirtualCamNative`)
- **Resolved Portrait Green Vertical Stripe (`Y=0, U=0, V=0`) Artifact**:
  - **Hardware Frame Strides & Plane Decoupling**: Decoupled Y and UV chroma plane uploads in `D3D11OpticsPipeline::processNv12ToNv12`, explicitly respecting `AVFrame::linesize[0]` and `AVFrame::linesize[1]`.
  - **Eliminated Fixed Offset Plane Assumption**: Stopped assuming the UV plane resides at `data[0] + width * height`, which caused Direct3D11VA and DXVA2 hardware decoders to sample zeroed padding memory and create a vertical green stripe on Samsung Galaxy and other portrait-oriented mobile streams.
  - **Surgical Shader Coordinate Clamping**: Added `[0.0005, 0.9995]` texture coordinate boundary clamping in `SampleNv12` HLSL shader to prevent border bleed during non-standard aspect ratio rotations.
  - **Format-Agnostic CPU Fallback**: Direct pass-through of decoder planes and pitch metadata directly into `sws_scale` for seamless fallback operation.

### 📱 Android Companion App (`Virtual-Camera-Android` / `ccamera`)
- **Version Alignment & Codec Sync**:
  - Bumped Android companion client to v2.4.4 (`versionCode 244`).
  - Seamless low-latency streaming compatibility across Samsung, Pixel, and Xiaomi devices.

---

## 📦 Assets Included
1. `VirtualCamNative_Setup_v2.4.4.exe` — Windows 10/11 Full Inno Setup Installer (~75 MB)
2. `VirtualCamNative_Update.zip` — Fast Hot-Swap binary update package (~0.7 MB)
3. `VirtualCamNative.exe` — Standalone native executable (~0.85 MB)
4. `VirtualCam-v2.4.4.apk` — Android 8.0+ Client APK (versionCode 244, ~6.6 MB)
5. `SHA256SUMS.txt` — Cryptographic SHA-256 checksums for all release assets
