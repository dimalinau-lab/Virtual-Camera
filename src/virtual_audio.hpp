#pragma once
#include <windows.h>
#include <dshow.h>
#include <mmreg.h>
#include <atomic>
#include <thread>
#include <mutex>
#include <cstring>
#include <vector>
#include <chrono>
#include <algorithm>
#include "mf_shared_mem.hpp"

class VirtualAudioCaptureFilter;

class VirtualAudioPin : public IPin, public IAMStreamConfig, public IKsPropertySet {
public:
    VirtualAudioPin(VirtualAudioCaptureFilter* pFilter);
    virtual ~VirtualAudioPin();

    void StartStreaming();
    void StopStreaming();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IPin) {
            *ppv = static_cast<IPin*>(this);
        } else if (riid == IID_IAMStreamConfig) {
            *ppv = static_cast<IAMStreamConfig*>(this);
        } else if (riid == IID_IKsPropertySet) {
            *ppv = static_cast<IKsPropertySet*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++m_refCount; }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = --m_refCount;
        if (r == 0) delete this;
        return r;
    }

    // IPin
    STDMETHODIMP Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt) override {
        if (!pReceivePin) return E_POINTER;
        std::lock_guard<std::mutex> lock(m_pinMutex);
        if (m_connectedPin) return VFW_E_ALREADY_CONNECTED;
        if (pmt && QueryAccept(pmt) != S_OK) return VFW_E_TYPE_NOT_ACCEPTED;

        AM_MEDIA_TYPE* pDefaultMt = nullptr;
        if (FAILED(GetFormat(&pDefaultMt)) || !pDefaultMt) return E_OUTOFMEMORY;

        HRESULT hr = pReceivePin->ReceiveConnection(this, pDefaultMt);
        if (SUCCEEDED(hr)) {
            IMemInputPin* pMemInput = nullptr;
            if (SUCCEEDED(pReceivePin->QueryInterface(IID_IMemInputPin, (void**)&pMemInput)) && pMemInput) {
                ALLOCATOR_PROPERTIES req{}, actual{};
                req.cBuffers = 4;
                req.cbBuffer = 480 * 2; // 10ms @ 48kHz 16-bit mono
                req.cbAlign = 2;
                req.cbPrefix = 0;

                IMemAllocator* pAlloc = nullptr;
                hr = pMemInput->GetAllocator(&pAlloc);
                if (FAILED(hr) || !pAlloc) {
                    hr = CoCreateInstance(CLSID_MemoryAllocator, nullptr, CLSCTX_INPROC_SERVER, IID_IMemAllocator, (void**)&pAlloc);
                }
                if (SUCCEEDED(hr) && pAlloc) {
                    hr = pAlloc->SetProperties(&req, &actual);
                    if (SUCCEEDED(hr)) {
                        hr = pMemInput->NotifyAllocator(pAlloc, FALSE);
                    }
                }
                if (SUCCEEDED(hr)) {
                    m_connectedPin = pReceivePin;
                    m_connectedPin->AddRef();
                    m_memInputPin = pMemInput;
                    m_allocator = pAlloc;
                } else {
                    if (pAlloc) pAlloc->Release();
                    pMemInput->Release();
                }
            } else {
                hr = VFW_E_NO_TRANSPORT;
            }
        }
        if (pDefaultMt->pbFormat) CoTaskMemFree(pDefaultMt->pbFormat);
        CoTaskMemFree(pDefaultMt);
        return hr;
    }

    STDMETHODIMP ReceiveConnection(IPin*, const AM_MEDIA_TYPE*) override { return E_UNEXPECTED; }

    STDMETHODIMP Disconnect() override {
        StopStreaming();
        std::lock_guard<std::mutex> lock(m_pinMutex);
        if (!m_connectedPin) return S_FALSE;
        if (m_allocator) { m_allocator->Release(); m_allocator = nullptr; }
        if (m_memInputPin) { m_memInputPin->Release(); m_memInputPin = nullptr; }
        m_connectedPin->Release();
        m_connectedPin = nullptr;
        return S_OK;
    }

    STDMETHODIMP ConnectedTo(IPin** ppPin) override {
        if (!ppPin) return E_POINTER;
        std::lock_guard<std::mutex> lock(m_pinMutex);
        if (!m_connectedPin) { *ppPin = nullptr; return VFW_E_NOT_CONNECTED; }
        *ppPin = m_connectedPin;
        m_connectedPin->AddRef();
        return S_OK;
    }

    STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* pmt) override {
        if (!pmt) return E_POINTER;
        std::lock_guard<std::mutex> lock(m_pinMutex);
        if (!m_connectedPin) return VFW_E_NOT_CONNECTED;
        AM_MEDIA_TYPE* src = nullptr;
        HRESULT hr = GetFormat(&src);
        if (FAILED(hr) || !src) return hr;
        *pmt = *src;
        CoTaskMemFree(src);
        return S_OK;
    }

    STDMETHODIMP QueryPinInfo(PIN_INFO* pInfo) override;
    STDMETHODIMP QueryDirection(PIN_DIRECTION* pPinDir) override {
        if (!pPinDir) return E_POINTER;
        *pPinDir = PINDIR_OUTPUT;
        return S_OK;
    }
    STDMETHODIMP QueryId(LPWSTR* Id) override {
        if (!Id) return E_POINTER;
        const wchar_t* name = L"Capture";
        size_t sz = (wcslen(name) + 1) * sizeof(wchar_t);
        *Id = reinterpret_cast<LPWSTR>(CoTaskMemAlloc(sz));
        if (!*Id) return E_OUTOFMEMORY;
        memcpy(*Id, name, sz);
        return S_OK;
    }
    STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE* pmt) override {
        if (!pmt) return E_POINTER;
        if (pmt->majortype != MEDIATYPE_Audio || pmt->subtype != MEDIASUBTYPE_PCM ||
            pmt->formattype != FORMAT_WaveFormatEx || !pmt->pbFormat ||
            pmt->cbFormat < sizeof(WAVEFORMATEX)) {
            return S_FALSE;
        }
        const auto* pwfx = reinterpret_cast<const WAVEFORMATEX*>(pmt->pbFormat);
        if (pwfx->wFormatTag != WAVE_FORMAT_PCM || pwfx->nChannels != 1 ||
            pwfx->nSamplesPerSec != 48000 || pwfx->wBitsPerSample != 16) {
            return S_FALSE;
        }
        return S_OK;
    }

    STDMETHODIMP EnumMediaTypes(IEnumMediaTypes** ppEnum) override;
    STDMETHODIMP QueryInternalConnections(IPin**, ULONG*) override { return E_NOTIMPL; }
    STDMETHODIMP EndOfStream() override { return S_OK; }
    STDMETHODIMP BeginFlush() override { return S_OK; }
    STDMETHODIMP EndFlush() override { return S_OK; }
    STDMETHODIMP NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) override { return S_OK; }

    // IAMStreamConfig
    STDMETHODIMP SetFormat(AM_MEDIA_TYPE* pmt) override {
        if (!pmt) return E_POINTER;
        return (QueryAccept(pmt) == S_OK) ? S_OK : VFW_E_INVALIDMEDIATYPE;
    }
    STDMETHODIMP GetFormat(AM_MEDIA_TYPE** ppmt) override {
        if (!ppmt) return E_POINTER;
        auto* pmt = reinterpret_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
        if (!pmt) return E_OUTOFMEMORY;
        memset(pmt, 0, sizeof(AM_MEDIA_TYPE));
        auto* pwfx = reinterpret_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
        if (!pwfx) {
            CoTaskMemFree(pmt);
            return E_OUTOFMEMORY;
        }
        pwfx->wFormatTag = WAVE_FORMAT_PCM;
        pwfx->nChannels = 1;
        pwfx->nSamplesPerSec = 48000;
        pwfx->wBitsPerSample = 16;
        pwfx->nBlockAlign = 2;
        pwfx->nAvgBytesPerSec = 96000;
        pwfx->cbSize = 0;

        pmt->majortype = MEDIATYPE_Audio;
        pmt->subtype = MEDIASUBTYPE_PCM;
        pmt->bFixedSizeSamples = TRUE;
        pmt->bTemporalCompression = FALSE;
        pmt->lSampleSize = 2;
        pmt->formattype = FORMAT_WaveFormatEx;
        pmt->cbFormat = sizeof(WAVEFORMATEX);
        pmt->pbFormat = reinterpret_cast<BYTE*>(pwfx);
        *ppmt = pmt;
        return S_OK;
    }
    STDMETHODIMP GetNumberOfCapabilities(int* piCount, int* piSize) override {
        if (!piCount || !piSize) return E_POINTER;
        *piCount = 1;
        *piSize = sizeof(AUDIO_STREAM_CONFIG_CAPS);
        return S_OK;
    }
    STDMETHODIMP GetStreamCaps(int iIndex, AM_MEDIA_TYPE** ppmt, BYTE* pSCC) override {
        if (iIndex != 0) return S_FALSE;
        if (pSCC) {
            auto* caps = reinterpret_cast<AUDIO_STREAM_CONFIG_CAPS*>(pSCC);
            memset(caps, 0, sizeof(AUDIO_STREAM_CONFIG_CAPS));
            caps->guid = MEDIATYPE_Audio;
            caps->MinimumChannels = 1;
            caps->MaximumChannels = 1;
            caps->ChannelsGranularity = 1;
            caps->MinimumBitsPerSample = 16;
            caps->MaximumBitsPerSample = 16;
            caps->BitsPerSampleGranularity = 16;
            caps->MinimumSampleFrequency = 48000;
            caps->MaximumSampleFrequency = 48000;
            caps->SampleFrequencyGranularity = 48000;
        }
        return GetFormat(ppmt);
    }

    // IKsPropertySet
    STDMETHODIMP Set(REFGUID, DWORD, LPVOID, DWORD, LPVOID, DWORD) override { return E_NOTIMPL; }
    STDMETHODIMP Get(REFGUID guidPropSet, DWORD dwPropID, LPVOID, DWORD, LPVOID pPropData, DWORD cbPropData, DWORD* pcbReturned) override {
        if (guidPropSet == AMPROPSETID_Pin && dwPropID == AMPROPERTY_PIN_CATEGORY) {
            if (pcbReturned) *pcbReturned = sizeof(GUID);
            if (!pPropData || cbPropData < sizeof(GUID)) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
            *reinterpret_cast<GUID*>(pPropData) = PIN_CATEGORY_CAPTURE;
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

private:
    void workerLoop() {
        HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const DWORD frameBytes = 480 * 2; // 10ms @ 48kHz mono 16-bit
        const REFERENCE_TIME frameDuration = 100000; // 10ms in 100ns units
        REFERENCE_TIME streamTime = 0;

        HANDLE hMap = nullptr;
        MFAudioSharedHeader* shm = nullptr;
        uint8_t* ring = nullptr;
        HANDLE hMutex = CreateMutexA(nullptr, FALSE, MF_AUDIO_MUTEX_NAME);

        auto nextTick = std::chrono::steady_clock::now();

        while (m_streaming.load()) {
            nextTick += std::chrono::milliseconds(10);

            if (!shm) {
                hMap = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, MF_AUDIO_MEM_NAME);
                if (hMap) {
                    shm = reinterpret_cast<MFAudioSharedHeader*>(MapViewOfFile(hMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(MFAudioSharedHeader) + MF_AUDIO_BUFFER_SIZE));
                    if (shm && shm->magic == 0x4D464155) {
                        ring = reinterpret_cast<uint8_t*>(shm + 1);
                    } else {
                        if (shm) UnmapViewOfFile(shm);
                        shm = nullptr;
                        CloseHandle(hMap);
                        hMap = nullptr;
                    }
                }
            }

            IMemAllocator* pAlloc = nullptr;
            IMemInputPin* pMemIn = nullptr;
            {
                std::lock_guard<std::mutex> lock(m_pinMutex);
                pAlloc = m_allocator;
                pMemIn = m_memInputPin;
                if (pAlloc) pAlloc->AddRef();
                if (pMemIn) pMemIn->AddRef();
            }

            if (pAlloc && pMemIn) {
                IMediaSample* pSample = nullptr;
                if (SUCCEEDED(pAlloc->GetBuffer(&pSample, nullptr, nullptr, 0)) && pSample) {
                    BYTE* dst = nullptr;
                    if (SUCCEEDED(pSample->GetPointer(&dst)) && dst && pSample->GetSize() >= static_cast<long>(frameBytes)) {
                        memset(dst, 0, frameBytes);
                        if (shm && ring && shm->magic == 0x4D464155) {
                            if (!hMutex || WaitForSingleObject(hMutex, 2) == WAIT_OBJECT_0) {
                                uint32_t wPos = shm->writePos % MF_AUDIO_BUFFER_SIZE;
                                uint32_t rPos = shm->readPos % MF_AUDIO_BUFFER_SIZE;
                                uint32_t avail = (wPos >= rPos) ? (wPos - rPos) : (MF_AUDIO_BUFFER_SIZE - rPos + wPos);
                                if (avail >= frameBytes) {
                                    for (DWORD i = 0; i < frameBytes; ++i) {
                                        dst[i] = ring[(rPos + i) % MF_AUDIO_BUFFER_SIZE];
                                    }
                                    shm->readPos = (rPos + frameBytes) % MF_AUDIO_BUFFER_SIZE;
                                }
                                if (hMutex) ReleaseMutex(hMutex);
                            }
                        }
                        pSample->SetActualDataLength(frameBytes);
                        REFERENCE_TIME rtEnd = streamTime + frameDuration;
                        pSample->SetTime(&streamTime, &rtEnd);
                        pSample->SetSyncPoint(TRUE);
                        streamTime = rtEnd;
                        pMemIn->Receive(pSample);
                    }
                    pSample->Release();
                }
            }
            if (pAlloc) pAlloc->Release();
            if (pMemIn) pMemIn->Release();

            std::this_thread::sleep_until(nextTick);
            auto afterSleep = std::chrono::steady_clock::now();
            if (afterSleep > nextTick + std::chrono::milliseconds(50)) {
                nextTick = afterSleep;
            }
        }

        if (shm) UnmapViewOfFile(shm);
        if (hMap) CloseHandle(hMap);
        if (hMutex) CloseHandle(hMutex);
        if (SUCCEEDED(hrCo)) CoUninitialize();
    }

    VirtualAudioCaptureFilter* m_filter;
    std::atomic<ULONG> m_refCount{ 1 };
    std::mutex m_pinMutex;
    IPin* m_connectedPin{ nullptr };
    IMemInputPin* m_memInputPin{ nullptr };
    IMemAllocator* m_allocator{ nullptr };
    std::atomic<bool> m_streaming{ false };
    std::thread m_streamThread;
};

