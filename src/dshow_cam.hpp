#pragma once
#include <windows.h>
#include <dshow.h>
#include <strsafe.h>
#include <atomic>
#include <thread>
#include <algorithm>
#include <chrono>
#include <vector>
#include <emmintrin.h>

#include <ks.h>
#include <ksproxy.h>
#include <ksmedia.h>

#include "mf_shared_mem.hpp"

// Standard DirectShow I420 subtype GUID: {30323449-0000-0010-8000-00AA00389B71}
inline constexpr GUID MEDIASUBTYPE_I420_VCAM =
{ 0x30323449, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };

class DShowPin;

class DShowCaptureFilter : public IBaseFilter, public IAMFilterMiscFlags {
public:
    DShowCaptureFilter();
    virtual ~DShowCaptureFilter();

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_refCount); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG ref = InterlockedDecrement(&m_refCount);
        if (ref == 0) delete this;
        return ref;
    }

    STDMETHODIMP GetClassID(CLSID* pClassID) override {
        if (!pClassID) return E_POINTER;
        *pClassID = CLSID_NativeVirtualCam;
        return S_OK;
    }

    STDMETHODIMP Stop() override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Run(REFERENCE_TIME tStart) override;
    STDMETHODIMP GetState(DWORD dwMilliSecsTimeout, FILTER_STATE* State) override {
        if (!State) return E_POINTER;
        *State = m_state;
        return S_OK;
    }
    STDMETHODIMP SetSyncSource(IReferenceClock* pClock) override {
        if (m_pClock) m_pClock->Release();
        m_pClock = pClock;
        if (m_pClock) m_pClock->AddRef();
        return S_OK;
    }
    STDMETHODIMP GetSyncSource(IReferenceClock** ppClock) override {
        if (!ppClock) return E_POINTER;
        *ppClock = m_pClock;
        if (m_pClock) m_pClock->AddRef();
        return S_OK;
    }

    STDMETHODIMP EnumPins(IEnumPins** ppEnum) override;
    STDMETHODIMP FindPin(LPCWSTR Id, IPin** ppPin) override;
    STDMETHODIMP QueryFilterInfo(FILTER_INFO* pInfo) override {
        if (!pInfo) return E_POINTER;
        pInfo->pGraph = m_pGraph;
        if (m_pGraph) m_pGraph->AddRef();
        StringCchCopyW(pInfo->achName, 128, L"Native High-Speed Cam");
        return S_OK;
    }
    STDMETHODIMP JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR pName) override {
        m_pGraph = pGraph;
        return S_OK;
    }
    STDMETHODIMP QueryVendorInfo(LPWSTR* pVendorInfo) override { return E_NOTIMPL; }

    STDMETHODIMP_(ULONG) GetMiscFlags(void) override {
        return AM_FILTER_MISC_FLAGS_IS_SOURCE;
    }

private:
    long m_refCount = 1;
    FILTER_STATE m_state = State_Stopped;
    IFilterGraph* m_pGraph = nullptr;
    IReferenceClock* m_pClock = nullptr;
    DShowPin* m_pPin = nullptr;

    friend class DShowPin;
};

