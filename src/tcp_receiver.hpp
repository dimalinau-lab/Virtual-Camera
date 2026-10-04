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
    SOCKET getSocket() const { return m_socket.load(); }

    // Проверка наличия скопившихся данных в системном буфере
    int getPendingBytes() const {
        SOCKET s = m_socket.load();
        if (s == INVALID_SOCKET || !m_isConnected.load()) return 0;
        u_long bytes = 0;
        if (ioctlsocket(s, FIONREAD, &bytes) == 0) {
            return static_cast<int>(bytes);
        }
        return 0;
    }

private:
    std::atomic<SOCKET> m_socket{ INVALID_SOCKET };
    std::atomic<bool> m_isConnected{ false };
};