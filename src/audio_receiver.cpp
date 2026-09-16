#include "audio_receiver.hpp"
#include "mf_shared_mem.hpp"
#include <iostream>
#include <algorithm>
#include <functiondiscoverykeys_devpkey.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")

static HANDLE g_hAudioShm = nullptr;
static MFAudioSharedHeader* g_audioShm = nullptr;
static uint8_t* g_audioRingBuffer = nullptr;

static bool initAudioSharedMem() {
    if (g_audioShm) return true;
    size_t totalSize = sizeof(MFAudioSharedHeader) + MF_AUDIO_BUFFER_SIZE;
    g_hAudioShm = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, (DWORD)totalSize, MF_AUDIO_MEM_NAME);
    if (!g_hAudioShm) return false;

    g_audioShm = reinterpret_cast<MFAudioSharedHeader*>(MapViewOfFile(g_hAudioShm, FILE_MAP_ALL_ACCESS, 0, 0, totalSize));
    if (!g_audioShm) return false;

    g_audioShm->magic = 0x4D464155;
    g_audioShm->sampleRate = 48000;
    g_audioShm->channels = 1;
    g_audioShm->bitsPerSample = 16;
    g_audioShm->writePos = 0;
    g_audioShm->readPos = 0;
    g_audioRingBuffer = reinterpret_cast<uint8_t*>(g_audioShm + 1);
    return true;
}

AudioReceiver::AudioReceiver() {}

AudioReceiver::~AudioReceiver() {
    stop();
}

void AudioReceiver::setVolume(float vol) {
    m_volume.store(std::clamp(vol, 0.0f, 2.0f));
}

void AudioReceiver::setMute(bool muted) {
    m_isMuted.store(muted);
}

bool AudioReceiver::initWasapi() {
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&m_deviceEnumerator);
    if (FAILED(hr)) return false;

    IMMDeviceCollection* pCollection = nullptr;
    hr = m_deviceEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection);
    if (FAILED(hr)) return false;

    UINT count = 0;
    pCollection->GetCount(&count);

    for (UINT i = 0; i < count; ++i) {
        IMMDevice* pDevice = nullptr;
        pCollection->Item(i, &pDevice);
        if (!pDevice) continue;

        IPropertyStore* pStore = nullptr;
        if (SUCCEEDED(pDevice->OpenPropertyStore(STGM_READ, &pStore))) {
            PROPVARIANT varName;
            PropVariantInit(&varName);
            if (SUCCEEDED(pStore->GetValue(PKEY_Device_FriendlyName, &varName))) {
                if (varName.pwszVal && wcsstr(varName.pwszVal, L"CABLE Input") != nullptr) {
                    m_cableDevice = pDevice;
                    m_cableDevice->AddRef();
                    PropVariantClear(&varName);
                    pStore->Release();
                    pDevice->Release();
                    break;
                }
                PropVariantClear(&varName);
            }
            pStore->Release();
        }
        pDevice->Release();
    }
    pCollection->Release();

    if (!m_cableDevice) {
        std::cout << "[AUDIO] CABLE Input не найден, звук идет только в shared memory.\n";
        return false;
    }

    hr = m_cableDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&m_audioClient);
    if (FAILED(hr)) return false;

    hr = m_audioClient->GetMixFormat(&m_pwfx);
    if (FAILED(hr)) return false;

    // Увеличиваем буфер до 100 мс для защиты от треска при задержках
    REFERENCE_TIME hnsBufferDuration = 1000000;
    hr = m_audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, hnsBufferDuration, 0, m_pwfx, nullptr);
    if (FAILED(hr)) return false;

    hr = m_audioClient->GetBufferSize(&m_bufferFrameCount);
    if (FAILED(hr)) return false;

    hr = m_audioClient->GetService(__uuidof(IAudioRenderClient), (void**)&m_renderClient);
    if (FAILED(hr)) return false;

    hr = m_audioClient->Start();
    return SUCCEEDED(hr);
}

void AudioReceiver::cleanupWasapi() {
    if (m_audioClient) {
        m_audioClient->Stop();
        m_audioClient->Release();
        m_audioClient = nullptr;
    }
    if (m_renderClient) {
        m_renderClient->Release();
        m_renderClient = nullptr;
    }
    if (m_cableDevice) {
        m_cableDevice->Release();
        m_cableDevice = nullptr;
    }
    if (m_deviceEnumerator) {
        m_deviceEnumerator->Release();
        m_deviceEnumerator = nullptr;
    }
    if (m_pwfx) {
        CoTaskMemFree(m_pwfx);
        m_pwfx = nullptr;
    }
}