class DShowPin : public IPin, public IAMStreamConfig, public IQualityControl, public IKsPropertySet {
public:
    DShowPin(DShowCaptureFilter* pFilter);
    virtual ~DShowPin();

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_refPinCount); }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG ref = InterlockedDecrement(&m_refPinCount);
        if (ref == 0) delete this;
        return ref;
    }

    STDMETHODIMP Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP ReceiveConnection(IPin* pConnector, const AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP Disconnect() override;
    STDMETHODIMP ConnectedTo(IPin** pPin) override {
        if (!pPin) return E_POINTER;
        if (!m_pConnectedPin) return VFW_E_NOT_CONNECTED;
        *pPin = m_pConnectedPin;
        m_pConnectedPin->AddRef();
        return S_OK;
    }
    STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP QueryPinInfo(PIN_INFO* pInfo) override {
        if (!pInfo) return E_POINTER;
        pInfo->pFilter = static_cast<IBaseFilter*>(m_pFilter);
        if (pInfo->pFilter) pInfo->pFilter->AddRef();
        pInfo->dir = PINDIR_OUTPUT;
        StringCchCopyW(pInfo->achName, MAX_PIN_NAME, L"Capture");
        return S_OK;
    }
    STDMETHODIMP QueryDirection(PIN_DIRECTION* pPinDir) override {
        if (!pPinDir) return E_POINTER;
        *pPinDir = PINDIR_OUTPUT;
        return S_OK;
    }
    STDMETHODIMP QueryId(LPWSTR* Id) override {
        if (!Id) return E_POINTER;
        *Id = (LPWSTR)CoTaskMemAlloc(sizeof(L"Capture"));
        if (!*Id) return E_OUTOFMEMORY;
        StringCchCopyW(*Id, 8, L"Capture");
        return S_OK;
    }
    STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP EnumMediaTypes(IEnumMediaTypes** ppEnum) override;
    STDMETHODIMP QueryInternalConnections(IPin** apPin, ULONG* nPin) override { return E_NOTIMPL; }
    STDMETHODIMP EndOfStream() override { return S_OK; }
    STDMETHODIMP BeginFlush() override { return S_OK; }
    STDMETHODIMP EndFlush() override { return S_OK; }
    STDMETHODIMP NewSegment(REFERENCE_TIME tStart, REFERENCE_TIME tStop, double dRate) override { return S_OK; }

    STDMETHODIMP SetFormat(AM_MEDIA_TYPE* pmt) override;
    STDMETHODIMP GetFormat(AM_MEDIA_TYPE** ppmt) override;
    STDMETHODIMP GetNumberOfCapabilities(int* piCount, int* piSize) override;
    STDMETHODIMP GetStreamCaps(int iIndex, AM_MEDIA_TYPE** ppmt, BYTE* pSCC) override;

    STDMETHODIMP Notify(IBaseFilter* pSelf, Quality q) override { return S_OK; }
    STDMETHODIMP SetSink(IQualityControl* piqc) override { return S_OK; }

    STDMETHODIMP Set(REFGUID guidPropSet, DWORD dwPropID, LPVOID pInstanceData, DWORD cbInstanceData, LPVOID pPropData, DWORD cbPropData) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP Get(REFGUID guidPropSet, DWORD dwPropID, LPVOID pInstanceData, DWORD cbInstanceData, LPVOID pPropData, DWORD cbPropData, DWORD* pcbReturned) override {
        if (guidPropSet == AMPROPSETID_Pin && dwPropID == AMPROPERTY_PIN_CATEGORY) {
            if (!pPropData || cbPropData < sizeof(GUID)) return E_POINTER;
            *reinterpret_cast<GUID*>(pPropData) = PIN_CATEGORY_CAPTURE;
            if (pcbReturned) *pcbReturned = sizeof(GUID);
            return S_OK;
        }
        return E_PROP_ID_UNSUPPORTED;
    }
    STDMETHODIMP QuerySupported(REFGUID guidPropSet, DWORD dwPropID, DWORD* pTypeSupport) override {
        if (guidPropSet == AMPROPSETID_Pin && dwPropID == AMPROPERTY_PIN_CATEGORY) {
            if (pTypeSupport) *pTypeSupport = KSPROPERTY_SUPPORT_GET;
            return S_OK;
        }
        return E_PROP_ID_UNSUPPORTED;
    }

    void StartStreaming();
    void StopStreaming();
    void FillMediaType(AM_MEDIA_TYPE* pmt, int capIndex);
    void FillMediaTypeSpec(AM_MEDIA_TYPE* pmt, int width, int height, REFERENCE_TIME avgTimePerFrame, int subtypeIndex);

    struct CapSpec {
        int width;
        int height;
        REFERENCE_TIME avgTimePerFrame;
        int subtypeIndex; // 0 = I420, 1 = NV12, 2 = RGB24
    };

    static constexpr CapSpec kCapabilities[] = {
        { 3840, 2160, 333333, 0 }, // I420 4K30 (Chromium/WebRTC when 4K selected)
        { 1920, 1080, 333333, 0 }, // I420 1080p30
        { 1280,  720, 333333, 0 }, // I420 720p30
        {  960,  540, 333333, 0 }, // I420 540p30
        {  640,  360, 333333, 0 }, // I420 360p30
        { 3840, 2160, 333333, 1 }, // NV12 4K30 (OBS / native NV12)
        { 1920, 1080, 333333, 1 }, // NV12 1080p30
        { 1280,  720, 333333, 1 }, // NV12 720p30
        {  960,  540, 333333, 1 }, // NV12 540p30
        {  640,  360, 333333, 1 }, // NV12 360p30
        { 1920, 1080, 333333, 2 }, // RGB24 1080p30 (Legacy fallback)
        { 1280,  720, 333333, 2 }, // RGB24 720p30
        {  640,  360, 333333, 2 }, // RGB24 360p30
    };
    static constexpr int kNumCapabilities = static_cast<int>(sizeof(kCapabilities) / sizeof(kCapabilities[0]));
    static constexpr int kNumMediaTypes = kNumCapabilities;

    void SetCapabilityCeilingForTest(int maxWidth) { m_testMaxWidth = maxWidth; }

    int GetActiveMaxWidth() const {
        if (m_testMaxWidth > 0) return m_testMaxWidth;
        if (m_pSharedBuffer) {
            const auto* hdr = reinterpret_cast<const MFVirtualCamHeader*>(m_pSharedBuffer);
            if (hdr->magic == 0x4D465643 && hdr->width >= 640 && hdr->width <= 3840) {
                return static_cast<int>(hdr->width);
            }
        }
        HANDLE hMap = OpenFileMappingA(FILE_MAP_READ, FALSE, MF_VCAM_MEM_NAME);
        if (hMap) {
            const void* view = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, sizeof(MFVirtualCamHeader));
            int foundWidth = 0;
            if (view) {
                const auto* hdr = reinterpret_cast<const MFVirtualCamHeader*>(view);
                if (hdr->magic == 0x4D465643 && hdr->width >= 640 && hdr->width <= 3840) {
                    foundWidth = static_cast<int>(hdr->width);
                }
                UnmapViewOfFile(view);
            }
            CloseHandle(hMap);
            if (foundWidth > 0) return foundWidth;
        }
        return 1920;
    }

    int GetActiveCapCount() const {
        const int maxW = GetActiveMaxWidth();
        int count = 0;
        for (int i = 0; i < kNumCapabilities; ++i) {
            if (kCapabilities[i].width <= maxW) ++count;
        }
        return count > 0 ? count : 1;
    }

    const CapSpec* GetActiveCap(int activeIndex) const {
        if (activeIndex < 0) return nullptr;
        const int maxW = GetActiveMaxWidth();
        int seen = 0;
        for (int i = 0; i < kNumCapabilities; ++i) {
            if (kCapabilities[i].width <= maxW) {
                if (seen == activeIndex) return &kCapabilities[i];
                ++seen;
            }
        }
        return nullptr;
    }

    static int SubtypeToIndex(const GUID& subtype) {
        if (subtype == MEDIASUBTYPE_I420_VCAM) return 0;
        if (subtype == MEDIASUBTYPE_NV12) return 1;
        if (subtype == MEDIASUBTYPE_RGB24) return 2;
        return 0;
    }

private:
    void StreamingLoop();

    long m_refPinCount = 1;
    DShowCaptureFilter* m_pFilter = nullptr;
    IPin* m_pConnectedPin = nullptr;
    IMemInputPin* m_pInputPin = nullptr;
    IMemAllocator* m_pAllocator = nullptr;

    std::atomic<bool> m_isStreaming{ false };
    std::thread m_streamThread;

    HANDLE m_hSharedMem = nullptr;
    uint8_t* m_pSharedBuffer = nullptr;
    bool m_isWritableView = false;
    bool m_countedReader = false;
    HANDLE m_hFrameEvent = nullptr;

    int m_width = 1280;
    int m_height = 720;
    int m_testMaxWidth = 0;
    bool m_formatExplicitlySet = false;
    GUID m_selectedSubtype = MEDIASUBTYPE_I420_VCAM;
    REFERENCE_TIME m_avgTimePerFrame = 333333;
    LONGLONG m_frameCounter = 0;
};

