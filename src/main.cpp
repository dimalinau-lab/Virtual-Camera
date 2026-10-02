#pragma comment(linker, "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup")
#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <thread>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <mmsystem.h>
#include <immintrin.h>
#include <omp.h>

extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

#include "httplib.h"
#include "gui_window.hpp"
#include "http_server.hpp"
#include "tcp_receiver.hpp"
#include "nvdec_decoder.hpp"
#include "mf_vcam_writer.hpp"
#include "audio_receiver.hpp"
#include "udp_discovery.hpp"
#include "studio_optics.hpp"
#include "d3d11_optics_pipeline.hpp"
#include "config_manager.hpp"
#include "update_manager.hpp"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ole32.lib")

std::atomic<float> g_audioVolume{ 1.0f };
std::atomic<bool>  g_audioMuted{ false };
std::atomic<int>   g_audioDelayMs{ 0 };          // Lip-Sync задержка (0 - 500 мс)
std::atomic<bool>  g_noiseGateEnabled{ true };   // Студийный Noise Gate
std::atomic<float> g_noiseGateThreshold{ 0.015f }; // Порог срабатывания гейта
std::atomic<bool>  g_aiNoiseEnabled{ true };      // AI Шумоподавление клавиатуры
std::atomic<bool>  g_agcEnabled{ true };          // Auto-Gain Control
std::atomic<bool>  g_declickerEnabled{ true };    // Подавление механических щелчков
std::atomic<float> g_eqLowDb{ 0.0f };             // Low Shelf (120 Hz)
std::atomic<float> g_eqMidDb{ 0.0f };             // Mid Peak (2200 Hz)
std::atomic<float> g_eqHighDb{ 0.0f };            // High Shelf (7500 Hz)

// AI Neural Background
std::atomic<int>   g_bgEffectMode{ 0 };           // 0: Off, 1: Bokeh Blur, 2: Green Screen, 3: Dark Studio
std::atomic<float> g_bgBlurRadius{ 8.0f };        // 1.0 .. 20.0 px
std::atomic<float> g_bgEdgeSoftness{ 0.15f };
std::atomic<float> g_bgThreshold{ 0.50f };
std::atomic<bool>  g_privacyShield{ false };      // Blackout / Privacy Shield режим шторки

std::atomic<bool> g_isAppRunning{ true };
std::atomic<bool> g_isStreamActive{ false };
std::atomic<bool> g_closeToTray{ true };
std::atomic<bool> g_showConsole{ false };

void setConsoleVisible(bool visible) {
    g_showConsole.store(visible);
    HWND hConsole = GetConsoleWindow();
    if (hConsole) {
        ShowWindow(hConsole, visible ? SW_SHOW : SW_HIDE);
        if (visible) {
            SetForegroundWindow(hConsole);
        }
    }
}

bool isConsoleVisible() {
    return g_showConsole.load();
}

void setCloseToTray(bool closeToTray) {
    g_closeToTray.store(closeToTray);
}

bool isCloseToTray() {
    return g_closeToTray.load();
}
std::atomic<bool> g_connectRequested{ false };
std::string g_targetIp = "127.0.0.1";
int g_targetPort = 8554;
std::string g_targetMode = "usb";

std::atomic<int> g_currentWidth{ 1280 };
std::atomic<int> g_currentHeight{ 720 };
std::atomic<int> g_currentFps{ 60 };
std::atomic<bool> g_fpsChanged{ false };
std::atomic<bool> g_resolutionChanged{ false };

std::atomic<bool> g_mirrorEnabled{ false };
std::atomic<bool> g_flip180{ false };
std::atomic<bool> g_blurEnabled{ false };
std::atomic<bool> g_isFrontCamera{ false };
std::atomic<bool> g_isLandscapeMode{ false };

// --- TROLL FX ФЛАГИ ---
std::atomic<bool> g_trollFpsLimit{ false };     // 5 FPS режим
std::atomic<int>  g_trollPixelate{ 1 };          // Размер пиксельного блока (1 = выкл, 8, 16, 32)
std::atomic<bool> g_trollGlitch{ false };        // Эффект сбитых строк
std::atomic<bool> g_trollBitcrush{ false };      // Эффект 4-битного цвета
std::atomic<bool> g_trollOverexposure{ false };  // Ядерный пересвет

static HttpServer g_httpServer;

bool isRunAsAdmin() {
    BOOL isAdmin = FALSE;
    PSID adminGroup = nullptr;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;

    if (AllocateAndInitializeSid(&ntAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID,
        DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(nullptr, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin == TRUE;
}

bool relaunchAsAdmin(const std::string& extraArg = "--register") {
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);

    SHELLEXECUTEINFOA sei = { sizeof(sei) };
    sei.lpVerb = "runas";
    sei.lpFile = exePath;
    sei.lpParameters = extraArg.c_str();
    sei.nShow = SW_NORMAL;

    return ShellExecuteExA(&sei) == TRUE;
}

bool isCameraRegistered() {
    HKEY hKey = nullptr;
    const char* subkey = "CLSID\\{860BB310-5D01-11D0-BD3B-00A0C911CE86}\\Instance\\{E1D3B890-5F16-47D8-9C9D-9F0A3E8B81B1}";
    LSTATUS status = RegOpenKeyExA(HKEY_CLASSES_ROOT, subkey, 0, KEY_READ, &hKey);
    if (status == ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return true;
    }
    return false;
}

bool isMicRegistered() {
    HKEY hKey = nullptr;
    const char* subkey = "CLSID\\{33D9A762-90C8-11d0-BD43-00A0C911CE86}\\Instance\\{A1B2C3D4-E5F6-7890-ABCD-EF0123456789}";
    LSTATUS status = RegOpenKeyExA(HKEY_CLASSES_ROOT, subkey, 0, KEY_READ, &hKey);
    if (status == ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return true;
    }
    return false;
}

bool isVBCableInstalled() {
    UINT numDevs = waveOutGetNumDevs();
    for (UINT i = 0; i < numDevs; ++i) {
        WAVEOUTCAPSW caps{};
        if (waveOutGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
            if (wcsstr(caps.szPname, L"CABLE Input") != nullptr) {
                return true;
            }
        }
    }

    HKEY hKey = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Services\\VBCABLE", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return true;
    }
    return false;
}

bool installVBCableSilently() {
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::filesystem::path dir = std::filesystem::path(exePath).parent_path();

    std::filesystem::path setupPath = dir / "driver" / "VBCABLE_Setup_x64.exe";
    if (!std::filesystem::exists(setupPath)) {
        setupPath = dir / "VBCABLE_Setup_x64.exe";
    }

    if (!std::filesystem::exists(setupPath)) {
        OutputDebugStringA("[AUDIO-WARN] VBCABLE_Setup_x64.exe не найден!\n");
        return false;
    }

    SHELLEXECUTEINFOA sei = { sizeof(sei) };
    sei.lpVerb = "runas";
    sei.lpFile = setupPath.string().c_str();
    sei.lpParameters = "-i -h";
    sei.nShow = SW_HIDE;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;

    if (ShellExecuteExA(&sei) && sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 30000);
        DWORD exitCode = 0;
        GetExitCodeProcess(sei.hProcess, &exitCode);
        CloseHandle(sei.hProcess);
        Sleep(1500);
        return true;
    }
    return false;
}

