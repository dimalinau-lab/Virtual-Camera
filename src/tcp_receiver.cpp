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

    // 1. Отключаем алгоритм Нейгла для нулевой сетевой задержки
    int nodelay = 1;
    setsockopt(m_socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

    // Оптимальный буфер сокета (256 КБ) для надежного приема IDR-кадров без накопления скрытой очереди
    int rcvBuf = 256 * 1024;
    setsockopt(m_socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvBuf), sizeof(rcvBuf));

    DWORD timeout = 8000;
    setsockopt(m_socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

    if (connect(m_socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
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

bool TcpReceiver::readExact(SOCKET sock, uint8_t* dst, size_t count) {
    size_t total = 0;
    while (total < count) {
        int r = recv(sock, reinterpret_cast<char*>(dst + total), static_cast<int>(count - total), 0);
        if (r <= 0) {
            if (r < 0 && WSAGetLastError() == WSAETIMEDOUT) {
                return false;
            }
            m_isConnected = false;
            return false;
        }
        total += r;
    }
    return true;
}

int TcpReceiver::receiveNalu(std::vector<uint8_t>& outBuffer, int64_t* outPtsUs, bool* outIsKeyframe) {
    SOCKET sock = m_socket.load();
    if (!m_isConnected.load() || sock == INVALID_SOCKET) return -1;

    constexpr size_t AV_PADDING = 64;

    while (m_isConnected.load()) {
        uint8_t first2[2];
        if (!readExact(sock, first2, 2)) {
            if (!m_isConnected.load()) return -1;
            return 0; // timeout
        }

        if (first2[0] == MC_MAGIC_0 && first2[1] == MC_MAGIC_1) {
            // 16-байт заголовок McPacketHeader
            uint8_t restHdr[14];
            if (!readExact(sock, restHdr, 14)) {
                if (!m_isConnected.load()) return -1;
                return 0;
            }

            uint8_t type = restHdr[0];
            uint8_t flags = restHdr[1];
            uint32_t payloadLen = (static_cast<uint32_t>(restHdr[2]) << 24) |
                                  (static_cast<uint32_t>(restHdr[3]) << 16) |
                                  (static_cast<uint32_t>(restHdr[4]) << 8)  |
                                  static_cast<uint32_t>(restHdr[5]);

            int64_t ptsUs = (static_cast<int64_t>(restHdr[6])  << 56) |
                            (static_cast<int64_t>(restHdr[7])  << 48) |
                            (static_cast<int64_t>(restHdr[8])  << 40) |
                            (static_cast<int64_t>(restHdr[9])  << 32) |
                            (static_cast<int64_t>(restHdr[10]) << 24) |
                            (static_cast<int64_t>(restHdr[11]) << 16) |
                            (static_cast<int64_t>(restHdr[12]) << 8)  |
                            static_cast<int64_t>(restHdr[13]);

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
                    }
                    size_t fPos = jsonStr.find("\"facing\":");
                    if (fPos != std::string::npos) {
                        bool isFront = (jsonStr.find("\"front\"", fPos) != std::string::npos);
                        g_isFrontCamera.store(isFront);
                    }
                    size_t fpsPos = jsonStr.find("\"fps\":");
                    if (fpsPos != std::string::npos) {
                        int streamFps = std::atoi(jsonStr.c_str() + fpsPos + 6);
                        if (streamFps > 0 && streamFps != g_currentFps.load()) {
                            g_currentFps.store(streamFps);
                            g_fpsChanged.store(true);
                        }
                    }
                }
                continue;
            }
            else if (type == PKT_AUDIO_CONFIG || type == PKT_PING) {
                if (payloadLen > 0) {
                    if (m_payloadBuf.size() < payloadLen) m_payloadBuf.resize(payloadLen);
                    if (!readExact(sock, m_payloadBuf.data(), payloadLen)) return -1;
                }
                continue;
            }
            else if (type == PKT_VIDEO_CONFIG || type == PKT_VIDEO_FRAME) {
                if (outPtsUs) *outPtsUs = ptsUs;
                if (outIsKeyframe) *outIsKeyframe = (flags & 0x01) != 0;

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