class DShowEnumPins : public IEnumPins {
public:
    DShowEnumPins(DShowPin* pPin) : m_pPin(pPin) {
        if (m_pPin) m_pPin->AddRef();
    }
    ~DShowEnumPins() {
        if (m_pPin) m_pPin->Release();
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IEnumPins) {
            *ppv = static_cast<IEnumPins*>(this);
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

    STDMETHODIMP Next(ULONG cPins, IPin** ppPins, ULONG* pcFetched) override {
        if (!ppPins) return E_POINTER;
        if (cPins == 0) {
            if (pcFetched) *pcFetched = 0;
            return S_OK;
        }

        ULONG fetched = 0;
        if (m_index == 0 && cPins > 0 && m_pPin) {
            ppPins[0] = static_cast<IPin*>(m_pPin);
            m_pPin->AddRef();
            m_index = 1;
            fetched = 1;
        }

        if (pcFetched) {
            *pcFetched = fetched;
        }
        return (fetched == cPins) ? S_OK : S_FALSE;
    }

    STDMETHODIMP Skip(ULONG cPins) override {
        m_index += cPins;
        return (m_index >= 1) ? S_FALSE : S_OK;
    }

    STDMETHODIMP Reset() override {
        m_index = 0;
        return S_OK;
    }

    STDMETHODIMP Clone(IEnumPins** ppEnum) override {
        if (!ppEnum) return E_POINTER;
        *ppEnum = new (std::nothrow) DShowEnumPins(m_pPin);
        return *ppEnum ? S_OK : E_OUTOFMEMORY;
    }

private:
    long m_refCount = 1;
    DShowPin* m_pPin = nullptr;
    ULONG m_index = 0;
};

class DShowEnumMediaTypes : public IEnumMediaTypes {
public:
    DShowEnumMediaTypes(DShowPin* pPin) : m_pPin(pPin) {
        if (m_pPin) m_pPin->AddRef();
    }
    ~DShowEnumMediaTypes() {
        if (m_pPin) m_pPin->Release();
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IEnumMediaTypes) {
            *ppv = static_cast<IEnumMediaTypes*>(this);
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

    STDMETHODIMP Next(ULONG cMediaTypes, AM_MEDIA_TYPE** ppMediaTypes, ULONG* pcFetched) override;

    STDMETHODIMP Skip(ULONG cMediaTypes) override {
        m_index += cMediaTypes;
        const ULONG capCount = m_pPin ? static_cast<ULONG>(m_pPin->GetActiveCapCount()) : 0;
        return (m_index >= capCount) ? S_FALSE : S_OK;
    }

    STDMETHODIMP Reset() override {
        m_index = 0;
        return S_OK;
    }

    STDMETHODIMP Clone(IEnumMediaTypes** ppEnum) override {
        if (!ppEnum) return E_POINTER;
        *ppEnum = new (std::nothrow) DShowEnumMediaTypes(m_pPin);
        return *ppEnum ? S_OK : E_OUTOFMEMORY;
    }

private:
    long m_refCount = 1;
    DShowPin* m_pPin = nullptr;
    ULONG m_index = 0;
};

inline STDMETHODIMP DShowCaptureFilter::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    if (riid == IID_IUnknown || riid == IID_IBaseFilter || riid == IID_IMediaFilter) {
        *ppv = static_cast<IBaseFilter*>(this);
    }
    else if (riid == IID_IPersist) {
        *ppv = static_cast<IPersist*>(this);
    }
    else if (riid == IID_IAMFilterMiscFlags) {
        *ppv = static_cast<IAMFilterMiscFlags*>(this);
    }
    else {
        return E_NOINTERFACE;
    }

    AddRef();
    return S_OK;
}

inline DShowCaptureFilter::DShowCaptureFilter() {
    g_vcamComRefCount.fetch_add(1, std::memory_order_relaxed);
    m_pPin = new DShowPin(this);
}

inline DShowCaptureFilter::~DShowCaptureFilter() {
    Stop();
    if (m_pPin) {
        m_pPin->Release();
        m_pPin = nullptr;
    }
    if (m_pClock) {
        m_pClock->Release();
        m_pClock = nullptr;
    }
    g_vcamComRefCount.fetch_sub(1, std::memory_order_relaxed);
}

inline STDMETHODIMP DShowCaptureFilter::EnumPins(IEnumPins** ppEnum) {
    if (!ppEnum) return E_POINTER;
    *ppEnum = nullptr;

    if (!m_pPin) return E_UNEXPECTED;

    DShowEnumPins* pEnum = new (std::nothrow) DShowEnumPins(m_pPin);
    if (!pEnum) return E_OUTOFMEMORY;

    *ppEnum = pEnum;
    return S_OK;
}

inline STDMETHODIMP DShowCaptureFilter::FindPin(LPCWSTR Id, IPin** ppPin) {
    if (!ppPin) return E_POINTER;
    *ppPin = nullptr;

    if (!m_pPin) return VFW_E_NOT_FOUND;

    if (Id == nullptr ||
        _wcsicmp(Id, L"Capture") == 0 ||
        _wcsicmp(Id, L"~capture") == 0 ||
        wcscmp(Id, L"0") == 0 ||
        wcscmp(Id, L"1") == 0)
    {
        *ppPin = m_pPin;
        m_pPin->AddRef();
        return S_OK;
    }
    return VFW_E_NOT_FOUND;
}

inline STDMETHODIMP DShowCaptureFilter::Stop() {
    m_state = State_Stopped;
    if (m_pPin) m_pPin->StopStreaming();
    return S_OK;
}

inline STDMETHODIMP DShowCaptureFilter::Pause() {
    m_state = State_Paused;
    if (m_pPin) m_pPin->StartStreaming();
    return S_OK;
}

inline STDMETHODIMP DShowCaptureFilter::Run(REFERENCE_TIME tStart) {
    m_state = State_Running;
    if (m_pPin) m_pPin->StartStreaming();
    return S_OK;
}

inline DShowPin::DShowPin(DShowCaptureFilter* pFilter) : m_pFilter(pFilter) {}

inline DShowPin::~DShowPin() {
    Disconnect();
}

inline STDMETHODIMP DShowPin::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;

    if (riid == IID_IUnknown || riid == IID_IPin) {
        *ppv = static_cast<IPin*>(this);
        AddRef();
        return S_OK;
    }
    if (riid == IID_IAMStreamConfig) {
        *ppv = static_cast<IAMStreamConfig*>(this);
        AddRef();
        return S_OK;
    }
    if (riid == IID_IQualityControl) {
        *ppv = static_cast<IQualityControl*>(this);
        AddRef();
        return S_OK;
    }
    if (riid == IID_IKsPropertySet) {
        *ppv = static_cast<IKsPropertySet*>(this);
        AddRef();
        return S_OK;
    }

    return E_NOINTERFACE;
}

inline void DShowPin::FillMediaTypeSpec(AM_MEDIA_TYPE* pmt, int width, int height, REFERENCE_TIME avgTimePerFrame, int subtypeIndex) {
    ZeroMemory(pmt, sizeof(AM_MEDIA_TYPE));
    pmt->majortype = MEDIATYPE_Video;
    pmt->bFixedSizeSamples = TRUE;
    pmt->bTemporalCompression = FALSE;
    pmt->formattype = FORMAT_VideoInfo;
    pmt->cbFormat = sizeof(VIDEOINFOHEADER);
    pmt->pUnk = nullptr;

    VIDEOINFOHEADER* vih = (VIDEOINFOHEADER*)CoTaskMemAlloc(sizeof(VIDEOINFOHEADER));
    if (!vih) return;
    ZeroMemory(vih, sizeof(VIDEOINFOHEADER));
    vih->rcSource = RECT{ 0, 0, width, height };
    vih->rcTarget = RECT{ 0, 0, width, height };
    vih->AvgTimePerFrame = avgTimePerFrame;
    vih->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    vih->bmiHeader.biWidth = width;
    vih->bmiHeader.biPlanes = 1;

    if (subtypeIndex == 0) {
        pmt->subtype = MEDIASUBTYPE_I420_VCAM;
        pmt->lSampleSize = (ULONG)(width * height * 3 / 2);
        vih->bmiHeader.biHeight = height;
        vih->bmiHeader.biBitCount = 12;
        vih->bmiHeader.biCompression = MAKEFOURCC('I', '4', '2', '0');
        vih->bmiHeader.biSizeImage = pmt->lSampleSize;
    }
    else if (subtypeIndex == 1) {
        pmt->subtype = MEDIASUBTYPE_NV12;
        pmt->lSampleSize = (ULONG)(width * height * 3 / 2);
        vih->bmiHeader.biHeight = height;
        vih->bmiHeader.biBitCount = 12;
        vih->bmiHeader.biCompression = MAKEFOURCC('N', 'V', '1', '2');
        vih->bmiHeader.biSizeImage = pmt->lSampleSize;
    }
    else {
        pmt->subtype = MEDIASUBTYPE_RGB24;
        pmt->lSampleSize = (ULONG)(width * height * 3);
        vih->bmiHeader.biHeight = height;
        vih->bmiHeader.biBitCount = 24;
        vih->bmiHeader.biCompression = BI_RGB;
        vih->bmiHeader.biSizeImage = pmt->lSampleSize;
    }
    pmt->pbFormat = (BYTE*)vih;
}

