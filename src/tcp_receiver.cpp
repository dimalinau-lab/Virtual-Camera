#include "tcp_receiver.hpp"
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
// Отключаем алгоритм Нагла
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

int TcpReceiver::receiveNalu(std::vector<uint8_t>& outBuffer) {
    SOCKET sock = m_socket.load();
    if (!m_isConnected.load() || sock == INVALID_SOCKET) return -1;

    // 1. Читаем 4 байта длины с таймаутом
    uint8_t header[4];
    int readBytes = 0;
    while (readBytes < 4) {
        int r = recv(sock, reinterpret_cast<char*>(header + readBytes), 4 - readBytes, 0);
        if (r <= 0) {
            if (r < 0) {
                int err = WSAGetLastError();
                if (err == WSAETIMEDOUT) {
                    // Временная пауза сети Wi-Fi: не рвем сессию, даем циклу повторить попытку
                    return 0;
                }
            }
            m_isConnected = false;
            return -1;
        }
        readBytes += r;
    }

    uint32_t nalSize = (static_cast<uint32_t>(header[0]) << 24) |
        (static_cast<uint32_t>(header[1]) << 16) |
        (static_cast<uint32_t>(header[2]) << 8) |
        static_cast<uint32_t>(header[3]);

    if (nalSize == 0 || nalSize > 10 * 1024 * 1024) {
        m_isConnected = false;
        return -1;
    }

    // FFmpeg libavcodec bitstream parsers require AV_INPUT_BUFFER_PADDING_SIZE (64 bytes) zero padding
    constexpr size_t AV_PADDING = 64;
    size_t totalAlloc = static_cast<size_t>(nalSize) + 4 + AV_PADDING;
    if (outBuffer.size() < totalAlloc) {
        outBuffer.resize(totalAlloc);
    }

    readBytes = 0;
    while (readBytes < static_cast<int>(nalSize)) {
        int r = recv(sock, reinterpret_cast<char*>(outBuffer.data() + readBytes), static_cast<int>(nalSize) - readBytes, 0);
        if (r <= 0) {
            if (r < 0) {
                int err = WSAGetLastError();
                if (err == WSAETIMEDOUT) {
                    continue; // Дочитываем остаток тела кадра при временном джиттере
                }
            }
            m_isConnected = false;
            return -1;
        }
        readBytes += r;
    }

    // Проверяем, содержит ли буфер уже стартовый код Annex-B (0x000001 или 0x00000001)
    const uint8_t* p = outBuffer.data();
    bool hasAnnexB = (nalSize >= 3 && p[0] == 0x00 && p[1] == 0x00 && (p[2] == 0x01 || (nalSize >= 4 && p[2] == 0x00 && p[3] == 0x01)));
    if (hasAnnexB) {
        memset(outBuffer.data() + nalSize, 0, AV_PADDING);
        return static_cast<int>(nalSize);
    }

    // Устаревший формат: добавляем 00 00 00 01 в начало
    memmove(outBuffer.data() + 4, outBuffer.data(), nalSize);
    outBuffer[0] = 0x00;
    outBuffer[1] = 0x00;
    outBuffer[2] = 0x00;
    outBuffer[3] = 0x01;
    memset(outBuffer.data() + nalSize + 4, 0, AV_PADDING);
    return static_cast<int>(nalSize + 4);
}