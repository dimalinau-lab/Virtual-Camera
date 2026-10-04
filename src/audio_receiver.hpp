#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <thread>
#include <atomic>
#include <string>
#include <vector>
#include "wasapi_capture.hpp"

class AudioReceiver {
public:
    static AudioReceiver& instance() {
        static AudioReceiver inst;
        return inst;
    }

    AudioReceiver();
    ~AudioReceiver();

    void setVolume(float vol);
    void setMute(bool muted);
    void setDelayMs(int delayMs);
    void setNoiseGate(bool enabled, float threshold = 0.015f);
    void setAiNoise(bool enabled);
    void setAgc(bool enabled);
    void setEq(float lowDb, float midDb, float highDb);
    void setDeclicker(bool enabled);

    std::vector<WasapiDeviceInfo> getAudioDevices();
    void setAudioDevice(const std::string& deviceId);
    std::string getCurrentAudioDevice();
    void triggerGsmBurst();

    bool start(const std::string& ip = "", int port = 8555);
    void setPhoneTarget(const std::string& ip, int port = 8555);
    void stop();

private:
    bool initWasapi();
    void cleanupWasapi();
    void playPcmChunk(const uint8_t* data, size_t size);

    void audioWorker();

    std::mutex m_lifecycleMutex;
    std::mutex m_targetMutex;
    std::string m_phoneIp{ "127.0.0.1" };
    int m_phonePort{ 8555 };
    std::atomic<bool> m_targetChanged{ false };

    std::atomic<bool> m_isRunning{ false };
    std::atomic<bool> m_isMuted{ false };
    std::atomic<float> m_volume{ 1.0f };
    std::atomic<int> m_delayMs{ 0 };
    std::atomic<bool> m_noiseGateEnabled{ true };
    std::atomic<float> m_noiseGateThreshold{ 0.015f };
    std::atomic<bool> m_aiNoiseEnabled{ true };
    std::atomic<bool> m_agcEnabled{ true };
    std::atomic<bool> m_declickerEnabled{ true };
    std::atomic<float> m_eqLowDb{ 0.0f };
    std::atomic<float> m_eqMidDb{ 0.0f };
    std::atomic<float> m_eqHighDb{ 0.0f };
    std::atomic<bool> m_gsmBurstTrigger{ false };
    std::thread m_workerThread;
    SOCKET m_socket{ INVALID_SOCKET };

    WasapiCaptureClient m_wasapiCapture;

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