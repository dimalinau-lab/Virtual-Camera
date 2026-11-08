#pragma once
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

struct DiscoveredDevice {
    std::string id;
    std::string name;
    std::string ip;
    int port = 8080;
    std::chrono::steady_clock::time_point lastSeen;
};

class DeviceDiscoveryService {
public:
    static DeviceDiscoveryService& instance() {
        static DeviceDiscoveryService s_instance;
        return s_instance;
    }

    void start(int listenPort = 8888) {
        if (m_running) return;
        m_running = true;
        m_worker = std::thread(&DeviceDiscoveryService::listenLoop, this, listenPort);
        // Немедленный активный UDP probe-запрос в сеть
        sendDiscoveryProbe();
    }

    void stop() {
        m_running = false;
        if (m_sock != INVALID_SOCKET) {
            closesocket(m_sock);
            m_sock = INVALID_SOCKET;
        }
        if (m_worker.joinable()) {
            m_worker.join();
        }
    }

    void sendDiscoveryProbe() {
        uint64_t now = GetTickCount64();
        if (now - m_lastProbeTick.load() < 500) {
            return; // Защита от перегрузки сети
        }
        m_lastProbeTick.store(now);

        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET) return;

        BOOL bOpt = TRUE;
        setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&bOpt), sizeof(bOpt));

        const char probeMsg[] = "{\"service\":\"VirtualCamNative\",\"action\":\"probe\"}";
        int len = static_cast<int>(sizeof(probeMsg) - 1);

        // 1. Отправка на общий broadcast 255.255.255.255:8889
        sockaddr_in bcastAddr{};
        bcastAddr.sin_family = AF_INET;
        bcastAddr.sin_port = htons(8889);
        bcastAddr.sin_addr.s_addr = INADDR_BROADCAST;
        sendto(s, probeMsg, len, 0, reinterpret_cast<sockaddr*>(&bcastAddr), sizeof(bcastAddr));

        // 2. Направленная отправка в широковещательные адреса локальных подсетей (Subnet Directed Broadcast)
        char hostName[256];
        if (gethostname(hostName, sizeof(hostName)) == 0) {
            addrinfo hints{};
            hints.ai_family = AF_INET;
            addrinfo* res = nullptr;
            if (getaddrinfo(hostName, nullptr, &hints, &res) == 0) {
                for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
                    sockaddr_in* sin = reinterpret_cast<sockaddr_in*>(p->ai_addr);
                    sockaddr_in subBcast = *sin;
                    subBcast.sin_port = htons(8889);
                    subBcast.sin_addr.S_un.S_un_b.s_b4 = 255;
                    sendto(s, probeMsg, len, 0, reinterpret_cast<sockaddr*>(&subBcast), sizeof(subBcast));
                }
                freeaddrinfo(res);
            }
        }

        closesocket(s);
    }

    std::vector<DiscoveredDevice> getActiveDevices(int timeoutSec = 5) {
        std::lock_guard<std::mutex> lock(m_mtx);
        auto now = std::chrono::steady_clock::now();
        std::vector<DiscoveredDevice> active;

        for (auto it = m_devices.begin(); it != m_devices.end(); ) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.lastSeen).count();
            if (elapsed > timeoutSec) {
                it = m_devices.erase(it);
            }
            else {
                active.push_back(it->second);
                ++it;
            }
        }

        if (active.empty()) {
            sendDiscoveryProbe();
        }

        return active;
    }

private:
    DeviceDiscoveryService() = default;
    ~DeviceDiscoveryService() { stop(); }

    void listenLoop(int port) {
        m_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (m_sock == INVALID_SOCKET) return;

        BOOL reuse = TRUE;
        setsockopt(m_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        DWORD tv = 250; // Быстрый цикл ожидания (250 мс)
        setsockopt(m_sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));

        sockaddr_in bindAddr{};
        bindAddr.sin_family = AF_INET;
        bindAddr.sin_port = htons(static_cast<u_short>(port));
        bindAddr.sin_addr.s_addr = INADDR_ANY;

        if (bind(m_sock, reinterpret_cast<sockaddr*>(&bindAddr), sizeof(bindAddr)) == SOCKET_ERROR) {
            closesocket(m_sock);
            m_sock = INVALID_SOCKET;
            return;
        }

        char buffer[1024];
        while (m_running) {
            sockaddr_in senderAddr{};
            int senderLen = sizeof(senderAddr);
            int received = recvfrom(m_sock, buffer, sizeof(buffer) - 1, 0, reinterpret_cast<sockaddr*>(&senderAddr), &senderLen);

            if (received > 0) {
                buffer[received] = '\0';
                std::string msg(buffer);

                if (msg.find("\"service\":\"VirtualCamNative\"") != std::string::npos ||
                    msg.find("\"service\": \"VirtualCamNative\"") != std::string::npos) {

                    char ipStr[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &senderAddr.sin_addr, ipStr, sizeof(ipStr));

                    auto extractJson = [&](const std::string& key) -> std::string {
                        size_t pos = msg.find("\"" + key + "\"");
                        if (pos == std::string::npos) return "";
                        size_t colon = msg.find(":", pos);
                        if (colon == std::string::npos) return "";
                        size_t startQuote = msg.find("\"", colon);
                        if (startQuote == std::string::npos) return "";
                        size_t endQuote = msg.find("\"", startQuote + 1);
                        if (endQuote == std::string::npos) return "";
                        return msg.substr(startQuote + 1, endQuote - startQuote - 1);
                    };

                    std::string devName = extractJson("device_name");
                    std::string devId = extractJson("device_id");
                    if (devName.empty()) devName = "Android Device";
                    if (devId.empty()) devId = ipStr;

                    DiscoveredDevice dev;
                    dev.id = devId;
                    dev.name = devName;
                    dev.ip = ipStr;
                    dev.port = 8080;
                    dev.lastSeen = std::chrono::steady_clock::now();

                    std::lock_guard<std::mutex> lock(m_mtx);
                    m_devices[devId] = dev;
                }
            }
        }
    }

    std::atomic<bool> m_running{ false };
    std::atomic<uint64_t> m_lastProbeTick{ 0 };
    SOCKET m_sock{ INVALID_SOCKET };
    std::thread m_worker;
    std::mutex m_mtx;
    std::map<std::string, DiscoveredDevice> m_devices;
};