inline void DShowPin::FillMediaType(AM_MEDIA_TYPE* pmt, int capIndex) {
    const CapSpec* cap = GetActiveCap(capIndex);
    if (!cap) cap = GetActiveCap(0);
    if (!cap) cap = &kCapabilities[0];
    FillMediaTypeSpec(pmt, cap->width, cap->height, cap->avgTimePerFrame, cap->subtypeIndex);
}

inline STDMETHODIMP DShowPin::Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt) {
    if (!pReceivePin) return E_POINTER;
    if (m_pConnectedPin) return VFW_E_ALREADY_CONNECTED;

    AM_MEDIA_TYPE mt{};
    bool allocated = false;
    HRESULT hr = E_FAIL;

    if (pmt) {
        mt = *pmt;
        if (pmt->formattype == FORMAT_VideoInfo && pmt->pbFormat && pmt->cbFormat >= sizeof(VIDEOINFOHEADER)) {
            const VIDEOINFOHEADER* vih = reinterpret_cast<const VIDEOINFOHEADER*>(pmt->pbFormat);
            if (vih->bmiHeader.biWidth > 0 && abs(vih->bmiHeader.biHeight) > 0) {
                m_width = vih->bmiHeader.biWidth;
                m_height = abs(vih->bmiHeader.biHeight);
                m_formatExplicitlySet = true;
            }
            if (vih->AvgTimePerFrame > 0) {
                m_avgTimePerFrame = vih->AvgTimePerFrame;
            }
        }
        hr = pReceivePin->ReceiveConnection(this, &mt);
    }
    else {
        if (!m_formatExplicitlySet) {
            if (const CapSpec* cap0 = GetActiveCap(0)) {
                m_width = cap0->width;
                m_height = cap0->height;
            }
        }
        const int preferredIdx = SubtypeToIndex(m_selectedSubtype);
        FillMediaTypeSpec(&mt, m_width, m_height, m_avgTimePerFrame, preferredIdx);
        allocated = true;
        hr = pReceivePin->ReceiveConnection(this, &mt);
        for (int idx = 0; FAILED(hr) && idx < 3; ++idx) {
            if (idx == preferredIdx) continue;
            if (mt.pbFormat) {
                CoTaskMemFree(mt.pbFormat);
                mt.pbFormat = nullptr;
            }
            FillMediaTypeSpec(&mt, m_width, m_height, m_avgTimePerFrame, idx);
            hr = pReceivePin->ReceiveConnection(this, &mt);
        }
    }

    if (SUCCEEDED(hr)) {
        m_pConnectedPin = pReceivePin;
        m_pConnectedPin->AddRef();
        m_selectedSubtype = mt.subtype;

        hr = m_pConnectedPin->QueryInterface(IID_IMemInputPin, (void**)&m_pInputPin);
        if (SUCCEEDED(hr)) {
            hr = m_pInputPin->GetAllocator(&m_pAllocator);
            if (FAILED(hr) || !m_pAllocator) {
                hr = CoCreateInstance(CLSID_MemoryAllocator, nullptr, CLSCTX_INPROC_SERVER, IID_IMemAllocator, (void**)&m_pAllocator);
            }
            if (SUCCEEDED(hr) && m_pAllocator) {
                ALLOCATOR_PROPERTIES prop{}, actual{};
                prop.cBuffers = 2;
                prop.cbBuffer = (m_selectedSubtype == MEDIASUBTYPE_RGB24) ? (m_width * m_height * 3) : (m_width * m_height * 3 / 2);
                prop.cbAlign = 16;
                prop.cbPrefix = 0;
                m_pAllocator->SetProperties(&prop, &actual);
                m_pInputPin->NotifyAllocator(m_pAllocator, FALSE);
            }
        }
    }

    if (allocated && mt.pbFormat) {
        CoTaskMemFree(mt.pbFormat);
    }
    return hr;
}

inline STDMETHODIMP DShowPin::ReceiveConnection(IPin* pConnector, const AM_MEDIA_TYPE* pmt) {
    if (!pConnector || !pmt) return E_POINTER;
    if (m_pConnectedPin) return VFW_E_ALREADY_CONNECTED;
    if (QueryAccept(pmt) != S_OK) return VFW_E_TYPE_NOT_ACCEPTED;

    if (pmt->formattype == FORMAT_VideoInfo && pmt->pbFormat && pmt->cbFormat >= sizeof(VIDEOINFOHEADER)) {
        const VIDEOINFOHEADER* vih = reinterpret_cast<const VIDEOINFOHEADER*>(pmt->pbFormat);
        if (vih->bmiHeader.biWidth > 0 && abs(vih->bmiHeader.biHeight) > 0) {
            m_width = vih->bmiHeader.biWidth;
            m_height = abs(vih->bmiHeader.biHeight);
        }
        if (vih->AvgTimePerFrame > 0) {
            m_avgTimePerFrame = vih->AvgTimePerFrame;
        }
    }

    m_pConnectedPin = pConnector;
    m_pConnectedPin->AddRef();
    m_selectedSubtype = pmt->subtype;
    return S_OK;
}

inline STDMETHODIMP DShowPin::Disconnect() {
    StopStreaming();
    if (m_pAllocator) {
        m_pAllocator->Decommit();
        m_pAllocator->Release();
        m_pAllocator = nullptr;
    }
    if (m_pInputPin) {
        m_pInputPin->Release();
        m_pInputPin = nullptr;
    }
    if (m_pConnectedPin) {
        m_pConnectedPin->Release();
        m_pConnectedPin = nullptr;
    }
    return S_OK;
}

inline STDMETHODIMP DShowPin::ConnectionMediaType(AM_MEDIA_TYPE* pmt) {
    if (!pmt) return E_POINTER;
    FillMediaTypeSpec(pmt, m_width, m_height, m_avgTimePerFrame, SubtypeToIndex(m_selectedSubtype));
    return S_OK;
}

