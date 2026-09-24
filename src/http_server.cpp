#ifndef CPPHTTPLIB_NO_OPENSSL
#define CPPHTTPLIB_NO_OPENSSL
#endif

#include "httplib.h"
#include "http_server.hpp"
#include "udp_discovery.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>

extern std::atomic<bool> g_isStreamActive;
extern std::atomic<bool> g_connectRequested;
extern std::string g_targetIp;
extern int g_targetPort;
extern std::string g_targetMode;
extern std::atomic<int> g_currentWidth;
extern std::atomic<int> g_currentHeight;
extern std::atomic<int> g_currentFps;
extern std::atomic<bool> g_fpsChanged;
extern std::atomic<bool> g_resolutionChanged;

extern std::atomic<bool> g_mirrorEnabled;
extern std::atomic<bool> g_blurEnabled;
extern std::atomic<bool> g_isFrontCamera;
extern std::atomic<bool> g_isLandscapeMode;

// Управление громкостью и мутом микрофона
extern std::atomic<float> g_audioVolume;
extern std::atomic<bool> g_audioMuted;

// Управление эффектами приколов (Troll FX)
extern std::atomic<bool> g_trollFpsLimit;
extern std::atomic<int>  g_trollPixelate;
extern std::atomic<bool> g_trollGlitch;
extern std::atomic<bool> g_trollBitcrush;
extern std::atomic<bool> g_trollOverexposure;

extern bool setupAdbForwards();

static inline std::string getPhoneHost() {
    if (g_targetMode == "usb" || g_targetIp.empty()) {
        return "127.0.0.1";
    }
    return g_targetIp;
}

static std::filesystem::path getConfigFilePath() {
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    return std::filesystem::path(exePath).parent_path() / "config.json";
}

HttpServer::HttpServer() {
    m_jpegCodec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    m_pkt = av_packet_alloc();
}

HttpServer::~HttpServer() {
    stop();
    if (m_swsRgbaToYuv) {
        sws_freeContext(m_swsRgbaToYuv);
        m_swsRgbaToYuv = nullptr;
    }
    if (m_pkt) {
        av_packet_free(&m_pkt);
    }
    if (m_yuvFrame) {
        if (m_yuvFrame->data[0]) {
            av_freep(&m_yuvFrame->data[0]);
        }
        av_frame_free(&m_yuvFrame);
    }
    if (m_jpegCtx) {
        avcodec_free_context(&m_jpegCtx);
    }
}

bool HttpServer::encodeJpeg(const uint8_t* rgbaData, int width, int height, std::vector<uint8_t>& outJpeg) {
    if (!rgbaData || width <= 0 || height <= 0 || !m_jpegCodec || !m_pkt) return false;

    static std::mutex s_encodeMutex;
    std::lock_guard<std::mutex> lock(s_encodeMutex);

    int targetFps = g_currentFps.load();
    if (targetFps <= 0) targetFps = 30;

    int previewWidth = (width > height) ? 960 : 540;
    int previewHeight = (width > height) ? 540 : 960;

    if (!m_jpegCtx || m_jpegCtx->width != previewWidth || m_jpegCtx->height != previewHeight) {
        if (m_jpegCtx) {
            avcodec_free_context(&m_jpegCtx);
            m_jpegCtx = nullptr;
        }

        m_jpegCtx = avcodec_alloc_context3(m_jpegCodec);
        if (!m_jpegCtx) return false;

        m_jpegCtx->bit_rate = 4000000;
        m_jpegCtx->width = previewWidth;
        m_jpegCtx->height = previewHeight;
        m_jpegCtx->time_base = AVRational{ 1, targetFps };
        m_jpegCtx->framerate = AVRational{ targetFps, 1 };
        m_jpegCtx->pix_fmt = AV_PIX_FMT_YUVJ420P;
        m_jpegCtx->color_range = AVCOL_RANGE_JPEG;
        m_jpegCtx->flags |= AV_CODEC_FLAG_QSCALE;
        m_jpegCtx->global_quality = FF_QP2LAMBDA * 5;

        if (avcodec_open2(m_jpegCtx, m_jpegCodec, nullptr) < 0) {
            avcodec_free_context(&m_jpegCtx);
            return false;
        }

        if (m_yuvFrame) {
            if (m_yuvFrame->data[0]) av_freep(&m_yuvFrame->data[0]);
            av_frame_free(&m_yuvFrame);
        }

        m_yuvFrame = av_frame_alloc();
        m_yuvFrame->format = m_jpegCtx->pix_fmt;
        m_yuvFrame->color_range = AVCOL_RANGE_JPEG;
        m_yuvFrame->width = previewWidth;
        m_yuvFrame->height = previewHeight;
        av_image_alloc(m_yuvFrame->data, m_yuvFrame->linesize, previewWidth, previewHeight, m_jpegCtx->pix_fmt, 32);

        if (m_swsRgbaToYuv) {
            sws_freeContext(m_swsRgbaToYuv);
            m_swsRgbaToYuv = nullptr;
        }
    }

    if (!m_yuvFrame || !m_yuvFrame->data[0]) return false;

    m_swsRgbaToYuv = sws_getCachedContext(
        m_swsRgbaToYuv,
        width, height, AV_PIX_FMT_BGRA,
        previewWidth, previewHeight, AV_PIX_FMT_YUVJ420P,
        SWS_POINT, nullptr, nullptr, nullptr
    );

    if (!m_swsRgbaToYuv) return false;

    const uint8_t* srcData[4] = { rgbaData, nullptr, nullptr, nullptr };
    int srcLinesize[4] = { width * 4, 0, 0, 0 };
    sws_scale(m_swsRgbaToYuv, srcData, srcLinesize, 0, height, m_yuvFrame->data, m_yuvFrame->linesize);

    if (avcodec_send_frame(m_jpegCtx, m_yuvFrame) < 0) return false;

    if (avcodec_receive_packet(m_jpegCtx, m_pkt) == 0) {
        outJpeg.assign(m_pkt->data, m_pkt->data + m_pkt->size);
        av_packet_unref(m_pkt);
        return true;
    }

    return false;
}

