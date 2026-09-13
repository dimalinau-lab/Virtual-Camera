#pragma once
#include <windows.h>
#include <iostream>
#include "mf_shared_mem.hpp"

class MfVirtualCamWriter {
public:
    MfVirtualCamWriter() = default;
    ~MfVirtualCamWriter() { stop(); }

    bool start(int width = 720, int height = 1280) {
        m_width = width;
        m_height = height;
        m_frameSize = (size_t)width * height * 3 / 2;
        size_t totalMem = sizeof(MFVirtualCamHeader) + m_frameSize;

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
        hdr->width = width;
        hdr->height = height;
        hdr->strideY = width;
        hdr->strideUV = width;
        hdr->frameSize = (uint32_t)m_frameSize;
        hdr->frameIndex = 0;

        // Инициализация чёрным цветом NV12 (Y=16, UV=128)
        uint8_t* dst = m_pBuffer + sizeof(MFVirtualCamHeader);
        memset(dst, 0x10, (size_t)width * height);
        memset(dst + ((size_t)width * height), 0x80, (size_t)width * height / 2);

        std::cout << "[MF WRITER] Общая память Media Foundation готова (NV12 Portrait " << width << "x" << height << ")\n";
        return true;
    }

    void writeFrameNV12(const uint8_t* nv12Data) {
        if (!m_pBuffer || !nv12Data) return;

        WaitForSingleObject(m_hMutex, 5);

        MFVirtualCamHeader* hdr = reinterpret_cast<MFVirtualCamHeader*>(m_pBuffer);
        uint8_t* dst = m_pBuffer + sizeof(MFVirtualCamHeader);

        memcpy(dst, nv12Data, m_frameSize);
        hdr->frameIndex++;

        ReleaseMutex(m_hMutex);
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
    size_t m_frameSize = 0;
};