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
#include <mutex>
#include <fstream>
#include <iomanip>

inline void logDebug(const std::string& msg) {
    static std::mutex s_logMutex;
    std::lock_guard<std::mutex> lock(s_logMutex);
    std::ofstream ofs("crash_debug.log", std::ios::app);
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    auto timer = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_s(&tm, &timer);
    ofs << std::put_time(&tm, "%H:%M:%S") << "." << std::setfill('0') << std::setw(3) << ms.count() << " " << msg << std::endl;
}

#include <algorithm>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <mmsystem.h>
#include <immintrin.h>
#include <omp.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

inline void dumpStackTrace(std::ostream& os, CONTEXT* ctx) {
    HANDLE hProcess = GetCurrentProcess();
    HANDLE hThread = GetCurrentThread();

    SymInitialize(hProcess, nullptr, TRUE);
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);

    STACKFRAME64 frame = {};
#ifdef _M_X64
    DWORD machineType = IMAGE_FILE_MACHINE_AMD64;
    if (ctx) {
        frame.AddrPC.Offset = ctx->Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = ctx->Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = ctx->Rsp;
        frame.AddrStack.Mode = AddrModeFlat;
    }
#else
    DWORD machineType = IMAGE_FILE_MACHINE_I386;
    if (ctx) {
        frame.AddrPC.Offset = ctx->Eip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = ctx->Ebp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = ctx->Esp;
        frame.AddrStack.Mode = AddrModeFlat;
    }
#endif

    CONTEXT ctxCopy;
    if (ctx) {
        ctxCopy = *ctx;
    } else {
        RtlCaptureContext(&ctxCopy);
#ifdef _M_X64
        frame.AddrPC.Offset = ctxCopy.Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = ctxCopy.Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = ctxCopy.Rsp;
        frame.AddrStack.Mode = AddrModeFlat;
#endif
    }

    os << "\n--- STACK TRACE ---" << std::endl;
    int frameNum = 0;
    while (StackWalk64(machineType, hProcess, hThread, &frame, &ctxCopy, nullptr,
                       SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
        if (frame.AddrPC.Offset == 0) break;

        char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)] = {};
        auto* symbol = reinterpret_cast<PSYMBOL_INFO>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = MAX_SYM_NAME;

        DWORD64 displacement = 0;
        std::string symName = "<unknown>";
        if (SymFromAddr(hProcess, frame.AddrPC.Offset, &displacement, symbol)) {
            symName = symbol->Name;
        }

        IMAGEHLP_LINE64 line = {};
        line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
        DWORD lineDisp = 0;
        std::string fileLine = "";
        if (SymGetLineFromAddr64(hProcess, frame.AddrPC.Offset, &lineDisp, &line)) {
            fileLine = std::string(line.FileName) + ":" + std::to_string(line.LineNumber);
        }

        os << "#" << std::dec << frameNum++ << " 0x" << std::hex << frame.AddrPC.Offset
           << " in " << symName;
        if (!fileLine.empty()) {
            os << " at " << fileLine;
        }
        os << std::endl;
    }
    os.flush();
}

inline LONG WINAPI CustomUnhandledExceptionFilter(EXCEPTION_POINTERS* pExceptionPointers) {
    std::ofstream ofs("crash_fatal.log", std::ios::out);
    ofs << "=== UNHANDLED SEH EXCEPTION ===" << std::endl;
    if (pExceptionPointers && pExceptionPointers->ExceptionRecord) {
        auto* rec = pExceptionPointers->ExceptionRecord;
        ofs << "Exception Code: 0x" << std::hex << std::uppercase << rec->ExceptionCode << std::endl;
        ofs << "Exception Flags: 0x" << rec->ExceptionFlags << std::endl;
        ofs << "Exception Address: 0x" << rec->ExceptionAddress << std::endl;
        dumpStackTrace(ofs, pExceptionPointers->ContextRecord);
    } else {
        dumpStackTrace(ofs, nullptr);
    }
    ofs.flush();
    return EXCEPTION_CONTINUE_SEARCH;
}

inline void CustomTerminateHandler() {
    std::ofstream ofs("crash_fatal.log", std::ios::app);
    ofs << "=== STD::TERMINATE INVOKED ===" << std::endl;
    std::exception_ptr ep = std::current_exception();
    if (ep) {
        try {
            std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            ofs << "C++ Exception: " << e.what() << " | Type: " << typeid(e).name() << std::endl;
        } catch (...) {
            ofs << "Unknown non-std::exception" << std::endl;
        }
    } else {
        ofs << "No active C++ exception (terminate called directly)" << std::endl;
    }
    dumpStackTrace(ofs, nullptr);
    ofs.flush();
    std::abort();
}