class AudioEnumMediaTypes : public IEnumMediaTypes {
public:
    AudioEnumMediaTypes(VirtualAudioPin* pin, ULONG pos = 0) : m_pin(pin), m_pos(pos) {
        if (m_pin) m_pin->AddRef();
        g_vcamComRefCount.fetch_add(1, std::memory_order_relaxed);
    }
    virtual ~AudioEnumMediaTypes() {
        if (m_pin) m_pin->Release();
        g_vcamComRefCount.fetch_sub(1, std::memory_order_relaxed);
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
    STDMETHODIMP_(ULONG) AddRef() override { return ++m_refCount; }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = --m_refCount;
        if (r == 0) delete this;
        return r;
    }
    STDMETHODIMP Next(ULONG cMediaTypes, AM_MEDIA_TYPE** ppMediaTypes, ULONG* pcFetched) override {
        if (!ppMediaTypes) return E_POINTER;
        ULONG fetched = 0;
        if (m_pos == 0 && cMediaTypes > 0 && m_pin) {
            AM_MEDIA_TYPE* pmt = nullptr;
            if (SUCCEEDED(m_pin->GetFormat(&pmt)) && pmt) {
                ppMediaTypes[0] = pmt;
                fetched = 1;
                m_pos = 1;
            }
        }
        if (pcFetched) *pcFetched = fetched;
        return (fetched == cMediaTypes) ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG cMediaTypes) override {
        m_pos = (std::min<ULONG>)(1, m_pos + cMediaTypes);
        return (cMediaTypes == 0 || m_pos == 0) ? S_OK : S_FALSE;
    }
    STDMETHODIMP Reset() override { m_pos = 0; return S_OK; }
    STDMETHODIMP Clone(IEnumMediaTypes** ppEnum) override {
        if (!ppEnum) return E_POINTER;
        *ppEnum = new AudioEnumMediaTypes(m_pin, m_pos);
        return S_OK;
    }
private:
    VirtualAudioPin* m_pin;
    ULONG m_pos;
    std::atomic<ULONG> m_refCount{ 1 };
};

