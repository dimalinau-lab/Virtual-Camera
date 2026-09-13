#pragma comment(linker, "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup")
#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
#include <thread>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

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

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

std::atomic<bool> g_isAppRunning{ true };
std::atomic<bool> g_isStreamActive{ false };
std::atomic<bool> g_connectRequested{ false };
std::string g_targetIp = "127.0.0.1";
int g_targetPort = 8554;
std::string g_targetMode = "usb";

std::atomic<int> g_currentWidth{ 1280 };
std::atomic<int> g_currentHeight{ 720 };
std::atomic<int> g_currentFps{ 30 };
std::atomic<bool> g_resolutionChanged{ false };

std::atomic<bool> g_mirrorEnabled{ false };
std::atomic<bool> g_blurEnabled{ false };
std::atomic<bool> g_isFrontCamera{ false };
std::atomic<bool> g_isLandscapeMode{ false };

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

    std::system(cmdVideo.c_str());
    std::system(cmdControl.c_str());
    return true;
}

inline void transformPortraitFrame(const uint32_t* src, int w, int h, uint32_t* dst, bool mirror, bool isFront) {
    const int blockSize = 32;
    for (int y0 = 0; y0 < h; y0 += blockSize) {
        for (int x0 = 0; x0 < w; x0 += blockSize) {
            int yMax = (std::min)(y0 + blockSize, h);
            int xMax = (std::min)(x0 + blockSize, w);
            for (int y = y0; y < yMax; ++y) {
                const uint32_t* srcRow = src + y * w;
                for (int x = x0; x < xMax; ++x) {
                    int dstX, dstY;
                    if (!isFront) {
                        dstX = mirror ? y : (h - 1 - y);
                        dstY = x;
                    }
                    else {
                        dstX = mirror ? (h - 1 - y) : y;
                        dstY = w - 1 - x;
                    }
                    dst[dstY * h + dstX] = srcRow[x];
                }
            }
        }
    }
}

