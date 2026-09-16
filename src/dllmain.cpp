#include <windows.h>
#include <dshow.h>
#include <strsafe.h>
#include "virtual_camera.hpp"
#include "dshow_cam.hpp"
#include "virtual_audio.hpp"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "strmiids.lib")

#pragma comment(linker, "/EXPORT:DllGetClassObject,PRIVATE")
#pragma comment(linker, "/EXPORT:DllCanUnloadNow,PRIVATE")
#pragma comment(linker, "/EXPORT:DllRegisterServer,PRIVATE")
#pragma comment(linker, "/EXPORT:DllUnregisterServer,PRIVATE")

HMODULE g_hModule = nullptr;

// CLSID виртуальной камеры: {E1D3B890-5F16-47D8-9C9D-9F0A3E8B81B1}
static const GUID CLSID_NativeVirtualCam =
{ 0xe1d3b890, 0x5f16, 0x47d8, { 0x9c, 0x9d, 0x9f, 0x0a, 0x3e, 0x8b, 0x81, 0xb1 } };

// Замените static const GUID на обычный const GUID:
const GUID CLSID_VirtualCamNativeMic =
{ 0xa1b2c3d4, 0xe5f6, 0x7890, { 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89 } };

// Фабрика классов для видеокамеры
class VirtualCamClassFactory : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_refCount); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG ref = InterlockedDecrement(&m_refCount);
        if (ref == 0) delete this;
        return ref;
    }

    STDMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) override {
        if (pUnkOuter) return CLASS_E_NOAGGREGATION;
        if (!ppv) return E_POINTER;
        *ppv = nullptr;

        if (riid == __uuidof(IMFMediaSource)) {
            auto source = Microsoft::WRL::Make<VirtualCamMediaSource>();
            if (!source) return E_OUTOFMEMORY;
            HRESULT hr = source->Initialize();
            if (FAILED(hr)) return hr;
            return source->QueryInterface(riid, ppv);
        }

        DShowCaptureFilter* pFilter = new (std::nothrow) DShowCaptureFilter();
        if (!pFilter) return E_OUTOFMEMORY;

        HRESULT hr = pFilter->QueryInterface(riid, ppv);
        pFilter->Release();
        return hr;
    }

    STDMETHODIMP LockServer(BOOL fLock) override { return S_OK; }

private:
    long m_refCount = 1;
};

// Фабрика классов для виртуального микрофона
class VirtualMicClassFactory : public IClassFactory {
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_refCount); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG ref = InterlockedDecrement(&m_refCount);
        if (ref == 0) delete this;
        return ref;
    }

    STDMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) override {
        if (pUnkOuter) return CLASS_E_NOAGGREGATION;
        if (!ppv) return E_POINTER;
        *ppv = nullptr;

        VirtualAudioCaptureFilter* pMicFilter = new (std::nothrow) VirtualAudioCaptureFilter();
        if (!pMicFilter) return E_OUTOFMEMORY;

        HRESULT hr = pMicFilter->QueryInterface(riid, ppv);
        pMicFilter->Release();
        return hr;
    }

    STDMETHODIMP LockServer(BOOL fLock) override { return S_OK; }