class AudioEnumPins : public IEnumPins {
public:
    AudioEnumPins(IPin* pin, ULONG pos = 0) : m_pin(pin), m_pos(pos) {
        if (m_pin) m_pin->AddRef();
        g_vcamComRefCount.fetch_add(1, std::memory_order_relaxed);
    }
    virtual ~AudioEnumPins() {
        if (m_pin) m_pin->Release();
        g_vcamComRefCount.fetch_sub(1, std::memory_order_relaxed);
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
    STDMETHODIMP_(ULONG) AddRef() override { return ++m_refCount; }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = --m_refCount;
        if (r == 0) delete this;
        return r;
    }
    STDMETHODIMP Next(ULONG cPins, IPin** ppPins, ULONG* pcFetched) override {
        if (!ppPins) return E_POINTER;
        ULONG fetched = 0;
        if (m_pos == 0 && cPins > 0 && m_pin) {
            ppPins[0] = m_pin;
            m_pin->AddRef();
            fetched = 1;
            m_pos = 1;
        }
        if (pcFetched) *pcFetched = fetched;
        return (fetched == cPins) ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG cPins) override {
        m_pos = (std::min<ULONG>)(1, m_pos + cPins);
        return S_OK;
    }
    STDMETHODIMP Reset() override { m_pos = 0; return S_OK; }
    STDMETHODIMP Clone(IEnumPins** ppEnum) override {
        if (!ppEnum) return E_POINTER;
        *ppEnum = new AudioEnumPins(m_pin, m_pos);
        return S_OK;
    }
private:
    IPin* m_pin;
    ULONG m_pos;
    std::atomic<ULONG> m_refCount{ 1 };
};

