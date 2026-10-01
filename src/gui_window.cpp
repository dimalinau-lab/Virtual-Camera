#include "gui_window.hpp"
#include "config_manager.hpp"
#include <dwmapi.h>
#include <shlobj.h>
#include <shellapi.h>
#include <iostream>
#include <thread>
#include "httplib.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")

static GuiWindow* g_pWindowInstance = nullptr;

extern std::atomic<bool> g_isAppRunning;
extern std::atomic<bool> g_isStreamActive;
extern std::atomic<bool> g_closeToTray;
extern std::atomic<bool> g_showConsole;
extern std::atomic<bool> g_audioMuted;
extern std::atomic<bool> g_privacyShield;
extern std::atomic<bool> g_isFrontCamera;
extern std::atomic<bool> g_flip180;
extern std::atomic<bool> g_trollGlitch;
extern std::atomic<bool> g_trollBitcrush;
extern std::atomic<bool> g_trollOverexposure;
extern std::atomic<int>  g_trollPixelate;
extern std::atomic<bool> g_trollFpsLimit;
extern std::string g_targetMode;
extern std::string g_targetIp;

GuiWindow::GuiWindow(HINSTANCE hInstance) : m_hInstance(hInstance) {
    g_pWindowInstance = this;
}

GuiWindow::~GuiWindow() {
    close();
}

void GuiWindow::initTrayIcon(HICON hIcon) {
    if (m_trayAdded || !m_hWnd) return;

    ZeroMemory(&m_nid, sizeof(m_nid));
    m_nid.cbSize = sizeof(NOTIFYICONDATAW);
    m_nid.hWnd = m_hWnd;
    m_nid.uID = 1;
    m_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    m_nid.uCallbackMessage = WM_APP_TRAY;
    m_nid.hIcon = hIcon ? hIcon : LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    StringCchCopyW(m_nid.szTip, ARRAYSIZE(m_nid.szTip), L"VirtualCamNative (Активен)");

    Shell_NotifyIconW(NIM_ADD, &m_nid);
    m_trayAdded = true;
}

void GuiWindow::removeTrayIcon() {
    if (m_trayAdded && m_hWnd) {
        Shell_NotifyIconW(NIM_DELETE, &m_nid);
        m_trayAdded = false;
    }
}

void GuiWindow::registerGlobalHotkeys() {
    if (!m_hWnd) return;

    // Ctrl + Shift + M : Мут/размут микрофона
    RegisterHotKey(m_hWnd, HOTKEY_ID_MIC_MUTE, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'M');

    // Ctrl + Shift + B : Шторка камеры (Privacy Shield / Blackout)
    RegisterHotKey(m_hWnd, HOTKEY_ID_PRIVACY_SHIELD, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'B');

    // Ctrl + Shift + C : Переключение камеры (основная / фронтальная)
    RegisterHotKey(m_hWnd, HOTKEY_ID_SWITCH_CAMERA, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'C');

    // Ctrl + Shift + T : Быстрое переключение Troll FX
    RegisterHotKey(m_hWnd, HOTKEY_ID_TOGGLE_TROLL, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'T');
}

void GuiWindow::unregisterGlobalHotkeys() {
    if (!m_hWnd) return;
    UnregisterHotKey(m_hWnd, HOTKEY_ID_MIC_MUTE);
    UnregisterHotKey(m_hWnd, HOTKEY_ID_PRIVACY_SHIELD);
    UnregisterHotKey(m_hWnd, HOTKEY_ID_SWITCH_CAMERA);
    UnregisterHotKey(m_hWnd, HOTKEY_ID_TOGGLE_TROLL);
}

void GuiWindow::notifyWebviewHotkey(const std::wstring& action) {
    if (m_webview) {
        std::wstring js = L"if (typeof window.onSystemHotkey === 'function') { window.onSystemHotkey('" + action + L"'); }";
        m_webview->ExecuteScript(js.c_str(), nullptr);
    }
}

void GuiWindow::showWindow() {
    if (m_hWnd) {
        ShowWindow(m_hWnd, SW_RESTORE);
        SetForegroundWindow(m_hWnd);
    }
}

