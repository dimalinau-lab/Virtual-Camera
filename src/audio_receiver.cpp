#include "audio_receiver.hpp"
#include "mf_shared_mem.hpp"
#include "audio_dsp.hpp"
#include "app_state.hpp"
#include <iostream>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <chrono>
#include "debug_logger.hpp"

inline void logAudioDebug(const std::string& msg) {
    logDebug(msg);
}

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

AudioReceiver::AudioReceiver() {
    m_delayRing.assign(48000, 0);
}

AudioReceiver::~AudioReceiver() {
    stop();
}

void AudioReceiver::setVolume(float vol) {
    m_volume.store((std::clamp)(vol, 0.0f, 2.0f));
}

void AudioReceiver::setMute(bool muted) {
    m_isMuted.store(muted);
}

void AudioReceiver::setDelayMs(int delayMs) {
    m_delayMs.store((std::clamp)(delayMs, 0, 500));
}

void AudioReceiver::setNoiseGate(bool enabled, float threshold) {
    m_noiseGateEnabled.store(enabled);
    m_noiseGateThreshold.store((std::clamp)(threshold, 0.001f, 0.2f));
}

void AudioReceiver::setAiNoise(bool enabled) {
    m_aiNoiseEnabled.store(enabled);
}

void AudioReceiver::setAgc(bool enabled) {
    m_agcEnabled.store(enabled);
}

void AudioReceiver::setEq(float lowDb, float midDb, float highDb) {
    m_eqLowDb.store(lowDb);
    m_eqMidDb.store(midDb);
    m_eqHighDb.store(highDb);
}

void AudioReceiver::setDeclicker(bool enabled) {
    m_declickerEnabled.store(enabled);
}

std::vector<WasapiDeviceInfo> AudioReceiver::getAudioDevices() {
    return WasapiCaptureClient::enumerateDevices();
}

void AudioReceiver::setAudioDevice(const std::string& deviceId) {
    g_audioInputDeviceId = deviceId;
    g_audioInputDeviceChanged.store(true);
}

std::string AudioReceiver::getCurrentAudioDevice() {
    return g_audioInputDeviceId;
}

void AudioReceiver::triggerGsmBurst() {
    m_gsmBurstTrigger.store(true);
}