class VirtualAudioCaptureFilter : public IBaseFilter, public IAMStreamConfig {
public:
    VirtualAudioCaptureFilter() : m_refCount(1), m_state(State_Stopped), m_graph(nullptr) {
        g_vcamComRefCount.fetch_add(1, std::memory_order_relaxed);
        m_pin = new VirtualAudioPin(this);
    }

    virtual ~VirtualAudioCaptureFilter() {
        if (m_pin) {
            m_pin->StopStreaming();
            m_pin->Release();
            m_pin = nullptr;
        }
        g_vcamComRefCount.fetch_sub(1, std::memory_order_relaxed);
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IBaseFilter || riid == IID_IMediaFilter) {
            *ppv = static_cast<IBaseFilter*>(this);
        } else if (riid == IID_IAMStreamConfig) {
            *ppv = static_cast<IAMStreamConfig*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    STDMETHODIMP_(ULONG) AddRef() override { return ++m_refCount; }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG count = --m_refCount;
        if (count == 0) delete this;
        return count;
    }

    // IPersist
    STDMETHODIMP GetClassID(CLSID* pClassID) override {
        if (!pClassID) return E_POINTER;
        *pClassID = CLSID_VirtualCamNativeMic;
        return S_OK;
    }

    // IMediaFilter
    STDMETHODIMP Stop() override {
        m_state = State_Stopped;
        if (m_pin) m_pin->StopStreaming();
        return S_OK;
    }
    STDMETHODIMP Pause() override {
        m_state = State_Paused;
        return S_OK;
    }
    STDMETHODIMP Run(REFERENCE_TIME) override {
        m_state = State_Running;
        if (m_pin) m_pin->StartStreaming();
        return S_OK;
    }
    STDMETHODIMP GetState(DWORD, FILTER_STATE* pState) override {
        if (!pState) return E_POINTER;
        *pState = m_state;
        return S_OK;
    }
    STDMETHODIMP SetSyncSource(IReferenceClock*) override { return S_OK; }
    STDMETHODIMP GetSyncSource(IReferenceClock** ppClock) override { if (ppClock) *ppClock = nullptr; return S_OK; }

