#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <string>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <regex>
#include <unordered_map>
#include <windows.h>
#include "studio_optics.hpp"

#include "app_state.hpp"

struct PairedDeviceInfo {
    std::string name;
    std::string ip;
    std::string token;
};

struct AppConfig {
    bool mirror_enabled = false;
    bool flip180 = false;
    std::string lang = "ru";
    std::string ui_skin = "index2.html";
    std::string last_connection_mode = "wifi";
    std::string phone_resolution = "1080p";
    int phone_fps = 60;
    std::string phone_codec = "h265";
    int phone_bitrate = 10;
    bool is_vertical = true;
    int aspect_ratio_mode = 0; // 0: 9:16 (Phone Portrait), 1: 4:3 (Classic), 2: 16:9 (Wide)
    std::string audio_device_id = "phone";
    std::string audio_output_device_id = "vcam_only";
    int audio_volume = 100;
    int audio_delay = 0;
    bool noise_gate = true;
    float noise_gate_threshold = 0.015f;
    bool ai_noise = true;
    bool agc = true;
    bool declicker = true;
    int eq_low = 0;
    int eq_mid = 0;
    int eq_high = 0;
    float optics_zoom = 1.0f;
    int optics_brightness = 0;
    int optics_contrast = 100;
    int optics_saturation = 100;
    int optics_temp = 0;
    int optics_lut = 0;
    bool close_to_tray = true;
    bool start_with_windows = false;
    bool show_console = false;
    bool show_hud_stats = false;
    bool auto_check_updates = true;
    int64_t last_update_check_ts = 0;

    // Troll FX (Сессионные и постоянные эффекты)
    bool troll_fps_5 = false;
    int troll_pixelate = 1;
    bool troll_glitch = false;
    bool troll_bitcrush = false;
    bool troll_overexposure = false;
    bool troll_gsm_voice = false;
    bool troll_walkie_talkie = false;
    bool troll_robot_voice = false;
    bool troll_cctv = false;
    bool troll_fake_lag = false;
    std::vector<PairedDeviceInfo> paired_devices;
};

class ConfigManager {
private:
    AppConfig m_config;
    std::unordered_map<std::string, std::string> m_runtimeTokens;
    bool m_diskSaveEnabled = true;
    std::mutex m_mutex;

