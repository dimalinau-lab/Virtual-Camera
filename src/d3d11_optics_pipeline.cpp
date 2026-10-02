#include "d3d11_optics_pipeline.hpp"
#include <iostream>
#include <algorithm>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

static const char* g_hlslShaderSource = R"(
struct VSOutput {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOutput VSMain(uint id : SV_VertexID) {
    VSOutput output;
    output.uv = float2((id << 1) & 2, id & 2);
    output.pos = float4(output.uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}

cbuffer Params : register(b0) {
    float g_brightness;
    float g_contrast;
    float g_saturation;
    float g_colorTemp;
    int   g_lutPreset;
    float g_zoom;
    float g_panX;
    float g_panY;
    int   g_trollPixelate;
    int   g_trollOverexposure;
    int   g_trollBitcrush;
    int   g_trollGlitch;
    float g_timeSeconds;
    float g_canvasWidth;
    float g_canvasHeight;
    float g_isPortrait;
    int   g_bgEffectMode;
    float g_bgBlurRadius;
    float g_bgEdgeSoftness;
    float g_bgThreshold;
};

Texture2D g_inputTexture : register(t0);
SamplerState g_samplerLinear : register(s0);
SamplerState g_samplerPoint  : register(s1);

float4 PS_Optics(VSOutput input) : SV_TARGET {
    float2 uv = input.uv;

    // 1. Digital Zoom & Framing
    if (g_zoom > 1.01f) {
        float2 center = float2(0.5f, 0.5f) + float2(g_panX, g_panY);
        uv = (uv - center) / g_zoom + float2(0.5f, 0.5f);
        if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f) {
            return float4(0.0f, 0.0f, 0.0f, 1.0f);
        }
    }

    // 2. Troll FX: Pixelate
    if (g_trollPixelate > 1) {
        float2 blk = float2((float)g_trollPixelate, (float)g_trollPixelate) / float2(g_canvasWidth, g_canvasHeight);
        uv = floor(uv / blk) * blk + blk * 0.5f;
    }

    // 3. Troll FX: VHS Glitch Jitter
    if (g_trollGlitch > 0) {
        float scanline = sin(uv.y * 120.0f + g_timeSeconds * 12.0f);
        if (abs(scanline) > 0.82f) {
            float shift = sin(uv.y * 50.0f + g_timeSeconds * 25.0f) * 0.035f;
            uv.x = frac(uv.x + shift);
        }
    }

    float4 col = g_inputTexture.Sample(g_samplerLinear, uv);

    // 4. White Balance / Color Temperature
    if (g_colorTemp != 0.0f) {
        col.r = saturate(col.r + g_colorTemp * 0.20f);
        col.b = saturate(col.b - g_colorTemp * 0.20f);
    }

    // 5. Brightness & Contrast
    if (g_brightness != 0.0f || g_contrast != 1.0f) {
        col.rgb = saturate((col.rgb - 0.5f) * g_contrast + 0.5f + g_brightness);
    }

    // Rec. 601 Luma
    float luma = dot(col.rgb, float3(0.299f, 0.587f, 0.114f));

    // 6. 3D LUT Presets
    if (g_lutPreset == 1) {
        // Teal & Orange
        if (luma > 0.5f) {
            float factor = (luma - 0.5f) * 2.0f;
            col.r = saturate(col.r + factor * 0.137f);
            col.g = saturate(col.g + factor * 0.039f);
            col.b = saturate(col.b - factor * 0.098f);
        } else {
            float factor = (0.5f - luma) * 2.0f;
            col.b = saturate(col.b + factor * 0.137f);
            col.g = saturate(col.g + factor * 0.059f);
            col.r = saturate(col.r - factor * 0.098f);
        }
    }
    else if (g_lutPreset == 2) {
        // Warm Cinema
        col.r = saturate(col.r * 1.08f + 0.03f);
        col.g = saturate(col.g * 1.02f);
        col.b = saturate(col.b * 0.92f);
    }
    else if (g_lutPreset == 3) {
        // Noir B&W
        col.rgb = luma.xxx;
    }
    else if (g_lutPreset == 4) {
        // Cyber Neon
        col.r = saturate(col.r * 1.15f);
        col.g = saturate(col.g * 0.90f);
        col.b = saturate(col.b * 1.25f);
    }
    else if (g_lutPreset == 5) {
        // Clean Studio
        col.rgb = saturate((col.rgb - 0.5f) * 1.08f + 0.5f);
        col.r = saturate(col.r * 1.02f);
    }

