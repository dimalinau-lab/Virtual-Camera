#include "nvdec_decoder.hpp"
#include <iostream>
#include <cstring>
#include <thread>
#include <algorithm>

NvdecDecoder::NvdecDecoder() {
    av_log_set_level(AV_LOG_QUIET);
}

NvdecDecoder::~NvdecDecoder() {
    cleanup();
}

bool NvdecDecoder::init(int initialWidth, int initialHeight) {
    cleanup();

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    if (!codec) return false;

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) return false;

    m_codecCtx->pix_fmt = AV_PIX_FMT_YUV420P;

    unsigned int threads = std::thread::hardware_concurrency();
    m_codecCtx->thread_count = (threads > 0) ? threads : 4;
    m_codecCtx->thread_type = FF_THREAD_SLICE;

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

void NvdecDecoder::reinit() {
    flush();
}

void NvdecDecoder::flush() {
    if (m_codecCtx) avcodec_flush_buffers(m_codecCtx);
    if (m_swsCtx) {
        sws_freeContext(m_swsCtx);
        m_swsCtx = nullptr;
    }
    m_lastWidth = 0;
    m_lastHeight = 0;
}

void NvdecDecoder::cleanup() {
    if (m_swsCtx) {
        sws_freeContext(m_swsCtx);
        m_swsCtx = nullptr;
    }
    if (m_packet) av_packet_free(&m_packet);
    if (m_frame) av_frame_free(&m_frame);
    if (m_codecCtx) avcodec_free_context(&m_codecCtx);

    m_lastWidth = 0;
    m_lastHeight = 0;
}

void NvdecDecoder::decodeNalu(const uint8_t* data, int size, std::function<void(const uint8_t* bgra, int width, int height)> onFrame) {
    if (!m_codecCtx || !data || size <= 0) return;

    m_packet->data = const_cast<uint8_t*>(data);
    m_packet->size = size;

    int ret = avcodec_send_packet(m_codecCtx, m_packet);
    if (ret < 0) return;

    while (true) {
        ret = avcodec_receive_frame(m_codecCtx, m_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF || ret < 0) break;

        int inW = m_frame->width;
        int inH = m_frame->height;
        if (inW <= 0 || inH <= 0) continue;

        // Если пришел 4K, сразу масштабируем в быстрый промежуточный размер (1920x1080),
        // исключая прогон 33.2 МБ через кэш CPU
        int outW = (inW >= 3840) ? 1920 : inW;
        int outH = (inH >= 2160) ? 1080 : inH;

        size_t bgraSize = static_cast<size_t>(outW) * outH * 4;
        if (m_bgraBuffer.size() != bgraSize) {
            m_bgraBuffer.resize(bgraSize);
        }

        if (!m_swsCtx || m_lastWidth != inW || m_lastHeight != inH) {
            m_swsCtx = sws_getCachedContext(
                m_swsCtx,
                inW, inH, (AVPixelFormat)m_frame->format,
                outW, outH, AV_PIX_FMT_BGRA,
                SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
            );
            m_lastWidth = inW;
            m_lastHeight = inH;
        }

        if (m_swsCtx) {
            uint8_t* dstData[4] = { m_bgraBuffer.data(), nullptr, nullptr, nullptr };
            int dstLinesize[4] = { outW * 4, 0, 0, 0 };

            sws_scale(m_swsCtx, m_frame->data, m_frame->linesize, 0, inH, dstData, dstLinesize);

            if (onFrame) {
                // Передаем готовый BGRA и исходные габариты для телеметрии
                onFrame(m_bgraBuffer.data(), outW, outH);
            }
        }
    }
}