    ConfigManager() = default;

public:
    void setDiskSaveEnabled(bool enabled) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_diskSaveEnabled = enabled;
    }
    static std::string escapeJsonString(const std::string& input) {
        std::string out;
        out.reserve(input.size() + 8);
        for (unsigned char c : input) {
            switch (c) {
                case '\"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b";  break;
                case '\f': out += "\\f";  break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:
                    if (c < 0x20) {
                        char buf[8];
                        snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    } else {
                        out.push_back(static_cast<char>(c));
                    }
                    break;
            }
        }
        return out;
    }

    // Вспомогательные методы робастного парсинга JSON
    static bool extractBool(const std::string& json, const std::string& key, bool defaultVal) {
        std::regex re("\"" + key + "\"\\s*:\\s*(true|false|1|0)", std::regex::icase);
        std::smatch match;
        if (std::regex_search(json, match, re)) {
            std::string val = match[1].str();
            std::transform(val.begin(), val.end(), val.begin(), ::tolower);
            return (val == "true" || val == "1");
        }
        return defaultVal;
    }

    static int extractInt(const std::string& json, const std::string& key, int defaultVal) {
        std::regex re("\"" + key + "\"\\s*:\\s*(-?[0-9]+)");
        std::smatch match;
        if (std::regex_search(json, match, re)) {
            try { return std::stoi(match[1].str()); } catch (...) {}
        }
        return defaultVal;
    }

    static float extractFloat(const std::string& json, const std::string& key, float defaultVal) {
        std::regex re("\"" + key + "\"\\s*:\\s*(-?[0-9]+(?:\\.[0-9]+)?)");
        std::smatch match;
        if (std::regex_search(json, match, re)) {
            try { return std::stof(match[1].str()); } catch (...) {}
        }
        return defaultVal;
    }

    static std::string extractString(const std::string& json, const std::string& key, const std::string& defaultVal) {
        std::regex re("\"" + key + "\"\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\"");
        std::smatch match;
        if (std::regex_search(json, match, re)) {
            const std::string raw = match[1].str();
            std::string unescaped;
            unescaped.reserve(raw.size());
            for (size_t i = 0; i < raw.size(); ++i) {
                if (raw[i] == '\\' && i + 1 < raw.size()) {
                    char next = raw[++i];
                    switch (next) {
                        case '\"': unescaped.push_back('\"'); break;
                        case '\\': unescaped.push_back('\\'); break;
                        case '/':  unescaped.push_back('/');  break;
                        case 'b':  unescaped.push_back('\b'); break;
                        case 'f':  unescaped.push_back('\f'); break;
                        case 'n':  unescaped.push_back('\n'); break;
                        case 'r':  unescaped.push_back('\r'); break;
                        case 't':  unescaped.push_back('\t'); break;
                        default:   unescaped.push_back(next); break;
                    }
                } else {
                    unescaped.push_back(raw[i]);
                }
            }
            return unescaped;
        }
        return defaultVal;
    }

    static bool hasKey(const std::string& json, const std::string& key) {
        std::regex re("\"" + key + "\"\\s*:");
        return std::regex_search(json, re);
    }

    static void setWindowsAutostart(bool enable) {
        HKEY hKey;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
            if (enable) {
                wchar_t exePathBuf[MAX_PATH];
                if (GetModuleFileNameW(nullptr, exePathBuf, MAX_PATH)) {
                    std::wstring val = L"\"" + std::wstring(exePathBuf) + L"\" --autostart";
                    RegSetValueExW(hKey, L"VirtualCam", 0, REG_SZ, reinterpret_cast<const BYTE*>(val.c_str()), static_cast<DWORD>((val.length() + 1) * sizeof(wchar_t)));
                }
            } else {
                RegDeleteValueW(hKey, L"VirtualCam");
            }
            RegCloseKey(hKey);
        }
    }

    static bool isWindowsAutostartEnabled() {
        HKEY hKey;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_QUERY_VALUE, &hKey) == ERROR_SUCCESS) {
            DWORD type = 0;
            DWORD size = 0;
            LONG res = RegQueryValueExW(hKey, L"VirtualCam", nullptr, &type, nullptr, &size);
            RegCloseKey(hKey);
            return (res == ERROR_SUCCESS);
        }
        return false;
    }

    static ConfigManager& instance() {
        static ConfigManager s_inst;
        return s_inst;
    }

    static std::filesystem::path getConfigFilePath() {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        return std::filesystem::path(exePath).parent_path() / "config.json";
    }

    AppConfig get() {
        std::lock_guard<std::mutex> lock(m_mutex);
        syncLiveGlobalsToConfig();
        return m_config;
    }

    void syncLiveGlobalsToConfig() {
        m_config.mirror_enabled = g_mirrorEnabled.load();
        m_config.flip180 = g_flip180.load();
        m_config.is_vertical = !g_isLandscapeMode.load();
        m_config.aspect_ratio_mode = g_aspectRatioMode.load();
        if (!g_audioInputDeviceId.empty()) {
            m_config.audio_device_id = g_audioInputDeviceId;
        }
        if (!g_audioOutputDeviceId.empty()) {
            m_config.audio_output_device_id = g_audioOutputDeviceId;
        }
        m_config.audio_volume = (std::clamp)((int)std::round(g_audioVolume.load() * 100.0f), 0, 200);
        m_config.audio_delay = g_audioDelayMs.load();
        m_config.noise_gate = g_noiseGateEnabled.load();
        m_config.noise_gate_threshold = g_noiseGateThreshold.load();
        m_config.ai_noise = g_aiNoiseEnabled.load();
        m_config.agc = g_agcEnabled.load();
        m_config.declicker = g_declickerEnabled.load();
        m_config.close_to_tray = g_closeToTray.load();
        m_config.show_console = g_showConsole.load();
        m_config.phone_fps = g_currentFps.load();
        m_config.last_connection_mode = g_targetMode;

        m_config.optics_zoom = StudioOptics::instance().getZoom();
        m_config.optics_brightness = StudioOptics::instance().getBrightness();
        m_config.optics_contrast = StudioOptics::instance().getContrast();
        m_config.optics_saturation = StudioOptics::instance().getSaturation();
        m_config.optics_temp = StudioOptics::instance().getColorTemp();
        m_config.optics_lut = StudioOptics::instance().getLutPreset();

        m_config.troll_fps_5 = g_trollFpsLimit.load();
        m_config.troll_pixelate = g_trollPixelate.load();
        m_config.troll_glitch = g_trollGlitch.load();
        m_config.troll_bitcrush = g_trollBitcrush.load();
        m_config.troll_overexposure = g_trollOverexposure.load();
        m_config.troll_gsm_voice = g_trollGsmVoice.load();
        m_config.troll_walkie_talkie = g_trollWalkieTalkie.load();
        m_config.troll_robot_voice = g_trollRobotVoice.load();
        m_config.troll_cctv = g_trollCctv.load();
        m_config.troll_fake_lag = g_trollFakeLag.load();
    }

    void applyConfigToSystem() {
        g_mirrorEnabled.store(m_config.mirror_enabled);
        g_flip180.store(m_config.flip180);
        g_isLandscapeMode.store(!m_config.is_vertical);
        g_aspectRatioMode.store(m_config.aspect_ratio_mode);
        if (!m_config.audio_device_id.empty()) {
            g_audioInputDeviceId = m_config.audio_device_id;
            g_audioInputDeviceChanged.store(true);
        }
        if (!m_config.audio_output_device_id.empty()) {
            g_audioOutputDeviceId = m_config.audio_output_device_id;
            g_audioOutputDeviceChanged.store(true);
        }
        g_audioVolume.store(m_config.audio_volume / 100.0f);
        g_audioDelayMs.store(m_config.audio_delay);
        g_noiseGateEnabled.store(m_config.noise_gate);
        g_noiseGateThreshold.store(m_config.noise_gate_threshold);
        g_aiNoiseEnabled.store(m_config.ai_noise);
        g_agcEnabled.store(m_config.agc);
        g_declickerEnabled.store(m_config.declicker);
        g_eqLowDb.store(static_cast<float>(m_config.eq_low));
        g_eqMidDb.store(static_cast<float>(m_config.eq_mid));
        g_eqHighDb.store(static_cast<float>(m_config.eq_high));
        g_closeToTray.store(m_config.close_to_tray);
        g_showConsole.store(m_config.show_console);
        g_currentFps.store(m_config.phone_fps);
        g_targetMode = m_config.last_connection_mode;

        // Troll FX
        g_trollFpsLimit.store(m_config.troll_fps_5);
        g_trollPixelate.store(m_config.troll_pixelate);
        g_trollGlitch.store(m_config.troll_glitch);
        g_trollBitcrush.store(m_config.troll_bitcrush);
        g_trollOverexposure.store(m_config.troll_overexposure);
        g_trollGsmVoice.store(m_config.troll_gsm_voice);
        g_trollWalkieTalkie.store(m_config.troll_walkie_talkie);
        g_trollRobotVoice.store(m_config.troll_robot_voice);
        g_trollCctv.store(m_config.troll_cctv);
        g_trollFakeLag.store(m_config.troll_fake_lag);

        // Разрешение по умолчанию
        applyResolutionString(m_config.phone_resolution, false);

        // Оптика Studio Optics
        StudioOptics::instance().setZoom(m_config.optics_zoom);
        StudioOptics::instance().setBrightness(m_config.optics_brightness);
        StudioOptics::instance().setContrast(m_config.optics_contrast);
        StudioOptics::instance().setSaturation(m_config.optics_saturation);
        StudioOptics::instance().setColorTemp(m_config.optics_temp);
        StudioOptics::instance().setLutPreset(m_config.optics_lut);
    }

    void load() {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::filesystem::path cfgPath = getConfigFilePath();

        std::ifstream file(cfgPath);
        if (!file.is_open()) {
            // Файл отсутствует — сохраняем дефолтный конфиг
            applyConfigToSystem();
            saveInternal();
            return;
        }

        std::stringstream ss;
        ss << file.rdbuf();
        std::string json = ss.str();
        file.close();

        if (json.empty() || json.find('{') == std::string::npos) {
            applyConfigToSystem();
            saveInternal();
            return;
        }

        // Робастный парсинг всех полей
        m_config.mirror_enabled = extractBool(json, "mirror_enabled", m_config.mirror_enabled);
        m_config.flip180 = extractBool(json, "flip180", m_config.flip180);
        m_config.lang = extractString(json, "lang", m_config.lang);
        m_config.ui_skin = extractString(json, "ui_skin", m_config.ui_skin);
        if (m_config.ui_skin != "index3.html") m_config.ui_skin = "index2.html";
        m_config.last_connection_mode = extractString(json, "last_connection_mode", m_config.last_connection_mode);
        m_config.phone_resolution = extractString(json, "phone_resolution", m_config.phone_resolution);
        m_config.phone_fps = extractInt(json, "phone_fps", m_config.phone_fps);
        m_config.phone_codec = extractString(json, "phone_codec", m_config.phone_codec);
        m_config.phone_bitrate = extractInt(json, "phone_bitrate", m_config.phone_bitrate);
        m_config.is_vertical = extractBool(json, "is_vertical", m_config.is_vertical);
        m_config.aspect_ratio_mode = extractInt(json, "aspect_ratio_mode", m_config.aspect_ratio_mode);
        m_config.audio_device_id = extractString(json, "audio_device_id", m_config.audio_device_id);
        m_config.audio_output_device_id = extractString(json, "audio_output_device_id", m_config.audio_output_device_id);
        m_config.audio_volume = extractInt(json, "audio_volume", m_config.audio_volume);
        m_config.audio_delay = extractInt(json, "audio_delay", m_config.audio_delay);
        m_config.noise_gate = extractBool(json, "noise_gate", m_config.noise_gate);
        m_config.noise_gate_threshold = extractFloat(json, "noise_gate_threshold", m_config.noise_gate_threshold);
        m_config.ai_noise = extractBool(json, "ai_noise", m_config.ai_noise);
        m_config.agc = extractBool(json, "agc", m_config.agc);
        m_config.declicker = extractBool(json, "declicker", m_config.declicker);
        m_config.eq_low = extractInt(json, "eq_low", m_config.eq_low);
        m_config.eq_mid = extractInt(json, "eq_mid", m_config.eq_mid);
        m_config.eq_high = extractInt(json, "eq_high", m_config.eq_high);
        m_config.optics_zoom = extractFloat(json, "optics_zoom", m_config.optics_zoom);
        m_config.optics_brightness = extractInt(json, "optics_brightness", m_config.optics_brightness);
        m_config.optics_contrast = extractInt(json, "optics_contrast", m_config.optics_contrast);
        m_config.optics_saturation = extractInt(json, "optics_saturation", m_config.optics_saturation);
        m_config.optics_temp = extractInt(json, "optics_temp", m_config.optics_temp);
        m_config.optics_lut = extractInt(json, "optics_lut", m_config.optics_lut);
        m_config.close_to_tray = extractBool(json, "close_to_tray", m_config.close_to_tray);
        m_config.start_with_windows = extractBool(json, "start_with_windows", isWindowsAutostartEnabled());
        m_config.show_console = extractBool(json, "show_console", m_config.show_console);
        m_config.show_hud_stats = extractBool(json, "show_hud_stats", m_config.show_hud_stats);
        m_config.auto_check_updates = extractBool(json, "auto_check_updates", m_config.auto_check_updates);
        if (hasKey(json, "last_update_check_ts")) {
            std::regex re("\"last_update_check_ts\"\\s*:\\s*([0-9]+)");
            std::smatch match;
            if (std::regex_search(json, match, re)) {
                try { m_config.last_update_check_ts = std::stoll(match[1].str()); } catch (...) {}
            }
        }

        m_config.troll_fps_5 = extractBool(json, "troll_fps_5", m_config.troll_fps_5);
        m_config.troll_pixelate = extractInt(json, "troll_pixelate", m_config.troll_pixelate);
        m_config.troll_glitch = extractBool(json, "troll_glitch", m_config.troll_glitch);
        m_config.troll_bitcrush = extractBool(json, "troll_bitcrush", m_config.troll_bitcrush);
        m_config.troll_overexposure = extractBool(json, "troll_overexposure", m_config.troll_overexposure);
        m_config.troll_gsm_voice = extractBool(json, "troll_gsm_voice", m_config.troll_gsm_voice);
        m_config.troll_walkie_talkie = extractBool(json, "troll_walkie_talkie", m_config.troll_walkie_talkie);
        m_config.troll_robot_voice = extractBool(json, "troll_robot_voice", m_config.troll_robot_voice);
        m_config.troll_cctv = extractBool(json, "troll_cctv", m_config.troll_cctv);
        m_config.troll_fake_lag = extractBool(json, "troll_fake_lag", m_config.troll_fake_lag);

        // Парсинг доверенных устройств (белый список сопряженных узлов)
        m_config.paired_devices.clear();
        std::regex reArray(R"("paired_devices"\s*:\s*\[([\s\S]*?)\])");
        std::smatch mArr;
        if (std::regex_search(json, mArr, reArray)) {
            std::string arrContent = mArr[1].str();
            std::regex reObj(R"(\{([^{}]*)\})");
            auto b = std::sregex_iterator(arrContent.begin(), arrContent.end(), reObj);
            auto e = std::sregex_iterator();
            for (auto it = b; it != e; ++it) {
                std::string objStr = (*it)[1].str();
                PairedDeviceInfo dev;
                dev.name = extractString("{" + objStr + "}", "name", "");
                dev.ip = extractString("{" + objStr + "}", "ip", "");
                dev.token = extractString("{" + objStr + "}", "token", "");
                if (!dev.ip.empty()) {
                    m_config.paired_devices.push_back(dev);
                }
            }
        }

        applyConfigToSystem();
    }

    void updateFromJson(const std::string& json) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (json.empty()) return;

        if (hasKey(json, "mirror_enabled")) m_config.mirror_enabled = extractBool(json, "mirror_enabled", m_config.mirror_enabled);
        if (hasKey(json, "flip180")) m_config.flip180 = extractBool(json, "flip180", m_config.flip180);
        if (hasKey(json, "lang")) m_config.lang = extractString(json, "lang", m_config.lang);
        if (hasKey(json, "ui_skin")) {
            m_config.ui_skin = extractString(json, "ui_skin", m_config.ui_skin);
            if (m_config.ui_skin != "index3.html") m_config.ui_skin = "index2.html";
        }
        if (hasKey(json, "last_connection_mode")) m_config.last_connection_mode = extractString(json, "last_connection_mode", m_config.last_connection_mode);
        if (hasKey(json, "phone_resolution")) {
            m_config.phone_resolution = extractString(json, "phone_resolution", m_config.phone_resolution);
            applyResolutionString(m_config.phone_resolution, true);
        }
        if (hasKey(json, "phone_fps")) m_config.phone_fps = extractInt(json, "phone_fps", m_config.phone_fps);
        if (hasKey(json, "phone_codec")) m_config.phone_codec = extractString(json, "phone_codec", m_config.phone_codec);
        if (hasKey(json, "phone_bitrate")) m_config.phone_bitrate = extractInt(json, "phone_bitrate", m_config.phone_bitrate);
        if (hasKey(json, "is_vertical")) m_config.is_vertical = extractBool(json, "is_vertical", m_config.is_vertical);
        if (hasKey(json, "aspect_ratio_mode")) m_config.aspect_ratio_mode = extractInt(json, "aspect_ratio_mode", m_config.aspect_ratio_mode);
        if (hasKey(json, "audio_device_id")) m_config.audio_device_id = extractString(json, "audio_device_id", m_config.audio_device_id);
        if (hasKey(json, "audio_output_device_id")) m_config.audio_output_device_id = extractString(json, "audio_output_device_id", m_config.audio_output_device_id);
        if (hasKey(json, "audio_volume")) m_config.audio_volume = extractInt(json, "audio_volume", m_config.audio_volume);
        if (hasKey(json, "audio_delay")) m_config.audio_delay = extractInt(json, "audio_delay", m_config.audio_delay);
        if (hasKey(json, "noise_gate")) m_config.noise_gate = extractBool(json, "noise_gate", m_config.noise_gate);
        if (hasKey(json, "noise_gate_threshold")) m_config.noise_gate_threshold = extractFloat(json, "noise_gate_threshold", m_config.noise_gate_threshold);
        if (hasKey(json, "ai_noise")) m_config.ai_noise = extractBool(json, "ai_noise", m_config.ai_noise);
        if (hasKey(json, "agc")) m_config.agc = extractBool(json, "agc", m_config.agc);
        if (hasKey(json, "declicker")) m_config.declicker = extractBool(json, "declicker", m_config.declicker);
        if (hasKey(json, "eq_low")) m_config.eq_low = extractInt(json, "eq_low", m_config.eq_low);
        if (hasKey(json, "eq_mid")) m_config.eq_mid = extractInt(json, "eq_mid", m_config.eq_mid);
        if (hasKey(json, "eq_high")) m_config.eq_high = extractInt(json, "eq_high", m_config.eq_high);
        if (hasKey(json, "optics_zoom")) m_config.optics_zoom = extractFloat(json, "optics_zoom", m_config.optics_zoom);
        if (hasKey(json, "optics_brightness")) m_config.optics_brightness = extractInt(json, "optics_brightness", m_config.optics_brightness);
        if (hasKey(json, "optics_contrast")) m_config.optics_contrast = extractInt(json, "optics_contrast", m_config.optics_contrast);
        if (hasKey(json, "optics_saturation")) m_config.optics_saturation = extractInt(json, "optics_saturation", m_config.optics_saturation);
        if (hasKey(json, "optics_temp")) m_config.optics_temp = extractInt(json, "optics_temp", m_config.optics_temp);
        if (hasKey(json, "optics_lut")) m_config.optics_lut = extractInt(json, "optics_lut", m_config.optics_lut);
        if (hasKey(json, "close_to_tray")) m_config.close_to_tray = extractBool(json, "close_to_tray", m_config.close_to_tray);
        if (hasKey(json, "start_with_windows")) {
            m_config.start_with_windows = extractBool(json, "start_with_windows", m_config.start_with_windows);
            setWindowsAutostart(m_config.start_with_windows);
        }
        if (hasKey(json, "show_console")) {
            m_config.show_console = extractBool(json, "show_console", m_config.show_console);
            g_app.setConsoleVisible(m_config.show_console);
        }
        if (hasKey(json, "show_hud_stats")) m_config.show_hud_stats = extractBool(json, "show_hud_stats", m_config.show_hud_stats);
        if (hasKey(json, "auto_check_updates")) m_config.auto_check_updates = extractBool(json, "auto_check_updates", m_config.auto_check_updates);
        if (hasKey(json, "last_update_check_ts")) {
            std::regex re("\"last_update_check_ts\"\\s*:\\s*([0-9]+)");
            std::smatch match;
            if (std::regex_search(json, match, re)) {
                try { m_config.last_update_check_ts = std::stoll(match[1].str()); } catch (...) {}
            }
        }

        if (hasKey(json, "troll_fps_5")) m_config.troll_fps_5 = extractBool(json, "troll_fps_5", m_config.troll_fps_5);
        if (hasKey(json, "troll_pixelate")) m_config.troll_pixelate = extractInt(json, "troll_pixelate", m_config.troll_pixelate);
        if (hasKey(json, "troll_glitch")) m_config.troll_glitch = extractBool(json, "troll_glitch", m_config.troll_glitch);
        if (hasKey(json, "troll_bitcrush")) m_config.troll_bitcrush = extractBool(json, "troll_bitcrush", m_config.troll_bitcrush);
        if (hasKey(json, "troll_overexposure")) m_config.troll_overexposure = extractBool(json, "troll_overexposure", m_config.troll_overexposure);
        if (hasKey(json, "troll_gsm_voice")) m_config.troll_gsm_voice = extractBool(json, "troll_gsm_voice", m_config.troll_gsm_voice);
        if (hasKey(json, "troll_walkie_talkie")) m_config.troll_walkie_talkie = extractBool(json, "troll_walkie_talkie", m_config.troll_walkie_talkie);
        if (hasKey(json, "troll_robot_voice")) m_config.troll_robot_voice = extractBool(json, "troll_robot_voice", m_config.troll_robot_voice);
        if (hasKey(json, "troll_cctv")) m_config.troll_cctv = extractBool(json, "troll_cctv", m_config.troll_cctv);
        if (hasKey(json, "troll_fake_lag")) m_config.troll_fake_lag = extractBool(json, "troll_fake_lag", m_config.troll_fake_lag);

        applyConfigToSystem();
        saveInternal();
    }

    void setShowHudStats(bool show) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_config.show_hud_stats = show;
        saveInternal();
    }

    void setAutoCheckUpdates(bool enabled) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_config.auto_check_updates = enabled;
        saveInternal();
    }

    void setStartWithWindows(bool enabled) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_config.start_with_windows = enabled;
        setWindowsAutostart(enabled);
        saveInternal();
    }

    void setLastUpdateCheck(int64_t ts) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_config.last_update_check_ts = ts;
        saveInternal();
    }

    void save() {
        std::lock_guard<std::mutex> lock(m_mutex);
        syncLiveGlobalsToConfig();
        saveInternal();
    }

    void resetToDefaults() {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::string currentSkin = m_config.ui_skin;
        if (currentSkin != "index3.html") currentSkin = "index2.html";
        std::string currentLang = m_config.lang;
        auto currentPaired = m_config.paired_devices;
        m_config = AppConfig{};
        m_config.ui_skin = currentSkin;
        m_config.lang = currentLang;
        m_config.paired_devices = currentPaired;
        applyConfigToSystem();
        saveInternal();
    }

    bool isDeviceAuthorized(const std::string& ip, const std::string& token = "") {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (token.empty()) return false;
        for (auto& dev : m_config.paired_devices) {
            if (!dev.token.empty() && dev.token == token) {
                if (!ip.empty() && dev.ip != ip) {
                    dev.ip = ip;
                    m_runtimeTokens[ip] = token;
                    saveInternal();
                }
                return true;
            }
        }
        return false;
    }

    std::vector<PairedDeviceInfo> getPairedDevices() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_config.paired_devices;
    }

    void setRuntimeToken(const std::string& ip, const std::string& token) {
        if (ip.empty() || token.empty()) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_runtimeTokens[ip] = token;
    }

    std::string getTokenForDevice(const std::string& ip, const std::string& name = "") {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!ip.empty()) {
            for (const auto& dev : m_config.paired_devices) {
                if (dev.ip == ip && !dev.token.empty()) {
                    return dev.token;
                }
            }
            auto it = m_runtimeTokens.find(ip);
            if (it != m_runtimeTokens.end()) {
                return it->second;
            }
        }
        if (!name.empty()) {
            for (auto it = m_config.paired_devices.rbegin(); it != m_config.paired_devices.rend(); ++it) {
                if (it->name == name && !it->token.empty()) {
                    return it->token;
                }
            }
        }
        return "";
    }

    std::string getTokenForIp(const std::string& ip) {
        return getTokenForDevice(ip, "");
    }

    static void applyResolutionString(const std::string& res, bool markChanged = true) {
        int newW = 1920, newH = 1080;
        if (res == "720p") {
            newW = 1280;
            newH = 720;
        } else if (res == "4k") {
            newW = 3840;
            newH = 2160;
        }
        int prevW = g_currentWidth.exchange(newW);
        int prevH = g_currentHeight.exchange(newH);
        if (markChanged && (prevW != newW || prevH != newH)) {
            g_resolutionChanged.store(true);
        }
    }

    void syncPhoneConfig(const std::string& resolution, int fps, int bitrateMbps) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!resolution.empty()) {
            m_config.phone_resolution = resolution;
            applyResolutionString(m_config.phone_resolution, true);
        }
        if (fps > 0) {
            m_config.phone_fps = fps;
            g_currentFps.store(fps);
        }
        if (bitrateMbps > 0) {
            m_config.phone_bitrate = (std::clamp)(bitrateMbps, 1, 12);
        }
        saveInternal();
    }

    void addPairedDevice(const std::string& name, const std::string& ip, const std::string& token) {
        std::lock_guard<std::mutex> lock(m_mutex);
        bool found = false;
        for (auto& dev : m_config.paired_devices) {
            if (dev.ip == ip || (!token.empty() && dev.token == token)) {
                if (!name.empty()) dev.name = name;
                if (!ip.empty()) dev.ip = ip;
                if (!token.empty()) dev.token = token;
                found = true;
                break;
            }
        }
        if (!found) {
            m_config.paired_devices.push_back({ name, ip, token });
        }
        if (!token.empty() && !ip.empty()) {
            m_runtimeTokens[ip] = token;
        }
        saveInternal();
    }

    void removePairedDevice(const std::string& ip) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_runtimeTokens.erase(ip);
        m_config.paired_devices.erase(
            std::remove_if(m_config.paired_devices.begin(), m_config.paired_devices.end(),
                [&ip](const PairedDeviceInfo& d) { return d.ip == ip; }),
            m_config.paired_devices.end()
        );
        saveInternal();
    }

    std::string toJson() {
        std::lock_guard<std::mutex> lock(m_mutex);
        syncLiveGlobalsToConfig();
        return serializeConfig(m_config);
    }

