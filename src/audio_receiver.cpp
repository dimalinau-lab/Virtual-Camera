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
static HANDLE g_hAudioMutex = nullptr;
static MFAudioSharedHeader* g_audioShm = nullptr;
static uint8_t* g_audioRingBuffer = nullptr;

static bool initAudioSharedMem() {
    if (!g_hAudioMutex) {
        g_hAudioMutex = CreateMutexA(nullptr, FALSE, MF_AUDIO_MUTEX_NAME);
    }
    if (g_audioShm) return true;
    size_t totalSize = sizeof(MFAudioSharedHeader) + MF_AUDIO_BUFFER_SIZE;
    g_hAudioShm = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, (DWORD)totalSize, MF_AUDIO_MEM_NAME);
    if (!g_hAudioShm) return false;

    g_audioShm = reinterpret_cast<MFAudioSharedHeader*>(MapViewOfFile(g_hAudioShm, FILE_MAP_ALL_ACCESS, 0, 0, totalSize));
    if (!g_audioShm) {
        CloseHandle(g_hAudioShm);
        g_hAudioShm = nullptr;
        return false;
    }

    if (g_hAudioMutex) WaitForSingleObject(g_hAudioMutex, 50);
    g_audioShm->magic = 0x4D464155;
    g_audioShm->sampleRate = 48000;
    g_audioShm->channels = 1;
    g_audioShm->bitsPerSample = 16;
    g_audioShm->writePos = 0;
    g_audioShm->readPos = 0;
    g_audioRingBuffer = reinterpret_cast<uint8_t*>(g_audioShm + 1);
    if (g_hAudioMutex) ReleaseMutex(g_hAudioMutex);
    return true;
}

static void cleanupAudioSharedMem() {
    if (g_audioShm) {
        if (g_hAudioMutex) WaitForSingleObject(g_hAudioMutex, 50);
        g_audioShm->magic = 0;
        g_audioShm->writePos = 0;
        g_audioShm->readPos = 0;
        if (g_hAudioMutex) ReleaseMutex(g_hAudioMutex);
        UnmapViewOfFile(g_audioShm);
        g_audioShm = nullptr;
        g_audioRingBuffer = nullptr;
    }
    if (g_hAudioShm) {
        CloseHandle(g_hAudioShm);
        g_hAudioShm = nullptr;
    }
    if (g_hAudioMutex) {
        CloseHandle(g_hAudioMutex);
        g_hAudioMutex = nullptr;
    }
}

AudioReceiver::AudioReceiver() {
    m_delayRing.assign(48000, 0);
}

