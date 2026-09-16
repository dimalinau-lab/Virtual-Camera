#pragma once

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
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
    void updateTelemetry(float fps, const std::string& bitrate, const std::string& codec);

private:
    void serverWorker(int port);
    bool encodeJpeg(const uint8_t* rgbaData, int width, int height, std::vector<uint8_t>& outJpeg);

    std::atomic<bool> m_isRunning{ false };
    std::thread m_serverThread;
    httplib::Server* m_pSvr{ nullptr };

    std::mutex m_frameMutex;
    std::vector<uint8_t> m_latestJpeg;

    std::mutex m_telemetryMutex;
    float m_fps{ 0.0f };
    std::string m_bitrate{ "Active" };
    std::string m_codec{ "H.265 / HEVC" };

    const AVCodec* m_jpegCodec{ nullptr };
    AVCodecContext* m_jpegCtx{ nullptr };
    AVFrame* m_yuvFrame{ nullptr };
    AVPacket* m_pkt{ nullptr };
    SwsContext* m_swsRgbaToYuv{ nullptr };
}; 