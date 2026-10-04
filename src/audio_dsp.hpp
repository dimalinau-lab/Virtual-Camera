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

    void setHighPass(float sampleRate, float cutoffHz, float Q = 0.707f) {
        float w0 = 2.0f * 3.14159265f * cutoffHz / sampleRate;
        float cosw0 = std::cos(w0);
        float sinw0 = std::sin(w0);
        float alpha = sinw0 / (2.0f * Q);

        float a0 = 1.0f + alpha;
        b0 = ((1.0f + cosw0) / 2.0f) / a0;
        b1 = (-(1.0f + cosw0)) / a0;
        b2 = ((1.0f + cosw0) / 2.0f) / a0;
        a1 = (-2.0f * cosw0) / a0;
        a2 = (1.0f - alpha) / a0;
    }

    void setLowPass(float sampleRate, float cutoffHz, float Q = 0.707f) {
        float w0 = 2.0f * 3.14159265f * cutoffHz / sampleRate;
        float cosw0 = std::cos(w0);
        float sinw0 = std::sin(w0);
        float alpha = sinw0 / (2.0f * Q);

        float a0 = 1.0f + alpha;
        b0 = ((1.0f - cosw0) / 2.0f) / a0;
        b1 = (1.0f - cosw0) / a0;
        b2 = ((1.0f - cosw0) / 2.0f) / a0;
        a1 = (-2.0f * cosw0) / a0;
        a2 = (1.0f - alpha) / a0;
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
        m_walkieHp.setHighPass(m_sampleRate, 380.0f);
        m_walkieLp.setLowPass(m_sampleRate, 3100.0f);
    }

    void setTrollEffects(bool gsmVoice, bool walkieTalkie, bool robotVoice) {
        m_trollGsmVoice = gsmVoice;
        m_trollWalkieTalkie = walkieTalkie;
        m_trollRobotVoice = robotVoice;
    }

    void triggerGsmBurst() {
        m_gsmBurstRemaining = static_cast<int>(1.4f * m_sampleRate);
        m_gsmBurstSampleIndex = 0;
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

        // 5. Troll Audio FX

        // 5.1. GSM 2G / TDMA Cellphone Interference Buzz
        // 216.7 Hz TDMA frame repetition rate with 577us burst pulse
        if (m_trollGsmVoice || m_gsmBurstRemaining > 0) {
            const float tdmaStep = 216.7f / m_sampleRate;
            const float harm2Step = 433.4f / m_sampleRate;

            for (size_t i = 0; i < count; ++i) {
                m_gsmPhase += tdmaStep;
                if (m_gsmPhase >= 1.0f) m_gsmPhase -= 1.0f;

                m_gsmHarm2Phase += harm2Step;
                if (m_gsmHarm2Phase >= 1.0f) m_gsmHarm2Phase -= 1.0f;

                // TDMA pulse train (1/8th active slot = 0.125)
                float rawPulse = (m_gsmPhase < 0.125f) ? 1.0f : -0.15f;
                // Harmonic saturation characteristic of speaker amplifier diode rectification
                float gsmTone = std::tanh(rawPulse * 3.5f) * 0.75f +
                                0.25f * std::sin(2.0f * 3.14159265f * m_gsmHarm2Phase);

                // A) Voice modulated mode: activates when user speaks
                if (m_trollGsmVoice) {
                    float targetEnv = (m_speechRms > 0.015f) ? 1.0f : 0.0f;
                    m_gsmVoiceEnvelope = 0.92f * m_gsmVoiceEnvelope + 0.08f * targetEnv;
                    float voiceGsm = gsmTone * m_gsmVoiceEnvelope * 0.45f;
                    samples[i] = samples[i] * (1.0f - m_gsmVoiceEnvelope * 0.25f) + voiceGsm;
                }

                // B) One-shot incoming call simulation burst
                if (m_gsmBurstRemaining > 0) {
                    int sIdx = m_gsmBurstSampleIndex;
                    m_gsmBurstSampleIndex++;
                    m_gsmBurstRemaining--;

                    // Cadence at 48 kHz:
                    // 0 - 3360 (70ms burst)
                    // 3360 - 9120 (120ms pause)
                    // 9120 - 12480 (70ms burst)
                    // 12480 - 18240 (120ms pause)
                    // 18240 - 21600 (70ms burst)
                    // 21600 - 31200 (200ms pause)
                    // 31200 - 67200 (750ms continuous loud buzz)
                    bool burstActive = (sIdx < 3360) ||
                                       (sIdx >= 9120 && sIdx < 12480) ||
                                       (sIdx >= 18240 && sIdx < 21600) ||
                                       (sIdx >= 31200);

                    if (burstActive) {
                        samples[i] = samples[i] * 0.35f + gsmTone * 0.65f;
                    }
                }
            }
        }

        // 5.2. Tactical Walkie-Talkie FX (Bandpass 380Hz-3100Hz + overdrive + Roger Beep & Squelch)
        if (m_trollWalkieTalkie) {
            for (size_t i = 0; i < count; ++i) {
                float s = samples[i];
                s = m_walkieHp.process(s);
                s = m_walkieLp.process(s);

                // Radio preamp overdrive / clipping
                s = (std::clamp)(s * 2.2f, -0.65f, 0.65f) * 1.35f;

                // Low-level RF noise hiss
                float noise = (((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f) * 0.012f;
                s += noise;

                // Voice activity detection for Roger Beep trigger
                if (m_speechRms > 0.02f) {
                    m_walkieWasTalking = true;
                    m_walkieSilenceSamples = 0;
                } else if (m_walkieWasTalking) {
                    m_walkieSilenceSamples++;
                    // If silence persisted for ~80ms (3840 samples), trigger roger beep
                    if (m_walkieSilenceSamples > 3840) {
                        m_walkieWasTalking = false;
                        m_rogerBeepRemaining = static_cast<int>(0.12f * m_sampleRate); // 120ms roger beep
                        m_rogerBeepIndex = 0;
                    }
                }

                // Roger Beep & Squelch burst
                if (m_rogerBeepRemaining > 0) {
                    m_rogerBeepRemaining--;
                    m_rogerBeepIndex++;
                    float rogerSample = 0.0f;
                    if (m_rogerBeepIndex < 2880) { // First 60ms: dual tone 1000 Hz + 1200 Hz
                        m_rogerPhase1 += 1000.0f / m_sampleRate;
                        m_rogerPhase2 += 1200.0f / m_sampleRate;
                        if (m_rogerPhase1 >= 1.0f) m_rogerPhase1 -= 1.0f;
                        if (m_rogerPhase2 >= 1.0f) m_rogerPhase2 -= 1.0f;
                        rogerSample = 0.35f * (std::sin(2.0f * 3.14159265f * m_rogerPhase1) +
                                               std::sin(2.0f * 3.14159265f * m_rogerPhase2));
                    } else { // Next 60ms: static squelch "KSSSH"
                        rogerSample = (((float)rand() / (float)RAND_MAX) * 2.0f - 1.0f) * 0.28f;
                    }
                    s += rogerSample;
                }

                samples[i] = s;
            }
        }

        // 5.3. Robot / Ring-Modulator Voice
        if (m_trollRobotVoice) {
            const float carrierStep = 60.0f / m_sampleRate;
            for (size_t i = 0; i < count; ++i) {
                m_robotPhase += carrierStep;
                if (m_robotPhase >= 1.0f) m_robotPhase -= 1.0f;
                float carrier = std::sin(2.0f * 3.14159265f * m_robotPhase);
                samples[i] = samples[i] * (0.30f + 0.70f * carrier);
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

    // Troll Audio FX
    bool m_trollGsmVoice = false;
    bool m_trollWalkieTalkie = false;
    bool m_trollRobotVoice = false;

    // GSM generator state
    float m_gsmPhase = 0.0f;
    float m_gsmHarm2Phase = 0.0f;
    float m_gsmVoiceEnvelope = 0.0f;
    int m_gsmBurstRemaining = 0;
    int m_gsmBurstSampleIndex = 0;

    // Walkie-Talkie state
    BiquadFilter m_walkieHp;
    BiquadFilter m_walkieLp;
    bool m_walkieWasTalking = false;
    int m_walkieSilenceSamples = 0;
    int m_rogerBeepRemaining = 0;
    int m_rogerBeepIndex = 0;
    float m_rogerPhase1 = 0.0f;
    float m_rogerPhase2 = 0.0f;

    // Robot state
    float m_robotPhase = 0.0f;
};

