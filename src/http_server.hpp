#pragma once

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

namespace httplib {
    class Server;
}

bool setupAdbForwards();

class HttpServer {
public:
    HttpServer();
    ~HttpServer();

    bool start(int port = 8000);
    void stop();

    void updatePreviewFrame(const uint8_t* rgbaData, int width, int height);
    void updatePreviewFrameNv12(const uint8_t* nv12Data, int width, int height);
    void updateTelemetry(float fps, const std::string& bitrate, const std::string& codec);
    bool hasPreviewSubscribers() const { return m_previewSubscribers.load(std::memory_order_relaxed) > 0; }

private:
    void serverWorker(int port);
    void previewWorkerLoop();
    bool encodeJpeg(const uint8_t* rgbaData, int width, int height, std::vector<uint8_t>& outJpeg);
    bool encodeJpegNv12(const uint8_t* nv12Data, int width, int height, std::vector<uint8_t>& outJpeg);

    std::atomic<bool> m_isRunning{ false };
    std::thread m_serverThread;
    std::thread m_previewWorkerThread;
    httplib::Server* m_pSvr{ nullptr };

    std::atomic<int> m_previewSubscribers{ 0 };

    // Staging single-slot buffer (Zero-allocation drop-oldest pattern)
    std::mutex m_stageMutex;
    std::condition_variable m_stageCv;
    std::vector<uint8_t> m_stagedNv12;
    std::vector<uint8_t> m_stagedRgba;
    int m_stagedWidth{ 0 };
    int m_stagedHeight{ 0 };
    bool m_stagedIsNv12{ false };
    bool m_hasStagedFrame{ false };

    // Final encoded JPEG distribution with Zero-Stall condition variable
    std::mutex m_frameMutex;
    std::condition_variable m_frameCond;
    uint64_t m_frameSeq{ 0 };
    std::shared_ptr<const std::vector<uint8_t>> m_latestJpegPtr;
    std::vector<uint8_t> m_latestJpeg;

    std::mutex m_telemetryMutex;
    float m_fps{ 0.0f };
    std::string m_bitrate{ "Active" };
    std::string m_codec{ "H.265 / HEVC" };

    std::mutex m_jpegMutex;
    const AVCodec* m_jpegCodec{ nullptr };
    AVCodecContext* m_jpegCtx{ nullptr };
    AVFrame* m_yuvFrame{ nullptr };
    AVPacket* m_pkt{ nullptr };
    SwsContext* m_swsRgbaToYuv{ nullptr };
    SwsContext* m_swsNv12ToYuv{ nullptr };
}; 