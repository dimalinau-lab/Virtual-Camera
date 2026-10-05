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
#include <windows.h>
#include "studio_optics.hpp"

#include "app_state.hpp"

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
    int audio_volume = 100;
    int audio_delay = 0;
    bool noise_gate = true;
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
    bool show_console = false;
    bool show_hud_stats = false;
    bool auto_check_updates = true;
    int64_t last_update_check_ts = 0;
};

class ConfigManager {
private:
    AppConfig m_config;
    std::mutex m_mutex;

    ConfigManager() = default;

public:
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
        std::regex re("\"" + key + "\"\\s*:\\s*\"([^\"]*)\"");
        std::smatch match;
        if (std::regex_search(json, match, re)) {
            return match[1].str();
        }
        return defaultVal;
    }

    static bool hasKey(const std::string& json, const std::string& key) {
        std::regex re("\"" + key + "\"\\s*:");
        return std::regex_search(json, re);
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
        m_config.audio_device_id = g_audioInputDeviceId;
        m_config.audio_volume = (std::clamp)((int)std::round(g_audioVolume.load() * 100.0f), 0, 100);
        m_config.audio_delay = g_audioDelayMs.load();
        m_config.noise_gate = g_noiseGateEnabled.load();
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
        g_audioVolume.store(m_config.audio_volume / 100.0f);
        g_audioDelayMs.store(m_config.audio_delay);
        g_noiseGateEnabled.store(m_config.noise_gate);
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

        // Разрешение по умолчанию
        if (m_config.phone_resolution == "720p") {
            g_currentWidth.store(1280);
            g_currentHeight.store(720);
        } else if (m_config.phone_resolution == "4k") {
            g_currentWidth.store(3840);
            g_currentHeight.store(2160);
        } else {
            g_currentWidth.store(1920);
            g_currentHeight.store(1080);
        }

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
        m_config.audio_device_id = extractString(json, "audio_device_id", m_config.audio_device_id);
        m_config.audio_volume = extractInt(json, "audio_volume", m_config.audio_volume);
        m_config.audio_delay = extractInt(json, "audio_delay", m_config.audio_delay);
        m_config.noise_gate = extractBool(json, "noise_gate", m_config.noise_gate);
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
        m_config.show_console = extractBool(json, "show_console", m_config.show_console);
        m_config.show_hud_stats = extractBool(json, "show_hud_stats", m_config.show_hud_stats);

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
        if (hasKey(json, "phone_resolution")) m_config.phone_resolution = extractString(json, "phone_resolution", m_config.phone_resolution);
        if (hasKey(json, "phone_fps")) m_config.phone_fps = extractInt(json, "phone_fps", m_config.phone_fps);
        if (hasKey(json, "phone_codec")) m_config.phone_codec = extractString(json, "phone_codec", m_config.phone_codec);
        if (hasKey(json, "phone_bitrate")) m_config.phone_bitrate = extractInt(json, "phone_bitrate", m_config.phone_bitrate);
        if (hasKey(json, "is_vertical")) m_config.is_vertical = extractBool(json, "is_vertical", m_config.is_vertical);
        if (hasKey(json, "aspect_ratio_mode")) m_config.aspect_ratio_mode = extractInt(json, "aspect_ratio_mode", m_config.aspect_ratio_mode);
        if (hasKey(json, "audio_device_id")) m_config.audio_device_id = extractString(json, "audio_device_id", m_config.audio_device_id);
        if (hasKey(json, "audio_volume")) m_config.audio_volume = extractInt(json, "audio_volume", m_config.audio_volume);
        if (hasKey(json, "audio_delay")) m_config.audio_delay = extractInt(json, "audio_delay", m_config.audio_delay);
        if (hasKey(json, "noise_gate")) m_config.noise_gate = extractBool(json, "noise_gate", m_config.noise_gate);
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

        applyConfigToSystem();
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
        m_config = AppConfig{};
        m_config.ui_skin = currentSkin;
        m_config.lang = currentLang;
        applyConfigToSystem();
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
           << "  \"lang\": \"" << c.lang << "\",\n"
           << "  \"ui_skin\": \"" << c.ui_skin << "\",\n"
           << "  \"last_connection_mode\": \"" << c.last_connection_mode << "\",\n"
           << "  \"phone_resolution\": \"" << c.phone_resolution << "\",\n"
           << "  \"phone_fps\": " << c.phone_fps << ",\n"
           << "  \"phone_codec\": \"" << c.phone_codec << "\",\n"
           << "  \"phone_bitrate\": " << c.phone_bitrate << ",\n"
           << "  \"is_vertical\": " << (c.is_vertical ? "true" : "false") << ",\n"
           << "  \"aspect_ratio_mode\": " << c.aspect_ratio_mode << ",\n"
           << "  \"audio_device_id\": \"" << c.audio_device_id << "\",\n"
           << "  \"audio_volume\": " << c.audio_volume << ",\n"
           << "  \"audio_delay\": " << c.audio_delay << ",\n"
           << "  \"noise_gate\": " << (c.noise_gate ? "true" : "false") << ",\n"
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
           << "  \"show_console\": " << (c.show_console ? "true" : "false") << ",\n"
           << "  \"show_hud_stats\": " << (c.show_hud_stats ? "true" : "false") << ",\n"
           << "  \"auto_check_updates\": " << (c.auto_check_updates ? "true" : "false") << ",\n"
           << "  \"last_update_check_ts\": " << c.last_update_check_ts << "\n"
           << "}\n";
        return ss.str();
    }

    void saveInternal() {
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
