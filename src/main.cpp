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
#include "debug_logger.hpp"

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

bool relaunchAsAdmin(const std::wstring& extraArg = L"--register") {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.lpVerb = L"runas";
    sei.lpFile = exePath;
    sei.lpParameters = extraArg.c_str();
    sei.nShow = SW_NORMAL;
    sei.fMask = SEE_MASK_FLAG_NO_UI;

    return ShellExecuteExW(&sei) == TRUE;
}

std::filesystem::path getExpectedVirtualCamDllPath() {
    wchar_t exePath[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH)) return {};
    return std::filesystem::path(exePath).parent_path() / L"VirtualCam.dll";
}

bool isComDllRegisteredToPath(const std::wstring& categoryInstanceSubkey, const std::wstring& clsidStr, const std::filesystem::path& expectedDllPath) {
    HKEY hCatKey = nullptr;
    if (RegOpenKeyExW(HKEY_CLASSES_ROOT, categoryInstanceSubkey.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &hCatKey) != ERROR_SUCCESS) {
        return false;
    }
    RegCloseKey(hCatKey);

    std::wstring inprocKey = L"SOFTWARE\\Classes\\CLSID\\" + clsidStr + L"\\InprocServer32";
    HKEY hInproc = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, inprocKey.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &hInproc) != ERROR_SUCCESS) {
        return false;
    }

    wchar_t registeredPath[MAX_PATH * 2] = {};
    DWORD type = 0;
    DWORD cbData = sizeof(registeredPath);
    LSTATUS st = RegQueryValueExW(hInproc, nullptr, nullptr, &type, reinterpret_cast<BYTE*>(registeredPath), &cbData);
    RegCloseKey(hInproc);

    if (st != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || registeredPath[0] == L'\0') {
        return false;
    }

    std::error_code ec;
    std::filesystem::path regPath(registeredPath);
    if (std::filesystem::exists(regPath, ec) && std::filesystem::exists(expectedDllPath, ec)) {
        if (std::filesystem::equivalent(regPath, expectedDllPath, ec)) {
            return true;
        }
    }
    return _wcsicmp(regPath.lexically_normal().c_str(), expectedDllPath.lexically_normal().c_str()) == 0;
}

bool isCameraRegistered() {
    std::filesystem::path expectedDll = getExpectedVirtualCamDllPath();
    if (expectedDll.empty()) return false;
    std::wstring catSubkey = std::wstring(L"CLSID\\{860BB310-5D01-11D0-BD3B-00A0C911CE86}\\Instance\\") + SZ_CLSID_NativeVirtualCam;
    return isComDllRegisteredToPath(catSubkey, SZ_CLSID_NativeVirtualCam, expectedDll);
}

bool isMicRegistered() {
    std::filesystem::path expectedDll = getExpectedVirtualCamDllPath();
    if (expectedDll.empty()) return false;
    std::wstring catSubkey = std::wstring(L"CLSID\\{33D9A762-90C8-11d0-BD43-00A0C911CE86}\\Instance\\") + SZ_CLSID_VirtualCamNativeMic;
    return isComDllRegisteredToPath(catSubkey, SZ_CLSID_VirtualCamNativeMic, expectedDll);
}

bool registerVirtualCamDll() {
    std::filesystem::path dllPath = getExpectedVirtualCamDllPath();
    if (dllPath.empty() || !std::filesystem::exists(dllPath)) return false;

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
    std::wstring clsidStr = SZ_CLSID_VirtualCamNativeMic;
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
    std::filesystem::path dllPath = getExpectedVirtualCamDllPath();
    if (dllPath.empty()) return false;

    bool okCam = registerVirtualCamDll();
    bool okMic = registerVirtualMicDevice(dllPath.wstring());

    // VB-Cable установка отключена во избежание конфликтов со сторонними DSP-студиями (Equalizer FD CRAFT)
    return okCam && okMic;
}

std::wstring findAdbExecutable() {
    wchar_t localAppData[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localAppData))) {
        std::filesystem::path sdkPath = std::filesystem::path(localAppData) / L"Android" / L"Sdk" / L"platform-tools" / L"adb.exe";
        if (std::filesystem::exists(sdkPath)) return L"\"" + sdkPath.wstring() + L"\"";
    }

    wchar_t profile[MAX_PATH];
    if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH) > 0) {
        std::filesystem::path sdkPath = std::filesystem::path(profile) / L"AppData" / L"Local" / L"Android" / L"Sdk" / L"platform-tools" / L"adb.exe";
        if (std::filesystem::exists(sdkPath)) return L"\"" + sdkPath.wstring() + L"\"";
    }

    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::filesystem::path appDir = std::filesystem::path(exePath).parent_path();
    std::filesystem::path localAdb = appDir / L"adb" / L"adb.exe";
    if (std::filesystem::exists(localAdb)) return L"\"" + localAdb.wstring() + L"\"";
    localAdb = appDir / L"adb.exe";
    if (std::filesystem::exists(localAdb)) return L"\"" + localAdb.wstring() + L"\"";

    return L"adb";
}

static std::atomic<uint64_t> g_lastAdbForwardTick{ 0 };

bool sendAdbSocketCommand(const std::string& cmd) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;

    DWORD timeout = 250;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(5037);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s);
        return false;
    }

    char hexLen[5];
    snprintf(hexLen, sizeof(hexLen), "%04x", static_cast<unsigned int>(cmd.length()));
    std::string packet = std::string(hexLen) + cmd;
    send(s, packet.c_str(), static_cast<int>(packet.length()), 0);

    char reply[5] = { 0 };
    int r = recv(s, reply, 4, 0);
    closesocket(s);
    return (r == 4 && memcmp(reply, "OKAY", 4) == 0);
}

