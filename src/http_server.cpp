#ifndef CPPHTTPLIB_NO_OPENSSL
#define CPPHTTPLIB_NO_OPENSSL
#endif

#include "httplib.h"
#include "http_server.hpp"
#include "udp_discovery.hpp"
#include "studio_optics.hpp"
#include "config_manager.hpp"
#include "gui_window.hpp"
#include "update_manager.hpp"
#include "app_state.hpp"
#include "audio_receiver.hpp"
#include "network_helper.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <ws2tcpip.h>

static inline std::string getPhoneHost() {
    std::lock_guard<std::mutex> lock(g_app.targetMutex);
    if (g_targetMode == "usb" || g_targetIp.empty()) {
        return "127.0.0.1";
    }
    return g_targetIp;
}

static inline bool isValidIPv4(const std::string& ip) {
    if (ip.empty() || ip.size() > 15) return false;
    struct in_addr addr{};
    return inet_pton(AF_INET, ip.c_str(), &addr) == 1;
}

static inline httplib::Headers makePhoneAuthHeaders(const std::string& phoneHost) {
    httplib::Headers hdrs;
    std::string tok = ConfigManager::instance().getTokenForIp(phoneHost);
    if (!tok.empty()) {
        hdrs.emplace("X-Auth-Token", tok);
    }
    return hdrs;
}

static inline void syncPhoneConfigOnConnect(const std::string& phoneHost) {
    if (phoneHost.empty()) return;
    std::thread([phoneHost]() {
        try {
            Sleep(150);
            AppConfig cfg = ConfigManager::instance().get();
            std::string resStr = cfg.phone_resolution.empty() ? "1080p" : cfg.phone_resolution;
            int fps = (cfg.phone_fps > 0) ? cfg.phone_fps : 30;
            int bitrateMbps = (std::clamp)(cfg.phone_bitrate, 1, 12);
            int bitrateBps = bitrateMbps * 1000000;

            httplib::Client cli("http://" + phoneHost + ":8080");
            cli.set_connection_timeout(1, 0);
            cli.set_read_timeout(1, 500000);
            std::string path = "/api/config?resolution=" + resStr + "&fps=" + std::to_string(fps) + "&bitrate=" + std::to_string(bitrateBps);
            std::string body = "{\"resolution\":\"" + ConfigManager::escapeJsonString(resStr) +
                               "\",\"fps\":" + std::to_string(fps) +
                               ",\"bitrate\":" + std::to_string(bitrateBps) + "}";
            cli.Post(path, makePhoneAuthHeaders(phoneHost), body, "application/json");
        } catch (...) {}
    }).detach();
}

static inline bool isLocalOrigin(const std::string& origin) {
    if (origin.empty()) return false;
    return origin == "http://127.0.0.1:8000" ||
           origin == "http://localhost:8000" ||
           origin == "https://127.0.0.1:8000" ||
           origin == "https://localhost:8000";
}

static std::filesystem::path getConfigFilePath() {
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    return std::filesystem::path(exePath).parent_path() / "config.json";
}

HttpServer::HttpServer() {
    m_jpegCodec = nullptr;
    m_pkt = nullptr;
}

HttpServer::~HttpServer() {
    stop();
    if (m_swsRgbaToYuv) {
        sws_freeContext(m_swsRgbaToYuv);
        m_swsRgbaToYuv = nullptr;
    }
    if (m_swsNv12ToYuv) {
        sws_freeContext(m_swsNv12ToYuv);
        m_swsNv12ToYuv = nullptr;
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

    std::lock_guard<std::mutex> lock(m_jpegMutex);

    int targetFps = g_currentFps.load();
    if (targetFps <= 0) targetFps = 30;

    int previewWidth = (width > height) ? 854 : 480;
    int previewHeight = (width > height) ? 480 : 854;
    previewWidth &= ~1;
    previewHeight &= ~1;

    if (!m_jpegCtx || m_jpegCtx->width != previewWidth || m_jpegCtx->height != previewHeight) {
        if (m_jpegCtx) {
            avcodec_free_context(&m_jpegCtx);
            m_jpegCtx = nullptr;
        }

        m_jpegCtx = avcodec_alloc_context3(m_jpegCodec);
        if (!m_jpegCtx) return false;

        m_jpegCtx->bit_rate = 1500000;
        m_jpegCtx->width = previewWidth;
        m_jpegCtx->height = previewHeight;
        m_jpegCtx->time_base = AVRational{ 1, 30 };
        m_jpegCtx->framerate = AVRational{ 30, 1 };
        m_jpegCtx->pix_fmt = AV_PIX_FMT_YUVJ420P;
        m_jpegCtx->color_range = AVCOL_RANGE_JPEG;
        m_jpegCtx->flags |= AV_CODEC_FLAG_QSCALE;
        m_jpegCtx->global_quality = FF_QP2LAMBDA * 8;

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
        SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
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
    if (m_previewSubscribers.load(std::memory_order_relaxed) <= 0) return;

    static auto s_lastPreviewRgbaTime = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - s_lastPreviewRgbaTime).count();
    if (elapsedMs < 12) return;
    s_lastPreviewRgbaTime = now;

    size_t dataSize = (size_t)width * height * 4;
    {
        std::lock_guard<std::mutex> lock(m_stageMutex);
        if (m_stagedRgba.size() < dataSize) {
            m_stagedRgba.resize(dataSize);
        }
        memcpy(m_stagedRgba.data(), rgbaData, dataSize);
        m_stagedWidth = width;
        m_stagedHeight = height;
        m_stagedIsNv12 = false;
        m_hasStagedFrame = true;
    }
    m_stageCv.notify_one();
}

bool HttpServer::encodeJpegNv12(const uint8_t* nv12Data, int width, int height, std::vector<uint8_t>& outJpeg) {
    if (!nv12Data || width <= 0 || height <= 0 || !m_jpegCodec || !m_pkt) return false;

    std::lock_guard<std::mutex> lock(m_jpegMutex);

    int targetFps = g_currentFps.load();
    if (targetFps <= 0) targetFps = 30;

    int previewWidth = (width > height) ? 854 : 480;
    int previewHeight = (width > height) ? 480 : 854;
    previewWidth &= ~1;
    previewHeight &= ~1;

    if (!m_jpegCtx || m_jpegCtx->width != previewWidth || m_jpegCtx->height != previewHeight) {
        if (m_jpegCtx) {
            avcodec_free_context(&m_jpegCtx);
            m_jpegCtx = nullptr;
        }

        m_jpegCtx = avcodec_alloc_context3(m_jpegCodec);
        if (!m_jpegCtx) return false;

        m_jpegCtx->bit_rate = 1500000;
        m_jpegCtx->width = previewWidth;
        m_jpegCtx->height = previewHeight;
        m_jpegCtx->time_base = AVRational{ 1, 30 };
        m_jpegCtx->framerate = AVRational{ 30, 1 };
        m_jpegCtx->pix_fmt = AV_PIX_FMT_YUVJ420P;
        m_jpegCtx->color_range = AVCOL_RANGE_JPEG;
        m_jpegCtx->flags |= AV_CODEC_FLAG_QSCALE;
        m_jpegCtx->global_quality = FF_QP2LAMBDA * 8;

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

        if (m_swsNv12ToYuv) {
            sws_freeContext(m_swsNv12ToYuv);
            m_swsNv12ToYuv = nullptr;
        }
    }

    if (!m_yuvFrame || !m_yuvFrame->data[0]) return false;

    m_swsNv12ToYuv = sws_getCachedContext(
        m_swsNv12ToYuv,
        width, height, AV_PIX_FMT_NV12,
        previewWidth, previewHeight, AV_PIX_FMT_YUVJ420P,
        SWS_FAST_BILINEAR, nullptr, nullptr, nullptr
    );

    if (!m_swsNv12ToYuv) return false;

    const uint8_t* srcData[4] = { nv12Data, nv12Data + ((size_t)width * height), nullptr, nullptr };
    int srcLinesize[4] = { width, width, 0, 0 };
    sws_scale(m_swsNv12ToYuv, srcData, srcLinesize, 0, height, m_yuvFrame->data, m_yuvFrame->linesize);

    if (avcodec_send_frame(m_jpegCtx, m_yuvFrame) < 0) return false;

    if (avcodec_receive_packet(m_jpegCtx, m_pkt) == 0) {
        outJpeg.assign(m_pkt->data, m_pkt->data + m_pkt->size);
        av_packet_unref(m_pkt);
        return true;
    }

    return false;
}

void HttpServer::updatePreviewFrameNv12(const uint8_t* nv12Data, int width, int height) {
    if (!nv12Data || width <= 0 || height <= 0) return;
    if (m_previewSubscribers.load(std::memory_order_relaxed) <= 0) return;

    static auto s_lastPreviewNv12Time = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - s_lastPreviewNv12Time).count();
    if (elapsedMs < 12) return;
    s_lastPreviewNv12Time = now;

    size_t dataSize = (size_t)width * height * 3 / 2;
    {
        std::lock_guard<std::mutex> lock(m_stageMutex);
        if (m_stagedNv12.size() < dataSize) {
            m_stagedNv12.resize(dataSize);
        }
        memcpy(m_stagedNv12.data(), nv12Data, dataSize);
        m_stagedWidth = width;
        m_stagedHeight = height;
        m_stagedIsNv12 = true;
        m_hasStagedFrame = true;
    }
    m_stageCv.notify_one();
}

