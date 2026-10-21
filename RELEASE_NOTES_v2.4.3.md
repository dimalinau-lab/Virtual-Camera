# VirtualCamNative v2.4.3 - In-Place Hot-Swap Perfection & Version UI Normalization

## 🚀 What's New in v2.4.3

### 🖥️ Windows Desktop Client (`VirtualCamNative`)
- **Seamless Atomic Hot-Swap Updater**:
  - **Iterator Invalidation Guard**: Pre-scans all pending update files into an immutable vector before applying file modifications, preventing iterator truncation and aborts during directory updates.
  - **Timestamped Backup Rotation (`.<timestamp>.old`)**: Eliminates `ERROR_ACCESS_DENIED` during consecutive updates or restarts by assigning unique collision-free timestamps to replaced executables and DLLs.
  - **Dual Launch Strategy**: Combines `CreateProcessW` with a graceful `ShellExecuteExW` fallback to ensure seamless post-update restart on all Windows 10 and 11 privilege levels.
  - **HTTP Response Grace Period**: Allocated a 400 ms flush window to guarantee the web client receives `{"status":"launching"}` before parent process termination.
- **UI Version String Normalization**:
  - Eliminated the redundant double `vv` prefix in update notification pills, toasts, and dialogs (now strictly `v2.4.2 ➔ v2.4.3`).
  - Added visual error recovery with a dedicated "Retry" action if file replacement is interrupted.

### 📱 Android Companion App (`Virtual-Camera-Android` / `ccamera`)
- **Version Alignment & Stability**:
  - Synchronized Android companion client to v2.4.3 (`versionCode 243`).
  - Full compatibility with the refined hot-swap desktop pipeline and thermal management.

---

## 📦 Assets Included
1. `VirtualCamNative_Setup_v2.4.3.exe` — Windows 10/11 Full Inno Setup Installer (~75 MB)
2. `VirtualCamNative_Update.zip` — Fast Hot-Swap binary update package (~0.7 MB)
3. `VirtualCamNative.exe` — Standalone native executable (~0.85 MB)
4. `VirtualCam-v2.4.3.apk` — Android 8.0+ Client APK (versionCode 243, ~6.6 MB)
5. `SHA256SUMS.txt` — Cryptographic SHA-256 checksums for all release assets