bool AudioReceiver::initWasapi() {
    logAudioDebug("[AUDIO] initWasapi started");
    cleanupWasapi();

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&m_deviceEnumerator);
    if (FAILED(hr) || !m_deviceEnumerator) {
        logAudioDebug("[AUDIO] CoCreateInstance MMDeviceEnumerator failed");
        return false;
    }

    IMMDeviceCollection* pCollection = nullptr;
    hr = m_deviceEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pCollection);
    if (FAILED(hr) || !pCollection) {
        logAudioDebug("[AUDIO] EnumAudioEndpoints failed");
        cleanupWasapi();
        return false;
    }

    UINT count = 0;
    pCollection->GetCount(&count);
    logAudioDebug("[AUDIO] Active render devices count: " + std::to_string(count));

    for (UINT i = 0; i < count; ++i) {
        IMMDevice* pDevice = nullptr;
        if (FAILED(pCollection->Item(i, &pDevice)) || !pDevice) continue;

        IPropertyStore* pStore = nullptr;
        if (SUCCEEDED(pDevice->OpenPropertyStore(STGM_READ, &pStore)) && pStore) {
            PROPVARIANT varName;
            PropVariantInit(&varName);
            if (SUCCEEDED(pStore->GetValue(PKEY_Device_FriendlyName, &varName))) {
                if (varName.vt == VT_LPWSTR && varName.pwszVal) {
                    if (wcsstr(varName.pwszVal, L"CABLE Input") != nullptr ||
                        wcsstr(varName.pwszVal, L"VB-Audio") != nullptr ||
                        wcsstr(varName.pwszVal, L"VoiceMeeter") != nullptr ||
                        wcsstr(varName.pwszVal, L"Virtual Audio Cable") != nullptr) {
                        m_cableDevice = pDevice;
                        m_cableDevice->AddRef();
                        PropVariantClear(&varName);
                        pStore->Release();
                        pDevice->Release();
                        break;
                    }
                }
                PropVariantClear(&varName);
            }
            pStore->Release();
        }
        pDevice->Release();
    }
    pCollection->Release();

    if (!m_cableDevice) {
        logAudioDebug("[AUDIO] Virtual Cable (VB-Cable/VoiceMeeter) not detected. Microphone audio routed exclusively via DirectShow Virtual Mic (shared memory). Playback to headphones disabled.");
        cleanupWasapi();
        return false;
    }

    hr = m_cableDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&m_audioClient);
    if (FAILED(hr) || !m_audioClient) {
        logAudioDebug("[AUDIO] Activate IAudioClient failed");
        cleanupWasapi();
        return false;
    }

    hr = m_audioClient->GetMixFormat(&m_pwfx);
    if (FAILED(hr) || !m_pwfx) {
        logAudioDebug("[AUDIO] GetMixFormat failed");
        cleanupWasapi();
        return false;
    }

    REFERENCE_TIME hnsBufferDuration = 1000000;
    hr = m_audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, hnsBufferDuration, 0, m_pwfx, nullptr);
    if (FAILED(hr)) {
        logAudioDebug("[AUDIO] Initialize IAudioClient failed hr=" + std::to_string(hr));
        cleanupWasapi();
        return false;
    }

    hr = m_audioClient->GetBufferSize(&m_bufferFrameCount);
    if (FAILED(hr)) {
        cleanupWasapi();
        return false;
    }

    hr = m_audioClient->GetService(__uuidof(IAudioRenderClient), (void**)&m_renderClient);
    if (FAILED(hr) || !m_renderClient) {
        cleanupWasapi();
        return false;
    }

    hr = m_audioClient->Start();
    if (FAILED(hr)) {
        cleanupWasapi();
        return false;
    }
    logAudioDebug("[AUDIO] initWasapi completed successfully");
    return true;
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
    if (!m_renderClient || !m_pwfx) return;

    UINT32 numFramesPadding = 0;
    HRESULT hr = m_audioClient->GetCurrentPadding(&numFramesPadding);
    if (FAILED(hr)) return;

    UINT32 numFramesAvailable = m_bufferFrameCount - numFramesPadding;
    UINT32 bytesPerFrame = m_pwfx->nBlockAlign;
    UINT32 inFrames = static_cast<UINT32>(size / sizeof(int16_t));
    UINT32 framesToWrite = (std::min)(inFrames, numFramesAvailable);

    if (framesToWrite == 0) return;

    BYTE* pBuffer = nullptr;
    hr = m_renderClient->GetBuffer(framesToWrite, &pBuffer);
    if (FAILED(hr)) return;

    const int16_t* inSamples = reinterpret_cast<const int16_t*>(data);

    if (m_pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
        (m_pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
            reinterpret_cast<WAVEFORMATEXTENSIBLE*>(m_pwfx)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
        float* outFloat = reinterpret_cast<float*>(pBuffer);
        int channels = m_pwfx->nChannels;

        for (UINT32 i = 0; i < framesToWrite; ++i) {
            float sampleFloat = inSamples[i] / 32768.0f;
            for (int c = 0; c < channels; ++c) {
                outFloat[i * channels + c] = sampleFloat;
            }
        }
    }
    else {
        int channels = m_pwfx->nChannels;
        int16_t* outPcm = reinterpret_cast<int16_t*>(pBuffer);

        for (UINT32 i = 0; i < framesToWrite; ++i) {
            int16_t sample = inSamples[i];
            for (int c = 0; c < channels; ++c) {
                outPcm[i * channels + c] = sample;
            }
        }
    }

    m_renderClient->ReleaseBuffer(framesToWrite, 0);
}