void GuiWindow::hideToTray() {
    if (m_hWnd) {
        ShowWindow(m_hWnd, SW_HIDE);
        if (m_trayAdded) {
            m_nid.uFlags |= NIF_INFO;
            StringCchCopyW(m_nid.szInfoTitle, ARRAYSIZE(m_nid.szInfoTitle), L"VirtualCamNative");
            StringCchCopyW(m_nid.szInfo, ARRAYSIZE(m_nid.szInfo), L"Приложение работает в трее. Кликните для открытия.");
            m_nid.dwInfoFlags = NIIF_INFO;
            Shell_NotifyIconW(NIM_MODIFY, &m_nid);
            m_nid.uFlags &= ~NIF_INFO;
        }
    }
}

void GuiWindow::exitApp() {
    g_isAppRunning = false;
    g_isStreamActive = false;
    close();
    PostQuitMessage(0);
}

void GuiWindow::showTrayMenu() {
    if (!m_hWnd) return;

    POINT pt;
    GetCursorPos(&pt);

    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    AppendMenuW(hMenu, MF_STRING, 1, L"Открыть VirtualCamNative");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    bool muted = g_audioMuted.load();
    AppendMenuW(hMenu, MF_STRING, 2, muted ? L"Включить микрофон (Ctrl+Shift+M)" : L"Заглушить микрофон (Ctrl+Shift+M)");

    bool shield = g_privacyShield.load();
    AppendMenuW(hMenu, MF_STRING, 3, shield ? L"Выключить шторку (Ctrl+Shift+B)" : L"Включить шторку камеры (Ctrl+Shift+B)");

    bool front = g_isFrontCamera.load();
    AppendMenuW(hMenu, MF_STRING, 4, front ? L"Камера: Фронтальная -> Переключить (Ctrl+Shift+C)" : L"Камера: Основная -> Переключить (Ctrl+Shift+C)");

    bool flip180 = g_flip180.load();
    UINT flipFlags = MF_STRING | (flip180 ? MF_CHECKED : MF_UNCHECKED);
    AppendMenuW(hMenu, flipFlags, 8, L"Поворот 180° (Вверх ногами)");

    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    bool showCon = g_showConsole.load();
    AppendMenuW(hMenu, MF_STRING, 6, showCon ? L"Скрыть консоль отладки" : L"Показать консоль отладки");

    bool ctt = g_closeToTray.load();
    UINT cttFlags = MF_STRING | (ctt ? MF_CHECKED : MF_UNCHECKED);
    AppendMenuW(hMenu, cttFlags, 7, L"Сворачивать в трей при закрытии");

    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, 5, L"Закрыть приложение");

    SetForegroundWindow(m_hWnd);
    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, m_hWnd, nullptr);
    DestroyMenu(hMenu);

    switch (cmd) {
    case 1:
        showWindow();
        break;
    case 2: {
        bool newMute = !g_audioMuted.load();
        g_audioMuted.store(newMute);
        notifyWebviewHotkey(newMute ? L"mic_muted" : L"mic_unmuted");
        break;
    }
    case 3: {
        bool newShield = !g_privacyShield.load();
        g_privacyShield.store(newShield);
        notifyWebviewHotkey(newShield ? L"privacy_on" : L"privacy_off");
        break;
    }
    case 4: {
        bool newFront = !g_isFrontCamera.load();
        g_isFrontCamera.store(newFront);
        std::thread([]() {
            std::string phoneHost = (g_targetMode == "usb" || g_targetIp.empty()) ? "127.0.0.1" : g_targetIp;
            httplib::Client cli("http://" + phoneHost + ":8080");
            cli.set_connection_timeout(1, 0);
            cli.Post("/api/action", "{\"action\":\"switch_camera\"}", "application/json");
        }).detach();
        notifyWebviewHotkey(newFront ? L"cam_front" : L"cam_back");
        break;
    }
    case 5:
        exitApp();
        break;
    case 6: {
        bool newShow = !g_showConsole.load();
        setConsoleVisible(newShow);
        ConfigManager::instance().save();
        notifyWebviewHotkey(newShow ? L"console_shown" : L"console_hidden");
        break;
    }
    case 7: {
        bool newCtt = !g_closeToTray.load();
        g_closeToTray.store(newCtt);
        ConfigManager::instance().save();
        notifyWebviewHotkey(newCtt ? L"tray_close_on" : L"tray_close_off");
        break;
    }
    case 8: {
        bool newFlip = !g_flip180.load();
        g_flip180.store(newFlip);
        ConfigManager::instance().save();
        notifyWebviewHotkey(newFlip ? L"flip180_on" : L"flip180_off");
        break;
    }
    }
}

