#include "virtual_camera.hpp"

// =============================================================
// VirtualCamMediaStream
// =============================================================
VirtualCamMediaStream::VirtualCamMediaStream() {
    g_vcamComRefCount.fetch_add(1, std::memory_order_relaxed);
}

VirtualCamMediaStream::~VirtualCamMediaStream() {
    Shutdown();
    g_vcamComRefCount.fetch_sub(1, std::memory_order_relaxed);
}

bool VirtualCamMediaStream::openSharedMemory() {
    if (m_pSharedBuffer) return true;
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
    if (!m_hFrameEvent) {
        m_hFrameEvent = OpenEventA(SYNCHRONIZE, FALSE, MF_VCAM_EVENT_NAME);
    }
    return m_pSharedBuffer != nullptr;
}

void VirtualCamMediaStream::setReaderActive(bool active) {
    if (!m_pSharedBuffer || !m_isWritableView) return;
    auto* hdr = reinterpret_cast<MFVirtualCamHeader*>(m_pSharedBuffer);
    if (active && !m_countedReader) {
        InterlockedIncrement(&hdr->activeReaders);
        m_countedReader = true;
    } else if (!active && m_countedReader) {
        if (hdr->activeReaders > 0) {
            InterlockedDecrement(&hdr->activeReaders);
        }
        m_countedReader = false;
    }
}

HRESULT VirtualCamMediaStream::Initialize(VirtualCamMediaSource* pSource, IMFStreamDescriptor* pSD) {
    if (!pSource || !pSD) return E_POINTER;
    std::lock_guard<std::mutex> lock(m_streamMutex);
    m_source = pSource;
    m_streamDescriptor = pSD;
    openSharedMemory();
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
    UINT32 targetW = 1280;
    UINT32 targetH = 720;
    UINT64 durationHns = 333333;
    ComPtr<IMFMediaEventQueue> eventQueue;
    HANDLE hFrameEvent = nullptr;

    {
        std::lock_guard<std::mutex> lock(m_streamMutex);
        if (m_isShutdown) return MF_E_SHUTDOWN;
        eventQueue = m_eventQueue;
        openSharedMemory();
        hFrameEvent = m_hFrameEvent;

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
    }

    DWORD waitMs = static_cast<DWORD>((durationHns / 10000ULL));
    if (waitMs < 8) waitMs = 8;
    if (waitMs > 34) waitMs = 34;

    if (hFrameEvent) {
        WaitForSingleObject(hFrameEvent, waitMs);
    } else {
        Sleep(waitMs);
    }

    const DWORD frameSize = targetW * targetH * 3 / 2; // NV12
    ComPtr<IMFMediaBuffer> pBuffer;
    HRESULT hr = MFCreateMemoryBuffer(frameSize, &pBuffer);
    if (FAILED(hr)) return hr;

    BYTE* pDst = nullptr;
    hr = pBuffer->Lock(&pDst, nullptr, nullptr);
    if (SUCCEEDED(hr)) {
        bool frameCopied = false;
        {
            std::lock_guard<std::mutex> lock(m_streamMutex);
            if (m_isShutdown) {
                pBuffer->Unlock();
                return MF_E_SHUTDOWN;
            }
            if (openSharedMemory() && m_pSharedBuffer) {
                auto* hdr = reinterpret_cast<MFVirtualCamHeader*>(m_pSharedBuffer);
                if (m_isWritableView) {
                    hdr->wantWidth = targetW;
                    hdr->wantHeight = targetH;
                    setReaderActive(true);
                }
                uint64_t now = GetTickCount64();
                bool isAlive = (hdr->writerTick != 0 && (now - hdr->writerTick) <= 3000);

                if (isAlive && hdr->magic == 0x4D465643 && hdr->width > 0 && hdr->height > 0 &&
                    hdr->frameSize >= (uint64_t)hdr->width * hdr->height * 3 / 2) {
                    const BYTE* pSrc = m_pSharedBuffer + sizeof(MFVirtualCamHeader);
                    for (int attempt = 0; attempt < 4; attempt++) {
                        int32_t s1 = hdr->seq;
                        if (s1 & 1) {
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
                            m_lastFrameIndex = hdr->frameIndex;
                            frameCopied = true;
                            break;
                        }
                    }
                }
            }
        }

        if (!frameCopied) {
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
    {
        std::lock_guard<std::mutex> lock(m_streamMutex);
        if (m_isShutdown || !eventQueue) return MF_E_SHUTDOWN;
        pSample->SetSampleTime(m_sampleTime);
        pSample->SetSampleDuration(durationHns);
        m_sampleTime += durationHns;
    }

    if (pToken) {
        pSample->SetUnknown(MFSampleExtension_Token, pToken);
    }

    return eventQueue->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, pSample.Get());
}

HRESULT VirtualCamMediaStream::Start() {
    std::lock_guard<std::mutex> lock(m_streamMutex);
    if (m_isShutdown) return MF_E_SHUTDOWN;
    m_sampleTime = 0;
    openSharedMemory();
    setReaderActive(true);
    return QueueEvent(MEStreamStarted, GUID_NULL, S_OK, nullptr);
}

HRESULT VirtualCamMediaStream::Stop() {
    std::lock_guard<std::mutex> lock(m_streamMutex);
    if (m_isShutdown) return MF_E_SHUTDOWN;
    setReaderActive(false);
    return QueueEvent(MEStreamStopped, GUID_NULL, S_OK, nullptr);
}

HRESULT VirtualCamMediaStream::Shutdown() {
    std::lock_guard<std::mutex> lock(m_streamMutex);
    m_isShutdown = true;
    setReaderActive(false);
    m_isWritableView = false;
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
VirtualCamMediaSource::VirtualCamMediaSource() {
    g_vcamComRefCount.fetch_add(1, std::memory_order_relaxed);
}

VirtualCamMediaSource::~VirtualCamMediaSource() {
    Shutdown();
    g_vcamComRefCount.fetch_sub(1, std::memory_order_relaxed);
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
        pTypes[i]->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        pTypes[i]->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
        pTypes[i]->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
        pTypes[i]->SetUINT32(MF_MT_DEFAULT_STRIDE, resList[i].w);
        pTypes[i]->SetUINT32(MF_MT_SAMPLE_SIZE, resList[i].w * resList[i].h * 3 / 2);
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