    // IBaseFilter
    STDMETHODIMP EnumPins(IEnumPins** ppEnum) override {
        if (!ppEnum) return E_POINTER;
        *ppEnum = new AudioEnumPins(m_pin);
        return S_OK;
    }
    STDMETHODIMP FindPin(LPCWSTR Id, IPin** ppPin) override {
        if (!ppPin) return E_POINTER;
        if (Id && wcscmp(Id, L"Capture") == 0 && m_pin) {
            *ppPin = m_pin;
            m_pin->AddRef();
            return S_OK;
        }
        *ppPin = nullptr;
        return VFW_E_NOT_FOUND;
    }
    STDMETHODIMP QueryFilterInfo(FILTER_INFO* pInfo) override {
        if (!pInfo) return E_POINTER;
        wcsncpy_s(pInfo->achName, L"VirtualCam Native Microphone", sizeof(pInfo->achName) / sizeof(wchar_t));
        pInfo->pGraph = m_graph;
        if (m_graph) m_graph->AddRef();
        return S_OK;
    }
    STDMETHODIMP JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR) override {
        m_graph = pGraph;
        return S_OK;
    }
    STDMETHODIMP QueryVendorInfo(LPWSTR* ppVendorInfo) override { if (ppVendorInfo) *ppVendorInfo = nullptr; return E_NOTIMPL; }

    // IAMStreamConfig
    STDMETHODIMP SetFormat(AM_MEDIA_TYPE* pmt) override { return m_pin ? m_pin->SetFormat(pmt) : E_FAIL; }
    STDMETHODIMP GetFormat(AM_MEDIA_TYPE** ppmt) override { return m_pin ? m_pin->GetFormat(ppmt) : E_FAIL; }
    STDMETHODIMP GetNumberOfCapabilities(int* piCount, int* piSize) override { return m_pin ? m_pin->GetNumberOfCapabilities(piCount, piSize) : E_FAIL; }
    STDMETHODIMP GetStreamCaps(int iIndex, AM_MEDIA_TYPE** ppmt, BYTE* pSCC) override { return m_pin ? m_pin->GetStreamCaps(iIndex, ppmt, pSCC) : E_FAIL; }

