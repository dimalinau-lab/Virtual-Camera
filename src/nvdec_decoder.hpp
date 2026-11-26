#pragma once

#include <vector>
#include <cstdint>
#include <functional>
#include <mutex>
#include <d3d11.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
}

class NvdecDecoder {
public:
    NvdecDecoder();
    ~NvdecDecoder();

    bool init(int initialWidth = 1280, int initialHeight = 720,
              ID3D11Device* sharedD3dDevice = nullptr,
              ID3D11DeviceContext* sharedD3dContext = nullptr,
              std::recursive_mutex* sharedMutex = nullptr);
    void reinit();
    void flush();
    bool onVideoConfig(const uint8_t* data, int size);
    bool fallbackToSoftware();
    void decodeNalu(const uint8_t* data, int size, std::function<void(const uint8_t* bgra, int width, int height)> onFrame);
    void decodeNaluDirect(const uint8_t* data, int size, std::function<void(const AVFrame* frame, int width, int height)> onFrame, bool decodeOnly = false);
    bool transferHwFrameToSw(const AVFrame* hwFrame, const AVFrame** outEffectiveFrame);
    void cleanup();

    bool isHwAccel() const { return m_isHwAccel; }
    bool isUsingSharedD3d11() const { return m_usingSharedD3d11; }
    const char* getHwAccelName() const { return m_hwAccelName; }

private:
    bool initInternal(bool allowHwAccel);

    ID3D11Device* m_initD3dDevice = nullptr;
    ID3D11DeviceContext* m_initD3dContext = nullptr;
    int m_hwErrorStreak = 0;

    AVCodecContext* m_codecCtx = nullptr;
    AVBufferRef* m_hwDeviceCtx = nullptr;
    AVFrame* m_frame = nullptr;
    AVFrame* m_swFrame = nullptr;
    AVPacket* m_packet = nullptr;
    SwsContext* m_swsCtx = nullptr;

    bool m_isHwAccel = false;
    bool m_usingSharedD3d11 = false;
    std::recursive_mutex* m_sharedMutex = nullptr;
    const char* m_hwAccelName = "CPU (Software)";

    std::vector<uint8_t> m_bgraBuffer;
    std::vector<uint8_t> m_lastConfigNalu;
    int m_lastWidth = 0;
    int m_lastHeight = 0;
};