# VirtualCamNative v2.4.6 — Samsung 0.5x Ultra-Wide, UI Centering & Seqlock Pipeline

## 🚀 Key Fixes & Improvements

### 1. Samsung 0.5x Ultra-Wide Seamless Multi-Camera Lens Switching
- **Issue:** On Samsung Galaxy devices (such as Galaxy A34 5G, A54, S-series) with MediaTek Dimensity or Exynos chipsets, switching to the 0.5x ultra-wide lens caused a complete video pipeline freeze. Calling `openCamera("2")` to target the physical ultra-wide sensor directly caused ISP deadlocks, dropped TCP frames, and codec stalls under 60 FPS capture sessions.
- **Fix:**
  - Implemented Camera2 Logical Multi-Camera zoom switching on Camera ID `0` (`CONTROL_ZOOM_RATIO = 0.5f`).
  - Added native Samsung vendor key compatibility (`samsung.android.scaler.zoomMapRatio = 500`).
  - Automatically dynamically clamps AE Target FPS range to `[30, 30]` when zoom $\le 0.75\times$ to respect ultra-wide sensor hardware limits.
  - Immediately transmits an IDR keyframe (`requestSyncFrame()`) upon lens switch to guarantee seamless instantaneous video rendering without stream interruption.

### 2. Studio Web UI Dock & Lens Selector Centering
- **Issue:** Shrinking or resizing the browser window caused `#capsuleDock` and `#previewLensSelector` in `index2.html` and `index3.html` to drift to the right edge rather than staying horizontally centered.
- **Fix:** Wrapped the dock and floating lens switcher in flexbox centering containers with `left-1/2 -translate-x-1/2`, ensuring rock-solid center alignment across all viewport widths.

### 3. Front Camera Zoom Normalization
- **Issue:** When switching from back camera telephoto lenses (2.0x, 3.0x, 5.0x) to the front selfie camera, the digital zoom multiplier was mistakenly retained, causing extreme close-up framing on selfie mode.
- **Fix:** Reset digital crop and zoom ratio back to clean `1.0x` whenever the front camera is selected.

### 4. Android Touch-Friendly Control Panel Pills & Spring Animation
- **Issue:** The previous 24dp chevron icon for collapsing the bottom controls was difficult to tap accurately during active streaming.
- **Fix:** Replaced the small chevron with ergonomic pill buttons ("Скрыть панель" / "Панель управления") styled with semi-transparent frosted glass, micro-haptics, and Emil Kowalski spring physics transitions (< 220ms).

### 5. Media Foundation Seqlock Shared Memory (`MFVirtualCamHeader`)
- **Issue:** High frame rate rendering into shared memory occasionally caused minor tearing when high-FPS consumer apps (OBS, Discord, Zoom) read memory mid-write.
- **Fix:** Integrated atomic seqlock synchronization (`InterlockedIncrement` on `seq`) and memory barriers into `mf_vcam_writer.hpp` and `NativeMFVirtualCam.dll`. Active readers verify sequence parity before and after reading, guaranteeing 100% tear-free frames.

### 6. Real-Time PTS Video/Audio Multiplexing
- **Fix:** Expanded the MagerCam protocol with unified 16-byte packet framing (`McPacketHeader`), allowing zero-latency interleaved audio PCM streaming (`PKT_AUDIO`) alongside H.264/H.265 video (`PKT_VIDEO_FRAME`) over a single port, complete with mutex-protected WASAPI playback.

---

## 📦 Artifacts & Checksums
| File | SHA-256 |
|---|---|
| `VirtualCamNative_Setup_v2.4.6.exe` | `02186e9a174e2501764a14228747392c17a470faf5fd71e5f37e293fb0a5477d` |
| `VirtualCamNative_Update.zip` | `2010c409daca9d1e951e56975f19a53dcc52a403039dbea61918ff2044a3036f` |
| `VirtualCamNative.exe` | `350a59cbc1f2b51e38b5a1cfa7dca87584c5faffe8c4e46d88a68f454611683c` |
| `VirtualCam-v2.4.6.apk` | `544ae0cb4a6da8f150f4219078288b6acbbc57a4cc2b3e5a2f33f0a7774290e0` |
| `SHA256SUMS.txt` | Cryptographic SHA-256 verification signatures for all release assets |

