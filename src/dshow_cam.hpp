#pragma once
#include <windows.h>
#include <dshow.h>
#include <strsafe.h>
#include <atomic>
#include <thread>

#include <ks.h>
#include <ksproxy.h>
#include <ksmedia.h>

#include "mf_shared_mem.hpp"

// CLSID: {E1D3B890-5F16-47D8-9C9D-9F0A3E8B81B1}
static const GUID CLSID_NativeVirtualCamDShow =
{ 0xe1d3b890, 0x5f16, 0x47d8, { 0x9c, 0x9d, 0x9f, 0x0a, 0x3e, 0x8b, 0x81, 0xb1 } };

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
        *pClassID = CLSID_NativeVirtualCamDShow;
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
    void FillMediaType(AM_MEDIA_TYPE* pmt, int index);

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
    HANDLE m_hFrameEvent = nullptr;

    int m_width = 1280;
    int m_height = 720;
    GUID m_selectedSubtype = MEDIASUBTYPE_NV12;
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
        return (m_index >= 2) ? S_FALSE : S_OK;
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

inline void DShowPin::FillMediaType(AM_MEDIA_TYPE* pmt, int index) {
    ZeroMemory(pmt, sizeof(AM_MEDIA_TYPE));
    pmt->majortype = MEDIATYPE_Video;
    pmt->bFixedSizeSamples = TRUE;
    pmt->bTemporalCompression = FALSE;
    pmt->formattype = FORMAT_VideoInfo;
    pmt->cbFormat = sizeof(VIDEOINFOHEADER);
    pmt->pUnk = nullptr;

    VIDEOINFOHEADER* vih = (VIDEOINFOHEADER*)CoTaskMemAlloc(sizeof(VIDEOINFOHEADER));
    ZeroMemory(vih, sizeof(VIDEOINFOHEADER));
    vih->rcSource = RECT{ 0, 0, m_width, m_height };
    vih->rcTarget = RECT{ 0, 0, m_width, m_height };
    vih->AvgTimePerFrame = m_avgTimePerFrame;
    vih->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    vih->bmiHeader.biWidth = m_width;
    vih->bmiHeader.biPlanes = 1;

    if (index == 0) {
        pmt->subtype = MEDIASUBTYPE_NV12;
        pmt->lSampleSize = (ULONG)(m_width * m_height * 3 / 2);
        vih->bmiHeader.biHeight = m_height;
        vih->bmiHeader.biBitCount = 12;
        vih->bmiHeader.biCompression = MAKEFOURCC('N', 'V', '1', '2');
        vih->bmiHeader.biSizeImage = pmt->lSampleSize;
    }
    else {
        pmt->subtype = MEDIASUBTYPE_RGB24;
        pmt->lSampleSize = (ULONG)(m_width * m_height * 3);
        vih->bmiHeader.biHeight = m_height;
        vih->bmiHeader.biBitCount = 24;
        vih->bmiHeader.biCompression = BI_RGB;
        vih->bmiHeader.biSizeImage = pmt->lSampleSize;
    }
    pmt->pbFormat = (BYTE*)vih;
}

