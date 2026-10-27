# VirtualCamNative v2.4.5 — Bug Fixes & Reliability Release

## 🚀 Key Fixes & Improvements

### 1. Dynamic GUI Version Resolution
- **Issue:** The user interface in `index2.html` and `index3.html` previously displayed a hardcoded `v2.3.0` version string regardless of the actual application build installed.
- **Fix:** Assigned element ID `appCurrentVersionBadge` to GUI headers and bound dynamic polling via `/api/update/status`. The GUI now automatically queries the native backend and renders the exact running version (`v2.4.5`).

### 2. Window-Scoped Hotkeys (Browser Shortcut Non-Interference)
- **Issue:** OS-level global registration via WinAPI `RegisterHotKey(MOD_CONTROL | MOD_SHIFT, ...)` intercepted key combinations system-wide, breaking critical browser shortcuts (e.g., `Ctrl+Shift+T` to restore closed tabs, `Ctrl+Shift+B` for bookmarks bar, `Ctrl+Shift+P` for Private browsing).
- **Fix:** Removed all global OS-wide hotkey hooks. Hotkey detection is now scoped strictly to the VirtualCamNative WebView2 window. Key strokes are intercepted only when the VirtualCamNative window is focused and active.

### 3. Detached Autonomous Background Auto-Updater
- **Issue:** The in-process updater attempted atomic file swapping while DLLs (`NativeMFVirtualCam.dll`, `WebView2Loader.dll`) and `VirtualCamNative.exe` were locked in memory. Relaunching before the old process exited resulted in HTTP/UDP socket collision (ports 8000 and 8888) and update abortion.
- **Fix:** Replaced in-process replacement with an autonomous detached `.cmd` updater script:
  - Actively waits for the target PID to terminate via `tasklist`.
  - Performs file synchronization via `robocopy /E /IS /IT /R:5 /W:1` to cleanly replace all binaries, drivers, and UI assets.
  - Automatically requests UAC elevation (`runas`) when installed in protected directories (e.g., `Program Files`).
  - Restarts `VirtualCamNative.exe` after ports are freed and self-cleans.

---

## 📦 Artifacts & Checksums
- `VirtualCamNative_Setup_v2.4.5.exe` — Full Windows x64 Inno Setup Installer.
- `VirtualCamNative_Update.zip` — Lightweight hot-swap update archive for automatic updates.
- `VirtualCamNative.exe` — Standalone portable executable.
- `SHA256SUMS.txt` — Cryptographic SHA-256 verification signatures for all release assets.