    // 7. Saturation
    if (g_lutPreset != 3 && g_saturation != 1.0f) {
        col.rgb = saturate(lerp(luma.xxx, col.rgb, g_saturation));
    }

    // 8. Troll FX: Nuclear Flashbang
    if (g_trollOverexposure > 0) {
        if (luma > 0.45f) {
            col.rgb = saturate(col.rgb * 3.0f);
        } else {
            col.rgb = col.rgb * col.rgb;
        }
    }

    // 9. Troll FX: Bitcrush
    if (g_trollBitcrush > 0) {
        col.rgb = floor(col.rgb * 8.0f) / 8.0f;
    }

    // 10. AI Neural Background Effects (Studio Bokeh Blur, Virtual Green Screen, Dark Studio)
    if (g_bgEffectMode > 0) {
        float2 personCenter = float2(0.50f, 0.46f);
        float2 personHalfSize = (g_isPortrait > 0.5f) ? float2(0.12f, 0.48f) : float2(0.34f, 0.52f);
        float2 distVec = (uv - personCenter) / personHalfSize;
        float radialDist = length(distVec);

        float skinTone = col.r / max(col.g + col.b, 0.001f);
        float isSkin = smoothstep(0.48f, 0.65f, skinTone);

        float personConf = 1.0f - smoothstep(g_bgThreshold - g_bgEdgeSoftness, g_bgThreshold + g_bgEdgeSoftness, radialDist - isSkin * 0.12f);

        // В портретном режиме сохраняем черные боковые полосы чистыми
        if (g_isPortrait > 0.5f && (uv.x < 0.34f || uv.x > 0.66f)) {
            personConf = 1.0f;
        }

        if (g_bgEffectMode == 1) {
            // Mode 1: Studio Bokeh Blur (Многонаправленный фильтр боке)
            float2 texel = float2(g_bgBlurRadius / g_canvasWidth, g_bgBlurRadius / g_canvasHeight);
            float3 blurred = col.rgb * 0.20f;
            blurred += g_inputTexture.Sample(g_samplerLinear, uv + float2( 1.4f,  0.0f) * texel).rgb * 0.12f;
            blurred += g_inputTexture.Sample(g_samplerLinear, uv + float2(-1.4f,  0.0f) * texel).rgb * 0.12f;
            blurred += g_inputTexture.Sample(g_samplerLinear, uv + float2( 0.0f,  1.4f) * texel).rgb * 0.12f;
            blurred += g_inputTexture.Sample(g_samplerLinear, uv + float2( 0.0f, -1.4f) * texel).rgb * 0.12f;
            blurred += g_inputTexture.Sample(g_samplerLinear, uv + float2( 1.1f,  1.1f) * texel).rgb * 0.08f;
            blurred += g_inputTexture.Sample(g_samplerLinear, uv + float2(-1.1f,  1.1f) * texel).rgb * 0.08f;
            blurred += g_inputTexture.Sample(g_samplerLinear, uv + float2( 1.1f, -1.1f) * texel).rgb * 0.08f;
            blurred += g_inputTexture.Sample(g_samplerLinear, uv + float2(-1.1f, -1.1f) * texel).rgb * 0.08f;
            blurred += g_inputTexture.Sample(g_samplerLinear, uv + float2( 2.4f,  0.0f) * texel).rgb * 0.04f;
            blurred += g_inputTexture.Sample(g_samplerLinear, uv + float2(-2.4f,  0.0f) * texel).rgb * 0.04f;
            blurred += g_inputTexture.Sample(g_samplerLinear, uv + float2( 0.0f,  2.4f) * texel).rgb * 0.04f;
            blurred += g_inputTexture.Sample(g_samplerLinear, uv + float2( 0.0f, -2.4f) * texel).rgb * 0.04f;
            col.rgb = lerp(blurred, col.rgb, saturate(personConf));
        }
        else if (g_bgEffectMode == 2) {
            // Mode 2: Virtual Green Screen (Чистый хромакей #00FF00)
            float3 greenScreen = float3(0.0f, 1.0f, 0.0f);
            col.rgb = lerp(greenScreen, col.rgb, saturate(personConf));
        }
        else if (g_bgEffectMode == 3) {
            // Mode 3: Dark Studio Backdrop
            float3 darkBackdrop = float3(0.06f, 0.07f, 0.09f);
            col.rgb = lerp(darkBackdrop, col.rgb, saturate(personConf));
        }
    }

