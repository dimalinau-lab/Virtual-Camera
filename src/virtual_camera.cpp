#include "virtual_camera.hpp"

// =============================================================
// VirtualCamMediaStream
// =============================================================
VirtualCamMediaStream::VirtualCamMediaStream() {}

VirtualCamMediaStream::~VirtualCamMediaStream() {
    Shutdown();
}

HRESULT VirtualCamMediaStream::Initialize(VirtualCamMediaSource* pSource, IMFStreamDescriptor* pSD) {
    if (!pSource || !pSD) return E_POINTER;
    m_source = pSource;
    m_streamDescriptor = pSD;

    // Подключаемся к общей памяти C++ стримера
    m_hSharedMem = OpenFileMappingA(FILE_MAP_READ, FALSE, MF_VCAM_MEM_NAME);
    if (m_hSharedMem) {
        m_pSharedBuffer = (uint8_t*)MapViewOfFile(m_hSharedMem, FILE_MAP_READ, 0, 0, 0);
    }

    return MFCreateEventQueue(&m_eventQueue);
}

STDMETHODIMP VirtualCamMediaStream::GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent) {
    if (m_isShutdown) return MF_E_SHUTDOWN;
    return m_eventQueue->GetEvent(dwFlags, ppEvent);
}

STDMETHODIMP VirtualCamMediaStream::BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState) {
    if (m_isShutdown) return MF_E_SHUTDOWN;
    return m_eventQueue->BeginGetEvent(pCallback, punkState);
}

STDMETHODIMP VirtualCamMediaStream::EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent) {
    if (m_isShutdown) return MF_E_SHUTDOWN;
    return m_eventQueue->EndGetEvent(pResult, ppEvent);
}

STDMETHODIMP VirtualCamMediaStream::QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue) {
    if (m_isShutdown) return MF_E_SHUTDOWN;
    return m_eventQueue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue);
}

STDMETHODIMP VirtualCamMediaStream::GetMediaSource(IMFMediaSource** ppMediaSource) {
    if (!ppMediaSource) return E_POINTER;
    if (m_isShutdown) return MF_E_SHUTDOWN;
    return m_source.CopyTo(ppMediaSource);
}

STDMETHODIMP VirtualCamMediaStream::GetStreamDescriptor(IMFStreamDescriptor** ppStreamDescriptor) {
    if (!ppStreamDescriptor) return E_POINTER;
    if (m_isShutdown) return MF_E_SHUTDOWN;
    return m_streamDescriptor.CopyTo(ppStreamDescriptor);
}

