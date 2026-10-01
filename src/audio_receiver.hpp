#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <thread>
#include <atomic>
#include <string>
#include <vector>

class AudioReceiver {
public:
    AudioReceiver();
    ~AudioReceiver();

    void setVolume(float vol);
    void setMute(bool muted);
    void setDelayMs(int delayMs);
    void setNoiseGate(bool enabled, float threshold = 0.015f);

    bool start(const std::string& ip, int port = 8555);
    void stop();

private:
    bool initWasapi();
    void cleanupWasapi();
    void playPcmChunk(const uint8_t* data, size_t size);

    void audioWorker(std::string ip, int port);

    std::atomic<bool> m_isRunning{ false };
    std::atomic<bool> m_isMuted{ false };
    std::atomic<float> m_volume{ 1.0f };
    std::atomic<int> m_delayMs{ 0 };
    std::atomic<bool> m_noiseGateEnabled{ true };
    std::atomic<float> m_noiseGateThreshold{ 0.015f };
    std::thread m_workerThread;
    SOCKET m_socket{ INVALID_SOCKET };

    // WASAPI интерфейсы для вывода в VB-Cable
    IMMDeviceEnumerator* m_deviceEnumerator{ nullptr };
    IMMDevice* m_cableDevice{ nullptr };
    IAudioClient* m_audioClient{ nullptr };
    IAudioRenderClient* m_renderClient{ nullptr };
    UINT32 m_bufferFrameCount{ 0 };
    WAVEFORMATEX* m_pwfx{ nullptr };

    // Lip-Sync кольцевой буфер задержки (48000 сэмплов = 1000 мс)
    std::vector<int16_t> m_delayRing;
    size_t m_delayWritePos{ 0 };

    // Noise Gate & DSP фильтры
    float m_hpfPrevIn{ 0.0f };
    float m_hpfPrevOut{ 0.0f };
    float m_gateEnvelope{ 0.0f };
    float m_gateGain{ 1.0f };
};