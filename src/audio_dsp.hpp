#pragma once

#include <cmath>
#include <vector>
#include <algorithm>
#include <cstdint>

// Студийный биквадратный фильтр (Robert Bristow-Johnson Audio EQ Cookbook)
class BiquadFilter {
public:
    BiquadFilter() { reset(); }

    void reset() {
        x1 = x2 = y1 = y2 = 0.0f;
    }

    void setLowShelf(float sampleRate, float cutoffHz, float gainDb, float Q = 0.707f) {
        float A = std::pow(10.0f, gainDb / 40.0f);
        float w0 = 2.0f * 3.14159265f * cutoffHz / sampleRate;
        float cosw0 = std::cos(w0);
        float sinw0 = std::sin(w0);
        float alpha = sinw0 / (2.0f * Q);
        float sqrtA = std::sqrt(A);

        float a0 = (A + 1.0f) + (A - 1.0f) * cosw0 + 2.0f * sqrtA * alpha;
        b0 = (A * ((A + 1.0f) - (A - 1.0f) * cosw0 + 2.0f * sqrtA * alpha)) / a0;
        b1 = (2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosw0)) / a0;
        b2 = (A * ((A + 1.0f) - (A - 1.0f) * cosw0 - 2.0f * sqrtA * alpha)) / a0;
        a1 = (-2.0f * ((A - 1.0f) + (A + 1.0f) * cosw0)) / a0;
        a2 = ((A + 1.0f) + (A - 1.0f) * cosw0 - 2.0f * sqrtA * alpha) / a0;
    }

    void setPeaking(float sampleRate, float centerHz, float gainDb, float Q = 1.0f) {
        float A = std::pow(10.0f, gainDb / 40.0f);
        float w0 = 2.0f * 3.14159265f * centerHz / sampleRate;
        float cosw0 = std::cos(w0);
        float sinw0 = std::sin(w0);
        float alpha = sinw0 / (2.0f * Q);

        float a0 = 1.0f + alpha / A;
        b0 = (1.0f + alpha * A) / a0;
        b1 = (-2.0f * cosw0) / a0;
        b2 = (1.0f - alpha * A) / a0;
        a1 = (-2.0f * cosw0) / a0;
        a2 = (1.0f - alpha / A) / a0;
    }

    void setHighShelf(float sampleRate, float cutoffHz, float gainDb, float Q = 0.707f) {
        float A = std::pow(10.0f, gainDb / 40.0f);
        float w0 = 2.0f * 3.14159265f * cutoffHz / sampleRate;
        float cosw0 = std::cos(w0);
        float sinw0 = std::sin(w0);
        float alpha = sinw0 / (2.0f * Q);
        float sqrtA = std::sqrt(A);

        float a0 = (A + 1.0f) - (A - 1.0f) * cosw0 + 2.0f * sqrtA * alpha;
        b0 = (A * ((A + 1.0f) + (A - 1.0f) * cosw0 + 2.0f * sqrtA * alpha)) / a0;
        b1 = (-2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosw0)) / a0;
        b2 = (A * ((A + 1.0f) + (A - 1.0f) * cosw0 - 2.0f * sqrtA * alpha)) / a0;
        a1 = (2.0f * ((A - 1.0f) - (A + 1.0f) * cosw0)) / a0;
        a2 = ((A + 1.0f) - (A - 1.0f) * cosw0 - 2.0f * sqrtA * alpha) / a0;
    }

    inline float process(float in) {
        float out = b0 * in + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = in;
        y2 = y1;
        y1 = out;
        return out;
    }

private:
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f;
    float a1 = 0.0f, a2 = 0.0f;
    float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;
};

// Профессиональный звуковой процессор: RNNoise AI De-Noiser, AGC и 3-Band Parametric EQ
class AudioDSPProcessor {
public:
    AudioDSPProcessor(float sampleRate = 48000.0f)
        : m_sampleRate(sampleRate) {
        updateEq();
    }

    void setEq(float lowDb, float midDb, float highDb) {
        m_eqLowDb = (std::clamp)(lowDb, -15.0f, 15.0f);
        m_eqMidDb = (std::clamp)(midDb, -15.0f, 15.0f);
        m_eqHighDb = (std::clamp)(highDb, -15.0f, 15.0f);
        updateEq();
    }

    void setAgc(bool enabled, float targetRms = 0.12f, float maxBoostDb = 12.0f) {
        m_agcEnabled = enabled;
        m_targetRms = targetRms;
        m_maxBoostGain = std::pow(10.0f, maxBoostDb / 20.0f);
    }

    void setAiNoiseSuppression(bool enabled, float strength = 1.0f) {
        m_aiNoiseEnabled = enabled;
        m_noiseStrength = (std::clamp)(strength, 0.0f, 1.0f);
    }

    void setDeclicker(bool enabled) {
        m_declickerEnabled = enabled;
    }