bool registerVirtualCamDll() {
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::filesystem::path dir = std::filesystem::path(exePath).parent_path();
    std::filesystem::path dllPath = dir / "NativeMFVirtualCam.dll";

    if (!std::filesystem::exists(dllPath)) return false;

    HMODULE hDll = LoadLibraryExW(dllPath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!hDll) return false;

    using DllRegFn = HRESULT(STDAPICALLTYPE*)();
    auto pfnRegister = reinterpret_cast<DllRegFn>(GetProcAddress(hDll, "DllRegisterServer"));
    if (!pfnRegister) {
        FreeLibrary(hDll);
        return false;
    }

    HRESULT hr = pfnRegister();
    FreeLibrary(hDll);
    return SUCCEEDED(hr);
}

bool registerVirtualMicDevice(const std::wstring& dllPath) {
    HKEY hKey = nullptr;
    std::wstring clsidStr = L"{A1B2C3D4-E5F6-7890-ABCD-EF0123456789}";
    std::wstring keyPath = L"CLSID\\" + clsidStr;

    if (RegCreateKeyExW(HKEY_CLASSES_ROOT, keyPath.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr) != ERROR_SUCCESS) {
        return false;
    }

    const wchar_t* friendlyName = L"VirtualCam Native Microphone";
    RegSetValueExW(hKey, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(friendlyName), static_cast<DWORD>((wcslen(friendlyName) + 1) * sizeof(wchar_t)));

    HKEY hInproc = nullptr;
    if (RegCreateKeyExW(hKey, L"InprocServer32", 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hInproc, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(hInproc, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(dllPath.c_str()), static_cast<DWORD>((dllPath.length() + 1) * sizeof(wchar_t)));
        const wchar_t* threading = L"Both";
        RegSetValueExW(hInproc, L"ThreadingModel", 0, REG_SZ, reinterpret_cast<const BYTE*>(threading), static_cast<DWORD>((wcslen(threading) + 1) * sizeof(wchar_t)));
        RegCloseKey(hInproc);
    }
    RegCloseKey(hKey);

    std::wstring catPath = L"CLSID\\{33D9A762-90C8-11d0-BD43-00A0C911CE86}\\Instance\\" + clsidStr;
    HKEY hCat = nullptr;
    if (RegCreateKeyExW(HKEY_CLASSES_ROOT, catPath.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hCat, nullptr) == ERROR_SUCCESS) {
        RegSetValueExW(hCat, L"FriendlyName", 0, REG_SZ, reinterpret_cast<const BYTE*>(friendlyName), static_cast<DWORD>((wcslen(friendlyName) + 1) * sizeof(wchar_t)));
        RegSetValueExW(hCat, L"CLSID", 0, REG_SZ, reinterpret_cast<const BYTE*>(clsidStr.c_str()), static_cast<DWORD>((clsidStr.length() + 1) * sizeof(wchar_t)));
        RegCloseKey(hCat);
    }

    return true;
}

bool registerAllDrivers() {
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::filesystem::path dir = std::filesystem::path(exePath).parent_path();
    std::filesystem::path dllPath = dir / "NativeMFVirtualCam.dll";

    bool okCam = registerVirtualCamDll();
    bool okMic = registerVirtualMicDevice(dllPath.wstring());

    if (!isVBCableInstalled()) {
        installVBCableSilently();
    }

    return okCam && okMic;
}

std::string findAdbExecutable() {
    char localAppData[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localAppData))) {
        std::string sdkPath = std::string(localAppData) + "\\Android\\Sdk\\platform-tools\\adb.exe";
        if (std::filesystem::exists(sdkPath)) return "\"" + sdkPath + "\"";
    }

    char profile[MAX_PATH];
    if (GetEnvironmentVariableA("USERPROFILE", profile, MAX_PATH) > 0) {
        std::string sdkPath = std::string(profile) + "\\AppData\\Local\\Android\\Sdk\\platform-tools\\adb.exe";
        if (std::filesystem::exists(sdkPath)) return "\"" + sdkPath + "\"";
    }

    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string localAdb = (std::filesystem::path(exePath).parent_path() / "adb.exe").string();
    if (std::filesystem::exists(localAdb)) return "\"" + localAdb + "\"";

    return "adb";
}

