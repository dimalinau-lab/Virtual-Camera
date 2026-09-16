#pragma once
#include <windows.h>
#include <cstdint>

// Общая память для видеокамеры (NV12)
#define MF_VCAM_MEM_NAME   "Local\\MFVirtualCam_SharedMemory"
#define MF_VCAM_MUTEX_NAME "Local\\MFVirtualCam_Mutex"
#define MF_VCAM_EVENT_NAME "Local\\MFVirtualCam_FrameEvent"

#pragma pack(push, 4)
struct MFVirtualCamHeader {
    uint32_t magic;         // 0x4D465643 ("MFVC")
    uint32_t width;         // 1280
    uint32_t height;        // 720
    uint32_t strideY;       // 1280
    uint32_t strideUV;      // 1280
    uint32_t frameSize;     // width * height * 3 / 2 (для NV12: 1382400 байт)
    volatile uint64_t frameIndex;
    volatile uint64_t timestampNs;
};
#pragma pack(pop)

// Общая память для виртуального микрофона (PCM 16-bit 48kHz моно)
#define MF_AUDIO_MEM_NAME   "Local\\MFAudio_SharedMemory"
#define MF_AUDIO_MUTEX_NAME "Local\\MFAudio_Mutex"
#define MF_AUDIO_BUFFER_SIZE (48000 * 2 * 2) // Буфер на 2 секунды (192 КБ)

#pragma pack(push, 4)
struct MFAudioSharedHeader {
    uint32_t magic;         // 0x4D464155 ("MFAU")
    uint32_t sampleRate;    // 48000
    uint16_t channels;      // 1
    uint16_t bitsPerSample; // 16
    volatile uint32_t writePos;
    volatile uint32_t readPos;
};
#pragma pack(pop)