STDMETHODIMP VirtualCamMediaStream::RequestSample(IUnknown* pToken) {
    if (m_isShutdown) return MF_E_SHUTDOWN;

    UINT32 targetW = 1280;
    UINT32 targetH = 720;
    UINT64 durationHns = 333333; // 30 fps default

    ComPtr<IMFMediaTypeHandler> pHandler;
    if (m_streamDescriptor && SUCCEEDED(m_streamDescriptor->GetMediaTypeHandler(&pHandler))) {
        ComPtr<IMFMediaType> pCurType;
        if (SUCCEEDED(pHandler->GetCurrentMediaType(&pCurType))) {
            MFGetAttributeSize(pCurType.Get(), MF_MT_FRAME_SIZE, &targetW, &targetH);
            UINT32 num = 30, den = 1;
            if (SUCCEEDED(MFGetAttributeRatio(pCurType.Get(), MF_MT_FRAME_RATE, &num, &den)) && num > 0) {
                durationHns = (10000000ULL * den) / num;
            }
        }
    }

    const DWORD frameSize = targetW * targetH * 3 / 2; // NV12
    ComPtr<IMFMediaBuffer> pBuffer;
    HRESULT hr = MFCreateMemoryBuffer(frameSize, &pBuffer);
    if (FAILED(hr)) return hr;

    BYTE* pDst = nullptr;
    hr = pBuffer->Lock(&pDst, nullptr, nullptr);
    if (SUCCEEDED(hr)) {
        // Если общая память ещё не была открыта, пытаемся подключиться
        if (!m_pSharedBuffer) {
            m_hSharedMem = OpenFileMappingA(FILE_MAP_READ, FALSE, MF_VCAM_MEM_NAME);
            if (m_hSharedMem) {
                m_pSharedBuffer = (uint8_t*)MapViewOfFile(m_hSharedMem, FILE_MAP_READ, 0, 0, 0);
            }
        }

        bool frameCopied = false;
        if (m_pSharedBuffer) {
            auto* hdr = reinterpret_cast<MFVirtualCamHeader*>(m_pSharedBuffer);
            hdr->wantWidth = targetW;
            hdr->wantHeight = targetH;
            uint64_t now = GetTickCount64();
            bool isAlive = (hdr->writerTick != 0 && (now - hdr->writerTick) <= 3000);

            if (isAlive && hdr->magic == 0x4D465643) {
                const BYTE* pSrc = m_pSharedBuffer + sizeof(MFVirtualCamHeader);
                for (int attempt = 0; attempt < 4; attempt++) {
                    int32_t s1 = hdr->seq;
                    if (s1 & 1) { // Запись в процессе
                        Sleep(1);
                        continue;
                    }
                    MemoryBarrier();
                    if (hdr->width == targetW && hdr->height == targetH) {
                        memcpy(pDst, pSrc, frameSize);
                    } else {
                        uint32_t copyW = (targetW < hdr->width) ? targetW : hdr->width;
                        uint32_t copyH = (targetH < hdr->height) ? targetH : hdr->height;
                        memset(pDst, 0x10, (size_t)targetW * targetH);
                        memset(pDst + (size_t)targetW * targetH, 0x80, (size_t)targetW * targetH / 2);
                        for (uint32_t y = 0; y < copyH; ++y) {
                            memcpy(pDst + y * targetW, pSrc + y * hdr->width, copyW);
                        }
                        for (uint32_t y = 0; y < copyH / 2; ++y) {
                            memcpy(pDst + (size_t)targetW * targetH + y * targetW,
                                   pSrc + (size_t)hdr->width * hdr->height + y * hdr->width, copyW);
                        }
                    }
                    MemoryBarrier();
                    if (hdr->seq == s1) {
                        frameCopied = true;
                        break;
                    }
                }
            }
        }

        if (!frameCopied) {
            // Элегантный брендовый placeholder: темный фон (Y=24) с полосой статуса
            memset(pDst, 0x18, (size_t)targetW * targetH);
            uint32_t barYStart = (targetH > 30) ? (targetH / 2 - 12) : 0;
            uint32_t barYEnd = (targetH > 30) ? (targetH / 2 + 12) : targetH;
            uint32_t barXStart = targetW / 4;
            uint32_t barXEnd = targetW * 3 / 4;
            for (uint32_t y = barYStart; y < barYEnd; ++y) {
                memset(pDst + y * targetW + barXStart, 0x85, barXEnd - barXStart);
            }
            memset(pDst + (size_t)targetW * targetH, 0x80, (size_t)targetW * targetH / 2);
        }

        pBuffer->Unlock();
        pBuffer->SetCurrentLength(frameSize);
    }

    ComPtr<IMFSample> pSample;
    hr = MFCreateSample(&pSample);
    if (FAILED(hr)) return hr;

    pSample->AddBuffer(pBuffer.Get());
    pSample->SetSampleTime(m_sampleTime);
    pSample->SetSampleDuration(durationHns);
    m_sampleTime += durationHns;

    if (pToken) {
        pSample->SetUnknown(MFSampleExtension_Token, pToken);
    }

    return m_eventQueue->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, pSample.Get());
}

HRESULT VirtualCamMediaStream::Start() {
    if (m_isShutdown) return MF_E_SHUTDOWN;
    m_sampleTime = 0;
    return QueueEvent(MEStreamStarted, GUID_NULL, S_OK, nullptr);
}

HRESULT VirtualCamMediaStream::Stop() {
    if (m_isShutdown) return MF_E_SHUTDOWN;
    return QueueEvent(MEStreamStopped, GUID_NULL, S_OK, nullptr);
}

HRESULT VirtualCamMediaStream::Shutdown() {
    m_isShutdown = true;
    if (m_pSharedBuffer) {
        UnmapViewOfFile(m_pSharedBuffer);
        m_pSharedBuffer = nullptr;
    }
    if (m_hSharedMem) {
        CloseHandle(m_hSharedMem);
        m_hSharedMem = nullptr;
    }
    if (m_eventQueue) {
        m_eventQueue->Shutdown();
        m_eventQueue.Reset();
    }
    m_streamDescriptor.Reset();
    m_source.Reset();
    return S_OK;
}

// =============================================================
// VirtualCamMediaSource
// =============================================================
VirtualCamMediaSource::VirtualCamMediaSource() {}

VirtualCamMediaSource::~VirtualCamMediaSource() {
    Shutdown();
}