inline STDMETHODIMP DShowPin::Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt) {
    if (!pReceivePin) return E_POINTER;
    if (m_pConnectedPin) return VFW_E_ALREADY_CONNECTED;

    AM_MEDIA_TYPE mt{};
    bool allocated = false;

    if (pmt) {
        mt = *pmt;
    }
    else {
        FillMediaType(&mt, 0);
        allocated = true;
    }

    HRESULT hr = pReceivePin->ReceiveConnection(this, &mt);
    if (FAILED(hr) && !pmt) {
        if (allocated && mt.pbFormat) CoTaskMemFree(mt.pbFormat);
        FillMediaType(&mt, 1);
        hr = pReceivePin->ReceiveConnection(this, &mt);
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
                prop.cbBuffer = (m_selectedSubtype == MEDIASUBTYPE_NV12) ? (m_width * m_height * 3 / 2) : (m_width * m_height * 3);
                prop.cbAlign = 1;
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
    FillMediaType(pmt, (m_selectedSubtype == MEDIASUBTYPE_RGB24) ? 1 : 0);
    return S_OK;
}

inline STDMETHODIMP DShowPin::QueryAccept(const AM_MEDIA_TYPE* pmt) {
    if (!pmt) return E_POINTER;
    if (pmt->majortype != MEDIATYPE_Video) return S_FALSE;

    if (pmt->subtype == MEDIASUBTYPE_NV12 ||
        pmt->subtype == MEDIASUBTYPE_RGB24 ||
        pmt->subtype == MEDIASUBTYPE_RGB32) {
        return S_OK;
    }
    return S_FALSE;
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
    while (m_index < 2 && count < cMediaTypes) {
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
    if (pmt->majortype != MEDIATYPE_Video) return E_FAIL;

    if (pmt->formattype == FORMAT_VideoInfo && pmt->pbFormat) {
        VIDEOINFOHEADER* vih = (VIDEOINFOHEADER*)pmt->pbFormat;
        m_width = vih->bmiHeader.biWidth;
        m_height = abs(vih->bmiHeader.biHeight);
        if (vih->AvgTimePerFrame > 0) {
            m_avgTimePerFrame = vih->AvgTimePerFrame;
        }
    }
    m_selectedSubtype = pmt->subtype;
    return S_OK;
}

inline STDMETHODIMP DShowPin::GetFormat(AM_MEDIA_TYPE** ppmt) {
    if (!ppmt) return E_POINTER;
    AM_MEDIA_TYPE* pmt = (AM_MEDIA_TYPE*)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
    FillMediaType(pmt, (m_selectedSubtype == MEDIASUBTYPE_RGB24) ? 1 : 0);
    *ppmt = pmt;
    return S_OK;
}

inline STDMETHODIMP DShowPin::GetNumberOfCapabilities(int* piCount, int* piSize) {
    if (!piCount || !piSize) return E_POINTER;
    *piCount = 2;
    *piSize = sizeof(VIDEO_STREAM_CONFIG_CAPS);
    return S_OK;
}

inline STDMETHODIMP DShowPin::GetStreamCaps(int iIndex, AM_MEDIA_TYPE** ppmt, BYTE* pSCC) {
    if (!ppmt || !pSCC) return E_POINTER;
    if (iIndex < 0 || iIndex > 1) return S_FALSE;

    AM_MEDIA_TYPE* pmt = (AM_MEDIA_TYPE*)CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE));
    FillMediaType(pmt, iIndex);
    *ppmt = pmt;

    VIDEO_STREAM_CONFIG_CAPS* caps = (VIDEO_STREAM_CONFIG_CAPS*)pSCC;
    ZeroMemory(caps, sizeof(VIDEO_STREAM_CONFIG_CAPS));
    caps->guid = FORMAT_VideoInfo;
    caps->VideoStandard = 0;
    caps->InputSize.cx = 1280;
    caps->InputSize.cy = 720;
    caps->MinCroppingSize.cx = 1280;
    caps->MinCroppingSize.cy = 720;
    caps->MaxCroppingSize.cx = 1280;
    caps->MaxCroppingSize.cy = 720;
    caps->CropGranularityX = 1;
    caps->CropGranularityY = 1;
    caps->CropAlignX = 0;
    caps->CropAlignY = 0;
    caps->MinOutputSize.cx = 1280;
    caps->MinOutputSize.cy = 720;
    caps->MaxOutputSize.cx = 1280;
    caps->MaxOutputSize.cy = 720;
    caps->OutputGranularityX = 1;
    caps->OutputGranularityY = 1;
    caps->MinFrameInterval = 166666;
    caps->MaxFrameInterval = 333333;

    return S_OK;
}

