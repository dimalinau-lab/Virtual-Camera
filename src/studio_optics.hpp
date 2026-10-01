#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstdint>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include <omp.h>

// ====================================================================================
// VirtualCamNative Studio Optics & Cinematic Color Grading Engine (Phase 3)
// ====================================================================================
// - Zero-cost 256-entry L1-cached RGB lookup tables (LUT) for realtime color grading
// - Cinematic presets: None, Teal & Orange, Warm Cinema, Noir B&W, Cyber Neon, Clean Studio
// - Parametric adjustments: Brightness, Contrast, Saturation, Color Temperature
// - Fixed-point Digital Zoom & Pan with smooth sub-pixel framing
// ====================================================================================

enum StudioLutPreset : int {
    LUT_PRESET_NONE = 0,
    LUT_PRESET_TEAL_ORANGE = 1,
    LUT_PRESET_WARM_CINEMA = 2,
    LUT_PRESET_NOIR_BW = 3,
    LUT_PRESET_CYBER_NEON = 4,
    LUT_PRESET_CLEAN_STUDIO = 5
};

class StudioOptics {
public:
    static StudioOptics& instance() {
        static StudioOptics s_inst;
        return s_inst;
    }

    StudioOptics() {
        rebuildLutTables();
    }

    // --- Параметры цветокоррекции ---
    void setBrightness(int val) {
        m_brightness.store((std::clamp)(val, -100, 100));
        m_dirty.store(true);
    }
    int getBrightness() const { return m_brightness.load(); }

    void setContrast(int val) {
        m_contrast.store((std::clamp)(val, 50, 200));
        m_dirty.store(true);
    }
    int getContrast() const { return m_contrast.load(); }

    void setSaturation(int val) {
        m_saturation.store((std::clamp)(val, 0, 200));
        m_dirty.store(true);
    }
    int getSaturation() const { return m_saturation.load(); }

    void setColorTemp(int val) {
        m_colorTemp.store((std::clamp)(val, -50, 50));
        m_dirty.store(true);
    }
    int getColorTemp() const { return m_colorTemp.load(); }

    void setLutPreset(int preset) {
        m_lutPreset.store((std::clamp)(preset, 0, 5));
        m_dirty.store(true);
    }
    int getLutPreset() const { return m_lutPreset.load(); }

    // --- Кадрирование и Zoom ---
    void setZoom(float zoom) {
        m_zoom.store((std::clamp)(zoom, 1.0f, 3.0f));
    }
    float getZoom() const { return m_zoom.load(); }

    void setPan(float panX, float panY) {
        m_panX.store((std::clamp)(panX, -0.5f, 0.5f));
        m_panY.store((std::clamp)(panY, -0.5f, 0.5f));
    }
    float getPanX() const { return m_panX.load(); }
    float getPanY() const { return m_panY.load(); }

    void resetAll() {
        m_brightness.store(0);
        m_contrast.store(100);
        m_saturation.store(100);
        m_colorTemp.store(0);
        m_lutPreset.store(LUT_PRESET_NONE);
        m_zoom.store(1.0f);
        m_panX.store(0.0f);
        m_panY.store(0.0f);
        m_dirty.store(true);
    }

    bool hasActiveOptics() const {
        return (m_zoom.load() > 1.02f) ||
               (m_brightness.load() != 0) ||
               (m_contrast.load() != 100) ||
               (m_saturation.load() != 100) ||
               (m_colorTemp.load() != 0) ||
               (m_lutPreset.load() != LUT_PRESET_NONE);
    }

    // --- Применение цветокоррекции на 32-битный BGRA буфер (OpenMP SIMD) ---
    void processFrame(uint32_t* canvas, int width, int height) {
        if (m_dirty.exchange(false)) {
            rebuildLutTables();
        }

        const int sat = m_saturation.load();
        const int preset = m_lutPreset.load();
        const bool needSatProcessing = (sat != 100 || preset == LUT_PRESET_NOIR_BW || preset == LUT_PRESET_TEAL_ORANGE || preset == LUT_PRESET_CYBER_NEON);

        const size_t totalPixels = (size_t)width * height;
        const uint8_t* rLut = m_lutR;
        const uint8_t* gLut = m_lutG;
        const uint8_t* bLut = m_lutB;

        if (!needSatProcessing) {
            #pragma omp parallel for schedule(static)
            for (int i = 0; i < (int)totalPixels; ++i) {
                uint32_t p = canvas[i];
                uint8_t b = bLut[p & 0xFF];
                uint8_t g = gLut[(p >> 8) & 0xFF];
                uint8_t r = rLut[(p >> 16) & 0xFF];
                canvas[i] = 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
            }
        }
        else {
            const int satFp = (sat * 256) / 100; // 8-битный fixed-point фактор насыщенности

            #pragma omp parallel for schedule(static)
            for (int i = 0; i < (int)totalPixels; ++i) {
                uint32_t p = canvas[i];
                int b = bLut[p & 0xFF];
                int g = gLut[(p >> 8) & 0xFF];
                int r = rLut[(p >> 16) & 0xFF];

                // Быстрый расчет яркости Luma (Rec. 601: Y = 0.299R + 0.587G + 0.114B)
                int luma = (r * 77 + g * 150 + b * 29) >> 8;

                if (preset == LUT_PRESET_NOIR_BW || sat == 0) {
                    canvas[i] = 0xFF000000 | ((uint32_t)luma << 16) | ((uint32_t)luma << 8) | (uint32_t)luma;
                    continue;
                }

                // Teal & Orange матричная тонировка теней и бликов
                if (preset == LUT_PRESET_TEAL_ORANGE) {
                    if (luma > 128) {
                        // Блики в теплый персиковый/оранжевый
                        int factor = luma - 128;
                        r = (std::min)(255, r + (factor * 35) / 128);
                        g = (std::min)(255, g + (factor * 10) / 128);
                        b = (std::max)(0, b - (factor * 25) / 128);
                    }
                    else {
                        // Тени в глубокий сине-зеленый (Teal/Cyan)
                        int factor = 128 - luma;
                        b = (std::min)(255, b + (factor * 35) / 128);
                        g = (std::min)(255, g + (factor * 15) / 128);
                        r = (std::max)(0, r - (factor * 25) / 128);
                    }
                }
                else if (preset == LUT_PRESET_CYBER_NEON) {
                    // Усиление пурпурных и неоново-голубых тонов
                    b = (std::min)(255, (b * 12) / 10);
                    r = (std::min)(255, (r * 11) / 10);
                    g = (g * 9) / 10;
                }

                if (sat != 100) {
                    r = luma + (((r - luma) * satFp) >> 8);
                    g = luma + (((g - luma) * satFp) >> 8);
                    b = luma + (((b - luma) * satFp) >> 8);

                    r = (std::clamp)(r, 0, 255);
                    g = (std::clamp)(g, 0, 255);
                    b = (std::clamp)(b, 0, 255);
                }

                canvas[i] = 0xFF000000 | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            }
        }
    }