void HttpServer::previewWorkerLoop() {
    std::vector<uint8_t> localNv12;
    std::vector<uint8_t> localRgba;
    int localWidth = 0;
    int localHeight = 0;
    bool localIsNv12 = false;
    std::vector<uint8_t> jpeg;

    while (m_isRunning) {
        {
            std::unique_lock<std::mutex> lock(m_stageMutex);
            m_stageCv.wait(lock, [this] {
                return !m_isRunning || m_hasStagedFrame;
            });
            if (!m_isRunning) break;

            localIsNv12 = m_stagedIsNv12;
            localWidth = m_stagedWidth;
            localHeight = m_stagedHeight;
            m_hasStagedFrame = false;

            if (localIsNv12) {
                size_t sz = (size_t)localWidth * localHeight * 3 / 2;
                if (localNv12.size() < sz) localNv12.resize(sz);
                memcpy(localNv12.data(), m_stagedNv12.data(), sz);
            } else {
                size_t sz = (size_t)localWidth * localHeight * 4;
                if (localRgba.size() < sz) localRgba.resize(sz);
                memcpy(localRgba.data(), m_stagedRgba.data(), sz);
            }
        }

        jpeg.clear();
        bool ok = false;
        if (localIsNv12) {
            ok = encodeJpegNv12(localNv12.data(), localWidth, localHeight, jpeg);
        } else {
            ok = encodeJpeg(localRgba.data(), localWidth, localHeight, jpeg);
        }

        if (ok && !jpeg.empty()) {
            auto newPtr = std::make_shared<std::vector<uint8_t>>(std::move(jpeg));
            {
                std::lock_guard<std::mutex> lock(m_frameMutex);
                m_latestJpegPtr = std::move(newPtr);
                m_frameSeq++;
            }
            m_frameCond.notify_all();
        }
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
    if (!m_jpegCodec) {
        m_jpegCodec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    }
    if (!m_pkt) {
        m_pkt = av_packet_alloc();
    }
    m_isRunning = true;
    m_serverThread = std::thread(&HttpServer::serverWorker, this, port);
    m_previewWorkerThread = std::thread(&HttpServer::previewWorkerLoop, this);
    return true;
}

void HttpServer::stop() {
    m_isRunning = false;
    m_stageCv.notify_all();
    m_frameCond.notify_all();
    if (m_pSvr) {
        m_pSvr->stop();
    }
    if (m_previewWorkerThread.joinable()) {
        m_previewWorkerThread.join();
    }
    if (m_serverThread.joinable()) {
        m_serverThread.join();
    }
}

void HttpServer::serverWorker(int port) {
    httplib::Server svr;
    svr.set_socket_options([](socket_t sock) {
        int opt = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
    });
    svr.new_task_queue = [] { return new httplib::ThreadPool(4); };
    svr.set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
        try {
            if (ep) std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            std::cerr << "[HTTP] Exception caught in handler: " << e.what() << "\n";
        } catch (...) {
            std::cerr << "[HTTP] Unknown exception caught in handler\n";
        }
        res.status = 500;
    });
    m_pSvr = &svr;

    // Регистрация проверки авторизации для входящих UDP Wake beacons
    DeviceDiscoveryService::instance().setWakeAuthChecker([](const std::string& ip, const std::string& token) {
        return ConfigManager::instance().isDeviceAuthorized(ip, token);
    });

    // Базовая CSRF-защита: блокируем кросс-сайтовые запросы из посторонних браузерных вкладок
    svr.set_pre_routing_handler([](const httplib::Request& req, httplib::Response& res) {
        if (req.has_header("Sec-Fetch-Site") && req.get_header_value("Sec-Fetch-Site") == "cross-site") {
            res.status = 403;
            res.set_content("{\"status\":\"forbidden\",\"message\":\"Cross-site requests blocked\"}", "application/json");
            return httplib::Server::HandlerResponse::Handled;
        }
        if (req.has_header("Origin") && !isLocalOrigin(req.get_header_value("Origin"))) {
            res.status = 403;
            res.set_content("{\"status\":\"forbidden\",\"message\":\"Invalid Origin\"}", "application/json");
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });

    // Разрешаем CORS только для локального WebView / localhost:8000 (без wildcard "*")
    svr.set_post_routing_handler([](const httplib::Request& req, httplib::Response& res) {
        if (req.has_header("Origin")) {
            const std::string origin = req.get_header_value("Origin");
            if (isLocalOrigin(origin)) {
                res.set_header("Access-Control-Allow-Origin", origin);
                res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
                res.set_header("Access-Control-Allow-Headers", "Content-Type, Accept, Authorization, X-Requested-With, X-Auth-Token");
            }
        }
    });

    svr.Options(".*", [](const httplib::Request&, httplib::Response& res) {
        res.status = 204;
    });

    // 0. Отдача статических HTML файлов скинов напрямую из рабочей папки
    wchar_t exePathBuf[MAX_PATH];
    GetModuleFileNameW(nullptr, exePathBuf, MAX_PATH);
    std::filesystem::path binDirPath = std::filesystem::path(exePathBuf).parent_path();

    auto serveStaticFile = [binDirPath](const std::string& filename, const std::string& mime) {
        return [binDirPath, filename, mime](const httplib::Request&, httplib::Response& res) {
            std::filesystem::path target = binDirPath / "web" / filename;
            if (!std::filesystem::exists(target)) {
                target = binDirPath / filename;
            }
            if (!std::filesystem::exists(target)) {
                target = binDirPath / ".." / "src" / "web" / filename;
            }
            if (!std::filesystem::exists(target)) {
                target = binDirPath / ".." / "src" / filename;
            }
            std::ifstream file(target, std::ios::binary);
            if (file) {
                std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                res.set_content(content, mime);
            } else {
                res.status = 404;
                res.set_content("File not found: " + filename, "text/plain");
            }
        };
    };

    svr.Get("/", [binDirPath, serveStaticFile](const httplib::Request& req, httplib::Response& res) {
        std::string skin = ConfigManager::instance().get().ui_skin;
        if (skin != "index3.html") skin = "index2.html";
        serveStaticFile(skin, "text/html; charset=utf-8")(req, res);
    });

    svr.Get("/index.html", [](const httplib::Request&, httplib::Response& res) {
        res.set_redirect("/index2.html");
    });
    svr.Get("/index2.html", serveStaticFile("index2.html", "text/html; charset=utf-8"));
    svr.Get("/index3.html", serveStaticFile("index3.html", "text/html; charset=utf-8"));
    svr.Get("/tailwind.min.js", serveStaticFile("tailwind.min.js", "application/javascript; charset=utf-8"));
    svr.Get("/anime.min.js", serveStaticFile("anime.min.js", "application/javascript; charset=utf-8"));
    svr.Get("/translations.js", serveStaticFile("translations.js", "application/javascript; charset=utf-8"));

    svr.Get("/icon.ico", [binDirPath](const httplib::Request&, httplib::Response& res) {
        std::filesystem::path target = binDirPath / "icon.ico";
        std::ifstream file(target, std::ios::binary);
        if (file) {
            std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            res.set_content(content, "image/x-icon");
        } else {
            res.status = 404;
        }
    });

    // 1. Поток предпросмотра (MJPEG) с нулевой задержкой и без дубликатов
    svr.Get("/stream", [this](const httplib::Request&, httplib::Response& res) {
        m_previewSubscribers.fetch_add(1, std::memory_order_relaxed);
        res.set_header("Cache-Control", "no-cache, no-store, must-revalidate");
        res.set_header("Pragma", "no-cache");
        res.set_header("Expires", "0");
        res.set_header("X-Accel-Buffering", "no");

        auto pLastSeq = std::make_shared<uint64_t>(0);

        res.set_chunked_content_provider(
            "multipart/x-mixed-replace; boundary=frame",
            [this, pLastSeq](size_t, httplib::DataSink& sink) {
                try {
                    if (!m_isRunning) return false;
                    if (sink.is_writable && !sink.is_writable()) return false;

                    if (!g_isStreamActive.load()) {
                        Sleep(50);
                        return sink.is_writable ? sink.is_writable() : true;
                    }

                    std::shared_ptr<const std::vector<uint8_t>> framePtr;
                    {
                        std::unique_lock<std::mutex> lock(m_frameMutex);
                        bool gotNew = m_frameCond.wait_for(lock, std::chrono::milliseconds(100), [this, pLastSeq] {
                            return !m_isRunning || m_frameSeq > *pLastSeq;
                        });

                        if (!m_isRunning) return false;
                        if (!gotNew || m_frameSeq == *pLastSeq) {
                            return sink.is_writable ? sink.is_writable() : true;
                        }

                        *pLastSeq = m_frameSeq;
                        framePtr = m_latestJpegPtr;
                    }

                    if (framePtr && !framePtr->empty()) {
                        std::string header = "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " +
                            std::to_string(framePtr->size()) + "\r\n\r\n";
                        if (!sink.write(header.data(), header.size())) return false;
                        if (!sink.write(reinterpret_cast<const char*>(framePtr->data()), framePtr->size())) return false;
                        if (!sink.write("\r\n", 2)) return false;
                    }
                    return true;
                } catch (...) {
                    return false;
                }
            },
            [this](bool) {
                m_previewSubscribers.fetch_sub(1, std::memory_order_relaxed);
            }
        );
    });

    // 2. Статус телефона
    svr.Get("/api/status", [](const httplib::Request&, httplib::Response& res) {
        if (!g_isStreamActive.load()) {
            std::string mac = NetworkHelper::getPrimaryMacAddress();
            std::string ip = NetworkHelper::getPrimaryLocalIp();
            res.set_content("{\"device_name\":\"\",\"url\":\"\",\"pc_mac\":\"" + mac + "\",\"pc_ip\":\"" + ip + "\"}", "application/json");
            return;
        }
        std::string phoneHost = getPhoneHost();
        httplib::Client cli("http://" + phoneHost + ":8080");
        cli.set_connection_timeout(0, 250000);
        cli.set_read_timeout(0, 250000);
        auto phoneRes = cli.Get("/api/status", makePhoneAuthHeaders(phoneHost));
        if (phoneRes && phoneRes->status == 200) {
            if (phoneRes->body.find("\"camera\":\"front\"") != std::string::npos) {
                g_isFrontCamera = true;
            }
            else {
                g_isFrontCamera = false;
            }

            // Автоматическая адаптация ориентации по датчику гравитации телефона (MagerCam protocol)
            size_t oPos = phoneRes->body.find("\"orient\":");
            if (oPos != std::string::npos) {
                int orientVal = std::atoi(phoneRes->body.c_str() + oPos + 9);
                g_streamOrientation.store(orientVal);
                bool isLand = (orientVal == 0 || orientVal == 180);
                g_isLandscapeMode.store(isLand);
            } else if (phoneRes->body.find("\"orientation\":\"vertical\"") != std::string::npos) {
                g_streamOrientation.store(90);
                g_isLandscapeMode.store(false);
            } else if (phoneRes->body.find("\"orientation\":\"horizontal\"") != std::string::npos) {
                g_streamOrientation.store(0);
                g_isLandscapeMode.store(true);
            }

            size_t fpsPos = phoneRes->body.find("\"fps\":");
            if (fpsPos == std::string::npos) {
                fpsPos = phoneRes->body.find("\"current_fps\":");
                if (fpsPos != std::string::npos) fpsPos += 8;
            }
            if (fpsPos != std::string::npos) {
                int streamFps = std::atoi(phoneRes->body.c_str() + fpsPos + 6);
                // Не понижаем целевой FPS пользователя (например 60) из-за временного переключения на 30 FPS aux-сенсор
                if (streamFps >= 60 && g_currentFps.load() < 60) {
                    g_currentFps.store(streamFps);
                    g_fpsChanged.store(true);
                }
            }

            res.set_content(phoneRes->body, "application/json");
        }
        else {
            res.set_content("{\"device_name\":\"Connected Phone\",\"url\":\"tcp://" + phoneHost + ":8554\"}", "application/json");
        }
        });

    // 3. Телеметрия
    svr.Get("/api/telemetry", [this](const httplib::Request&, httplib::Response& res) {
        float fps = 0.0f;
        std::string bitrate;
        std::string codec;
        {
            std::lock_guard<std::mutex> lock(m_telemetryMutex);
            fps = m_fps;
            bitrate = m_bitrate;
            codec = m_codec;
        }
        std::ostringstream ss;
        ss << "{\"fps\":" << fps
           << ",\"bitrate\":\"" << bitrate
           << "\",\"codec\":\"" << codec
           << "\",\"streaming\":" << (g_isStreamActive.load() ? "true" : "false")
           << ",\"mode\":\"" << g_targetMode << "\""
           << ",\"tally\":" << (g_isTallyActive.load() ? "true" : "false")
           << ",\"frozen\":" << (g_isFrozen.load() ? "true" : "false")
           << ",\"pc_mac\":\"" << NetworkHelper::getPrimaryMacAddress() << "\""
           << ",\"pc_ip\":\"" << NetworkHelper::getPrimaryLocalIp() << "\""
           << "}";
        res.set_content(ss.str(), "application/json");
    });

    svr.Post("/api/freeze_frame", [](const httplib::Request& req, httplib::Response& res) {
        bool freezeVal = !g_isFrozen.load();
        if (req.has_param("enabled")) {
            freezeVal = req.get_param_value("enabled") == "true" || req.get_param_value("enabled") == "1";
        }
        g_isFrozen.store(freezeVal);
        res.set_content("{\"status\":\"ok\",\"frozen\":" + std::string(freezeVal ? "true" : "false") + "}", "application/json");
    });

    // 4. Подключение через ADB (USB)
    svr.Post("/api/connect_adb", [](const httplib::Request&, httplib::Response& res) {
        setupAdbForwards();

        httplib::Client cli("http://127.0.0.1:8080");
        cli.set_connection_timeout(0, 400000); // 400 ms таймаут
        cli.set_read_timeout(0, 400000);

        std::string mac = NetworkHelper::getPrimaryMacAddress();
        std::string payload = "{\"mode\":\"usb\",\"pc_mac\":\"" + mac + "\"}";
        auto pRes = cli.Post("/api/connect", payload, "application/json");

        if (pRes && pRes->status == 200) {
            {
                std::lock_guard<std::mutex> lock(g_app.targetMutex);
                g_targetIp = "127.0.0.1";
                g_targetPort = 8554;
                g_targetMode = "usb";
            }
            g_connectRequested = true;
            syncPhoneConfigOnConnect("127.0.0.1");
            res.set_content("{\"status\":\"connected\",\"mode\":\"usb\"}", "application/json");
        }
        else {
            res.set_content("{\"status\":\"error\",\"message\":\"Телефон не отвечает по USB (порт 8080)\"}", "application/json");
        }
        });

    // 5. Подключение через Wi-Fi
    svr.Post("/api/connect", [](const httplib::Request& req, httplib::Response& res) {
        std::string phoneIp = ConfigManager::extractString(req.body, "ip", "");
        std::string authToken = ConfigManager::extractString(req.body, "auth_token", "");

        if (!isValidIPv4(phoneIp)) {
            res.status = 400;
            res.set_content("{\"status\":\"error\",\"message\":\"Valid IPv4 address required\"}", "application/json");
            return;
        }

        std::string devName = phoneIp;
        auto activeDevs = DeviceDiscoveryService::instance().getActiveDevices();
        for (const auto& d : activeDevs) {
            if (d.ip == phoneIp && !d.name.empty()) {
                devName = d.name;
                break;
            }
        }
        if (authToken.empty()) {
            authToken = ConfigManager::instance().getTokenForDevice(phoneIp, devName == phoneIp ? "" : devName);
        }

        httplib::Client cli("http://" + phoneIp + ":8080");
        cli.set_connection_timeout(5, 0); // 5 сек на подключение к Wi-Fi
        cli.set_read_timeout(30, 0);           // 30 сек на подтверждение сопряжения на смартфоне

        std::string mac = NetworkHelper::getPrimaryMacAddress();
        std::string localIp = NetworkHelper::getPrimaryLocalIp();
        std::string payload = "{\"mode\":\"wifi\",\"auth_token\":\"" + ConfigManager::escapeJsonString(authToken) +
                              "\",\"pc_mac\":\"" + ConfigManager::escapeJsonString(mac) +
                              "\",\"pc_ip\":\"" + ConfigManager::escapeJsonString(localIp) + "\"}";
        auto pRes = cli.Post("/api/connect", payload, "application/json");

        if (pRes) {
            if (pRes->status == 200 && pRes->body.find("\"connected\"") != std::string::npos) {
                {
                    std::lock_guard<std::mutex> lock(g_app.targetMutex);
                    g_targetIp = phoneIp;
                    g_targetPort = 8554;
                    g_targetMode = "wifi";
                }
                g_connectRequested = true;

                std::string returnedToken = ConfigManager::extractString(pRes->body, "auth_token", "");
                std::string effectiveToken = !returnedToken.empty() ? returnedToken : authToken;
                if (!effectiveToken.empty()) {
                    ConfigManager::instance().addPairedDevice(devName, phoneIp, effectiveToken);
                    if (authToken.empty()) authToken = effectiveToken;
                }
                syncPhoneConfigOnConnect(phoneIp);

                res.set_content("{\"status\":\"connected\",\"mode\":\"wifi\",\"ip\":\"" + ConfigManager::escapeJsonString(phoneIp) + "\"}", "application/json");
            }
            else if (pRes->status == 401) {
                ConfigManager::instance().removePairedDevice(phoneIp);
                res.set_content("{\"status\":\"unauthorized\",\"message\":\"Device rejected token\"}", "application/json");
            }
            else {
                res.set_content(pRes->body, "application/json");
            }
        }
        else {
            res.set_content("{\"status\":\"error\",\"message\":\"Телефон (" + ConfigManager::escapeJsonString(phoneIp) + ") не отвечает по Wi-Fi\"}", "application/json");
        }
        });

    // 6. Отключение
    svr.Post("/api/disconnect", [](const httplib::Request&, httplib::Response& res) {
        g_isStreamActive = false;
        g_connectRequested = false;

        std::string phoneHost = getPhoneHost();
        if (!phoneHost.empty()) {
            httplib::Headers hdrs = makePhoneAuthHeaders(phoneHost);
            std::thread([phoneHost, hdrs]() {
                try {
                    httplib::Client cli("http://" + phoneHost + ":8080");
                    cli.set_connection_timeout(1, 0);
                    cli.set_read_timeout(1, 0);
                    cli.Post("/api/disconnect", hdrs, "{}", "application/json");
                } catch (...) {}
            }).detach();
        }

        res.set_content("{\"status\":\"disconnected\"}", "application/json");
        });

    // 6.1. Информация об устройстве ПК (MAC-адрес, локальный IP, имя хоста)
    auto handleDeviceInfo = [](const httplib::Request&, httplib::Response& res) {
        std::string mac = NetworkHelper::getPrimaryMacAddress();
        std::string ip = NetworkHelper::getPrimaryLocalIp();
        char hostName[256] = {0};
        gethostname(hostName, sizeof(hostName));

        std::ostringstream ss;
        ss << "{\"status\":\"ok\",\"mac\":\"" << mac << "\",\"ip\":\"" << ip << "\",\"hostname\":\"" << hostName << "\",\"port\":8000}";
        res.set_content(ss.str(), "application/json");
    };
    svr.Get("/api/device_info", handleDeviceInfo);
    svr.Post("/api/device_info", handleDeviceInfo);

    // 6.2. Пробуждение VirtualCam по запросу со смартфона (USB / Wi-Fi)
    auto handleWake = [](const httplib::Request& req, httplib::Response& res) {
        const bool isLoopback = (req.remote_addr == "127.0.0.1" || req.remote_addr == "::1");
        std::string mode = isLoopback ? "usb" : "wifi";
        std::string phoneIp = isLoopback ? "127.0.0.1" : req.remote_addr;

        // Извлечение auth_token из запроса пробуждения (если передан)
        std::string authToken = ConfigManager::extractString(req.body, "auth_token", "");

        // Проверка авторизации/сопряжения устройства для Wi-Fi (отклоняем посторонние устройства и подмену IP)
        if (!isLoopback) {
            if (!isValidIPv4(phoneIp) || !ConfigManager::instance().isDeviceAuthorized(phoneIp, authToken)) {
                res.status = 401;
                res.set_content("{\"status\":\"unauthorized\",\"message\":\"Device not paired or unauthorized\"}", "application/json");
                return;
            }
        }

        if (mode == "usb") {
            setupAdbForwards();
            {
                std::lock_guard<std::mutex> lock(g_app.targetMutex);
                g_targetIp = "127.0.0.1";
                g_targetPort = 8554;
                g_targetMode = "usb";
            }
        } else {
            {
                std::lock_guard<std::mutex> lock(g_app.targetMutex);
                g_targetIp = phoneIp;
                g_targetPort = 8554;
                g_targetMode = "wifi";
            }
        }
        g_connectRequested = true;

        // Фоновое пробуждение стрима: окно VirtualCam остается в трее
        // Пользователь может открыть его в любой момент кликом по иконке в трее

        std::string mac = NetworkHelper::getPrimaryMacAddress();
        std::ostringstream ss;
        ss << "{\"status\":\"ok\",\"message\":\"VirtualCam awakened\",\"mode\":\"" << ConfigManager::escapeJsonString(mode)
           << "\",\"mac\":\"" << ConfigManager::escapeJsonString(mac) << "\"}";
        res.set_content(ss.str(), "application/json");
    };
    svr.Get("/api/wake", handleWake);
    svr.Post("/api/wake", handleWake);

    // 7. Конфигурация камеры смартфона
    svr.Post("/api/phone/set_config", [](const httplib::Request& req, httplib::Response& res) {
        AppConfig curCfg = ConfigManager::instance().get();
        int newFps = g_currentFps.load();
        std::regex reFps(R"(\"fps\"\s*:\s*([0-9]+))");
        std::smatch m;
        if (std::regex_search(req.body, m, reFps)) {
            try { newFps = std::stoi(m[1].str()); } catch (...) {}
        }

        if (g_currentFps.load() != newFps && newFps > 0) {
            g_currentFps.store(newFps);
            g_fpsChanged.store(true);
        }

        std::string resStr = curCfg.phone_resolution.empty() ? "1080p" : curCfg.phone_resolution;
        std::regex reRes(R"(\"resolution\"\s*:\s*\"([^\"]+)\")");
        if (std::regex_search(req.body, m, reRes)) {
            resStr = m[1].str();
        }

        int bitrate = (std::clamp)(curCfg.phone_bitrate, 1, 12) * 1000000;
        std::regex reBr(R"(\"bitrate\"\s*:\s*([0-9]+))");
        if (std::regex_search(req.body, m, reBr)) {
            try { bitrate = std::stoi(m[1].str()); } catch (...) {}
        }

        ConfigManager::instance().syncPhoneConfig(resStr, newFps, (std::clamp)(bitrate / 1000000, 1, 12));

        if (!g_isStreamActive.load()) {
            res.set_content("{\"status\":\"ok\",\"phone\":\"offline\"}", "application/json");
            return;
        }

        std::string phoneHost = getPhoneHost();
        httplib::Client cli("http://" + phoneHost + ":8080");
        cli.set_connection_timeout(1, 0);
        cli.set_read_timeout(1, 500000);
        std::string path = "/api/config?resolution=" + resStr + "&fps=" + std::to_string(newFps) + "&bitrate=" + std::to_string(bitrate);
        auto pRes = cli.Post(path, makePhoneAuthHeaders(phoneHost), req.body, "application/json");

        if (pRes) {
            res.set_content(pRes->body, "application/json");
        }
        else {
            res.set_content("{\"status\":\"ok\"}", "application/json");
        }
        });

    // 8. Запрос и изменение ориентации (Синхронизация с телефоном + автодатчик)
    auto handleOrientation = [](const httplib::Request& req, httplib::Response& res) {
        std::string reqMode = req.has_param("mode") ? req.get_param_value("mode") : ConfigManager::extractString(req.body, "mode", "");
        if (!reqMode.empty() && g_isStreamActive.load()) {
            if (reqMode == "horizontal" || reqMode == "vertical" || reqMode == "auto") {
                std::string phoneHost = getPhoneHost();
                httplib::Client cli("http://" + phoneHost + ":8080");
                cli.set_connection_timeout(1, 0);
                cli.set_read_timeout(1, 500000);
                auto pRes = cli.Get("/api/orientation?mode=" + reqMode, makePhoneAuthHeaders(phoneHost));
                if (pRes && pRes->status == 200) {
                    res.set_content(pRes->body, "application/json");
                    return;
                }
            }
        }
        int curOrient = g_streamOrientation.load();
        bool isLand = (curOrient == 0 || curOrient == 180);
        res.set_content("{\"status\":\"ok\",\"mode\":\"" + std::string(isLand ? "horizontal" : "vertical") + "\",\"orient\":" + std::to_string(curOrient) + "}", "application/json");
    };
    svr.Get("/api/orientation", handleOrientation);
    svr.Post("/api/orientation", handleOrientation);

    // 8.1. Выбор соотношения сторон (0: 9:16, 1: 4:3, 2: 16:9)
    svr.Get("/api/aspect_ratio", [](const httplib::Request& req, httplib::Response& res) {
        if (req.has_param("mode")) {
            std::string val = req.get_param_value("mode");
            int mode = 0;
            if (val == "9_16" || val == "0" || val == "9:16") mode = 0;
            else if (val == "4_3" || val == "1" || val == "4:3") mode = 1;
            else if (val == "16_9" || val == "2" || val == "16:9") mode = 2;
            else {
                try { mode = std::stoi(val); } catch (...) {}
            }
            g_aspectRatioMode.store(std::clamp(mode, 0, 2));
            ConfigManager::instance().save();
        }
        res.set_content("{\"status\":\"ok\",\"mode\":" + std::to_string(g_aspectRatioMode.load()) + "}", "application/json");
        });

    // 9. Регулировка громкости микрофона
    auto handleAudioVolume = [](const httplib::Request& req, httplib::Response& res) {
        if (req.has_param("val")) {
            try {
                float vol = std::stof(req.get_param_value("val"));
                vol = (std::clamp)(vol, 0.0f, 2.0f);
                g_audioVolume.store(vol);
                AudioReceiver::instance().setVolume(vol);
                ConfigManager::instance().save();
            }
            catch (...) {}
        }
        res.set_content("{\"status\":\"ok\",\"volume\":" + std::to_string(g_audioVolume.load()) + "}", "application/json");
    };
    svr.Get("/api/audio_volume", handleAudioVolume);
    svr.Post("/api/audio_volume", handleAudioVolume);

    // 9.1. Lip-Sync калибровка задержки звука (0 - 500 мс)
    auto handleAudioDelay = [](const httplib::Request& req, httplib::Response& res) {
        if (req.has_param("val")) {
            try {
                int delay = std::stoi(req.get_param_value("val"));
                delay = (std::clamp)(delay, 0, 500);
                g_audioDelayMs.store(delay);
                AudioReceiver::instance().setDelayMs(delay);
                ConfigManager::instance().save();
            }
            catch (...) {}
        }
        res.set_content("{\"status\":\"ok\",\"delay\":" + std::to_string(g_audioDelayMs.load()) + "}", "application/json");
    };
    svr.Get("/api/audio_delay", handleAudioDelay);
    svr.Post("/api/audio_delay", handleAudioDelay);

    // 9.2. Студийный Noise Gate
    auto handleNoiseGate = [](const httplib::Request& req, httplib::Response& res) {
        bool changed = false;
        if (req.has_param("enabled")) {
            std::string en = req.get_param_value("enabled");
            g_noiseGateEnabled.store(en == "true" || en == "1");
            changed = true;
        }
        if (req.has_param("threshold")) {
            try {
                float th = std::stof(req.get_param_value("threshold"));
                g_noiseGateThreshold.store((std::clamp)(th, 0.001f, 0.2f));
                changed = true;
            }
            catch (...) {}
        }
        if (changed) {
            AudioReceiver::instance().setNoiseGate(g_noiseGateEnabled.load(), g_noiseGateThreshold.load());
            ConfigManager::instance().save();
        }
        res.set_content("{\"status\":\"ok\",\"enabled\":" + std::string(g_noiseGateEnabled.load() ? "true" : "false") + ",\"threshold\":" + std::to_string(g_noiseGateThreshold.load()) + "}", "application/json");
    };
    svr.Get("/api/audio_noisegate", handleNoiseGate);
    svr.Post("/api/audio_noisegate", handleNoiseGate);

    // 9.3. Перечисление и выбор устройств ввода звука (Микрофон ПК / Телефон)
    svr.Get("/api/audio/devices", [](const httplib::Request& req, httplib::Response& res) {
        auto devices = AudioReceiver::instance().getAudioDevices();
        std::string currentId = AudioReceiver::instance().getCurrentAudioDevice();
        std::ostringstream ss;
        ss << "[";
        for (size_t i = 0; i < devices.size(); ++i) {
            if (i > 0) ss << ",";
            std::string escapedName;
            for (char c : devices[i].name) {
                if (c == '"' || c == '\\') escapedName += '\\';
                escapedName += c;
            }
            std::string escapedId;
            for (char c : devices[i].id) {
                if (c == '"' || c == '\\') escapedId += '\\';
                escapedId += c;
            }
            bool isSel = (devices[i].id == currentId) || 
                         (currentId.empty() && devices[i].id == "phone");
            ss << "{\"id\":\"" << escapedId << "\",\"name\":\"" << escapedName 
               << "\",\"group\":\"" << devices[i].group
               << "\",\"is_default\":" << (devices[i].isDefault ? "true" : "false")
               << ",\"selected\":" << (isSel ? "true" : "false") << "}";
        }
        ss << "]";
        res.set_content(ss.str(), "application/json");
    });

    auto handleAudioDeviceSelect = [](const httplib::Request& req, httplib::Response& res) {
        if (req.has_param("id")) {
            std::string devId = req.get_param_value("id");
            AudioReceiver::instance().setAudioDevice(devId);
            ConfigManager::instance().save();
        }
        std::string currentId = AudioReceiver::instance().getCurrentAudioDevice();
        res.set_content("{\"status\":\"ok\",\"selected_id\":\"" + currentId + "\"}", "application/json");
    };
    svr.Get("/api/audio/device", handleAudioDeviceSelect);
    svr.Post("/api/audio/device", handleAudioDeviceSelect);

    // 9.4. Перечисление и выбор устройств вывода звука (DirectShow / Windows Default / VB-Cable / Динамики / Наушники)
    svr.Get("/api/audio/output_devices", [](const httplib::Request& req, httplib::Response& res) {
        auto devices = AudioReceiver::instance().getAudioOutputDevices();
        std::string currentId = AudioReceiver::instance().getCurrentAudioOutputDevice();
        std::ostringstream ss;
        ss << "[";
        for (size_t i = 0; i < devices.size(); ++i) {
            if (i > 0) ss << ",";
            std::string escapedName;
            for (char c : devices[i].name) {
                if (c == '"' || c == '\\') escapedName += '\\';
                escapedName += c;
            }
            std::string escapedId;
            for (char c : devices[i].id) {
                if (c == '"' || c == '\\') escapedId += '\\';
                escapedId += c;
            }
            bool isSel = (devices[i].id == currentId) || 
                         (currentId.empty() && devices[i].id == "vcam_only");
            ss << "{\"id\":\"" << escapedId << "\",\"name\":\"" << escapedName 
               << "\",\"group\":\"" << devices[i].group
               << "\",\"is_default\":" << (devices[i].isDefault ? "true" : "false")
               << ",\"selected\":" << (isSel ? "true" : "false") << "}";
        }
        ss << "]";
        res.set_content(ss.str(), "application/json");
    });

    auto handleAudioOutputDeviceSelect = [](const httplib::Request& req, httplib::Response& res) {
        if (req.has_param("id")) {
            std::string devId = req.get_param_value("id");
            AudioReceiver::instance().setAudioOutputDevice(devId);
            ConfigManager::instance().save();
        }
        std::string currentId = AudioReceiver::instance().getCurrentAudioOutputDevice();
        res.set_content("{\"status\":\"ok\",\"selected_id\":\"" + currentId + "\"}", "application/json");
    };
    svr.Get("/api/audio/output_device", handleAudioOutputDeviceSelect);
    svr.Post("/api/audio/output_device", handleAudioOutputDeviceSelect);

    // 10. Действия телефона
    svr.Post("/api/phone/action/(.*)", [](const httplib::Request& req, httplib::Response& res) {
        std::string actionName = req.matches[1];
        if (actionName == "switch_camera") {
            g_isFrontCamera = !g_isFrontCamera.load();
        }
        else if (actionName == "toggle_mic_mute") {
            g_audioMuted = !g_audioMuted.load();
            AudioReceiver::instance().setMute(g_audioMuted.load());
        }
        else if (actionName == "toggle_privacy") {
            g_privacyShield = !g_privacyShield.load();
            res.set_content("{\"status\":\"ok\",\"privacy\":" + std::string(g_privacyShield.load() ? "true" : "false") + "}", "application/json");
            return;
        }

        if (!g_isStreamActive.load()) {
            res.set_content("{\"status\":\"offline\"}", "application/json");
            return;
        }

        std::string phoneHost = getPhoneHost();
        httplib::Client cli("http://" + phoneHost + ":8080");
        cli.set_connection_timeout(1, 500000);
        cli.set_read_timeout(1, 500000);

        std::string body = req.body;
        if (body.empty() || body == "{}") {
            body = "{\"action\":\"" + actionName + "\"}";
        }
        else if (body.find("\"action\"") == std::string::npos) {
            size_t pos = body.find('{');
            if (pos != std::string::npos) {
                body.insert(pos + 1, "\"action\":\"" + actionName + "\",");
            }
        }

        std::string path = "/api/action?action=" + actionName;
        auto pRes = cli.Post(path, makePhoneAuthHeaders(phoneHost), body, "application/json");
        if (pRes) {
            res.set_content(pRes->body, "application/json");
        }
        else {
            res.set_content("{\"status\":\"error\",\"message\":\"Телефон не ответил по адресу " + ConfigManager::escapeJsonString(phoneHost) + "\"}", "application/json");
        }
        });

    // 10.1. Ручная экспозиция камеры
    svr.Post("/api/phone/manual_exposure", [](const httplib::Request& req, httplib::Response& res) {
        if (!g_isStreamActive.load()) {
            res.set_content("{\"status\":\"offline\"}", "application/json");
            return;
        }
        std::string phoneHost = getPhoneHost();
        httplib::Client cli("http://" + phoneHost + ":8080");
        cli.set_connection_timeout(1, 500000);
        cli.set_read_timeout(1, 500000);
        auto pRes = cli.Post("/api/manual_exposure", makePhoneAuthHeaders(phoneHost), req.body, "application/json");
        if (pRes) res.set_content(pRes->body, "application/json");
        else res.set_content("{\"status\":\"error\"}", "application/json");
        });

    // 10.2. Ручной фокус камеры
    svr.Post("/api/phone/manual_focus", [](const httplib::Request& req, httplib::Response& res) {
        if (!g_isStreamActive.load()) {
            res.set_content("{\"status\":\"offline\"}", "application/json");
            return;
        }
        std::string phoneHost = getPhoneHost();
        httplib::Client cli("http://" + phoneHost + ":8080");
        cli.set_connection_timeout(1, 500000);
        cli.set_read_timeout(1, 500000);
        auto pRes = cli.Post("/api/manual_focus", makePhoneAuthHeaders(phoneHost), req.body, "application/json");
        if (pRes) res.set_content(pRes->body, "application/json");
        else res.set_content("{\"status\":\"error\"}", "application/json");
        });

    // 10.3. Режим баланса белого AWB
    svr.Post("/api/phone/awb", [](const httplib::Request& req, httplib::Response& res) {
        if (!g_isStreamActive.load()) {
            res.set_content("{\"status\":\"offline\"}", "application/json");
            return;
        }
        std::string phoneHost = getPhoneHost();
        httplib::Client cli("http://" + phoneHost + ":8080");
        cli.set_connection_timeout(1, 500000);
        cli.set_read_timeout(1, 500000);
        auto pRes = cli.Post("/api/awb", makePhoneAuthHeaders(phoneHost), req.body, "application/json");
        if (pRes) res.set_content(pRes->body, "application/json");
        else res.set_content("{\"status\":\"error\"}", "application/json");
        });

    // 11. Список устройств (Discovery)
    svr.Get("/api/devices", [](const httplib::Request&, httplib::Response& res) {
        DeviceDiscoveryService::instance().sendDiscoveryProbe();
        auto list = DeviceDiscoveryService::instance().getActiveDevices();
        std::ostringstream ss;
        ss << "[";
        for (size_t i = 0; i < list.size(); ++i) {
            ss << "{\"id\":\"" << ConfigManager::escapeJsonString(list[i].id) << "\","
                << "\"name\":\"" << ConfigManager::escapeJsonString(list[i].name) << "\","
                << "\"ip\":\"" << ConfigManager::escapeJsonString(list[i].ip) << "\","
                << "\"port\":" << list[i].port << "}";
            if (i + 1 < list.size()) ss << ",";
        }
        ss << "]";
        res.set_content(ss.str(), "application/json");
        });

    // 12. Сопряжение устройства
    svr.Post("/api/pair_device", [](const httplib::Request& req, httplib::Response& res) {
        std::string ip = req.has_param("ip") ? req.get_param_value("ip") : ConfigManager::extractString(req.body, "ip", "");

        if (!isValidIPv4(ip)) {
            res.status = 400;
            res.set_content("{\"status\":\"error\",\"message\":\"Valid IPv4 address required\"}", "application/json");
            return;
        }

        httplib::Client cli("http://" + ip + ":8080");
        cli.set_connection_timeout(30, 0);
        cli.set_read_timeout(30, 0);

        char compName[MAX_COMPUTERNAME_LENGTH + 1];
        DWORD size = sizeof(compName);
        GetComputerNameA(compName, &size);

        std::string safeName = ConfigManager::escapeJsonString(std::string(compName));
        std::string payload = "{\"client_name\":\"" + safeName + "\",\"client_id\":\"pc-" + safeName + "\"}";
        auto pRes = cli.Post("/api/pair", payload, "application/json");

        if (pRes) {
            if (pRes->status == 200 && pRes->body.find("\"paired\"") != std::string::npos) {
                std::string token = ConfigManager::extractString(pRes->body, "auth_token", "");
                std::string devName = ip;
                auto activeDevs = DeviceDiscoveryService::instance().getActiveDevices();
                for (const auto& d : activeDevs) {
                    if (d.ip == ip && !d.name.empty()) {
                        devName = d.name;
                        break;
                    }
                }
                ConfigManager::instance().addPairedDevice(devName, ip, token);
            }
            res.set_content(pRes->body, "application/json");
        }
        else {
            res.set_content("{\"status\":\"error\",\"message\":\"Phone did not respond\"}", "application/json");
        }
        });

    // 13. Сброс авторизации на телефоне (Забыть) - асинхронно без зависания UI
    svr.Post("/api/unpair_device", [](const httplib::Request& req, httplib::Response& res) {
        std::string ip = req.has_param("ip") ? req.get_param_value("ip") : ConfigManager::extractString(req.body, "ip", "");
        std::string token = req.has_param("token") ? req.get_param_value("token") : ConfigManager::extractString(req.body, "token", "");
        if (token.empty() && !ip.empty()) {
            token = ConfigManager::instance().getTokenForIp(ip);
        }

        if (!ip.empty() && isValidIPv4(ip)) {
            ConfigManager::instance().removePairedDevice(ip);

            char compName[MAX_COMPUTERNAME_LENGTH + 1];
            DWORD size = sizeof(compName);
            GetComputerNameA(compName, &size);
            std::string clientId = "pc-" + std::string(compName);

            std::thread([ip, token, clientId]() {
                try {
                    httplib::Client cli("http://" + ip + ":8080");
                    cli.set_connection_timeout(5, 0); // 5 сек надежный таймаут для выхода смартфона из энергосбережения
                    cli.set_read_timeout(5, 0);
                    httplib::Headers hdrs;
                    if (!token.empty()) hdrs.emplace("X-Auth-Token", token);
                    std::string payload = "{\"token\":\"" + ConfigManager::escapeJsonString(token) +
                                          "\",\"client_id\":\"" + ConfigManager::escapeJsonString(clientId) + "\"}";
                    cli.Post("/api/unpair", hdrs, payload, "application/json");
                } catch (...) {}
            }).detach();
        }
        res.set_content("{\"status\":\"ok\"}", "application/json");
        });

    // 13.1. Список доверенных сопряженных устройств (без раскрытия секретного токена в JS)
    svr.Get("/api/paired_devices", [](const httplib::Request&, httplib::Response& res) {
        auto list = ConfigManager::instance().getPairedDevices();
        std::ostringstream ss;
        ss << "[";
        for (size_t i = 0; i < list.size(); ++i) {
            ss << "{\"name\":\"" << ConfigManager::escapeJsonString(list[i].name)
               << "\",\"ip\":\"" << ConfigManager::escapeJsonString(list[i].ip)
               << "\",\"paired\":" << (!list[i].token.empty() ? "true" : "false") << "}";
            if (i + 1 < list.size()) ss << ",";
        }
        ss << "]";
        res.set_content(ss.str(), "application/json");
        });

    // 14. Настройки UI (применение и сохранение в конфиг)
    svr.Post("/api/settings", [](const httplib::Request& req, httplib::Response& res) {
        ConfigManager::instance().updateFromJson(req.body);
        res.set_content("{\"status\":\"ok\",\"vcam_active\":true}", "application/json");
        });

    // 14b. Переворот изображения 180°
    auto handleFlip180 = [](const httplib::Request& req, httplib::Response& res) {
        bool changed = false;
        if (req.has_param("enabled")) {
            std::string v = req.get_param_value("enabled");
            g_flip180 = (v == "1" || v == "true");
            changed = true;
        } else if (req.has_param("toggle")) {
            g_flip180 = !g_flip180.load();
            changed = true;
        } else if (!req.body.empty()) {
            if (req.body.find("\"enabled\":true") != std::string::npos || req.body.find("\"flip180\":true") != std::string::npos || req.body.find("\"flip180\": true") != std::string::npos) {
                g_flip180 = true;
                changed = true;
            } else if (req.body.find("\"enabled\":false") != std::string::npos || req.body.find("\"flip180\":false") != std::string::npos || req.body.find("\"flip180\": false") != std::string::npos) {
                g_flip180 = false;
                changed = true;
            }
        }
        if (changed) {
            ConfigManager::instance().save();
        }
        res.set_content("{\"status\":\"ok\",\"flip180\":" + std::string(g_flip180.load() ? "true" : "false") + "}", "application/json");
    };
    svr.Get("/api/flip180", handleFlip180);
    svr.Post("/api/flip180", handleFlip180);

    // 14c. Зеркальное отражение камеры (Mirror / Flip Horizontal)
    auto handleMirror = [](const httplib::Request& req, httplib::Response& res) {
        bool changed = false;
        if (req.has_param("enabled")) {
            std::string v = req.get_param_value("enabled");
            g_mirrorEnabled = (v == "1" || v == "true");
            changed = true;
        } else if (req.has_param("toggle")) {
            g_mirrorEnabled = !g_mirrorEnabled.load();
            changed = true;
        } else if (!req.body.empty()) {
            bool currentVal = g_mirrorEnabled.load();
            bool newVal = ConfigManager::extractBool(req.body, "mirror_enabled",
                          ConfigManager::extractBool(req.body, "enabled",
                          ConfigManager::extractBool(req.body, "mirror", currentVal)));
            if (newVal != currentVal) {
                g_mirrorEnabled = newVal;
                changed = true;
            }
        }
        if (changed) {
            ConfigManager::instance().save();
        }
        res.set_content("{\"status\":\"ok\",\"mirror_enabled\":" + std::string(g_mirrorEnabled.load() ? "true" : "false") + "}", "application/json");
    };
    svr.Get("/api/mirror", handleMirror);
    svr.Post("/api/mirror", handleMirror);

    // 15. Чтение конфига
    svr.Get("/api/get_config", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(ConfigManager::instance().toJson(), "application/json");
        });

    // 16. Запись конфига
    svr.Post("/api/save_file", [](const httplib::Request& req, httplib::Response& res) {
        ConfigManager::instance().updateFromJson(req.body);
        res.set_content("{\"status\":\"ok\"}", "application/json");
        });

    // 16a. Сброс конфига к значениям по умолчанию
    auto handleResetConfig = [](const httplib::Request&, httplib::Response& res) {
        ConfigManager::instance().resetToDefaults();
        StudioOptics::instance().resetAll();
        res.set_content(ConfigManager::instance().toJson(), "application/json");
    };
    svr.Post("/api/reset_config", handleResetConfig);
    svr.Get("/api/reset_config", handleResetConfig);

    // 16c. Навигация скинов интерфейса через C++ хост
    auto handleSkinNavigate = [](const httplib::Request& req, httplib::Response& res) {
        std::string skin = req.has_param("skin") ? req.get_param_value("skin") : "index2.html";
        if (skin != "index3.html") skin = "index2.html";
        int skinId = (skin == "index3.html") ? 3 : 2;
        ConfigManager::instance().updateFromJson("{\"ui_skin\":\"" + skin + "\"}");
        if (g_pWindowInstance && g_pWindowInstance->getHwnd()) {
            PostMessageW(g_pWindowInstance->getHwnd(), WM_APP_NAVIGATE_SKIN, skinId, 0);
        }
        res.set_content("{\"status\":\"ok\",\"skin\":\"" + skin + "\"}", "application/json");
    };
    svr.Get("/api/navigate", handleSkinNavigate);
    svr.Post("/api/navigate", handleSkinNavigate);

    // 16b. Параметры приложения: трей и консоль (GET & POST)
    auto handleAppSettings = [](const httplib::Request& req, httplib::Response& res) {
        bool changed = false;
        if (req.has_param("close_to_tray")) {
            std::string v = req.get_param_value("close_to_tray");
            g_closeToTray = (v == "1" || v == "true");
            changed = true;
        }
        if (req.has_param("show_console")) {
            std::string v = req.get_param_value("show_console");
            bool b = (v == "1" || v == "true");
            g_app.setConsoleVisible(b);
            g_showConsole = b;
            changed = true;
        }
        if (req.has_param("flip180")) {
            std::string v = req.get_param_value("flip180");
            g_flip180 = (v == "1" || v == "true");
            changed = true;
        }
        if (req.has_param("show_hud_stats")) {
            std::string v = req.get_param_value("show_hud_stats");
            bool b = (v == "1" || v == "true");
            ConfigManager::instance().setShowHudStats(b);
        }
        if (req.has_param("auto_check_updates")) {
            std::string v = req.get_param_value("auto_check_updates");
            bool b = (v == "1" || v == "true");
            ConfigManager::instance().setAutoCheckUpdates(b);
        }
        if (req.has_param("start_with_windows")) {
            std::string v = req.get_param_value("start_with_windows");
            bool b = (v == "1" || v == "true");
            ConfigManager::instance().setStartWithWindows(b);
            changed = true;
        }
        if (changed) {
            ConfigManager::instance().save();
        }
        auto cfg = ConfigManager::instance().get();
        std::ostringstream ss;
        ss << "{\"status\":\"ok\",\"close_to_tray\":" << (g_closeToTray.load() ? "true" : "false")
           << ",\"show_console\":" << (g_showConsole.load() ? "true" : "false")
           << ",\"flip180\":" << (g_flip180.load() ? "true" : "false")
           << ",\"show_hud_stats\":" << (cfg.show_hud_stats ? "true" : "false")
           << ",\"auto_check_updates\":" << (cfg.auto_check_updates ? "true" : "false")
           << ",\"start_with_windows\":" << (cfg.start_with_windows ? "true" : "false")
           << ",\"pc_mac\":\"" << NetworkHelper::getPrimaryMacAddress() << "\""
           << ",\"pc_ip\":\"" << NetworkHelper::getPrimaryLocalIp() << "\""
           << "}";
        res.set_content(ss.str(), "application/json");
    };
    svr.Get("/api/app_settings", handleAppSettings);
    svr.Post("/api/app_settings", handleAppSettings);

    // 17.1. Мгновенный запуск звука наводки GSM 2G (Входящий вызов)
    svr.Post("/api/troll/gsm_burst", [](const httplib::Request& req, httplib::Response& res) {
        AudioReceiver::instance().triggerGsmBurst();
        res.set_content("{\"status\":\"ok\",\"burst\":true}", "application/json");
    });

    // 17. Эндпоинт приколов (Troll FX) — 5 FPS, пиксели, глитчи, биткраш, пересвет, GSM 2G, рация, робот, CCTV, лаги
    auto handleTroll = [](const httplib::Request& req, httplib::Response& res) {
        bool changed = false;
        if (req.has_param("fps_5")) {
            g_trollFpsLimit = (req.get_param_value("fps_5") == "1");
            changed = true;
        }
        if (req.has_param("pixelate")) {
            try { g_trollPixelate = std::stoi(req.get_param_value("pixelate")); changed = true; } catch (...) {}
        }
        if (req.has_param("glitch")) {
            g_trollGlitch = (req.get_param_value("glitch") == "1");
            changed = true;
        }
        if (req.has_param("bitcrush")) {
            g_trollBitcrush = (req.get_param_value("bitcrush") == "1");
            changed = true;
        }
        if (req.has_param("overexposure")) {
            g_trollOverexposure = (req.get_param_value("overexposure") == "1");
            changed = true;
        }
        if (req.has_param("gsm_voice")) {
            g_trollGsmVoice = (req.get_param_value("gsm_voice") == "1");
            changed = true;
        }
        if (req.has_param("walkie_talkie")) {
            g_trollWalkieTalkie = (req.get_param_value("walkie_talkie") == "1");
            changed = true;
        }
        if (req.has_param("robot_voice")) {
            g_trollRobotVoice = (req.get_param_value("robot_voice") == "1");
            changed = true;
        }
        if (req.has_param("cctv")) {
            g_trollCctv = (req.get_param_value("cctv") == "1");
            changed = true;
        }
        if (req.has_param("fake_lag")) {
            g_trollFakeLag = (req.get_param_value("fake_lag") == "1");
            changed = true;
        }
        if (changed) {
            ConfigManager::instance().save();
        }

        std::ostringstream ss;
        ss << "{\"status\":\"ok\","
           << "\"fps_5\":" << (g_trollFpsLimit.load() ? "1" : "0") << ","
           << "\"pixelate\":" << g_trollPixelate.load() << ","
           << "\"glitch\":" << (g_trollGlitch.load() ? "1" : "0") << ","
           << "\"bitcrush\":" << (g_trollBitcrush.load() ? "1" : "0") << ","
           << "\"overexposure\":" << (g_trollOverexposure.load() ? "1" : "0") << ","
           << "\"gsm_voice\":" << (g_trollGsmVoice.load() ? "1" : "0") << ","
           << "\"walkie_talkie\":" << (g_trollWalkieTalkie.load() ? "1" : "0") << ","
           << "\"robot_voice\":" << (g_trollRobotVoice.load() ? "1" : "0") << ","
           << "\"cctv\":" << (g_trollCctv.load() ? "1" : "0") << ","
           << "\"fake_lag\":" << (g_trollFakeLag.load() ? "1" : "0") << "}";
        res.set_content(ss.str(), "application/json");
    };
    svr.Get("/api/troll", handleTroll);
    svr.Post("/api/troll", handleTroll);

    // 18. Studio Optics: Цветокоррекция & 3D LUT (GET & POST)
    auto handleOpticsColor = [](const httplib::Request& req, httplib::Response& res) {
        auto& optics = StudioOptics::instance();
        if (req.has_param("brightness")) {
            try { optics.setBrightness(std::stoi(req.get_param_value("brightness"))); } catch (...) {}
        }
        if (req.has_param("contrast")) {
            try { optics.setContrast(std::stoi(req.get_param_value("contrast"))); } catch (...) {}
        }
        if (req.has_param("saturation")) {
            try { optics.setSaturation(std::stoi(req.get_param_value("saturation"))); } catch (...) {}
        }
        if (req.has_param("temp")) {
            try { optics.setColorTemp(std::stoi(req.get_param_value("temp"))); } catch (...) {}
        }
        if (req.has_param("preset")) {
            try { optics.setLutPreset(std::stoi(req.get_param_value("preset"))); } catch (...) {}
        }
        ConfigManager::instance().save();
        res.set_content("{\"status\":\"ok\"}", "application/json");
    };
    svr.Get("/api/optics/color", handleOpticsColor);
    svr.Post("/api/optics/color", handleOpticsColor);

    // 19. Studio Optics: Digital Zoom & Framing (GET & POST)
    auto handleOpticsZoom = [](const httplib::Request& req, httplib::Response& res) {
        auto& optics = StudioOptics::instance();
        if (req.has_param("zoom")) {
            try { optics.setZoom(std::stof(req.get_param_value("zoom"))); } catch (...) {}
        }
        if (req.has_param("pan_x") && req.has_param("pan_y")) {
            try {
                optics.setPan(std::stof(req.get_param_value("pan_x")), std::stof(req.get_param_value("pan_y")));
            } catch (...) {}
        }
        ConfigManager::instance().save();
        res.set_content("{\"status\":\"ok\"}", "application/json");
    };
    svr.Get("/api/optics/zoom", handleOpticsZoom);
    svr.Post("/api/optics/zoom", handleOpticsZoom);

    // 20. Studio Optics: Получение текущего состояния
    svr.Get("/api/optics/get", [](const httplib::Request&, httplib::Response& res) {
        auto& optics = StudioOptics::instance();
        std::ostringstream ss;
        ss << "{"
           << "\"brightness\":" << optics.getBrightness() << ","
           << "\"contrast\":" << optics.getContrast() << ","
           << "\"saturation\":" << optics.getSaturation() << ","
           << "\"temp\":" << optics.getColorTemp() << ","
           << "\"preset\":" << optics.getLutPreset() << ","
           << "\"zoom\":" << optics.getZoom() << ","
           << "\"pan_x\":" << optics.getPanX() << ","
           << "\"pan_y\":" << optics.getPanY()
           << "}";
        res.set_content(ss.str(), "application/json");
        });

    // 21. Studio Optics: Сброс настроек к дефолтным (GET & POST)
    auto handleOpticsReset = [](const httplib::Request&, httplib::Response& res) {
        StudioOptics::instance().resetAll();
        res.set_content("{\"status\":\"ok\"}", "application/json");
    };
    svr.Get("/api/optics/reset", handleOpticsReset);
    svr.Post("/api/optics/reset", handleOpticsReset);

    // 22. Переключение оптической линзы смартфона (0.5x, 1x, 2x/3x) (GET & POST)
    auto handlePhoneLens = [](const httplib::Request& req, httplib::Response& res) {
        std::string lens = req.has_param("lens") ? req.get_param_value("lens") : "1x";
        if (!g_isStreamActive.load()) {
            res.set_content("{\"status\":\"ok\",\"lens\":\"" + lens + "\"}", "application/json");
            return;
        }
        std::string phoneHost = getPhoneHost();
        httplib::Client cli("http://" + phoneHost + ":8080");
        cli.set_connection_timeout(1, 500000);
        cli.set_read_timeout(1, 500000);

        std::string body = "{\"action\":\"set_lens\",\"lens\":\"" + ConfigManager::escapeJsonString(lens) + "\"}";
        auto pRes = cli.Post("/api/action", makePhoneAuthHeaders(phoneHost), body, "application/json");
        if (pRes) {
            res.set_content(pRes->body, "application/json");
        } else {
            res.set_content("{\"status\":\"ok\",\"lens\":\"" + ConfigManager::escapeJsonString(lens) + "\"}", "application/json");
        }
    };
    svr.Get("/api/phone/lens", handlePhoneLens);
    svr.Post("/api/phone/lens", handlePhoneLens);

    // 22.1. Аппаратный зум камеры смартфона (Camera2 Zoom Ratio до 100x) (GET & POST)
    auto handlePhoneZoom = [](const httplib::Request& req, httplib::Response& res) {
        std::string ratioStr = req.has_param("ratio") ? req.get_param_value("ratio") : "1.0";
        if (!g_isStreamActive.load()) {
            res.set_content("{\"status\":\"ok\",\"zoom\":" + ratioStr + "}", "application/json");
            return;
        }
        std::string phoneHost = getPhoneHost();
        httplib::Client cli("http://" + phoneHost + ":8080");
        cli.set_connection_timeout(1, 500000);
        cli.set_read_timeout(1, 500000);

        std::string body = "{\"action\":\"set_zoom\",\"ratio\":" + ratioStr + "}";
        auto pRes = cli.Post("/api/action", makePhoneAuthHeaders(phoneHost), body, "application/json");
        if (pRes) {
            res.set_content(pRes->body, "application/json");
        } else {
            res.set_content("{\"status\":\"ok\",\"zoom\":" + ratioStr + "}", "application/json");
        }
    };
    svr.Get("/api/phone/zoom", handlePhoneZoom);
    svr.Post("/api/phone/zoom", handlePhoneZoom);

    // 23. AI Шумоподавление клавиатуры и фона (RNNoise AI)
    auto handleAiNoise = [](const httplib::Request& req, httplib::Response& res) {
        if (req.has_param("enabled")) {
            std::string en = req.get_param_value("enabled");
            g_aiNoiseEnabled.store(en == "true" || en == "1");
            AudioReceiver::instance().setAiNoise(g_aiNoiseEnabled.load());
            ConfigManager::instance().save();
        }
        res.set_content("{\"status\":\"ok\",\"enabled\":" + std::string(g_aiNoiseEnabled.load() ? "true" : "false") + "}", "application/json");
    };
    svr.Get("/api/audio_ai_noise", handleAiNoise);
    svr.Post("/api/audio_ai_noise", handleAiNoise);

    // 24. Auto-Gain Control (AGC)
    auto handleAgc = [](const httplib::Request& req, httplib::Response& res) {
        if (req.has_param("enabled")) {
            std::string en = req.get_param_value("enabled");
            g_agcEnabled.store(en == "true" || en == "1");
            AudioReceiver::instance().setAgc(g_agcEnabled.load());
            ConfigManager::instance().save();
        }
        res.set_content("{\"status\":\"ok\",\"enabled\":" + std::string(g_agcEnabled.load() ? "true" : "false") + "}", "application/json");
    };
    svr.Get("/api/audio_agc", handleAgc);
    svr.Post("/api/audio_agc", handleAgc);

    // 25. Подавление механических щелчков (De-clicker)
    auto handleDeclicker = [](const httplib::Request& req, httplib::Response& res) {
        if (req.has_param("enabled")) {
            std::string en = req.get_param_value("enabled");
            g_declickerEnabled.store(en == "true" || en == "1");
            AudioReceiver::instance().setDeclicker(g_declickerEnabled.load());
            ConfigManager::instance().save();
        }
        res.set_content("{\"status\":\"ok\",\"enabled\":" + std::string(g_declickerEnabled.load() ? "true" : "false") + "}", "application/json");
    };
    svr.Get("/api/audio_declicker", handleDeclicker);
    svr.Post("/api/audio_declicker", handleDeclicker);

    // 26. 3-полосный параметрический EQ
    auto handleAudioEq = [](const httplib::Request& req, httplib::Response& res) {
        bool changed = false;
        if (req.has_param("low")) {
            try { g_eqLowDb.store(std::stof(req.get_param_value("low"))); changed = true; } catch (...) {}
        }
        if (req.has_param("mid")) {
            try { g_eqMidDb.store(std::stof(req.get_param_value("mid"))); changed = true; } catch (...) {}
        }
        if (req.has_param("high")) {
            try { g_eqHighDb.store(std::stof(req.get_param_value("high"))); changed = true; } catch (...) {}
        }
        if (changed) {
            AudioReceiver::instance().setEq(g_eqLowDb.load(), g_eqMidDb.load(), g_eqHighDb.load());
            ConfigManager::instance().save();
        }

        std::ostringstream ss;
        ss << "{\"status\":\"ok\",\"low\":" << g_eqLowDb.load()
           << ",\"mid\":" << g_eqMidDb.load()
           << ",\"high\":" << g_eqHighDb.load() << "}";
        res.set_content(ss.str(), "application/json");
    };
    svr.Get("/api/audio_eq", handleAudioEq);
    svr.Post("/api/audio_eq", handleAudioEq);

    // 27. AI Background Effects (Completely Removed / Disabled)
    auto handleBgEffect = [](const httplib::Request&, httplib::Response& res) {
        res.set_content("{\"status\":\"ok\",\"mode\":0,\"radius\":0}", "application/json");
    };
    svr.Get("/api/bg_effect", handleBgEffect);
    svr.Post("/api/bg_effect", handleBgEffect);

    static std::atomic<int> s_multicamChannel{1};
    static std::string s_multicamIp1 = "";
    static std::string s_multicamIp2 = "";

    auto handleMulticam = [](const httplib::Request& req, httplib::Response& res) {
        if (req.has_param("ip1")) {
            std::string ip1 = req.get_param_value("ip1");
            if (ip1.empty() || isValidIPv4(ip1)) s_multicamIp1 = ip1;
        }
        if (req.has_param("ip2")) {
            std::string ip2 = req.get_param_value("ip2");
            if (ip2.empty() || isValidIPv4(ip2)) s_multicamIp2 = ip2;
        }
        if (req.has_param("channel")) {
            try {
                int ch = std::stoi(req.get_param_value("channel"));
                if (ch == 1 || ch == 2) {
                    s_multicamChannel.store(ch);
                    std::string targetIp = (ch == 1) ? s_multicamIp1 : s_multicamIp2;
                    if (!targetIp.empty() && isValidIPv4(targetIp)) {
                        std::lock_guard<std::mutex> lock(g_app.targetMutex);
                        if (targetIp != g_targetIp) {
                            g_targetIp = targetIp;
                            g_targetPort = 8554;
                            g_targetMode = "wifi";
                            g_connectRequested = true;
                        }
                    }
                }
            } catch (...) {}
        }

        std::ostringstream ss;
        ss << "{\"status\":\"ok\",\"channel\":" << s_multicamChannel.load()
           << ",\"ip1\":\"" << ConfigManager::escapeJsonString(s_multicamIp1) << "\""
           << ",\"ip2\":\"" << ConfigManager::escapeJsonString(s_multicamIp2) << "\"}";
        res.set_content(ss.str(), "application/json");
    };
    svr.Get("/api/multicam", handleMulticam);
    svr.Post("/api/multicam", handleMulticam);

    // 28. Автообновление: Проверка обновлений (GET/POST /api/update/check)
    auto handleUpdateCheck = [](const httplib::Request& req, httplib::Response& res) {
        bool force = req.has_param("force") && (req.get_param_value("force") == "1" || req.get_param_value("force") == "true");
        UpdateManager::instance().checkForUpdatesAsync(force);
        res.set_content(UpdateManager::instance().getStatusJson(), "application/json");
    };
    svr.Get("/api/update/check", handleUpdateCheck);
    svr.Post("/api/update/check", handleUpdateCheck);

    // 29. Автообновление: Текущий статус и прогресс (GET /api/update/status)
    auto handleUpdateStatus = [](const httplib::Request& req, httplib::Response& res) {
        res.set_content(UpdateManager::instance().getStatusJson(), "application/json");
    };
    svr.Get("/api/update/status", handleUpdateStatus);

    // 30. Автообновление: Старт скачивания (POST /api/update/download)
    auto handleUpdateDownload = [](const httplib::Request& req, httplib::Response& res) {
        bool ok = UpdateManager::instance().startDownloadAsync();
        res.set_content(std::string("{\"status\":\"") + (ok ? "started" : "error") + "\"}", "application/json");
    };
    svr.Post("/api/update/download", handleUpdateDownload);

    // 31. Автообновление: Установка и перезапуск (POST /api/update/install)
    auto handleUpdateInstall = [](const httplib::Request& req, httplib::Response& res) {
        bool ok = UpdateManager::instance().installAndQuit();
        res.set_content(std::string("{\"status\":\"") + (ok ? "launching" : "error") + "\"}", "application/json");
    };
    svr.Post("/api/update/install", handleUpdateInstall);

    // 32. Подтверждение одноразового уведомления после обновления
    auto handleUpdateAck = [](const httplib::Request& req, httplib::Response& res) {
        UpdateManager::instance().ackPostUpdateNotice();
        res.set_content("{\"status\":\"ok\"}", "application/json");
    };
    svr.Post("/api/update/ack_notice", handleUpdateAck);

    // Запуск сервера с повторными попытками при TIME_WAIT сокета
    for (int retry = 0; retry < 10; ++retry) {
        if (svr.listen("127.0.0.1", port)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    m_pSvr = nullptr;
}