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
    std::thread m_workerThread;
    SOCKET m_socket{ INVALID_SOCKET };

    // WASAPI интерфейсы для вывода в VB-Cable
    IMMDeviceEnumerator* m_deviceEnumerator{ nullptr };
    IMMDevice* m_cableDevice{ nullptr };
    IAudioClient* m_audioClient{ nullptr };
    IAudioRenderClient* m_renderClient{ nullptr };
    UINT32 m_bufferFrameCount{ 0 };
    WAVEFORMATEX* m_pwfx{ nullptr };
};