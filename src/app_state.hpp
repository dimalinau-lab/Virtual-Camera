#pragma once
#include <atomic>
#include <string>
#include <mutex>
#include <windows.h>

/**
 * @brief Unified thread-safe application state context (AppState).
 * Encapsulates global state, pipeline configurations, audio DSP flags, and UI parameters.
 */
struct AppState {
    // 1. Application Lifecycle & UI Modes
    std::atomic<bool> isAppRunning{ true };
    std::atomic<bool> isStreamActive{ false };
    std::atomic<bool> closeToTray{ true };
    std::atomic<bool> showConsole{ false };
    std::atomic<bool> connectRequested{ false };

    // 2. Stream Target & Connection
    std::string targetIp{ "127.0.0.1" };
    int targetPort{ 8554 };
    std::string targetMode{ "usb" };
    std::mutex targetMutex;

    // 3. Resolution, FPS & Video Stream
    std::atomic<int>  currentWidth{ 1280 };
    std::atomic<int>  currentHeight{ 720 };
    std::atomic<int>  currentFps{ 60 };
    std::atomic<bool> fpsChanged{ false };
    std::atomic<bool> resolutionChanged{ false };

    // 4. Orientation & Optical Geometry
    std::atomic<bool> mirrorEnabled{ false };
    std::atomic<bool> flip180{ false };
    std::atomic<bool> blurEnabled{ false };
    std::atomic<bool> isFrontCamera{ false };
    std::atomic<bool> isLandscapeMode{ false };
    std::atomic<int>  aspectRatioMode{ 0 };           // 0: 9:16 (Phone Portrait), 1: 4:3 (Classic), 2: 16:9 (Wide)

    // 5. Audio DSP Engine
    std::atomic<float> audioVolume{ 1.0f };
    std::atomic<bool>  audioMuted{ false };
    std::atomic<int>   audioDelayMs{ 0 };             // Lip-Sync delay (0 - 500 ms)
    std::atomic<bool>  noiseGateEnabled{ true };      // Studio Noise Gate
    std::atomic<float> noiseGateThreshold{ 0.015f };  // Gate threshold
    std::atomic<bool>  aiNoiseEnabled{ true };         // RNNoise AI denoiser
    std::atomic<bool>  agcEnabled{ true };             // Auto-Gain Control
    std::atomic<bool>  declickerEnabled{ true };       // Mechanical de-clicker
    std::atomic<float> eqLowDb{ 0.0f };                // Low Shelf (120 Hz)
    std::atomic<float> eqMidDb{ 0.0f };                // Mid Peak (2200 Hz)
    std::atomic<float> eqHighDb{ 0.0f };               // High Shelf (7500 Hz)

    // 6. AI Neural Background & Privacy Shield
    std::atomic<int>   bgEffectMode{ 0 };              // 0: Off, 1: Bokeh Blur, 2: Green Screen, 3: Dark Studio
    std::atomic<float> bgBlurRadius{ 8.0f };           // 1.0 .. 20.0 px
    std::atomic<float> bgEdgeSoftness{ 0.15f };
    std::atomic<float> bgThreshold{ 0.50f };
    std::atomic<bool>  privacyShield{ false };         // Blackout / Privacy Shield
    std::atomic<bool>  isTallyActive{ false };         // Tally Light: true when OBS/Discord captures virtual camera
    std::atomic<bool>  isFrozen{ false };              // Freeze Frame (Ctrl+Shift+F)

    // 7. Troll FX
    std::atomic<bool> trollFpsLimit{ false };          // 5 FPS Slideshow
    std::atomic<int>  trollPixelate{ 1 };             // Pixelation factor (1 = off)
    std::atomic<bool> trollGlitch{ false };           // Glitch line displacement
    std::atomic<bool> trollBitcrush{ false };         // 16-bit retro quantization
    std::atomic<bool> trollOverexposure{ false };     // Nuclear flashbang
    std::atomic<bool> trollGsmVoice{ false };         // 2G/GSM Cellphone TDMA buzz on speech
    std::atomic<bool> trollGsmBurstTrigger{ false };  // One-shot incoming call buzz burst
    std::atomic<bool> trollWalkieTalkie{ false };     // Military walkie-talkie + roger beep
    std::atomic<bool> trollRobotVoice{ false };       // Ring-modulated robot voice
    std::atomic<bool> trollCctv{ false };             // 90s Camcorder / CCTV OSD
    std::atomic<bool> trollFakeLag{ false };          // Random packet loss / stutter lag

    // Audio Input Device ("phone", "default", or specific WASAPI endpoint GUID)
    std::string audioInputDeviceId{ "phone" };
    std::atomic<bool> audioInputDeviceChanged{ false };

    // Singleton accessor
    static AppState& get() {
        static AppState instance;
        return instance;
    }

