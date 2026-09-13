#include "gui_window.hpp"
#include <dwmapi.h>
#include <shlobj.h>
#include <iostream>

#pragma comment(lib, "dwmapi.lib")

static GuiWindow* g_pWindowInstance = nullptr;
extern std::atomic<bool> g_isAppRunning;

GuiWindow::GuiWindow(HINSTANCE hInstance) : m_hInstance(hInstance) {
    g_pWindowInstance = this;
}

GuiWindow::~GuiWindow() {
    close();
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

    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(m_hWnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));

    ShowWindow(m_hWnd, SW_SHOW);
    UpdateWindow(m_hWnd);

    return true;
}

void GuiWindow::navigate(const std::wstring& urlOrPath) {
    m_initialUrl = urlOrPath;

    // Получаем путь к безопасной системной папке AppData/Local текущего пользователя
    wchar_t localAppData[MAX_PATH];
    std::wstring userDataFolder = L"";

    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, localAppData))) {
        userDataFolder = std::wstring(localAppData) + L"\\VirtualCamNative";
    }

    // Передаем userDataFolder вторым параметром, чтобы устранить ошибку доступа к Program Files
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

                            // Отключаем встроенные панели, контекстные меню и DevTools
                            ComPtr<ICoreWebView2Settings> settings;
                            if (SUCCEEDED(m_webview->get_Settings(&settings))) {
                                settings->put_AreDefaultContextMenusEnabled(FALSE);
                                settings->put_IsStatusBarEnabled(FALSE);
                                settings->put_AreDevToolsEnabled(FALSE);
                            }

                            // Принудительно держим заголовок окна
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

    case WM_CLOSE:
        // 1. Сигнализируем всем фоновым потокам завершение работы
        g_isAppRunning = false;

        // 2. Освобождаем WebView2 контроллер
        if (g_pWindowInstance) {
            g_pWindowInstance->close();
        }

        DestroyWindow(hWnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        // 3. Мгновенное закрытие процесса без зависания в диспетчере задач
        ExitProcess(0);
        return 0;

    default:
        return DefWindowProcW(hWnd, message, wParam, lParam);
    }
    return 0;
}