private:
    std::atomic<ULONG> m_refCount;
    FILTER_STATE m_state;
    IFilterGraph* m_graph;
    VirtualAudioPin* m_pin;
};

inline VirtualAudioPin::VirtualAudioPin(VirtualAudioCaptureFilter* pFilter) : m_filter(pFilter) {
    g_vcamComRefCount.fetch_add(1, std::memory_order_relaxed);
}
inline VirtualAudioPin::~VirtualAudioPin() {
    StopStreaming();
    Disconnect();
    g_vcamComRefCount.fetch_sub(1, std::memory_order_relaxed);
}
inline void VirtualAudioPin::StartStreaming() {
    std::lock_guard<std::mutex> lock(m_pinMutex);
    if (!m_memInputPin || m_streaming.load()) return;
    if (m_allocator) m_allocator->Commit();
    m_streaming.store(true);
    m_streamThread = std::thread(&VirtualAudioPin::workerLoop, this);
}
inline void VirtualAudioPin::StopStreaming() {
    m_streaming.store(false);
    if (m_streamThread.joinable()) {
        m_streamThread.join();
    }
    std::lock_guard<std::mutex> lock(m_pinMutex);
    if (m_allocator) m_allocator->Decommit();
}
inline STDMETHODIMP VirtualAudioPin::QueryPinInfo(PIN_INFO* pInfo) {
    if (!pInfo) return E_POINTER;
    pInfo->pFilter = static_cast<IBaseFilter*>(m_filter);
    if (pInfo->pFilter) pInfo->pFilter->AddRef();
    pInfo->dir = PINDIR_OUTPUT;
    wcsncpy_s(pInfo->achName, L"Capture", sizeof(pInfo->achName) / sizeof(wchar_t));
    return S_OK;
}
inline STDMETHODIMP VirtualAudioPin::EnumMediaTypes(IEnumMediaTypes** ppEnum) {
    if (!ppEnum) return E_POINTER;
    *ppEnum = new AudioEnumMediaTypes(this);
    return S_OK;
}