inline STDMETHODIMP DShowPin::QueryAccept(const AM_MEDIA_TYPE* pmt) {
    if (!pmt) return E_POINTER;
    if (pmt->majortype != MEDIATYPE_Video) return S_FALSE;

    if (pmt->subtype != MEDIASUBTYPE_I420_VCAM &&
        pmt->subtype != MEDIASUBTYPE_NV12 &&
        pmt->subtype != MEDIASUBTYPE_RGB24) {
        return S_FALSE;
    }

    if (pmt->formattype == FORMAT_VideoInfo) {
        if (!pmt->pbFormat || pmt->cbFormat < sizeof(VIDEOINFOHEADER)) return S_FALSE;
        const auto* vih = reinterpret_cast<const VIDEOINFOHEADER*>(pmt->pbFormat);
        int w = vih->bmiHeader.biWidth;
        int h = abs(vih->bmiHeader.biHeight);
        if (w < 2 || h < 2 || (w & 1) || (h & 1) || w > 3840 || h > 2160) {
            return S_FALSE;
        }
    }
    return S_OK;
}

inline STDMETHODIMP DShowPin::EnumMediaTypes(IEnumMediaTypes** ppEnum) {
    if (!ppEnum) return E_POINTER;
    *ppEnum = new (std::nothrow) DShowEnumMediaTypes(this);
    return *ppEnum ? S_OK : E_OUTOFMEMORY;
}

inline STDMETHODIMP DShowEnumMediaTypes::Next(ULONG cMediaTypes, AM_MEDIA_TYPE** ppMediaTypes, ULONG* pcFetched) {
    if (!ppMediaTypes) return E_POINTER;
    if (cMediaTypes == 0) {
        if (pcFetched) *pcFetched = 0;
        return S_OK;
    }

    ULONG count = 0;
    const ULONG capCount = static_cast<ULONG>(m_pPin->GetActiveCapCount());
    while (m_index < capCount && count < cMediaTypes) {
        AM_MEDIA_TYPE* pmt = (AM_MEDIA_TYPE*)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
        if (!pmt) break;
        m_pPin->FillMediaType(pmt, m_index);
        ppMediaTypes[count++] = pmt;
        m_index++;
    }

    if (pcFetched) *pcFetched = count;
    return (count == cMediaTypes) ? S_OK : S_FALSE;
}

inline STDMETHODIMP DShowPin::SetFormat(AM_MEDIA_TYPE* pmt) {
    if (!pmt) return E_POINTER;
    if (QueryAccept(pmt) != S_OK) return VFW_E_TYPE_NOT_ACCEPTED;

    if (pmt->formattype == FORMAT_VideoInfo) {
        if (!pmt->pbFormat || pmt->cbFormat < sizeof(VIDEOINFOHEADER)) return E_INVALIDARG;
        VIDEOINFOHEADER* vih = (VIDEOINFOHEADER*)pmt->pbFormat;
        int w = vih->bmiHeader.biWidth;
        int h = abs(vih->bmiHeader.biHeight);
        if (w <= 0 || h <= 0) return E_INVALIDARG;
        if (m_pConnectedPin && (w != m_width || h != m_height)) {
            return VFW_E_WRONG_STATE;
        }
        m_width = w;
        m_height = h;
        m_formatExplicitlySet = true;
        if (vih->AvgTimePerFrame > 0) {
            m_avgTimePerFrame = vih->AvgTimePerFrame;
        }
    }
    m_selectedSubtype = pmt->subtype;
    return S_OK;
}

inline STDMETHODIMP DShowPin::GetFormat(AM_MEDIA_TYPE** ppmt) {
    if (!ppmt) return E_POINTER;
    int outW = m_width;
    int outH = m_height;
    if (!m_pConnectedPin && !m_formatExplicitlySet) {
        if (const CapSpec* cap0 = GetActiveCap(0)) {
            outW = cap0->width;
            outH = cap0->height;
        }
    }
    AM_MEDIA_TYPE* pmt = (AM_MEDIA_TYPE*)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
    if (!pmt) return E_OUTOFMEMORY;
    FillMediaTypeSpec(pmt, outW, outH, m_avgTimePerFrame, SubtypeToIndex(m_selectedSubtype));
    if (!pmt->pbFormat) { CoTaskMemFree(pmt); return E_OUTOFMEMORY; }
    *ppmt = pmt;
    return S_OK;
}

inline STDMETHODIMP DShowPin::GetNumberOfCapabilities(int* piCount, int* piSize) {
    if (!piCount || !piSize) return E_POINTER;
    *piCount = GetActiveCapCount();
    *piSize = sizeof(VIDEO_STREAM_CONFIG_CAPS);
    return S_OK;
}

inline STDMETHODIMP DShowPin::GetStreamCaps(int iIndex, AM_MEDIA_TYPE** ppmt, BYTE* pSCC) {
    if (!ppmt || !pSCC) return E_POINTER;
    const CapSpec* cap = GetActiveCap(iIndex);
    if (!cap) return S_FALSE;

    AM_MEDIA_TYPE* pmt = (AM_MEDIA_TYPE*)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
    if (!pmt) return E_OUTOFMEMORY;
    FillMediaTypeSpec(pmt, cap->width, cap->height, cap->avgTimePerFrame, cap->subtypeIndex);
    if (!pmt->pbFormat) { CoTaskMemFree(pmt); return E_OUTOFMEMORY; }
    *ppmt = pmt;

    VIDEO_STREAM_CONFIG_CAPS* caps = (VIDEO_STREAM_CONFIG_CAPS*)pSCC;
    ZeroMemory(caps, sizeof(VIDEO_STREAM_CONFIG_CAPS));
    caps->guid = FORMAT_VideoInfo;
    caps->VideoStandard = 0;
    caps->InputSize.cx = cap->width;
    caps->InputSize.cy = cap->height;
    caps->MinCroppingSize.cx = cap->width;
    caps->MinCroppingSize.cy = cap->height;
    caps->MaxCroppingSize.cx = cap->width;
    caps->MaxCroppingSize.cy = cap->height;
    caps->CropGranularityX = 1;
    caps->CropGranularityY = 1;
    caps->CropAlignX = 0;
    caps->CropAlignY = 0;
    caps->MinOutputSize.cx = cap->width;
    caps->MinOutputSize.cy = cap->height;
    caps->MaxOutputSize.cx = cap->width;
    caps->MaxOutputSize.cy = cap->height;
    caps->OutputGranularityX = 1;
    caps->OutputGranularityY = 1;
    caps->MinFrameInterval = 166666;
    caps->MaxFrameInterval = 666666;

    return S_OK;
}

