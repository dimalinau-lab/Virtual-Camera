#pragma once
#include <windows.h>
#include <cstdint>
#include <vector>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>

#pragma pack(push, 1)
struct UnityCaptureMemoryHead {
    volatile int32_t ReceiveOrder; // 0
    volatile int32_t SendOrder;    // 4
    int32_t Width;                 // 8
    int32_t Height;                // 12
    int32_t Format;                // 16: 0 = RGBA32
    int32_t MaxDimension;          // 20
    int32_t ResizeMode;            // 24: 1 = Linear
    int32_t MirrorMode;            // 28: 0 = Normal
    int32_t Timeout;               // 32: 1000 ms
};
#pragma pack(pop)

static_assert(sizeof(UnityCaptureMemoryHead) == 36, "Заголовок UnityCapture обязан быть ровно 36 байт!");

struct VCamDevice {
    HANDLE hMutex = nullptr;
    HANDLE hEventSent = nullptr;
    HANDLE hEventWant = nullptr;
    HANDLE hSharedMem = nullptr;
    uint8_t* pBuffer = nullptr;
    std::string name;
};

class ObsVirtualCam {
public:
    ObsVirtualCam();
    ~ObsVirtualCam();

    bool start(int width = 1280, int height = 720, int fps = 30);
    void sendFrameRGBA(const uint8_t* rgbaData, int actualWidth, int actualHeight);
    void stop();

private:
    void syncWorker();

    std::vector<VCamDevice> devices;
    int frameWidth;
    int frameHeight;
    size_t frameSizeBytes;
    size_t totalMemSize;

    std::vector<uint8_t> latestFrameBuffer;
    std::mutex frameMtx;
    std::atomic<bool> isRunning{ false };
    std::thread workerThread;
};