void HttpServer::updatePreviewFrame(const uint8_t* rgbaData, int width, int height) {
    if (!rgbaData || width <= 0 || height <= 0) return;
    std::vector<uint8_t> jpeg;
    if (encodeJpeg(rgbaData, width, height, jpeg)) {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        m_latestJpeg = std::move(jpeg);
    }
}

void HttpServer::updateTelemetry(float fps, const std::string& bitrate, const std::string& codec) {
    std::lock_guard<std::mutex> lock(m_telemetryMutex);
    m_fps = fps;
    m_bitrate = bitrate;
    m_codec = codec;
}

bool HttpServer::start(int port) {
    if (m_isRunning) return true;
    m_isRunning = true;
    m_serverThread = std::thread(&HttpServer::serverWorker, this, port);
    return true;
}

void HttpServer::stop() {
    m_isRunning = false;
    if (m_pSvr) {
        m_pSvr->stop();
    }
    if (m_serverThread.joinable()) {
        m_serverThread.join();
    }
}

void HttpServer::serverWorker(int port) {
    httplib::Server svr;
    m_pSvr = &svr;

    svr.set_default_headers({
        {"Access-Control-Allow-Origin", "*"},
        {"Access-Control-Allow-Methods", "GET, POST, OPTIONS"},
        {"Access-Control-Allow-Headers", "Content-Type, Accept"}
        });

    svr.Options(".*", [](const httplib::Request&, httplib::Response& res) {
        res.status = 200;
        });

    // 1. Поток предпросмотра (MJPEG)
    svr.Get("/stream", [this](const httplib::Request&, httplib::Response& res) {
        res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
        res.set_header("Pragma", "no-cache");
        res.set_header("Expires", "0");

        res.set_chunked_content_provider(
            "multipart/x-mixed-replace; boundary=frame",
            [this](size_t, httplib::DataSink& sink) {
                if (!m_isRunning || !g_isStreamActive.load()) {
                    Sleep(50);
                    return true;
                }

                std::vector<uint8_t> frameData;
                {
                    std::lock_guard<std::mutex> lock(m_frameMutex);
                    frameData = m_latestJpeg;
                }

                if (!frameData.empty()) {
                    std::string header = "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " +
                        std::to_string(frameData.size()) + "\r\n\r\n";
                    if (!sink.write(header.data(), header.size())) return false;
                    if (!sink.write(reinterpret_cast<const char*>(frameData.data()), frameData.size())) return false;
                    if (!sink.write("\r\n", 2)) return false;
                }

                int fps = g_currentFps.load();
                int sleepMs = (fps >= 60) ? 16 : 33;
                Sleep(sleepMs);
                return true;
            }
        );
        });

    // 2. Статус телефона
    svr.Get("/api/status", [](const httplib::Request&, httplib::Response& res) {
        std::string phoneHost = getPhoneHost();
        httplib::Client cli("http://" + phoneHost + ":8080");
        cli.set_connection_timeout(1, 0);
        cli.set_read_timeout(1, 0);
        auto phoneRes = cli.Get("/api/status");
        if (phoneRes && phoneRes->status == 200) {
            if (phoneRes->body.find("\"camera\":\"front\"") != std::string::npos) {
                g_isFrontCamera = true;
            }
            else {
                g_isFrontCamera = false;
            }
            res.set_content(phoneRes->body, "application/json");
        }
        else {
            if (g_isStreamActive.load()) {
                res.set_content("{\"device_name\":\"Connected Phone\",\"url\":\"tcp://" + phoneHost + ":8554\"}", "application/json");
            }
            else {
                res.set_content("{\"device_name\":\"\",\"url\":\"\"}", "application/json");
            }
        }
        });

    // 3. Телеметрия
    svr.Get("/api/telemetry", [this](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::mutex> lock(m_telemetryMutex);
        std::ostringstream ss;
        ss << "{\"fps\":" << m_fps << ",\"bitrate\":\"" << m_bitrate << "\",\"codec\":\"" << m_codec << "\"}";
        res.set_content(ss.str(), "application/json");
        });

    // 4. Подключение через ADB (USB)
    svr.Post("/api/connect_adb", [](const httplib::Request&, httplib::Response& res) {
        setupAdbForwards();

        httplib::Client cli("http://127.0.0.1:8080");
        cli.set_connection_timeout(2, 0);
        cli.set_read_timeout(2, 0);

        std::string payload = "{\"mode\":\"usb\"}";
        auto pRes = cli.Post("/api/connect", payload, "application/json");

        if (pRes && pRes->status == 200) {
            Sleep(300);
            g_targetIp = "127.0.0.1";
            g_targetPort = 8554;
            g_targetMode = "usb";
            g_connectRequested = true;
            res.set_content("{\"status\":\"connected\",\"mode\":\"usb\"}", "application/json");
        }
        else {
            res.set_content("{\"status\":\"error\",\"message\":\"Телефон не отвечает по USB (порт 8080)\"}", "application/json");
        }
        });

    // 5. Подключение через Wi-Fi
    svr.Post("/api/connect", [](const httplib::Request& req, httplib::Response& res) {
        std::string phoneIp = "";
        std::string authToken = "";

        if (req.body.find("\"ip\":\"") != std::string::npos) {
            size_t start = req.body.find("\"ip\":\"") + 6;
            size_t end = req.body.find("\"", start);
            if (end != std::string::npos) phoneIp = req.body.substr(start, end - start);
        }

        if (req.body.find("\"auth_token\":\"") != std::string::npos) {
            size_t start = req.body.find("\"auth_token\":\"") + 14;
            size_t end = req.body.find("\"", start);
            if (end != std::string::npos) authToken = req.body.substr(start, end - start);
        }

        if (phoneIp.empty()) {
            res.set_content("{\"status\":\"error\",\"message\":\"IP address missing\"}", "application/json");
            return;
        }

        httplib::Client cli("http://" + phoneIp + ":8080");
        cli.set_connection_timeout(3, 0);
        cli.set_read_timeout(3, 0);

        std::string payload = "{\"mode\":\"wifi\",\"auth_token\":\"" + authToken + "\"}";
        auto pRes = cli.Post("/api/connect", payload, "application/json");

        if (pRes) {
            if (pRes->status == 200 && pRes->body.find("\"connected\"") != std::string::npos) {
                Sleep(250);
                g_targetIp = phoneIp;
                g_targetPort = 8554;
                g_targetMode = "wifi";
                g_connectRequested = true;
                res.set_content("{\"status\":\"connected\",\"mode\":\"wifi\",\"ip\":\"" + phoneIp + "\"}", "application/json");
            }
            else if (pRes->status == 401) {
                res.set_content("{\"status\":\"unauthorized\",\"message\":\"Device rejected token\"}", "application/json");
            }
            else {
                res.set_content(pRes->body, "application/json");
            }
        }
        else {
            res.set_content("{\"status\":\"error\",\"message\":\"Phone did not respond\"}", "application/json");
        }
        });

    // 6. Отключение
    svr.Post("/api/disconnect", [](const httplib::Request&, httplib::Response& res) {
        g_isStreamActive = false;
        g_connectRequested = false;

        std::string phoneHost = getPhoneHost();
        httplib::Client cli("http://" + phoneHost + ":8080");
        cli.set_connection_timeout(1, 0);
        cli.Post("/api/disconnect", "{}", "application/json");

        res.set_content("{\"status\":\"disconnected\"}", "application/json");
        });

    // 7. Конфигурация камеры смартфона
    svr.Post("/api/phone/set_config", [](const httplib::Request& req, httplib::Response& res) {
        int newFps = 30;
        if (req.body.find("\"fps\":60") != std::string::npos || req.body.find("\"fps\": 60") != std::string::npos) {
            newFps = 60;
        }

        if (g_currentFps.load() != newFps) {
            g_currentFps.store(newFps);
            g_fpsChanged.store(true);
        }

        std::string phoneHost = getPhoneHost();
        httplib::Client cli("http://" + phoneHost + ":8080");
        cli.set_connection_timeout(2, 0);
        cli.set_read_timeout(3, 0);
        auto pRes = cli.Post("/api/config", req.body, "application/json");

        if (pRes) {
            res.set_content(pRes->body, "application/json");
        }
        else {
            res.set_content("{\"status\":\"ok\"}", "application/json");
        }
        });

    // 8. Переключение ориентации
    svr.Get("/api/orientation", [](const httplib::Request& req, httplib::Response& res) {
        std::string mode = req.has_param("mode") ? req.get_param_value("mode") : "vertical";
        g_isLandscapeMode = (mode == "horizontal");

        res.set_content("{\"status\":\"ok\",\"mode\":\"" + mode + "\"}", "application/json");
        });

    // 9. Регулировка громкости микрофона
    svr.Get("/api/audio_volume", [](const httplib::Request& req, httplib::Response& res) {
        if (req.has_param("val")) {
            try {
                float vol = std::stof(req.get_param_value("val"));
                g_audioVolume.store(vol);
            }
            catch (...) {}
        }
        res.set_content("{\"status\":\"ok\"}", "application/json");
        });

    // 10. Действия телефона
    svr.Post("/api/phone/action/(.*)", [](const httplib::Request& req, httplib::Response& res) {
        std::string actionName = req.matches[1];
        if (actionName == "switch_camera") {
            g_isFrontCamera = !g_isFrontCamera.load();
        }
        else if (actionName == "toggle_mic_mute") {
            g_audioMuted = !g_audioMuted.load();
        }

        std::string phoneHost = getPhoneHost();
        httplib::Client cli("http://" + phoneHost + ":8080");
        cli.set_connection_timeout(1, 500000);
        cli.set_read_timeout(1, 500000);

        std::string body = "{\"action\":\"" + actionName + "\"}";
        auto pRes = cli.Post("/api/action", body, "application/json");
        if (pRes) {
            res.set_content(pRes->body, "application/json");
        }
        else {
            res.set_content("{\"status\":\"error\",\"message\":\"Телефон не ответил по адресу " + phoneHost + "\"}", "application/json");
        }
        });

    // 11. Список устройств (Discovery)
    svr.Get("/api/devices", [](const httplib::Request&, httplib::Response& res) {
        auto list = DeviceDiscoveryService::instance().getActiveDevices();
        std::ostringstream ss;
        ss << "[";
        for (size_t i = 0; i < list.size(); ++i) {
            ss << "{\"id\":\"" << list[i].id << "\","
                << "\"name\":\"" << list[i].name << "\","
                << "\"ip\":\"" << list[i].ip << "\","
                << "\"port\":" << list[i].port << "}";
            if (i + 1 < list.size()) ss << ",";
        }
        ss << "]";
        res.set_content(ss.str(), "application/json");
        });

    // 12. Сопряжение устройства
    svr.Post("/api/pair_device", [](const httplib::Request& req, httplib::Response& res) {
        std::string ip = req.has_param("ip") ? req.get_param_value("ip") : "";
        if (ip.empty()) {
            if (req.body.find("\"ip\":\"") != std::string::npos) {
                size_t start = req.body.find("\"ip\":\"") + 6;
                size_t end = req.body.find("\"", start);
                if (end != std::string::npos) {
                    ip = req.body.substr(start, end - start);
                }
            }
        }

        if (ip.empty()) {
            res.set_content("{\"status\":\"error\",\"message\":\"IP address missing\"}", "application/json");
            return;
        }

        httplib::Client cli("http://" + ip + ":8080");
        cli.set_connection_timeout(25, 0);
        cli.set_read_timeout(25, 0);

        char compName[MAX_COMPUTERNAME_LENGTH + 1];
        DWORD size = sizeof(compName);
        GetComputerNameA(compName, &size);

        std::string payload = "{\"client_name\":\"" + std::string(compName) + "\",\"client_id\":\"pc-" + std::string(compName) + "\"}";
        auto pRes = cli.Post("/api/pair", payload, "application/json");

        if (pRes) {
            res.set_content(pRes->body, "application/json");
        }
        else {
            res.set_content("{\"status\":\"error\",\"message\":\"Phone did not respond\"}", "application/json");
        }
        });

    // 13. Сброс авторизации на телефоне (Забыть)
    svr.Post("/api/unpair_device", [](const httplib::Request& req, httplib::Response& res) {
        std::string ip = req.has_param("ip") ? req.get_param_value("ip") : "";
        std::string token = req.has_param("token") ? req.get_param_value("token") : "";
        if (!ip.empty()) {
            httplib::Client cli("http://" + ip + ":8080");
            cli.set_connection_timeout(3, 0);
            cli.Post("/api/unpair", "{\"token\":\"" + token + "\"}", "application/json");
        }
        res.set_content("{\"status\":\"ok\"}", "application/json");
        });

    // 14. Настройки UI
    svr.Post("/api/settings", [](const httplib::Request& req, httplib::Response& res) {
        if (req.body.find("\"mirror_enabled\":true") != std::string::npos) g_mirrorEnabled = true;
        else if (req.body.find("\"mirror_enabled\":false") != std::string::npos) g_mirrorEnabled = false;

        if (req.body.find("\"blur_enabled\":true") != std::string::npos) g_blurEnabled = true;
        else if (req.body.find("\"blur_enabled\":false") != std::string::npos) g_blurEnabled = false;

        res.set_content("{\"status\":\"ok\",\"vcam_active\":true}", "application/json");
        });

    // 15. Чтение конфига
    svr.Get("/api/get_config", [](const httplib::Request&, httplib::Response& res) {
        std::filesystem::path cfgPath = getConfigFilePath();
        std::ifstream f(cfgPath);
        if (f.is_open()) {
            std::stringstream ss;
            ss << f.rdbuf();
            res.set_content(ss.str(), "application/json");
        }
        else {
            res.set_content("{}", "application/json");
        }
        });

    // 16. Запись конфига
    svr.Post("/api/save_file", [](const httplib::Request& req, httplib::Response& res) {
        std::filesystem::path cfgPath = getConfigFilePath();
        std::ofstream f(cfgPath, std::ios::trunc);
        if (f.is_open()) {
            f << req.body;
            f.close();
            res.set_content("{\"status\":\"ok\"}", "application/json");
        }
        else {
            res.set_content("{\"status\":\"error\"}", "application/json");
        }
        });

    // 17. Эндпоинт приколов (Troll FX) — поддержка 5 FPS, пикселей, глитчей, биткраша и пересвета
    svr.Post("/api/troll", [](const httplib::Request& req, httplib::Response& res) {
        if (req.has_param("fps_5")) {
            g_trollFpsLimit = (req.get_param_value("fps_5") == "1");
        }
        if (req.has_param("pixelate")) {
            try {
                g_trollPixelate = std::stoi(req.get_param_value("pixelate"));
            }
            catch (...) {}
        }
        if (req.has_param("glitch")) {
            g_trollGlitch = (req.get_param_value("glitch") == "1");
        }
        if (req.has_param("bitcrush")) {
            g_trollBitcrush = (req.get_param_value("bitcrush") == "1");
        }
        if (req.has_param("overexposure")) {
            g_trollOverexposure = (req.get_param_value("overexposure") == "1");
        }
        res.set_content("{\"status\":\"ok\"}", "application/json");
        });

    // Запуск сервера
    svr.listen("127.0.0.1", port);
    m_pSvr = nullptr;
}