inline void DShowPin::StartStreaming() {
    if (m_isStreaming) return;
    m_isStreaming = true;
    m_frameCounter = 0;

    m_hSharedMem = OpenFileMappingA(FILE_MAP_READ, FALSE, MF_VCAM_MEM_NAME);
    if (m_hSharedMem) {
        m_pSharedBuffer = (uint8_t*)MapViewOfFile(m_hSharedMem, FILE_MAP_READ, 0, 0, 0);
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
        UnmapViewOfFile(m_pSharedBuffer);
        m_pSharedBuffer = nullptr;
    }
    if (m_hSharedMem) {
        CloseHandle(m_hSharedMem);
        m_hSharedMem = nullptr;
    }
    if (m_hFrameEvent) {
        CloseHandle(m_hFrameEvent);
        m_hFrameEvent = nullptr;
    }
}

static inline void ConvertNV12toRGB24(const uint8_t* yPlane, const uint8_t* uvPlane, uint8_t* rgbDst, int width, int height) {
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int yVal = yPlane[y * width + x] - 16;
            int uvIdx = (y / 2) * width + (x & ~1);
            int uVal = uvPlane[uvIdx] - 128;
            int vVal = uvPlane[uvIdx + 1] - 128;

            int r = (298 * yVal + 409 * vVal + 128) >> 8;
            int g = (298 * yVal - 100 * uVal - 208 * vVal + 128) >> 8;
            int b = (298 * yVal + 516 * uVal + 128) >> 8;

            int dstIdx = ((height - 1 - y) * width + x) * 3;
            rgbDst[dstIdx] = (uint8_t)max(0, min(255, b));
            rgbDst[dstIdx + 1] = (uint8_t)max(0, min(255, g));
            rgbDst[dstIdx + 2] = (uint8_t)max(0, min(255, r));
        }
    }
}

inline void DShowPin::StreamingLoop() {
    const size_t nv12Size = (size_t)m_width * m_height * 3 / 2;
    const size_t rgbSize = (size_t)m_width * m_height * 3;

    if (!m_pSharedBuffer) {
        m_hSharedMem = OpenFileMappingA(FILE_MAP_READ, FALSE, MF_VCAM_MEM_NAME);
        if (m_hSharedMem) {
            m_pSharedBuffer = (uint8_t*)MapViewOfFile(m_hSharedMem, FILE_MAP_READ, 0, 0, 0);
        }
    }
    if (!m_hFrameEvent) {
        m_hFrameEvent = OpenEventA(SYNCHRONIZE, FALSE, MF_VCAM_EVENT_NAME);
    }

    while (m_isStreaming) {
        if (m_hFrameEvent) {
            WaitForSingleObject(m_hFrameEvent, 33);
        }
        else {
            Sleep(33);
        }

        if (!m_isStreaming) break;

        if (!m_pSharedBuffer) {
            m_hSharedMem = OpenFileMappingA(FILE_MAP_READ, FALSE, MF_VCAM_MEM_NAME);
            if (m_hSharedMem) {
                m_pSharedBuffer = (uint8_t*)MapViewOfFile(m_hSharedMem, FILE_MAP_READ, 0, 0, 0);
            }
        }

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
                    if (m_selectedSubtype == MEDIASUBTYPE_NV12) {
                        if (m_pSharedBuffer) {
                            BYTE* pSrc = m_pSharedBuffer + sizeof(MFVirtualCamHeader);
                            memcpy(pDst, pSrc, nv12Size);
                        }
                        else {
                            memset(pDst, 0x10, m_width * m_height);
                            memset(pDst + m_width * m_height, 0x80, m_width * m_height / 2);
                        }
                        pSample->SetActualDataLength((long)nv12Size);
                    }
                    else {
                        if (m_pSharedBuffer) {
                            uint8_t* pSrcY = m_pSharedBuffer + sizeof(MFVirtualCamHeader);
                            uint8_t* pSrcUV = pSrcY + (m_width * m_height);
                            ConvertNV12toRGB24(pSrcY, pSrcUV, pDst, m_width, m_height);
                        }
                        else {
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