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
    bool is_zip_package = false;
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
           << "\"is_zip_package\":" << (m_info.is_zip_package ? "true" : "false") << ","
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

    // Асинхронное скачивание инсталлятора или ZIP-пакета
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
        bool isZip = m_info.is_zip_package;

        std::thread([this, url, expectedSize, isZip]() {
            performDownload(url, expectedSize, isZip);
            m_isDownloading.store(false);
        }).detach();

        return true;
    }

    // Запуск бесшовного обновления (Hot-Swap) или фоллбэк на инсталлятор
    bool installAndQuit() {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::filesystem::path appDir = std::filesystem::path(exePath).parent_path();
        std::filesystem::path stagedDir = appDir / "_update";

        // 1. Бесшовный режим Hot-Swap: если в папке _update/ есть файлы
        if (std::filesystem::exists(stagedDir) && !std::filesystem::is_empty(stagedDir)) {
            std::filesystem::path batPath = appDir / "apply_update.bat";
            DWORD currentPid = GetCurrentProcessId();

            std::ofstream bat(batPath, std::ios::trunc);
            if (bat.is_open()) {
                bat << "@echo off\r\n"
                    << "chcp 65001 >nul\r\n"
                    << "cd /d \"%~dp0\"\r\n"
                    << "echo [%date% %time%] Hot-swap started > apply_update.log 2>&1\r\n"
                    << "\r\n"
                    << ":: Пауза 1.5 сек для полного закрытия родительского процесса и освобождения дескрипторов\r\n"
                    << "ping 127.0.0.1 -n 3 >nul 2>&1\r\n"
                    << "\r\n"
                    << ":: Попытка переноса старого исполняемого файла с повторами (до 15 попыток)\r\n"
                    << "set /a tries=0\r\n"
                    << ":retry_exe\r\n"
                    << "set /a tries+=1\r\n"
                    << "if exist \"_update\\VirtualCamNative.exe\" (\r\n"
                    << "    del /f /q \"VirtualCamNative.exe.old\" >nul 2>&1\r\n"
                    << "    move /y \"VirtualCamNative.exe\" \"VirtualCamNative.exe.old\" >> apply_update.log 2>&1\r\n"
                    << "    if errorlevel 1 (\r\n"
                    << "        if %tries% lss 15 (\r\n"
                    << "            ping 127.0.0.1 -n 2 >nul 2>&1\r\n"
                    << "            goto retry_exe\r\n"
                    << "        )\r\n"
                    << "    )\r\n"
                    << "    copy /y \"_update\\VirtualCamNative.exe\" \"VirtualCamNative.exe\" >> apply_update.log 2>&1\r\n"
                    << ")\r\n"
                    << "\r\n"
                    << "if exist \"_update\\NativeMFVirtualCam.dll\" (\r\n"
                    << "    del /f /q \"NativeMFVirtualCam.dll.old\" >nul 2>&1\r\n"
                    << "    move /y \"NativeMFVirtualCam.dll\" \"NativeMFVirtualCam.dll.old\" >> apply_update.log 2>&1\r\n"
                    << "    copy /y \"_update\\NativeMFVirtualCam.dll\" \"NativeMFVirtualCam.dll\" >> apply_update.log 2>&1\r\n"
                    << "    regsvr32.exe /s \"NativeMFVirtualCam.dll\" >> apply_update.log 2>&1\r\n"
                    << ")\r\n"
                    << "\r\n"
                    << "if exist \"_update\\web\" (\r\n"
                    << "    xcopy /y /e /q /i \"_update\\web\\*\" \"web\\\" >> apply_update.log 2>&1\r\n"
                    << ")\r\n"
                    << "xcopy /y /e /q /i \"_update\\*\" \".\\\" >> apply_update.log 2>&1\r\n"
                    << "\r\n"
                    << "rd /s /q \"_update\" >> apply_update.log 2>&1\r\n"
                    << "del /f /q \"*.old\" >> apply_update.log 2>&1\r\n"
                    << "\r\n"
                    << "echo [%date% %time%] Launching updated VirtualCamNative.exe >> apply_update.log 2>&1\r\n"
                    << "start \"\" \"VirtualCamNative.exe\"\r\n"
                    << "(goto) 2>nul & del \"%~f0\"\r\n";
                bat.close();
            }

            std::wstring batCmd = L"cmd.exe /c \"" + batPath.wstring() + L"\"";
            std::wstring appDirStr = appDir.wstring();
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pi{};
            DWORD creationFlags = CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP;
            if (CreateProcessW(nullptr, &batCmd[0], nullptr, nullptr, FALSE, creationFlags, nullptr, appDirStr.c_str(), &si, &pi)) {
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);

                std::thread([]() {
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                    ExitProcess(0);
                }).detach();
                return true;
            }
        }

        // 2. Фоллбэк: запуск скачанного установщика
        std::string path;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            path = m_info.downloaded_file_path;
        }

        if (path.empty() || !std::filesystem::exists(path)) {
            return false;
        }

        std::wstring wPath(path.begin(), path.end());
        HINSTANCE res = ShellExecuteW(NULL, L"open", wPath.c_str(), L"/VERYSILENT /SUPPRESSMSGBOXES /NORESTART", NULL, SW_SHOWNORMAL);
        if ((INT_PTR)res > 32) {
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

        std::string downloadUrl;
        int64_t fileSize = 0;
        bool isZip = false;

        // 1. Приоритет: легковесный ZIP-пакет (2-4 МБ) для бесшовного Hot-Swap обновления
        std::regex reZipUrl("\"browser_download_url\"\\s*:\\s*\"([^\"]*VirtualCamNative[^\"]*\\.zip)\"");
        if (std::regex_search(responseBody, m, reZipUrl)) {
            downloadUrl = m[1].str();
            isZip = true;
            size_t pos = responseBody.find(downloadUrl);
            if (pos != std::string::npos) {
                size_t startPos = (pos > 600) ? (pos - 600) : 0;
                std::string block = responseBody.substr(startPos, 1200);
                std::regex reAssetSize("\"size\"\\s*:\\s*([0-9]+)");
                std::smatch mSize;
                if (std::regex_search(block, mSize, reAssetSize)) {
                    try { fileSize = std::stoll(mSize[1].str()); } catch (...) {}
                }
            }
        }

        // 2. Фоллбэк: если ZIP не найден, ищем инсталлятор .exe
        if (downloadUrl.empty()) {
            std::regex reAssetUrl("\"browser_download_url\"\\s*:\\s*\"([^\"]*VirtualCamNative_Setup[^\"]*\\.exe)\"");
            if (std::regex_search(responseBody, m, reAssetUrl)) {
                downloadUrl = m[1].str();
                isZip = false;
            }
            std::regex reAssetSize("\"size\"\\s*:\\s*([0-9]+)");
            if (std::regex_search(responseBody, m, reAssetSize)) {
                try { fileSize = std::stoll(m[1].str()); } catch (...) {}
            }
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
            m_info.is_zip_package = isZip;
            m_info.update_available = newer;
            m_info.status = newer ? "available" : "up_to_date";
        }
    }

    bool fetchHttpsJson(const std::wstring& host, const std::wstring& path, std::string& outBody) {
        HINTERNET hSession = WinHttpOpen(L"VirtualCamNative-Updater/2.3.0 (Windows NT)",
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

        LPCWSTR headers = L"User-Agent: VirtualCamNative-Updater/2.3.0\r\nAccept: application/vnd.github+json\r\n";
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

    void performDownload(const std::string& url, int64_t expectedSize, bool isZip) {
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
        std::wstring targetFilePath = std::wstring(tempDir) + (isZip ? L"VirtualCamNative_Update.zip" : L"VirtualCamNative_Setup_Update.exe");

        std::ofstream out(targetFilePath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_info.status = "error";
            m_info.error_message = isZip ? "Не удалось создать временный ZIP файл" : "Не удалось создать временный файл инсталлятора";
            return;
        }

        HINTERNET hSession = WinHttpOpen(L"VirtualCamNative-Updater/2.3.0 (Windows NT)",
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

        BOOL bResults = WinHttpSendRequest(hRequest, L"User-Agent: VirtualCamNative-Updater/2.3.0\r\nAccept: application/octet-stream\r\n", (DWORD)-1L,
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

        // Если это ZIP архив, распаковываем его во временную папку _update в директории приложения
        if (isZip) {
            wchar_t exePath[MAX_PATH];
            GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            std::filesystem::path appDir = std::filesystem::path(exePath).parent_path();
            std::filesystem::path stagedDir = appDir / "_update";

            std::error_code ec;
            std::filesystem::remove_all(stagedDir, ec);
            std::filesystem::create_directories(stagedDir, ec);

            // Используем стандартную системную утилиту Windows tar.exe (есть во всех Win10/11)
            std::wstring tarCmd = L"tar.exe -xf \"" + targetFilePath + L"\" -C \"" + stagedDir.wstring() + L"\"";
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(nullptr, &tarCmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                WaitForSingleObject(pi.hProcess, 15000); // Ожидание распаковки до 15 сек
                DWORD exitCode = 1;
                GetExitCodeProcess(pi.hProcess, &exitCode);
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);

                // Удаляем временный zip
                DeleteFileW(targetFilePath.c_str());

                // Если файлы оказались в подпапке архива, вытягиваем их в stagedDir
                if (!std::filesystem::exists(stagedDir / "VirtualCamNative.exe")) {
                    for (const auto& entry : std::filesystem::directory_iterator(stagedDir)) {
                        if (entry.is_directory() && std::filesystem::exists(entry.path() / "VirtualCamNative.exe")) {
                            for (const auto& sub : std::filesystem::directory_iterator(entry.path())) {
                                std::error_code moveEc;
                                std::filesystem::copy(sub.path(), stagedDir / sub.path().filename(),
                                                      std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, moveEc);
                            }
                            std::filesystem::remove_all(entry.path(), ec);
                            break;
                        }
                    }
                }

                if (exitCode != 0 || std::filesystem::is_empty(stagedDir)) {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_info.status = "error";
                    m_info.error_message = "Ошибка распаковки архива обновления tar.exe";
                    return;
                }
            } else {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_info.status = "error";
                m_info.error_message = "Не удалось запустить утилиту распаковки tar.exe";
                return;
            }
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
