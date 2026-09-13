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

private:
    SOCKET m_socket{ INVALID_SOCKET };
    std::atomic<bool> m_isConnected{ false };
};