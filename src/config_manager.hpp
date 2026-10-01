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

// Внешние атомики и переменные ядра VirtualCamNative
extern std::atomic<int> g_currentWidth;
extern std::atomic<int> g_currentHeight;
extern std::atomic<int> g_currentFps;
extern std::atomic<bool> g_fpsChanged;
extern std::atomic<bool> g_resolutionChanged;

extern std::atomic<bool> g_mirrorEnabled;
extern std::atomic<bool> g_flip180;
extern std::atomic<bool> g_isFrontCamera;
extern std::atomic<bool> g_isLandscapeMode;

extern std::atomic<float> g_audioVolume;
extern std::atomic<int> g_audioDelayMs;
extern std::atomic<bool> g_noiseGateEnabled;

extern std::atomic<bool> g_closeToTray;
extern std::atomic<bool> g_showConsole;
extern std::string g_targetMode;
extern std::string g_targetIp;

void setConsoleVisible(bool visible);

struct AppConfig {
    bool mirror_enabled = false;
    bool flip180 = false;
    std::string lang = "ru";
    std::string ui_skin = "index3.html";
    std::string last_connection_mode = "wifi";
    std::string phone_resolution = "1080p";
    int phone_fps = 60;
    std::string phone_codec = "h265";
    int phone_bitrate = 10;
    bool is_vertical = true;
    int audio_volume = 100;
    int audio_delay = 0;
    bool noise_gate = true;
    float optics_zoom = 1.0f;
    int optics_brightness = 0;
    int optics_contrast = 100;
    int optics_saturation = 100;
    int optics_temp = 0;
    int optics_lut = 0;
    bool close_to_tray = true;
    bool show_console = false;
};

class ConfigManager {
private:
    AppConfig m_config;
    std::mutex m_mutex;

    ConfigManager() = default;

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

public:
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
        g_audioVolume.store(m_config.audio_volume / 100.0f);
        g_audioDelayMs.store(m_config.audio_delay);
        g_noiseGateEnabled.store(m_config.noise_gate);
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
        m_config.last_connection_mode = extractString(json, "last_connection_mode", m_config.last_connection_mode);
        m_config.phone_resolution = extractString(json, "phone_resolution", m_config.phone_resolution);
        m_config.phone_fps = extractInt(json, "phone_fps", m_config.phone_fps);
        m_config.phone_codec = extractString(json, "phone_codec", m_config.phone_codec);
        m_config.phone_bitrate = extractInt(json, "phone_bitrate", m_config.phone_bitrate);
        m_config.is_vertical = extractBool(json, "is_vertical", m_config.is_vertical);
        m_config.audio_volume = extractInt(json, "audio_volume", m_config.audio_volume);
        m_config.audio_delay = extractInt(json, "audio_delay", m_config.audio_delay);
        m_config.noise_gate = extractBool(json, "noise_gate", m_config.noise_gate);
        m_config.optics_zoom = extractFloat(json, "optics_zoom", m_config.optics_zoom);
        m_config.optics_brightness = extractInt(json, "optics_brightness", m_config.optics_brightness);
        m_config.optics_contrast = extractInt(json, "optics_contrast", m_config.optics_contrast);
        m_config.optics_saturation = extractInt(json, "optics_saturation", m_config.optics_saturation);
        m_config.optics_temp = extractInt(json, "optics_temp", m_config.optics_temp);
        m_config.optics_lut = extractInt(json, "optics_lut", m_config.optics_lut);
        m_config.close_to_tray = extractBool(json, "close_to_tray", m_config.close_to_tray);
        m_config.show_console = extractBool(json, "show_console", m_config.show_console);

        applyConfigToSystem();
    }

    void updateFromJson(const std::string& json) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (json.empty()) return;

        if (hasKey(json, "mirror_enabled")) m_config.mirror_enabled = extractBool(json, "mirror_enabled", m_config.mirror_enabled);
        if (hasKey(json, "flip180")) m_config.flip180 = extractBool(json, "flip180", m_config.flip180);
        if (hasKey(json, "lang")) m_config.lang = extractString(json, "lang", m_config.lang);
        if (hasKey(json, "ui_skin")) m_config.ui_skin = extractString(json, "ui_skin", m_config.ui_skin);
        if (hasKey(json, "last_connection_mode")) m_config.last_connection_mode = extractString(json, "last_connection_mode", m_config.last_connection_mode);
        if (hasKey(json, "phone_resolution")) m_config.phone_resolution = extractString(json, "phone_resolution", m_config.phone_resolution);
        if (hasKey(json, "phone_fps")) m_config.phone_fps = extractInt(json, "phone_fps", m_config.phone_fps);
        if (hasKey(json, "phone_codec")) m_config.phone_codec = extractString(json, "phone_codec", m_config.phone_codec);
        if (hasKey(json, "phone_bitrate")) m_config.phone_bitrate = extractInt(json, "phone_bitrate", m_config.phone_bitrate);
        if (hasKey(json, "is_vertical")) m_config.is_vertical = extractBool(json, "is_vertical", m_config.is_vertical);
        if (hasKey(json, "audio_volume")) m_config.audio_volume = extractInt(json, "audio_volume", m_config.audio_volume);
        if (hasKey(json, "audio_delay")) m_config.audio_delay = extractInt(json, "audio_delay", m_config.audio_delay);
        if (hasKey(json, "noise_gate")) m_config.noise_gate = extractBool(json, "noise_gate", m_config.noise_gate);
        if (hasKey(json, "optics_zoom")) m_config.optics_zoom = extractFloat(json, "optics_zoom", m_config.optics_zoom);
        if (hasKey(json, "optics_brightness")) m_config.optics_brightness = extractInt(json, "optics_brightness", m_config.optics_brightness);
        if (hasKey(json, "optics_contrast")) m_config.optics_contrast = extractInt(json, "optics_contrast", m_config.optics_contrast);
        if (hasKey(json, "optics_saturation")) m_config.optics_saturation = extractInt(json, "optics_saturation", m_config.optics_saturation);
        if (hasKey(json, "optics_temp")) m_config.optics_temp = extractInt(json, "optics_temp", m_config.optics_temp);
        if (hasKey(json, "optics_lut")) m_config.optics_lut = extractInt(json, "optics_lut", m_config.optics_lut);
        if (hasKey(json, "close_to_tray")) m_config.close_to_tray = extractBool(json, "close_to_tray", m_config.close_to_tray);
        if (hasKey(json, "show_console")) {
            m_config.show_console = extractBool(json, "show_console", m_config.show_console);
            setConsoleVisible(m_config.show_console);
        }

        applyConfigToSystem();
        saveInternal();
    }

    void save() {
        std::lock_guard<std::mutex> lock(m_mutex);
        syncLiveGlobalsToConfig();
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
           << "  \"audio_volume\": " << c.audio_volume << ",\n"
           << "  \"audio_delay\": " << c.audio_delay << ",\n"
           << "  \"noise_gate\": " << (c.noise_gate ? "true" : "false") << ",\n"
           << "  \"optics_zoom\": " << c.optics_zoom << ",\n"
           << "  \"optics_brightness\": " << c.optics_brightness << ",\n"
           << "  \"optics_contrast\": " << c.optics_contrast << ",\n"
           << "  \"optics_saturation\": " << c.optics_saturation << ",\n"
           << "  \"optics_temp\": " << c.optics_temp << ",\n"
           << "  \"optics_lut\": " << c.optics_lut << ",\n"
           << "  \"close_to_tray\": " << (c.close_to_tray ? "true" : "false") << ",\n"
           << "  \"show_console\": " << (c.show_console ? "true" : "false") << "\n"
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