void videoStreamWorker() {
    const int canvasW = 1280;
    const int canvasH = 720;

    MfVirtualCamWriter vcamWriter;
    vcamWriter.start(canvasW, canvasH);

    NvdecDecoder decoder;
    decoder.init(1280, 720);

    TcpReceiver receiver;
    std::vector<uint8_t> naluBuffer;
    naluBuffer.reserve(4 * 1024 * 1024);

    std::vector<uint32_t> canvasBgra((size_t)canvasW * canvasH, 0xFF000000);
    std::vector<uint8_t> nv12Buffer((size_t)canvasW * canvasH * 3 / 2);

    std::vector<uint32_t> rotatedBuffer;
    std::vector<uint32_t> scaledBuffer;

    SwsContext* swsScale = nullptr;
    SwsContext* swsCanvasToNv12 = nullptr;

    int fpsCounter = 0;
    auto lastFpsTime = std::chrono::steady_clock::now();

    while (g_isAppRunning) {
        if (!g_connectRequested) {
            Sleep(30);
            continue;
        }

        std::string ip = g_targetIp;
        int port = g_targetPort;

        if (g_targetMode == "usb") {
            setupAdbForwards();
        }

        std::cout << "[CONNECT] Подключение по " << g_targetMode << " к " << ip << ":" << port << "...\n";
        if (!receiver.connectToPhone(ip, port)) {
            Sleep(250);
            continue;
        }

        std::cout << "[CONNECT] Поток успешно запущен!\n";
        g_isStreamActive = true;
        g_connectRequested = false;
        decoder.flush();

        while (g_isAppRunning && g_isStreamActive) {
            u_long pendingBytes = 0;
            ioctlsocket(receiver.getSocket(), FIONREAD, &pendingBytes);

            int naluSize = receiver.receiveNalu(naluBuffer);
            if (naluSize <= 0) {
                Sleep(2);
                break;
            }

            // Быстрый сброс хвоста очереди для удержания низкой задержки
            if (pendingBytes > 65536) {
                int dropCount = 0;
                while (pendingBytes > 32768 && dropCount < 3) {
                    int nextSize = receiver.receiveNalu(naluBuffer);
                    if (nextSize > 0) {
                        naluSize = nextSize;
                    }
                    dropCount++;
                    ioctlsocket(receiver.getSocket(), FIONREAD, &pendingBytes);
                }
            }

            auto tStartPipeline = std::chrono::high_resolution_clock::now();

            decoder.decodeNalu(naluBuffer.data(), naluSize, [&](const uint8_t* rawBgra, int inW, int inH) {
                if (!rawBgra || inW <= 0 || inH <= 0) return;

                bool isLandscape = g_isLandscapeMode.load();
                std::fill(canvasBgra.begin(), canvasBgra.end(), 0xFF000000);

                if (isLandscape) {
                    // Используем легковесный алгоритм SWS_POINT для сокращения задержки до < 10 ms
                    swsScale = sws_getCachedContext(
                        swsScale,
                        inW, inH, AV_PIX_FMT_BGRA,
                        canvasW, canvasH, AV_PIX_FMT_BGRA,
                        SWS_POINT, nullptr, nullptr, nullptr
                    );

                    if (swsScale) {
                        const uint8_t* srcSlice[4] = { rawBgra, nullptr, nullptr, nullptr };
                        int srcStride[4] = { inW * 4, 0, 0, 0 };

                        uint8_t* dstSlice[4] = { reinterpret_cast<uint8_t*>(canvasBgra.data()), nullptr, nullptr, nullptr };
                        int dstStride[4] = { canvasW * 4, 0, 0, 0 };

                        sws_scale(swsScale, srcSlice, srcStride, 0, inH, dstSlice, dstStride);
                    }

                    if (g_mirrorEnabled.load()) {
                        for (int y = 0; y < canvasH; ++y) {
                            uint32_t* row = canvasBgra.data() + y * canvasW;
                            for (int x = 0; x < canvasW / 2; ++x) {
                                std::swap(row[x], row[canvasW - 1 - x]);
                            }
                        }
                    }

                    g_httpServer.updatePreviewFrame(reinterpret_cast<const uint8_t*>(canvasBgra.data()), canvasW, canvasH);
                }
                else {
                    int rotW = inH;
                    int rotH = inW;
                    size_t rotPixels = (size_t)rotW * rotH;

                    if (rotatedBuffer.size() != rotPixels) {
                        rotatedBuffer.resize(rotPixels);
                    }

                    transformPortraitFrame(
                        reinterpret_cast<const uint32_t*>(rawBgra),
                        inW, inH,
                        rotatedBuffer.data(),
                        g_mirrorEnabled.load(),
                        g_isFrontCamera.load()
                    );

                    g_httpServer.updatePreviewFrame(reinterpret_cast<const uint8_t*>(rotatedBuffer.data()), rotW, rotH);

                    int targetH = canvasH;
                    int targetW = (rotW * canvasH) / rotH;
                    if (targetW % 2 != 0) targetW--;

                    int offsetX = (canvasW - targetW) / 2;
                    if (offsetX < 0) offsetX = 0;

                    size_t scaledPixels = (size_t)targetW * targetH;
                    if (scaledBuffer.size() != scaledPixels) {
                        scaledBuffer.resize(scaledPixels);
                    }

                    swsScale = sws_getCachedContext(
                        swsScale,
                        rotW, rotH, AV_PIX_FMT_BGRA,
                        targetW, targetH, AV_PIX_FMT_BGRA,
                        SWS_POINT, nullptr, nullptr, nullptr
                    );

                    if (swsScale) {
                        const uint8_t* srcSlice[4] = { reinterpret_cast<const uint8_t*>(rotatedBuffer.data()), nullptr, nullptr, nullptr };
                        int srcStride[4] = { rotW * 4, 0, 0, 0 };

                        uint8_t* dstSlice[4] = { reinterpret_cast<uint8_t*>(scaledBuffer.data()), nullptr, nullptr, nullptr };
                        int dstStride[4] = { targetW * 4, 0, 0, 0 };

                        sws_scale(swsScale, srcSlice, srcStride, 0, rotH, dstSlice, dstStride);

                        for (int y = 0; y < targetH; ++y) {
                            memcpy(
                                canvasBgra.data() + (y * canvasW + offsetX),
                                scaledBuffer.data() + (y * targetW),
                                targetW * sizeof(uint32_t)
                            );
                        }
                    }
                }

                // Конвертация холста в NV12 через SWS_POINT
                swsCanvasToNv12 = sws_getCachedContext(
                    swsCanvasToNv12,
                    canvasW, canvasH, AV_PIX_FMT_BGRA,
                    canvasW, canvasH, AV_PIX_FMT_NV12,
                    SWS_POINT, nullptr, nullptr, nullptr
                );

                if (swsCanvasToNv12) {
                    const uint8_t* srcSlice[4] = { reinterpret_cast<const uint8_t*>(canvasBgra.data()), nullptr, nullptr, nullptr };
                    int srcStride[4] = { canvasW * 4, 0, 0, 0 };

                    uint8_t* dstY = nv12Buffer.data();
                    uint8_t* dstUV = dstY + ((size_t)canvasW * canvasH);
                    uint8_t* dstSlice[4] = { dstY, dstUV, nullptr, nullptr };
                    int dstStride[4] = { canvasW, canvasW, 0, 0 };

                    sws_scale(swsCanvasToNv12, srcSlice, srcStride, 0, canvasH, dstSlice, dstStride);
                    vcamWriter.writeFrameNV12(nv12Buffer.data());
                }

                auto tEndPipeline = std::chrono::high_resolution_clock::now();
                float pipelineMs = std::chrono::duration<float, std::milli>(tEndPipeline - tStartPipeline).count();

                fpsCounter++;
                auto now = std::chrono::steady_clock::now();
                std::chrono::duration<float> elapsed = now - lastFpsTime;
                if (elapsed.count() >= 1.0f) {
                    float currentFps = fpsCounter / elapsed.count();
                    g_httpServer.updateTelemetry(currentFps, "Active", "H.265 / HEVC");

                    char diagMsg[256];
                    snprintf(diagMsg, sizeof(diagMsg),
                        "[DIAG] FPS: %.1f | Рендер: %.2f ms | Сокет: %.1f KB\n",
                        currentFps, pipelineMs, static_cast<float>(pendingBytes) / 1024.0f);
                    OutputDebugStringA(diagMsg);
                    std::cout << diagMsg;

                    fpsCounter = 0;
                    lastFpsTime = now;
                }
                });
        }

        receiver.stop();
        if (g_isStreamActive.load()) {
            Sleep(150);
            g_connectRequested = true;
        }
    }

    if (swsScale) sws_freeContext(swsScale);
    if (swsCanvasToNv12) sws_freeContext(swsCanvasToNv12);
    vcamWriter.stop();
}

int main(int argc, char* argv[]) {
    // Скрываем окно консоли для релизной сборки
    HWND hConsole = GetConsoleWindow();
    if (hConsole) {
        ShowWindow(hConsole, SW_HIDE);
    }

    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    if (argc > 1 && std::string(argv[1]) == "--register") {
        registerVirtualCamDll();
        return 0;
    }

    if (!isCameraRegistered()) {
        if (isRunAsAdmin()) {
            registerVirtualCamDll();
        }
        else {
            if (relaunchAsAdmin("--register")) {
                Sleep(1500);
            }
            else {
                return -1;
            }
        }
    }

    g_httpServer.start(8000);

    std::thread streamThread(videoStreamWorker);

    GuiWindow gui(GetModuleHandle(nullptr));
    if (gui.create(L"VirtualCamNative", 1240, 760)) {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::wstring htmlPath = (std::filesystem::path(exePath).parent_path() / L"index.html").wstring();

        gui.navigate(htmlPath);
        gui.runMessageLoop();
    }

    g_isAppRunning = false;
    g_isStreamActive = false;
    if (streamThread.joinable()) {
        streamThread.join();
    }
    g_httpServer.stop();

    return 0;
}