inline void DShowPin::StartStreaming() {
    if (m_isStreaming) return;
    m_isStreaming = true;
    m_frameCounter = 0;
    m_isWritableView = false;
    m_countedReader = false;

    m_hSharedMem = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, MF_VCAM_MEM_NAME);
    if (m_hSharedMem) {
        m_pSharedBuffer = (uint8_t*)MapViewOfFile(m_hSharedMem, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        if (m_pSharedBuffer) {
            m_isWritableView = true;
        } else {
            CloseHandle(m_hSharedMem);
            m_hSharedMem = nullptr;
        }
    }
    if (!m_pSharedBuffer) {
        m_hSharedMem = OpenFileMappingA(FILE_MAP_READ, FALSE, MF_VCAM_MEM_NAME);
        if (m_hSharedMem) {
            m_pSharedBuffer = (uint8_t*)MapViewOfFile(m_hSharedMem, FILE_MAP_READ, 0, 0, 0);
            m_isWritableView = false;
        }
    }
    if (m_pSharedBuffer && m_isWritableView) {
        auto* hdr = reinterpret_cast<MFVirtualCamHeader*>(m_pSharedBuffer);
        InterlockedIncrement(&hdr->activeReaders);
        m_countedReader = true;
    }
    m_hFrameEvent = OpenEventA(SYNCHRONIZE, FALSE, MF_VCAM_EVENT_NAME);

    if (m_pAllocator) {
        m_pAllocator->Commit();
    }

    m_streamThread = std::thread(&DShowPin::StreamingLoop, this);
}

inline void DShowPin::StopStreaming() {
    m_isStreaming = false;
    if (m_streamThread.joinable()) {
        m_streamThread.join();
    }
    if (m_pAllocator) {
        m_pAllocator->Decommit();
    }
    if (m_pSharedBuffer) {
        if (m_isWritableView && m_countedReader) {
            auto* hdr = reinterpret_cast<MFVirtualCamHeader*>(m_pSharedBuffer);
            if (hdr->activeReaders > 0) {
                InterlockedDecrement(&hdr->activeReaders);
            }
        }
        UnmapViewOfFile(m_pSharedBuffer);
        m_pSharedBuffer = nullptr;
    }
    m_isWritableView = false;
    m_countedReader = false;
    if (m_hSharedMem) {
        CloseHandle(m_hSharedMem);
        m_hSharedMem = nullptr;
    }
    if (m_hFrameEvent) {
        CloseHandle(m_hFrameEvent);
        m_hFrameEvent = nullptr;
    }
}

static inline uint8_t clampU8(int v) {
    return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

// Fast SSE2 NV12 -> I420 conversion (zero color-space math: Y memcpy + interleaved UV split)
static inline void ConvertNV12toI420(const uint8_t* yPlane, const uint8_t* uvPlane, uint8_t* i420Dst, int width, int height) {
    const size_t yBytes = static_cast<size_t>(width) * height;
    const size_t uvBytes = yBytes / 2;
    const size_t chromaBytes = yBytes / 4;

    memcpy(i420Dst, yPlane, yBytes);

    uint8_t* uDst = i420Dst + yBytes;
    uint8_t* vDst = uDst + chromaBytes;

    size_t i = 0;
    const __m128i maskLow = _mm_set1_epi16(0x00FF);
    for (; i + 32 <= uvBytes; i += 32) {
        __m128i c0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(uvPlane + i));
        __m128i c1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(uvPlane + i + 16));

        __m128i u0 = _mm_and_si128(c0, maskLow);
        __m128i u1 = _mm_and_si128(c1, maskLow);
        __m128i v0 = _mm_srli_epi16(c0, 8);
        __m128i v1 = _mm_srli_epi16(c1, 8);

        __m128i uPacked = _mm_packus_epi16(u0, u1);
        __m128i vPacked = _mm_packus_epi16(v0, v1);

        _mm_storeu_si128(reinterpret_cast<__m128i*>(uDst + (i >> 1)), uPacked);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(vDst + (i >> 1)), vPacked);
    }

    for (; i + 1 < uvBytes; i += 2) {
        size_t dstIdx = i >> 1;
        uDst[dstIdx] = uvPlane[i];
        vDst[dstIdx] = uvPlane[i + 1];
    }
}

