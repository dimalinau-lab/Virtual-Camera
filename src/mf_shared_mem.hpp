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
    volatile uint32_t activeReaders; // 0 = idle, >0 = ON AIR (OBS, Zoom, Discord, etc.)
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

// CLSID виртуальной камеры: {E1D3B890-5F16-47D8-9C9D-9F0A3E8B81B1}
inline constexpr GUID CLSID_NativeVirtualCam =
{ 0xe1d3b890, 0x5f16, 0x47d8, { 0x9c, 0x9d, 0x9f, 0x0a, 0x3e, 0x8b, 0x81, 0xb1 } };
inline constexpr const wchar_t* SZ_CLSID_NativeVirtualCam = L"{E1D3B890-5F16-47D8-9C9D-9F0A3E8B81B1}";
inline constexpr const char*    SZA_CLSID_NativeVirtualCam = "{E1D3B890-5F16-47D8-9C9D-9F0A3E8B81B1}";

// CLSID виртуального микрофона: {A1B2C3D4-E5F6-7890-ABCD-EF0123456789}
inline constexpr GUID CLSID_VirtualCamNativeMic =
{ 0xa1b2c3d4, 0xe5f6, 0x7890, { 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67, 0x89 } };
inline constexpr const wchar_t* SZ_CLSID_VirtualCamNativeMic = L"{A1B2C3D4-E5F6-7890-ABCD-EF0123456789}";
inline constexpr const char*    SZA_CLSID_VirtualCamNativeMic = "{A1B2C3D4-E5F6-7890-ABCD-EF0123456789}";