    return float4(col.rgb, 1.0f);
}

Texture2D g_processedTex : register(t0);

// Hardware NV12 Y Plane Pass (Rec.601)
float4 PS_NV12_Y(VSOutput input) : SV_TARGET {
    float3 col = g_processedTex.Sample(g_samplerPoint, input.uv).rgb;
    float y = 16.0f / 255.0f + (65.738f * col.r + 129.057f * col.g + 25.064f * col.b) / 256.0f;
    return float4(saturate(y), 0.0f, 0.0f, 1.0f);
}

// Hardware NV12 UV Interleaved Plane Pass (Rec.601)
float4 PS_NV12_UV(VSOutput input) : SV_TARGET {
    float3 col = g_processedTex.Sample(g_samplerLinear, input.uv).rgb;
    float u = 128.0f / 255.0f + (-37.945f * col.r - 74.494f * col.g + 112.439f * col.b) / 256.0f;
    float v = 128.0f / 255.0f + (112.439f * col.r - 94.154f * col.g - 18.285f * col.b) / 256.0f;
    return float4(saturate(u), saturate(v), 0.0f, 1.0f);
}
)";

D3D11OpticsPipeline& D3D11OpticsPipeline::instance() {
    static D3D11OpticsPipeline s_instance;
    return s_instance;
}

D3D11OpticsPipeline::D3D11OpticsPipeline() {}

D3D11OpticsPipeline::~D3D11OpticsPipeline() {
    cleanup();
}

void D3D11OpticsPipeline::cleanup() {
    m_initialized = false;
    m_rasterizerState.Reset();
    m_linearSampler.Reset();
    m_pointSampler.Reset();
    m_constantBuffer.Reset();
    m_vertexShader.Reset();
    m_opticsPixelShader.Reset();
    m_nv12YPixelShader.Reset();
    m_nv12UVPixelShader.Reset();

    m_nv12UVStagingTex.Reset();
    m_nv12UVRTV.Reset();
    m_nv12UVTex.Reset();

    m_nv12YStagingTex.Reset();
    m_nv12YRTV.Reset();
    m_nv12YTex.Reset();

    m_stagingBgraTex.Reset();
    m_processedSRV.Reset();
    m_processedRTV.Reset();
    m_processedTex.Reset();

    m_inputSRV.Reset();
    m_inputTex.Reset();

    m_context.Reset();
    m_device.Reset();
    m_sharedHandle = nullptr;
}

bool D3D11OpticsPipeline::init(int width, int height) {
    if (m_initialized && m_width == width && m_height == height) {
        return true;
    }

    cleanup();
    m_width = width;
    m_height = height;

    UINT createDeviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
    createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };
    D3D_FEATURE_LEVEL featureLevel;

    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        createDeviceFlags,
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        &m_device,
        &featureLevel,
        &m_context
    );

    if (FAILED(hr)) {
        // Fallback на WARP software rasterizer если hardware недоступен
        hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_WARP,
            nullptr,
            createDeviceFlags,
            featureLevels,
            ARRAYSIZE(featureLevels),
            D3D11_SDK_VERSION,
            &m_device,
            &featureLevel,
            &m_context
        );
        if (FAILED(hr)) {
            std::cerr << "[D3D11 PIPELINE] Ошибка создания D3D11Device: 0x" << std::hex << hr << std::dec << "\n";
            return false;
        }
    }

    if (!compileShaders()) {
        cleanup();
        return false;
    }

    if (!createResources()) {
        cleanup();
        return false;
    }

    m_initialized = true;
    std::cout << "[D3D11 PIPELINE] Direct3D 11 GPU шейдерный конвейер успешно запущен ("
              << m_width << "x" << m_height << " HLSL Shader Model 5.0)\n";
    return true;
}