AudioReceiver::~AudioReceiver() {
    stop();
    cleanupAudioSharedMem();
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

void AudioReceiver::setSourceFormat(int sampleRate, int channels) {
    int clampedRate = (std::clamp)(sampleRate, 8000, 192000);
    int clampedCh = (std::clamp)(channels, 1, 8);
    int prevRate = m_sourceSampleRate.exchange(clampedRate);
    int prevCh = m_sourceChannels.exchange(clampedCh);
    if (prevRate != clampedRate || prevCh != clampedCh) {
        logAudioDebug("[AUDIO] Source format set: " + std::to_string(clampedRate) + "Hz, " + std::to_string(clampedCh) + "ch -> resampling to 48000Hz mono");
    }
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

std::vector<WasapiDeviceInfo> AudioReceiver::getAudioOutputDevices() {
    return WasapiCaptureClient::enumerateRenderDevices();
}

void AudioReceiver::setAudioOutputDevice(const std::string& deviceId) {
    g_audioOutputDeviceId = deviceId;
    g_audioOutputDeviceChanged.store(true);
}

std::string AudioReceiver::getCurrentAudioOutputDevice() {
    return g_audioOutputDeviceId;
}

void AudioReceiver::triggerGsmBurst() {
    m_gsmBurstTrigger.store(true);
}

bool AudioReceiver::initWasapi(const std::string& deviceId) {
    std::lock_guard<std::mutex> lock(m_renderMutex);
    cleanupWasapi();

    std::string target = deviceId.empty() ? g_audioOutputDeviceId : deviceId;
    if (target.empty() || target == "vcam_only") {
        logAudioDebug("[AUDIO] Output mode: DirectShow Virtual Microphone only (vcam_only). WASAPI render bypassed.");
        return true;
    }

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&m_deviceEnumerator);
    if (FAILED(hr) || !m_deviceEnumerator) {
        logAudioDebug("[AUDIO] Failed to create MMDeviceEnumerator for render: hr=" + std::to_string(hr));
        return false;
    }

    if (target == "default") {
        hr = m_deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &m_cableDevice);
    } else {
        int wLen = MultiByteToWideChar(CP_UTF8, 0, target.c_str(), -1, nullptr, 0);
        if (wLen > 0) {
            std::wstring wId(wLen - 1, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, target.c_str(), -1, &wId[0], wLen);
            hr = m_deviceEnumerator->GetDevice(wId.c_str(), &m_cableDevice);
        } else {
            hr = E_INVALIDARG;
        }
    }

    if (FAILED(hr) || !m_cableDevice) {
        logAudioDebug("[AUDIO] Failed to get audio render device: " + target);
        cleanupWasapi();
        return false;
    }

    hr = m_cableDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&m_audioClient);
    if (FAILED(hr) || !m_audioClient) {
        logAudioDebug("[AUDIO] Failed to activate IAudioClient on render device: hr=" + std::to_string(hr));
        cleanupWasapi();
        return false;
    }

    hr = m_audioClient->GetMixFormat(&m_pwfx);
    if (FAILED(hr) || !m_pwfx) {
        logAudioDebug("[AUDIO] Failed to get render mix format: hr=" + std::to_string(hr));
        cleanupWasapi();
        return false;
    }

    // 100 ms buffer in shared mode
    REFERENCE_TIME hnsBufferDuration = 1000000;
    hr = m_audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, hnsBufferDuration, 0, m_pwfx, nullptr);
    if (FAILED(hr)) {
        logAudioDebug("[AUDIO] Failed to initialize render IAudioClient: hr=" + std::to_string(hr));
        cleanupWasapi();
        return false;
    }

    hr = m_audioClient->GetBufferSize(&m_bufferFrameCount);
    if (FAILED(hr) || m_bufferFrameCount == 0) {
        logAudioDebug("[AUDIO] Failed to get render buffer size: hr=" + std::to_string(hr));
        cleanupWasapi();
        return false;
    }

    hr = m_audioClient->GetService(__uuidof(IAudioRenderClient), (void**)&m_renderClient);
    if (FAILED(hr) || !m_renderClient) {
        logAudioDebug("[AUDIO] Failed to get IAudioRenderClient: hr=" + std::to_string(hr));
        cleanupWasapi();
        return false;
    }

    hr = m_audioClient->Start();
    if (FAILED(hr)) {
        logAudioDebug("[AUDIO] Failed to start render IAudioClient: hr=" + std::to_string(hr));
        cleanupWasapi();
        return false;
    }

    logAudioDebug("[AUDIO] Output device active: " + target + " (format: " + std::to_string(m_pwfx->nSamplesPerSec) + "Hz, " + std::to_string(m_pwfx->nChannels) + "ch)");
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
    m_bufferFrameCount = 0;
}

