#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <winsock2.h>

class TcpReceiver {
public:
    TcpReceiver();
    ~TcpReceiver();

    bool connectToPhone(const std::string& ip, int port);
    void stop();
    int receiveNalu(std::vector<uint8_t>& outBuffer);
    bool isConnected() const { return m_isConnected.load(); }
    SOCKET getSocket() const { return m_socket; }

    // Проверка наличия скопившихся данных в системном буфере
    int getPendingBytes() const {
        if (m_socket == INVALID_SOCKET || !m_isConnected.load()) return 0;
        u_long bytes = 0;
        if (ioctlsocket(m_socket, FIONREAD, &bytes) == 0) {
            return static_cast<int>(bytes);
        }
        return 0;
    }

private:
    SOCKET m_socket{ INVALID_SOCKET };
    std::atomic<bool> m_isConnected{ false };
};