bool GuiWindow::create(const std::wstring& title, int width, int height) {
    WNDCLASSEXW wcex = {};
    wcex.cbSize = sizeof(WNDCLASSEXW);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = GuiWindow::WndProc;
    wcex.hInstance = m_hInstance;
    wcex.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wcex.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wcex.lpszClassName = L"VirtualCamNativeWindowClass";

    // Ищем icon.ico рядом с текущим exe файлом приложения
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring iconPath = exePath;
    size_t lastSlash = iconPath.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        iconPath = iconPath.substr(0, lastSlash + 1) + L"icon.ico";
    }

    HICON hIcon = (HICON)LoadImageW(
        nullptr,
        iconPath.c_str(),
        IMAGE_ICON,
        0, 0,
        LR_LOADFROMFILE | LR_DEFAULTSIZE | LR_SHARED
    );

    if (hIcon) {
        wcex.hIcon = hIcon;
        wcex.hIconSm = hIcon;
    }

    RegisterClassExW(&wcex);

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int posX = max(0, (screenW - width) / 2);
    int posY = max(0, (screenH - height) / 2);

    m_hWnd = CreateWindowExW(
        0,
        L"VirtualCamNativeWindowClass",
        L"VirtualCamNative",
        WS_OVERLAPPEDWINDOW,
        posX, posY, width, height,
        nullptr, nullptr, m_hInstance, nullptr
    );

    if (!m_hWnd) return false;

    if (hIcon) {
        SendMessageW(m_hWnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
        SendMessageW(m_hWnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
    }

    initTrayIcon(hIcon);
    registerGlobalHotkeys();

    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(m_hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));

    ShowWindow(m_hWnd, SW_SHOW);
    UpdateWindow(m_hWnd);

    return true;
}

void GuiWindow::navigate(const std::wstring& urlOrPath) {
    m_initialUrl = urlOrPath;

    wchar_t localAppData[MAX_PATH];
    std::wstring userDataFolder = L"";

    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, localAppData))) {
        userDataFolder = std::wstring(localAppData) + L"\\VirtualCamNative";
    }

    CreateCoreWebView2EnvironmentWithOptions(
        nullptr,
        userDataFolder.empty() ? nullptr : userDataFolder.c_str(),
        nullptr,
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [this](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result) || !env) return result;

                env->CreateCoreWebView2Controller(m_hWnd,
                    Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [this](HRESULT res, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(res) || !controller) return res;

                            m_controller = controller;
                            m_controller->get_CoreWebView2(&m_webview);

                            onResize();

                            ComPtr<ICoreWebView2Settings> settings;
                            if (SUCCEEDED(m_webview->get_Settings(&settings))) {
                                settings->put_AreDefaultContextMenusEnabled(FALSE);
                                settings->put_IsStatusBarEnabled(FALSE);
                                settings->put_AreDevToolsEnabled(FALSE);
                            }

                            EventRegistrationToken token;
                            m_webview->add_DocumentTitleChanged(
                                Microsoft::WRL::Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
                                    [this](ICoreWebView2* sender, IUnknown* args) -> HRESULT {
                                        SetWindowTextW(m_hWnd, L"VirtualCamNative");
                                        return S_OK;
                                    }).Get(), &token);

                            if (!m_initialUrl.empty()) {
                                m_webview->Navigate(m_initialUrl.c_str());
                            }

                            SetWindowTextW(m_hWnd, L"VirtualCamNative");
                            return S_OK;
                        }).Get());

                return S_OK;
            }).Get());
}

