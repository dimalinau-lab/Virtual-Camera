#pragma once

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <vector>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <iostream>
#include <algorithm>

struct WasapiDeviceInfo {
    std::string id;       // Device ID string ("phone", "default", or endpoint GUID)
    std::string name;     // Friendly Name (e.g. "Microphone (Realtek Audio)")
    bool isDefault{ false };
};

class WasapiCaptureClient {
public:
    WasapiCaptureClient() {
        m_ringBuffer.assign(48000, 0);
    }
    ~WasapiCaptureClient() {
        stop();
    }

    static std::vector<WasapiDeviceInfo> enumerateDevices() {
        std::vector<WasapiDeviceInfo> list;
        // Phone audio stream
        list.push_back({ "phone", "📱 Phone Microphone (Android Stream)", false });
        // Windows Default communication/recording device
        list.push_back({ "default", "🎙️ Default Windows Microphone", true });

        IMMDeviceEnumerator* pEnum = nullptr;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator), (void**)&pEnum);
        if (FAILED(hr) || !pEnum) return list;

        IMMDevice* pDefDev = nullptr;
        std::wstring defaultId;
        if (SUCCEEDED(pEnum->GetDefaultAudioEndpoint(eCapture, eConsole, &pDefDev)) && pDefDev) {
            LPWSTR pId = nullptr;
            if (SUCCEEDED(pDefDev->GetId(&pId)) && pId) {
                defaultId = pId;
                CoTaskMemFree(pId);
            }
            pDefDev->Release();
        }

        IMMDeviceCollection* pCol = nullptr;
        if (SUCCEEDED(pEnum->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &pCol)) && pCol) {
            UINT count = 0;
            pCol->GetCount(&count);
            for (UINT i = 0; i < count; ++i) {
                IMMDevice* pDev = nullptr;
                if (SUCCEEDED(pCol->Item(i, &pDev)) && pDev) {
                    LPWSTR pId = nullptr;
                    std::string idStr;
                    bool isDef = false;
                    if (SUCCEEDED(pDev->GetId(&pId)) && pId) {
                        if (defaultId == pId) isDef = true;
                        int len = WideCharToMultiByte(CP_UTF8, 0, pId, -1, nullptr, 0, nullptr, nullptr);
                        if (len > 0) {
                            idStr.resize(len - 1);
                            WideCharToMultiByte(CP_UTF8, 0, pId, -1, &idStr[0], len, nullptr, nullptr);
                        }
                        CoTaskMemFree(pId);
                    }

                    std::string nameStr;
                    IPropertyStore* pStore = nullptr;
                    if (SUCCEEDED(pDev->OpenPropertyStore(STGM_READ, &pStore)) && pStore) {
                        PROPVARIANT varName;
                        PropVariantInit(&varName);
                        if (SUCCEEDED(pStore->GetValue(PKEY_Device_FriendlyName, &varName)) && varName.pwszVal) {
                            int len = WideCharToMultiByte(CP_UTF8, 0, varName.pwszVal, -1, nullptr, 0, nullptr, nullptr);
                            if (len > 0) {
                                nameStr.resize(len - 1);
                                WideCharToMultiByte(CP_UTF8, 0, varName.pwszVal, -1, &nameStr[0], len, nullptr, nullptr);
                            }
                        }
                        PropVariantClear(&varName);
                        pStore->Release();
                    }

                    if (!idStr.empty() && !nameStr.empty()) {
                        list.push_back({ idStr, nameStr, isDef });
                    }
                    pDev->Release();
                }
            }
            pCol->Release();
        }
        pEnum->Release();
        return list;
    }

    bool start(const std::string& deviceId) {
        std::lock_guard<std::mutex> lock(m_lifecycleMutex);
        if (m_isRunning.load() && m_targetDeviceId == deviceId && m_thread.joinable()) {
            return true;
        }
        stopInternal();
        m_targetDeviceId = deviceId;
        m_isRunning = true;
        m_thread = std::thread(&WasapiCaptureClient::captureWorker, this);
        return true;
    }

    void stop() {
        std::lock_guard<std::mutex> lock(m_lifecycleMutex);
        stopInternal();
    }

    bool isRunning() const {
        return m_isRunning.load();
    }

    // Read available 48kHz 16-bit mono samples into buffer. Returns number of samples read.
    size_t readSamples(int16_t* outSamples, size_t maxCount) {
        std::lock_guard<std::mutex> lock(m_ringMutex);
        if (m_ringBuffer.empty()) return 0;

        size_t available = (m_writePos >= m_readPos) ? (m_writePos - m_readPos) 
                                                     : (m_ringBuffer.size() - m_readPos + m_writePos);
        size_t toRead = (std::min)(available, maxCount);
        for (size_t i = 0; i < toRead; ++i) {
            outSamples[i] = m_ringBuffer[m_readPos];
            m_readPos = (m_readPos + 1) % m_ringBuffer.size();
        }
        return toRead;
    }