// Fast NV12 scaler: SSE2 2x2 box downscale for exact 2:1 (e.g. 1280x720 -> 640x360), fixed-point bilinear otherwise
static inline void ScaleNV12(const uint8_t* src, int srcW, int srcH, uint8_t* dst, int dstW, int dstH) {
    if (!src || !dst || srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return;

    if (srcW == dstW && srcH == dstH) {
        memcpy(dst, src, static_cast<size_t>(dstW) * dstH * 3 / 2);
        return;
    }

    const uint8_t* srcY = src;
    const uint8_t* srcUV = src + static_cast<size_t>(srcW) * srcH;
    uint8_t* dstY = dst;
    uint8_t* dstUV = dst + static_cast<size_t>(dstW) * dstH;

    if (srcW == dstW * 2 && srcH == dstH * 2) {
        const __m128i maskLow = _mm_set1_epi16(0x00FF);
        const __m128i ones = _mm_set1_epi16(1);

        for (int y = 0; y < dstH; ++y) {
            const uint8_t* r0 = srcY + static_cast<size_t>(y * 2) * srcW;
            const uint8_t* r1 = r0 + srcW;
            uint8_t* out = dstY + static_cast<size_t>(y) * dstW;

            int x = 0;
            for (; x + 16 <= dstW; x += 16) {
                __m128i a0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(r0 + x * 2));
                __m128i b0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(r1 + x * 2));
                __m128i a1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(r0 + x * 2 + 16));
                __m128i b1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(r1 + x * 2 + 16));

                __m128i v0 = _mm_avg_epu8(a0, b0);
                __m128i v1 = _mm_avg_epu8(a1, b1);

                __m128i even0 = _mm_and_si128(v0, maskLow);
                __m128i odd0 = _mm_srli_epi16(v0, 8);
                __m128i avg0 = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(even0, odd0), ones), 1);

                __m128i even1 = _mm_and_si128(v1, maskLow);
                __m128i odd1 = _mm_srli_epi16(v1, 8);
                __m128i avg1 = _mm_srli_epi16(_mm_add_epi16(_mm_add_epi16(even1, odd1), ones), 1);

                _mm_storeu_si128(reinterpret_cast<__m128i*>(out + x), _mm_packus_epi16(avg0, avg1));
            }
            for (; x < dstW; ++x) {
                int sx = x * 2;
                out[x] = static_cast<uint8_t>((r0[sx] + r0[sx + 1] + r1[sx] + r1[sx + 1] + 2) >> 2);
            }
        }

        const int dstUvPairs = dstW / 2;
        const int dstUvH = dstH / 2;
        for (int y = 0; y < dstUvH; ++y) {
            const uint8_t* r0 = srcUV + static_cast<size_t>(y * 2) * srcW;
            const uint8_t* r1 = r0 + srcW;
            uint8_t* out = dstUV + static_cast<size_t>(y) * dstW;
            for (int x = 0; x < dstUvPairs; ++x) {
                int sx = x * 4;
                out[x * 2]     = static_cast<uint8_t>((r0[sx] + r0[sx + 2] + r1[sx] + r1[sx + 2] + 2) >> 2);
                out[x * 2 + 1] = static_cast<uint8_t>((r0[sx + 1] + r0[sx + 3] + r1[sx + 1] + r1[sx + 3] + 2) >> 2);
            }
        }
        return;
    }

    // General fixed-point bilinear NV12 scaler (e.g. 1280x720 -> 960x540)
    const uint32_t stepX = (dstW > 1) ? ((static_cast<uint32_t>(srcW - 1) << 16) / static_cast<uint32_t>(dstW - 1)) : 0;
    const uint32_t stepY = (dstH > 1) ? ((static_cast<uint32_t>(srcH - 1) << 16) / static_cast<uint32_t>(dstH - 1)) : 0;

    for (int y = 0; y < dstH; ++y) {
        uint32_t syFp = static_cast<uint32_t>(y) * stepY;
        int sy0 = static_cast<int>(syFp >> 16);
        int sy1 = (sy0 + 1 < srcH) ? (sy0 + 1) : sy0;
        uint32_t wy = (syFp & 0xFFFF) >> 8;
        uint32_t iwy = 256 - wy;

        const uint8_t* r0 = srcY + static_cast<size_t>(sy0) * srcW;
        const uint8_t* r1 = srcY + static_cast<size_t>(sy1) * srcW;
        uint8_t* out = dstY + static_cast<size_t>(y) * dstW;

        uint32_t sxFp = 0;
        for (int x = 0; x < dstW; ++x, sxFp += stepX) {
            int sx0 = static_cast<int>(sxFp >> 16);
            int sx1 = (sx0 + 1 < srcW) ? (sx0 + 1) : sx0;
            uint32_t wx = (sxFp & 0xFFFF) >> 8;
            uint32_t iwx = 256 - wx;

            uint32_t top = r0[sx0] * iwx + r0[sx1] * wx;
            uint32_t bot = r1[sx0] * iwx + r1[sx1] * wx;
            out[x] = static_cast<uint8_t>((top * iwy + bot * wy + 32768) >> 16);
        }
    }

    const int srcUvW = srcW / 2;
    const int srcUvH = srcH / 2;
    const int dstUvW = dstW / 2;
    const int dstUvH = dstH / 2;
    const uint32_t uvStepX = (dstUvW > 1) ? ((static_cast<uint32_t>(srcUvW - 1) << 16) / static_cast<uint32_t>(dstUvW - 1)) : 0;
    const uint32_t uvStepY = (dstUvH > 1) ? ((static_cast<uint32_t>(srcUvH - 1) << 16) / static_cast<uint32_t>(dstUvH - 1)) : 0;

    for (int y = 0; y < dstUvH; ++y) {
        uint32_t syFp = static_cast<uint32_t>(y) * uvStepY;
        int sy0 = static_cast<int>(syFp >> 16);
        int sy1 = (sy0 + 1 < srcUvH) ? (sy0 + 1) : sy0;
        uint32_t wy = (syFp & 0xFFFF) >> 8;
        uint32_t iwy = 256 - wy;

        const uint8_t* r0 = srcUV + static_cast<size_t>(sy0) * srcW;
        const uint8_t* r1 = srcUV + static_cast<size_t>(sy1) * srcW;
        uint8_t* out = dstUV + static_cast<size_t>(y) * dstW;

        uint32_t sxFp = 0;
        for (int x = 0; x < dstUvW; ++x, sxFp += uvStepX) {
            int sx0 = static_cast<int>(sxFp >> 16);
            int sx1 = (sx0 + 1 < srcUvW) ? (sx0 + 1) : sx0;
            uint32_t wx = (sxFp & 0xFFFF) >> 8;
            uint32_t iwx = 256 - wx;

            int o0 = sx0 * 2;
            int o1 = sx1 * 2;
            uint32_t uTop = r0[o0] * iwx + r0[o1] * wx;
            uint32_t uBot = r1[o0] * iwx + r1[o1] * wx;
            uint32_t vTop = r0[o0 + 1] * iwx + r0[o1 + 1] * wx;
            uint32_t vBot = r1[o0 + 1] * iwx + r1[o1 + 1] * wx;

            out[x * 2]     = static_cast<uint8_t>((uTop * iwy + uBot * wy + 32768) >> 16);
            out[x * 2 + 1] = static_cast<uint8_t>((vTop * iwy + vBot * wy + 32768) >> 16);
        }
    }
}

static inline void ConvertNV12toRGB24(const uint8_t* yPlane, const uint8_t* uvPlane, uint8_t* rgbDst, int width, int height) {
    for (int y = 0; y < height; ++y) {
        const uint8_t* yRow = yPlane + y * width;
        const uint8_t* uvRow = uvPlane + (y / 2) * width;
        uint8_t* dstRow = rgbDst + static_cast<size_t>(height - 1 - y) * width * 3;

        for (int x = 0; x < width; x += 2) {
            int uVal = static_cast<int>(uvRow[x]) - 128;
            int vVal = static_cast<int>(uvRow[x + 1]) - 128;
            int rCoeff = 409 * vVal + 128;
            int gCoeff = -100 * uVal - 208 * vVal + 128;
            int bCoeff = 516 * uVal + 128;

            // Pixel 0
            int y0 = (static_cast<int>(yRow[x]) - 16) * 298;
            dstRow[x * 3]     = clampU8((y0 + bCoeff) >> 8);
            dstRow[x * 3 + 1] = clampU8((y0 + gCoeff) >> 8);
            dstRow[x * 3 + 2] = clampU8((y0 + rCoeff) >> 8);

            // Pixel 1 (shares chroma U/V sample in NV12)
            if (x + 1 < width) {
                int y1 = (static_cast<int>(yRow[x + 1]) - 16) * 298;
                dstRow[x * 3 + 3] = clampU8((y1 + bCoeff) >> 8);
                dstRow[x * 3 + 4] = clampU8((y1 + gCoeff) >> 8);
                dstRow[x * 3 + 5] = clampU8((y1 + rCoeff) >> 8);
            }
        }
    }
}