void GuiWindow::onResize() {
    if (m_controller && m_hWnd) {
        RECT bounds;
        GetClientRect(m_hWnd, &bounds);
        m_controller->put_Bounds(bounds);
    }
}

void GuiWindow::runMessageLoop() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void GuiWindow::close() {
    unregisterGlobalHotkeys();
    removeTrayIcon();

    if (m_controller) {
        m_controller->Close();
        m_controller = nullptr;
    }
    m_webview = nullptr;
    if (m_hWnd) {
        DestroyWindow(m_hWnd);
        m_hWnd = nullptr;
    }
}

LRESULT CALLBACK GuiWindow::WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_SIZE:
        if (g_pWindowInstance) {
            g_pWindowInstance->onResize();
        }
        break;

    case WM_SETTEXT:
        return DefWindowProcW(hWnd, message, wParam, (LPARAM)L"VirtualCamNative");

    case WM_HOTKEY:
        if (g_pWindowInstance) {
            switch (wParam) {
            case HOTKEY_ID_MIC_MUTE: {
                bool newMute = !g_audioMuted.load();
                g_audioMuted.store(newMute);
                g_pWindowInstance->notifyWebviewHotkey(newMute ? L"mic_muted" : L"mic_unmuted");
                break;
            }
            case HOTKEY_ID_PRIVACY_SHIELD: {
                bool newShield = !g_privacyShield.load();
                g_privacyShield.store(newShield);
                g_pWindowInstance->notifyWebviewHotkey(newShield ? L"privacy_on" : L"privacy_off");
                break;
            }
            case HOTKEY_ID_SWITCH_CAMERA: {
                bool newFront = !g_isFrontCamera.load();
                g_isFrontCamera.store(newFront);
                std::thread([]() {
                    std::string phoneHost = (g_targetMode == "usb" || g_targetIp.empty()) ? "127.0.0.1" : g_targetIp;
                    httplib::Client cli("http://" + phoneHost + ":8080");
                    cli.set_connection_timeout(1, 0);
                    cli.Post("/api/action", "{\"action\":\"switch_camera\"}", "application/json");
                }).detach();
                g_pWindowInstance->notifyWebviewHotkey(newFront ? L"cam_front" : L"cam_back");
                break;
            }
            case HOTKEY_ID_TOGGLE_TROLL: {
                bool active = g_trollGlitch.load() || g_trollBitcrush.load() || g_trollOverexposure.load();
                if (active) {
                    g_trollGlitch.store(false);
                    g_trollBitcrush.store(false);
                    g_trollOverexposure.store(false);
                    g_trollPixelate.store(1);
                    g_trollFpsLimit.store(false);
                    g_pWindowInstance->notifyWebviewHotkey(L"troll_off");
                } else {
                    g_trollGlitch.store(true);
                    g_trollOverexposure.store(true);
                    g_pWindowInstance->notifyWebviewHotkey(L"troll_on");
                }
                break;
            }
            }
        }
        return 0;

    case WM_APP_TRAY:
        if (lParam == WM_LBUTTONUP || lParam == WM_LBUTTONDBLCLK) {
            if (g_pWindowInstance) {
                g_pWindowInstance->showWindow();
            }
        }
        else if (lParam == WM_RBUTTONUP) {
            if (g_pWindowInstance) {
                g_pWindowInstance->showTrayMenu();
            }
        }
        return 0;

    case WM_CLOSE:
        if (g_pWindowInstance) {
            if (g_closeToTray.load()) {
                g_pWindowInstance->hideToTray();
            } else {
                g_pWindowInstance->exitApp();
            }
        }
        return 0;

    case WM_DESTROY:
        g_isAppRunning = false;
        g_isStreamActive = false;
        if (g_pWindowInstance) {
            g_pWindowInstance->removeTrayIcon();
            g_pWindowInstance->unregisterGlobalHotkeys();
        }
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(hWnd, message, wParam, lParam);
    }
    return 0;
}