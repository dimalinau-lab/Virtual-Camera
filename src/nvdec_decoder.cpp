#include "nvdec_decoder.hpp"
#include <iostream>
#include <cstring>
#include <thread>
#include <algorithm>

extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
}

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

static void lockSharedD3d11(void* ctx) {
    if (ctx) static_cast<std::recursive_mutex*>(ctx)->lock();
}

static void unlockSharedD3d11(void* ctx) {
    if (ctx) static_cast<std::recursive_mutex*>(ctx)->unlock();
}

bool NvdecDecoder::init(
    int initialWidth, int initialHeight,
    ID3D11Device* sharedD3dDevice,
    ID3D11DeviceContext* sharedD3dContext,
    std::recursive_mutex* sharedMutex
) {
    (void)initialWidth;
    (void)initialHeight;
    m_initD3dDevice = sharedD3dDevice;
    m_initD3dContext = sharedD3dContext;
    m_sharedMutex = sharedMutex;
    if (initInternal(true)) {
        return true;
    }
    return initInternal(false);
}

bool NvdecDecoder::fallbackToSoftware() {
    if (!m_isHwAccel) return false;
    ID3D11Device* dev = m_initD3dDevice;
    ID3D11DeviceContext* ctx = m_initD3dContext;
    std::recursive_mutex* mtx = m_sharedMutex;
    std::vector<uint8_t> savedCfg = m_lastConfigNalu;
    bool ok = initInternal(false);
    m_initD3dDevice = dev;
    m_initD3dContext = ctx;
    m_sharedMutex = mtx;
    m_lastConfigNalu = std::move(savedCfg);
    if (ok && !m_lastConfigNalu.empty() && m_codecCtx && m_packet) {
        m_packet->data = m_lastConfigNalu.data();
        m_packet->size = static_cast<int>(m_lastConfigNalu.size());
        avcodec_send_packet(m_codecCtx, m_packet);
    }
    return ok;
}

bool NvdecDecoder::onVideoConfig(const uint8_t* data, int size) {
    if (!data || size <= 0) return false;
    bool changed = !m_lastConfigNalu.empty() &&
                   (m_lastConfigNalu.size() != static_cast<size_t>(size) ||
                    memcmp(m_lastConfigNalu.data(), data, static_cast<size_t>(size)) != 0);
    m_lastConfigNalu.assign(data, data + size);
    if (!changed) {
        return false;
    }

    // Reinitialize codec context cleanly on mid-stream VPS/SPS/PPS (resolution) change
    ID3D11Device* dev = m_initD3dDevice;
    ID3D11DeviceContext* ctx = m_initD3dContext;
    std::recursive_mutex* mtx = m_sharedMutex;
    std::vector<uint8_t> savedCfg = m_lastConfigNalu;
    bool wantHw = (dev != nullptr && ctx != nullptr);
    if (!initInternal(wantHw)) {
        initInternal(false);
    }
    m_initD3dDevice = dev;
    m_initD3dContext = ctx;
    m_sharedMutex = mtx;
    m_lastConfigNalu = std::move(savedCfg);
    return true;
}

bool NvdecDecoder::initInternal(bool allowHwAccel) {
    ID3D11Device* sharedD3dDevice = allowHwAccel ? m_initD3dDevice : nullptr;
    ID3D11DeviceContext* sharedD3dContext = allowHwAccel ? m_initD3dContext : nullptr;
    std::recursive_mutex* sharedMutex = m_sharedMutex;
    cleanup();
    m_sharedMutex = sharedMutex;
    m_hwErrorStreak = 0;

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
    if (!codec) return false;

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) return false;

    m_codecCtx->pix_fmt = AV_PIX_FMT_YUV420P;

    unsigned int threads = std::thread::hardware_concurrency();
    m_codecCtx->thread_count = (threads > 0) ? (std::min)(4u, threads) : 2;
    m_codecCtx->thread_type = FF_THREAD_SLICE;

    m_codecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    m_codecCtx->flags2 |= AV_CODEC_FLAG2_FAST;
    m_codecCtx->delay = 0;
    m_codecCtx->has_b_frames = 0;
    m_codecCtx->max_b_frames = 0;
    m_codecCtx->extra_hw_frames = 4;

    m_hwDeviceCtx = nullptr;
    m_isHwAccel = false;
    m_usingSharedD3d11 = false;
    m_hwAccelName = "CPU (Software)";

    if (allowHwAccel && sharedD3dDevice && sharedD3dContext) {
        AVBufferRef* devRef = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (devRef) {
            auto* hwDevCtx = reinterpret_cast<AVHWDeviceContext*>(devRef->data);
            auto* d3d11Ctx = reinterpret_cast<AVD3D11VADeviceContext*>(hwDevCtx->hwctx);
            d3d11Ctx->device = sharedD3dDevice;
            sharedD3dDevice->AddRef();
            d3d11Ctx->device_context = sharedD3dContext;
            sharedD3dContext->AddRef();
            if (sharedMutex) {
                d3d11Ctx->lock_ctx = sharedMutex;
                d3d11Ctx->lock = lockSharedD3d11;
                d3d11Ctx->unlock = unlockSharedD3d11;
            }
            if (av_hwdevice_ctx_init(devRef) >= 0) {
                m_hwDeviceCtx = devRef;
                m_codecCtx->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
                m_codecCtx->get_format = get_hw_format;
                m_isHwAccel = true;
                m_usingSharedD3d11 = true;
                m_hwAccelName = "Direct3D 11 (Zero-Copy D3D11VA)";
            } else {
                av_buffer_unref(&devRef);
            }
        }
    }

    if (allowHwAccel && !m_isHwAccel) {
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
    }

    if (m_isHwAccel) {
        // Аппаратному GPU-декодеру не нужны множественные CPU-потоки (исключает скрытую буферизацию кадров в FFmpeg)
        m_codecCtx->thread_count = 1;
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
    m_lastConfigNalu.clear();
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
    m_usingSharedD3d11 = false;
    m_sharedMutex = nullptr;
    m_hwAccelName = "CPU (Software)";
    m_lastWidth = 0;
    m_lastHeight = 0;
}

