#pragma once

#include <vector>
#include <cstdint>
#include <functional>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

class NvdecDecoder {
public:
    NvdecDecoder();
    ~NvdecDecoder();

    bool init(int initialWidth = 1280, int initialHeight = 720);
    void flush();
    void decodeNalu(const uint8_t* data, int size, std::function<void(const uint8_t* bgra, int width, int height)> onFrame);
    void cleanup();

private:
    AVCodecContext* m_codecCtx = nullptr;
    AVFrame* m_frame = nullptr;
    AVPacket* m_packet = nullptr;
    SwsContext* m_swsCtx = nullptr;

    std::vector<uint8_t> m_bgraBuffer;
    int m_lastWidth = 0;
    int m_lastHeight = 0;
};