bool setupAdbForwards() {
    std::string adb = findAdbExecutable();
    std::string cmdVideo = adb + " forward tcp:8554 tcp:8554";
    std::string cmdControl = adb + " forward tcp:8080 tcp:8080";
    std::string cmdAudio = adb + " forward tcp:8555 tcp:8555";

    auto runHiddenCmd = [](const std::string& cmdLine) {
        STARTUPINFOA si{};
        PROCESS_INFORMATION pi{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;

        std::vector<char> cmdVec(cmdLine.begin(), cmdLine.end());
        cmdVec.push_back('\0');

        if (CreateProcessA(nullptr, cmdVec.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 3000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
        };

    runHiddenCmd(cmdVideo);
    runHiddenCmd(cmdControl);
    runHiddenCmd(cmdAudio);
    return true;
}

inline void mirrorNv12(uint8_t* nv12, int w, int h) {
    uint8_t* yPlane = nv12;
    uint8_t* uvPlane = nv12 + ((size_t)w * h);

    #pragma omp parallel for schedule(static)
    for (int y = 0; y < h; ++y) {
        uint8_t* rowY = yPlane + y * w;
        for (int x = 0; x < w / 2; ++x) {
            std::swap(rowY[x], rowY[w - 1 - x]);
        }
    }

    int halfH = h / 2;
    int halfW = w / 2;
    #pragma omp parallel for schedule(static)
    for (int y = 0; y < halfH; ++y) {
        uint16_t* rowUV = reinterpret_cast<uint16_t*>(uvPlane + y * w);
        for (int x = 0; x < halfW / 2; ++x) {
            std::swap(rowUV[x], rowUV[halfW - 1 - x]);
        }
    }
}

inline void flip180Nv12(uint8_t* nv12, int w, int h) {
    uint8_t* yPlane = nv12;
    uint8_t* uvPlane = nv12 + ((size_t)w * h);

    #pragma omp parallel for schedule(static)
    for (int y = 0; y < h / 2; ++y) {
        uint8_t* rowTop = yPlane + y * w;
        uint8_t* rowBottom = yPlane + (h - 1 - y) * w;
        for (int x = 0; x < w; ++x) {
            std::swap(rowTop[x], rowBottom[w - 1 - x]);
        }
    }

    int halfH = h / 2;
    int halfW = w / 2;
    #pragma omp parallel for schedule(static)
    for (int y = 0; y < halfH / 2; ++y) {
        uint16_t* rowTop = reinterpret_cast<uint16_t*>(uvPlane + y * w);
        uint16_t* rowBottom = reinterpret_cast<uint16_t*>(uvPlane + (halfH - 1 - y) * w);
        for (int x = 0; x < halfW; ++x) {
            std::swap(rowTop[x], rowBottom[halfW - 1 - x]);
        }
    }
}

inline void transformPortraitDirect(
    const uint32_t* __restrict src, int inW, int inH,
    uint32_t* __restrict canvas, int canvasW, int canvasH,
    bool mirror, bool isFront, bool flip180 = false)
{
    const int targetH = canvasH;
    int targetW = (inH * canvasH) / inW;
    if (targetW % 2 != 0) targetW--;
    if (targetW > canvasW) targetW = canvasW;

    const int offsetX = (canvasW - targetW) / 2;
    const int rightX = offsetX + targetW;

    const int64_t stepX_fp = ((int64_t)inW << 16) / targetH;
    const int64_t stepY_fp = ((int64_t)inH << 16) / targetW;

    bool invertY = !isFront;
    if (flip180) invertY = !invertY;

    bool invertX = isFront ? !mirror : mirror;
    if (flip180) invertX = !invertX;

    #pragma omp parallel for schedule(static)
    for (int y = 0; y < canvasH; ++y) {
        uint32_t* row = canvas + y * canvasW;
        if (offsetX > 0) {
            memset(row, 0, offsetX * sizeof(uint32_t));
        }
        if (canvasW > rightX) {
            memset(row + rightX, 0, (canvasW - rightX) * sizeof(uint32_t));
        }

        int srcX = invertY
            ? (int)(((targetH - 1 - y) * stepX_fp) >> 16)
            : (int)((y * stepX_fp) >> 16);

        if (srcX >= inW) srcX = inW - 1;
        if (srcX < 0) srcX = 0;

        for (int dx = 0; dx < targetW; ++dx) {
            int srcY = invertX
                ? (int)(((targetW - 1 - dx) * stepY_fp) >> 16)
                : (int)((dx * stepY_fp) >> 16);

            if (srcY >= inH) srcY = inH - 1;
            if (srcY < 0) srcY = 0;

            row[offsetX + dx] = src[srcY * inW + srcX];
        }
    }
}

// Применение эффектов приколов (144p, глитчи, биткраш, пересвет)
inline void applyTrollEffects(uint32_t* canvas, int w, int h) {
    // 1. Пикселизация
    int blockSize = g_trollPixelate.load();
    if (blockSize > 1) {
        for (int y = 0; y < h; y += blockSize) {
            for (int x = 0; x < w; x += blockSize) {
                uint32_t color = canvas[y * w + x];
                int yEnd = (std::min)(y + blockSize, h);
                int xEnd = (std::min)(x + blockSize, w);
                for (int by = y; by < yEnd; ++by) {
                    for (int bx = x; bx < xEnd; ++bx) {
                        canvas[by * w + bx] = color;
                    }
                }
            }
        }
    }

    // 2. Ядерный пересвет (Nuclear Overexposure / Flashbang)
    if (g_trollOverexposure.load()) {
        const size_t total = (size_t)w * h;
        for (size_t i = 0; i < total; ++i) {
            uint32_t pixel = canvas[i];
            uint32_t b = (pixel & 0xFF);
            uint32_t g = ((pixel >> 8) & 0xFF);
            uint32_t r = ((pixel >> 16) & 0xFF);

            uint32_t luma = (r * 77 + g * 151 + b * 28) >> 8;

            if (luma > 115) {
                r = (std::min)(255u, r + (r * 2));
                g = (std::min)(255u, g + (g * 2));
                b = (std::min)(255u, b + (b * 2));
            }
            else {
                r = (r * r) / 255;
                g = (g * g) / 255;
                b = (b * b) / 255;
            }

            canvas[i] = 0xFF000000 | (r << 16) | (g << 8) | b;
        }
    }

    // 3. Шакализация цвета (Bitcrush)
    if (g_trollBitcrush.load()) {
        const size_t total = (size_t)w * h;
        for (size_t i = 0; i < total; ++i) {
            canvas[i] &= 0xFFE0E0E0;
        }
    }

    // 4. Помехи / VHS Глитчи
    if (g_trollGlitch.load()) {
        static int glitchCounter = 0;
        if (++glitchCounter % 3 == 0) {
            for (int i = 0; i < 6; ++i) {
                int startY = rand() % (h - 20);
                int height = 2 + (rand() % 12);
                int shift = (rand() % 40) - 20;

                for (int y = startY; y < startY + height && y < h; ++y) {
                    uint32_t* row = canvas + y * w;
                    if (shift > 0) {
                        for (int x = w - 1; x >= shift; --x) row[x] = row[x - shift];
                    }
                    else if (shift < 0) {
                        for (int x = 0; x < w + shift; ++x) row[x] = row[x - shift];
                    }
                }
            }
        }
    }
}

static inline uint8_t getH265NalType(const uint8_t* data, int size) {
    if (!data || size < 5) return 0;
    return (data[4] >> 1) & 0x3F;
}

inline void blendNv12Buffer(const uint8_t* a, const uint8_t* b, uint8_t* dst, size_t size) {
    size_t i = 0;
    for (; i + 32 <= size; i += 32) {
        __m256i va = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(a + i));
        __m256i vb = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(b + i));
        __m256i vavg = _mm256_avg_epu8(va, vb);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), vavg);
    }
    for (; i < size; ++i) {
        dst[i] = static_cast<uint8_t>((static_cast<uint16_t>(a[i]) + static_cast<uint16_t>(b[i])) >> 1);
    }
}