bool D3D11OpticsPipeline::compileShaders() {
    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;

    ComPtr<ID3DBlob> vsBlob, psOpticsBlob, psNV12YBlob, psNV12UVBlob, errorBlob;

    // 1. Vertex Shader
    HRESULT hr = D3DCompile(g_hlslShaderSource, strlen(g_hlslShaderSource), "D3D11OpticsHLSL",
                            nullptr, nullptr, "VSMain", "vs_5_0", compileFlags, 0, &vsBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) std::cerr << "[D3D11 HLSL] VS error: " << (char*)errorBlob->GetBufferPointer() << "\n";
        return false;
    }
    hr = m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_vertexShader);
    if (FAILED(hr)) return false;

    // 2. Optics & Troll FX Pixel Shader
    hr = D3DCompile(g_hlslShaderSource, strlen(g_hlslShaderSource), "D3D11OpticsHLSL",
                    nullptr, nullptr, "PS_Optics", "ps_5_0", compileFlags, 0, &psOpticsBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) std::cerr << "[D3D11 HLSL] PS_Optics error: " << (char*)errorBlob->GetBufferPointer() << "\n";
        return false;
    }
    hr = m_device->CreatePixelShader(psOpticsBlob->GetBufferPointer(), psOpticsBlob->GetBufferSize(), nullptr, &m_opticsPixelShader);
    if (FAILED(hr)) return false;

    // 3. NV12 Y Plane Pixel Shader
    hr = D3DCompile(g_hlslShaderSource, strlen(g_hlslShaderSource), "D3D11OpticsHLSL",
                    nullptr, nullptr, "PS_NV12_Y", "ps_5_0", compileFlags, 0, &psNV12YBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) std::cerr << "[D3D11 HLSL] PS_NV12_Y error: " << (char*)errorBlob->GetBufferPointer() << "\n";
        return false;
    }
    hr = m_device->CreatePixelShader(psNV12YBlob->GetBufferPointer(), psNV12YBlob->GetBufferSize(), nullptr, &m_nv12YPixelShader);
    if (FAILED(hr)) return false;

    // 4. NV12 UV Plane Pixel Shader
    hr = D3DCompile(g_hlslShaderSource, strlen(g_hlslShaderSource), "D3D11OpticsHLSL",
                    nullptr, nullptr, "PS_NV12_UV", "ps_5_0", compileFlags, 0, &psNV12UVBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) std::cerr << "[D3D11 HLSL] PS_NV12_UV error: " << (char*)errorBlob->GetBufferPointer() << "\n";
        return false;
    }
    hr = m_device->CreatePixelShader(psNV12UVBlob->GetBufferPointer(), psNV12UVBlob->GetBufferSize(), nullptr, &m_nv12UVPixelShader);
    if (FAILED(hr)) return false;

    return true;
}