private:
    static std::string serializeConfig(const AppConfig& c) {
        std::ostringstream ss;
        ss << "{\n"
           << "  \"mirror_enabled\": " << (c.mirror_enabled ? "true" : "false") << ",\n"
           << "  \"flip180\": " << (c.flip180 ? "true" : "false") << ",\n"
           << "  \"lang\": \"" << escapeJsonString(c.lang) << "\",\n"
           << "  \"ui_skin\": \"" << escapeJsonString(c.ui_skin) << "\",\n"
           << "  \"last_connection_mode\": \"" << escapeJsonString(c.last_connection_mode) << "\",\n"
           << "  \"phone_resolution\": \"" << escapeJsonString(c.phone_resolution) << "\",\n"
           << "  \"phone_fps\": " << c.phone_fps << ",\n"
           << "  \"phone_codec\": \"" << escapeJsonString(c.phone_codec) << "\",\n"
           << "  \"phone_bitrate\": " << c.phone_bitrate << ",\n"
           << "  \"is_vertical\": " << (c.is_vertical ? "true" : "false") << ",\n"
           << "  \"aspect_ratio_mode\": " << c.aspect_ratio_mode << ",\n"
           << "  \"audio_device_id\": \"" << escapeJsonString(c.audio_device_id) << "\",\n"
           << "  \"audio_output_device_id\": \"" << escapeJsonString(c.audio_output_device_id) << "\",\n"
           << "  \"audio_volume\": " << c.audio_volume << ",\n"
           << "  \"audio_delay\": " << c.audio_delay << ",\n"
           << "  \"noise_gate\": " << (c.noise_gate ? "true" : "false") << ",\n"
           << "  \"noise_gate_threshold\": " << c.noise_gate_threshold << ",\n"
           << "  \"ai_noise\": " << (c.ai_noise ? "true" : "false") << ",\n"
           << "  \"agc\": " << (c.agc ? "true" : "false") << ",\n"
           << "  \"declicker\": " << (c.declicker ? "true" : "false") << ",\n"
           << "  \"eq_low\": " << c.eq_low << ",\n"
           << "  \"eq_mid\": " << c.eq_mid << ",\n"
           << "  \"eq_high\": " << c.eq_high << ",\n"
           << "  \"optics_zoom\": " << c.optics_zoom << ",\n"
           << "  \"optics_brightness\": " << c.optics_brightness << ",\n"
           << "  \"optics_contrast\": " << c.optics_contrast << ",\n"
           << "  \"optics_saturation\": " << c.optics_saturation << ",\n"
           << "  \"optics_temp\": " << c.optics_temp << ",\n"
           << "  \"optics_lut\": " << c.optics_lut << ",\n"
           << "  \"close_to_tray\": " << (c.close_to_tray ? "true" : "false") << ",\n"
           << "  \"start_with_windows\": " << (c.start_with_windows ? "true" : "false") << ",\n"
           << "  \"show_console\": " << (c.show_console ? "true" : "false") << ",\n"
           << "  \"show_hud_stats\": " << (c.show_hud_stats ? "true" : "false") << ",\n"
           << "  \"auto_check_updates\": " << (c.auto_check_updates ? "true" : "false") << ",\n"
           << "  \"last_update_check_ts\": " << c.last_update_check_ts << ",\n"
           << "  \"troll_fps_5\": " << (c.troll_fps_5 ? "true" : "false") << ",\n"
           << "  \"troll_pixelate\": " << c.troll_pixelate << ",\n"
           << "  \"troll_glitch\": " << (c.troll_glitch ? "true" : "false") << ",\n"
           << "  \"troll_bitcrush\": " << (c.troll_bitcrush ? "true" : "false") << ",\n"
           << "  \"troll_overexposure\": " << (c.troll_overexposure ? "true" : "false") << ",\n"
           << "  \"troll_gsm_voice\": " << (c.troll_gsm_voice ? "true" : "false") << ",\n"
           << "  \"troll_walkie_talkie\": " << (c.troll_walkie_talkie ? "true" : "false") << ",\n"
           << "  \"troll_robot_voice\": " << (c.troll_robot_voice ? "true" : "false") << ",\n"
           << "  \"troll_cctv\": " << (c.troll_cctv ? "true" : "false") << ",\n"
           << "  \"troll_fake_lag\": " << (c.troll_fake_lag ? "true" : "false") << ",\n"
           << "  \"paired_devices\": [\n";
        for (size_t i = 0; i < c.paired_devices.size(); ++i) {
            ss << "    {\"name\": \"" << escapeJsonString(c.paired_devices[i].name)
               << "\", \"ip\": \"" << escapeJsonString(c.paired_devices[i].ip)
               << "\", \"token\": \"" << escapeJsonString(c.paired_devices[i].token) << "\"}";
            if (i + 1 < c.paired_devices.size()) ss << ",";
            ss << "\n";
        }
        ss << "  ]\n"
           << "}\n";
        return ss.str();
    }

    void saveInternal() {
        if (!m_diskSaveEnabled) return;
        std::filesystem::path cfgPath = getConfigFilePath();
        std::filesystem::path tmpPath = cfgPath;
        tmpPath += L".tmp";

        std::string jsonStr = serializeConfig(m_config);

        {
            std::ofstream out(tmpPath, std::ios::trunc | std::ios::binary);
            if (!out.is_open()) return;
            out << jsonStr;
            out.flush();
        }

        if (!MoveFileExW(tmpPath.c_str(), cfgPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            // Direct write fallback
            std::ofstream direct(cfgPath, std::ios::trunc | std::ios::binary);
            if (direct.is_open()) {
                direct << jsonStr;
                direct.flush();
            }
            std::error_code ec;
            std::filesystem::remove(tmpPath, ec);
        }
    }
};
