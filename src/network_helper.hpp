#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <string>
#include <vector>
#include <stdio.h>
#include <mutex>
#include <chrono>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace NetworkHelper {

inline std::string getPrimaryMacAddress() {
    static std::mutex s_macMtx;
    static std::string s_cachedMac;
    static auto s_lastMacTime = std::chrono::steady_clock::time_point{};

    std::lock_guard<std::mutex> lock(s_macMtx);
    auto now = std::chrono::steady_clock::now();
    if (!s_cachedMac.empty() && (now - s_lastMacTime) < std::chrono::seconds(10)) {
        return s_cachedMac;
    }

    ULONG outBufLen = 15000;
    std::vector<BYTE> buf(outBufLen);
    PIP_ADAPTER_ADDRESSES pAddresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());

    ULONG flags = GAA_FLAG_INCLUDE_PREFIX | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    DWORD dwRetVal = GetAdaptersAddresses(AF_INET, flags, NULL, pAddresses, &outBufLen);
    if (dwRetVal == ERROR_BUFFER_OVERFLOW) {
        buf.resize(outBufLen);
        pAddresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        dwRetVal = GetAdaptersAddresses(AF_INET, flags, NULL, pAddresses, &outBufLen);
    }

    if (dwRetVal == NO_ERROR) {
        for (PIP_ADAPTER_ADDRESSES pCurr = pAddresses; pCurr != NULL; pCurr = pCurr->Next) {
            if (pCurr->OperStatus == IfOperStatusUp && pCurr->PhysicalAddressLength == 6 &&
                pCurr->IfType != IF_TYPE_SOFTWARE_LOOPBACK) {
                if (pCurr->FirstUnicastAddress != nullptr) {
                    char macStr[32] = {0};
                    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                        pCurr->PhysicalAddress[0], pCurr->PhysicalAddress[1],
                        pCurr->PhysicalAddress[2], pCurr->PhysicalAddress[3],
                        pCurr->PhysicalAddress[4], pCurr->PhysicalAddress[5]);
                    s_cachedMac = macStr;
                    s_lastMacTime = now;
                    return s_cachedMac;
                }
            }
        }
    }
    return s_cachedMac;
}

inline std::string getPrimaryLocalIp() {
    static std::mutex s_ipMtx;
    static std::string s_cachedIp;
    static auto s_lastIpTime = std::chrono::steady_clock::time_point{};

    std::lock_guard<std::mutex> lock(s_ipMtx);
    auto now = std::chrono::steady_clock::now();
    if (!s_cachedIp.empty() && (now - s_lastIpTime) < std::chrono::seconds(5)) {
        return s_cachedIp;
    }

    char hostName[256] = {0};
    if (gethostname(hostName, sizeof(hostName)) == 0) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(hostName, nullptr, &hints, &res) == 0) {
            for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
                sockaddr_in* sin = reinterpret_cast<sockaddr_in*>(p->ai_addr);
                char ipStr[INET_ADDRSTRLEN] = {0};
                inet_ntop(AF_INET, &(sin->sin_addr), ipStr, sizeof(ipStr));
                std::string s(ipStr);
                if (s != "127.0.0.1" && s.rfind("169.254.", 0) != 0) {
                    freeaddrinfo(res);
                    s_cachedIp = s;
                    s_lastIpTime = now;
                    return s_cachedIp;
                }
            }
            freeaddrinfo(res);
        }
    }
    if (!s_cachedIp.empty()) return s_cachedIp;
    return "127.0.0.1";
}

} // namespace NetworkHelper
