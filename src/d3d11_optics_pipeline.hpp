#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>

using Microsoft::WRL::ComPtr;

struct D3D11ShaderParams {
    float brightness;        // -1.0 .. 1.0 (из -100..100)
    float contrast;          // 0.5 .. 2.0  (из 50..200)
    float saturation;        // 0.0 .. 2.0  (из 0..200)
    float colorTemp;         // -0.5 .. 0.5 (из -50..50)

    int   lutPreset;         // 0..5
    float zoom;              // 1.0 .. 3.0
    float panX;              // -0.5 .. 0.5
    float panY;              // -0.5 .. 0.5

    int   trollPixelate;     // >1
    int   trollOverexposure; // 0 или 1
    int   trollBitcrush;     // 0 или 1
    int   trollGlitch;       // 0 или 1

    float timeSeconds;
    float canvasWidth;
    float canvasHeight;
    float isPortrait;

    int   bgEffectMode;      // 0: Off, 1: Studio Bokeh Blur, 2: Virtual Green Screen, 3: Dark Studio
    float bgBlurRadius;      // 1.0 .. 20.0
    float bgEdgeSoftness;    // 0.05 .. 0.50
    float bgThreshold;       // 0.2 .. 0.8
};

class D3D11OpticsPipeline {
public:
    static D3D11OpticsPipeline& instance();

    D3D11OpticsPipeline();
    ~D3D11OpticsPipeline();

    bool init(int width = 1280, int height = 720);
    void cleanup();
    bool isReady() const { return m_initialized; }

    // Аппаратная обработка Studio Optics и Troll FX на GPU:
    // Принимает входной BGRA кадр и параметры, возвращает обработанный BGRA
    bool processBgra(const uint32_t* srcBgra, uint32_t* dstBgra, const D3D11ShaderParams& params);

    // Аппаратная обработка + прямое GPU-преобразование в NV12 без CPU нагрузки
    bool processToNv12(const uint32_t* srcBgra, uint8_t* dstNv12, const D3D11ShaderParams& params);

    // Разделяемый хэндл DXGI для Zero-Copy драйвера
    HANDLE getSharedHandle() const { return m_sharedHandle; }

private:
    bool compileShaders();
    bool createResources();

    bool m_initialized = false;
    int m_width = 1280;
    int m_height = 720;

    ComPtr<ID3D11Device>            m_device;
    ComPtr<ID3D11DeviceContext>     m_context;

    // Входная динамическая текстура (BGRA)
    ComPtr<ID3D11Texture2D>          m_inputTex;
    ComPtr<ID3D11ShaderResourceView> m_inputSRV;

    // Промежуточная текстура рендера Studio Optics (BGRA)
    ComPtr<ID3D11Texture2D>          m_processedTex;
    ComPtr<ID3D11RenderTargetView>   m_processedRTV;
    ComPtr<ID3D11ShaderResourceView> m_processedSRV;
    HANDLE                          m_sharedHandle = nullptr;

    // Staging текстура для быстрого чтения BGRA обратно на CPU
    ComPtr<ID3D11Texture2D>          m_stagingBgraTex;

    // Текстуры для GPU NV12 конвертации (Y и UV плоскости)
    ComPtr<ID3D11Texture2D>          m_nv12YTex;
    ComPtr<ID3D11RenderTargetView>   m_nv12YRTV;
    ComPtr<ID3D11Texture2D>          m_nv12YStagingTex;

    ComPtr<ID3D11Texture2D>          m_nv12UVTex;
    ComPtr<ID3D11RenderTargetView>   m_nv12UVRTV;
    ComPtr<ID3D11Texture2D>          m_nv12UVStagingTex;

    // Константный буфер параметров шейдера
    ComPtr<ID3D11Buffer>            m_constantBuffer;

    // Шейдеры
    ComPtr<ID3D11VertexShader>      m_vertexShader;
    ComPtr<ID3D11PixelShader>       m_opticsPixelShader;
    ComPtr<ID3D11PixelShader>       m_nv12YPixelShader;
    ComPtr<ID3D11PixelShader>       m_nv12UVPixelShader;

    // Сэмплеры
    ComPtr<ID3D11SamplerState>      m_linearSampler;
    ComPtr<ID3D11SamplerState>      m_pointSampler;

    // Растеризатор
    ComPtr<ID3D11RasterizerState>   m_rasterizerState;
};
