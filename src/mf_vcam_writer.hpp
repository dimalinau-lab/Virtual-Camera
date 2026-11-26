#pragma once
#include <windows.h>
#include <iostream>
#include <cstdint>
#include "mf_shared_mem.hpp"
#include "app_state.hpp"

class MfVirtualCamWriter {
public:
    MfVirtualCamWriter() = default;
    ~MfVirtualCamWriter() { stop(); }

    static constexpr size_t kMaxFrameSize = 3840ULL * 2160ULL * 3 / 2;

    bool start(int width = 1280, int height = 720, int fps = 30) {
        m_width = (std::min)(width, 3840);
        m_height = (std::min)(height, 2160);
        m_fps = fps > 0 ? fps : 30;
        m_frameDurationHns = 10000000LL / m_fps;
        m_frameSize = (size_t)m_width * m_height * 3 / 2;
        // Pre-allocate 4K-capable shared memory so dynamic 720p/1080p/4K switches require zero handle reallocation
        size_t totalMem = sizeof(MFVirtualCamHeader) + kMaxFrameSize;

        m_hMutex = CreateMutexA(nullptr, FALSE, MF_VCAM_MUTEX_NAME);
        m_hEvent = CreateEventA(nullptr, FALSE, FALSE, MF_VCAM_EVENT_NAME);
        m_hMap = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, (DWORD)totalMem, MF_VCAM_MEM_NAME);

        if (!m_hMap) {
            std::cerr << "[MF WRITER] Ошибка создания памяти: " << GetLastError() << "\n";
            return false;
        }

        m_pBuffer = (uint8_t*)MapViewOfFile(m_hMap, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        if (!m_pBuffer) return false;

        MFVirtualCamHeader* hdr = reinterpret_cast<MFVirtualCamHeader*>(m_pBuffer);
        hdr->magic = 0x4D465643;
        hdr->width = m_width;
        hdr->height = m_height;
        hdr->strideY = m_width;
        hdr->strideUV = m_width;
        hdr->frameSize = (uint32_t)m_frameSize;
        hdr->frameIndex = 0;
        hdr->activeReaders = 0;
        hdr->seq = 0;
        hdr->writerTick = GetTickCount64();
        hdr->wantWidth = m_width;
        hdr->wantHeight = m_height;

        // Инициализация чёрным цветом NV12 (Y=16, UV=128)
        uint8_t* dst = m_pBuffer + sizeof(MFVirtualCamHeader);
        memset(dst, 0x10, (size_t)m_width * m_height);
        memset(dst + ((size_t)m_width * m_height), 0x80, (size_t)m_width * m_height / 2);

        std::cout << "[MF WRITER] Общая память Media Foundation готова (NV12 "
            << m_width << "x" << m_height << " @" << m_fps << " FPS, Seqlock enabled)\n";
        return true;
    }

    void setResolution(int width, int height) {
        if (width < 2 || height < 2 || width > 3840 || height > 2160) return;
        size_t newFrameSize = (size_t)width * height * 3 / 2;
        if (newFrameSize > kMaxFrameSize) return;
        if (width == m_width && height == m_height && m_frameSize == newFrameSize) return;

        m_width = width;
        m_height = height;
        m_frameSize = newFrameSize;

        if (!m_pBuffer) return;

        MFVirtualCamHeader* hdr = reinterpret_cast<MFVirtualCamHeader*>(m_pBuffer);
        uint8_t* dst = m_pBuffer + sizeof(MFVirtualCamHeader);

        InterlockedIncrement((volatile LONG*)&hdr->seq);
        MemoryBarrier();

        hdr->width = width;
        hdr->height = height;
        hdr->strideY = width;
        hdr->strideUV = width;
        hdr->frameSize = (uint32_t)newFrameSize;
        hdr->wantWidth = width;
        hdr->wantHeight = height;
        hdr->writerTick = GetTickCount64();

        memset(dst, 0x10, (size_t)width * height);
        memset(dst + ((size_t)width * height), 0x80, (size_t)width * height / 2);

        MemoryBarrier();
        InterlockedIncrement((volatile LONG*)&hdr->seq);
        InterlockedIncrement(&hdr->frameIndex);
        SetEvent(m_hEvent);
    }

    uint32_t getActiveReaders() const {
        if (!m_pBuffer) return 0;
        const auto* hdr = reinterpret_cast<const MFVirtualCamHeader*>(m_pBuffer);
        return hdr->activeReaders;
    }

    void setFps(int fps) {
        if (fps <= 0) return;
        m_fps = fps;
        m_frameDurationHns = 10000000LL / fps;
    }

    int getFps() const {
        return m_fps;
    }

    int64_t getFrameDurationHns() const {
        return m_frameDurationHns;
    }

    void writeFrameNV12(const uint8_t* nv12Data) {
        if (!m_pBuffer || !nv12Data) return;

        MFVirtualCamHeader* hdr = reinterpret_cast<MFVirtualCamHeader*>(m_pBuffer);
        uint8_t* dst = m_pBuffer + sizeof(MFVirtualCamHeader);

        // Update Tally state from activeReaders (OBS, Discord, Zoom, etc.)
        g_app.isTallyActive.store(hdr->activeReaders > 0, std::memory_order_relaxed);

        // Privacy Shield имеет приоритет над Freeze Frame: чёрный фон должен перекрывать эфир немедленно
        if (g_app.isFrozen.load(std::memory_order_relaxed) && !g_app.privacyShield.load(std::memory_order_relaxed)) {
            hdr->writerTick = GetTickCount64();
            InterlockedIncrement(&hdr->frameIndex);
            SetEvent(m_hEvent);
            return;
        }

        // Seqlock: нечетный seq - идет запись кадра
        InterlockedIncrement((volatile LONG*)&hdr->seq);
        MemoryBarrier();

        hdr->writerTick = GetTickCount64();
        memcpy(dst, nv12Data, m_frameSize);

        MemoryBarrier();
        // Seqlock: четный seq - кадр готов и целостен
        InterlockedIncrement((volatile LONG*)&hdr->seq);
        InterlockedIncrement(&hdr->frameIndex);

        SetEvent(m_hEvent);
    }

    void stop() {
        if (m_pBuffer) { UnmapViewOfFile(m_pBuffer); m_pBuffer = nullptr; }
        if (m_hMap) { CloseHandle(m_hMap); m_hMap = nullptr; }
        if (m_hEvent) { CloseHandle(m_hEvent); m_hEvent = nullptr; }
        if (m_hMutex) { CloseHandle(m_hMutex); m_hMutex = nullptr; }
    }

private:
    HANDLE m_hMap = nullptr;
    HANDLE m_hMutex = nullptr;
    HANDLE m_hEvent = nullptr;
    uint8_t* m_pBuffer = nullptr;
    int m_width = 720;
    int m_height = 1280;
    int m_fps = 30;
    int64_t m_frameDurationHns = 333333LL;
    size_t m_frameSize = 0;
};