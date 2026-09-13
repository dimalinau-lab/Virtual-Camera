#pragma once
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include "mf_shared_mem.hpp"

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;

// CLSID нашей виртуальной камеры: {E1D3B890-5F16-47D8-9C9D-9F0A3E8B81B1}
static const GUID CLSID_MFVirtualCamSource =
{ 0xe1d3b890, 0x5f16, 0x47d8, { 0x9c, 0x9d, 0x9f, 0xa, 0x3e, 0x8b, 0x81, 0xb1 } };

class VirtualCamMediaSource;

class VirtualCamMediaStream : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IMFMediaStream> {
public:
    VirtualCamMediaStream();
    ~VirtualCamMediaStream();

    HRESULT Initialize(VirtualCamMediaSource* pSource, IMFStreamDescriptor* pSD);

    // IMFMediaEventGenerator
    STDMETHODIMP GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent) override;
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent) override;
    STDMETHODIMP QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue) override;

    // IMFMediaStream
    STDMETHODIMP GetMediaSource(IMFMediaSource** ppMediaSource) override;
    STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** ppStreamDescriptor) override;
    STDMETHODIMP RequestSample(IUnknown* pToken) override;

    HRESULT Start();
    HRESULT Stop();
    HRESULT Shutdown();

private:
    ComPtr<IMFMediaEventQueue> m_eventQueue;
    ComPtr<IMFStreamDescriptor> m_streamDescriptor;
    ComPtr<IMFMediaSource> m_source;
    HANDLE m_hSharedMem = nullptr;
    uint8_t* m_pSharedBuffer = nullptr;
    LONGLONG m_sampleTime = 0;
    bool m_isShutdown = false;
};

class VirtualCamMediaSource : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IMFMediaSource> {
public:
    VirtualCamMediaSource();
    ~VirtualCamMediaSource();

    HRESULT Initialize();

    // IMFMediaEventGenerator
    STDMETHODIMP GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent) override;
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent) override;
    STDMETHODIMP QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue) override;

    // IMFMediaSource
    STDMETHODIMP GetCharacteristics(DWORD* pdwCharacteristics) override;
    STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** ppPresentationDescriptor) override;
    STDMETHODIMP Start(IMFPresentationDescriptor* pPresentationDescriptor, const GUID* pguidTimeFormat, const PROPVARIANT* pvarStartPosition) override;
    STDMETHODIMP Stop() override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Shutdown() override;

private:
    ComPtr<IMFMediaEventQueue> m_eventQueue;
    ComPtr<IMFPresentationDescriptor> m_presDescriptor;
    ComPtr<VirtualCamMediaStream> m_stream;
    bool m_isShutdown = false;
};