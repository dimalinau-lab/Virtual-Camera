#pragma once
#include <windows.h>
#include <string>
#include <functional>
#include <wrl.h>
#include "WebView2.h"

using Microsoft::WRL::ComPtr;

class GuiWindow {
public:
    GuiWindow(HINSTANCE hInstance);
    ~GuiWindow();

    bool create(const std::wstring& title, int width = 1240, int height = 760);
    void navigate(const std::wstring& urlOrPath);
    void runMessageLoop();
    void close();

    HWND getHwnd() const { return m_hWnd; }

private:
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
    void onResize();

    HINSTANCE m_hInstance;
    HWND m_hWnd = nullptr;
    std::wstring m_initialUrl;

    ComPtr<ICoreWebView2Controller> m_controller;
    ComPtr<ICoreWebView2> m_webview;
};