void AudioReceiver::playPcmChunk(const uint8_t* data, size_t size) {
    if (!m_audioClient || !m_renderClient || !m_pwfx || size == 0) return;

    UINT32 padding = 0;
    if (FAILED(m_audioClient->GetCurrentPadding(&padding))) return;

    UINT32 availableFrames = m_bufferFrameCount - padding;
    if (availableFrames == 0) return;

    const int16_t* srcSamples = reinterpret_cast<const int16_t*>(data);
    UINT32 inputFrames = static_cast<UINT32>(size / sizeof(int16_t));
    UINT32 framesToWrite = (std::min)(availableFrames, inputFrames);

    BYTE* pBuffer = nullptr;
    if (FAILED(m_renderClient->GetBuffer(framesToWrite, &pBuffer))) return;

    if (m_pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
        (m_pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
            reinterpret_cast<WAVEFORMATEXTENSIBLE*>(m_pwfx)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {

        float* dst = reinterpret_cast<float*>(pBuffer);
        for (UINT32 i = 0; i < framesToWrite; ++i) {
            float s = static_cast<float>(srcSamples[i]) / 32768.0f;
            for (WORD ch = 0; ch < m_pwfx->nChannels; ++ch) {
                *dst++ = s;
            }
        }
    }
    else {
        int16_t* dst = reinterpret_cast<int16_t*>(pBuffer);
        for (UINT32 i = 0; i < framesToWrite; ++i) {
            int16_t s = srcSamples[i];
            for (WORD ch = 0; ch < m_pwfx->nChannels; ++ch) {
                *dst++ = s;
            }
        }
    }

    m_renderClient->ReleaseBuffer(framesToWrite, 0);
}

bool AudioReceiver::start(const std::string& ip, int port) {
    stop();
    initAudioSharedMem();
    m_isRunning = true;
    m_workerThread = std::thread(&AudioReceiver::audioWorker, this, ip, port);
    return true;
}

void AudioReceiver::stop() {
    m_isRunning = false;
    if (m_socket != INVALID_SOCKET) {
        shutdown(m_socket, SD_BOTH);
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
}

void AudioReceiver::audioWorker(std::string ip, int port) {
    // Инициализация COM строго внутри этого потока
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    initWasapi();

    m_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_socket != INVALID_SOCKET) {
        int nodelay = 1;
        setsockopt(m_socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

        int rcvBuf = 64 * 1024;
        setsockopt(m_socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvBuf), sizeof(rcvBuf));

        DWORD timeout = 4000;
        setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<u_short>(port));
        inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

        if (connect(m_socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != SOCKET_ERROR) {
            std::cout << "[AUDIO] Поток микрофона подключен к сокету " << port << "!\n";
            std::vector<uint8_t> recvBuf(2048);

            while (m_isRunning) {
                int received = recv(m_socket, reinterpret_cast<char*>(recvBuf.data()), static_cast<int>(recvBuf.size()), 0);
                if (received <= 0) break;

                int16_t* samples = reinterpret_cast<int16_t*>(recvBuf.data());
                int sampleCount = received / static_cast<int>(sizeof(int16_t));
                float vol = m_volume.load();
                bool muted = m_isMuted.load();

                for (int i = 0; i < sampleCount; ++i) {
                    if (muted) {
                        samples[i] = 0;
                    }
                    else if (vol != 1.0f) {
                        int32_t s = static_cast<int32_t>(samples[i] * vol);
                        samples[i] = static_cast<int16_t>(std::clamp(s, -32768, 32767));
                    }
                }

                playPcmChunk(recvBuf.data(), static_cast<size_t>(received));

                if (g_audioShm && g_audioRingBuffer) {
                    uint32_t wPos = g_audioShm->writePos;
                    for (int i = 0; i < received; ++i) {
                        g_audioRingBuffer[(wPos + i) % MF_AUDIO_BUFFER_SIZE] = recvBuf[i];
                    }
                    g_audioShm->writePos = (wPos + received) % MF_AUDIO_BUFFER_SIZE;
                }
            }
        }
    }

    if (m_socket != INVALID_SOCKET) {
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }

    cleanupWasapi();
    CoUninitialize();
}  