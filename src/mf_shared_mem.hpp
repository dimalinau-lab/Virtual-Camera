#pragma once
#include <windows.h>
#include <cstdint>

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