#pragma once
#include <string>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iostream>

#pragma comment(lib, "ws2_32.lib")

// Сканирует локальную сеть в течение timeoutMs миллисекунд в поисках маяка телефона
inline std::string discoverPhoneIp(int timeoutMs = 1800) {
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return "";

    // Разрешаем повторное использование адреса
    BOOL reuse = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    // Выставляем таймаут ожидания пакета
    DWORD tv = timeoutMs;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));

    sockaddr_in bindAddr{};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons(8888);
    bindAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) == SOCKET_ERROR) {
        closesocket(sock);
        return "";
    }

    char buffer[512];
    sockaddr_in senderAddr{};
    int senderLen = sizeof(senderAddr);

    int received = recvfrom(sock, buffer, sizeof(buffer) - 1, 0, reinterpret_cast<sockaddr*>(&senderAddr), &senderLen);
    closesocket(sock);

    if (received > 0) {
        buffer[received] = '\0';
        std::string msg(buffer);

        // Проверяем валидность полученного сервисного маяка
        if (msg.find("VirtualCamNative") != std::string::npos) {
            char ipStr[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &senderAddr.sin_addr, ipStr, sizeof(ipStr));
            return std::string(ipStr);
        }
    }

    return "";
}