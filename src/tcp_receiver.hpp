#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <cstdint>
#include <winsock2.h>
#include "mf_shared_mem.hpp"

class TcpReceiver {
public:
    TcpReceiver();
    ~TcpReceiver();

    bool connectToPhone(const std::string& ip, int port);
    void stop();
    // Читает следующий NALU кадр (возвращает размер буфера или <=0 при ошибке/таймауте).
    // Автоматически обрабатывает мультиплексированные пакеты аудио/статуса.
    int receiveNalu(std::vector<uint8_t>& outBuffer, int64_t* outPtsUs = nullptr, bool* outIsKeyframe = nullptr);
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
    bool readExact(SOCKET sock, uint8_t* dst, size_t count);

    std::atomic<SOCKET> m_socket{ INVALID_SOCKET };
    std::atomic<bool> m_isConnected{ false };
    std::vector<uint8_t> m_payloadBuf;
};