bool setupAdbForwards() {
    uint64_t now = GetTickCount64();
    if (now - g_lastAdbForwardTick.load() < 2500) {
        return true;
    }

    // 1. Мгновенный сокет-проброс через локальный ADB демон (127.0.0.1:5037)
    bool ok1 = sendAdbSocketCommand("host:forward:tcp:8554;tcp:8554");
    bool ok2 = sendAdbSocketCommand("host:forward:tcp:8080;tcp:8080");
    sendAdbSocketCommand("host:reverse:forward:tcp:8000;tcp:8000");

    if (ok1 && ok2) {
        g_lastAdbForwardTick.store(now);
        logDebug("[ADB] Сверхбыстрый сокет-проброс портов выполнен (<5 мс)");
        return true;
    }

    // 2. Если демон ADB еще не запущен, однократно поднимаем его в фоне
    std::wstring adb = findAdbExecutable();
    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    std::wstring startCmd = adb + L" start-server";
    std::vector<wchar_t> cmdVec(startCmd.begin(), startCmd.end());
    cmdVec.push_back(L'\0');

    if (CreateProcessW(nullptr, cmdVec.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 800);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }

    ok1 = sendAdbSocketCommand("host:forward:tcp:8554;tcp:8554");
    ok2 = sendAdbSocketCommand("host:forward:tcp:8080;tcp:8080");
    sendAdbSocketCommand("host:reverse:forward:tcp:8000;tcp:8000");
    if (ok1 && ok2) {
        g_lastAdbForwardTick.store(now);
        return true;
    }

    // 3. Фоллбэк через adb.exe forward / reverse
    auto runHiddenCmd = [](const std::wstring& cmdLine) {
        STARTUPINFOW s{};
        PROCESS_INFORMATION p{};
        s.cb = sizeof(s);
        s.dwFlags = STARTF_USESHOWWINDOW;
        s.wShowWindow = SW_HIDE;

        std::vector<wchar_t> vec(cmdLine.begin(), cmdLine.end());
        vec.push_back(L'\0');

        if (CreateProcessW(nullptr, vec.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &s, &p)) {
            WaitForSingleObject(p.hProcess, 1000);
            CloseHandle(p.hProcess);
            CloseHandle(p.hThread);
        }
    };

    runHiddenCmd(adb + L" forward tcp:8554 tcp:8554");
    runHiddenCmd(adb + L" forward tcp:8080 tcp:8080");
    runHiddenCmd(adb + L" reverse tcp:8000 tcp:8000");
    g_lastAdbForwardTick.store(now);
    return true;
}

void startUsbAutoConnectWorker() {
    std::thread([]() {
        Sleep(100);
        setupAdbForwards();
    }).detach();
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
    int aspectRatioMode = 0,
    int streamOrientation = 90)
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

    bool invertY = (streamOrientation == 270) ? !isFront : isFront;
    if (flip180) invertY = !invertY;

    bool invertX = (streamOrientation == 270)
        ? (isFront ? !mirror : mirror)
        : (isFront ? mirror : !mirror);
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
#if defined(__AVX2__) || defined(_M_X64)
        for (; x + 32 <= halfW; x += 32) {
            __m256i u256 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rowU + x));
            __m256i v256 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(rowV + x));

            __m256i uvLo = _mm256_unpacklo_epi8(u256, v256);
            __m256i uvHi = _mm256_unpackhi_epi8(u256, v256);

            __m256i lane0 = _mm256_permute2x128_si256(uvLo, uvHi, 0x20);
            __m256i lane1 = _mm256_permute2x128_si256(uvLo, uvHi, 0x31);

            _mm256_storeu_si256(reinterpret_cast<__m256i*>(rowUV + x * 2), lane0);
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(rowUV + x * 2 + 32), lane1);
        }