void AudioReceiver::setPhoneTarget(const std::string& ip, int port) {
    if (ip.empty()) return;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(m_targetMutex);
        if (m_phoneIp != ip || m_phonePort != port) {
            m_phoneIp = ip;
            m_phonePort = port;
            m_targetChanged.store(true);
            changed = true;
        }
    }
    if (changed && m_socket != INVALID_SOCKET) {
        logAudioDebug("[AUDIO] Phone target changed to " + ip + ":" + std::to_string(port) + ", reconnecting socket");
        SOCKET s = m_socket;
        m_socket = INVALID_SOCKET;
        shutdown(s, SD_BOTH);
        closesocket(s);
    }
}

bool AudioReceiver::start(const std::string& ip, int port) {
    std::lock_guard<std::mutex> lock(m_lifecycleMutex);
    try {
        logAudioDebug("[AUDIO] start() requested: ip=" + ip + ":" + std::to_string(port));
        if (!ip.empty()) {
            setPhoneTarget(ip, port);
        }

        if (m_isRunning.load() && m_workerThread.joinable()) {
            logAudioDebug("[AUDIO] audioWorker is already running, updated target only");
            return true;
        }

        initAudioSharedMem();
        m_isRunning = true;
        m_workerThread = std::thread(&AudioReceiver::audioWorker, this);
        logAudioDebug("[AUDIO] start() thread spawned successfully");
        return true;
    } catch (const std::exception& e) {
        logAudioDebug(std::string("[AUDIO] start() exception: ") + e.what());
        return false;
    } catch (...) {
        logAudioDebug("[AUDIO] start() unknown exception");
        return false;
    }
}

void AudioReceiver::stop() {
    std::lock_guard<std::mutex> lock(m_lifecycleMutex);
    m_isRunning = false;
    m_wasapiCapture.stop();
    if (m_socket != INVALID_SOCKET) {
        SOCKET s = m_socket;
        m_socket = INVALID_SOCKET;
        shutdown(s, SD_BOTH);
        closesocket(s);
    }
    if (m_workerThread.joinable()) {
        if (m_workerThread.get_id() != std::this_thread::get_id()) {
            m_workerThread.join();
        }
    }
}

