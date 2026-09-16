#pragma once
#include <windows.h>
#include <dshow.h>
#include <mmreg.h>
#include <atomic>
#include "mf_shared_mem.hpp"

// Внешнее объявление GUID виртуального микрофона
extern const GUID CLSID_VirtualCamNativeMic;



class VirtualAudioCaptureFilter : public IBaseFilter, public IAMStreamConfig {
public:
    VirtualAudioCaptureFilter() : m_refCount(1), m_state(State_Stopped), m_hMap(nullptr), m_shmHeader(nullptr) {
        m_hMap = OpenFileMappingA(FILE_MAP_READ, FALSE, MF_AUDIO_MEM_NAME);
        if (m_hMap) {
            m_shmHeader = reinterpret_cast<MFAudioSharedHeader*>(MapViewOfFile(m_hMap, FILE_MAP_READ, 0, 0, sizeof(MFAudioSharedHeader) + MF_AUDIO_BUFFER_SIZE));
        }
    }

    virtual ~VirtualAudioCaptureFilter() {
        if (m_shmHeader) UnmapViewOfFile(m_shmHeader);
        if (m_hMap) CloseHandle(m_hMap);
    }

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IBaseFilter || riid == IID_IMediaFilter) {
            *ppv = static_cast<IBaseFilter*>(this);
        }
        else if (riid == IID_IAMStreamConfig) {
            *ppv = static_cast<IAMStreamConfig*>(this);
        }
        else {
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
    STDMETHODIMP Stop() override { m_state = State_Stopped; return S_OK; }
    STDMETHODIMP Pause() override { m_state = State_Paused; return S_OK; }
    STDMETHODIMP Run(REFERENCE_TIME) override { m_state = State_Running; return S_OK; }
    STDMETHODIMP GetState(DWORD, FILTER_STATE* pState) override {
        if (!pState) return E_POINTER;
        *pState = m_state;
        return S_OK;
    }
    STDMETHODIMP SetSyncSource(IReferenceClock*) override { return S_OK; }
    STDMETHODIMP GetSyncSource(IReferenceClock** ppClock) override { if (ppClock) *ppClock = nullptr; return S_OK; }

    // IBaseFilter
    STDMETHODIMP EnumPins(IEnumPins** ppEnum) override { if (ppEnum) *ppEnum = nullptr; return E_NOTIMPL; }
    STDMETHODIMP FindPin(LPCWSTR, IPin** ppPin) override { if (ppPin) *ppPin = nullptr; return E_NOTIMPL; }
    STDMETHODIMP QueryFilterInfo(FILTER_INFO* pInfo) override {
        if (!pInfo) return E_POINTER;
        wcsncpy_s(pInfo->achName, L"VirtualCam Native Microphone", sizeof(pInfo->achName) / sizeof(wchar_t));
        pInfo->pGraph = nullptr;
        return S_OK;
    }
    STDMETHODIMP JoinFilterGraph(IFilterGraph*, LPCWSTR) override { return S_OK; }
    STDMETHODIMP QueryVendorInfo(LPWSTR* ppVendorInfo) override { if (ppVendorInfo) *ppVendorInfo = nullptr; return E_NOTIMPL; }

    // IAMStreamConfig
    STDMETHODIMP SetFormat(AM_MEDIA_TYPE*) override { return S_OK; }
    STDMETHODIMP GetFormat(AM_MEDIA_TYPE** ppmt) override {
        if (!ppmt) return E_POINTER;
        auto* pmt = reinterpret_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
        memset(pmt, 0, sizeof(AM_MEDIA_TYPE));
        pmt->majortype = MEDIATYPE_Audio;
        pmt->subtype = MEDIASUBTYPE_PCM;
        pmt->formattype = FORMAT_WaveFormatEx;
        pmt->cbFormat = sizeof(WAVEFORMATEX);
        auto* pwfx = reinterpret_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
        pwfx->wFormatTag = WAVE_FORMAT_PCM;
        pwfx->nChannels = 1;
        pwfx->nSamplesPerSec = 48000;
        pwfx->wBitsPerSample = 16;
        pwfx->nBlockAlign = 2;
        pwfx->nAvgBytesPerSec = 96000;
        pwfx->cbSize = 0;
        pmt->pbFormat = reinterpret_cast<BYTE*>(pwfx);
        *ppmt = pmt;
        return S_OK;
    }
    STDMETHODIMP GetNumberOfCapabilities(int* piCount, int* piSize) override {
        if (piCount) *piCount = 1;
        if (piSize) *piSize = sizeof(AUDIO_STREAM_CONFIG_CAPS);
        return S_OK;
    }
    STDMETHODIMP GetStreamCaps(int, AM_MEDIA_TYPE** ppmt, BYTE*) override {
        return GetFormat(ppmt);
    }

private:
    std::atomic<ULONG> m_refCount;
    FILTER_STATE m_state;
    HANDLE m_hMap;
    MFAudioSharedHeader* m_shmHeader;
};