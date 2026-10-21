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

static enum AVPixelFormat get_hw_format(AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts) {
    const enum AVPixelFormat* p;
    for (p = pix_fmts; *p != -1; p++) {
        if (*p == AV_PIX_FMT_D3D11 || *p == AV_PIX_FMT_DXVA2_VLD || *p == AV_PIX_FMT_CUDA) {
            return *p;
        }
    }
    return AV_PIX_FMT_NONE;
}

bool NvdecDecoder::init(int initialWidth, int initialHeight) {
    cleanup();

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    if (!codec) return false;

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) return false;

    m_codecCtx->pix_fmt = AV_PIX_FMT_YUV420P;

    unsigned int threads = std::thread::hardware_concurrency();
    m_codecCtx->thread_count = (threads > 0) ? std::min(4u, threads) : 2;
    m_codecCtx->thread_type = FF_THREAD_SLICE;

    m_codecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    m_codecCtx->flags2 |= AV_CODEC_FLAG2_FAST;
    m_codecCtx->delay = 0;
    m_codecCtx->has_b_frames = 0;
    m_codecCtx->max_b_frames = 0;

    // Пытаемся задействовать аппаратное декодирование GPU:
    // 1) D3D11VA (DirectX 11 - универсально для всех современных GPU)
    // 2) DXVA2 (DirectX 9 - совместимость)
    // 3) CUDA (Nvidia NVDEC)
    // Если GPU недоступен — автоматический бесшовный фоллбэк на CPU
    m_hwDeviceCtx = nullptr;
    m_isHwAccel = false;
    m_hwAccelName = "CPU (Software)";

    if (av_hwdevice_ctx_create(&m_hwDeviceCtx, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) >= 0) {
        m_codecCtx->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
        m_codecCtx->get_format = get_hw_format;
        m_isHwAccel = true;
        m_hwAccelName = "Direct3D 11 (D3D11VA)";
    } else if (av_hwdevice_ctx_create(&m_hwDeviceCtx, AV_HWDEVICE_TYPE_DXVA2, nullptr, nullptr, 0) >= 0) {
        m_codecCtx->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
        m_codecCtx->get_format = get_hw_format;
        m_isHwAccel = true;
        m_hwAccelName = "DirectX 9 (DXVA2)";
    } else if (av_hwdevice_ctx_create(&m_hwDeviceCtx, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0) >= 0) {
        m_codecCtx->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
        m_codecCtx->get_format = get_hw_format;
        m_isHwAccel = true;
        m_hwAccelName = "NVIDIA NVDEC (CUDA)";
    }

    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) {
        cleanup();
        return false;
    }

    m_frame = av_frame_alloc();
    m_swFrame = av_frame_alloc();
    m_packet = av_packet_alloc();

    return true;
}

void NvdecDecoder::reinit() {
    flush();
}

void NvdecDecoder::flush() {
    if (m_codecCtx) avcodec_flush_buffers(m_codecCtx);
    if (m_swFrame) av_frame_unref(m_swFrame);
    if (m_frame) av_frame_unref(m_frame);
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
    if (m_swFrame) {
        av_frame_free(&m_swFrame);
        m_swFrame = nullptr;
    }
    if (m_packet) {
        av_packet_free(&m_packet);
        m_packet = nullptr;
    }
    if (m_frame) {
        av_frame_free(&m_frame);
        m_frame = nullptr;
    }
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
        m_codecCtx = nullptr;
    }
    if (m_hwDeviceCtx) {
        av_buffer_unref(&m_hwDeviceCtx);
        m_hwDeviceCtx = nullptr;
    }
    m_isHwAccel = false;
    m_hwAccelName = "CPU (Software)";
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

        const AVFrame* effectiveFrame = m_frame;
        if (m_frame->format == AV_PIX_FMT_D3D11 || m_frame->format == AV_PIX_FMT_DXVA2_VLD || m_frame->format == AV_PIX_FMT_CUDA) {
            if (!m_swFrame) m_swFrame = av_frame_alloc();
            if (av_hwframe_transfer_data(m_swFrame, m_frame, 0) >= 0) {
                m_swFrame->width = m_frame->width;
                m_swFrame->height = m_frame->height;
                effectiveFrame = m_swFrame;
            } else {
                av_frame_unref(m_frame);
                continue;
            }
        }

        int inW = effectiveFrame->width;
        int inH = effectiveFrame->height;
        if (inW <= 0 || inH <= 0) {
            if (effectiveFrame != m_frame) av_frame_unref(m_swFrame);
            av_frame_unref(m_frame);
            continue;
        }

        int outW = 1280;
        int outH = 720;
        size_t bgraSize = static_cast<size_t>(outW) * outH * 4;
        if (m_bgraBuffer.size() != bgraSize) {
            m_bgraBuffer.resize(bgraSize);
        }

        if (!m_swsCtx || m_lastWidth != inW || m_lastHeight != inH) {
            m_swsCtx = sws_getCachedContext(
                m_swsCtx,
                inW, inH, (AVPixelFormat)effectiveFrame->format,
                outW, outH, AV_PIX_FMT_BGRA,
                SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
            );
            m_lastWidth = inW;
            m_lastHeight = inH;
        }

        if (m_swsCtx) {
            uint8_t* dstData[4] = { m_bgraBuffer.data(), nullptr, nullptr, nullptr };
            int dstLinesize[4] = { outW * 4, 0, 0, 0 };

            sws_scale(m_swsCtx, effectiveFrame->data, effectiveFrame->linesize, 0, inH, dstData, dstLinesize);

            if (onFrame) {
                onFrame(m_bgraBuffer.data(), outW, outH);
            }
        }

        if (effectiveFrame != m_frame) {
            av_frame_unref(m_swFrame);
        }
        av_frame_unref(m_frame);
    }
}

void NvdecDecoder::decodeNaluDirect(const uint8_t* data, int size, std::function<void(const AVFrame* frame, int width, int height)> onFrame) {
    if (!m_codecCtx || !data || size <= 0) return;

    m_packet->data = const_cast<uint8_t*>(data);
    m_packet->size = size;

    int ret = avcodec_send_packet(m_codecCtx, m_packet);
    if (ret < 0) return;

    while (true) {
        ret = avcodec_receive_frame(m_codecCtx, m_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF || ret < 0) break;

        const AVFrame* effectiveFrame = m_frame;
        if (m_frame->format == AV_PIX_FMT_D3D11 || m_frame->format == AV_PIX_FMT_DXVA2_VLD || m_frame->format == AV_PIX_FMT_CUDA) {
            if (!m_swFrame) m_swFrame = av_frame_alloc();
            if (av_hwframe_transfer_data(m_swFrame, m_frame, 0) >= 0) {
                m_swFrame->width = m_frame->width;
                m_swFrame->height = m_frame->height;
                effectiveFrame = m_swFrame;
            } else {
                av_frame_unref(m_frame);
                continue;
            }
        }

        int inW = effectiveFrame->width;
        int inH = effectiveFrame->height;
        if (inW <= 0 || inH <= 0) {
            if (effectiveFrame != m_frame) av_frame_unref(m_swFrame);
            av_frame_unref(m_frame);
            continue;
        }

        if (onFrame) {
            onFrame(effectiveFrame, inW, inH);
        }

        if (effectiveFrame != m_frame) {
            av_frame_unref(m_swFrame);
        }
        av_frame_unref(m_frame);
    }
}