void AudioReceiver::audioWorker() {
    try {
        logAudioDebug("[AUDIO] audioWorker thread running");
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        initWasapi();

        std::vector<uint8_t> recvBuf(2048);
        std::vector<int16_t> captureBuf(1024);
        std::vector<int16_t> processedSamples(1024);
        std::vector<float> floatSamples(1024);
        AudioDSPProcessor dspProcessor(48000.0f);

        std::string activeDevice = g_audioInputDeviceId;
        bool useWasapi = (activeDevice != "phone");
        if (useWasapi) {
            logAudioDebug("[AUDIO] Starting wasapi capture: " + activeDevice);
            m_wasapiCapture.start(activeDevice);
        }

        auto lastConnectTry = std::chrono::steady_clock::now() - std::chrono::seconds(10);

        while (m_isRunning) {
            // 1. Проверяем динамическую смену устройства ввода
            if (g_audioInputDeviceChanged.exchange(false) || g_audioInputDeviceId != activeDevice) {
                activeDevice = g_audioInputDeviceId;
                useWasapi = (activeDevice != "phone");
                if (useWasapi) {
                    if (m_socket != INVALID_SOCKET) {
                        SOCKET s = m_socket;
                        m_socket = INVALID_SOCKET;
                        shutdown(s, SD_BOTH);
                        closesocket(s);
                    }
                    logAudioDebug("[AUDIO] Switching to WASAPI device: " + activeDevice);
                    m_wasapiCapture.start(activeDevice);
                } else {
                    logAudioDebug("[AUDIO] Switching to Phone network audio");
                    m_wasapiCapture.stop();
                }
            }

            int sampleCount = 0;
            int16_t* inSamples = nullptr;

            if (useWasapi) {
                // Чтение из локального микрофона ПК через WASAPI Capture
                sampleCount = static_cast<int>(m_wasapiCapture.readSamples(captureBuf.data(), 480));
                if (sampleCount <= 0) {
                    Sleep(4);
                    continue;
                }
                inSamples = captureBuf.data();
            } else {
                // Чтение из сетевого сокета телефона
                std::string targetIp;
                int targetPort = 8555;
                {
                    std::lock_guard<std::mutex> lock(m_targetMutex);
                    targetIp = m_phoneIp;
                    targetPort = m_phonePort;
                }

                if (m_socket == INVALID_SOCKET && !targetIp.empty()) {
                    auto now = std::chrono::steady_clock::now();
                    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastConnectTry).count() > 1000) {
                        lastConnectTry = now;
                        m_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                        if (m_socket != INVALID_SOCKET) {
                            int nodelay = 1;
                            setsockopt(m_socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
                            DWORD timeout = 2000;
                            setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

                            sockaddr_in addr = {};
                            addr.sin_family = AF_INET;
                            addr.sin_port = htons(static_cast<u_short>(targetPort));
                            inet_pton(AF_INET, targetIp.c_str(), &addr.sin_addr);
                            if (connect(m_socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
                                closesocket(m_socket);
                                m_socket = INVALID_SOCKET;
                            } else {
                                std::cout << "[AUDIO] Микрофон телефона подключен: " << targetIp << ":" << targetPort << "!\n";
                                logAudioDebug("[AUDIO] Phone audio connected: " + targetIp + ":" + std::to_string(targetPort));
                            }
                        }
                    }
                }

                if (m_socket == INVALID_SOCKET) {
                    Sleep(20);
                    continue;
                }

                int received = recv(m_socket, reinterpret_cast<char*>(recvBuf.data()), static_cast<int>(recvBuf.size()), 0);
                if (received <= 0) {
                    int err = WSAGetLastError();
                    if (err == WSAETIMEDOUT) {
                        continue;
                    }
                    SOCKET s = m_socket;
                    m_socket = INVALID_SOCKET;
                    closesocket(s);
                    Sleep(20);
                    continue;
                }
                sampleCount = received / static_cast<int>(sizeof(int16_t));
                if (sampleCount <= 0) continue;
                inSamples = reinterpret_cast<int16_t*>(recvBuf.data());
            }

        if (processedSamples.size() < static_cast<size_t>(sampleCount)) {
            processedSamples.resize(sampleCount);
        }

        float vol = m_volume.load();
        bool muted = m_isMuted.load();
        int delayMs = m_delayMs.load();
        bool gateEnabled = m_noiseGateEnabled.load();
        float gateThresh = m_noiseGateThreshold.load();

        // 2. Lip-Sync задержка через кольцевой буфер (48 кГц = 48 сэмплов/мс)
        if (delayMs > 0 && m_delayRing.size() >= 48000) {
            int delaySamples = delayMs * 48;
            if (delaySamples > 47000) delaySamples = 47000;
            size_t ringCap = m_delayRing.size();

            for (int i = 0; i < sampleCount; ++i) {
                m_delayRing[m_delayWritePos] = inSamples[i];
                size_t readPos = (m_delayWritePos + ringCap - delaySamples) % ringCap;
                processedSamples[i] = m_delayRing[readPos];
                m_delayWritePos = (m_delayWritePos + 1) % ringCap;
            }
        } else {
            memcpy(processedSamples.data(), inSamples, sampleCount * sizeof(int16_t));
        }

        // 3. AI Шумоподавление, De-clicker, 3-полосный EQ, AGC и Troll Audio FX
        dspProcessor.setAiNoiseSuppression(m_aiNoiseEnabled.load());
        dspProcessor.setAgc(m_agcEnabled.load());
        dspProcessor.setDeclicker(m_declickerEnabled.load());
        dspProcessor.setEq(m_eqLowDb.load(), m_eqMidDb.load(), m_eqHighDb.load());
        dspProcessor.setTrollEffects(g_trollGsmVoice.load(), g_trollWalkieTalkie.load(), g_trollRobotVoice.load());
        if (m_gsmBurstTrigger.exchange(false) || g_trollGsmBurstTrigger.exchange(false)) {
            dspProcessor.triggerGsmBurst();
        }

        if (floatSamples.size() < static_cast<size_t>(sampleCount)) {
            floatSamples.resize(sampleCount);
        }
        for (int i = 0; i < sampleCount; ++i) {
            floatSamples[i] = static_cast<float>(processedSamples[i]) / 32768.0f;
        }
        dspProcessor.process(floatSamples.data(), sampleCount);

        // Fake Lag: симуляция выпадения пакетов звука при обрыве связи
        if (g_trollFakeLag.load()) {
            static auto s_lastAudioLag = std::chrono::steady_clock::now();
            static bool s_inAudioDropout = false;
            static auto s_dropoutUntil = std::chrono::steady_clock::now();
            auto nowAudio = std::chrono::steady_clock::now();

            if (!s_inAudioDropout) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(nowAudio - s_lastAudioLag).count();
                if (elapsed > 1600 && (rand() % 16 == 0)) {
                    s_inAudioDropout = true;
                    s_dropoutUntil = nowAudio + std::chrono::milliseconds(260 + (rand() % 260));
                    s_lastAudioLag = nowAudio;
                }
            } else {
                if (nowAudio < s_dropoutUntil) {
                    for (int i = 0; i < sampleCount; ++i) {
                        floatSamples[i] = 0.0f;
                    }
                } else {
                    s_inAudioDropout = false;
                    s_lastAudioLag = nowAudio;
                }
            }
        }
        const float alphaAttack = 0.85f;
        const float alphaRelease = 0.999f;
        const float hpfCoeff = 0.995f;

        for (int i = 0; i < sampleCount; ++i) {
            if (muted) {
                processedSamples[i] = 0;
                continue;
            }

            float sNorm = floatSamples[i];

            if (gateEnabled) {
                float filtered = sNorm - m_hpfPrevIn + hpfCoeff * m_hpfPrevOut;
                m_hpfPrevIn = sNorm;
                m_hpfPrevOut = filtered;
                sNorm = filtered;

                float absVal = std::abs(sNorm);
                if (absVal > m_gateEnvelope) {
                    m_gateEnvelope = alphaAttack * m_gateEnvelope + (1.0f - alphaAttack) * absVal;
                } else {
                    m_gateEnvelope = alphaRelease * m_gateEnvelope;
                }

                float targetGain = (m_gateEnvelope >= gateThresh) ? 1.0f : 0.0f;
                m_gateGain = 0.92f * m_gateGain + 0.08f * targetGain;
                sNorm *= m_gateGain;
            }

            if (vol != 1.0f) {
                sNorm *= vol;
            }

            int32_t finalSample = static_cast<int32_t>(sNorm * 32768.0f);
            processedSamples[i] = static_cast<int16_t>((std::clamp)(finalSample, -32768, 32767));
        }

        size_t pcmBytesCount = sampleCount * sizeof(int16_t);
        playPcmChunk(reinterpret_cast<const uint8_t*>(processedSamples.data()), pcmBytesCount);

        if (g_audioShm && g_audioRingBuffer) {
            uint32_t wPos = g_audioShm->writePos;
            const uint8_t* pcmBytes = reinterpret_cast<const uint8_t*>(processedSamples.data());
            for (size_t i = 0; i < pcmBytesCount; ++i) {
                g_audioRingBuffer[(wPos + i) % MF_AUDIO_BUFFER_SIZE] = pcmBytes[i];
            }
            g_audioShm->writePos = (wPos + static_cast<uint32_t>(pcmBytesCount)) % MF_AUDIO_BUFFER_SIZE;
        }
    }
    } catch (const std::exception& e) {
        logAudioDebug(std::string("[AUDIO] audioWorker unhandled exception: ") + e.what());
    } catch (...) {
        logAudioDebug("[AUDIO] audioWorker unhandled unknown exception");
    }

    m_wasapiCapture.stop();
    if (m_socket != INVALID_SOCKET) {
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }
    cleanupWasapi();
    CoUninitialize();
    logAudioDebug("[AUDIO] audioWorker exited cleanly");
}