bool D3D11OpticsPipeline::createResources() {
    // 1. Входная BGRA динамическая текстура
    D3D11_TEXTURE2D_DESC texDesc{};
    texDesc.Width = m_width;
    texDesc.Height = m_height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DYNAMIC;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    HRESULT hr = m_device->CreateTexture2D(&texDesc, nullptr, &m_inputTex);
    if (FAILED(hr)) return false;

    hr = m_device->CreateShaderResourceView(m_inputTex.Get(), nullptr, &m_inputSRV);
    if (FAILED(hr)) return false;

    // 2. Промежуточная текстура Studio Optics (с поддержкой Shared Handle)
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    texDesc.CPUAccessFlags = 0;
    texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

    hr = m_device->CreateTexture2D(&texDesc, nullptr, &m_processedTex);
    if (FAILED(hr)) return false;

    hr = m_device->CreateRenderTargetView(m_processedTex.Get(), nullptr, &m_processedRTV);
    if (FAILED(hr)) return false;

    hr = m_device->CreateShaderResourceView(m_processedTex.Get(), nullptr, &m_processedSRV);
    if (FAILED(hr)) return false;

    // Получаем DXGI Shared Handle
    ComPtr<IDXGIResource> dxgiResource;
    if (SUCCEEDED(m_processedTex.As(&dxgiResource))) {
        dxgiResource->GetSharedHandle(&m_sharedHandle);
    }

    // 3. Staging BGRA текстура для чтения на CPU
    texDesc.Usage = D3D11_USAGE_STAGING;
    texDesc.BindFlags = 0;
    texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    texDesc.MiscFlags = 0;

    hr = m_device->CreateTexture2D(&texDesc, nullptr, &m_stagingBgraTex);
    if (FAILED(hr)) return false;

    // 4. Текстуры для Y плоскости NV12 (1280x720, R8_UNORM)
    texDesc.Format = DXGI_FORMAT_R8_UNORM;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
    texDesc.CPUAccessFlags = 0;
    hr = m_device->CreateTexture2D(&texDesc, nullptr, &m_nv12YTex);
    if (FAILED(hr)) return false;

    hr = m_device->CreateRenderTargetView(m_nv12YTex.Get(), nullptr, &m_nv12YRTV);
    if (FAILED(hr)) return false;

    texDesc.Usage = D3D11_USAGE_STAGING;
    texDesc.BindFlags = 0;
    texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = m_device->CreateTexture2D(&texDesc, nullptr, &m_nv12YStagingTex);
    if (FAILED(hr)) return false;

    // 5. Текстуры для UV плоскости NV12 (640x360, R8G8_UNORM)
    texDesc.Width = m_width / 2;
    texDesc.Height = m_height / 2;
    texDesc.Format = DXGI_FORMAT_R8G8_UNORM;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
    texDesc.CPUAccessFlags = 0;
    hr = m_device->CreateTexture2D(&texDesc, nullptr, &m_nv12UVTex);
    if (FAILED(hr)) return false;

    hr = m_device->CreateRenderTargetView(m_nv12UVTex.Get(), nullptr, &m_nv12UVRTV);
    if (FAILED(hr)) return false;

    texDesc.Usage = D3D11_USAGE_STAGING;
    texDesc.BindFlags = 0;
    texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = m_device->CreateTexture2D(&texDesc, nullptr, &m_nv12UVStagingTex);
    if (FAILED(hr)) return false;

    // 6. Константный буфер параметров
    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = sizeof(D3D11ShaderParams);
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = m_device->CreateBuffer(&cbDesc, nullptr, &m_constantBuffer);
    if (FAILED(hr)) return false;

    // 7. Сэмплеры
    D3D11_SAMPLER_DESC sampDesc{};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    hr = m_device->CreateSamplerState(&sampDesc, &m_linearSampler);
    if (FAILED(hr)) return false;

    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    hr = m_device->CreateSamplerState(&sampDesc, &m_pointSampler);
    if (FAILED(hr)) return false;

    // 8. Растеризатор
    D3D11_RASTERIZER_DESC rastDesc{};
    rastDesc.FillMode = D3D11_FILL_SOLID;
    rastDesc.CullMode = D3D11_CULL_NONE;
    hr = m_device->CreateRasterizerState(&rastDesc, &m_rasterizerState);
    if (FAILED(hr)) return false;

    return true;
}

