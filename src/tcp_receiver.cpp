#include "tcp_receiver.hpp"
#include "audio_receiver.hpp"
#include "app_state.hpp"
#include <iostream>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

TcpReceiver::TcpReceiver() {
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
}

TcpReceiver::~TcpReceiver() {
    stop();
    WSACleanup();
}

bool TcpReceiver::connectToPhone(const std::string& ip, int port) {
    stop();

    m_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_socket == INVALID_SOCKET) {
        return false;
    }

    // 1. Отключаем алгоритм Нейгла для нулевой сетевой задержки и включаем TCP KeepAlive
    int nodelay = 1;
    setsockopt(m_socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

    int keepalive = 1;
    setsockopt(m_socket, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&keepalive), sizeof(keepalive));

    // Оптимальный буфер сокета (256 КБ) для надежного приема IDR-кадров без накопления скрытой очереди
    int rcvBuf = 256 * 1024;
    setsockopt(m_socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvBuf), sizeof(rcvBuf));

    DWORD timeout = 4000;
    setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
        return false;
    }

    // Неблокирующее подключение с шагом 200 мс, чтобы выход из приложения не зависал на 21 сек
    u_long nonBlocking = 1;
    ioctlsocket(m_socket, FIONBIO, &nonBlocking);
    int connRes = connect(m_socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    bool connected = (connRes == 0);
    if (!connected && WSAGetLastError() == WSAEWOULDBLOCK) {
        for (int i = 0; i < 15 && g_isAppRunning.load(); ++i) {
            fd_set writeSet, errSet;
            FD_ZERO(&writeSet);
            FD_ZERO(&errSet);
            FD_SET(m_socket, &writeSet);
            FD_SET(m_socket, &errSet);
            timeval tv{};
            tv.tv_usec = 200000;
            int sel = select(0, nullptr, &writeSet, &errSet, &tv);
            if (sel > 0) {
                int soErr = 0;
                int optLen = sizeof(soErr);
                getsockopt(m_socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soErr), &optLen);
                if (FD_ISSET(m_socket, &writeSet) && soErr == 0) {
                    connected = true;
                }
                break;
            }
            if (sel < 0) break;
        }
    }
    u_long blocking = 0;
    ioctlsocket(m_socket, FIONBIO, &blocking);

    if (!connected) {
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
        return false;
    }

    m_isConnected = true;
    return true;
}

void TcpReceiver::stop() {
    m_isConnected = false;
    SOCKET s = m_socket.exchange(INVALID_SOCKET);
    if (s != INVALID_SOCKET) {
        shutdown(s, SD_BOTH);
        closesocket(s);
    }
}

bool TcpReceiver::readExact(SOCKET sock, uint8_t* dst, size_t count, bool allowIdleTimeout) {
    size_t total = 0;
    int partialTimeouts = 0;
    while (total < count && m_isConnected.load()) {
        int r = recv(sock, reinterpret_cast<char*>(dst + total), static_cast<int>(count - total), 0);
        if (r > 0) {
            total += static_cast<size_t>(r);
            continue;
        }
        if (r < 0 && WSAGetLastError() == WSAETIMEDOUT) {
            // Если ни одного байта пакета ещё не пришло и разрешён idle timeout (ожидание заголовка), возвращаем false без разрыва
            if (allowIdleTimeout && total == 0) {
                return false;
            }
            // Если часть заголовка или payload уже пошла, дочитываем до конца во избежание рассинхронизации потока
            if (total > 0 && ++partialTimeouts <= 2) {
                continue;
            }
            m_isConnected = false;
            return false;
        }
        m_isConnected = false;
        return false;
    }
    return total == count;
}