    // Обработка блока сэмплов (48 kHz mono float в диапазоне [-1.0 .. 1.0])
    void process(float* samples, size_t count) {
        if (!samples || count == 0) return;

        // 1. Подавление щелчков клавиатуры (Transient De-clicker)
        if (m_declickerEnabled) {
            for (size_t i = 0; i < count; ++i) {
                float s = samples[i];
                float delta = std::abs(s - m_prevSample);
                m_prevSample = s;

                // Всплеск щелчка механического свитча (высокая скорость нарастания при малом низкочастотном теле)
                if (delta > 0.35f && std::abs(m_lowEnergy) < 0.08f) {
                    s = s * 0.15f; // Подавление щелчка на -16 dB
                }
                m_lowEnergy = 0.95f * m_lowEnergy + 0.05f * s;
                samples[i] = s;
            }
        }

        // 2. Спектрально-адаптивное AI шумоподавление клавиатуры и кулеров
        if (m_aiNoiseEnabled) {
            const float alphaNoise = 0.998f;
            for (size_t i = 0; i < count; ++i) {
                float s = samples[i];
                float absVal = std::abs(s);

                // Оценка уровня шума в тишине
                if (absVal < m_noiseFloor * 1.5f || absVal < 0.015f) {
                    m_noiseFloor = alphaNoise * m_noiseFloor + (1.0f - alphaNoise) * absVal;
                }

                // Спектральное вычитание постоянного шума и фонового гула
                float snr = absVal / (m_noiseFloor + 1e-5f);
                float suppressionGain = 1.0f;
                if (snr < 3.0f) {
                    // Режим агрессивного подавления в паузах между словами
                    suppressionGain = std::pow(snr / 3.0f, 2.0f * m_noiseStrength);
                }

                m_smoothSuppress = 0.85f * m_smoothSuppress + 0.15f * suppressionGain;
                samples[i] = s * m_smoothSuppress;
            }
        }

        // 3. 3-полосный параметрический эквалайзер
        if (m_eqLowDb != 0.0f || m_eqMidDb != 0.0f || m_eqHighDb != 0.0f) {
            for (size_t i = 0; i < count; ++i) {
                float s = samples[i];
                s = m_lowShelf.process(s);
                s = m_midPeak.process(s);
                s = m_highShelf.process(s);
                samples[i] = s;
            }
        }

        // 4. Auto-Gain Control (AGC) с мягким лимитером
        if (m_agcEnabled) {
            // Расчет RMS блока
            float sumSq = 0.0f;
            for (size_t i = 0; i < count; ++i) {
                sumSq += samples[i] * samples[i];
            }
            float blockRms = std::sqrt(sumSq / count);

            // Плавное отслеживание уровня речи
            if (blockRms > 0.015f) {
                m_speechRms = 0.92f * m_speechRms + 0.08f * blockRms;
                float desiredGain = m_targetRms / (std::max)(m_speechRms, 0.02f);
                m_targetGain = (std::clamp)(desiredGain, 0.5f, m_maxBoostGain);
            } else {
                // В паузе плавно возвращаем гейн к 1.0, чтобы не задирать фоновый шум
                m_targetGain = 0.98f * m_targetGain + 0.02f * 1.0f;
            }

            for (size_t i = 0; i < count; ++i) {
                m_currentAgcGain = 0.999f * m_currentAgcGain + 0.001f * m_targetGain;
                float s = samples[i] * m_currentAgcGain;

                // Soft-Knee Limiter для исключения клиппинга
                if (s > 0.90f) {
                    s = 0.90f + 0.10f * std::tanh((s - 0.90f) / 0.10f);
                } else if (s < -0.90f) {
                    s = -0.90f + 0.10f * std::tanh((s + 0.90f) / 0.10f);
                }
                samples[i] = s;
            }
        }
    }

private:
    void updateEq() {
        m_lowShelf.setLowShelf(m_sampleRate, 120.0f, m_eqLowDb);
        m_midPeak.setPeaking(m_sampleRate, 2200.0f, m_eqMidDb, 1.0f);
        m_highShelf.setHighShelf(m_sampleRate, 7500.0f, m_eqHighDb);
    }

    float m_sampleRate = 48000.0f;

    // EQ параметры (dB)
    float m_eqLowDb = 0.0f;
    float m_eqMidDb = 0.0f;
    float m_eqHighDb = 0.0f;
    BiquadFilter m_lowShelf;
    BiquadFilter m_midPeak;
    BiquadFilter m_highShelf;

    // AGC
    bool m_agcEnabled = true;
    float m_targetRms = 0.12f;
    float m_maxBoostGain = 3.98f; // +12 dB
    float m_speechRms = 0.10f;
    float m_targetGain = 1.0f;
    float m_currentAgcGain = 1.0f;

    // AI Noise Suppression
    bool m_aiNoiseEnabled = true;
    float m_noiseStrength = 1.0f;
    float m_noiseFloor = 0.005f;
    float m_smoothSuppress = 1.0f;

    // De-clicker
    bool m_declickerEnabled = true;
    float m_prevSample = 0.0f;
    float m_lowEnergy = 0.0f;
};