bool D3D11OpticsPipeline::processBgra(const uint32_t* srcBgra, uint32_t* dstBgra, const D3D11ShaderParams& params) {
    if (!m_initialized || !srcBgra || !dstBgra) return false;

    // 1. Загрузка входного кадра на GPU через Dynamic Texture Map (~0.03 мс)
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = m_context->Map(m_inputTex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return false;

    const size_t rowBytes = (size_t)m_width * 4;
    if (mapped.RowPitch == rowBytes) {
        memcpy(mapped.pData, srcBgra, (size_t)m_width * m_height * 4);
    } else {
        const uint8_t* srcBytes = reinterpret_cast<const uint8_t*>(srcBgra);
        uint8_t* dstBytes = reinterpret_cast<uint8_t*>(mapped.pData);
        for (int y = 0; y < m_height; ++y) {
            memcpy(dstBytes + y * mapped.RowPitch, srcBytes + y * rowBytes, rowBytes);
        }
    }
    m_context->Unmap(m_inputTex.Get(), 0);

    // 2. Обновление параметров в константном буфере
    hr = m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        memcpy(mapped.pData, &params, sizeof(D3D11ShaderParams));
        m_context->Unmap(m_constantBuffer.Get(), 0);
    }

    // 3. Установка конвейера шейдеров Studio Optics
    D3D11_VIEWPORT vp{};
    vp.Width = (float)m_width;
    vp.Height = (float)m_height;
    vp.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &vp);
    m_context->RSSetState(m_rasterizerState.Get());

    ID3D11RenderTargetView* rtvList[1] = { m_processedRTV.Get() };
    m_context->OMSetRenderTargets(1, rtvList, nullptr);

    ID3D11Buffer* cbList[1] = { m_constantBuffer.Get() };
    m_context->PSSetConstantBuffers(0, 1, cbList);

    ID3D11ShaderResourceView* srvList[1] = { m_inputSRV.Get() };
    m_context->PSSetShaderResources(0, 1, srvList);

    ID3D11SamplerState* samplers[2] = { m_linearSampler.Get(), m_pointSampler.Get() };
    m_context->PSSetSamplers(0, 2, samplers);

    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
    m_context->PSSetShader(m_opticsPixelShader.Get(), nullptr, 0);

    // 4. Отрисовка полноэкранного шейдерного квада (HLSL Optics + Troll FX)
    m_context->Draw(3, 0);

    // Отвязываем Render Target
    rtvList[0] = nullptr;
    m_context->OMSetRenderTargets(1, rtvList, nullptr);

    // 5. Копирование в Staging текстуру для чтения на CPU
    m_context->CopyResource(m_stagingBgraTex.Get(), m_processedTex.Get());

    hr = m_context->Map(m_stagingBgraTex.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return false;

    if (mapped.RowPitch == rowBytes) {
        memcpy(dstBgra, mapped.pData, (size_t)m_width * m_height * 4);
    } else {
        const uint8_t* srcBytes = reinterpret_cast<const uint8_t*>(mapped.pData);
        uint8_t* dstBytes = reinterpret_cast<uint8_t*>(dstBgra);
        for (int y = 0; y < m_height; ++y) {
            memcpy(dstBytes + y * rowBytes, srcBytes + y * mapped.RowPitch, rowBytes);
        }
    }
    m_context->Unmap(m_stagingBgraTex.Get(), 0);

    return true;
}