private:
    long m_refCount = 1;
};

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    if (ul_reason_for_call == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    if (IsEqualCLSID(rclsid, CLSID_NativeVirtualCam) || IsEqualCLSID(rclsid, CLSID_MFVirtualCamSource)) {
        auto factory = new (std::nothrow) VirtualCamClassFactory();
        if (!factory) return E_OUTOFMEMORY;
        HRESULT hr = factory->QueryInterface(riid, ppv);
        factory->Release();
        return hr;
    }

    if (IsEqualCLSID(rclsid, CLSID_VirtualCamNativeMic)) {
        auto factory = new (std::nothrow) VirtualMicClassFactory();
        if (!factory) return E_OUTOFMEMORY;
        HRESULT hr = factory->QueryInterface(riid, ppv);
        factory->Release();
        return hr;
    }

    return CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllCanUnloadNow(void) {
    return S_OK;
}

STDAPI DllUnregisterServer(void) {
    CoInitialize(nullptr);

    IFilterMapper2* pFM2 = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER, IID_IFilterMapper2, (void**)&pFM2))) {
        pFM2->UnregisterFilter(&CLSID_VideoInputDeviceCategory, nullptr, CLSID_NativeVirtualCam);
        pFM2->UnregisterFilter(&CLSID_AudioInputDeviceCategory, nullptr, CLSID_VirtualCamNativeMic);
        pFM2->Release();
    }

    // Зачистка реестра для видеокамеры
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\CLSID\\{860BB310-5D01-11D0-BD3B-00A0C911CE86}\\Instance\\{E1D3B890-5F16-47D8-9C9D-9F0A3E8B81B1}");
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\CLSID\\{860BB310-5D01-11D0-BD3B-00A0C911CE86}\\Instance\\Native High-Speed Cam");
    RegDeleteTreeW(HKEY_CLASSES_ROOT, L"CLSID\\{860BB310-5D01-11D0-BD3B-00A0C911CE86}\\Instance\\{E1D3B890-5F16-47D8-9C9D-9F0A3E8B81B1}");
    RegDeleteTreeW(HKEY_CLASSES_ROOT, L"CLSID\\{860BB310-5D01-11D0-BD3B-00A0C911CE86}\\Instance\\Native High-Speed Cam");
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\CLSID\\{E1D3B890-5F16-47D8-9C9D-9F0A3E8B81B1}");
    RegDeleteTreeW(HKEY_CLASSES_ROOT, L"CLSID\\{E1D3B890-5F16-47D8-9C9D-9F0A3E8B81B1}");

    // Зачистка реестра для микрофона
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\CLSID\\{33D9A762-90C8-11D0-BD43-00A0C911CE86}\\Instance\\{A1B2C3D4-E5F6-7890-ABCD-EF0123456789}");
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\CLSID\\{33D9A762-90C8-11D0-BD43-00A0C911CE86}\\Instance\\VirtualCam Native Microphone");
    RegDeleteTreeW(HKEY_CLASSES_ROOT, L"CLSID\\{33D9A762-90C8-11D0-BD43-00A0C911CE86}\\Instance\\{A1B2C3D4-E5F6-7890-ABCD-EF0123456789}");
    RegDeleteTreeW(HKEY_CLASSES_ROOT, L"CLSID\\{33D9A762-90C8-11D0-BD43-00A0C911CE86}\\Instance\\VirtualCam Native Microphone");
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\CLSID\\{A1B2C3D4-E5F6-7890-ABCD-EF0123456789}");
    RegDeleteTreeW(HKEY_CLASSES_ROOT, L"CLSID\\{A1B2C3D4-E5F6-7890-ABCD-EF0123456789}");

    CoUninitialize();
    return S_OK;
}