    // Helper functions for console & tray management
    void setConsoleVisible(bool visible) {
        showConsole.store(visible);
        HWND hConsole = GetConsoleWindow();
        if (hConsole) {
            ShowWindow(hConsole, visible ? SW_SHOW : SW_HIDE);
            if (visible) {
                SetForegroundWindow(hConsole);
            }
        }
    }

    bool isConsoleVisible() const {
        return showConsole.load();
    }

    void setCloseToTray(bool val) {
        closeToTray.store(val);
    }

    bool isCloseToTray() const {
        return closeToTray.load();
    }
};

// Global singleton reference alias
inline AppState& g_app = AppState::get();

// Zero-overhead transparent backward-compatibility aliases
inline std::atomic<bool>& g_isAppRunning = g_app.isAppRunning;
inline std::atomic<bool>& g_isStreamActive = g_app.isStreamActive;
inline std::atomic<bool>& g_closeToTray = g_app.closeToTray;
inline std::atomic<bool>& g_showConsole = g_app.showConsole;
inline std::atomic<bool>& g_connectRequested = g_app.connectRequested;

inline std::string& g_targetIp = g_app.targetIp;
inline int& g_targetPort = g_app.targetPort;
inline std::string& g_targetMode = g_app.targetMode;

inline std::atomic<int>& g_currentWidth = g_app.currentWidth;
inline std::atomic<int>& g_currentHeight = g_app.currentHeight;
inline std::atomic<int>& g_currentFps = g_app.currentFps;
inline std::atomic<bool>& g_fpsChanged = g_app.fpsChanged;
inline std::atomic<bool>& g_resolutionChanged = g_app.resolutionChanged;

inline std::atomic<bool>& g_mirrorEnabled = g_app.mirrorEnabled;
inline std::atomic<bool>& g_flip180 = g_app.flip180;
inline std::atomic<bool>& g_blurEnabled = g_app.blurEnabled;
inline std::atomic<bool>& g_isFrontCamera = g_app.isFrontCamera;
inline std::atomic<bool>& g_isLandscapeMode = g_app.isLandscapeMode;
inline std::atomic<int>&  g_aspectRatioMode = g_app.aspectRatioMode;

inline std::atomic<float>& g_audioVolume = g_app.audioVolume;
inline std::atomic<bool>& g_audioMuted = g_app.audioMuted;
inline std::atomic<int>& g_audioDelayMs = g_app.audioDelayMs;
inline std::atomic<bool>& g_noiseGateEnabled = g_app.noiseGateEnabled;
inline std::atomic<float>& g_noiseGateThreshold = g_app.noiseGateThreshold;
inline std::atomic<bool>& g_aiNoiseEnabled = g_app.aiNoiseEnabled;
inline std::atomic<bool>& g_agcEnabled = g_app.agcEnabled;
inline std::atomic<bool>& g_declickerEnabled = g_app.declickerEnabled;
inline std::atomic<float>& g_eqLowDb = g_app.eqLowDb;
inline std::atomic<float>& g_eqMidDb = g_app.eqMidDb;
inline std::atomic<float>& g_eqHighDb = g_app.eqHighDb;

inline std::atomic<int>& g_bgEffectMode = g_app.bgEffectMode;
inline std::atomic<float>& g_bgBlurRadius = g_app.bgBlurRadius;
inline std::atomic<float>& g_bgEdgeSoftness = g_app.bgEdgeSoftness;
inline std::atomic<float>& g_bgThreshold = g_app.bgThreshold;
inline std::atomic<bool>& g_privacyShield = g_app.privacyShield;

inline std::atomic<bool>& g_trollFpsLimit = g_app.trollFpsLimit;
inline std::atomic<int>& g_trollPixelate = g_app.trollPixelate;
inline std::atomic<bool>& g_trollGlitch = g_app.trollGlitch;
inline std::atomic<bool>& g_trollBitcrush = g_app.trollBitcrush;
inline std::atomic<bool>& g_trollOverexposure = g_app.trollOverexposure;
inline std::atomic<bool>& g_trollGsmVoice = g_app.trollGsmVoice;
inline std::atomic<bool>& g_trollGsmBurstTrigger = g_app.trollGsmBurstTrigger;
inline std::atomic<bool>& g_trollWalkieTalkie = g_app.trollWalkieTalkie;
inline std::atomic<bool>& g_trollRobotVoice = g_app.trollRobotVoice;
inline std::atomic<bool>& g_trollCctv = g_app.trollCctv;
inline std::atomic<bool>& g_trollFakeLag = g_app.trollFakeLag;

inline std::string& g_audioInputDeviceId = g_app.audioInputDeviceId;
inline std::atomic<bool>& g_audioInputDeviceChanged = g_app.audioInputDeviceChanged;

inline std::atomic<bool>& g_isTallyActive = g_app.isTallyActive;
inline std::atomic<bool>& g_isFrozen = g_app.isFrozen;