    // --- Быстрое цифровое кадрирование и зумирование (OpenMP Fixed-point Zoom) ---
    void applyZoom(const uint32_t* src, uint32_t* dst, int width, int height) {
        float z = m_zoom.load();
        if (z <= 1.01f) {
            return; // 1.0x — масштабирование не требуется
        }

        // Вычисляем размер видимой области (Crop Window)
        int cropW = (int)(width / z);
        int cropH = (int)(height / z);

        // Смещение панорамирования
        float px = m_panX.load();
        float py = m_panY.load();

        int maxOffsetX = width - cropW;
        int maxOffsetY = height - cropH;

        int startX = (maxOffsetX / 2) + (int)(px * maxOffsetX);
        int startY = (maxOffsetY / 2) + (int)(py * maxOffsetY);

        startX = (std::clamp)(startX, 0, maxOffsetX);
        startY = (std::clamp)(startY, 0, maxOffsetY);

        // Fixed-point шаги выборки (16-bit precision)
        int stepX_fp = (cropW << 16) / width;
        int stepY_fp = (cropH << 16) / height;

        #pragma omp parallel for schedule(static)
        for (int dy = 0; dy < height; ++dy) {
            int srcY = startY + ((dy * stepY_fp) >> 16);
            if (srcY >= height) srcY = height - 1;
            const uint32_t* srcRow = src + srcY * width;
            uint32_t* dstRow = dst + dy * width;

            for (int dx = 0; dx < width; ++dx) {
                int srcX = startX + ((dx * stepX_fp) >> 16);
                if (srcX >= width) srcX = width - 1;
                dstRow[dx] = srcRow[srcX];
            }
        }
    }

private:
    std::atomic<int>   m_brightness{ 0 };      // -100 .. +100
    std::atomic<int>   m_contrast{ 100 };      // 50 .. 200 %
    std::atomic<int>   m_saturation{ 100 };    // 0 .. 200 %
    std::atomic<int>   m_colorTemp{ 0 };       // -50 .. +50
    std::atomic<int>   m_lutPreset{ LUT_PRESET_NONE };
    std::atomic<float> m_zoom{ 1.0f };         // 1.0x .. 3.0x
    std::atomic<float> m_panX{ 0.0f };         // -0.5 .. +0.5
    std::atomic<float> m_panY{ 0.0f };         // -0.5 .. +0.5
    std::atomic<bool>  m_dirty{ true };

    uint8_t m_lutR[256];
    uint8_t m_lutG[256];
    uint8_t m_lutB[256];

    void rebuildLutTables() {
        const float bright = m_brightness.load() * 1.28f; // Преобразование в диапазон [-128, 128]
        const float contrastFactor = (m_contrast.load() / 100.0f);
        const int temp = m_colorTemp.load();
        const int preset = m_lutPreset.load();

        // Смещение цветовой температуры
        const float redTempShift = temp * 0.7f;
        const float blueTempShift = -temp * 0.7f;

        for (int i = 0; i < 256; ++i) {
            // 1. Базовый контраст относительно центральной точки 128
            float val = 128.0f + ((float)i - 128.0f) * contrastFactor;
            // 2. Яркость
            val += bright;

            float rVal = val + redTempShift;
            float gVal = val;
            float bVal = val + blueTempShift;

            // 3. Предустановленные кинематографические профили
            if (preset == LUT_PRESET_WARM_CINEMA) {
                // Мягкий золотистый тон в светах, кинематографичный подъем теней
                rVal = 12.0f + rVal * 1.05f;
                gVal = 8.0f + gVal * 1.02f;
                bVal = 6.0f + bVal * 0.92f;
            }
            else if (preset == LUT_PRESET_NOIR_BW) {
                // Контрастный пленочный монохром со глубокими черными
                float mono = 128.0f + ((float)i - 128.0f) * (contrastFactor * 1.35f) + bright;
                rVal = gVal = bVal = mono;
            }
            else if (preset == LUT_PRESET_CLEAN_STUDIO) {
                // Студийный чистый дневной свет (чистые полутона и естественная кожа)
                rVal = rVal * 1.02f;
                gVal = gVal * 1.01f;
                bVal = bVal * 1.02f;
            }

            m_lutR[i] = (uint8_t)(std::clamp)((int)std::round(rVal), 0, 255);
            m_lutG[i] = (uint8_t)(std::clamp)((int)std::round(gVal), 0, 255);
            m_lutB[i] = (uint8_t)(std::clamp)((int)std::round(bVal), 0, 255);
        }
    }
};