int TcpReceiver::receiveNalu(std::vector<uint8_t>& outBuffer, int64_t* outPtsUs, bool* outIsKeyframe, bool* outIsConfig) {
    SOCKET sock = m_socket.load();
    if (!m_isConnected.load() || sock == INVALID_SOCKET) return -1;

    constexpr size_t AV_PADDING = 64;
    if (outIsConfig) *outIsConfig = false;

    while (m_isConnected.load()) {
        uint8_t first2[2];
        if (!readExact(sock, first2, 2, true)) {
            if (!m_isConnected.load()) return -1;
            return 0; // idle timeout до начала пакета
        }

        if (first2[0] == MC_MAGIC_0 && first2[1] == MC_MAGIC_1) {
            // 16-байт заголовок McPacketHeader: уже прочитали 2 байта, дочитываем 14 строго до конца
            uint8_t restHdr[14];
            if (!readExact(sock, restHdr, 14, false)) {
                return -1;
            }

            uint8_t type = restHdr[0];
            uint8_t flags = restHdr[1];
            uint32_t payloadLen = (static_cast<uint32_t>(restHdr[2]) << 24) |
                                  (static_cast<uint32_t>(restHdr[3]) << 16) |
                                  (static_cast<uint32_t>(restHdr[4]) << 8)  |
                                  static_cast<uint32_t>(restHdr[5]);

            uint64_t uPts = (static_cast<uint64_t>(restHdr[6])  << 56) |
                            (static_cast<uint64_t>(restHdr[7])  << 48) |
                            (static_cast<uint64_t>(restHdr[8])  << 40) |
                            (static_cast<uint64_t>(restHdr[9])  << 32) |
                            (static_cast<uint64_t>(restHdr[10]) << 24) |
                            (static_cast<uint64_t>(restHdr[11]) << 16) |
                            (static_cast<uint64_t>(restHdr[12]) << 8)  |
                            static_cast<uint64_t>(restHdr[13]);
            int64_t ptsUs = static_cast<int64_t>(uPts);

            if (payloadLen > 16 * 1024 * 1024) {
                m_isConnected = false;
                return -1;
            }

            if (type == PKT_AUDIO) {
                if (m_payloadBuf.size() < payloadLen) m_payloadBuf.resize(payloadLen);
                if (payloadLen > 0) {
                    if (!readExact(sock, m_payloadBuf.data(), payloadLen)) return -1;
                    AudioReceiver::instance().pushPcmWithPts(m_payloadBuf.data(), payloadLen, ptsUs);
                }
                continue; // переходим к следующему пакету
            }
            else if (type == PKT_STATUS) {
                if (payloadLen > 0) {
                    if (m_payloadBuf.size() < payloadLen) m_payloadBuf.resize(payloadLen);
                    if (!readExact(sock, m_payloadBuf.data(), payloadLen)) return -1;
                    std::string jsonStr(reinterpret_cast<char*>(m_payloadBuf.data()), payloadLen);
                    size_t oPos = jsonStr.find("\"orient\":");
                    if (oPos != std::string::npos) {
                        int orientVal = std::atoi(jsonStr.c_str() + oPos + 9);
                        g_streamOrientation.store(orientVal);
                        bool isLand = (orientVal == 0 || orientVal == 180);
                        g_isLandscapeMode.store(isLand);
                        printf("[PHONE SENSOR] Stream orientation: orient=%d (%s)\n", orientVal, isLand ? "HORIZONTAL 16:9" : "VERTICAL 9:16");
                    }
                    size_t fPos = jsonStr.find("\"facing\":");
                    if (fPos != std::string::npos) {
                        size_t colonPos = jsonStr.find(':', fPos + 8);
                        size_t qStart = (colonPos != std::string::npos) ? jsonStr.find('"', colonPos + 1) : std::string::npos;
                        size_t qEnd = (qStart != std::string::npos) ? jsonStr.find('"', qStart + 1) : std::string::npos;
                        bool isFront = false;
                        if (qStart != std::string::npos && qEnd != std::string::npos) {
                            std::string facingVal = jsonStr.substr(qStart + 1, qEnd - qStart - 1);
                            isFront = (facingVal == "front");
                        }
                        g_isFrontCamera.store(isFront);
                    }
                    size_t fpsPos = jsonStr.find("\"fps\":");
                    if (fpsPos != std::string::npos) {
                        int streamFps = std::atoi(jsonStr.c_str() + fpsPos + 6);
                        // Не понижаем целевой FPS пользователя (например 60) из-за временного переключения на 30 FPS aux-сенсор
                        if (streamFps >= 60 && g_currentFps.load() < 60) {
                            g_currentFps.store(streamFps);
                            g_fpsChanged.store(true);
                        }
                    }
                    if (jsonStr.find("\"camera_switch\":true") != std::string::npos) {
                        g_streamResetRequested.store(true);
                        printf("[PHONE SENSOR] Camera switch event received -> signaling seamless stream reset\n");
                    }
                }
                continue;
            }
            else if (type == PKT_AUDIO_CONFIG) {
                if (payloadLen > 0) {
                    if (m_payloadBuf.size() < payloadLen) m_payloadBuf.resize(payloadLen);
                    if (!readExact(sock, m_payloadBuf.data(), payloadLen)) return -1;
                    if (payloadLen >= 8) {
                        uint32_t sampleRate = (static_cast<uint32_t>(m_payloadBuf[0]) << 24) |
                                              (static_cast<uint32_t>(m_payloadBuf[1]) << 16) |
                                              (static_cast<uint32_t>(m_payloadBuf[2]) << 8)  |
                                              static_cast<uint32_t>(m_payloadBuf[3]);
                        uint16_t channels = (static_cast<uint16_t>(m_payloadBuf[4]) << 8) | static_cast<uint16_t>(m_payloadBuf[5]);
                        if (sampleRate >= 8000 && sampleRate <= 192000 && channels >= 1 && channels <= 2) {
                            AudioReceiver::instance().setSourceFormat(static_cast<int>(sampleRate), static_cast<int>(channels));
                        }
                    }
                }
                continue;
            }
            else if (type == PKT_PING) {
                if (payloadLen > 0) {
                    if (m_payloadBuf.size() < payloadLen) m_payloadBuf.resize(payloadLen);
                    if (!readExact(sock, m_payloadBuf.data(), payloadLen)) return -1;
                }
                continue;
            }
            else if (type == PKT_VIDEO_CONFIG || type == PKT_VIDEO_FRAME) {
                const bool isCfg = (type == PKT_VIDEO_CONFIG);
                if (outIsConfig) *outIsConfig = isCfg;
                if (outPtsUs) *outPtsUs = ptsUs;
                if (outIsKeyframe) *outIsKeyframe = (flags & 0x01) != 0 || isCfg;

                size_t totalAlloc = static_cast<size_t>(payloadLen) + 4 + AV_PADDING;
                if (outBuffer.size() < totalAlloc) {
                    outBuffer.resize(totalAlloc);
                }

                if (payloadLen > 0) {
                    if (!readExact(sock, outBuffer.data() + 4, payloadLen)) return -1;
                }

                // Проверяем Annex-B
                uint8_t* p = outBuffer.data() + 4;
                bool hasAnnexB = (payloadLen >= 3 && p[0] == 0x00 && p[1] == 0x00 &&
                                 (p[2] == 0x01 || (payloadLen >= 4 && p[2] == 0x00 && p[3] == 0x01)));
                if (hasAnnexB) {
                    memmove(outBuffer.data(), outBuffer.data() + 4, payloadLen);
                    memset(outBuffer.data() + payloadLen, 0, AV_PADDING);
                    return static_cast<int>(payloadLen);
                }

                outBuffer[0] = 0x00;
                outBuffer[1] = 0x00;
                outBuffer[2] = 0x00;
                outBuffer[3] = 0x01;
                memset(outBuffer.data() + payloadLen + 4, 0, AV_PADDING);
                return static_cast<int>(payloadLen + 4);
            } else {
                if (payloadLen > 0) {
                    if (m_payloadBuf.size() < payloadLen) m_payloadBuf.resize(payloadLen);
                    if (!readExact(sock, m_payloadBuf.data(), payloadLen)) return -1;
                }
                continue;
            }
        } else {
            // Legacy fallback (first 2 bytes are in first2)
            uint8_t next2[2];
            if (!readExact(sock, next2, 2)) return -1;

            uint32_t nalSize = (static_cast<uint32_t>(first2[0]) << 24) |
                               (static_cast<uint32_t>(first2[1]) << 16) |
                               (static_cast<uint32_t>(next2[0]) << 8)  |
                               static_cast<uint32_t>(next2[1]);

            if (nalSize == 0 || nalSize > 16 * 1024 * 1024) {
                m_isConnected = false;
                return -1;
            }

            size_t totalAlloc = static_cast<size_t>(nalSize) + 4 + AV_PADDING;
            if (outBuffer.size() < totalAlloc) {
                outBuffer.resize(totalAlloc);
            }

            if (!readExact(sock, outBuffer.data() + 4, nalSize)) return -1;

            uint8_t* p = outBuffer.data() + 4;
            bool hasAnnexB = (nalSize >= 3 && p[0] == 0x00 && p[1] == 0x00 &&
                             (p[2] == 0x01 || (nalSize >= 4 && p[2] == 0x00 && p[3] == 0x01)));
            if (hasAnnexB) {
                memmove(outBuffer.data(), outBuffer.data() + 4, nalSize);
                memset(outBuffer.data() + nalSize, 0, AV_PADDING);
                return static_cast<int>(nalSize);
            }

            outBuffer[0] = 0x00;
            outBuffer[1] = 0x00;
            outBuffer[2] = 0x00;
            outBuffer[3] = 0x01;
            memset(outBuffer.data() + nalSize + 4, 0, AV_PADDING);
            return static_cast<int>(nalSize + 4);
        }
    }
    return -1;
}