bool D3D11OpticsPipeline::processToNv12(const uint32_t* srcBgra, uint8_t* dstNv12, const D3D11ShaderParams& params) {
    if (!m_initialized || !srcBgra || !dstNv12) return false;

    // 1. Загрузка входного кадра на GPU
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = m_context->Map(m_inputTex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return false;

    const size_t rowBytes = (size_t)m_width * 4;
    if (mapped.RowPitch == rowBytes) {
        memcpy(mapped.pData, srcBgra, (size_t)m_width * m_height * 4);
    } else {
        const uint8_t* srcBytes = reinterpret_cast<const uint8_t*>(srcBgra);
        uint8_t* dstBytes = reinterpret_cast<uint8_t*>(mapped.pData);
        for (int y = 0; y < m_height; ++y) {
            memcpy(dstBytes + y * mapped.RowPitch, srcBytes + y * rowBytes, rowBytes);
        }
    }
    m_context->Unmap(m_inputTex.Get(), 0);

    // 2. Обновление параметров константного буфера
    hr = m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        memcpy(mapped.pData, &params, sizeof(D3D11ShaderParams));
        m_context->Unmap(m_constantBuffer.Get(), 0);
    }

    // 3. Пасс 1: Studio Optics & Troll FX -> m_processedTex
    D3D11_VIEWPORT vp{};
    vp.Width = (float)m_width;
    vp.Height = (float)m_height;
    vp.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &vp);
    m_context->RSSetState(m_rasterizerState.Get());

    ID3D11RenderTargetView* rtvList[1] = { m_processedRTV.Get() };
    m_context->OMSetRenderTargets(1, rtvList, nullptr);

    ID3D11Buffer* cbList[1] = { m_constantBuffer.Get() };
    m_context->PSSetConstantBuffers(0, 1, cbList);

    ID3D11ShaderResourceView* srvList[1] = { m_inputSRV.Get() };
    m_context->PSSetShaderResources(0, 1, srvList);

    ID3D11SamplerState* samplers[2] = { m_linearSampler.Get(), m_pointSampler.Get() };
    m_context->PSSetSamplers(0, 2, samplers);

    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
    m_context->PSSetShader(m_opticsPixelShader.Get(), nullptr, 0);

    m_context->Draw(3, 0);

    // 4. Пасс 2: Аппаратный рендер Y-плоскости (1280x720, R8_UNORM)
    rtvList[0] = m_nv12YRTV.Get();
    m_context->OMSetRenderTargets(1, rtvList, nullptr);

    srvList[0] = m_processedSRV.Get();
    m_context->PSSetShaderResources(0, 1, srvList);
    m_context->PSSetShader(m_nv12YPixelShader.Get(), nullptr, 0);

    m_context->Draw(3, 0);

    // 5. Пасс 3: Аппаратный рендер UV-плоскости (640x360, R8G8_UNORM)
    vp.Width = (float)(m_width / 2);
    vp.Height = (float)(m_height / 2);
    m_context->RSSetViewports(1, &vp);

    rtvList[0] = m_nv12UVRTV.Get();
    m_context->OMSetRenderTargets(1, rtvList, nullptr);
    m_context->PSSetShader(m_nv12UVPixelShader.Get(), nullptr, 0);

    m_context->Draw(3, 0);

    // Сброс привязок
    rtvList[0] = nullptr;
    m_context->OMSetRenderTargets(1, rtvList, nullptr);
    srvList[0] = nullptr;
    m_context->PSSetShaderResources(0, 1, srvList);

    // 6. Копирование Y и UV в staging текстуры
    m_context->CopyResource(m_nv12YStagingTex.Get(), m_nv12YTex.Get());
    m_context->CopyResource(m_nv12UVStagingTex.Get(), m_nv12UVTex.Get());

    // 7. Чтение Y плоскости в dstNv12
    hr = m_context->Map(m_nv12YStagingTex.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return false;

    uint8_t* dstY = dstNv12;
    if (mapped.RowPitch == (UINT)m_width) {
        memcpy(dstY, mapped.pData, (size_t)m_width * m_height);
    } else {
        const uint8_t* srcY = reinterpret_cast<const uint8_t*>(mapped.pData);
        for (int y = 0; y < m_height; ++y) {
            memcpy(dstY + y * m_width, srcY + y * mapped.RowPitch, m_width);
        }
    }
    m_context->Unmap(m_nv12YStagingTex.Get(), 0);

    // 8. Чтение UV плоскости в dstNv12 + width*height
    hr = m_context->Map(m_nv12UVStagingTex.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) return false;

    uint8_t* dstUV = dstNv12 + ((size_t)m_width * m_height);
    const size_t uvRowBytes = (size_t)m_width; // 640 * 2 байта (R8G8) = 1280 байт
    if (mapped.RowPitch == (UINT)uvRowBytes) {
        memcpy(dstUV, mapped.pData, (size_t)m_width * m_height / 2);
    } else {
        const uint8_t* srcUV = reinterpret_cast<const uint8_t*>(mapped.pData);
        for (int y = 0; y < m_height / 2; ++y) {
            memcpy(dstUV + y * uvRowBytes, srcUV + y * mapped.RowPitch, uvRowBytes);
        }
    }
    m_context->Unmap(m_nv12UVStagingTex.Get(), 0);

    return true;
}