#endif
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
    HRESULT hrCoWorker = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool didCoInitWorker = (hrCoWorker == S_OK || hrCoWorker == S_FALSE);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    timeBeginPeriod(1);

    int canvasW = (g_currentWidth.load() >= 640 && g_currentWidth.load() <= 3840) ? g_currentWidth.load() : 1280;
    int canvasH = (g_currentHeight.load() >= 360 && g_currentHeight.load() <= 2160) ? g_currentHeight.load() : 720;
    size_t nv12Size = (size_t)canvasW * canvasH * 3 / 2;

    MfVirtualCamWriter vcamWriter;
    vcamWriter.start(canvasW, canvasH, g_currentFps.load());

    D3D11OpticsPipeline::instance().init(canvasW, canvasH);

    NvdecDecoder decoder;
    decoder.init(
        canvasW, canvasH,
        D3D11OpticsPipeline::instance().getDevice(),
        D3D11OpticsPipeline::instance().getContext(),
        &D3D11OpticsPipeline::instance().getDeviceMutex()
    );
    logDebug(std::string("[STREAM] Decoder backend: ") + decoder.getHwAccelName());

    TcpReceiver receiver;
    AudioReceiver& audioReceiver = AudioReceiver::instance();
    audioReceiver.setNoiseGate(g_noiseGateEnabled.load(), g_noiseGateThreshold.load());
    audioReceiver.setAiNoise(g_aiNoiseEnabled.load());
    audioReceiver.setAgc(g_agcEnabled.load());
    audioReceiver.setDeclicker(g_declickerEnabled.load());
    audioReceiver.setEq(g_eqLowDb.load(), g_eqMidDb.load(), g_eqHighDb.load());
    audioReceiver.start();

    std::vector<uint32_t> canvasBgra((size_t)canvasW * canvasH, 0xFF000000);
    std::vector<uint8_t> nv12Buffer(nv12Size);
    std::vector<uint8_t> synthInterpNv12(nv12Size);
    std::vector<uint8_t> prevNv12Buffer(nv12Size);
    std::atomic<bool> hasPrevNv12{ false };
    float smoothIntervalMs = 33.3f;
    std::atomic<int> fpsCounter{ 0 };
    std::mutex emitMutex;
    auto lastFpsTime = std::chrono::steady_clock::now();
    auto lastTrollFrameTime = std::chrono::steady_clock::now();

    auto emitNv12Frame = [&](const uint8_t* frameData, bool allowPreview = true, size_t expectedSize = 0) {
        std::lock_guard<std::mutex> lock(emitMutex);
        if (expectedSize != 0 && expectedSize != nv12Size) return;
        // Zero-Latency DirectShow VCam emitter: мгновенная передача кадра в shared memory (< 0.1 ms)
        vcamWriter.writeFrameNV12(frameData);
        fpsCounter.fetch_add(1, std::memory_order_relaxed);

        memcpy(prevNv12Buffer.data(), frameData, nv12Size);
        hasPrevNv12 = true;

        if (allowPreview && g_httpServer.hasPreviewSubscribers()) {
            g_httpServer.updatePreviewFrameNv12(frameData, canvasW, canvasH);
        }
    };

    SwsContext* swsDirectToNv12 = nullptr;
    SwsContext* swsLandscape = nullptr;
    SwsContext* swsCanvasToNv12 = nullptr;

    int prevInW = 0;
    int prevInH = 0;
    auto fillBlackNv12 = [&](std::vector<uint8_t>& buf) {
        size_t yBytes = (size_t)canvasW * canvasH;
        if (buf.size() < yBytes + yBytes / 2) buf.resize(yBytes + yBytes / 2);
        memset(buf.data(), 16, yBytes);
        memset(buf.data() + yBytes, 128, yBytes / 2);
    };
    fillBlackNv12(nv12Buffer);
    fillBlackNv12(prevNv12Buffer);

    auto applyCanvasResize = [&](int newW, int newH, std::mutex* pacerMutexPtr = nullptr,
                                 std::vector<uint8_t>* pacerPendingPtr = nullptr,
                                 std::vector<uint8_t>* pacerScratchPtr = nullptr,
                                 bool* hasPacerPendingPtr = nullptr) {
        if (newW < 640 || newH < 360 || newW > 3840 || newH > 2160) return;
        std::lock_guard<std::mutex> emitLock(emitMutex);
        std::unique_ptr<std::lock_guard<std::mutex>> pacerLock;
        if (pacerMutexPtr) {
            pacerLock = std::make_unique<std::lock_guard<std::mutex>>(*pacerMutexPtr);
        }
        if (newW == canvasW && newH == canvasH) return;

        canvasW = newW;
        canvasH = newH;
        nv12Size = (size_t)canvasW * canvasH * 3 / 2;

        if (swsDirectToNv12) { sws_freeContext(swsDirectToNv12); swsDirectToNv12 = nullptr; }
        if (swsLandscape) { sws_freeContext(swsLandscape); swsLandscape = nullptr; }
        if (swsCanvasToNv12) { sws_freeContext(swsCanvasToNv12); swsCanvasToNv12 = nullptr; }

        canvasBgra.assign((size_t)canvasW * canvasH, 0xFF000000);
        nv12Buffer.resize(nv12Size);
        synthInterpNv12.resize(nv12Size);
        prevNv12Buffer.resize(nv12Size);
        fillBlackNv12(nv12Buffer);
        fillBlackNv12(prevNv12Buffer);
        hasPrevNv12 = false;
        prevInW = 0;
        prevInH = 0;

        if (hasPacerPendingPtr) *hasPacerPendingPtr = false;
        if (pacerPendingPtr) pacerPendingPtr->resize(nv12Size);
        if (pacerScratchPtr) pacerScratchPtr->resize(nv12Size);

        D3D11OpticsPipeline::instance().resize(canvasW, canvasH);
        vcamWriter.setResolution(canvasW, canvasH);
        logDebug("[STREAM] Dynamic canvas resolution set to " + std::to_string(canvasW) + "x" + std::to_string(canvasH));
    };

    int previewSkipCounter = 0;
    bool lastLandscapeMode = g_isLandscapeMode.load();
    int lastAspectMode = g_aspectRatioMode.load();

    while (g_isAppRunning) {
        if (g_resolutionChanged.exchange(false)) {
            applyCanvasResize(g_currentWidth.load(), g_currentHeight.load());
        }
        if (!g_connectRequested && !g_isStreamActive) {
            Sleep(20);
            continue;
        }

        std::string ip;
        int port = 8554;
        std::string mode;
        {
            std::lock_guard<std::mutex> lock(g_app.targetMutex);
            ip = g_targetIp;
            port = g_targetPort;
            mode = g_targetMode;
        }

        if (mode == "usb") {
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
        D3D11OpticsPipeline::instance().resetMotionHistory();
        hasPrevNv12 = false;
        smoothIntervalMs = 33.3f;
        bool motionSynthActive = false;
        int synthWarmupFrames = 0;
        bool wasPrivacyShield = false;

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

        printf("[STREAM] Соединение установлено: %s:%d (режим: %s)\n", ip.c_str(), port, mode.c_str());
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
            bool isKeyframe = false;
            bool isConfig = false;
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
            int consecutiveTimeouts = 0;

            while (ingestRunning.load() && g_isAppRunning && g_isStreamActive) {
                bool isKey = false;
                bool isCfg = false;
                int received = receiver.receiveNalu(tempBuf, nullptr, &isKey, &isCfg);
                if (received < 0) {
                    ingestRunning.store(false);
                    queueCv.notify_all();
                    break;
                }
                if (received == 0) {
                    // Таймаут сокета (4 сек без кадров, аудио и PKT_PING heartbeat)
                    if (++consecutiveTimeouts >= 2) {
                        logDebug("[STREAM] TCP heartbeat/read timeout (8s) -> closing dead socket");
                        receiver.stop();
                        ingestRunning.store(false);
                        queueCv.notify_all();
                        break;
                    }
                    continue;
                }
                consecutiveTimeouts = 0;

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
                    // Жесткий потолок очереди (12 кадров) с сохранением актуального PKT_VIDEO_CONFIG перед IDR
                    if (frameQueue.size() >= 12) {
                        auto lastKeyIt = frameQueue.end();
                        for (auto it = frameQueue.begin(); it != frameQueue.end(); ++it) {
                            if (it->isKeyframe && !it->isConfig) lastKeyIt = it;
                        }
                        if (lastKeyIt != frameQueue.end() && lastKeyIt != frameQueue.begin()) {
                            auto keepFrom = lastKeyIt;
                            while (keepFrom != frameQueue.begin() && std::prev(keepFrom)->isConfig) {
                                --keepFrom;
                            }
                            while (frameQueue.begin() != keepFrom) {
                                releaseBuffer(std::move(frameQueue.front().buffer));
                                frameQueue.pop_front();
                            }
                        } else if (frameQueue.size() >= 12) {
                            releaseBuffer(std::move(frameQueue.front().buffer));
                            frameQueue.pop_front();
                        }
                    }

                    frameQueue.emplace_back();
                    auto& item = frameQueue.back();
                    item.buffer = acquireBuffer();
                    item.buffer.swap(tempBuf);
                    item.size = received;
                    item.frameIntervalMs = interval;
                    item.pending = pendingBytes;
                    item.isKeyframe = isKey;
                    item.isConfig = isCfg;
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

        // Zero-Lookahead 60Hz Frame Pacer (выдаёт отложенный на halfInterval реальный кадр Fn после F(n-0.5))
        std::mutex pacerMutex;
        std::condition_variable pacerCv;
        std::vector<uint8_t> pacerPendingFrame(nv12Size);
        std::vector<uint8_t> pacerEmitScratch(nv12Size);
        bool hasPacerPending = false;
        auto pacerDueTime = std::chrono::steady_clock::now();
        std::atomic<bool> pacerRunning{ true };

        std::thread pacerThread([&]() {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
            std::vector<uint8_t> workerLocal;
            while (pacerRunning.load()) {
                bool fireNow = false;
                size_t frameBytes = 0;
                {
                    std::unique_lock<std::mutex> lock(pacerMutex);
                    if (!hasPacerPending) {
                        pacerCv.wait(lock, [&]() { return !pacerRunning.load() || hasPacerPending; });
                    } else {
                        pacerCv.wait_until(lock, pacerDueTime, [&]() { return !pacerRunning.load() || !hasPacerPending; });
                    }
                    if (!pacerRunning.load()) break;
                    if (hasPacerPending && std::chrono::steady_clock::now() >= pacerDueTime) {
                        fireNow = true;
                        hasPacerPending = false;
                        frameBytes = (std::min)(pacerPendingFrame.size(), nv12Size);
                        workerLocal.assign(pacerPendingFrame.begin(), pacerPendingFrame.begin() + frameBytes);
                        if (pacerPendingFrame.size() < nv12Size) pacerPendingFrame.resize(nv12Size);
                    }
                }
                if (fireNow && frameBytes > 0) {
                    emitNv12Frame(workerLocal.data(), true, frameBytes);
                }
            }
        });

        struct PacerThreadGuard {
            std::atomic<bool>& running;
            std::condition_variable& cv;
            std::thread& th;
            ~PacerThreadGuard() {
                running.store(false);
                cv.notify_all();
                if (th.joinable()) th.join();
            }
        } pacerGuard{ pacerRunning, pacerCv, pacerThread };

        auto flushPendingPacedFrame = [&]() {
            bool needEmit = false;
            size_t frameBytes = 0;
            {
                std::lock_guard<std::mutex> lock(pacerMutex);
                if (hasPacerPending) {
                    hasPacerPending = false;
                    pacerEmitScratch.swap(pacerPendingFrame);
                    frameBytes = (std::min)(pacerEmitScratch.size(), nv12Size);
                    if (pacerPendingFrame.size() < nv12Size) pacerPendingFrame.resize(nv12Size);
                    needEmit = (frameBytes > 0);
                }
            }
            if (needEmit) {
                emitNv12Frame(pacerEmitScratch.data(), false, frameBytes);
            }
        };

        auto clearPendingPacedFrame = [&]() {
            std::lock_guard<std::mutex> lock(pacerMutex);
            hasPacerPending = false;
        };

        auto scheduleDelayedRealFrame = [&](const uint8_t* realNv12, float delayMs) {
            std::lock_guard<std::mutex> lock(pacerMutex);
            if (pacerPendingFrame.size() < nv12Size) pacerPendingFrame.resize(nv12Size);
            memcpy(pacerPendingFrame.data(), realNv12, nv12Size);
            hasPacerPending = true;
            pacerDueTime = std::chrono::steady_clock::now() + std::chrono::microseconds(static_cast<int64_t>(delayMs * 1000.0f));
            pacerCv.notify_one();
        };

        try {
            while (g_isAppRunning && g_isStreamActive) {
            if (g_connectRequested.load()) {
                break;
            }
            if (g_resolutionChanged.exchange(false)) {
                applyCanvasResize(g_currentWidth.load(), g_currentHeight.load(),
                                  &pacerMutex, &pacerPendingFrame, &pacerEmitScratch, &hasPacerPending);
                if (motionSynthActive && canvasW >= 3840) {
                    motionSynthActive = false;
                    D3D11OpticsPipeline::instance().resetMotionHistory();
                }
            }
            if (g_fpsChanged.exchange(false)) {
                int targetFps = g_currentFps.load();
                vcamWriter.setFps(targetFps);
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

            QueuedPacket currentPacket;
            std::vector<QueuedPacket> catchUpPackets;
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                queueCv.wait_for(lock, std::chrono::milliseconds(75), [&]() {
                    return !frameQueue.empty() || !ingestRunning.load() || !g_isStreamActive || !g_isAppRunning || g_connectRequested.load();
                });

                if (g_connectRequested.load() || !g_isStreamActive || !g_isAppRunning) {
                    break;
                }

                if (frameQueue.empty()) {
                    if (g_privacyShield.load()) {
                        wasPrivacyShield = true;
                        clearPendingPacedFrame();
                        memset(nv12Buffer.data(), 16, canvasW * canvasH);
                        memset(nv12Buffer.data() + canvasW * canvasH, 128, canvasW * canvasH / 2);
                        emitNv12Frame(nv12Buffer.data(), true);
                        Sleep(30);
                        continue;
                    }
                    if (!ingestRunning.load()) break;
                    continue;
                }

                if (frameQueue.size() > 1) {
                    auto lastKeyIt = frameQueue.end();
                    for (auto it = frameQueue.begin(); it != frameQueue.end(); ++it) {
                        if (it->isKeyframe && !it->isConfig) lastKeyIt = it;
                    }
                    if (lastKeyIt != frameQueue.end() && lastKeyIt != frameQueue.begin()) {
                        auto keepFrom = lastKeyIt;
                        while (keepFrom != frameQueue.begin() && std::prev(keepFrom)->isConfig) {
                            --keepFrom;
                        }
                        while (frameQueue.begin() != keepFrom) {
                            releaseBuffer(std::move(frameQueue.front().buffer));
                            frameQueue.pop_front();
                        }
                    }
                }

                const size_t drainThreshold = isCurrently4K ? 2 : 8;
                while (frameQueue.size() > drainThreshold) {
                    catchUpPackets.push_back(std::move(frameQueue.front()));
                    frameQueue.pop_front();
                }

                currentPacket = std::move(frameQueue.front());
                frameQueue.pop_front();
            }

            if (!catchUpPackets.empty()) {
                if (g_streamResetRequested.exchange(false)) {
                    logDebug("[STREAM] Seamless camera switch / IDR reset: flushing decoder DPB");
                    decoder.flush();
                    hasPrevNv12 = false;
                    prevInW = 0;
                    prevInH = 0;
                }
                for (auto& pkt : catchUpPackets) {
                    if (pkt.isConfig && decoder.onVideoConfig(pkt.buffer.data(), pkt.size)) {
                        D3D11OpticsPipeline::instance().resetMotionHistory();
                        clearPendingPacedFrame();
                        hasPrevNv12 = false;
                        prevInW = 0;
                        prevInH = 0;
                    }
                    decoder.decodeNaluDirect(pkt.buffer.data(), pkt.size, nullptr, true);
                }
                std::lock_guard<std::mutex> lock(queueMutex);
                for (auto& pkt : catchUpPackets) {
                    releaseBuffer(std::move(pkt.buffer));
                }
            }
            int naluSize = currentPacket.size;
            float frameIntervalMs = currentPacket.frameIntervalMs;
            int pending = currentPacket.pending;
            const uint8_t* pNaluData = currentPacket.buffer.data();

            if (frameIntervalMs > 2.0f && frameIntervalMs < 200.0f) {
                sumIntervalMs += frameIntervalMs;
                if (frameIntervalMs < minIntervalMs) minIntervalMs = frameIntervalMs;
                if (frameIntervalMs > maxIntervalMs) maxIntervalMs = frameIntervalMs;
                intervalSamples++;
                smoothIntervalMs = smoothIntervalMs * 0.9f + frameIntervalMs * 0.1f;
                synthWarmupFrames++;
            } else if (frameIntervalMs > 0.0f) {
                sumIntervalMs += frameIntervalMs;
                if (frameIntervalMs < minIntervalMs) minIntervalMs = frameIntervalMs;
                if (frameIntervalMs > maxIntervalMs) maxIntervalMs = frameIntervalMs;
                intervalSamples++;
            }

            {
                bool want60Fps = (g_currentFps.load() >= 60) && (canvasW < 3840) && !g_trollFpsLimit.load() && !g_privacyShield.load();
                if (!want60Fps) {
                    if (motionSynthActive) {
                        motionSynthActive = false;
                        flushPendingPacedFrame();
                        D3D11OpticsPipeline::instance().resetMotionHistory();
                    }
                } else if (synthWarmupFrames >= 4) {
                    if (!motionSynthActive && smoothIntervalMs >= 25.0f) {
                        motionSynthActive = true;
                        logDebug("[SYNTH 60FPS] Auto-enabled GPU Motion-Compensated Frame Synthesis (avg src dt=" + std::to_string((int)smoothIntervalMs) + "ms)");
                    } else if (motionSynthActive && smoothIntervalMs <= 21.0f) {
                        motionSynthActive = false;
                        flushPendingPacedFrame();
                        D3D11OpticsPipeline::instance().resetMotionHistory();
                        logDebug("[SYNTH 60FPS] Hardware 60 FPS detected (avg src dt=" + std::to_string((int)smoothIntervalMs) + "ms), synthesis disabled");
                    }
                }
            }

            auto tStartPipeline = std::chrono::high_resolution_clock::now();

            if (g_streamResetRequested.exchange(false)) {
                logDebug("[STREAM] Seamless camera switch / IDR reset: flushing decoder DPB");
                decoder.flush();
                D3D11OpticsPipeline::instance().resetMotionHistory();
                clearPendingPacedFrame();
                hasPrevNv12 = false;
                prevInW = 0;
                prevInH = 0;
            }

            if (currentPacket.isConfig && decoder.onVideoConfig(pNaluData, naluSize)) {
                logDebug("[STREAM] New video config (resolution/SPS change) -> decoder reinitialized");
                D3D11OpticsPipeline::instance().resetMotionHistory();
                clearPendingPacedFrame();
                hasPrevNv12 = false;
                prevInW = 0;
                prevInH = 0;
            }

            if (g_privacyShield.load()) {
                wasPrivacyShield = true;
                clearPendingPacedFrame();
                // Декодируем NALU в холостую (decodeOnly=true), чтобы сохранить целостность DPB и P-кадров при выходе из шторки
                decoder.decodeNaluDirect(pNaluData, naluSize, nullptr, true);
                memset(nv12Buffer.data(), 16, canvasW * canvasH);
                memset(nv12Buffer.data() + canvasW * canvasH, 128, canvasW * canvasH / 2);
                emitNv12Frame(nv12Buffer.data(), true);
                {
                    std::lock_guard<std::mutex> lock(queueMutex);
                    releaseBuffer(std::move(currentPacket.buffer));
                }
                continue;
            }
            if (wasPrivacyShield) {
                wasPrivacyShield = false;
                D3D11OpticsPipeline::instance().resetMotionHistory();
            }

            bool hasActiveAspect = (g_aspectRatioMode.load() != 2);
            bool hasActiveTrollFx = g_trollFpsLimit.load() || (g_trollPixelate.load() > 1) ||
                                    g_trollGlitch.load() || g_trollBitcrush.load() || g_trollOverexposure.load() ||
                                    g_trollCctv.load() || g_trollFakeLag.load();
            bool hasActiveOptics = StudioOptics::instance().hasActiveOptics();
            bool isLandscape = g_isLandscapeMode.load();
            int curOrient = g_streamOrientation.load();

            bool hasD3D11 = D3D11OpticsPipeline::instance().isReady() && !g_trollCctv.load();
            // Единый аппаратный GPU-конвейер D3D11 для всех ориентаций: устраняет переключение конвейеров, микро-лаги и фризы
            bool canFastMemcpy = (!hasD3D11 && isLandscape && curOrient == 0 && !g_flip180.load() && !g_mirrorEnabled.load() &&
                                  !hasActiveTrollFx && !hasActiveOptics &&
                                  (g_aspectRatioMode.load() == 2) &&
                                  (prevInW == canvasW && prevInH == canvasH));

            if (canFastMemcpy) {
                // --- FAST-PATH ZERO-COPY: Прямая конвертация в NV12 без промежуточного BGRA (Альбомный режим 16:9) ---
                decoder.decodeNaluDirect(pNaluData, naluSize, [&](const AVFrame* rawFrame, int inW, int inH) {
                    if (!rawFrame || inW <= 0 || inH <= 0) return;

                    const AVFrame* frame = rawFrame;
                    if ((AVPixelFormat)rawFrame->format == AV_PIX_FMT_D3D11) {
                        if (!decoder.transferHwFrameToSw(rawFrame, &frame) || !frame) return;
                    }

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

                    emitNv12Frame(nv12Buffer.data());

                    int curFps = g_currentFps.load();
                    int decimation = (curFps > 35 && smoothIntervalMs < 24.0f) ? 2 : 1;
                    if (g_httpServer.hasPreviewSubscribers() && (++previewSkipCounter % decimation == 0)) {
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
                        float currentFps = fpsCounter.exchange(0, std::memory_order_relaxed) / elapsed.count();
                        float avgInterval = intervalSamples > 0 ? (sumIntervalMs / intervalSamples) : 0.0f;

                        std::string resBadge = std::to_string(prevInW) + "×" + std::to_string(prevInH);
                        g_httpServer.updateTelemetry(currentFps, "", resBadge);

                        printf("------------------------------------------------------------------------------------\n");
                        printf(">>> [STATS 1s] FPS: %4.1f | Средний Δt: %5.2f ms (Мин: %5.2f, Макс: %5.2f) | Рендер: %4.2f ms\n",
                            currentFps, avgInterval, (minIntervalMs < 9000 ? minIntervalMs : 0.0f),
                            maxIntervalMs, pipelineMs);
                        printf("------------------------------------------------------------------------------------\n");

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
                decoder.decodeNaluDirect(pNaluData, naluSize, [&](const AVFrame* rawFrame, int inW, int inH) {
                    if (!rawFrame || inW <= 0 || inH <= 0) return;
                    auto tStartRender = std::chrono::high_resolution_clock::now();

                    isCurrently4K = (inW >= 3840 || inH >= 3840);
                    bool isLandscape = g_isLandscapeMode.load();

                    if (inW != prevInW || inH != prevInH) {
                        prevInW = inW;
                        prevInH = inH;
                        if (swsLandscape) { sws_freeContext(swsLandscape); swsLandscape = nullptr; }
                        if (swsCanvasToNv12) { sws_freeContext(swsCanvasToNv12); swsCanvasToNv12 = nullptr; }
                        std::fill(canvasBgra.begin(), canvasBgra.end(), 0xFF000000);
                        D3D11OpticsPipeline::instance().resetMotionHistory();
                        clearPendingPacedFrame();
                        hasPrevNv12 = false;
                    }

                    int curAspect = g_aspectRatioMode.load();
                    if (lastLandscapeMode != isLandscape || lastAspectMode != curAspect) {
                        std::fill(canvasBgra.begin(), canvasBgra.end(), 0xFF000000);
                        D3D11OpticsPipeline::instance().resetMotionHistory();
                        clearPendingPacedFrame();
                        lastLandscapeMode = isLandscape;
                        lastAspectMode = curAspect;
                    }

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

                    bool usedD3D11 = false;
                    bool didSynth = false;
                    D3D11ShaderParams gpuParams{};
                    if (D3D11OpticsPipeline::instance().isReady() && !g_trollCctv.load()) {
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
                        static float s_animAngleDeg = 90.0f;
                        static float s_animBlend = 1.0f;
                        static float s_animAspectHalfWidth = 0.1582f;
                        static auto s_lastOrientAnimTime = std::chrono::steady_clock::now();
                        static bool s_firstOrientFrame = true;

                        int curOrient = g_streamOrientation.load();
                        float targetAngleDeg = static_cast<float>(curOrient);
                        float targetBlend = (curOrient == 90 || curOrient == 270) ? 1.0f : 0.0f;

                        float targetHalfWidth = 0.5f;
                        if (curOrient == 90 || curOrient == 270) {
                            if (curAspect == 1) {
                                targetHalfWidth = 0.375f;
                            } else if (curAspect == 2) {
                                targetHalfWidth = 0.5f;
                            } else {
                                targetHalfWidth = 0.1582f;
                            }
                        } else {
                            if (curAspect == 1) {
                                targetHalfWidth = 0.375f;
                            } else {
                                targetHalfWidth = 0.5f;
                            }
                        }

                        auto nowAnim = std::chrono::steady_clock::now();
                        float dtAnim = std::chrono::duration<float>(nowAnim - s_lastOrientAnimTime).count();
                        s_lastOrientAnimTime = nowAnim;
                        if (dtAnim > 1.0f) {
                            s_firstOrientFrame = true;
                        }
                        if (s_firstOrientFrame) {
                            s_firstOrientFrame = false;
                            s_animAngleDeg = targetAngleDeg;
                            s_animBlend = targetBlend;
                            s_animAspectHalfWidth = targetHalfWidth;
                        }
                        if (dtAnim <= 0.0f || dtAnim > 0.1f) dtAnim = 0.016f;

                        float smoothFactor = 1.0f - std::exp(-18.0f * dtAnim);

                        float diffAngle = targetAngleDeg - s_animAngleDeg;
                        while (diffAngle > 180.0f) diffAngle -= 360.0f;
                        while (diffAngle < -180.0f) diffAngle += 360.0f;

                        if (std::abs(diffAngle) < 0.2f) {
                            s_animAngleDeg = targetAngleDeg;
                        } else {
                            s_animAngleDeg += diffAngle * smoothFactor;
                        }

                        while (s_animAngleDeg >= 360.0f) s_animAngleDeg -= 360.0f;
                        while (s_animAngleDeg < 0.0f) s_animAngleDeg += 360.0f;

                        float diffBlend = targetBlend - s_animBlend;
                        if (std::abs(diffBlend) < 0.005f) {
                            s_animBlend = targetBlend;
                        } else {
                            s_animBlend += diffBlend * smoothFactor;
                        }

                        float diffWidth = targetHalfWidth - s_animAspectHalfWidth;
                        if (std::abs(diffWidth) < 0.002f) {
                            s_animAspectHalfWidth = targetHalfWidth;
                        } else {
                            s_animAspectHalfWidth += diffWidth * smoothFactor;
                        }

                        float animRad = s_animAngleDeg * (3.14159265f / 180.0f);
                        gpuParams.streamOrientation   = curOrient;
                        gpuParams.animAngleRad        = animRad;
                        gpuParams.animBlend           = s_animBlend;
                        gpuParams.animAspectHalfWidth = s_animAspectHalfWidth;
                        gpuParams.isPortrait        = s_animBlend;
                        gpuParams.portraitMirror    = g_mirrorEnabled.load() ? 1 : 0;
                        gpuParams.portraitFlip180   = g_flip180.load()       ? 1 : 0;
                        gpuParams.portraitIsFront   = g_isFrontCamera.load() ? 1 : 0;
                        gpuParams.aspectRatioMode   = curAspect;

                        // Истинный Zero-Copy путь: декодерная текстура в VRAM -> D3D11OpticsPipeline (без скачивания на CPU)
                        if ((AVPixelFormat)rawFrame->format == AV_PIX_FMT_D3D11) {
                            usedD3D11 = D3D11OpticsPipeline::instance().processD3D11TextureToNv12WithSynth(
                                reinterpret_cast<ID3D11Texture2D*>(rawFrame->data[0]),
                                static_cast<UINT>(reinterpret_cast<intptr_t>(rawFrame->data[1])),
                                inW, inH,
                                nv12Buffer.data(),
                                synthInterpNv12.data(),
                                motionSynthActive,
                                &didSynth,
                                gpuParams
                            );
                        }
                    }

                    const AVFrame* frame = rawFrame;
                    if (!usedD3D11) {
                        if ((AVPixelFormat)rawFrame->format == AV_PIX_FMT_D3D11) {
                            if (!decoder.transferHwFrameToSw(rawFrame, &frame) || !frame) return;
                        }

                        static std::vector<uint8_t> s_rawNv12Buf;
                        const uint8_t* srcYPtr = nullptr;
                        const uint8_t* srcUvPtr = nullptr;
                        int srcStrideY = inW;
                        int srcStrideUv = inW;

                        if ((AVPixelFormat)frame->format == AV_PIX_FMT_NV12) {
                            srcYPtr = frame->data[0];
                            srcStrideY = frame->linesize[0];
                            srcUvPtr = frame->data[1];
                            srcStrideUv = frame->linesize[1];
                        } else if ((AVPixelFormat)frame->format == AV_PIX_FMT_YUV420P) {
                            size_t neededRawNv12Size = (size_t)inW * inH * 3 / 2;
                            if (s_rawNv12Buf.size() != neededRawNv12Size) {
                                s_rawNv12Buf.resize(neededRawNv12Size);
                            }
                            fastYuv420pToNv12(frame, s_rawNv12Buf.data(), inW, inH);
                            srcYPtr = s_rawNv12Buf.data();
                            srcStrideY = inW;
                            srcUvPtr = s_rawNv12Buf.data() + ((size_t)inW * inH);
                            srcStrideUv = inW;
                        } else {
                            size_t neededRawNv12Size = (size_t)inW * inH * 3 / 2;
                            if (s_rawNv12Buf.size() != neededRawNv12Size) {
                                s_rawNv12Buf.resize(neededRawNv12Size);
                            }
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
                                srcYPtr = s_rawNv12Buf.data();
                                srcStrideY = inW;
                                srcUvPtr = s_rawNv12Buf.data() + ((size_t)inW * inH);
                                srcStrideUv = inW;
                            }
                        }
                        if (!srcYPtr || !srcUvPtr) return;

                        if (D3D11OpticsPipeline::instance().isReady() && !g_trollCctv.load()) {
                            usedD3D11 = D3D11OpticsPipeline::instance().processNv12ToNv12WithSynth(
                                srcYPtr, srcStrideY,
                                srcUvPtr, srcStrideUv,
                                inW, inH,
                                nv12Buffer.data(),
                                synthInterpNv12.data(),
                                motionSynthActive,
                                &didSynth,
                                gpuParams
                            );
                        }
                    }

                    if (!usedD3D11 && hasPrevNv12 && D3D11OpticsPipeline::instance().isReady() && !g_trollCctv.load()) {
                        // Zero-Stall: мгновенно сохраняем 60 FPS на предыдущем кадре без CPU-блокировок
                        usedD3D11 = true;
                        didSynth = false;
                    }

                    if (usedD3D11) {
                        if (didSynth) {
                            // Zero-Lookahead Pacing: сразу отдаём F(n-0.5), а через ~halfInterval реальный кадр Fn
                            flushPendingPacedFrame();
                            emitNv12Frame(synthInterpNv12.data(), true);
                            float halfStepMs = (std::clamp)(smoothIntervalMs * 0.48f, 12.0f, 18.0f);
                            scheduleDelayedRealFrame(nv12Buffer.data(), halfStepMs);
                        } else {
                            flushPendingPacedFrame();
                            emitNv12Frame(nv12Buffer.data(), true);
                        }
                    } else {
                        // --- CPU FALLBACK PATH ---
                        if (isLandscape) {
                            int targetW = canvasW;
                            if (g_aspectRatioMode.load() == 1) {
                                targetW = (canvasH * 4) / 3; // 4:3 Classic
                            } else if (g_aspectRatioMode.load() == 0) {
                                targetW = (canvasH * 9) / 16; // 9:16 Phone
                            }
                            if (targetW % 2 != 0) targetW--;
                            int offsetX = (canvasW - targetW) / 2;

                            swsLandscape = sws_getCachedContext(
                                swsLandscape,
                                inW, inH, (AVPixelFormat)frame->format,
                                canvasW, canvasH, AV_PIX_FMT_BGRA,
                                SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
                            );
                            if (swsLandscape) {
                                uint8_t* dstSlice[4] = { reinterpret_cast<uint8_t*>(canvasBgra.data()), nullptr, nullptr, nullptr };
                                int dstStride[4] = { canvasW * 4, 0, 0, 0 };
                                sws_scale(swsLandscape, frame->data, frame->linesize, 0, inH, dstSlice, dstStride);
                            }
                            if (offsetX > 0) {
                                for (int y = 0; y < canvasH; ++y) {
                                    uint32_t* row = canvasBgra.data() + y * canvasW;
                                    memset(row, 0, offsetX * sizeof(uint32_t));
                                    memset(row + offsetX + targetW, 0, (canvasW - offsetX - targetW) * sizeof(uint32_t));
                                }
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
                                inW, inH, (AVPixelFormat)frame->format,
                                inW, inH, AV_PIX_FMT_BGRA,
                                SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
                            );
                            if (swsLandscape) {
                                uint8_t* dstSlice[4] = { reinterpret_cast<uint8_t*>(s_tempInBgra.data()), nullptr, nullptr, nullptr };
                                int dstStride[4] = { inW * 4, 0, 0, 0 };
                                sws_scale(swsLandscape, frame->data, frame->linesize, 0, inH, dstSlice, dstStride);
                            }
                            transformPortraitDirect(
                                s_tempInBgra.data(), inW, inH,
                                canvasBgra.data(), canvasW, canvasH,
                                g_mirrorEnabled.load(), g_isFrontCamera.load(), g_flip180.load(),
                                g_aspectRatioMode.load(),
                                curOrient
                            );
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

                        applyTrollEffects(canvasBgra.data(), canvasW, canvasH, false);

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

                            emitNv12Frame(nv12Buffer.data());
                        }

                        int curFps = g_currentFps.load();
                        int decimation = (curFps > 35 && smoothIntervalMs < 24.0f) ? 2 : 1;
                        if (g_httpServer.hasPreviewSubscribers() && (++previewSkipCounter % decimation == 0)) {
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
                        float currentFps = fpsCounter.exchange(0, std::memory_order_relaxed) / elapsed.count();
                        float avgInterval = intervalSamples > 0 ? (sumIntervalMs / intervalSamples) : 0.0f;

                        std::string resBadge = isLandscape ? (std::to_string(prevInW) + "×" + std::to_string(prevInH))
                                                           : (std::to_string(prevInH) + "×" + std::to_string(prevInW));
                        g_httpServer.updateTelemetry(currentFps, "", resBadge);

                        printf("------------------------------------------------------------------------------------\n");
                        int stallSkips = D3D11OpticsPipeline::instance().getStallSkipCount();
                        D3D11OpticsPipeline::instance().resetStallSkipCount();
                        printf("→→→ [STATS 1s %s | D3D11: %s%s%s] FPS: %4.1f | Средний Δt: %5.2f ms (Мин: %5.2f, Макс: %5.2f) | Декод: %4.2f ms | Рендер: %4.2f ms\n",
                            isLandscape ? "FX" : "PORTRAIT", (usedD3D11 ? "ON" : "OFF"),
                            (motionSynthActive ? " | FG:60" : ""),
                            (stallSkips > 0 ? (std::string(" | GPU-Stall skip: ") + std::to_string(stallSkips)).c_str() : ""),
                            currentFps, avgInterval, (minIntervalMs < 9000 ? minIntervalMs : 0.0f),
                            maxIntervalMs, decodeMs, renderMs);
                        printf("------------------------------------------------------------------------------------\n");

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

        pacerRunning.store(false);
        pacerCv.notify_all();
        ingestRunning.store(false);
        queueCv.notify_all();
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

    if (didCoInitWorker) CoUninitialize();
}

int main(int argc, char* argv[]) {
    // Настройка путей динамической загрузки DLL и утилит (ffmpeg/, adb/)
    wchar_t exePathBuf[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exePathBuf, MAX_PATH)) {
        std::filesystem::path appDir = std::filesystem::path(exePathBuf).parent_path();
        std::filesystem::path ffmpegDir = appDir / "ffmpeg";
        std::filesystem::path adbDir = appDir / "adb";

        if (std::filesystem::exists(ffmpegDir)) {
            SetDllDirectoryW(ffmpegDir.c_str());
            SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
            AddDllDirectory(ffmpegDir.c_str());
        }
        if (std::filesystem::exists(adbDir)) {
            AddDllDirectory(adbDir.c_str());
        }

        std::wstring curPath = _wgetenv(L"PATH") ? _wgetenv(L"PATH") : L"";
        std::wstring newPath = adbDir.wstring() + L";" + ffmpegDir.wstring() + L";" + curPath;
        _wputenv_s(L"PATH", newPath.c_str());
    }

    initCrashHandler();

    // Очистка временных файлов предыдущего обновления (In-Place Atomic Swap + VirtualCam.dll.pending)
    UpdateManager::cleanupPreviousUpdate();

    // Проверка актуальности версии COM-драйвера VirtualCam.dll
    {
        std::filesystem::path expectedDll = getExpectedVirtualCamDllPath();
        std::string drvVer = UpdateManager::readInstalledDriverVersion(expectedDll);
        if (drvVer.empty()) {
            logDebug("[DRIVER WARN] VirtualCam.dll missing or does not export GetVirtualCamDriverVersion");
        } else if (drvVer != VIRTUALCAM_VERSION) {
            logDebug("[DRIVER WARN] Version mismatch: VirtualCam.dll=" + drvVer + ", app=" + VIRTUALCAM_VERSION);
        } else {
            logDebug("[DRIVER] VirtualCam.dll verified v" + drvVer);
        }
    }

    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Ранняя инициализация WinSock
    WSADATA wsaData{};
    WSAStartup(MAKEWORD(2, 2), &wsaData);

    if (argc > 1 && std::string(argv[1]) == "--register") {
        registerAllDrivers();
        if (SUCCEEDED(hrCom)) CoUninitialize();
        return 0;
    }

    // Асинхронная проверка и фоновая регистрация драйверов (без блокировки UI и пауз Sleep)
    std::thread([]() {
        std::filesystem::path expectedDll = getExpectedVirtualCamDllPath();
        if (expectedDll.empty() || !std::filesystem::exists(expectedDll)) {
            return;
        }
        bool needCam = !isCameraRegistered();
        bool needMic = !isMicRegistered();

        if (needCam || needMic) {
            if (isRunAsAdmin()) {
                registerAllDrivers();
            }
            else {
                relaunchAsAdmin(L"--register");
            }
        }
    }).detach();

    // --- Инициализация и загрузка полного конфига (ConfigManager) ---
    ConfigManager::instance().load();
    AppConfig cfg = ConfigManager::instance().get();

    bool initShowConsole = cfg.show_console;
    bool initCloseToTray = cfg.close_to_tray;
    std::wstring targetSkinFile = (cfg.ui_skin == "index3.html") ? L"index3.html" : L"index2.html";

    // --- Инициализация диагностической консоли (только если включена пользователем) ---
    if (initShowConsole) {
        g_app.setConsoleVisible(true);
    } else {
        g_app.setConsoleVisible(false);
    }

    DeviceDiscoveryService::instance().start(8888);

    g_httpServer.start(8000);

    if (cfg.auto_check_updates) {
        UpdateManager::instance().checkForUpdatesAsync(false);
    }

    std::thread streamThread(videoStreamWorker);

    // Фоновая настройка ADB пробросов и реверса портов (без агрессивного захвата)
    startUsbAutoConnectWorker();

    bool startMinimized = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--autostart" || arg == "--minimized" || arg == "--tray") {
            startMinimized = true;
            break;
        }
    }

    GuiWindow gui(GetModuleHandle(nullptr));
    if (gui.create(L"VirtualCam", 1240, 760, startMinimized)) {
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
    WSACleanup();

    if (SUCCEEDED(hrCom)) {
        CoUninitialize();
    }
    return 0;
}