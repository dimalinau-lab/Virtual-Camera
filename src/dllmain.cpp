#include <windows.h>
#include <dshow.h>
#include <strsafe.h>
#include "virtual_camera.hpp"
#include "dshow_cam.hpp"

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
    return CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllCanUnloadNow(void) {
    return S_OK;
}

STDAPI DllUnregisterServer(void) {
    CoInitialize(nullptr);

    // Удаление через DirectShow FilterMapper2
    IFilterMapper2* pFM2 = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER, IID_IFilterMapper2, (void**)&pFM2))) {
        pFM2->UnregisterFilter(&CLSID_VideoInputDeviceCategory, nullptr, CLSID_NativeVirtualCam);
        pFM2->Release();
    }

    // Зачистка устаревших веток реестра
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\CLSID\\{860BB310-5D01-11D0-BD3B-00A0C911CE86}\\Instance\\{E1D3B890-5F16-47D8-9C9D-9F0A3E8B81B1}");
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\CLSID\\{860BB310-5D01-11D0-BD3B-00A0C911CE86}\\Instance\\Native High-Speed Cam");
    RegDeleteTreeW(HKEY_CLASSES_ROOT, L"CLSID\\{860BB310-5D01-11D0-BD3B-00A0C911CE86}\\Instance\\{E1D3B890-5F16-47D8-9C9D-9F0A3E8B81B1}");
    RegDeleteTreeW(HKEY_CLASSES_ROOT, L"CLSID\\{860BB310-5D01-11D0-BD3B-00A0C911CE86}\\Instance\\Native High-Speed Cam");

    RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes\\CLSID\\{E1D3B890-5F16-47D8-9C9D-9F0A3E8B81B1}");
    RegDeleteTreeW(HKEY_CLASSES_ROOT, L"CLSID\\{E1D3B890-5F16-47D8-9C9D-9F0A3E8B81B1}");

    CoUninitialize();
    return S_OK;
}

STDAPI DllRegisterServer(void) {
    wchar_t szModule[MAX_PATH];
    if (!GetModuleFileNameW(g_hModule, szModule, MAX_PATH)) return HRESULT_FROM_WIN32(GetLastError());

    DllUnregisterServer();

    const wchar_t* clsidStr = L"{E1D3B890-5F16-47D8-9C9D-9F0A3E8B81B1}";
    const wchar_t* friendlyName = L"Native High-Speed Cam";

    // 1. Регистрация InprocServer32 в HKEY_CLASSES_ROOT и HKLM
    auto regInproc = [&](HKEY root, const wchar_t* subkey) {
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

    wchar_t clsidKeyHKLM[256];
    StringCchPrintfW(clsidKeyHKLM, 256, L"SOFTWARE\\Classes\\CLSID\\%s", clsidStr);
    regInproc(HKEY_LOCAL_MACHINE, clsidKeyHKLM);

    wchar_t clsidKeyHKCR[256];
    StringCchPrintfW(clsidKeyHKCR, 256, L"CLSID\\%s", clsidStr);
    regInproc(HKEY_CLASSES_ROOT, clsidKeyHKCR);

    // 2. Официальная регистрация через IFilterMapper2 (szInstance = nullptr создаёт правильный ключ с GUID)
    CoInitialize(nullptr);
    IFilterMapper2* pFM2 = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER, IID_IFilterMapper2, (void**)&pFM2);
    if (SUCCEEDED(hr)) {
        REGPINTYPES pinTypes{};
        pinTypes.clsMajorType = &MEDIATYPE_Video;
        pinTypes.clsMinorType = &MEDIASUBTYPE_NULL;

        REGFILTERPINS2 pinReg{};
        pinReg.dwFlags = REG_PINFLAG_B_OUTPUT;
        pinReg.cInstances = 1;
        pinReg.nMediaTypes = 1;
        pinReg.lpMediaType = &pinTypes;
        pinReg.nMediums = 0;
        pinReg.lpMedium = nullptr;
        pinReg.clsPinCategory = &PIN_CATEGORY_CAPTURE;

        REGFILTER2 rf2{};
        rf2.dwVersion = 2;
        rf2.dwMerit = MERIT_DO_NOT_USE + 0x400000;
        rf2.cPins2 = 1;
        rf2.rgPins2 = &pinReg;

        hr = pFM2->RegisterFilter(
            CLSID_NativeVirtualCam,
            friendlyName,
            nullptr,  // szInstance = nullptr создаёт правильный ключ Instance\{GUID}
            &CLSID_VideoInputDeviceCategory,
            nullptr,
            &rf2
        );
        pFM2->Release();
    }
    CoUninitialize();

    return hr;
}