private:
    void stopInternal() {
        m_isRunning = false;
        if (m_thread.joinable()) {
            if (m_thread.get_id() != std::this_thread::get_id()) {
                m_thread.join();
            }
        }
        cleanup();
    }

    void cleanup() {
        if (m_audioClient) {
            m_audioClient->Stop();
            m_audioClient->Release();
            m_audioClient = nullptr;
        }
        if (m_captureClient) {
            m_captureClient->Release();
            m_captureClient = nullptr;
        }
        if (m_device) {
            m_device->Release();
            m_device = nullptr;
        }
        if (m_pwfx) {
            CoTaskMemFree(m_pwfx);
            m_pwfx = nullptr;
        }
    }

    void captureWorker() {
        try {
            HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            IMMDeviceEnumerator* pEnum = nullptr;
            hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                __uuidof(IMMDeviceEnumerator), (void**)&pEnum);
            if (FAILED(hr) || !pEnum) {
                CoUninitialize();
                return;
            }

            if (m_targetDeviceId == "default" || m_targetDeviceId.empty()) {
                hr = pEnum->GetDefaultAudioEndpoint(eCapture, eConsole, &m_device);
            } else {
                int wlen = MultiByteToWideChar(CP_UTF8, 0, m_targetDeviceId.c_str(), -1, nullptr, 0);
                if (wlen > 0) {
                    std::wstring wId(wlen, 0);
                    MultiByteToWideChar(CP_UTF8, 0, m_targetDeviceId.c_str(), -1, &wId[0], wlen);
                    hr = pEnum->GetDevice(wId.c_str(), &m_device);
                }
            }
            pEnum->Release();

            if (FAILED(hr) || !m_device) {
                CoUninitialize();
                return;
            }

            hr = m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&m_audioClient);
            if (FAILED(hr) || !m_audioClient) {
                cleanup();
                CoUninitialize();
                return;
            }

            hr = m_audioClient->GetMixFormat(&m_pwfx);
            if (FAILED(hr) || !m_pwfx) {
                cleanup();
                CoUninitialize();
                return;
            }

            REFERENCE_TIME hnsBuffer = 1000000; // 100ms
            hr = m_audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, hnsBuffer, 0, m_pwfx, nullptr);
            if (FAILED(hr)) {
                cleanup();
                CoUninitialize();
                return;
            }

            hr = m_audioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&m_captureClient);
            if (FAILED(hr) || !m_captureClient) {
                cleanup();
                CoUninitialize();
                return;
            }

            hr = m_audioClient->Start();
            if (FAILED(hr)) {
                cleanup();
                CoUninitialize();
                return;
            }

            // Initialize 1-second 48kHz mono ring buffer
            {
                std::lock_guard<std::mutex> lock(m_ringMutex);
                m_ringBuffer.assign(48000, 0);
                m_readPos = 0;
                m_writePos = 0;
            }

            const int devSampleRate = (m_pwfx->nSamplesPerSec > 0) ? m_pwfx->nSamplesPerSec : 48000;
            const int channels = (m_pwfx->nChannels > 0) ? m_pwfx->nChannels : 1;
            const bool isFloat = (m_pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
                (m_pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                 reinterpret_cast<WAVEFORMATEXTENSIBLE*>(m_pwfx)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);

            std::vector<float> resampleIn;
            double resamplePhase = 0.0;

            while (m_isRunning) {
                UINT32 packetLength = 0;
                hr = m_captureClient->GetNextPacketSize(&packetLength);
                if (FAILED(hr) || packetLength == 0) {
                    Sleep(5);
                    continue;
                }

                BYTE* pData = nullptr;
                UINT32 numFramesRead = 0;
                DWORD flags = 0;
                hr = m_captureClient->GetBuffer(&pData, &numFramesRead, &flags, nullptr, nullptr);
                if (SUCCEEDED(hr) && pData && numFramesRead > 0) {
                    // 1. Extract frames to mono float in [-1.0 .. 1.0]
                    resampleIn.resize(numFramesRead);
                    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                        std::fill(resampleIn.begin(), resampleIn.end(), 0.0f);
                    } else if (isFloat) {
                        const float* fData = reinterpret_cast<const float*>(pData);
                        for (UINT32 f = 0; f < numFramesRead; ++f) {
                            float sum = 0.0f;
                            for (int c = 0; c < channels; ++c) {
                                sum += fData[f * channels + c];
                            }
                            resampleIn[f] = sum / (float)channels;
                        }
                    } else {
                        const int16_t* sData = reinterpret_cast<const int16_t*>(pData);
                        for (UINT32 f = 0; f < numFramesRead; ++f) {
                            float sum = 0.0f;
                            for (int c = 0; c < channels; ++c) {
                                sum += (float)sData[f * channels + c] / 32768.0f;
                            }
                            resampleIn[f] = sum / (float)channels;
                        }
                    }

                    // 2. Resample / transfer to 48000 Hz int16_t and push to ring
                    {
                        std::lock_guard<std::mutex> lock(m_ringMutex);
                        if (!m_ringBuffer.empty()) {
                            if (devSampleRate == 48000) {
                                for (UINT32 f = 0; f < numFramesRead; ++f) {
                                    int16_t s16 = static_cast<int16_t>((std::clamp)(resampleIn[f] * 32767.0f, -32768.0f, 32767.0f));
                                    m_ringBuffer[m_writePos] = s16;
                                    m_writePos = (m_writePos + 1) % m_ringBuffer.size();
                                }
                            } else {
                                const double ratio = (double)devSampleRate / 48000.0;
                                while (resamplePhase < numFramesRead) {
                                    size_t idx0 = (size_t)resamplePhase;
                                    size_t idx1 = (std::min)(idx0 + 1, (size_t)numFramesRead - 1);
                                    double frac = resamplePhase - idx0;
                                    float val = static_cast<float>((1.0 - frac) * resampleIn[idx0] + frac * resampleIn[idx1]);
                                    int16_t s16 = static_cast<int16_t>((std::clamp)(val * 32767.0f, -32768.0f, 32767.0f));

                                    m_ringBuffer[m_writePos] = s16;
                                    m_writePos = (m_writePos + 1) % m_ringBuffer.size();

                                    resamplePhase += ratio;
                                }
                                resamplePhase -= numFramesRead;
                            }
                        }
                    }

                    m_captureClient->ReleaseBuffer(numFramesRead);
                }
            }

            cleanup();
            CoUninitialize();
        } catch (...) {
            cleanup();
            CoUninitialize();
        }
    }

    std::string m_targetDeviceId;
    std::atomic<bool> m_isRunning{ false };
    std::thread m_thread;

    IMMDevice* m_device{ nullptr };
    IAudioClient* m_audioClient{ nullptr };
    IAudioCaptureClient* m_captureClient{ nullptr };
    WAVEFORMATEX* m_pwfx{ nullptr };

    std::vector<int16_t> m_ringBuffer;
    size_t m_writePos{ 0 };
    size_t m_readPos{ 0 };
    std::mutex m_ringMutex;
    std::mutex m_lifecycleMutex;
};
