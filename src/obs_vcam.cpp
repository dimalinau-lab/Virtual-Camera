#include "obs_vcam.hpp"
#include <iostream>
#include <sddl.h>
#include <chrono>

#pragma comment(lib, "advapi32.lib")

// Точная структура видео-очереди OBS Studio Virtual Camera
#pragma pack(push, 1)
struct obs_video_info_shmem {
    uint32_t magic;          // 0x5643414D ('VCAM')
    uint32_t width;
    uint32_t height;
    uint32_t format;         // 1 = VIDEO_FORMAT_NV12
    uint64_t timestamp;      // DirectShow 100-ns units
    uint32_t frame_index;
    uint32_t linesize[4];
};
#pragma pack(pop)

ObsVirtualCam::ObsVirtualCam()
    : frameWidth(1280), frameHeight(720),
    frameSizeBytes(1280 * 720 * 3 / 2),
    totalMemSize(1920 * 1080 * 3 / 2 + sizeof(obs_video_info_shmem) + 1024) {
}

ObsVirtualCam::~ObsVirtualCam() {
    stop();
}

bool ObsVirtualCam::start(int width, int height, int fps) {
    frameWidth = width;
    frameHeight = height;
    frameSizeBytes = (size_t)width * height * 3 / 2; // NV12: Y + UV
    totalMemSize = (size_t)1920 * 1080 * 3 / 2 + sizeof(obs_video_info_shmem) + 1024;

    latestFrameBuffer.assign(frameSizeBytes, 0x10);

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    ConvertStringSecurityDescriptorToSecurityDescriptorA(
        "D:(A;;GA;;;WD)S:(ML;;NW;;;LW)",
        SDDL_REVISION_1,
        &sa.lpSecurityDescriptor,
        nullptr
    );

    auto tryConnect = [&](const std::string& baseName) {
        VCamDevice dev;
        dev.name = baseName;

        std::string eventName = baseName + "_Event";
        std::string mutexName = baseName + "_Mutex";

        dev.hMutex = OpenMutexA(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, mutexName.c_str());
        if (!dev.hMutex) dev.hMutex = CreateMutexA(&sa, FALSE, mutexName.c_str());

        dev.hEventSent = OpenEventA(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, eventName.c_str());
        if (!dev.hEventSent) dev.hEventSent = CreateEventA(&sa, FALSE, FALSE, eventName.c_str());

        dev.hSharedMem = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, baseName.c_str());
        if (!dev.hSharedMem) {
            dev.hSharedMem = CreateFileMappingA(
                INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, (DWORD)totalMemSize, baseName.c_str()
            );
        }

        if (!dev.hSharedMem) return;

        dev.pBuffer = (uint8_t*)MapViewOfFile(dev.hSharedMem, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        if (!dev.pBuffer) {
            CloseHandle(dev.hSharedMem);
            dev.hSharedMem = nullptr;
            return;
        }

        devices.push_back(dev);
        std::cout << "[OBS VCAM] Подключен целевой слот: " << baseName << "\n";
        };

    const std::vector<std::string> slotNames = {
        "Global\\OBSVirtualCam0",
        "OBSVirtualCam0",
        "Global\\OBSVirtualCam_SharedMemory",
        "OBSVirtualCam_SharedMemory"
    };

    for (const auto& name : slotNames) {
        tryConnect(name);
    }

    if (sa.lpSecurityDescriptor) {
        LocalFree(sa.lpSecurityDescriptor);
    }

    if (devices.empty()) return false;

    isRunning = true;
    workerThread = std::thread(&ObsVirtualCam::syncWorker, this);
    return true;
}

// Конвертация RGBA -> NV12 прямо перед отправкой в фильтр
void ObsVirtualCam::sendFrameRGBA(const uint8_t* rgbaData, int actualWidth, int actualHeight) {
    if (!rgbaData) return;

    std::lock_guard<std::mutex> lock(frameMtx);
    frameWidth = actualWidth;
    frameHeight = actualHeight;
    frameSizeBytes = (size_t)actualWidth * actualHeight * 3 / 2;

    if (latestFrameBuffer.size() < frameSizeBytes) {
        latestFrameBuffer.resize(frameSizeBytes);
    }

    uint8_t* yPlane = latestFrameBuffer.data();
    uint8_t* uvPlane = yPlane + (actualWidth * actualHeight);

    // Быстрый перевод RGB в NV12
    for (int y = 0; y < actualHeight; y++) {
        for (int x = 0; x < actualWidth; x++) {
            int srcIdx = (y * actualWidth + x) * 4;
            uint8_t r = rgbaData[srcIdx];
            uint8_t g = rgbaData[srcIdx + 1];
            uint8_t b = rgbaData[srcIdx + 2];

            yPlane[y * actualWidth + x] = static_cast<uint8_t>(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);

            if ((y % 2 == 0) && (x % 2 == 0)) {
                int uvIdx = (y / 2) * actualWidth + (x & ~1);
                uvPlane[uvIdx] = static_cast<uint8_t>(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128); // U
                uvPlane[uvIdx + 1] = static_cast<uint8_t>(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);  // V
            }
        }
    }
}

void ObsVirtualCam::syncWorker() {
    uint32_t frameCounter = 0;

    while (isRunning) {
        Sleep(16); // 60 fps

        std::lock_guard<std::mutex> lock(frameMtx);

        auto now = std::chrono::steady_clock::now().time_since_epoch();
        uint64_t ds_timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count() / 100;

        for (auto& dev : devices) {
            if (!dev.pBuffer) continue;

            if (dev.hMutex) WaitForSingleObject(dev.hMutex, 5);

            obs_video_info_shmem* hdr = reinterpret_cast<obs_video_info_shmem*>(dev.pBuffer);
            hdr->magic = 0x5643414D; // 'VCAM'
            hdr->width = frameWidth;
            hdr->height = frameHeight;
            hdr->format = 1;         // VIDEO_FORMAT_NV12
            hdr->timestamp = ds_timestamp;
            hdr->frame_index = ++frameCounter;
            hdr->linesize[0] = frameWidth;
            hdr->linesize[1] = frameWidth;
            hdr->linesize[2] = 0;
            hdr->linesize[3] = 0;

            uint8_t* dst = dev.pBuffer + sizeof(obs_video_info_shmem);
            memcpy(dst, latestFrameBuffer.data(), frameSizeBytes);

            if (dev.hMutex) ReleaseMutex(dev.hMutex);
            if (dev.hEventSent) SetEvent(dev.hEventSent);
        }
    }
}

void ObsVirtualCam::stop() {
    isRunning = false;
    if (workerThread.joinable()) workerThread.join();

    for (auto& dev : devices) {
        if (dev.pBuffer) UnmapViewOfFile(dev.pBuffer);
        if (dev.hSharedMem) CloseHandle(dev.hSharedMem);
        if (dev.hEventSent) CloseHandle(dev.hEventSent);
        if (dev.hMutex) CloseHandle(dev.hMutex);
    }
    devices.clear();
}