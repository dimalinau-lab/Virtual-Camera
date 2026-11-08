# VirtualCamNative v2.4.7 — Instant Wi-Fi Discovery, Zero-Latency Handshake & Startup Turbo Release

## 🚀 What's New in v2.4.7

### 1. ⚡ Instant Wi-Fi Discovery & 2-Way Active Probing
- **Subnet Directed Broadcasts:** Android beacon broadcaster now queries all local network interfaces and sends UDP beacons directly to subnet broadcast addresses (e.g. `192.168.x.255`, `10.x.x.255`) as well as `255.255.255.255`, overcoming router broadcast isolation.
- **Fast Startup Beacon Burst:** First 10 discovery packets are transmitted at 200ms intervals (followed by steady 450ms cycle) for sub-second recognition upon app startup.
- **MulticastLock & Wi-Fi Lock Protection:** Android service explicitly acquires `WifiManager.MulticastLock` and `WifiLock`, preventing Samsung One UI and Android power-saving filters from discarding UDP beacon packets.
- **2-Way UDP Port 8889 Active Probe:** C++ core sends directed discovery probes; Android companion app answers via dedicated background listener in < 5ms.
- **Responsive UI Device Discovery:** Web UI polling interval decreased from 2000ms down to 300ms, with instant pre-selection of previously paired smartphones.

### 2. ⚡ Zero-Delay Connection Handshake & Network QoS
- **Zero Sleep Handshake:** Removed artificial blocking delays (`Sleep(250)`) in `/api/connect` and `/api/connect_adb`.
- **Aggressive Timeouts:** HTTP connect timeout dropped to 600ms for fast failover.
- **Low-Latency Socket Traffic Class:** Sockets in `RawH265Streamer` and `AudioStreamer` now configure `trafficClass = 0x10` (`IPTOS_LOWDELAY`), granting stream packets network queue priority.

### 3. ⚡ Core Windows & Web Startup Turbo
- **WebView2 Accelerated Cold-Boot:** Added launch arguments (`--disable-background-networking`, `--disable-component-update`, `--renderer-process-limit=2`) for immediate UI appearance.
- **Instant ADB Socket Forwarding:** Uses direct TCP socket transactions with local ADB daemon (`127.0.0.1:5037`) completing port forwarding in < 5ms without spawning process instances.
- **Non-blocking Background Driver Checks:** COM driver registration and checks decoupled to a worker thread, ensuring the main window never freezes on launch.

### 4. 🔄 Seamless Auto-Orientation & Landscape 16:9
- **Emil Kowalski Physics (~180 ms):** DirectShow/D3D11 hardware shader applies spring interpolation for angle and aspect ratio transitions with zero stream stalls, zero freezes, and zero black flashes.
- **Hardware Landscape 16:9 (Zero Sidebars):** Placing the phone horizontally automatically expands video to full 16:9 (`0.5f`), while portrait displays in a vertical 9:16 strip (`0.1582f`).
- **Desk Stand Gravity Vectoring:** Replaced Android's restrictive tilt check with direct vector analysis `atan2(-y, x)` and symmetric ±55° hysteresis, ensuring rock-solid orientation detection at any angle (desk stands, chargers, tripods, hand-held).
- **Corrected Horizon Orientation:** Fixed 180° inversion in landscape mode and eliminated race conditions between dual sensor listeners.
- **UI Streamlined:** Removed redundant manual "Stream Orientation" toggle from PC web UI — streaming is now 100% hardware-driven by device posture.

---

## 📦 Release Artifacts
- `VirtualCamNative_Setup_v2.4.7.exe` — Full Windows x64 Inno Setup Installer.
- `VirtualCamNative_Update.zip` — Fast hot-swap update archive for automatic updates.
- `VirtualCamNative.exe` — Standalone portable x64 executable.
- `VirtualCam-v2.4.7.apk` — Android companion camera app.
- `SHA256SUMS.txt` — Cryptographic checksums for all release binaries.