inline void initCrashHandler() {
    SetUnhandledExceptionFilter(CustomUnhandledExceptionFilter);
    std::set_terminate(CustomTerminateHandler);
}

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
#include "app_state.hpp"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ole32.lib")

void setConsoleVisible(bool visible) {
    g_app.setConsoleVisible(visible);
}

bool isConsoleVisible() {
    return g_app.isConsoleVisible();
}

void setCloseToTray(bool closeToTray) {
    g_app.setCloseToTray(closeToTray);
}

bool isCloseToTray() {
    return g_app.isCloseToTray();
}

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
    bool mirror, bool isFront, bool flip180 = false,
    int aspectRatioMode = 0)
{
    const int targetH = canvasH;
    int targetW = (inH * canvasH) / inW; // 9:16 Phone Portrait: 405x720
    if (aspectRatioMode == 1) {
        targetW = (canvasH * 4) / 3;     // 4:3 Classic: 960x720
    } else if (aspectRatioMode == 2) {
        targetW = canvasW;               // 16:9 Wide: 1280x720
    }
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

// Применение эффектов приколов (144p, глитчи, биткраш, пересвет, CCTV)
inline void applyTrollEffects(uint32_t* canvas, int w, int h, bool cctvOnly = false) {
    if (!cctvOnly) {
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
    } // end if (!cctvOnly)

    // 5. CCTV / 90s Camcorder OSD Overlay
    if (g_trollCctv.load()) {
        static auto cctvStart = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        int msElapsed = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(now - cctvStart).count());
        bool recDotBlink = ((msElapsed / 500) % 2 == 0);

        // A) CRT/VHS Scanlines (dim every 3rd line)
        for (int y = 0; y < h; y += 3) {
            uint32_t* row = canvas + y * w;
            for (int x = 0; x < w; ++x) {
                uint32_t p = row[x];
                row[x] = (p & 0xFF000000) | (((p & 0x00FEFEFE) >> 1) + ((p & 0x00FCFCFC) >> 2));
            }
        }

        // B) Blinking Red REC Dot at (50, 45)
        if (recDotBlink && h > 80 && w > 200) {
            for (int dy = -7; dy <= 7; ++dy) {
                for (int dx = -7; dx <= 7; ++dx) {
                    if (dx * dx + dy * dy <= 49) {
                        canvas[(45 + dy) * w + (50 + dx)] = 0xFFFF2222;
                    }
                }
            }
        }

        // C) 5x7 Bitmap Font for Retro Camcorder OSD
        auto getChar5x7Rows = [](char c) -> const uint8_t* {
            static const uint8_t space[7] = {0,0,0,0,0,0,0};
            static const uint8_t d0[7] = {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E};
            static const uint8_t d1[7] = {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E};
            static const uint8_t d2[7] = {0x0E,0x11,0x01,0x06,0x08,0x10,0x1F};
            static const uint8_t d3[7] = {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E};
            static const uint8_t d4[7] = {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02};
            static const uint8_t d5[7] = {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E};
            static const uint8_t d6[7] = {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E};
            static const uint8_t d7[7] = {0x1F,0x01,0x02,0x04,0x08,0x08,0x08};
            static const uint8_t d8[7] = {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E};
            static const uint8_t d9[7] = {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C};
            static const uint8_t colon[7] = {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00};
            static const uint8_t rA[7] = {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11};
            static const uint8_t rB[7] = {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E};
            static const uint8_t rC[7] = {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E};
            static const uint8_t rE[7] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F};
            static const uint8_t rL[7] = {0x10,0x10,0x10,0x10,0x10,0x10,0x1F};
            static const uint8_t rP[7] = {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10};
            static const uint8_t rR[7] = {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11};
            static const uint8_t rT[7] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x04};
            static const uint8_t rY[7] = {0x11,0x11,0x0A,0x04,0x04,0x04,0x04};

            switch (c) {
                case '0': return d0;
                case '1': return d1;
                case '2': return d2;
                case '3': return d3;
                case '4': return d4;
                case '5': return d5;
                case '6': return d6;
                case '7': return d7;
                case '8': return d8;
                case '9': return d9;
                case ':': return colon;
                case 'A': return rA;
                case 'B': return rB;
                case 'C': return rC;
                case 'E': return rE;
                case 'L': return rL;
                case 'P': return rP;
                case 'R': return rR;
                case 'T': return rT;
                case 'Y': return rY;
                default:  return space;
            }
        };

        auto drawChar5x7 = [&](char c, int px, int py, uint32_t color) {
            const uint8_t* rows = getChar5x7Rows(c);
            const int scale = 2;
            for (int r = 0; r < 7; ++r) {
                uint8_t bits = rows[r];
                for (int col = 0; col < 5; ++col) {
                    if (bits & (0x10 >> col)) {
                        for (int sy = 0; sy < scale; ++sy) {
                            for (int sx = 0; sx < scale; ++sx) {
                                int cx = px + col * scale + sx;
                                int cy = py + r * scale + sy;
                                if (cx >= 0 && cx < w && cy >= 0 && cy < h) {
                                    canvas[cy * w + cx] = color;
                                }
                            }
                        }
                    }
                }
            }
        };

        auto drawString = [&](const std::string& str, int sx, int sy, uint32_t col) {
            int curX = sx;
            for (char ch : str) {
                drawChar5x7(ch, curX, sy, col);
                curX += 13;
            }
        };

        drawString("REC", 68, 38, 0xFFFFFFFF);

        int totalSec = msElapsed / 1000;
        int hours = (totalSec / 3600) % 24;
        int mins = (totalSec / 60) % 60;
        int secs = totalSec % 60;
        char timeBuf[32];
        snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d", hours, mins, secs);
        drawString(timeBuf, 50, h - 55, 0xFFE0E0E0);
        drawString("BAT", w - 110, 38, 0xFF55FF55);
    }
}

