#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <winhttp.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <thread>
#include <regex>
#include <fstream>
#include <filesystem>
#include <chrono>

#include "version.hpp"
#include "config_manager.hpp"

#pragma comment(lib, "winhttp.lib")

struct UpdateInfo {
    std::string current_version = VIRTUALCAM_VERSION;
    std::string latest_version;
    std::string release_name;
    std::string release_notes;
    std::string download_url;
    int64_t file_size = 0;
    bool update_available = false;
    
    // Состояние: "idle", "checking", "available", "up_to_date", "downloading", "ready", "error"
    std::string status = "idle";
    std::string error_message;
    int progress = 0; // 0..100
    int64_t downloaded_bytes = 0;
    std::string downloaded_file_path;
};

class UpdateManager {
public:
    static UpdateManager& instance() {
        static UpdateManager s_instance;
        return s_instance;
    }

    UpdateInfo getInfo() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_info;
    }

    std::string getStatusJson() {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::ostringstream ss;
        ss << "{"
           << "\"status\":\"" << m_info.status << "\","
           << "\"current_version\":\"" << m_info.current_version << "\","
           << "\"latest_version\":\"" << escapeJson(m_info.latest_version) << "\","
           << "\"release_name\":\"" << escapeJson(m_info.release_name) << "\","
           << "\"release_notes\":\"" << escapeJson(m_info.release_notes) << "\","
           << "\"download_url\":\"" << escapeJson(m_info.download_url) << "\","
           << "\"file_size\":" << m_info.file_size << ","
           << "\"update_available\":" << (m_info.update_available ? "true" : "false") << ","
           << "\"progress\":" << m_info.progress << ","
           << "\"downloaded_bytes\":" << m_info.downloaded_bytes << ","
           << "\"error_message\":\"" << escapeJson(m_info.error_message) << "\""
           << "}";
        return ss.str();
    }

    // Асинхронная проверка обновлений
    void checkForUpdatesAsync(bool force = false) {
        if (m_isChecking.exchange(true)) return;

        std::thread([this, force]() {
            performCheck(force);
            m_isChecking.store(false);
        }).detach();
    }

    // Асинхронное скачивание инсталлятора
    bool startDownloadAsync() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_info.download_url.empty() || m_isDownloading.load()) {
            return false;
        }

        m_isDownloading.store(true);
        m_info.status = "downloading";
        m_info.progress = 0;
        m_info.downloaded_bytes = 0;
        m_info.error_message.clear();

        std::string url = m_info.download_url;
        int64_t expectedSize = m_info.file_size;

        std::thread([this, url, expectedSize]() {
            performDownload(url, expectedSize);
            m_isDownloading.store(false);
        }).detach();

        return true;
    }

    // Запуск скачанного установщика и выход
    bool installAndQuit() {
        std::string path;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            path = m_info.downloaded_file_path;
        }

        if (path.empty() || !std::filesystem::exists(path)) {
            return false;
        }

        std::wstring wPath(path.begin(), path.end());
        HINSTANCE res = ShellExecuteW(NULL, L"open", wPath.c_str(), L"", NULL, SW_SHOWNORMAL);
        if ((INT_PTR)res > 32) {
            // Установщик успешно запущен, безопасно завершаем текущую программу
            std::thread([]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                ExitProcess(0);
            }).detach();
            return true;
        }
        return false;
    }

