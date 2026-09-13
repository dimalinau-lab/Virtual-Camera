#include "nvdec_decoder.hpp"
#include <iostream>

NvdecDecoder::NvdecDecoder() {
    av_log_set_level(AV_LOG_QUIET);
}

NvdecDecoder::~NvdecDecoder() {
    cleanup();
}

bool NvdecDecoder::init(int initialWidth, int initialHeight) {
    cleanup();

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    if (!codec) {
        return false;
    }

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) {
        return false;
    }

    m_codecCtx->width = initialWidth;
    m_codecCtx->height = initialHeight;
    m_codecCtx->pix_fmt = AV_PIX_FMT_YUV420P;
    m_codecCtx->thread_count = 2;
    m_codecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    m_codecCtx->flags2 |= AV_CODEC_FLAG2_FAST;

    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
        cleanup();
        return false;
    }

    m_frame = av_frame_alloc();
    m_packet = av_packet_alloc();

    return true;
}

void NvdecDecoder::flush() {
    if (m_codecCtx) {
        avcodec_flush_buffers(m_codecCtx);
    }
}

void NvdecDecoder::cleanup() {
    if (m_swsCtx) {
        sws_freeContext(m_swsCtx);
        m_swsCtx = nullptr;
    }
    if (m_packet) {
        av_packet_free(&m_packet);
    }
    if (m_frame) {
        av_frame_free(&m_frame);
    }
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
    }
    m_lastWidth = 0;
    m_lastHeight = 0;
}

void NvdecDecoder::decodeNalu(const uint8_t* data, int size, std::function<void(const uint8_t* bgra, int width, int height)> onFrame) {
    if (!m_codecCtx || !data || size <= 0) return;

    m_packet->data = const_cast<uint8_t*>(data);
    m_packet->size = size;

    if (avcodec_send_packet(m_codecCtx, m_packet) < 0) {
        return;
    }

    while (avcodec_receive_frame(m_codecCtx, m_frame) == 0) {
        int width = m_frame->width;
        int height = m_frame->height;

        if (width <= 0 || height <= 0) continue;

        size_t bgraSize = static_cast<size_t>(width * height * 4);
        if (m_bgraBuffer.size() != bgraSize) {
            m_bgraBuffer.resize(bgraSize);
        }

        if (!m_swsCtx || m_lastWidth != width || m_lastHeight != height) {
            m_swsCtx = sws_getCachedContext(
                m_swsCtx,
                width, height, (AVPixelFormat)m_frame->format,
                width, height, AV_PIX_FMT_BGRA,
                SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
            );
            m_lastWidth = width;
            m_lastHeight = height;
        }

        if (!m_swsCtx) continue;

        uint8_t* dstData[4] = { m_bgraBuffer.data(), nullptr, nullptr, nullptr };
        int dstLinesize[4] = { width * 4, 0, 0, 0 };

        sws_scale(m_swsCtx, m_frame->data, m_frame->linesize, 0, height, dstData, dstLinesize);

        if (onFrame) {
            onFrame(m_bgraBuffer.data(), width, height);
        }
    }
}