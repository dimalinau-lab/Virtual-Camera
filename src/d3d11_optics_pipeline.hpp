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

    // Portrait rotation & Aspect Ratio helpers
    int   portraitMirror;    // 0 или 1 (g_mirrorEnabled)
    int   portraitFlip180;   // 0 или 1 (g_flip180)
    int   portraitIsFront;   // 0 или 1 (g_isFrontCamera)
    int   aspectRatioMode;   // 0: 9:16 (Phone Portrait), 1: 4:3 (Classic), 2: 16:9 (Wide)
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

    // Аппаратная обработка + прямое GPU-преобразование в NV12 без CPU нагрузки.
    // Использует double-buffered staging для устранения GPU stall.
    bool processToNv12(const uint32_t* srcBgra, uint8_t* dstNv12, const D3D11ShaderParams& params);

    // Прямой ультра-быстрый конвейер NV12 -> D3D11 (Y: R8, UV: R8G8) -> Studio Optics -> VCam NV12 (< 1 мс!)
    // Полностью устраняет sws_scale, конверсию в BGRA и CPU overhead
    bool processNv12ToNv12(const uint8_t* srcNv12, int inW, int inH, uint8_t* dstNv12, const D3D11ShaderParams& params);

    // Количество кадров, пропущенных из-за того, что staging ещё занят GPU (диагностика).
    int getStallSkipCount() const { return m_stallSkipCount; }
    void resetStallSkipCount() { m_stallSkipCount = 0; }

    // Разделяемый хэндл DXGI для Zero-Copy драйвера
    HANDLE getSharedHandle() const { return m_sharedHandle; }

private:
    bool compileShaders();
    bool createResources();
    bool ensureNv12InputResources(int inW, int inH);

    bool m_initialized = false;
    int m_width = 1280;
    int m_height = 720;

    ComPtr<ID3D11Device>            m_device;
    ComPtr<ID3D11DeviceContext>     m_context;

    // Входная динамическая текстура (BGRA)
    ComPtr<ID3D11Texture2D>          m_inputTex;
    ComPtr<ID3D11ShaderResourceView> m_inputSRV;

    // Входные динамические текстуры прямого NV12 (Y: R8_UNORM, UV: R8G8_UNORM)
    ComPtr<ID3D11Texture2D>          m_inputYTex;
    ComPtr<ID3D11ShaderResourceView> m_inputYSRV;
    ComPtr<ID3D11Texture2D>          m_inputUVTex;
    ComPtr<ID3D11ShaderResourceView> m_inputUVSRV;
    int                              m_inputTexW = 0;
    int                              m_inputTexH = 0;

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
    // Triple-buffered staging (3 текстуры) для 100% устранения GPU→CPU stall
    ComPtr<ID3D11Texture2D>          m_nv12YStagingTex[3];

    ComPtr<ID3D11Texture2D>          m_nv12UVTex;
    ComPtr<ID3D11RenderTargetView>   m_nv12UVRTV;
    ComPtr<ID3D11Texture2D>          m_nv12UVStagingTex[3];

    int                              m_stagingFrameIndex = 0;  // Текущий write-индекс staging ring
    int                              m_stallSkipCount    = 0;  // Счётчик DO_NOT_WAIT пропусков

    // Константный буфер параметров шейдера
    ComPtr<ID3D11Buffer>            m_constantBuffer;

    // Шейдеры
    ComPtr<ID3D11VertexShader>      m_vertexShader;
    ComPtr<ID3D11PixelShader>       m_opticsPixelShader;
    ComPtr<ID3D11PixelShader>       m_opticsNv12PixelShader;
    ComPtr<ID3D11PixelShader>       m_nv12YPixelShader;
    ComPtr<ID3D11PixelShader>       m_nv12UVPixelShader;

    // Сэмплеры
    ComPtr<ID3D11SamplerState>      m_linearSampler;
    ComPtr<ID3D11SamplerState>      m_pointSampler;

    // Растеризатор
    ComPtr<ID3D11RasterizerState>   m_rasterizerState;
};
