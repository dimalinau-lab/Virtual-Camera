#pragma once
#include <windows.h>
#include <shellapi.h>
#include <strsafe.h>
#include <string>
#include <functional>
#include <wrl.h>
#include "WebView2.h"

using Microsoft::WRL::ComPtr;

#define WM_APP_TRAY (WM_APP + 101)
#define WM_APP_NAVIGATE_SKIN (WM_APP + 102)

class GuiWindow;
extern GuiWindow* g_pWindowInstance;

// Идентификаторы глобальных хоткеев
#define HOTKEY_ID_MIC_MUTE       101
#define HOTKEY_ID_PRIVACY_SHIELD 102
#define HOTKEY_ID_SWITCH_CAMERA  103
#define HOTKEY_ID_TOGGLE_TROLL   104
#define HOTKEY_ID_CAM_1          105
#define HOTKEY_ID_CAM_2          106
#define HOTKEY_ID_TOGGLE_HUD     107
#define HOTKEY_ID_FREEZE_FRAME   108
#define HOTKEY_ID_TOGGLE_PIP     109
#define HOTKEY_ID_GSM_BURST      110


class GuiWindow {
public:
    GuiWindow(HINSTANCE hInstance);
    ~GuiWindow();

    bool create(const std::wstring& title, int width = 1240, int height = 760);
    void navigate(const std::wstring& urlOrPath);
    void runMessageLoop();
    void close();

    HWND getHwnd() const { return m_hWnd; }

    void notifyWebviewHotkey(const std::wstring& action);
    void showTrayMenu();
    void showWindow();
    void hideToTray();
    void exitApp();

private:
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
    void onResize();
    void initTrayIcon(HICON hIcon);
    void removeTrayIcon();
    void registerGlobalHotkeys();
    void unregisterGlobalHotkeys();

    HINSTANCE m_hInstance;
    HWND m_hWnd = nullptr;
    std::wstring m_initialUrl;

    NOTIFYICONDATAW m_nid{};
    bool m_trayAdded = false;

    ComPtr<ICoreWebView2Controller> m_controller;
    ComPtr<ICoreWebView2> m_webview;
};