void videoStreamWorker() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    timeBeginPeriod(1);

    const int canvasW = 1280;
    const int canvasH = 720;
    const size_t nv12Size = (size_t)canvasW * canvasH * 3 / 2;

    MfVirtualCamWriter vcamWriter;
    vcamWriter.start(canvasW, canvasH, g_currentFps.load());

    NvdecDecoder decoder;
    decoder.init(canvasW, canvasH);

    D3D11OpticsPipeline::instance().init(canvasW, canvasH);

    TcpReceiver receiver;
    AudioReceiver audioReceiver;

    std::vector<uint8_t> naluBuffer;
    naluBuffer.reserve(8 * 1024 * 1024);

    std::vector<uint32_t> canvasBgra((size_t)canvasW * canvasH, 0xFF000000);
    std::vector<uint8_t> nv12Buffer(nv12Size);
    std::vector<uint8_t> prevNv12Buffer(nv12Size);
    std::vector<uint8_t> interpNv12Buffer(nv12Size);
    bool hasPrevNv12 = false;

    SwsContext* swsDirectToNv12 = nullptr;
    SwsContext* swsLandscape = nullptr;
    SwsContext* swsCanvasToNv12 = nullptr;

    int fpsCounter = 0;
    auto lastFpsTime = std::chrono::steady_clock::now();
    auto lastTrollFrameTime = std::chrono::steady_clock::now();

    int prevInW = 0;
    int prevInH = 0;
    int previewSkipCounter = 0;
    bool lastLandscapeMode = g_isLandscapeMode.load();

    memset(nv12Buffer.data(), 16, canvasW * canvasH);
    memset(nv12Buffer.data() + canvasW * canvasH, 128, canvasW * canvasH / 2);

    while (g_isAppRunning) {
        if (!g_connectRequested && !g_isStreamActive) {
            Sleep(20);
            continue;
        }

        std::string ip = g_targetIp;
        int port = g_targetPort;

        if (g_targetMode == "usb") {
            setupAdbForwards();
        }

        if (!receiver.connectToPhone(ip, port)) {
            Sleep(250);
            continue;
        }

        int nodelay = 1;
        setsockopt(receiver.getSocket(), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

        int currentRcvBuf = 1024 * 1024;
        setsockopt(receiver.getSocket(), SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&currentRcvBuf), sizeof(currentRcvBuf));

        g_isStreamActive = true;
        g_connectRequested = false;
        decoder.reinit();
        hasPrevNv12 = false;

        audioReceiver.start(ip, 8555);
        audioReceiver.setVolume(g_audioVolume.load());
        audioReceiver.setMute(g_audioMuted.load());
        audioReceiver.setDelayMs(g_audioDelayMs.load());
        audioReceiver.setNoiseGate(g_noiseGateEnabled.load(), g_noiseGateThreshold.load());
        audioReceiver.setAiNoise(g_aiNoiseEnabled.load());
        audioReceiver.setAgc(g_agcEnabled.load());
        audioReceiver.setDeclicker(g_declickerEnabled.load());
        audioReceiver.setEq(g_eqLowDb.load(), g_eqMidDb.load(), g_eqHighDb.load());

        std::fill(canvasBgra.begin(), canvasBgra.end(), 0xFF000000);

        bool isCurrently4K = false;

        printf("[STREAM] Соединение установлено: %s:%d (режим: %s)\n", ip.c_str(), port, g_targetMode.c_str());
        printf("[STREAM] Запуск мониторинга задержек кадров (Межкадровый Δt + Пайплайн)...\n\n");

        auto prevNaluTime = std::chrono::high_resolution_clock::now();
        bool hasPrevNaluTime = false;
        uint64_t totalFrameCount = 0;
        float sumIntervalMs = 0.0f;
        float minIntervalMs = 9999.0f;
        float maxIntervalMs = 0.0f;
        int intervalSamples = 0;

        while (g_isAppRunning && g_isStreamActive) {
            if (g_connectRequested.load()) {
                break;
            }
            if (g_fpsChanged.exchange(false)) {
                int targetFps = g_currentFps.load();
                vcamWriter.setFps(targetFps);
                decoder.flush();
                hasPrevNv12 = false;
            }

            audioReceiver.setVolume(g_audioVolume.load());
            audioReceiver.setMute(g_audioMuted.load());
            audioReceiver.setDelayMs(g_audioDelayMs.load());
            audioReceiver.setNoiseGate(g_noiseGateEnabled.load(), g_noiseGateThreshold.load());

            int naluSize = receiver.receiveNalu(naluBuffer);
            if (naluSize <= 0) {
                break;
            }

            auto tNaluArrival = std::chrono::high_resolution_clock::now();
            float frameIntervalMs = 0.0f;
            if (hasPrevNaluTime) {
                frameIntervalMs = std::chrono::duration<float, std::milli>(tNaluArrival - prevNaluTime).count();
                sumIntervalMs += frameIntervalMs;
                if (frameIntervalMs < minIntervalMs) minIntervalMs = frameIntervalMs;
                if (frameIntervalMs > maxIntervalMs) maxIntervalMs = frameIntervalMs;
                intervalSamples++;
            }
            prevNaluTime = tNaluArrival;
            hasPrevNaluTime = true;

            int pending = receiver.getPendingBytes();
            int bloatThreshold = isCurrently4K ? (500 * 1024) : (64 * 1024);

            if (pending > bloatThreshold) {
                while (pending > (isCurrently4K ? (150 * 1024) : (32 * 1024)) && g_isStreamActive) {
                    int skipped = receiver.receiveNalu(naluBuffer);
                    if (skipped <= 0) break;
                    uint8_t nType = getH265NalType(naluBuffer.data(), skipped);
                    if (nType == 19 || nType == 20 || nType == 32 || nType == 33 || nType == 34) {
                        naluSize = skipped;
                        decoder.flush();
                        break;
                    }
                    pending = receiver.getPendingBytes();
                }
            }

            auto tStartPipeline = std::chrono::high_resolution_clock::now();

            if (g_privacyShield.load()) {
                // Privacy Shield: быстрый матовый темный фон (Y=16, UV=128) без декодирования
                memset(nv12Buffer.data(), 16, canvasW * canvasH);
                memset(nv12Buffer.data() + canvasW * canvasH, 128, canvasW * canvasH / 2);
                vcamWriter.writeFrameNV12(nv12Buffer.data());
                fpsCounter++;
                Sleep(16);
                continue;
            }

            bool hasActiveTrollFx = g_trollFpsLimit.load() || (g_trollPixelate.load() > 1) ||
                                    g_trollGlitch.load() || g_trollBitcrush.load() || g_trollOverexposure.load();
            bool hasActiveOptics = StudioOptics::instance().hasActiveOptics() || (g_bgEffectMode.load() > 0);
            bool isLandscape = g_isLandscapeMode.load();

            if (isLandscape && !hasActiveTrollFx && !hasActiveOptics) {
                // --- FAST-PATH ZERO-COPY: Прямая конвертация в NV12 без промежуточного BGRA (Альбомный режим) ---
                decoder.decodeNaluDirect(naluBuffer.data(), naluSize, [&](const AVFrame* frame, int inW, int inH) {
                    if (!frame || inW <= 0 || inH <= 0) return;

                    isCurrently4K = (inW >= 3840 || inH >= 3840);

                    if (inW != prevInW || inH != prevInH) {
                        prevInW = inW;
                        prevInH = inH;
                        if (swsDirectToNv12) { sws_freeContext(swsDirectToNv12); swsDirectToNv12 = nullptr; }
                        hasPrevNv12 = false;
                    }

                    // Если формат уже NV12 и размер точно равен 1280x720 - прямой построчный memcpy (~0.08 ms!)
                    if ((AVPixelFormat)frame->format == AV_PIX_FMT_NV12 && inW == canvasW && inH == canvasH) {
                        uint8_t* dstY = nv12Buffer.data();
                        uint8_t* dstUV = dstY + ((size_t)canvasW * canvasH);
                        for (int y = 0; y < canvasH; ++y) {
                            memcpy(dstY + y * canvasW, frame->data[0] + y * frame->linesize[0], canvasW);
                        }
                        for (int y = 0; y < canvasH / 2; ++y) {
                            memcpy(dstUV + y * canvasW, frame->data[1] + y * frame->linesize[1], canvasW);
                        }
                    }
                    else {
                        swsDirectToNv12 = sws_getCachedContext(
                            swsDirectToNv12,
                            inW, inH, (AVPixelFormat)frame->format,
                            canvasW, canvasH, AV_PIX_FMT_NV12,
                            SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
                        );

                        if (swsDirectToNv12) {
                            uint8_t* dstY = nv12Buffer.data();
                            uint8_t* dstUV = dstY + ((size_t)canvasW * canvasH);
                            uint8_t* dstSlice[4] = { dstY, dstUV, nullptr, nullptr };
                            int dstStride[4] = { canvasW, canvasW, 0, 0 };

                            sws_scale(swsDirectToNv12, frame->data, frame->linesize, 0, inH, dstSlice, dstStride);
                        }
                    }

                    if (g_flip180.load()) {
                        flip180Nv12(nv12Buffer.data(), canvasW, canvasH);
                    }

                    if (g_mirrorEnabled.load()) {
                        mirrorNv12(nv12Buffer.data(), canvasW, canvasH);
                    }

                        bool targetIs60 = (g_currentFps.load() >= 60);

                        if (targetIs60 && hasPrevNv12) {
                            blendNv12Buffer(prevNv12Buffer.data(), nv12Buffer.data(), interpNv12Buffer.data(), nv12Size);
                            vcamWriter.writeFrameNV12(interpNv12Buffer.data());
                            fpsCounter++;
                        }

                        vcamWriter.writeFrameNV12(nv12Buffer.data());
                        fpsCounter++;

                        memcpy(prevNv12Buffer.data(), nv12Buffer.data(), nv12Size);
                        hasPrevNv12 = true;

                    if (++previewSkipCounter % 20 == 0) {
                        g_httpServer.updatePreviewFrameNv12(nv12Buffer.data(), canvasW, canvasH);
                    }

                    auto tEndPipeline = std::chrono::high_resolution_clock::now();
                    float pipelineMs = std::chrono::duration<float, std::milli>(tEndPipeline - tStartPipeline).count();

                    uint64_t seq = ++totalFrameCount;
                    if (seq % 5 == 0 || frameIntervalMs > 40.0f) {
                        const char* flag = (frameIntervalMs > 45.0f) ? " [WARN: ЗАДЕРЖКА!]" : "";
                        printf("[FAST-PATH] Кадр #%-6llu | Δt (между кадрами): %5.1f ms | Рендер: %4.2f ms | Буфер: %4d KB%s\n",
                            seq, frameIntervalMs, pipelineMs, pending / 1024, flag);
                    }

                    auto now = std::chrono::steady_clock::now();
                    std::chrono::duration<float> elapsed = now - lastFpsTime;
                    if (elapsed.count() >= 1.0f) {
                        float currentFps = fpsCounter / elapsed.count();
                        float avgInterval = intervalSamples > 0 ? (sumIntervalMs / intervalSamples) : 0.0f;

                        std::string resBadge = "H.265 Direct (" + std::to_string(prevInW) + "x" + std::to_string(prevInH) + ")";
                        g_httpServer.updateTelemetry(currentFps, "Active (Zero-Copy)", resBadge);

                        printf("------------------------------------------------------------------------------------\n");
                        printf(">>> [STATS 1s] FPS: %4.1f | Средний Δt: %5.2f ms (Мин: %5.2f, Макс: %5.2f) | Рендер: %4.2f ms\n",
                            currentFps, avgInterval, (minIntervalMs < 9000 ? minIntervalMs : 0.0f), maxIntervalMs, pipelineMs);
                        printf("------------------------------------------------------------------------------------\n");

                        fpsCounter = 0;
                        lastFpsTime = now;
                        sumIntervalMs = 0.0f;
                        minIntervalMs = 9999.0f;
                        maxIntervalMs = 0.0f;
                        intervalSamples = 0;
                    }
                });
            }
            else {
                // --- FX / PORTRAIT PATH: BGRA CONVERSION ---
                decoder.decodeNalu(naluBuffer.data(), naluSize, [&](const uint8_t* rawBgra, int inW, int inH) {
                    if (!rawBgra || inW <= 0 || inH <= 0) return;

                    isCurrently4K = (inW >= 3840 || inH >= 3840);
                    bool isLandscape = g_isLandscapeMode.load();

                    if (inW != prevInW || inH != prevInH) {
                        prevInW = inW;
                        prevInH = inH;
                        if (swsLandscape) { sws_freeContext(swsLandscape); swsLandscape = nullptr; }
                        if (swsCanvasToNv12) { sws_freeContext(swsCanvasToNv12); swsCanvasToNv12 = nullptr; }
                        std::fill(canvasBgra.begin(), canvasBgra.end(), 0xFF000000);
                        hasPrevNv12 = false;
                    }

                    if (lastLandscapeMode != isLandscape) {
                        std::fill(canvasBgra.begin(), canvasBgra.end(), 0xFF000000);
                        lastLandscapeMode = isLandscape;
                    }

                    if (isLandscape) {
                        swsLandscape = sws_getCachedContext(
                            swsLandscape,
                            inW, inH, AV_PIX_FMT_BGRA,
                            canvasW, canvasH, AV_PIX_FMT_BGRA,
                            SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
                        );

                        if (swsLandscape) {
                            const uint8_t* srcSlice[4] = { rawBgra, nullptr, nullptr, nullptr };
                            int srcStride[4] = { inW * 4, 0, 0, 0 };
                            uint8_t* dstSlice[4] = { reinterpret_cast<uint8_t*>(canvasBgra.data()), nullptr, nullptr, nullptr };
                            int dstStride[4] = { canvasW * 4, 0, 0, 0 };

                            sws_scale(swsLandscape, srcSlice, srcStride, 0, inH, dstSlice, dstStride);
                        }

                        if (g_flip180.load()) {
                            for (int y = 0; y < canvasH / 2; ++y) {
                                uint32_t* rowTop = canvasBgra.data() + y * canvasW;
                                uint32_t* rowBottom = canvasBgra.data() + (canvasH - 1 - y) * canvasW;
                                for (int x = 0; x < canvasW; ++x) {
                                    std::swap(rowTop[x], rowBottom[canvasW - 1 - x]);
                                }
                            }
                        }

                        if (g_mirrorEnabled.load()) {
                            for (int y = 0; y < canvasH; ++y) {
                                uint32_t* row = canvasBgra.data() + y * canvasW;
                                for (int x = 0; x < canvasW / 2; ++x) {
                                    std::swap(row[x], row[canvasW - 1 - x]);
                                }
                            }
                        }
                    }
                    else {
                        transformPortraitDirect(
                            reinterpret_cast<const uint32_t*>(rawBgra),
                            inW, inH,
                            canvasBgra.data(),
                            canvasW, canvasH,
                            g_mirrorEnabled.load(),
                            g_isFrontCamera.load(),
                            g_flip180.load()
                        );
                    }

                    // --- DIRECT3D 11 GPU PIPELINE: STUDIO OPTICS & TROLL FX ---
                    bool usedD3D11 = false;
                    if (D3D11OpticsPipeline::instance().isReady()) {
                        D3D11ShaderParams gpuParams{};
                        gpuParams.brightness = StudioOptics::instance().getBrightness() / 100.0f;
                        gpuParams.contrast = StudioOptics::instance().getContrast() / 100.0f;
                        gpuParams.saturation = StudioOptics::instance().getSaturation() / 100.0f;
                        gpuParams.colorTemp = StudioOptics::instance().getColorTemp() / 100.0f;
                        gpuParams.lutPreset = StudioOptics::instance().getLutPreset();
                        gpuParams.zoom = StudioOptics::instance().getZoom();
                        gpuParams.panX = StudioOptics::instance().getPanX();
                        gpuParams.panY = StudioOptics::instance().getPanY();
                        gpuParams.trollPixelate = g_trollPixelate.load();
                        gpuParams.trollOverexposure = g_trollOverexposure.load() ? 1 : 0;
                        gpuParams.trollBitcrush = g_trollBitcrush.load() ? 1 : 0;
                        gpuParams.trollGlitch = g_trollGlitch.load() ? 1 : 0;
                        gpuParams.timeSeconds = static_cast<float>(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
                        gpuParams.canvasWidth = static_cast<float>(canvasW);
                        gpuParams.canvasHeight = static_cast<float>(canvasH);
                        gpuParams.isPortrait = isLandscape ? 0.0f : 1.0f;
                        gpuParams.bgEffectMode = g_bgEffectMode.load();
                        gpuParams.bgBlurRadius = g_bgBlurRadius.load();
                        gpuParams.bgEdgeSoftness = g_bgEdgeSoftness.load();
                        gpuParams.bgThreshold = g_bgThreshold.load();

                        if (g_trollFpsLimit.load()) {
                            auto nowTroll = std::chrono::steady_clock::now();
                            if (std::chrono::duration_cast<std::chrono::milliseconds>(nowTroll - lastTrollFrameTime).count() < 200) {
                                return;
                            }
                            lastTrollFrameTime = nowTroll;
                        }

                        usedD3D11 = D3D11OpticsPipeline::instance().processToNv12(canvasBgra.data(), nv12Buffer.data(), gpuParams);

                        if (usedD3D11) {
                            bool targetIs60 = (g_currentFps.load() >= 60);
                            if (targetIs60 && hasPrevNv12 && !g_trollFpsLimit.load()) {
                                blendNv12Buffer(prevNv12Buffer.data(), nv12Buffer.data(), interpNv12Buffer.data(), nv12Size);
                                vcamWriter.writeFrameNV12(interpNv12Buffer.data());
                                fpsCounter++;
                            }

                            vcamWriter.writeFrameNV12(nv12Buffer.data());
                            fpsCounter++;

                            memcpy(prevNv12Buffer.data(), nv12Buffer.data(), nv12Size);
                            hasPrevNv12 = true;

                            if (++previewSkipCounter % 20 == 0) {
                                g_httpServer.updatePreviewFrameNv12(nv12Buffer.data(), canvasW, canvasH);
                            }
                        }
                    }

                    if (!usedD3D11) {
                        // --- CPU FALLBACK PATH ---
                        if (StudioOptics::instance().getZoom() > 1.02f) {
                            static std::vector<uint32_t> s_zoomBuffer;
                            if (s_zoomBuffer.size() != (size_t)canvasW * canvasH) {
                                s_zoomBuffer.resize(canvasW * canvasH);
                            }
                            StudioOptics::instance().applyZoom(canvasBgra.data(), s_zoomBuffer.data(), canvasW, canvasH);
                            memcpy(canvasBgra.data(), s_zoomBuffer.data(), canvasW * canvasH * sizeof(uint32_t));
                        }

                        if (StudioOptics::instance().hasActiveOptics()) {
                            StudioOptics::instance().processFrame(canvasBgra.data(), canvasW, canvasH);
                        }

                        if (g_trollFpsLimit.load()) {
                            auto nowTroll = std::chrono::steady_clock::now();
                            if (std::chrono::duration_cast<std::chrono::milliseconds>(nowTroll - lastTrollFrameTime).count() < 200) {
                                return;
                            }
                            lastTrollFrameTime = nowTroll;
                        }

                        applyTrollEffects(canvasBgra.data(), canvasW, canvasH);

                        swsCanvasToNv12 = sws_getCachedContext(
                            swsCanvasToNv12,
                            canvasW, canvasH, AV_PIX_FMT_BGRA,
                            canvasW, canvasH, AV_PIX_FMT_NV12,
                            SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
                        );

                        if (swsCanvasToNv12) {
                            const uint8_t* srcSlice[4] = { reinterpret_cast<const uint8_t*>(canvasBgra.data()), nullptr, nullptr, nullptr };
                            int srcStride[4] = { canvasW * 4, 0, 0, 0 };
                            uint8_t* dstY = nv12Buffer.data();
                            uint8_t* dstUV = dstY + ((size_t)canvasW * canvasH);
                            uint8_t* dstSlice[4] = { dstY, dstUV, nullptr, nullptr };
                            int dstStride[4] = { canvasW, canvasW, 0, 0 };

                            sws_scale(swsCanvasToNv12, srcSlice, srcStride, 0, canvasH, dstSlice, dstStride);

                            bool targetIs60 = (g_currentFps.load() >= 60);

                            if (targetIs60 && hasPrevNv12 && !g_trollFpsLimit.load()) {
                                blendNv12Buffer(prevNv12Buffer.data(), nv12Buffer.data(), interpNv12Buffer.data(), nv12Size);
                                vcamWriter.writeFrameNV12(interpNv12Buffer.data());
                                fpsCounter++;
                            }

                            vcamWriter.writeFrameNV12(nv12Buffer.data());
                            fpsCounter++;

                            memcpy(prevNv12Buffer.data(), nv12Buffer.data(), nv12Size);
                            hasPrevNv12 = true;
                        }

                        if (++previewSkipCounter % 20 == 0) {
                            g_httpServer.updatePreviewFrame(reinterpret_cast<const uint8_t*>(canvasBgra.data()), canvasW, canvasH);
                        }
                    }

                    auto tEndPipeline = std::chrono::high_resolution_clock::now();
                    float pipelineMs = std::chrono::duration<float, std::milli>(tEndPipeline - tStartPipeline).count();

                    uint64_t seq = ++totalFrameCount;
                    if (seq % 5 == 0 || frameIntervalMs > 40.0f) {
                        const char* flag = (frameIntervalMs > 45.0f) ? " [WARN: ЗАДЕРЖКА!]" : "";
                        const char* tag = isLandscape ? (usedD3D11 ? "[D3D11-GPU]" : "[FX-CPU]")
                                                      : (usedD3D11 ? "[D3D11-PORTRAIT]" : "[PORTRAIT-CPU]");
                        printf("%s Кадр #%-6llu | Δt (между кадрами): %5.1f ms | Рендер: %4.2f ms | Буфер: %4d KB%s\n",
                            tag, seq, frameIntervalMs, pipelineMs, pending / 1024, flag);
                    }

                    auto now = std::chrono::steady_clock::now();
                    std::chrono::duration<float> elapsed = now - lastFpsTime;
                    if (elapsed.count() >= 1.0f) {
                        float currentFps = fpsCounter / elapsed.count();
                        float avgInterval = intervalSamples > 0 ? (sumIntervalMs / intervalSamples) : 0.0f;

                        std::string resBadge = isLandscape ? ("H.265 (" + std::to_string(prevInW) + "x" + std::to_string(prevInH) + ")")
                                                           : ("H.265 Portrait (" + std::to_string(prevInH) + "x" + std::to_string(prevInW) + ")");
                        std::string statusBadge;
                        if (usedD3D11) {
                            int bgMode = g_bgEffectMode.load();
                            if (bgMode == 1) statusBadge = "Active (AI Bokeh Blur)";
                            else if (bgMode == 2) statusBadge = "Active (Virtual Green Screen)";
                            else if (bgMode == 3) statusBadge = "Active (Dark Studio Backdrop)";
                            else statusBadge = isLandscape ? (hasActiveOptics ? "Active (D3D11 GPU Optics)" : "Active (D3D11 GPU FX)")
                                                           : (hasActiveOptics ? "Active (D3D11 GPU Portrait)" : "Active (D3D11 GPU 90°)");
                        } else {
                            statusBadge = isLandscape ? (hasActiveOptics ? "Active (Studio Optics)" : "Active (FX)")
                                                      : (hasActiveOptics ? "Active (Portrait + Optics)" : "Active (Portrait 90°)");
                        }
                        g_httpServer.updateTelemetry(currentFps, statusBadge, resBadge);

                        printf("------------------------------------------------------------------------------------\n");
                        printf(">>> [STATS 1s %s | D3D11: %s] FPS: %4.1f | Средний Δt: %5.2f ms (Мин: %5.2f, Макс: %5.2f) | Рендер: %4.2f ms\n",
                            isLandscape ? "FX" : "PORTRAIT", (usedD3D11 ? "ON" : "OFF"), currentFps, avgInterval, (minIntervalMs < 9000 ? minIntervalMs : 0.0f), maxIntervalMs, pipelineMs);
                        printf("------------------------------------------------------------------------------------\n");

                        fpsCounter = 0;
                        lastFpsTime = now;
                        sumIntervalMs = 0.0f;
                        minIntervalMs = 9999.0f;
                        maxIntervalMs = 0.0f;
                        intervalSamples = 0;
                    }
                });
            }
        }

        audioReceiver.stop();
        receiver.stop();

        if (g_isStreamActive.load()) {
            Sleep(250);
            g_connectRequested = true;
        }
    }

    timeEndPeriod(1);
    audioReceiver.stop();
    if (swsDirectToNv12) sws_freeContext(swsDirectToNv12);
    if (swsLandscape) sws_freeContext(swsLandscape);
    if (swsCanvasToNv12) sws_freeContext(swsCanvasToNv12);
    vcamWriter.stop();

    CoUninitialize();
}