void NvdecDecoder::decodeNalu(const uint8_t* data, int size, std::function<void(const uint8_t* bgra, int width, int height)> onFrame) {
    if (!m_codecCtx || !data || size <= 0) return;

    m_packet->data = const_cast<uint8_t*>(data);
    m_packet->size = size;

    int ret = avcodec_send_packet(m_codecCtx, m_packet);
    if (ret < 0) {
        if (m_isHwAccel && ++m_hwErrorStreak >= 15) {
            fallbackToSoftware();
        }
        return;
    }
    m_hwErrorStreak = 0;

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

bool NvdecDecoder::transferHwFrameToSw(const AVFrame* hwFrame, const AVFrame** outEffectiveFrame) {
    if (!hwFrame || !outEffectiveFrame) return false;
    if (hwFrame->format == AV_PIX_FMT_D3D11 || hwFrame->format == AV_PIX_FMT_DXVA2_VLD || hwFrame->format == AV_PIX_FMT_CUDA) {
        if (!m_swFrame) m_swFrame = av_frame_alloc();
        av_frame_unref(m_swFrame);
        if (av_hwframe_transfer_data(m_swFrame, hwFrame, 0) >= 0) {
            m_swFrame->width = hwFrame->width;
            m_swFrame->height = hwFrame->height;
            *outEffectiveFrame = m_swFrame;
            return true;
        }
        return false;
    }
    *outEffectiveFrame = hwFrame;
    return true;
}

void NvdecDecoder::decodeNaluDirect(const uint8_t* data, int size, std::function<void(const AVFrame* frame, int width, int height)> onFrame, bool decodeOnly) {
    if (!m_codecCtx || !data || size <= 0) return;

    m_packet->data = const_cast<uint8_t*>(data);
    m_packet->size = size;

    int ret = avcodec_send_packet(m_codecCtx, m_packet);
    if (ret < 0) {
        if (m_isHwAccel && ++m_hwErrorStreak >= 15) {
            fallbackToSoftware();
        }
        return;
    }
    m_hwErrorStreak = 0;

    while (true) {
        ret = avcodec_receive_frame(m_codecCtx, m_frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF || ret < 0) break;

        if (decodeOnly) {
            av_frame_unref(m_frame);
            continue;
        }

        const AVFrame* effectiveFrame = m_frame;
        bool transferred = false;
        if (m_frame->format == AV_PIX_FMT_D3D11 && m_usingSharedD3d11) {
            effectiveFrame = m_frame;
        } else if (m_frame->format == AV_PIX_FMT_D3D11 || m_frame->format == AV_PIX_FMT_DXVA2_VLD || m_frame->format == AV_PIX_FMT_CUDA) {
            if (!transferHwFrameToSw(m_frame, &effectiveFrame)) {
                av_frame_unref(m_frame);
                continue;
            }
            transferred = true;
        }

        int inW = effectiveFrame->width;
        int inH = effectiveFrame->height;
        if (inW <= 0 || inH <= 0) {
            if (transferred) av_frame_unref(m_swFrame);
            av_frame_unref(m_frame);
            continue;
        }

        if (onFrame) {
            onFrame(effectiveFrame, inW, inH);
        }

        if (transferred || effectiveFrame == m_swFrame) {
            av_frame_unref(m_swFrame);
        }
        av_frame_unref(m_frame);
    }
}