STDAPI DllRegisterServer(void) {
    wchar_t szModule[MAX_PATH];
    if (!GetModuleFileNameW(g_hModule, szModule, MAX_PATH)) return HRESULT_FROM_WIN32(GetLastError());

    DllUnregisterServer();

    auto regInproc = [&](HKEY root, const wchar_t* subkey, const wchar_t* friendlyName) {
        HKEY hKey = nullptr;
        if (RegCreateKeyExW(root, subkey, 0, nullptr, 0, KEY_WRITE | KEY_WOW64_64KEY, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
            RegSetValueExW(hKey, nullptr, 0, REG_SZ, (const BYTE*)friendlyName, (DWORD)((wcslen(friendlyName) + 1) * sizeof(wchar_t)));
            HKEY hInproc = nullptr;
            if (RegCreateKeyExW(hKey, L"InprocServer32", 0, nullptr, 0, KEY_WRITE | KEY_WOW64_64KEY, nullptr, &hInproc, nullptr) == ERROR_SUCCESS) {
                RegSetValueExW(hInproc, nullptr, 0, REG_SZ, (const BYTE*)szModule, (DWORD)((wcslen(szModule) + 1) * sizeof(wchar_t)));
                const wchar_t* threading = L"Both";
                RegSetValueExW(hInproc, L"ThreadingModel", 0, REG_SZ, (const BYTE*)threading, (DWORD)((wcslen(threading) + 1) * sizeof(wchar_t)));
                RegCloseKey(hInproc);
            }
            RegCloseKey(hKey);
        }
        };

    // 1. Регистрация видеокамеры
    const wchar_t* camClsidStr = L"{E1D3B890-5F16-47D8-9C9D-9F0A3E8B81B1}";
    const wchar_t* camFriendlyName = L"Native High-Speed Cam";

    wchar_t camKeyHKLM[256];
    StringCchPrintfW(camKeyHKLM, 256, L"SOFTWARE\\Classes\\CLSID\\%s", camClsidStr);
    regInproc(HKEY_LOCAL_MACHINE, camKeyHKLM, camFriendlyName);

    wchar_t camKeyHKCR[256];
    StringCchPrintfW(camKeyHKCR, 256, L"CLSID\\%s", camClsidStr);
    regInproc(HKEY_CLASSES_ROOT, camKeyHKCR, camFriendlyName);

    // 2. Регистрация виртуального микрофона
    const wchar_t* micClsidStr = L"{A1B2C3D4-E5F6-7890-ABCD-EF0123456789}";
    const wchar_t* micFriendlyName = L"VirtualCam Native Microphone";

    wchar_t micKeyHKLM[256];
    StringCchPrintfW(micKeyHKLM, 256, L"SOFTWARE\\Classes\\CLSID\\%s", micClsidStr);
    regInproc(HKEY_LOCAL_MACHINE, micKeyHKLM, micFriendlyName);

    wchar_t micKeyHKCR[256];
    StringCchPrintfW(micKeyHKCR, 256, L"CLSID\\%s", micClsidStr);
    regInproc(HKEY_CLASSES_ROOT, micKeyHKCR, micFriendlyName);

    // 3. Официальная регистрация фильтров через IFilterMapper2
    CoInitialize(nullptr);
    IFilterMapper2* pFM2 = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER, IID_IFilterMapper2, (void**)&pFM2);
    if (SUCCEEDED(hr)) {
        // Регистрация видеофильтра
        REGPINTYPES camPinTypes{};
        camPinTypes.clsMajorType = &MEDIATYPE_Video;
        camPinTypes.clsMinorType = &MEDIASUBTYPE_NULL;

        REGFILTERPINS2 camPinReg{};
        camPinReg.dwFlags = REG_PINFLAG_B_OUTPUT;
        camPinReg.cInstances = 1;
        camPinReg.nMediaTypes = 1;
        camPinReg.lpMediaType = &camPinTypes;
        camPinReg.clsPinCategory = &PIN_CATEGORY_CAPTURE;

        REGFILTER2 camRf2{};
        camRf2.dwVersion = 2;
        camRf2.dwMerit = MERIT_DO_NOT_USE + 0x400000;
        camRf2.cPins2 = 1;
        camRf2.rgPins2 = &camPinReg;

        pFM2->RegisterFilter(
            CLSID_NativeVirtualCam,
            camFriendlyName,
            nullptr,
            &CLSID_VideoInputDeviceCategory,
            nullptr,
            &camRf2
        );

        // Регистрация аудиофильтра (микрофон)
        REGPINTYPES micPinTypes{};
        micPinTypes.clsMajorType = &MEDIATYPE_Audio;
        micPinTypes.clsMinorType = &MEDIASUBTYPE_PCM;

        REGFILTERPINS2 micPinReg{};
        micPinReg.dwFlags = REG_PINFLAG_B_OUTPUT;
        micPinReg.cInstances = 1;
        micPinReg.nMediaTypes = 1;
        micPinReg.lpMediaType = &micPinTypes;
        micPinReg.clsPinCategory = &PIN_CATEGORY_CAPTURE;

        REGFILTER2 micRf2{};
        micRf2.dwVersion = 2;
        micRf2.dwMerit = MERIT_DO_NOT_USE + 0x400000;
        micRf2.cPins2 = 1;
        micRf2.rgPins2 = &micPinReg;

        pFM2->RegisterFilter(
            CLSID_VirtualCamNativeMic,
            micFriendlyName,
            nullptr,
            &CLSID_AudioInputDeviceCategory,
            nullptr,
            &micRf2
        );

        pFM2->Release();
    }
    CoUninitialize();

    return hr;
}  