void AudioReceiver::playPcmChunk(const uint8_t* data, size_t size) {
    if (!data || size == 0) return;

    std::unique_lock<std::mutex> lock(m_renderMutex, std::try_to_lock);
    if (!lock.owns_lock() || !m_renderClient || !m_audioClient || !m_pwfx) {
        return;
    }

    const int16_t* inSamples = reinterpret_cast<const int16_t*>(data);
    UINT32 inFrames = static_cast<UINT32>(size / sizeof(int16_t));
    if (inFrames == 0) return;

    UINT32 padding = 0;
    HRESULT hr = m_audioClient->GetCurrentPadding(&padding);
    if (FAILED(hr)) return;

    UINT32 availableFrames = (m_bufferFrameCount > padding) ? (m_bufferFrameCount - padding) : 0;
    if (availableFrames == 0) return;

    UINT32 channels = m_pwfx->nChannels;
    DWORD sampleRate = m_pwfx->nSamplesPerSec;

    UINT32 outFrames = inFrames;
    if (sampleRate != 48000 && sampleRate > 0) {
        outFrames = static_cast<UINT32>((static_cast<uint64_t>(inFrames) * sampleRate) / 48000);
    }
    if (outFrames > availableFrames) {
        outFrames = availableFrames;
    }
    if (outFrames == 0) return;

    BYTE* pBuffer = nullptr;
    hr = m_renderClient->GetBuffer(outFrames, &pBuffer);
    if (FAILED(hr) || !pBuffer) return;

    bool isFloat = (m_pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
        (m_pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
         reinterpret_cast<WAVEFORMATEXTENSIBLE*>(m_pwfx)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);

    if (isFloat) {
        float* outFloat = reinterpret_cast<float*>(pBuffer);
        for (UINT32 f = 0; f < outFrames; ++f) {
            float sample = 0.0f;
            if (sampleRate == 48000) {
                UINT32 idx = (f < inFrames) ? f : (inFrames - 1);
                sample = static_cast<float>(inSamples[idx]) / 32768.0f;
            } else {
                float inPos = static_cast<float>(f) * 48000.0f / static_cast<float>(sampleRate);
                UINT32 i0 = static_cast<UINT32>(inPos);
                UINT32 i1 = (i0 + 1 < inFrames) ? (i0 + 1) : i0;
                if (i0 >= inFrames) i0 = inFrames - 1;
                float frac = inPos - static_cast<float>(i0);
                sample = ((1.0f - frac) * inSamples[i0] + frac * inSamples[i1]) / 32768.0f;
            }
            for (UINT32 ch = 0; ch < channels; ++ch) {
                outFloat[f * channels + ch] = sample;
            }
        }
    } else {
        int16_t* outPcm = reinterpret_cast<int16_t*>(pBuffer);
        for (UINT32 f = 0; f < outFrames; ++f) {
            int16_t sample = 0;
            if (sampleRate == 48000) {
                UINT32 idx = (f < inFrames) ? f : (inFrames - 1);
                sample = inSamples[idx];
            } else {
                float inPos = static_cast<float>(f) * 48000.0f / static_cast<float>(sampleRate);
                UINT32 i0 = static_cast<UINT32>(inPos);
                UINT32 i1 = (i0 + 1 < inFrames) ? (i0 + 1) : i0;
                if (i0 >= inFrames) i0 = inFrames - 1;
                float frac = inPos - static_cast<float>(i0);
                sample = static_cast<int16_t>((1.0f - frac) * inSamples[i0] + frac * inSamples[i1]);
            }
            for (UINT32 ch = 0; ch < channels; ++ch) {
                outPcm[f * channels + ch] = sample;
            }
        }
    }

    m_renderClient->ReleaseBuffer(outFrames, 0);
}

void AudioReceiver::pushPcmWithPts(const uint8_t* data, size_t size, int64_t ptsUs) {
    m_lastAudioPtsUs.store(ptsUs, std::memory_order_relaxed);
    if (!data || size == 0) return;
    {
        std::lock_guard<std::mutex> lock(m_muxQueueMutex);
        if (m_muxAudioQueue.size() >= 64) {
            m_muxAudioQueue.pop(); // сбрасываем устаревший буфер при сетевом джиттере
        }
        m_muxAudioQueue.emplace(data, data + size);
    }
    m_muxCv.notify_one();
}

bool AudioReceiver::start(const std::string&, int) {
    std::lock_guard<std::mutex> lock(m_lifecycleMutex);
    try {
        logAudioDebug("[AUDIO] start() requested");

        if (m_isRunning.load() && m_workerThread.joinable()) {
            logAudioDebug("[AUDIO] audioWorker is already running");
            return true;
        }

        m_srcResamplePhase = 0.0;
        initAudioSharedMem();
        m_isRunning = true;
        m_workerThread = std::thread(&AudioReceiver::audioWorker, this);
        logAudioDebug("[AUDIO] start() thread spawned successfully");
        return true;
    } catch (const std::exception& e) {
        m_isRunning = false;
        logAudioDebug(std::string("[AUDIO] start() exception: ") + e.what());
        return false;
    } catch (...) {
        m_isRunning = false;
        logAudioDebug("[AUDIO] start() unknown exception");
        return false;
    }
}

void AudioReceiver::stop() {
    std::lock_guard<std::mutex> lock(m_lifecycleMutex);
    m_isRunning = false;
    m_muxCv.notify_all();
    m_wasapiCapture.stop();
    if (m_workerThread.joinable()) {
        if (m_workerThread.get_id() != std::this_thread::get_id()) {
            m_workerThread.join();
        }
    }
    {
        std::lock_guard<std::mutex> rLock(m_renderMutex);
        cleanupWasapi();
    }
    cleanupAudioSharedMem();
}

void AudioReceiver::audioWorker() {
    HRESULT hrCo = E_FAIL;
    try {
        logAudioDebug("[AUDIO] audioWorker thread running");
        hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

        std::string activeOutputDevice = g_audioOutputDeviceId;
        initWasapi(activeOutputDevice);

        std::vector<int16_t> phoneMonoBuf(2048);
        std::vector<int16_t> captureBuf(2048);
        std::vector<int16_t> processedSamples(2048);
        std::vector<float> floatSamples(2048);
        std::vector<float> monoTemp;
        AudioDSPProcessor dspProcessor(48000.0f);
        int lastSrcRate = m_sourceSampleRate.load();
        int lastSrcCh = m_sourceChannels.load();

        auto convertPhoneTo48kMono = [&](const std::vector<uint8_t>& pkt, std::vector<int16_t>& out) -> int {
            int srcRate = m_sourceSampleRate.load();
            int srcCh = (std::max)(1, m_sourceChannels.load());
            if (srcRate != lastSrcRate || srcCh != lastSrcCh) {
                m_srcResamplePhase = 0.0;
                lastSrcRate = srcRate;
                lastSrcCh = srcCh;
            }
            size_t totalShorts = pkt.size() / sizeof(int16_t);
            size_t srcFrames = totalShorts / static_cast<size_t>(srcCh);
            if (srcFrames == 0) return 0;

            const int16_t* raw = reinterpret_cast<const int16_t*>(pkt.data());
            if (srcRate == 48000 && srcCh == 1) {
                if (out.size() < srcFrames) out.resize(srcFrames);
                memcpy(out.data(), raw, srcFrames * sizeof(int16_t));
                return static_cast<int>(srcFrames);
            }

            monoTemp.resize(srcFrames);
            if (srcCh == 1) {
                for (size_t i = 0; i < srcFrames; ++i) {
                    monoTemp[i] = static_cast<float>(raw[i]);
                }
            } else {
                for (size_t i = 0; i < srcFrames; ++i) {
                    float sum = 0.0f;
                    for (int c = 0; c < srcCh; ++c) {
                        sum += static_cast<float>(raw[i * srcCh + c]);
                    }
                    monoTemp[i] = sum / static_cast<float>(srcCh);
                }
            }

            if (srcRate == 48000) {
                if (out.size() < srcFrames) out.resize(srcFrames);
                for (size_t i = 0; i < srcFrames; ++i) {
                    out[i] = static_cast<int16_t>((std::clamp)(monoTemp[i], -32768.0f, 32767.0f));
                }
                return static_cast<int>(srcFrames);
            }

            double step = static_cast<double>(srcRate) / 48000.0;
            size_t estOut = static_cast<size_t>(srcFrames / step) + 8;
            if (out.size() < estOut) out.resize(estOut);
            int produced = 0;
            double pos = m_srcResamplePhase;
            while (pos < static_cast<double>(srcFrames)) {
                size_t i0 = static_cast<size_t>(pos);
                size_t i1 = (i0 + 1 < srcFrames) ? (i0 + 1) : i0;
                double frac = pos - static_cast<double>(i0);
                float val = static_cast<float>((1.0 - frac) * monoTemp[i0] + frac * monoTemp[i1]);
                if (static_cast<size_t>(produced) >= out.size()) out.push_back(0);
                out[produced++] = static_cast<int16_t>((std::clamp)(val, -32768.0f, 32767.0f));
                pos += step;
            }
            m_srcResamplePhase = pos - static_cast<double>(srcFrames);
            if (m_srcResamplePhase < 0.0) m_srcResamplePhase = 0.0;
            return produced;
        };

        std::string activeDevice = g_audioInputDeviceId;
        bool isDualMic = (activeDevice == "dual_mic");
        bool useWasapiOnly = (activeDevice != "phone" && !isDualMic);
        bool needWasapi = (useWasapiOnly || isDualMic);
        if (needWasapi) {
            std::string wasapiDev = isDualMic ? "default" : activeDevice;
            logAudioDebug("[AUDIO] Starting wasapi capture: " + wasapiDev);
            m_wasapiCapture.start(wasapiDev);
        }

        auto lastConnectTry = std::chrono::steady_clock::now() - std::chrono::seconds(10);

        while (m_isRunning) {
            // 1. Проверяем динамическую смену устройства ввода
            if (g_audioInputDeviceChanged.exchange(false) || g_audioInputDeviceId != activeDevice) {
                activeDevice = g_audioInputDeviceId;
                isDualMic = (activeDevice == "dual_mic");
                useWasapiOnly = (activeDevice != "phone" && !isDualMic);
                needWasapi = (useWasapiOnly || isDualMic);
                if (needWasapi) {
                    std::string wasapiDev = isDualMic ? "default" : activeDevice;
                    logAudioDebug("[AUDIO] Switching WASAPI device: " + wasapiDev);
                    m_wasapiCapture.start(wasapiDev);
                } else {
                    logAudioDebug("[AUDIO] Switching to Phone network audio only");
                    m_wasapiCapture.stop();
                }
            }

            // 1.1. Проверяем динамическую смену устройства вывода
            if (g_audioOutputDeviceChanged.exchange(false) || g_audioOutputDeviceId != activeOutputDevice) {
                activeOutputDevice = g_audioOutputDeviceId;
                initWasapi(activeOutputDevice);
            }

            int sampleCount = 0;
            int16_t* inSamples = nullptr;

            if (useWasapiOnly) {
                // Чтение только из локального микрофона ПК через WASAPI Capture
                sampleCount = static_cast<int>(m_wasapiCapture.readSamples(captureBuf.data(), 480));
                if (sampleCount <= 0) {
                    Sleep(4);
                    continue;
                }
                inSamples = captureBuf.data();
            } else if (isDualMic) {
                // Режим Dual-Mic: одновременный захват с телефона и микрофона ПК
                std::vector<uint8_t> muxPacket;
                {
                    std::unique_lock<std::mutex> lock(m_muxQueueMutex);
                    if (m_muxAudioQueue.empty()) {
                        m_muxCv.wait_for(lock, std::chrono::milliseconds(8), [this] {
                            return !m_muxAudioQueue.empty() || !m_isRunning;
                        });
                    }
                    if (!m_muxAudioQueue.empty()) {
                        muxPacket = std::move(m_muxAudioQueue.front());
                        m_muxAudioQueue.pop();
                    }
                }

                int phoneSamples = muxPacket.empty() ? 0 : convertPhoneTo48kMono(muxPacket, phoneMonoBuf);
                size_t wantPc = (phoneSamples > 0) ? static_cast<size_t>(phoneSamples) : 480;
                if (captureBuf.size() < wantPc) captureBuf.resize(wantPc);
                size_t pcRead = m_wasapiCapture.readSamples(captureBuf.data(), wantPc);

                if (phoneSamples > 0) {
                    sampleCount = phoneSamples;
                    inSamples = phoneMonoBuf.data();

                    // Микширование звука телефона со звуком микрофона ПК
                    if (pcRead > 0) {
                        for (int i = 0; i < sampleCount; ++i) {
                            int32_t sPhone = inSamples[i];
                            int32_t sPc = (i < static_cast<int>(pcRead)) ? captureBuf[i] : 0;
                            int32_t mixed = sPhone + sPc;
                            inSamples[i] = static_cast<int16_t>((std::clamp)(mixed, -32768, 32767));
                        }
                    }
                } else if (pcRead > 0) {
                    sampleCount = static_cast<int>(pcRead);
                    if (phoneMonoBuf.size() < pcRead) phoneMonoBuf.resize(pcRead);
                    memcpy(phoneMonoBuf.data(), captureBuf.data(), pcRead * sizeof(int16_t));
                    inSamples = phoneMonoBuf.data();
                } else {
                    Sleep(4);
                    continue;
                }
            } else {
                // Чтение мультиплексированного звука только из потока телефона
                std::vector<uint8_t> muxPacket;
                {
                    std::unique_lock<std::mutex> lock(m_muxQueueMutex);
                    if (m_muxAudioQueue.empty()) {
                        m_muxCv.wait_for(lock, std::chrono::milliseconds(12), [this] {
                            return !m_muxAudioQueue.empty() || !m_isRunning;
                        });
                    }
                    if (!m_muxAudioQueue.empty()) {
                        muxPacket = std::move(m_muxAudioQueue.front());
                        m_muxAudioQueue.pop();
                    }
                }

                if (!muxPacket.empty()) {
                    sampleCount = convertPhoneTo48kMono(muxPacket, phoneMonoBuf);
                    if (sampleCount <= 0) continue;
                    inSamples = phoneMonoBuf.data();
                } else {
                    continue;
                }
            }

        if (processedSamples.size() < static_cast<size_t>(sampleCount)) {
            processedSamples.resize(sampleCount);
        }

        float vol = m_volume.load();
        bool muted = m_isMuted.load();
        int delayMs = m_delayMs.load();
        bool gateEnabled = m_noiseGateEnabled.load();
        float gateThresh = m_noiseGateThreshold.load();

        // 2. Всегда непрерывно пишем в кольцевой буфер Lip-Sync (исключает щелчок при включении задержки > 0)
        if (m_delayRing.size() >= 48000) {
            int delaySamples = delayMs * 48;
            if (delaySamples > 47000) delaySamples = 47000;
            size_t ringCap = m_delayRing.size();

            for (int i = 0; i < sampleCount; ++i) {
                m_delayRing[m_delayWritePos] = inSamples[i];
                if (delaySamples > 0) {
                    size_t readPos = (m_delayWritePos + ringCap - delaySamples) % ringCap;
                    processedSamples[i] = m_delayRing[readPos];
                } else {
                    processedSamples[i] = inSamples[i];
                }
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
        const float hpfCoeff = 0.995f;

        for (int i = 0; i < sampleCount; ++i) {
            if (muted) {
                processedSamples[i] = 0;
                continue;
            }

            float sNorm = floatSamples[i];

            if (gateEnabled) {
                // High-pass filter (< 80 Hz) для отсечения сверхнизкочастотного гула
                float filtered = sNorm - m_hpfPrevIn + hpfCoeff * m_hpfPrevOut;
                m_hpfPrevIn = sNorm;
                m_hpfPrevOut = filtered;
                sNorm = filtered;

                float absVal = std::abs(sNorm);
                // Мгновенная атака (не глушит первый звук фразы)
                if (absVal > m_gateEnvelope) {
                    m_gateEnvelope = 0.40f * m_gateEnvelope + 0.60f * absVal;
                } else {
                    m_gateEnvelope = 0.9992f * m_gateEnvelope;
                }

                // Студийный гистерезис: порог открытия gateThresh, порог закрытия на -3 dB ниже (70%)
                float openThresh = (std::max)(gateThresh, 0.002f);
                float closeThresh = openThresh * 0.70f;

                if (m_gateEnvelope >= openThresh) {
                    m_gateHoldSamples = 5760; // 120 мс удержание (Hold time) при 48 кГц
                } else if (m_gateHoldSamples > 0) {
                    m_gateHoldSamples--;
                }

                float targetGain = (m_gateHoldSamples > 0 || m_gateEnvelope >= closeThresh) ? 1.0f : 0.0f;
                // Мгновенное открытие за ~2 мс, бесшовное плавное затухание за ~40 мс (без щелчков)
                float slew = (targetGain > m_gateGain) ? 0.30f : 0.015f;
                m_gateGain = (1.0f - slew) * m_gateGain + slew * targetGain;

                sNorm *= m_gateGain;
            }

            if (vol != 1.0f) {
                sNorm *= vol;
            }

            // Универсальный Soft-Knee лимитер (мягкое скругление tanh выше 0.85f):
            // Гарантирует полное отсутствие жесткого клиппинга и хрипов
            if (sNorm > 0.85f) {
                sNorm = 0.85f + 0.15f * std::tanh((sNorm - 0.85f) / 0.15f);
            } else if (sNorm < -0.85f) {
                sNorm = -0.85f + 0.15f * std::tanh((sNorm + 0.85f) / 0.15f);
            }

            int32_t finalSample = static_cast<int32_t>(sNorm * 32767.0f);
            processedSamples[i] = static_cast<int16_t>((std::clamp)(finalSample, -32768, 32767));
        }

        size_t pcmBytesCount = sampleCount * sizeof(int16_t);
        playPcmChunk(reinterpret_cast<const uint8_t*>(processedSamples.data()), pcmBytesCount);

        if (g_audioShm && g_audioRingBuffer) {
            if (!g_hAudioMutex || WaitForSingleObject(g_hAudioMutex, 2) == WAIT_OBJECT_0) {
                uint32_t wPos = g_audioShm->writePos % MF_AUDIO_BUFFER_SIZE;
                uint32_t rPos = g_audioShm->readPos % MF_AUDIO_BUFFER_SIZE;
                uint32_t used = (wPos >= rPos) ? (wPos - rPos) : (MF_AUDIO_BUFFER_SIZE - rPos + wPos);
                if (used + pcmBytesCount >= MF_AUDIO_BUFFER_SIZE) {
                    // Сбрасываем старый непрочитанный хвост при переполнении буфера микрофона
                    rPos = (wPos + static_cast<uint32_t>(pcmBytesCount) + 1) % MF_AUDIO_BUFFER_SIZE;
                    g_audioShm->readPos = rPos;
                }
                const uint8_t* pcmBytes = reinterpret_cast<const uint8_t*>(processedSamples.data());
                for (size_t i = 0; i < pcmBytesCount; ++i) {
                    g_audioRingBuffer[(wPos + i) % MF_AUDIO_BUFFER_SIZE] = pcmBytes[i];
                }
                g_audioShm->writePos = (wPos + static_cast<uint32_t>(pcmBytesCount)) % MF_AUDIO_BUFFER_SIZE;
                if (g_hAudioMutex) ReleaseMutex(g_hAudioMutex);
            }
        }
    }
    } catch (const std::exception& e) {
        logAudioDebug(std::string("[AUDIO] audioWorker unhandled exception: ") + e.what());
    } catch (...) {
        logAudioDebug("[AUDIO] audioWorker unhandled unknown exception");
    }

    m_wasapiCapture.stop();
    {
        std::lock_guard<std::mutex> lock(m_renderMutex);
        cleanupWasapi();
    }
    if (SUCCEEDED(hrCo)) CoUninitialize();
    logAudioDebug("[AUDIO] audioWorker exited cleanly");
}