int main(int argc, char* argv[]) {
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    if (argc > 1 && std::string(argv[1]) == "--register") {
        registerAllDrivers();
        if (SUCCEEDED(hrCom)) CoUninitialize();
        return 0;
    }

    bool needCam = !isCameraRegistered();
    bool needMic = !isMicRegistered();
    bool needCable = !isVBCableInstalled();

    if (needCam || needMic || needCable) {
        if (isRunAsAdmin()) {
            registerAllDrivers();
        }
        else {
            if (relaunchAsAdmin("--register")) {
                Sleep(2000);
            }
            else {
                printf("[WARN] Driver registration skipped or not elevated. Continuing to launch GUI...\n");
            }
        }
    }

    // --- Инициализация и загрузка полного конфига (ConfigManager) ---
    ConfigManager::instance().load();
    AppConfig cfg = ConfigManager::instance().get();

    bool initShowConsole = cfg.show_console;
    bool initCloseToTray = cfg.close_to_tray;
    std::wstring targetSkinFile = (cfg.ui_skin == "index3.html") ? L"index3.html" : L"index2.html";

    // --- Инициализация диагностической консоли ---
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        AllocConsole();
    }
    FILE* fpOut = nullptr;
    FILE* fpErr = nullptr;
    FILE* fpIn = nullptr;
    freopen_s(&fpOut, "CONOUT$", "w", stdout);
    freopen_s(&fpErr, "CONOUT$", "w", stderr);
    freopen_s(&fpIn, "CONIN$", "r", stdin);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    SetConsoleTitleW(L"VirtualCamNative - Мониторинг задержек и FPS");

    HWND hConsole = GetConsoleWindow();
    if (hConsole) {
        ShowWindow(hConsole, initShowConsole ? SW_SHOW : SW_HIDE);
    }

    printf("\n====================================================================================\n");
    printf("   VirtualCamNative Diagnostics & Frame Latency Monitor\n");
    printf("====================================================================================\n");
    printf(" [HOTKEYS] Ctrl+Shift+M : Заглушить / Включить микрофон\n");
    printf(" [HOTKEYS] Ctrl+Shift+B : Шторка приватности (Blackout)\n");
    printf(" [HOTKEYS] Ctrl+Shift+C : Переключение камеры (Фронтальная / Основная)\n");
    printf(" [HOTKEYS] Ctrl+Shift+T : Быстрое переключение Troll FX\n");
    printf("====================================================================================\n\n");
    fflush(stdout);

    DeviceDiscoveryService::instance().start(8888);

    g_httpServer.start(8000);

    if (cfg.auto_check_updates) {
        UpdateManager::instance().checkForUpdatesAsync(false);
    }

    std::thread streamThread(videoStreamWorker);

    GuiWindow gui(GetModuleHandle(nullptr));
    if (gui.create(L"VirtualCamNative", 1240, 760)) {
        gui.navigate(L"http://127.0.0.1:8000/" + targetSkinFile);
        gui.runMessageLoop();
    }

    g_isAppRunning = false;
    g_isStreamActive = false;
    if (streamThread.joinable()) {
        streamThread.join();
    }

    DeviceDiscoveryService::instance().stop();
    g_httpServer.stop();

    if (SUCCEEDED(hrCom)) {
        CoUninitialize();
    }
    return 0;
}