static inline uint8_t getH265NalType(const uint8_t* data, int size) {
    if (!data || size < 4) return 0;
    if (data[0] == 0 && data[1] == 0) {
        if (data[2] == 1) return (data[3] >> 1) & 0x3F;
        if (size >= 5 && data[2] == 0 && data[3] == 1) return (data[4] >> 1) & 0x3F;
    }
    return 0;
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

inline void fastYuv420pToNv12(const AVFrame* frame, uint8_t* dstNv12, int width, int height) {
    const uint8_t* srcY = frame->data[0];
    int strideY = frame->linesize[0];
    uint8_t* dstY = dstNv12;
    if (strideY == width) {
        memcpy(dstY, srcY, (size_t)width * height);
    } else {
        for (int y = 0; y < height; ++y) {
            memcpy(dstY + y * width, srcY + y * strideY, width);
        }
    }

    const uint8_t* srcU = frame->data[1];
    const uint8_t* srcV = frame->data[2];
    int strideU = frame->linesize[1];
    int strideV = frame->linesize[2];
    uint8_t* dstUV = dstNv12 + ((size_t)width * height);

    int halfW = width / 2;
    int halfH = height / 2;

    for (int y = 0; y < halfH; ++y) {
        const uint8_t* rowU = srcU + y * strideU;
        const uint8_t* rowV = srcV + y * strideV;
        uint8_t* rowUV = dstUV + y * width;

        int x = 0;
        for (; x + 16 <= halfW; x += 16) {
            __m128i u16 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(rowU + x));
            __m128i v16 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(rowV + x));
            __m128i uvLo = _mm_unpacklo_epi8(u16, v16);
            __m128i uvHi = _mm_unpackhi_epi8(u16, v16);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(rowUV + x * 2), uvLo);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(rowUV + x * 2 + 16), uvHi);
        }
        for (; x < halfW; ++x) {
            rowUV[x * 2] = rowU[x];
            rowUV[x * 2 + 1] = rowV[x];
        }
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
    AudioReceiver& audioReceiver = AudioReceiver::instance();
    audioReceiver.start(g_targetIp, 8555);

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
    auto fillBlackNv12 = [canvasW, canvasH](std::vector<uint8_t>& buf) {
        memset(buf.data(), 16, (size_t)canvasW * canvasH);
        memset(buf.data() + (size_t)canvasW * canvasH, 128, (size_t)canvasW * canvasH / 2);
    };
    fillBlackNv12(nv12Buffer);
    fillBlackNv12(prevNv12Buffer);
    fillBlackNv12(interpNv12Buffer);

    int previewSkipCounter = 0;
    bool lastLandscapeMode = g_isLandscapeMode.load();

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

        logDebug("[STREAM] Connect requested to: " + ip + ":" + std::to_string(port));

        if (!receiver.connectToPhone(ip, port)) {
            logDebug("[STREAM] receiver.connectToPhone failed");
            Sleep(250);
            continue;
        }
        logDebug("[STREAM] receiver.connectToPhone succeeded");

        int nodelay = 1;
        setsockopt(receiver.getSocket(), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

        int currentRcvBuf = 256 * 1024;
        setsockopt(receiver.getSocket(), SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&currentRcvBuf), sizeof(currentRcvBuf));

        g_isStreamActive = true;
        g_connectRequested = false;
        logDebug("[STREAM] Reinitializing decoder...");
        decoder.reinit();
        hasPrevNv12 = false;

        logDebug("[STREAM] Calling audioReceiver.setPhoneTarget...");
        audioReceiver.setPhoneTarget(ip, 8555);
        logDebug("[STREAM] audioReceiver.setPhoneTarget finished");
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

        logDebug("[STREAM] Setup completed, creating ingestThread");

        printf("[STREAM] Соединение установлено: %s:%d (режим: %s)\n", ip.c_str(), port, g_targetMode.c_str());
        printf("[STREAM] Запуск мониторинга задержек кадров (Межкадровый Δt + Пайплайн)...\n\n");

        uint64_t totalFrameCount = 0;
        float sumIntervalMs = 0.0f;
        float minIntervalMs = 9999.0f;
        float maxIntervalMs = 0.0f;
        int intervalSamples = 0;

        auto isH265KeyFrame = [](const uint8_t* data, int size) -> bool {
            if (!data || size < 5) return false;
            int offset = -1;
            if (size >= 5 && data[0] == 0 && data[1] == 0 && data[2] == 1) {
                offset = 3;
            } else if (size >= 6 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1) {
                offset = 4;
            }
            if (offset < 0 || offset >= size) return false;
            uint8_t nalType = (data[offset] >> 1) & 0x3F;
            return (nalType >= 16 && nalType <= 21);
        };

        struct QueuedPacket {
            std::vector<uint8_t> buffer;
            int size = 0;
            float frameIntervalMs = 0.0f;
            int pending = 0;
        };

        std::mutex queueMutex;
        std::condition_variable queueCv;
        std::deque<QueuedPacket> frameQueue;
        std::vector<std::vector<uint8_t>> freeBufferPool;
        std::atomic<bool> ingestRunning{ true };

        auto acquireBuffer = [&]() -> std::vector<uint8_t> {
            if (!freeBufferPool.empty()) {
                auto b = std::move(freeBufferPool.back());
                freeBufferPool.pop_back();
                return b;
            }
            std::vector<uint8_t> b;
            b.reserve(512 * 1024);
            return b;
        };

        auto releaseBuffer = [&](std::vector<uint8_t>&& b) {
            if (freeBufferPool.size() < 12) {
                freeBufferPool.push_back(std::move(b));
            }
        };

        // Network Ingest Thread: непрерывно вычитывает сокет параллельно декодеру
        std::thread ingestThread([&]() {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
            std::vector<uint8_t> tempBuf;
            tempBuf.reserve(512 * 1024);

            auto prevArrival = std::chrono::high_resolution_clock::now();
            bool hasPrev = false;

            while (ingestRunning.load() && g_isAppRunning && g_isStreamActive) {
                int received = receiver.receiveNalu(tempBuf);
                if (received < 0) {
                    ingestRunning.store(false);
                    queueCv.notify_all();
                    break;
                }
                if (received == 0) {
                    // Пауза Wi-Fi (WSAETIMEDOUT): продолжаем ожидание пакета без разрыва сокета
                    continue;
                }

                auto now = std::chrono::high_resolution_clock::now();
                float interval = 0.0f;
                if (hasPrev) {
                    interval = std::chrono::duration<float, std::milli>(now - prevArrival).count();
                }
                prevArrival = now;
                hasPrev = true;

                int pendingBytes = receiver.getPendingBytes();

                {
                    std::lock_guard<std::mutex> lock(queueMutex);
                    // Жесткое ограничение очереди: максимум 3 кадра (~50 мс буфера).
                    // Старые кадры мгновенно сбрасываются в пул для предотвращения накопления задержки.
                    while (frameQueue.size() >= 3) {
                        releaseBuffer(std::move(frameQueue.front().buffer));
                        frameQueue.pop_front();
                    }

                    frameQueue.emplace_back();
                    auto& item = frameQueue.back();
                    item.buffer = acquireBuffer();
                    item.buffer.swap(tempBuf);
                    item.size = received;
                    item.frameIntervalMs = interval;
                    item.pending = pendingBytes;
                    if (tempBuf.capacity() < 512 * 1024) tempBuf.reserve(512 * 1024);
                }
                queueCv.notify_one();
            }
        });

        struct IngestThreadGuard {
            std::atomic<bool>& running;
            std::condition_variable& cv;
            TcpReceiver& rcv;
            std::thread& th;
            ~IngestThreadGuard() {
                running.store(false);
                cv.notify_all();
                rcv.stop();
                if (th.joinable()) {
                    th.join();
                }
            }
        } ingestGuard{ ingestRunning, queueCv, receiver, ingestThread };

        try {
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

            // Audio state: обновляем только при изменении (убираем per-frame polling)
            {
                static float  s_vol   = -1.f;
                static bool   s_mute  = false;
                static int    s_delay = -1;
                static bool   s_ng    = false;
                static float  s_ngThr = -1.f;
                float  vol   = g_audioVolume.load();
                bool   mute  = g_audioMuted.load();
                int    delay = g_audioDelayMs.load();
                bool   ng    = g_noiseGateEnabled.load();
                float  ngThr = g_noiseGateThreshold.load();
                if (vol   != s_vol)   { audioReceiver.setVolume(vol);      s_vol   = vol; }
                if (mute  != s_mute)  { audioReceiver.setMute(mute);       s_mute  = mute; }
                if (delay != s_delay) { audioReceiver.setDelayMs(delay);   s_delay = delay; }
                if (ng != s_ng || ngThr != s_ngThr) {
                    audioReceiver.setNoiseGate(ng, ngThr);
                    s_ng = ng; s_ngThr = ngThr;
                }
            }

            std::vector<QueuedPacket> batch;
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                queueCv.wait_for(lock, std::chrono::milliseconds(40), [&]() {
                    return !frameQueue.empty() || !ingestRunning.load() || !g_isStreamActive || !g_isAppRunning || g_connectRequested.load();
                });

                if (g_connectRequested.load() || !g_isStreamActive || !g_isAppRunning) {
                    break;
                }

                if (frameQueue.empty()) {
                    if (!ingestRunning.load()) break;
                    continue;
                }

                // Атомарно вычитываем все накопившиеся пакеты: очередь мгновенно опустошается до 0
                while (!frameQueue.empty()) {
                    batch.push_back(std::move(frameQueue.front()));
                    frameQueue.pop_front();
                }
            }

            if (batch.empty()) continue;

            // Если накопилось несколько кадров (джиттер сети), ищем самый последний ключевой кадр IDR:
            size_t startIndex = 0;
            for (size_t i = batch.size(); i-- > 0; ) {
                if (isH265KeyFrame(batch[i].buffer.data(), batch[i].size)) {
                    startIndex = i;
                    break;
                }
            }

            // Кадры до последнего ключевого кадра полностью сбрасываем без декодирования
            for (size_t i = 0; i < startIndex; ++i) {
                std::lock_guard<std::mutex> lock(queueMutex);
                releaseBuffer(std::move(batch[i].buffer));
            }

            // Промежуточные P-кадры декодируем в турбо-режиме без рендера (< 0.8 мс) для обновления референсов H.265:
            for (size_t i = startIndex; i + 1 < batch.size(); ++i) {
                decoder.decodeNaluDirect(batch[i].buffer.data(), batch[i].size, nullptr);
                std::lock_guard<std::mutex> lock(queueMutex);
                releaseBuffer(std::move(batch[i].buffer));
            }

            // Самый свежий кадр — выводим на экран без малейшей задержки:
            QueuedPacket currentPacket = std::move(batch.back());
            int naluSize = currentPacket.size;
            float frameIntervalMs = currentPacket.frameIntervalMs;
            int pending = currentPacket.pending;
            const uint8_t* pNaluData = currentPacket.buffer.data();

            if (frameIntervalMs > 0.0f) {
                sumIntervalMs += frameIntervalMs;
                if (frameIntervalMs < minIntervalMs) minIntervalMs = frameIntervalMs;
                if (frameIntervalMs > maxIntervalMs) maxIntervalMs = frameIntervalMs;
                intervalSamples++;
            }

            auto tStartPipeline = std::chrono::high_resolution_clock::now();

            if (g_privacyShield.load()) {
                // Privacy Shield: быстрый матовый темный фон (Y=16, UV=128) без декодирования
                memset(nv12Buffer.data(), 16, canvasW * canvasH);
                memset(nv12Buffer.data() + canvasW * canvasH, 128, canvasW * canvasH / 2);
                vcamWriter.writeFrameNV12(nv12Buffer.data());
                fpsCounter++;
                {
                    std::lock_guard<std::mutex> lock(queueMutex);
                    releaseBuffer(std::move(currentPacket.buffer));
                }
                Sleep(16);
                continue;
            }

            bool hasActiveTrollFx = g_trollFpsLimit.load() || (g_trollPixelate.load() > 1) ||
                                    g_trollGlitch.load() || g_trollBitcrush.load() || g_trollOverexposure.load() ||
                                    g_trollCctv.load() || g_trollFakeLag.load();
            bool hasActiveOptics = StudioOptics::instance().hasActiveOptics() || (g_bgEffectMode.load() > 0);
            bool isLandscape = g_isLandscapeMode.load();

            if (isLandscape && !hasActiveTrollFx && !hasActiveOptics) {
                // --- FAST-PATH ZERO-COPY: Прямая конвертация в NV12 без промежуточного BGRA (Альбомный режим) ---
                decoder.decodeNaluDirect(pNaluData, naluSize, [&](const AVFrame* frame, int inW, int inH) {
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
                    else if ((AVPixelFormat)frame->format == AV_PIX_FMT_YUV420P && inW == canvasW && inH == canvasH) {
                        fastYuv420pToNv12(frame, nv12Buffer.data(), canvasW, canvasH);
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

                        vcamWriter.writeFrameNV12(nv12Buffer.data());
                        fpsCounter++;
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
                // --- FX / PORTRAIT PATH: DIRECT ZERO-COPY NV12 (< 1.5 ms GPU) ---
                decoder.decodeNaluDirect(pNaluData, naluSize, [&](const AVFrame* frame, int inW, int inH) {
                    if (!frame || inW <= 0 || inH <= 0) return;
                    auto tStartRender = std::chrono::high_resolution_clock::now();

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
                        hasPrevNv12 = false;
                    }

                    // Быстрая векторная конвертация YUV420P -> NV12 через AVX2 (0.12 мс)
                    static std::vector<uint8_t> s_rawNv12Buf;
                    size_t neededRawNv12Size = (size_t)inW * inH * 3 / 2;
                    if (s_rawNv12Buf.size() != neededRawNv12Size) {
                        s_rawNv12Buf.resize(neededRawNv12Size);
                    }

                    const uint8_t* srcNv12Ptr = nullptr;
                    if ((AVPixelFormat)frame->format == AV_PIX_FMT_NV12 && frame->linesize[0] == inW && frame->linesize[1] == inW) {
                        srcNv12Ptr = frame->data[0];
                    } else if ((AVPixelFormat)frame->format == AV_PIX_FMT_YUV420P) {
                        fastYuv420pToNv12(frame, s_rawNv12Buf.data(), inW, inH);
                        srcNv12Ptr = s_rawNv12Buf.data();
                    } else {
                        swsDirectToNv12 = sws_getCachedContext(
                            swsDirectToNv12,
                            inW, inH, (AVPixelFormat)frame->format,
                            inW, inH, AV_PIX_FMT_NV12,
                            SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
                        );
                        if (swsDirectToNv12) {
                            uint8_t* dstY = s_rawNv12Buf.data();
                            uint8_t* dstUV = dstY + ((size_t)inW * inH);
                            uint8_t* dstSlice[4] = { dstY, dstUV, nullptr, nullptr };
                            int dstStride[4] = { inW, inW, 0, 0 };
                            sws_scale(swsDirectToNv12, frame->data, frame->linesize, 0, inH, dstSlice, dstStride);
                            srcNv12Ptr = s_rawNv12Buf.data();
                        }
                    }
                    if (!srcNv12Ptr) return;

                    // Troll FX: Fake Lag / Internet Disconnect Stutter Simulation
                    if (g_trollFakeLag.load()) {
                        static auto s_lastLagCheck = std::chrono::steady_clock::now();
                        static bool s_inLagStutter = false;
                        static auto s_stutterUntil = std::chrono::steady_clock::now();
                        auto nowLag = std::chrono::steady_clock::now();

                        if (!s_inLagStutter) {
                            auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(nowLag - s_lastLagCheck).count();
                            if (elapsedMs > 1600 && (rand() % 16 == 0)) {
                                s_inLagStutter = true;
                                s_stutterUntil = nowLag + std::chrono::milliseconds(260 + (rand() % 260));
                                s_lastLagCheck = nowLag;
                            }
                        } else {
                            if (nowLag < s_stutterUntil) {
                                return; // Freeze video frame
                            } else {
                                s_inLagStutter = false;
                                s_lastLagCheck = nowLag;
                            }
                        }
                    }

                    if (g_trollFpsLimit.load()) {
                        auto nowTroll = std::chrono::steady_clock::now();
                        if (std::chrono::duration_cast<std::chrono::milliseconds>(nowTroll - lastTrollFrameTime).count() < 200) {
                            return;
                        }
                        lastTrollFrameTime = nowTroll;
                    }

                    // --- DIRECT3D 11 GPU PIPELINE: ZERO-COPY NV12 -> STUDIO OPTICS -> NV12 (~1.0 ms) ---
                    bool usedD3D11 = false;
                    if (D3D11OpticsPipeline::instance().isReady() && !g_trollCctv.load()) {
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
                        gpuParams.portraitMirror   = g_mirrorEnabled.load() ? 1 : 0;
                        gpuParams.portraitFlip180  = g_flip180.load()       ? 1 : 0;
                        gpuParams.portraitIsFront  = g_isFrontCamera.load() ? 1 : 0;
                        gpuParams.aspectRatioMode  = g_aspectRatioMode.load();

                        usedD3D11 = D3D11OpticsPipeline::instance().processNv12ToNv12(srcNv12Ptr, inW, inH, nv12Buffer.data(), gpuParams);
                    }

                    if (!usedD3D11 && hasPrevNv12 && D3D11OpticsPipeline::instance().isReady() && !g_trollCctv.load()) {
                        // Zero-Stall: мгновенно сохраняем 60 FPS на предыдущем кадре без CPU-блокировок
                        usedD3D11 = true;
                    }

                    if (usedD3D11) {
                        vcamWriter.writeFrameNV12(nv12Buffer.data());
                        fpsCounter++;
                        hasPrevNv12 = true;

                        if (++previewSkipCounter % 20 == 0) {
                            g_httpServer.updatePreviewFrameNv12(nv12Buffer.data(), canvasW, canvasH);
                        }
                    } else {
                        // --- CPU FALLBACK PATH ---
                        if (isLandscape) {
                            swsLandscape = sws_getCachedContext(
                                swsLandscape,
                                inW, inH, AV_PIX_FMT_NV12,
                                canvasW, canvasH, AV_PIX_FMT_BGRA,
                                SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
                            );
                            if (swsLandscape) {
                                const uint8_t* srcSlice[4] = { srcNv12Ptr, srcNv12Ptr + ((size_t)inW * inH), nullptr, nullptr };
                                int srcStride[4] = { inW, inW, 0, 0 };
                                uint8_t* dstSlice[4] = { reinterpret_cast<uint8_t*>(canvasBgra.data()), nullptr, nullptr, nullptr };
                                int dstStride[4] = { canvasW * 4, 0, 0, 0 };
                                sws_scale(swsLandscape, srcSlice, srcStride, 0, inH, dstSlice, dstStride);
                            }
                            if (g_flip180.load()) {
                                for (int y = 0; y < canvasH / 2; ++y) {
                                    uint32_t* rowTop = canvasBgra.data() + y * canvasW;
                                    uint32_t* rowBottom = canvasBgra.data() + (canvasH - 1 - y) * canvasW;
                                    for (int x = 0; x < canvasW; ++x) std::swap(rowTop[x], rowBottom[canvasW - 1 - x]);
                                }
                            }
                            if (g_mirrorEnabled.load()) {
                                for (int y = 0; y < canvasH; ++y) {
                                    uint32_t* row = canvasBgra.data() + y * canvasW;
                                    for (int x = 0; x < canvasW / 2; ++x) std::swap(row[x], row[canvasW - 1 - x]);
                                }
                            }
                        } else {
                            static std::vector<uint32_t> s_tempInBgra;
                            if (s_tempInBgra.size() != (size_t)inW * inH) s_tempInBgra.resize((size_t)inW * inH);
                            swsLandscape = sws_getCachedContext(
                                swsLandscape,
                                inW, inH, AV_PIX_FMT_NV12,
                                inW, inH, AV_PIX_FMT_BGRA,
                                SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
                            );
                            if (swsLandscape) {
                                const uint8_t* srcSlice[4] = { srcNv12Ptr, srcNv12Ptr + ((size_t)inW * inH), nullptr, nullptr };
                                int srcStride[4] = { inW, inW, 0, 0 };
                                uint8_t* dstSlice[4] = { reinterpret_cast<uint8_t*>(s_tempInBgra.data()), nullptr, nullptr, nullptr };
                                int dstStride[4] = { inW * 4, 0, 0, 0 };
                                sws_scale(swsLandscape, srcSlice, srcStride, 0, inH, dstSlice, dstStride);
                            }
                            transformPortraitDirect(
                                s_tempInBgra.data(), inW, inH,
                                canvasBgra.data(), canvasW, canvasH,
                                g_mirrorEnabled.load(), g_isFrontCamera.load(), g_flip180.load(),
                                g_aspectRatioMode.load()
                            );
                        }

                        if (g_trollCctv.load()) {
                            applyTrollEffects(canvasBgra.data(), canvasW, canvasH, true);
                        }

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

                            vcamWriter.writeFrameNV12(nv12Buffer.data());
                            fpsCounter++;
                            hasPrevNv12 = true;
                        }

                        if (++previewSkipCounter % 20 == 0) {
                            g_httpServer.updatePreviewFrame(reinterpret_cast<const uint8_t*>(canvasBgra.data()), canvasW, canvasH);
                        }
                    }

                    auto tEndPipeline = std::chrono::high_resolution_clock::now();
                    float decodeMs = std::chrono::duration<float, std::milli>(tStartRender - tStartPipeline).count();
                    float renderMs = std::chrono::duration<float, std::milli>(tEndPipeline - tStartRender).count();

                    uint64_t seq = ++totalFrameCount;
                    if (seq % 5 == 0 || frameIntervalMs > 40.0f) {
                        const char* flag = (frameIntervalMs > 45.0f) ? " [WARN: ЗАДЕРЖКА!]" : "";
                        const char* tag = isLandscape ? (usedD3D11 ? "[D3D11-GPU]" : "[FX-CPU]")
                                                      : (usedD3D11 ? "[D3D11-PORTRAIT]" : "[PORTRAIT-CPU]");
                        printf("%s Кадр #%-6llu | Δt (между кадрами): %5.1f ms | Декод: %4.2f ms | Рендер: %4.2f ms | Буфер: %4d KB%s\n",
                            tag, seq, frameIntervalMs, decodeMs, renderMs, pending / 1024, flag);
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
                        int stallSkips = D3D11OpticsPipeline::instance().getStallSkipCount();
                        D3D11OpticsPipeline::instance().resetStallSkipCount();
                        printf("→→→ [STATS 1s %s | D3D11: %s%s] FPS: %4.1f | Средний Δt: %5.2f ms (Мин: %5.2f, Макс: %5.2f) | Декод: %4.2f ms | Рендер: %4.2f ms\n",
                            isLandscape ? "FX" : "PORTRAIT", (usedD3D11 ? "ON" : "OFF"),
                            (stallSkips > 0 ? (std::string(" | GPU-Stall skip: ") + std::to_string(stallSkips)).c_str() : ""),
                            currentFps, avgInterval, (minIntervalMs < 9000 ? minIntervalMs : 0.0f), maxIntervalMs, decodeMs, renderMs);
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
            {
                std::lock_guard<std::mutex> lock(queueMutex);
                releaseBuffer(std::move(currentPacket.buffer));
            }
        }
        } catch (const std::exception& ex) {
            printf("[STREAM] Исключение в обработке кадров: %s\n", ex.what());
        } catch (...) {
            printf("[STREAM] Неизвестное исключение в обработке кадров\n");
        }

        ingestRunning.store(false);
        queueCv.notify_all();
        receiver.stop();
        if (ingestThread.joinable()) {
            ingestThread.join();
        }

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
    initCrashHandler();

    // Очистка временных файлов предыдущего обновления (Hot-Swap)
    {
        wchar_t exePathBuf[MAX_PATH];
        if (GetModuleFileNameW(nullptr, exePathBuf, MAX_PATH)) {
            std::filesystem::path appDir = std::filesystem::path(exePathBuf).parent_path();
            std::error_code ec;
            std::filesystem::remove(appDir / "VirtualCamNative.exe.old", ec);
            std::filesystem::remove(appDir / "NativeMFVirtualCam.dll.old", ec);
            std::filesystem::remove(appDir / "apply_update.bat", ec);
            std::filesystem::remove_all(appDir / "_update", ec);
        }
    }

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