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

    const DWORD frameSize = 1280 * 720 * 3 / 2; // NV12
    ComPtr<IMFMediaBuffer> pBuffer;
    HRESULT hr = MFCreateMemoryBuffer(frameSize, &pBuffer);
    if (FAILED(hr)) return hr;

    BYTE* pDst = nullptr;
    hr = pBuffer->Lock(&pDst, nullptr, nullptr);
    if (SUCCEEDED(hr)) {
        // Если общая память ещё не была открыта, пытаемся подключиться повторно
        if (!m_pSharedBuffer) {
            m_hSharedMem = OpenFileMappingA(FILE_MAP_READ, FALSE, MF_VCAM_MEM_NAME);
            if (m_hSharedMem) {
                m_pSharedBuffer = (uint8_t*)MapViewOfFile(m_hSharedMem, FILE_MAP_READ, 0, 0, 0);
            }
        }

        if (m_pSharedBuffer) {
            BYTE* pSrc = m_pSharedBuffer + sizeof(MFVirtualCamHeader);
            memcpy(pDst, pSrc, frameSize);
        }
        else {
            // Тестовый черный фон NV12, если стример не запущен
            memset(pDst, 0x10, 1280 * 720);                     // Y
            memset(pDst + 1280 * 720, 0x80, 1280 * 720 / 2);     // UV
        }

        pBuffer->Unlock();
        pBuffer->SetCurrentLength(frameSize);
    }

    ComPtr<IMFSample> pSample;
    hr = MFCreateSample(&pSample);
    if (FAILED(hr)) return hr;

    pSample->AddBuffer(pBuffer.Get());
    pSample->SetSampleTime(m_sampleTime);
    pSample->SetSampleDuration(333333); // 30 кадров в секунду (в единицах по 100 нс)
    m_sampleTime += 333333;

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

    // Конфигурируем тип потока: 1280x720, NV12, 30 fps
    ComPtr<IMFMediaType> pMediaType;
    hr = MFCreateMediaType(&pMediaType);
    if (FAILED(hr)) return hr;

    pMediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    pMediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(pMediaType.Get(), MF_MT_FRAME_SIZE, 1280, 720);
    MFSetAttributeRatio(pMediaType.Get(), MF_MT_FRAME_RATE, 30, 1);
    MFSetAttributeRatio(pMediaType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    IMFMediaType* mediaTypes[1] = { pMediaType.Get() };
    ComPtr<IMFStreamDescriptor> pSD;
    hr = MFCreateStreamDescriptor(0, 1, mediaTypes, &pSD);
    if (FAILED(hr)) return hr;

    ComPtr<IMFMediaTypeHandler> pHandler;
    if (SUCCEEDED(pSD->GetMediaTypeHandler(&pHandler))) {
        pHandler->SetCurrentMediaType(pMediaType.Get());
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