inline void DShowPin::StreamingLoop() {
    const size_t nv12Size = (size_t)m_width * m_height * 3 / 2;
    const size_t rgbSize = (size_t)m_width * m_height * 3;
    std::vector<uint8_t> localNv12Scratch(nv12Size);
    std::vector<uint8_t> localSrcScratch;
    auto nextFrameTick = std::chrono::steady_clock::now();
    const auto frameInterval = std::chrono::microseconds((m_avgTimePerFrame > 0) ? (m_avgTimePerFrame / 10) : 33333);

    while (m_isStreaming) {
        if (!m_pSharedBuffer) {
            m_hSharedMem = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, MF_VCAM_MEM_NAME);
            if (m_hSharedMem) {
                m_pSharedBuffer = (uint8_t*)MapViewOfFile(m_hSharedMem, FILE_MAP_ALL_ACCESS, 0, 0, 0);
                if (m_pSharedBuffer) {
                    m_isWritableView = true;
                } else {
                    CloseHandle(m_hSharedMem);
                    m_hSharedMem = nullptr;
                }
            }
            if (!m_pSharedBuffer) {
                m_hSharedMem = OpenFileMappingA(FILE_MAP_READ, FALSE, MF_VCAM_MEM_NAME);
                if (m_hSharedMem) {
                    m_pSharedBuffer = (uint8_t*)MapViewOfFile(m_hSharedMem, FILE_MAP_READ, 0, 0, 0);
                    m_isWritableView = false;
                }
            }
            if (m_pSharedBuffer && m_isWritableView && !m_countedReader) {
                auto* hdr = reinterpret_cast<MFVirtualCamHeader*>(m_pSharedBuffer);
                InterlockedIncrement(&hdr->activeReaders);
                m_countedReader = true;
            }
        }
        if (!m_hFrameEvent) {
            m_hFrameEvent = OpenEventA(SYNCHRONIZE, FALSE, MF_VCAM_EVENT_NAME);
        }

        if (m_hFrameEvent) {
            DWORD waitMs = static_cast<DWORD>((frameInterval.count() + 999) / 1000);
            if (waitMs < 8) waitMs = 8;
            WaitForSingleObject(m_hFrameEvent, waitMs);
            auto nowTick = std::chrono::steady_clock::now();
            if (nowTick < nextFrameTick) {
                std::this_thread::sleep_until(nextFrameTick);
            }
            nextFrameTick = (std::max)(nowTick, nextFrameTick) + frameInterval;
        } else {
            std::this_thread::sleep_for(frameInterval);
        }

        if (!m_isStreaming) break;

        if (m_pInputPin && m_pAllocator) {
            IMediaSample* pSample = nullptr;
            HRESULT hr = m_pAllocator->GetBuffer(&pSample, nullptr, nullptr, 0);

            if (hr == VFW_E_NOT_COMMITTED) {
                m_pAllocator->Commit();
                hr = m_pAllocator->GetBuffer(&pSample, nullptr, nullptr, 0);
            }

            if (SUCCEEDED(hr) && pSample) {
                BYTE* pDst = nullptr;
                if (SUCCEEDED(pSample->GetPointer(&pDst))) {
                    bool frameCopied = false;
                    if (m_pSharedBuffer) {
                        const auto* hdr = reinterpret_cast<const MFVirtualCamHeader*>(m_pSharedBuffer);
                        uint64_t nowMs = GetTickCount64();
                        bool isAlive = (hdr->writerTick != 0 && (nowMs - hdr->writerTick) <= 3000);
                        if (isAlive && hdr->magic == 0x4D465643) {
                            const uint8_t* pSrc = m_pSharedBuffer + sizeof(MFVirtualCamHeader);
                            uint8_t* copyTarget = (m_selectedSubtype == MEDIASUBTYPE_NV12) ? pDst : localNv12Scratch.data();
                            uint32_t copiedW = 0, copiedH = 0;
                            bool copiedScaled = false;

                            for (int attempt = 0; attempt < 4; ++attempt) {
                                int32_t s1 = hdr->seq;
                                if (s1 & 1) {
                                    Sleep(1);
                                    continue;
                                }
                                MemoryBarrier();
                                const uint32_t srcW = hdr->width;
                                const uint32_t srcH = hdr->height;
                                const size_t srcNv12Size = static_cast<size_t>(srcW) * srcH * 3 / 2;
                                if (srcW < 2 || srcH < 2 || srcW > 3840 || srcH > 2160 || hdr->frameSize < srcNv12Size) {
                                    break;
                                }
                                const bool needsScale = (srcW != (uint32_t)m_width || srcH != (uint32_t)m_height);
                                if (needsScale && localSrcScratch.size() < srcNv12Size) {
                                    localSrcScratch.resize(srcNv12Size);
                                }
                                if (!needsScale) {
                                    memcpy(copyTarget, pSrc, nv12Size);
                                } else {
                                    memcpy(localSrcScratch.data(), pSrc, srcNv12Size);
                                }
                                MemoryBarrier();
                                if (hdr->seq == s1) {
                                    frameCopied = true;
                                    copiedW = srcW;
                                    copiedH = srcH;
                                    copiedScaled = needsScale;
                                    break;
                                }
                            }
                            if (frameCopied && copiedScaled) {
                                ScaleNV12(localSrcScratch.data(), static_cast<int>(copiedW), static_cast<int>(copiedH),
                                          copyTarget, m_width, m_height);
                            }
                        }
                    }

                    if (m_selectedSubtype == MEDIASUBTYPE_NV12) {
                        if (!frameCopied) {
                            memset(pDst, 0x10, (size_t)m_width * m_height);
                            memset(pDst + (size_t)m_width * m_height, 0x80, (size_t)m_width * m_height / 2);
                        }
                        pSample->SetActualDataLength((long)nv12Size);
                    } else if (m_selectedSubtype == MEDIASUBTYPE_I420_VCAM) {
                        if (frameCopied) {
                            const uint8_t* pSrcY = localNv12Scratch.data();
                            const uint8_t* pSrcUV = pSrcY + ((size_t)m_width * m_height);
                            ConvertNV12toI420(pSrcY, pSrcUV, pDst, m_width, m_height);
                        } else {
                            memset(pDst, 0x10, (size_t)m_width * m_height);
                            memset(pDst + (size_t)m_width * m_height, 0x80, (size_t)m_width * m_height / 2);
                        }
                        pSample->SetActualDataLength((long)nv12Size);
                    } else {
                        if (frameCopied) {
                            const uint8_t* pSrcY = localNv12Scratch.data();
                            const uint8_t* pSrcUV = pSrcY + ((size_t)m_width * m_height);
                            ConvertNV12toRGB24(pSrcY, pSrcUV, pDst, m_width, m_height);
                        } else {
                            memset(pDst, 0x00, rgbSize);
                        }
                        pSample->SetActualDataLength((long)rgbSize);
                    }

                    pSample->SetSyncPoint(TRUE);
                    pSample->SetPreroll(FALSE);

                    REFERENCE_TIME rtStart = m_frameCounter * m_avgTimePerFrame;
                    REFERENCE_TIME rtEnd = rtStart + m_avgTimePerFrame;
                    pSample->SetTime(&rtStart, &rtEnd);
                    m_frameCounter++;

                    m_pInputPin->Receive(pSample);
                }
                pSample->Release();
            }
        }
    }
}