private:
    UpdateManager() = default;
    ~UpdateManager() = default;

    std::mutex m_mutex;
    UpdateInfo m_info;
    std::atomic<bool> m_isChecking{false};
    std::atomic<bool> m_isDownloading{false};

    static std::string escapeJson(const std::string& in) {
        std::ostringstream ss;
        for (char c : in) {
            if (c == '"') ss << "\\\"";
            else if (c == '\\') ss << "\\\\";
            else if (c == '\b') ss << "\\b";
            else if (c == '\f') ss << "\\f";
            else if (c == '\n') ss << "\\n";
            else if (c == '\r') ss << "\\r";
            else if (c == '\t') ss << "\\t";
            else if (static_cast<unsigned char>(c) < 32) {
                // Игнорируем управляющие символы
            } else {
                ss << c;
            }
        }
        return ss.str();
    }

    void performCheck(bool force) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_info.status = "checking";
            m_info.error_message.clear();
        }

        int64_t nowSec = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        auto cfg = ConfigManager::instance().get();
        // Rate-limit: проверяем не чаще 1 раза в 6 часов, если не force
        if (!force && (nowSec - cfg.last_update_check_ts < 21600) && !cfg.last_update_check_ts == 0) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_info.status = m_info.update_available ? "available" : "up_to_date";
            return;
        }

        std::string responseBody;
        if (!fetchHttpsJson(L"api.github.com", L"/repos/dimalinau-lab/Virtual-Camera/releases/latest", responseBody)) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_info.status = "error";
            m_info.error_message = "Не удалось подключиться к серверу обновлений GitHub";
            return;
        }

        ConfigManager::instance().setLastUpdateCheck(nowSec);

        // Парсинг JSON ответа от GitHub Releases API
        std::string tagName;
        std::regex reTag("\"tag_name\"\\s*:\\s*\"([^\"]+)\"");
        std::smatch m;
        if (std::regex_search(responseBody, m, reTag)) {
            tagName = m[1].str();
        }

        std::string releaseName;
        std::regex reName("\"name\"\\s*:\\s*\"([^\"]+)\"");
        if (std::regex_search(responseBody, m, reName)) {
            releaseName = m[1].str();
        }

        std::string releaseBody;
        std::regex reBody("\"body\"\\s*:\\s*\"([\\s\\S]*?)\",\\s*\"reactions\"");
        if (std::regex_search(responseBody, m, reBody)) {
            releaseBody = m[1].str();
        }

        // Поиск ассета VirtualCamNative_Setup_*.exe
        std::string downloadUrl;
        int64_t fileSize = 0;

        std::regex reAssetUrl("\"browser_download_url\"\\s*:\\s*\"([^\"]*VirtualCamNative_Setup[^\"]*\\.exe)\"");
        if (std::regex_search(responseBody, m, reAssetUrl)) {
            downloadUrl = m[1].str();
        }

        std::regex reAssetSize("\"size\"\\s*:\\s*([0-9]+)");
        if (std::regex_search(responseBody, m, reAssetSize)) {
            try { fileSize = std::stoll(m[1].str()); } catch (...) {}
        }

        bool newer = false;
        if (!tagName.empty()) {
            newer = VersionHelper::isNewer(VIRTUALCAM_VERSION, tagName);
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_info.latest_version = tagName;
            m_info.release_name = releaseName;
            m_info.release_notes = releaseBody;
            m_info.download_url = downloadUrl;
            m_info.file_size = fileSize;
            m_info.update_available = newer;
            m_info.status = newer ? "available" : "up_to_date";
        }
    }

    bool fetchHttpsJson(const std::wstring& host, const std::wstring& path, std::string& outBody) {
        HINTERNET hSession = WinHttpOpen(L"VirtualCamNative-Updater/2.2.0 (Windows NT)",
                                         WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                         WINHTTP_NO_PROXY_NAME,
                                         WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) return false;

        HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!hConnect) {
            WinHttpCloseHandle(hSession);
            return false;
        }

        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(),
                                               NULL, WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               WINHTTP_FLAG_SECURE);
        if (!hRequest) {
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            return false;
        }

        // Автоматическое следование редиректам
        DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

        LPCWSTR headers = L"User-Agent: VirtualCamNative-Updater/2.2.0\r\nAccept: application/vnd.github+json\r\n";
        BOOL bResults = WinHttpSendRequest(hRequest, headers, (DWORD)-1L,
                                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0);

        if (bResults) {
            bResults = WinHttpReceiveResponse(hRequest, NULL);
        }

        if (bResults) {
            DWORD dwSize = 0;
            do {
                dwSize = 0;
                if (!WinHttpQueryDataAvailable(hRequest, &dwSize)) break;
                if (dwSize == 0) break;

                std::vector<char> buf(dwSize);
                DWORD dwDownloaded = 0;
                if (WinHttpReadData(hRequest, buf.data(), dwSize, &dwDownloaded)) {
                    outBody.append(buf.data(), dwDownloaded);
                }
            } while (dwSize > 0);
        }

        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return bResults && !outBody.empty();
    }

    void performDownload(const std::string& url, int64_t expectedSize) {
        // Парсинг URL
        std::wstring wUrl(url.begin(), url.end());
        URL_COMPONENTS urlComp{};
        urlComp.dwStructSize = sizeof(urlComp);
        wchar_t hostName[256] = {0};
        wchar_t urlPath[2048] = {0};
        urlComp.lpszHostName = hostName;
        urlComp.dwHostNameLength = 256;
        urlComp.lpszUrlPath = urlPath;
        urlComp.dwUrlPathLength = 2048;

        if (!WinHttpCrackUrl(wUrl.c_str(), (DWORD)wUrl.length(), 0, &urlComp)) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_info.status = "error";
            m_info.error_message = "Неверная ссылка для загрузки";
            return;
        }

        wchar_t tempDir[MAX_PATH];
        GetTempPathW(MAX_PATH, tempDir);
        std::wstring targetFilePath = std::wstring(tempDir) + L"VirtualCamNative_Setup_Update.exe";

        std::ofstream out(targetFilePath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_info.status = "error";
            m_info.error_message = "Не удалось создать временный файл инсталлятора";
            return;
        }

        HINTERNET hSession = WinHttpOpen(L"VirtualCamNative-Updater/2.2.0 (Windows NT)",
                                         WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                         WINHTTP_NO_PROXY_NAME,
                                         WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_info.status = "error";
            m_info.error_message = "Ошибка инициализации сетевого стека WinHTTP";
            return;
        }

        HINTERNET hConnect = WinHttpConnect(hSession, hostName, urlComp.nPort, 0);
        if (!hConnect) {
            WinHttpCloseHandle(hSession);
            std::lock_guard<std::mutex> lock(m_mutex);
            m_info.status = "error";
            m_info.error_message = "Не удалось подключиться к серверу загрузки";
            return;
        }

        DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", urlPath, NULL,
                                               WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);

        DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

        BOOL bResults = WinHttpSendRequest(hRequest, L"User-Agent: VirtualCamNative-Updater/2.2.0\r\n", (DWORD)-1L,
                                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
        if (bResults) {
            bResults = WinHttpReceiveResponse(hRequest, NULL);
        }

        if (!bResults) {
            WinHttpCloseHandle(hRequest);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            std::lock_guard<std::mutex> lock(m_mutex);
            m_info.status = "error";
            m_info.error_message = "Ошибка при получении ответа от сервера загрузки";
            return;
        }

        // Если размер не был передан в JSON, читаем заголовок Content-Length
        if (expectedSize <= 0) {
            wchar_t lenHeader[64] = {0};
            DWORD headerSize = sizeof(lenHeader);
            if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                                    lenHeader, &headerSize, WINHTTP_NO_HEADER_INDEX)) {
                try { expectedSize = std::stoll(lenHeader); } catch (...) {}
            }
        }

        int64_t totalDownloaded = 0;
        DWORD dwSize = 0;
        std::vector<char> buffer(65536); // 64 KB буфер

        while (true) {
            dwSize = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &dwSize) || dwSize == 0) {
                break;
            }

            DWORD dwRead = 0;
            DWORD toRead = (std::min)(dwSize, (DWORD)buffer.size());
            if (WinHttpReadData(hRequest, buffer.data(), toRead, &dwRead) && dwRead > 0) {
                out.write(buffer.data(), dwRead);
                totalDownloaded += dwRead;

                int progressPct = 0;
                if (expectedSize > 0) {
                    progressPct = static_cast<int>((totalDownloaded * 100) / expectedSize);
                    if (progressPct > 100) progressPct = 100;
                }

                // Обновление прогресса
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_info.downloaded_bytes = totalDownloaded;
                    m_info.progress = progressPct;
                }
            } else {
                break;
            }
        }

        out.close();
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);

        if (totalDownloaded == 0 || (expectedSize > 0 && totalDownloaded < expectedSize * 0.95)) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_info.status = "error";
            m_info.error_message = "Файл скачан не полностью";
            return;
        }

        char narrowBuf[MAX_PATH * 2] = {0};
        WideCharToMultiByte(CP_UTF8, 0, targetFilePath.c_str(), -1, narrowBuf, sizeof(narrowBuf), nullptr, nullptr);
        std::string narrowTarget(narrowBuf);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_info.status = "ready";
            m_info.progress = 100;
            m_info.downloaded_file_path = narrowTarget;
        }
    }
};