HRESULT VirtualCamMediaSource::Initialize() {
    HRESULT hr = MFCreateEventQueue(&m_eventQueue);
    if (FAILED(hr)) return hr;

    // Конфигурируем список доступных разрешений: 1080p (60/30fps), 720p (60/30fps)
    struct ResSpec { UINT32 w, h, fps; };
    const ResSpec resList[4] = {
        { 1920, 1080, 60 },
        { 1920, 1080, 30 },
        { 1280, 720,  60 },
        { 1280, 720,  30 }
    };

    ComPtr<IMFMediaType> pTypes[4];
    IMFMediaType* rawTypes[4] = { nullptr };

    for (int i = 0; i < 4; ++i) {
        hr = MFCreateMediaType(&pTypes[i]);
        if (FAILED(hr)) return hr;
        pTypes[i]->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        pTypes[i]->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        MFSetAttributeSize(pTypes[i].Get(), MF_MT_FRAME_SIZE, resList[i].w, resList[i].h);
        MFSetAttributeRatio(pTypes[i].Get(), MF_MT_FRAME_RATE, resList[i].fps, 1);
        MFSetAttributeRatio(pTypes[i].Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        rawTypes[i] = pTypes[i].Get();
    }

    ComPtr<IMFStreamDescriptor> pSD;
    hr = MFCreateStreamDescriptor(0, 4, rawTypes, &pSD);
    if (FAILED(hr)) return hr;

    ComPtr<IMFMediaTypeHandler> pHandler;
    if (SUCCEEDED(pSD->GetMediaTypeHandler(&pHandler))) {
        // По умолчанию активируем 1080p 60fps или 1080p 30fps
        pHandler->SetCurrentMediaType(pTypes[1].Get());
    }

    IMFStreamDescriptor* streamDescriptors[1] = { pSD.Get() };
    hr = MFCreatePresentationDescriptor(1, streamDescriptors, &m_presDescriptor);
    if (FAILED(hr)) return hr;

    m_presDescriptor->SelectStream(0);

    m_stream = Microsoft::WRL::Make<VirtualCamMediaStream>();
    return m_stream->Initialize(this, pSD.Get());
}

STDMETHODIMP VirtualCamMediaSource::GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent) {
    if (m_isShutdown) return MF_E_SHUTDOWN;
    return m_eventQueue->GetEvent(dwFlags, ppEvent);
}

STDMETHODIMP VirtualCamMediaSource::BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState) {
    if (m_isShutdown) return MF_E_SHUTDOWN;
    return m_eventQueue->BeginGetEvent(pCallback, punkState);
}

STDMETHODIMP VirtualCamMediaSource::EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent) {
    if (m_isShutdown) return MF_E_SHUTDOWN;
    return m_eventQueue->EndGetEvent(pResult, ppEvent);
}

STDMETHODIMP VirtualCamMediaSource::QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue) {
    if (m_isShutdown) return MF_E_SHUTDOWN;
    return m_eventQueue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue);
}

STDMETHODIMP VirtualCamMediaSource::GetCharacteristics(DWORD* pdwCharacteristics) {
    if (!pdwCharacteristics) return E_POINTER;
    if (m_isShutdown) return MF_E_SHUTDOWN;
    *pdwCharacteristics = MFMEDIASOURCE_IS_LIVE;
    return S_OK;
}

STDMETHODIMP VirtualCamMediaSource::CreatePresentationDescriptor(IMFPresentationDescriptor** ppPresentationDescriptor) {
    if (!ppPresentationDescriptor) return E_POINTER;
    if (m_isShutdown) return MF_E_SHUTDOWN;
    if (!m_presDescriptor) return E_UNEXPECTED;
    return m_presDescriptor->Clone(ppPresentationDescriptor);
}

STDMETHODIMP VirtualCamMediaSource::Start(IMFPresentationDescriptor* pPresentationDescriptor, const GUID* pguidTimeFormat, const PROPVARIANT* pvarStartPosition) {
    if (m_isShutdown) return MF_E_SHUTDOWN;
    if (m_stream) m_stream->Start();
    return QueueEvent(MESourceStarted, GUID_NULL, S_OK, nullptr);
}

STDMETHODIMP VirtualCamMediaSource::Stop() {
    if (m_isShutdown) return MF_E_SHUTDOWN;
    if (m_stream) m_stream->Stop();
    return QueueEvent(MESourceStopped, GUID_NULL, S_OK, nullptr);
}

STDMETHODIMP VirtualCamMediaSource::Pause() {
    return MF_E_INVALID_STATE_TRANSITION;
}

STDMETHODIMP VirtualCamMediaSource::Shutdown() {
    m_isShutdown = true;
    if (m_stream) {
        m_stream->Shutdown();
        m_stream.Reset();
    }
    if (m_eventQueue) {
        m_eventQueue->Shutdown();
        m_eventQueue.Reset();
    }
    m_presDescriptor.Reset();
    return S_OK;
}