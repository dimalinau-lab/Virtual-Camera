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

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ole32.lib")

std::atomic<float> g_audioVolume{ 1.0f };
std::atomic<bool> g_audioMuted{ false };

std::atomic<bool> g_isAppRunning{ true };
std::atomic<bool> g_isStreamActive{ false };
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

inline void transformPortraitDirect(
    const uint32_t* __restrict src, int inW, int inH,
    uint32_t* __restrict canvas, int canvasW, int canvasH,
    bool mirror, bool isFront)
{
    const int targetH = canvasH;
    int targetW = (inH * canvasH) / inW;
    if (targetW % 2 != 0) targetW--;
    if (targetW > canvasW) targetW = canvasW;

    const int offsetX = (canvasW - targetW) / 2;
    const int rightX = offsetX + targetW;

    const int64_t stepX_fp = ((int64_t)inW << 16) / targetH;
    const int64_t stepY_fp = ((int64_t)inH << 16) / targetW;

    for (int y = 0; y < canvasH; ++y) {
        uint32_t* row = canvas + y * canvasW;
        if (offsetX > 0) {
            memset(row, 0, offsetX * sizeof(uint32_t));
        }
        if (canvasW > rightX) {
            memset(row + rightX, 0, (canvasW - rightX) * sizeof(uint32_t));
        }

        // Вертикаль сенсора: при прямой ориентации берём выборку сверху вниз
        int srcX = isFront
            ? (int)(((targetH - 1 - y) * stepX_fp) >> 16)
            : (int)((y * stepX_fp) >> 16);

        if (srcX >= inW) srcX = inW - 1;
        if (srcX < 0) srcX = 0;

        for (int dx = 0; dx < targetW; ++dx) {
            int srcY;
            if (!isFront) {
                // Основная камера (правильная горизонтальная развёртка)
                srcY = mirror
                    ? (int)((dx * stepY_fp) >> 16)
                    : (int)(((targetW - 1 - dx) * stepY_fp) >> 16);
            }
            else {
                // Фронтальная камера
                srcY = mirror
                    ? (int)(((targetW - 1 - dx) * stepY_fp) >> 16)
                    : (int)((dx * stepY_fp) >> 16);
            }

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

    TcpReceiver receiver;
    AudioReceiver audioReceiver;

    std::vector<uint8_t> naluBuffer;
    naluBuffer.reserve(8 * 1024 * 1024);

    std::vector<uint32_t> canvasBgra((size_t)canvasW * canvasH, 0xFF000000);
    std::vector<uint8_t> nv12Buffer(nv12Size);
    std::vector<uint8_t> prevNv12Buffer(nv12Size);
    std::vector<uint8_t> interpNv12Buffer(nv12Size);
    bool hasPrevNv12 = false;

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

        std::fill(canvasBgra.begin(), canvasBgra.end(), 0xFF000000);

        bool isCurrently4K = false;

        while (g_isAppRunning && g_isStreamActive) {
            if (g_fpsChanged.exchange(false)) {
                int targetFps = g_currentFps.load();
                vcamWriter.setFps(targetFps);
                decoder.flush();
                hasPrevNv12 = false;
            }

            audioReceiver.setVolume(g_audioVolume.load());
            audioReceiver.setMute(g_audioMuted.load());

            int naluSize = receiver.receiveNalu(naluBuffer);
            if (naluSize <= 0) {
                break;
            }

            int pending = receiver.getPendingBytes();
            int bloatThreshold = isCurrently4K ? (1500 * 1024) : (350 * 1024);

            if (pending > bloatThreshold) {
                while (pending > (isCurrently4K ? (400 * 1024) : (100 * 1024)) && g_isStreamActive) {
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

            decoder.decodeNalu(naluBuffer.data(), naluSize, [&](const uint8_t* rawBgra, int inW, int inH) {
                if (!rawBgra || inW <= 0 || inH <= 0) return;

                isCurrently4K = (inW >= 3840 || inH >= 3840);

                if (inW != prevInW || inH != prevInH) {
                    prevInW = inW;
                    prevInH = inH;
                    if (swsLandscape) { sws_freeContext(swsLandscape); swsLandscape = nullptr; }
                    if (swsCanvasToNv12) { sws_freeContext(swsCanvasToNv12); swsCanvasToNv12 = nullptr; }
                    std::fill(canvasBgra.begin(), canvasBgra.end(), 0xFF000000);
                    hasPrevNv12 = false;
                }

                bool isLandscape = g_isLandscapeMode.load();

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
                        g_isFrontCamera.load()
                    );
                }

                // --- TROLL FX: 5 FPS Лимит ---
                if (g_trollFpsLimit.load()) {
                    auto nowTroll = std::chrono::steady_clock::now();
                    if (std::chrono::duration_cast<std::chrono::milliseconds>(nowTroll - lastTrollFrameTime).count() < 200) {
                        return;
                    }
                    lastTrollFrameTime = nowTroll;
                }

                // --- TROLL FX: Наложение эффектов (пиксели, глитч, биткраш, пересвет) ---
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

                auto tEndPipeline = std::chrono::high_resolution_clock::now();
                float pipelineMs = std::chrono::duration<float, std::milli>(tEndPipeline - tStartPipeline).count();

                auto now = std::chrono::steady_clock::now();
                std::chrono::duration<float> elapsed = now - lastFpsTime;
                if (elapsed.count() >= 1.0f) {
                    float currentFps = fpsCounter / elapsed.count();

                    std::string resBadge = "H.265 (" + std::to_string(prevInW) + "x" + std::to_string(prevInH) + ")";
                    g_httpServer.updateTelemetry(currentFps, "Active", resBadge);

                    char diagMsg[128];
                    snprintf(diagMsg, sizeof(diagMsg), "[DIAG] FPS: %.1f | Рендер: %.2f ms | Разрешение: %dx%d | Цель: %d FPS\n",
                        currentFps, pipelineMs, prevInW, prevInH, g_currentFps.load());
                    OutputDebugStringA(diagMsg);

                    fpsCounter = 0;
                    lastFpsTime = now;
                }
                });
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
                if (SUCCEEDED(hrCom)) CoUninitialize();
                return -1;
            }
        }
    }

    DeviceDiscoveryService::instance().start(8888);

    g_httpServer.start(8000);

    std::thread streamThread(videoStreamWorker);

    GuiWindow gui(GetModuleHandle(nullptr));
    if (gui.create(L"VirtualCamNative", 1240, 760)) {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::filesystem::path dir = std::filesystem::path(exePath).parent_path();

        // Проверяем, какой интерфейс был выбран последним
        std::wstring targetSkinFile = L"index.html";
        std::filesystem::path cfgPath = dir / "config.json";
        std::ifstream cfgFile(cfgPath);
        if (cfgFile.is_open()) {
            std::stringstream ss;
            ss << cfgFile.rdbuf();
            std::string content = ss.str();
            if (content.find("\"ui_skin\":\"index2.html\"") != std::string::npos) {
                targetSkinFile = L"index2.html";
            }
        }

        std::wstring htmlPath = (dir / targetSkinFile).wstring();
        gui.navigate(htmlPath);
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