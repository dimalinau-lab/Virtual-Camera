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
    int nodelay = 1;
    setsockopt(m_socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));

    // 2. Системный буфер 256 КБ (соответствует размеру sendBufferSize сокета телефона)
    int rcvBuf = 256 * 1024;
    setsockopt(m_socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvBuf), sizeof(rcvBuf));

    DWORD timeout = 2500;
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
    if (m_socket != INVALID_SOCKET) {
        shutdown(m_socket, SD_BOTH);
        closesocket(m_socket);
        m_socket = INVALID_SOCKET;
    }
}

int TcpReceiver::receiveNalu(std::vector<uint8_t>& outBuffer) {
    if (!m_isConnected || m_socket == INVALID_SOCKET) return -1;

    // 1. Читаем 4 байта длины (writeInt в Java отправляет Big-Endian)
    uint8_t header[4];
    int readBytes = 0;
    while (readBytes < 4) {
        int r = recv(m_socket, reinterpret_cast<char*>(header + readBytes), 4 - readBytes, 0);
        if (r <= 0) {
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
        return -1;
    }

    // 2. Резервируем место под 4 байта стартового кода (00 00 00 01) + само тело NALU
    size_t totalPacketSize = 4 + static_cast<size_t>(nalSize);
    if (outBuffer.size() < totalPacketSize) {
        outBuffer.resize(totalPacketSize);
    }

    outBuffer[0] = 0x00;
    outBuffer[1] = 0x00;
    outBuffer[2] = 0x00;
    outBuffer[3] = 0x01;

    // 3. Вычитываем полезную нагрузку NALU
    readBytes = 0;
    while (readBytes < static_cast<int>(nalSize)) {
        int r = recv(m_socket, reinterpret_cast<char*>(outBuffer.data() + 4 + readBytes), static_cast<int>(nalSize) - readBytes, 0);
        if (r <= 0) {
            m_isConnected = false;
            return -1;
        }
        readBytes += r;
    }

    return static_cast<int>(totalPacketSize);
}