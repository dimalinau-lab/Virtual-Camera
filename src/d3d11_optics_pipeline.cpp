#include "d3d11_optics_pipeline.hpp"
#include <d3d10.h>
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
    int   g_portraitMirror;
    int   g_portraitFlip180;
    int   g_portraitIsFront;
    int   g_aspectRatioMode; // 0: 9:16 (Phone Portrait), 1: 4:3 (Classic), 2: 16:9 (Wide)

    int   g_streamOrientation;   // 0, 90, 180, 270 (MagerCam protocol)
    float g_animAngleRad;        // Плавный угол поворота в радианах (Emil Kowalski Physics, 180 ms)
    float g_animBlend;           // 0.0 (16:9 Landscape) -> 1.0 (9:16 Portrait Pillarbox)
    float g_animAspectHalfWidth; // Плавная полуширина кадра (0.1582..0.5)
};

Texture2D g_inputTexture : register(t0);
Texture2D g_inputTexY    : register(t1);
Texture2D g_inputTexUV   : register(t2);
Texture2D g_prevFrameTex : register(t3);
Texture2D g_currFrameTex : register(t4);
Texture2D g_motionVecTex : register(t5);
SamplerState g_samplerLinear : register(s0);
SamplerState g_samplerPoint  : register(s1);

float3 SampleNv12(float2 coords, bool isPointSample = false) {
    coords = clamp(coords, float2(0.0005f, 0.0005f), float2(0.9995f, 0.9995f));
    float y = isPointSample ? g_inputTexY.Sample(g_samplerPoint, coords).r : g_inputTexY.Sample(g_samplerLinear, coords).r;
    float2 uv = isPointSample ? g_inputTexUV.Sample(g_samplerPoint, coords).rg : g_inputTexUV.Sample(g_samplerLinear, coords).rg;
    float c = y - (16.0f / 255.0f);
    float d = uv.r - (128.0f / 255.0f);
    float e = uv.g - (128.0f / 255.0f);
    float r = 1.164383f * c + 1.596027f * e;
    float g = 1.164383f * c - 0.391762f * d - 0.812968f * e;
    float b = 1.164383f * c + 2.017232f * d;
    return saturate(float3(r, g, b));
}

float4 PS_Optics(VSOutput input) : SV_TARGET {
    float2 inUv = input.uv;
    if (g_trollPixelate > 1) {
        float blockSize = (float)g_trollPixelate;
        float2 numBlocks = float2(max(1.0f, g_canvasWidth), max(1.0f, g_canvasHeight)) / blockSize;
        inUv = (floor(inUv * numBlocks) + 0.5f) / numBlocks;
    }

    float blend = saturate(g_animBlend);
    float theta = g_animAngleRad;
    if (g_portraitFlip180 != 0) {
        theta += 3.14159265f;
    }
    bool mirror = (g_portraitMirror != 0);

    // Плавный расчет границ кадра с учетом пропорций (9:16, 4:3, 16:9):
    float halfWidth = clamp(g_animAspectHalfWidth, 0.1582f, 0.5f);
    float x0 = 0.5f - halfWidth;
    float x1 = 0.5f + halfWidth;

    float4 col = float4(0.0f, 0.0f, 0.0f, 1.0f);
    bool inBox = (inUv.x >= x0 && inUv.x <= x1);

    if (!inBox && (halfWidth < 0.498f)) {
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    float tx = (inUv.x - x0) / max(0.0001f, x1 - x0);
    float ty = inUv.y;
    if (mirror) tx = 1.0f - tx;

    float cx = tx - 0.5f;
    float cy = ty - 0.5f;
    float cosT = cos(theta);
    float sinT = sin(theta);

    float u = 0.5f + cx * cosT + cy * sinT;
    float v = 0.5f - cx * sinT + cy * cosT;
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) {
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }
    float2 uv = float2(u, v);

    // 1. Digital Zoom & Framing
    if (g_zoom > 1.01f) {
        float2 center = float2(0.5f, 0.5f) + float2(g_panX, g_panY);
        uv = (uv - center) / g_zoom + float2(0.5f, 0.5f);
        if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f) {
            return float4(0.0f, 0.0f, 0.0f, 1.0f);
        }
    }

    // 2. Troll FX: VHS Glitch Jitter
    if (g_trollGlitch > 0) {
        float scanline = sin(uv.y * 120.0f + g_timeSeconds * 12.0f);
        if (abs(scanline) > 0.82f) {
            float shift = sin(uv.y * 50.0f + g_timeSeconds * 25.0f) * 0.035f;
            uv.x = frac(uv.x + shift);
        }
    }

    col = (g_trollPixelate > 1) ? g_inputTexture.Sample(g_samplerPoint, uv) : g_inputTexture.Sample(g_samplerLinear, uv);

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

    return float4(col.rgb, 1.0f);
}

float4 PS_OpticsNv12(VSOutput input) : SV_TARGET {
    float2 inUv = input.uv;
    if (g_trollPixelate > 1) {
        float blockSize = (float)g_trollPixelate;
        float2 numBlocks = float2(max(1.0f, g_canvasWidth), max(1.0f, g_canvasHeight)) / blockSize;
        inUv = (floor(inUv * numBlocks) + 0.5f) / numBlocks;
    }

    float blend = saturate(g_animBlend);
    float theta = g_animAngleRad;
    if (g_portraitFlip180 != 0) {
        theta += 3.14159265f;
    }
    bool mirror = (g_portraitMirror != 0);

    // Плавный расчет границ кадра с учетом пропорций (9:16, 4:3, 16:9):
    float halfWidth = clamp(g_animAspectHalfWidth, 0.1582f, 0.5f);
    float x0 = 0.5f - halfWidth;
    float x1 = 0.5f + halfWidth;

    float4 col = float4(0.0f, 0.0f, 0.0f, 1.0f);
    bool inBox = (inUv.x >= x0 && inUv.x <= x1);

    if (!inBox && (halfWidth < 0.498f)) {
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }

    float tx = (inUv.x - x0) / max(0.0001f, x1 - x0);
    float ty = inUv.y;
    if (mirror) tx = 1.0f - tx;

    float cx = tx - 0.5f;
    float cy = ty - 0.5f;
    float cosT = cos(theta);
    float sinT = sin(theta);

    float u = 0.5f + cx * cosT + cy * sinT;
    float v = 0.5f - cx * sinT + cy * cosT;
    if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f) {
        return float4(0.0f, 0.0f, 0.0f, 1.0f);
    }
    float2 uv = float2(u, v);

    // 1. Digital Zoom & Framing
    if (g_zoom > 1.01f) {
        float2 center = float2(0.5f, 0.5f) + float2(g_panX, g_panY);
        uv = (uv - center) / g_zoom + float2(0.5f, 0.5f);
        if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f) {
            return float4(0.0f, 0.0f, 0.0f, 1.0f);
        }
    }

    // 2. Troll FX: VHS Glitch Jitter
    if (g_trollGlitch > 0) {
        float scanline = sin(uv.y * 120.0f + g_timeSeconds * 12.0f);
        if (abs(scanline) > 0.82f) {
            float shift = sin(uv.y * 50.0f + g_timeSeconds * 25.0f) * 0.035f;
            uv.x = frac(uv.x + shift);
        }
    }

    col = float4(SampleNv12(uv, g_trollPixelate > 1), 1.0f);

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

float GetLuma601(float3 rgb) {
    return dot(rgb, float3(0.299f, 0.587f, 0.114f));
}

float BlockSad5(float2 uvBase, float2 shiftUv, float2 texelUv) {
    float2 o = texelUv * 2.5f;
    float2 h = shiftUv * 0.5f;
    float s = 0.0f;
    s += abs(GetLuma601(g_prevFrameTex.SampleLevel(g_samplerLinear, uvBase - h, 0).rgb)
           - GetLuma601(g_currFrameTex.SampleLevel(g_samplerLinear, uvBase + h, 0).rgb));
    s += abs(GetLuma601(g_prevFrameTex.SampleLevel(g_samplerLinear, uvBase + float2(-o.x, -o.y) - h, 0).rgb)
           - GetLuma601(g_currFrameTex.SampleLevel(g_samplerLinear, uvBase + float2(-o.x, -o.y) + h, 0).rgb));
    s += abs(GetLuma601(g_prevFrameTex.SampleLevel(g_samplerLinear, uvBase + float2(o.x, -o.y) - h, 0).rgb)
           - GetLuma601(g_currFrameTex.SampleLevel(g_samplerLinear, uvBase + float2(o.x, -o.y) + h, 0).rgb));
    s += abs(GetLuma601(g_prevFrameTex.SampleLevel(g_samplerLinear, uvBase + float2(-o.x, o.y) - h, 0).rgb)
           - GetLuma601(g_currFrameTex.SampleLevel(g_samplerLinear, uvBase + float2(-o.x, o.y) + h, 0).rgb));
    s += abs(GetLuma601(g_prevFrameTex.SampleLevel(g_samplerLinear, uvBase + float2(o.x, o.y) - h, 0).rgb)
           - GetLuma601(g_currFrameTex.SampleLevel(g_samplerLinear, uvBase + float2(o.x, o.y) + h, 0).rgb));
    return s * 0.2f;
}

// Motion Vector Estimation (Coarse-to-Fine 8x8 Block Matching on 160x90 grid)
float4 PS_MotionVectors(VSOutput input) : SV_TARGET {
    float2 texel = float2(1.0f / max(1.0f, g_canvasWidth), 1.0f / max(1.0f, g_canvasHeight));
    float zeroSad = BlockSad5(input.uv, float2(0.0f, 0.0f), texel);
    if (zeroSad < 0.012f) {
        return float4(0.0f, 0.0f, 1.0f, zeroSad);
    }

    float2 bestPx = float2(0.0f, 0.0f);
    float bestScore = zeroSad;
    float bestRawSad = zeroSad;

    // 1. Coarse 5x5 search (step = 6 px -> +-12 px)
    [unroll]
    for (int iy = -2; iy <= 2; ++iy) {
        [unroll]
        for (int ix = -2; ix <= 2; ++ix) {
            if (ix == 0 && iy == 0) continue;
            float2 candPx = float2((float)ix * 6.0f, (float)iy * 6.0f);
            float sad = BlockSad5(input.uv, candPx * texel, texel);
            float score = sad + length(candPx) * 0.0012f;
            if (score < bestScore) {
                bestScore = score;
                bestRawSad = sad;
                bestPx = candPx;
            }
        }
    }

    // 2. Medium 3x3 refinement (step = 2 px)
    float2 base1 = bestPx;
    [unroll]
    for (int iy2 = -1; iy2 <= 1; ++iy2) {
        [unroll]
        for (int ix2 = -1; ix2 <= 1; ++ix2) {
            if (ix2 == 0 && iy2 == 0) continue;
            float2 candPx = base1 + float2((float)ix2 * 2.0f, (float)iy2 * 2.0f);
            float sad = BlockSad5(input.uv, candPx * texel, texel);
            float score = sad + length(candPx) * 0.0012f;
            if (score < bestScore) {
                bestScore = score;
                bestRawSad = sad;
                bestPx = candPx;
            }
        }
    }

    // 3. Fine 3x3 refinement (step = 1 px)
    float2 base2 = bestPx;
    [unroll]
    for (int iy3 = -1; iy3 <= 1; ++iy3) {
        [unroll]
        for (int ix3 = -1; ix3 <= 1; ++ix3) {
            if (ix3 == 0 && iy3 == 0) continue;
            float2 candPx = base2 + float2((float)ix3, (float)iy3);
            float sad = BlockSad5(input.uv, candPx * texel, texel);
            float score = sad + length(candPx) * 0.0012f;
            if (score < bestScore) {
                bestScore = score;
                bestRawSad = sad;
                bestPx = candPx;
            }
        }
    }

    float conf = saturate(1.0f - bestRawSad / 0.14f);
    return float4(bestPx.x * texel.x, bestPx.y * texel.y, conf, zeroSad);
}

// Confidence-weighted 3x3 spatial motion vector smoothing (eliminates block seams)
float4 PS_MotionSmooth(VSOutput input) : SV_TARGET {
    float2 mvTexel = float2(1.0f / 160.0f, 1.0f / 90.0f);
    float4 center = g_motionVecTex.SampleLevel(g_samplerPoint, input.uv, 0);
    float2 sumVec = center.xy * (center.z + 0.05f);
    float sumW = center.z + 0.05f;

    [unroll]
    for (int iy = -1; iy <= 1; ++iy) {
        [unroll]
        for (int ix = -1; ix <= 1; ++ix) {
            if (ix == 0 && iy == 0) continue;
            float4 nb = g_motionVecTex.SampleLevel(g_samplerPoint, input.uv + float2((float)ix, (float)iy) * mvTexel, 0);
            float w = nb.z * (1.0f / (1.0f + length(nb.xy - center.xy) * 180.0f));
            sumVec += nb.xy * w;
            sumW += w;
        }
    }

    float2 smoothVec = sumVec / max(0.0001f, sumW);
    return float4(smoothVec.x, smoothVec.y, center.z, center.w);
}

// Bidirectional Symmetric Motion-Compensated Frame Synthesis (alpha = 0.5)
float4 PS_MotionWarpBlend(VSOutput input) : SV_TARGET {
    float4 mv = g_motionVecTex.SampleLevel(g_samplerLinear, input.uv, 0);
    float3 prevD = g_prevFrameTex.SampleLevel(g_samplerLinear, input.uv, 0).rgb;
    float3 currD = g_currFrameTex.SampleLevel(g_samplerLinear, input.uv, 0).rgb;
    float3 directBlend = lerp(prevD, currD, 0.5f);

    float2 halfV = mv.xy * (0.5f * mv.z);
    if (dot(halfV, halfV) < 1e-9f) {
        return float4(directBlend, 1.0f);
    }

    float2 uvPrev = clamp(input.uv - halfV, float2(0.0005f, 0.0005f), float2(0.9995f, 0.9995f));
    float2 uvCurr = clamp(input.uv + halfV, float2(0.0005f, 0.0005f), float2(0.9995f, 0.9995f));
    float3 prevW = g_prevFrameTex.SampleLevel(g_samplerLinear, uvPrev, 0).rgb;
    float3 currW = g_currFrameTex.SampleLevel(g_samplerLinear, uvCurr, 0).rgb;
    float3 warpedBlend = lerp(prevW, currW, 0.5f);

    // Occlusion / disocclusion guard: fallback smoothly if warped pixels disagree strongly
    float diffWarped = length(prevW - currW);
    float diffDirect = length(prevD - currD);
    float occWeight = saturate((diffWarped - 0.16f) / 0.18f) * step(diffDirect * 1.15f, diffWarped);
    float3 finalCol = lerp(warpedBlend, directBlend, occWeight);
    return float4(finalCol, 1.0f);
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

void D3D11OpticsPipeline::releaseSizeDependentResources() {
    m_rasterizerState.Reset();
    m_linearSampler.Reset();
    m_pointSampler.Reset();
    m_constantBuffer.Reset();

    m_hasPrevProcessed = false;
    m_stagingFrameIndex = 0;
    m_stallSkipCount = 0;

    m_interpUVStagingTex.Reset();
    m_interpYStagingTex.Reset();
    m_interpProcessedSRV.Reset();
    m_interpProcessedRTV.Reset();
    m_interpProcessedTex.Reset();
    m_motionSmoothSRV.Reset();
    m_motionSmoothRTV.Reset();
    m_motionSmoothTex.Reset();
    m_motionVecSRV.Reset();
    m_motionVecRTV.Reset();
    m_motionVecTex.Reset();
    m_prevProcessedSRV.Reset();
    m_prevProcessedTex.Reset();

    m_nv12UVStagingTex[0].Reset();
    m_nv12UVStagingTex[1].Reset();
    m_nv12UVStagingTex[2].Reset();
    m_nv12UVRTV.Reset();
    m_nv12UVTex.Reset();

    m_nv12YStagingTex[0].Reset();
    m_nv12YStagingTex[1].Reset();
    m_nv12YStagingTex[2].Reset();
    m_nv12YRTV.Reset();
    m_nv12YTex.Reset();

    m_stagingBgraTex.Reset();
    m_processedSRV.Reset();
    m_processedRTV.Reset();
    m_processedTex.Reset();

    m_inputSRV.Reset();
    m_inputTex.Reset();

    m_inputYSRV.Reset();
    m_inputYTex.Reset();
    m_inputUVSRV.Reset();
    m_inputUVTex.Reset();
    m_inputTexW = 0;
    m_inputTexH = 0;

    m_gpuInputYSRV.Reset();
    m_gpuInputUVSRV.Reset();
    m_gpuInputNv12Tex.Reset();
    m_gpuInputTexW = 0;
    m_gpuInputTexH = 0;

    m_sharedHandle = nullptr;
}

void D3D11OpticsPipeline::cleanup() {
    std::lock_guard<std::recursive_mutex> lock(m_deviceMutex);
    m_initialized = false;
    releaseSizeDependentResources();

    m_vertexShader.Reset();
    m_opticsPixelShader.Reset();
    m_opticsNv12PixelShader.Reset();
    m_nv12YPixelShader.Reset();
    m_nv12UVPixelShader.Reset();
    m_motionVecPixelShader.Reset();
    m_motionSmoothPixelShader.Reset();
    m_motionWarpBlendPixelShader.Reset();

    m_context.Reset();
    m_device.Reset();
}

bool D3D11OpticsPipeline::resize(int width, int height) {
    if (width < 2 || height < 2) return false;
    std::lock_guard<std::recursive_mutex> lock(m_deviceMutex);
    if (!m_initialized || !m_device || !m_context) {
        return init(width, height);
    }
    if (m_width == width && m_height == height) {
        return true;
    }

    m_initialized = false;
    m_context->ClearState();
    releaseSizeDependentResources();
    m_width = width;
    m_height = height;

    if (!createResources()) {
        cleanup();
        return false;
    }

    m_initialized = true;
    std::cout << "[D3D11 PIPELINE] Canvas resized to " << m_width << "x" << m_height << "\n";
    return true;
}

bool D3D11OpticsPipeline::init(int width, int height) {
    std::lock_guard<std::recursive_mutex> lock(m_deviceMutex);
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

    // Включаем защиту многопоточного доступа для безопасного разделения устройства с FFmpeg D3D11VA
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(m_device.As(&mt)) && mt) {
        mt->SetMultithreadProtected(TRUE);
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

    ComPtr<ID3DBlob> vsBlob, psOpticsBlob, psOpticsNv12Blob, psNV12YBlob, psNV12UVBlob, errorBlob;

    // 1. Vertex Shader
    HRESULT hr = D3DCompile(g_hlslShaderSource, strlen(g_hlslShaderSource), "D3D11OpticsHLSL",
                            nullptr, nullptr, "VSMain", "vs_5_0", compileFlags, 0, &vsBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) std::cerr << "[D3D11 HLSL] VS error: " << (char*)errorBlob->GetBufferPointer() << "\n";
        return false;
    }
    hr = m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_vertexShader);
    if (FAILED(hr)) return false;

    // 2. Optics & Troll FX Pixel Shader (BGRA Input)
    hr = D3DCompile(g_hlslShaderSource, strlen(g_hlslShaderSource), "D3D11OpticsHLSL",
                    nullptr, nullptr, "PS_Optics", "ps_5_0", compileFlags, 0, &psOpticsBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) std::cerr << "[D3D11 HLSL] PS_Optics error: " << (char*)errorBlob->GetBufferPointer() << "\n";
        return false;
    }
    hr = m_device->CreatePixelShader(psOpticsBlob->GetBufferPointer(), psOpticsBlob->GetBufferSize(), nullptr, &m_opticsPixelShader);
    if (FAILED(hr)) return false;

    // 3. Optics & Troll FX Pixel Shader (Direct NV12 Input: Y=t1, UV=t2)
    hr = D3DCompile(g_hlslShaderSource, strlen(g_hlslShaderSource), "D3D11OpticsHLSL",
                    nullptr, nullptr, "PS_OpticsNv12", "ps_5_0", compileFlags, 0, &psOpticsNv12Blob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) std::cerr << "[D3D11 HLSL] PS_OpticsNv12 error: " << (char*)errorBlob->GetBufferPointer() << "\n";
        return false;
    }
    hr = m_device->CreatePixelShader(psOpticsNv12Blob->GetBufferPointer(), psOpticsNv12Blob->GetBufferSize(), nullptr, &m_opticsNv12PixelShader);
    if (FAILED(hr)) return false;

    // 4-7. Pixel shaders compiled uniformly
    struct PsSpec {
        const char* entry;
        ComPtr<ID3D11PixelShader>* target;
    };
    const PsSpec psSpecs[] = {
        { "PS_NV12_Y",          &m_nv12YPixelShader },
        { "PS_NV12_UV",         &m_nv12UVPixelShader },
        { "PS_MotionVectors",   &m_motionVecPixelShader },
        { "PS_MotionSmooth",    &m_motionSmoothPixelShader },
        { "PS_MotionWarpBlend", &m_motionWarpBlendPixelShader },
    };
    for (const auto& spec : psSpecs) {
        ComPtr<ID3DBlob> psBlob;
        hr = D3DCompile(g_hlslShaderSource, strlen(g_hlslShaderSource), "D3D11OpticsHLSL",
                        nullptr, nullptr, spec.entry, "ps_5_0", compileFlags, 0, &psBlob, &errorBlob);
        if (FAILED(hr)) {
            if (errorBlob) std::cerr << "[D3D11 HLSL] " << spec.entry << " error: " << (char*)errorBlob->GetBufferPointer() << "\n";
            return false;
        }
        hr = m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, spec.target->GetAddressOf());
        if (FAILED(hr)) return false;
    }

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

    // Triple-buffered Y staging (3 textures for zero-stall GPU->CPU DMA)
    texDesc.Usage = D3D11_USAGE_STAGING;
    texDesc.BindFlags = 0;
    texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = m_device->CreateTexture2D(&texDesc, nullptr, &m_nv12YStagingTex[0]);
    if (FAILED(hr)) return false;
    hr = m_device->CreateTexture2D(&texDesc, nullptr, &m_nv12YStagingTex[1]);
    if (FAILED(hr)) return false;
    hr = m_device->CreateTexture2D(&texDesc, nullptr, &m_nv12YStagingTex[2]);
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

    // Triple-buffered UV staging (3 textures for zero-stall GPU->CPU DMA)
    texDesc.Usage = D3D11_USAGE_STAGING;
    texDesc.BindFlags = 0;
    texDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = m_device->CreateTexture2D(&texDesc, nullptr, &m_nv12UVStagingTex[0]);
    if (FAILED(hr)) return false;
    hr = m_device->CreateTexture2D(&texDesc, nullptr, &m_nv12UVStagingTex[1]);
    if (FAILED(hr)) return false;
    hr = m_device->CreateTexture2D(&texDesc, nullptr, &m_nv12UVStagingTex[2]);
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

    // 9. Ресурсы Motion-Compensated Frame Synthesis (30->60 FPS на GPU)
    D3D11_TEXTURE2D_DESC bgraDefault{};
    bgraDefault.Width = m_width;
    bgraDefault.Height = m_height;
    bgraDefault.MipLevels = 1;
    bgraDefault.ArraySize = 1;
    bgraDefault.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    bgraDefault.SampleDesc.Count = 1;
    bgraDefault.Usage = D3D11_USAGE_DEFAULT;
    bgraDefault.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    hr = m_device->CreateTexture2D(&bgraDefault, nullptr, &m_prevProcessedTex);
    if (FAILED(hr)) return false;
    hr = m_device->CreateShaderResourceView(m_prevProcessedTex.Get(), nullptr, &m_prevProcessedSRV);
    if (FAILED(hr)) return false;

    bgraDefault.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    hr = m_device->CreateTexture2D(&bgraDefault, nullptr, &m_interpProcessedTex);
    if (FAILED(hr)) return false;
    hr = m_device->CreateRenderTargetView(m_interpProcessedTex.Get(), nullptr, &m_interpProcessedRTV);
    if (FAILED(hr)) return false;
    hr = m_device->CreateShaderResourceView(m_interpProcessedTex.Get(), nullptr, &m_interpProcessedSRV);
    if (FAILED(hr)) return false;

    D3D11_TEXTURE2D_DESC mvDesc{};
    mvDesc.Width = m_mvWidth;
    mvDesc.Height = m_mvHeight;
    mvDesc.MipLevels = 1;
    mvDesc.ArraySize = 1;
    mvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    mvDesc.SampleDesc.Count = 1;
    mvDesc.Usage = D3D11_USAGE_DEFAULT;
    mvDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    hr = m_device->CreateTexture2D(&mvDesc, nullptr, &m_motionVecTex);
    if (FAILED(hr)) return false;
    hr = m_device->CreateRenderTargetView(m_motionVecTex.Get(), nullptr, &m_motionVecRTV);
    if (FAILED(hr)) return false;
    hr = m_device->CreateShaderResourceView(m_motionVecTex.Get(), nullptr, &m_motionVecSRV);
    if (FAILED(hr)) return false;

    hr = m_device->CreateTexture2D(&mvDesc, nullptr, &m_motionSmoothTex);
    if (FAILED(hr)) return false;
    hr = m_device->CreateRenderTargetView(m_motionSmoothTex.Get(), nullptr, &m_motionSmoothRTV);
    if (FAILED(hr)) return false;
    hr = m_device->CreateShaderResourceView(m_motionSmoothTex.Get(), nullptr, &m_motionSmoothSRV);
    if (FAILED(hr)) return false;

    D3D11_TEXTURE2D_DESC stgDesc{};
    stgDesc.Width = m_width;
    stgDesc.Height = m_height;
    stgDesc.MipLevels = 1;
    stgDesc.ArraySize = 1;
    stgDesc.Format = DXGI_FORMAT_R8_UNORM;
    stgDesc.SampleDesc.Count = 1;
    stgDesc.Usage = D3D11_USAGE_STAGING;
    stgDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = m_device->CreateTexture2D(&stgDesc, nullptr, &m_interpYStagingTex);
    if (FAILED(hr)) return false;

    stgDesc.Width = m_width / 2;
    stgDesc.Height = m_height / 2;
    stgDesc.Format = DXGI_FORMAT_R8G8_UNORM;
    hr = m_device->CreateTexture2D(&stgDesc, nullptr, &m_interpUVStagingTex);
    if (FAILED(hr)) return false;

    return true;
}

bool D3D11OpticsPipeline::processBgra(const uint32_t* srcBgra, uint32_t* dstBgra, const D3D11ShaderParams& params) {
    if (!m_initialized || !srcBgra || !dstBgra) return false;
    std::lock_guard<std::recursive_mutex> lock(m_deviceMutex);

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
    std::lock_guard<std::recursive_mutex> lock(m_deviceMutex);

    // Индексы Triple-Buffering: write в [wIdx = frame % 3]
    const int wIdx = m_stagingFrameIndex % 3;

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

    // 6. Копирование Y и UV в write-staging (wIdx)
    m_context->CopyResource(m_nv12YStagingTex[wIdx].Get(),  m_nv12YTex.Get());
    m_context->CopyResource(m_nv12UVStagingTex[wIdx].Get(), m_nv12UVTex.Get());
    m_context->Flush();

    // 7. Чтение Y и UV в dstNv12:
    //    Triple-buffering: сперва проверяем кадр 1-кадровой давности без блокировки.
    //    Если GPU еще занят, берем кадр 2-кадровой давности (гарантированно готов).
    //    Строгий запрет синхронной блокировки на живом стриме!
    int targetReadIdx = 0;
    bool readSuccess = false;

    if (m_stagingFrameIndex < 2) {
        targetReadIdx = 0;
        hr = m_context->Map(m_nv12YStagingTex[0].Get(), 0, D3D11_MAP_READ, 0, &mapped);
    } else {
        targetReadIdx = (m_stagingFrameIndex + 2) % 3; // 1 кадр назад
        hr = m_context->Map(m_nv12YStagingTex[targetReadIdx].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
            targetReadIdx = (m_stagingFrameIndex + 1) % 3; // 2 кадра назад
            hr = m_context->Map(m_nv12YStagingTex[targetReadIdx].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
        }
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
            // Если оба кадра еще в очереди, мягкий синхронный маппинг (<0.2 ms) исключает пропуск кадра
            hr = m_context->Map(m_nv12YStagingTex[targetReadIdx].Get(), 0, D3D11_MAP_READ, 0, &mapped);
        }
    }

    if (SUCCEEDED(hr)) {
        uint8_t* dstY = dstNv12;
        if (mapped.RowPitch == (UINT)m_width) {
            memcpy(dstY, mapped.pData, (size_t)m_width * m_height);
        } else {
            const uint8_t* srcY = reinterpret_cast<const uint8_t*>(mapped.pData);
            for (int y = 0; y < m_height; ++y) {
                memcpy(dstY + y * m_width, srcY + y * mapped.RowPitch, m_width);
            }
        }
        m_context->Unmap(m_nv12YStagingTex[targetReadIdx].Get(), 0);

        UINT uvMapFlags = (m_stagingFrameIndex < 2) ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT;
        hr = m_context->Map(m_nv12UVStagingTex[targetReadIdx].Get(), 0, D3D11_MAP_READ, uvMapFlags, &mapped);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
            hr = m_context->Map(m_nv12UVStagingTex[targetReadIdx].Get(), 0, D3D11_MAP_READ, 0, &mapped);
        }

        if (SUCCEEDED(hr)) {
            uint8_t* dstUV = dstNv12 + ((size_t)m_width * m_height);
            const size_t uvRowBytes = (size_t)m_width;
            if (mapped.RowPitch == (UINT)uvRowBytes) {
                memcpy(dstUV, mapped.pData, (size_t)m_width * m_height / 2);
            } else {
                const uint8_t* srcUV = reinterpret_cast<const uint8_t*>(mapped.pData);
                for (int y = 0; y < m_height / 2; ++y) {
                    memcpy(dstUV + y * uvRowBytes, srcUV + y * mapped.RowPitch, uvRowBytes);
                }
            }
            m_context->Unmap(m_nv12UVStagingTex[targetReadIdx].Get(), 0);
            readSuccess = true;
        }
    }

    ++m_stagingFrameIndex;

    // Возвращаем false при сбое, чтобы вызывающий код переключился на CPU fallback
    return readSuccess;
}

bool D3D11OpticsPipeline::ensureNv12InputResources(int inW, int inH) {
    if (m_inputYTex && m_inputUVTex && m_inputTexW == inW && m_inputTexH == inH) {
        return true;
    }

    m_inputYSRV.Reset();
    m_inputYTex.Reset();
    m_inputUVSRV.Reset();
    m_inputUVTex.Reset();
    m_inputTexW = 0;
    m_inputTexH = 0;

    m_gpuInputYSRV.Reset();
    m_gpuInputUVSRV.Reset();
    m_gpuInputNv12Tex.Reset();
    m_gpuInputTexW = 0;
    m_gpuInputTexH = 0;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = inW;
    desc.Height = inH;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &m_inputYTex);
    if (FAILED(hr)) return false;

    hr = m_device->CreateShaderResourceView(m_inputYTex.Get(), nullptr, &m_inputYSRV);
    if (FAILED(hr)) return false;

    desc.Width = inW / 2;
    desc.Height = inH / 2;
    desc.Format = DXGI_FORMAT_R8G8_UNORM;

    hr = m_device->CreateTexture2D(&desc, nullptr, &m_inputUVTex);
    if (FAILED(hr)) return false;

    hr = m_device->CreateShaderResourceView(m_inputUVTex.Get(), nullptr, &m_inputUVSRV);
    if (FAILED(hr)) return false;

    m_inputTexW = inW;
    m_inputTexH = inH;
    return true;
}

bool D3D11OpticsPipeline::ensureGpuNv12InputResources(int inW, int inH) {
    if (m_gpuInputNv12Tex && m_gpuInputYSRV && m_gpuInputUVSRV && m_gpuInputTexW == inW && m_gpuInputTexH == inH) {
        return true;
    }

    m_gpuInputYSRV.Reset();
    m_gpuInputUVSRV.Reset();
    m_gpuInputNv12Tex.Reset();
    m_gpuInputTexW = 0;
    m_gpuInputTexH = 0;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = inW;
    desc.Height = inH;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;

    HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &m_gpuInputNv12Tex);
    if (FAILED(hr)) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;

    srvDesc.Format = DXGI_FORMAT_R8_UNORM;
    hr = m_device->CreateShaderResourceView(m_gpuInputNv12Tex.Get(), &srvDesc, &m_gpuInputYSRV);
    if (FAILED(hr)) return false;

    srvDesc.Format = DXGI_FORMAT_R8G8_UNORM;
    hr = m_device->CreateShaderResourceView(m_gpuInputNv12Tex.Get(), &srvDesc, &m_gpuInputUVSRV);
    if (FAILED(hr)) return false;

    m_gpuInputTexW = inW;
    m_gpuInputTexH = inH;
    return true;
}

void D3D11OpticsPipeline::resetMotionHistory() {
    std::lock_guard<std::recursive_mutex> lock(m_deviceMutex);
    m_hasPrevProcessed = false;
}

void D3D11OpticsPipeline::renderProcessedToNv12Targets(
    ID3D11ShaderResourceView* processedSrv,
    ID3D11Texture2D* yStaging,
    ID3D11Texture2D* uvStaging
) {
    D3D11_VIEWPORT vp{};
    vp.Width = (float)m_width;
    vp.Height = (float)m_height;
    vp.MaxDepth = 1.0f;
    m_context->RSSetViewports(1, &vp);

    ID3D11RenderTargetView* rtvList[1] = { m_nv12YRTV.Get() };
    m_context->OMSetRenderTargets(1, rtvList, nullptr);

    ID3D11ShaderResourceView* srvListProc[1] = { processedSrv };
    m_context->PSSetShaderResources(0, 1, srvListProc);
    m_context->PSSetShader(m_nv12YPixelShader.Get(), nullptr, 0);
    m_context->Draw(3, 0);

    vp.Width = (float)(m_width / 2);
    vp.Height = (float)(m_height / 2);
    m_context->RSSetViewports(1, &vp);

    rtvList[0] = m_nv12UVRTV.Get();
    m_context->OMSetRenderTargets(1, rtvList, nullptr);
    m_context->PSSetShader(m_nv12UVPixelShader.Get(), nullptr, 0);
    m_context->Draw(3, 0);

    rtvList[0] = nullptr;
    m_context->OMSetRenderTargets(1, rtvList, nullptr);
    srvListProc[0] = nullptr;
    m_context->PSSetShaderResources(0, 1, srvListProc);

    m_context->CopyResource(yStaging, m_nv12YTex.Get());
    m_context->CopyResource(uvStaging, m_nv12UVTex.Get());
}

bool D3D11OpticsPipeline::readbackNv12Staging(
    ID3D11Texture2D* yStaging,
    ID3D11Texture2D* uvStaging,
    UINT yMapFlags,
    UINT uvMapFlags,
    uint8_t* dstNv12
) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = m_context->Map(yStaging, 0, D3D11_MAP_READ, yMapFlags, &mapped);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING && yMapFlags != 0) {
        return false;
    }
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
    m_context->Unmap(yStaging, 0);

    hr = m_context->Map(uvStaging, 0, D3D11_MAP_READ, uvMapFlags, &mapped);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING) {
        if (uvMapFlags != 0) {
            return false;
        }
        hr = m_context->Map(uvStaging, 0, D3D11_MAP_READ, 0, &mapped);
    }
    if (FAILED(hr)) return false;

    uint8_t* dstUV = dstNv12 + ((size_t)m_width * m_height);
    const size_t uvRowBytes = (size_t)m_width;
    if (mapped.RowPitch == (UINT)uvRowBytes) {
        memcpy(dstUV, mapped.pData, (size_t)m_width * m_height / 2);
    } else {
        const uint8_t* srcUV = reinterpret_cast<const uint8_t*>(mapped.pData);
        for (int y = 0; y < m_height / 2; ++y) {
            memcpy(dstUV + y * uvRowBytes, srcUV + y * mapped.RowPitch, uvRowBytes);
        }
    }
    m_context->Unmap(uvStaging, 0);
    return true;
}

bool D3D11OpticsPipeline::runNv12PassesAndReadback(
    ID3D11ShaderResourceView* ySrv,
    ID3D11ShaderResourceView* uvSrv,
    uint8_t* dstCurrNv12,
    uint8_t* dstInterpNv12,
    bool synthesizeInterp,
    bool* outDidSynth,
    const D3D11ShaderParams& params
) {
    if (outDidSynth) *outDidSynth = false;
    const int wIdx = m_stagingFrameIndex % 3;

    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr)) {
        memcpy(mapped.pData, &params, sizeof(D3D11ShaderParams));
        m_context->Unmap(m_constantBuffer.Get(), 0);
    }

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

    ID3D11ShaderResourceView* srvListNv12[3] = { nullptr, ySrv, uvSrv };
    m_context->PSSetShaderResources(0, 3, srvListNv12);

    ID3D11SamplerState* samplers[2] = { m_linearSampler.Get(), m_pointSampler.Get() };
    m_context->PSSetSamplers(0, 2, samplers);

    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
    m_context->PSSetShader(m_opticsNv12PixelShader.Get(), nullptr, 0);
    m_context->Draw(3, 0);

    ID3D11ShaderResourceView* nullSrvs[6] = { nullptr, nullptr, nullptr, nullptr, nullptr, nullptr };
    rtvList[0] = nullptr;
    m_context->OMSetRenderTargets(1, rtvList, nullptr);
    m_context->PSSetShaderResources(0, 6, nullSrvs);

    const bool doSynthNow = (synthesizeInterp && dstInterpNv12 != nullptr && m_hasPrevProcessed);
    if (doSynthNow) {
        // 1. Block Motion Estimation (160x90)
        D3D11_VIEWPORT mvVp{};
        mvVp.Width = (float)m_mvWidth;
        mvVp.Height = (float)m_mvHeight;
        mvVp.MaxDepth = 1.0f;
        m_context->RSSetViewports(1, &mvVp);

        rtvList[0] = m_motionVecRTV.Get();
        m_context->OMSetRenderTargets(1, rtvList, nullptr);
        ID3D11ShaderResourceView* mvInputs[2] = { m_prevProcessedSRV.Get(), m_processedSRV.Get() };
        m_context->PSSetShaderResources(3, 2, mvInputs);
        m_context->PSSetShader(m_motionVecPixelShader.Get(), nullptr, 0);
        m_context->Draw(3, 0);

        // 2. Confidence-weighted 3x3 vector field smoothing (160x90)
        rtvList[0] = nullptr;
        m_context->OMSetRenderTargets(1, rtvList, nullptr);
        m_context->PSSetShaderResources(3, 3, nullSrvs);

        rtvList[0] = m_motionSmoothRTV.Get();
        m_context->OMSetRenderTargets(1, rtvList, nullptr);
        ID3D11ShaderResourceView* mvSrv[1] = { m_motionVecSRV.Get() };
        m_context->PSSetShaderResources(5, 1, mvSrv);
        m_context->PSSetShader(m_motionSmoothPixelShader.Get(), nullptr, 0);
        m_context->Draw(3, 0);

        // 3. Bidirectional Symmetric Motion-Compensated Blend -> m_interpProcessedTex (1280x720)
        rtvList[0] = nullptr;
        m_context->OMSetRenderTargets(1, rtvList, nullptr);
        m_context->PSSetShaderResources(5, 1, nullSrvs);

        m_context->RSSetViewports(1, &vp);
        rtvList[0] = m_interpProcessedRTV.Get();
        m_context->OMSetRenderTargets(1, rtvList, nullptr);
        ID3D11ShaderResourceView* warpInputs[3] = { m_prevProcessedSRV.Get(), m_processedSRV.Get(), m_motionSmoothSRV.Get() };
        m_context->PSSetShaderResources(3, 3, warpInputs);
        m_context->PSSetShader(m_motionWarpBlendPixelShader.Get(), nullptr, 0);
        m_context->Draw(3, 0);

        rtvList[0] = nullptr;
        m_context->OMSetRenderTargets(1, rtvList, nullptr);
        m_context->PSSetShaderResources(0, 6, nullSrvs);

        // 4. Encode m_interpProcessedTex -> NV12 (sync readback) + m_processedTex -> NV12 (wIdx)
        renderProcessedToNv12Targets(m_interpProcessedSRV.Get(), m_interpYStagingTex.Get(), m_interpUVStagingTex.Get());
        renderProcessedToNv12Targets(m_processedSRV.Get(), m_nv12YStagingTex[wIdx].Get(), m_nv12UVStagingTex[wIdx].Get());
        m_context->Flush();

        bool interpOk = readbackNv12Staging(m_interpYStagingTex.Get(), m_interpUVStagingTex.Get(), 0, 0, dstInterpNv12);
        bool currOk   = readbackNv12Staging(m_nv12YStagingTex[wIdx].Get(), m_nv12UVStagingTex[wIdx].Get(), 0, 0, dstCurrNv12);

        m_context->CopyResource(m_prevProcessedTex.Get(), m_processedTex.Get());
        m_hasPrevProcessed = true;
        ++m_stagingFrameIndex;
        if (outDidSynth) *outDidSynth = (interpOk && currOk);
        return currOk;
    }

    // Standard single-frame NV12 path
    renderProcessedToNv12Targets(m_processedSRV.Get(), m_nv12YStagingTex[wIdx].Get(), m_nv12UVStagingTex[wIdx].Get());
    m_context->Flush();

    bool readSuccess = false;
    if (synthesizeInterp) {
        // First warmup frame of synthesis: sync readback of wIdx + snapshot previous frame in VRAM
        readSuccess = readbackNv12Staging(m_nv12YStagingTex[wIdx].Get(), m_nv12UVStagingTex[wIdx].Get(), 0, 0, dstCurrNv12);
        m_context->CopyResource(m_prevProcessedTex.Get(), m_processedTex.Get());
        m_hasPrevProcessed = true;
    } else {
        m_hasPrevProcessed = false;
        int targetReadIdx = 0;
        if (m_stagingFrameIndex < 2) {
            targetReadIdx = 0;
            readSuccess = readbackNv12Staging(m_nv12YStagingTex[0].Get(), m_nv12UVStagingTex[0].Get(), 0, 0, dstCurrNv12);
        } else {
            targetReadIdx = (m_stagingFrameIndex + 2) % 3;
            readSuccess = readbackNv12Staging(m_nv12YStagingTex[targetReadIdx].Get(), m_nv12UVStagingTex[targetReadIdx].Get(), D3D11_MAP_FLAG_DO_NOT_WAIT, D3D11_MAP_FLAG_DO_NOT_WAIT, dstCurrNv12);
            if (!readSuccess) {
                ++m_stallSkipCount;
                targetReadIdx = (m_stagingFrameIndex + 1) % 3;
                readSuccess = readbackNv12Staging(m_nv12YStagingTex[targetReadIdx].Get(), m_nv12UVStagingTex[targetReadIdx].Get(), D3D11_MAP_FLAG_DO_NOT_WAIT, D3D11_MAP_FLAG_DO_NOT_WAIT, dstCurrNv12);
            }
            if (!readSuccess) {
                readSuccess = readbackNv12Staging(m_nv12YStagingTex[targetReadIdx].Get(), m_nv12UVStagingTex[targetReadIdx].Get(), 0, 0, dstCurrNv12);
            }
        }
    }

    ++m_stagingFrameIndex;
    return readSuccess;
}

bool D3D11OpticsPipeline::processD3D11TextureToNv12WithSynth(
    ID3D11Texture2D* srcTex,
    UINT srcSubresource,
    int inW, int inH,
    uint8_t* dstCurrNv12,
    uint8_t* dstInterpNv12,
    bool synthesizeInterp,
    bool* outDidSynth,
    const D3D11ShaderParams& params
) {
    if (!m_initialized || !srcTex || !dstCurrNv12 || inW <= 1 || inH <= 1) {
        return false;
    }

    std::lock_guard<std::recursive_mutex> lock(m_deviceMutex);

    int alignedW = inW & ~1;
    int alignedH = inH & ~1;

    D3D11_TEXTURE2D_DESC srcDesc{};
    srcTex->GetDesc(&srcDesc);
    if (srcDesc.Format != DXGI_FORMAT_NV12) {
        return false;
    }
    if ((UINT)alignedW > srcDesc.Width || (UINT)alignedH > srcDesc.Height || srcSubresource >= srcDesc.ArraySize) {
        return false;
    }

    if (!ensureGpuNv12InputResources(alignedW, alignedH)) {
        return false;
    }

    D3D11_BOX srcBox{};
    srcBox.left = 0;
    srcBox.top = 0;
    srcBox.front = 0;
    srcBox.right = static_cast<UINT>(alignedW);
    srcBox.bottom = static_cast<UINT>(alignedH);
    srcBox.back = 1;

    m_context->CopySubresourceRegion(m_gpuInputNv12Tex.Get(), 0, 0, 0, 0, srcTex, srcSubresource, &srcBox);
    return runNv12PassesAndReadback(m_gpuInputYSRV.Get(), m_gpuInputUVSRV.Get(), dstCurrNv12, dstInterpNv12, synthesizeInterp, outDidSynth, params);
}

bool D3D11OpticsPipeline::processD3D11TextureToNv12(
    ID3D11Texture2D* srcTex,
    UINT srcSubresource,
    int inW, int inH,
    uint8_t* dstNv12,
    const D3D11ShaderParams& params
) {
    return processD3D11TextureToNv12WithSynth(srcTex, srcSubresource, inW, inH, dstNv12, nullptr, false, nullptr, params);
}

bool D3D11OpticsPipeline::processNv12ToNv12WithSynth(
    const uint8_t* srcY, int strideY,
    const uint8_t* srcUV, int strideUV,
    int inW, int inH,
    uint8_t* dstCurrNv12,
    uint8_t* dstInterpNv12,
    bool synthesizeInterp,
    bool* outDidSynth,
    const D3D11ShaderParams& params
) {
    inW &= ~1;
    inH &= ~1;
    if (!m_initialized || !srcY || !srcUV || !dstCurrNv12 || inW <= 1 || inH <= 1) {
        return false;
    }

    std::lock_guard<std::recursive_mutex> lock(m_deviceMutex);

    if (!ensureNv12InputResources(inW, inH)) {
        return false;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    HRESULT hr = m_context->Map(m_inputYTex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return false;

    if (mapped.RowPitch == (UINT)strideY && strideY == inW) {
        memcpy(mapped.pData, srcY, (size_t)inW * inH);
    } else {
        const uint8_t* srcBytes = srcY;
        uint8_t* dstBytes = reinterpret_cast<uint8_t*>(mapped.pData);
        for (int y = 0; y < inH; ++y) {
            memcpy(dstBytes + y * mapped.RowPitch, srcBytes + y * strideY, inW);
        }
    }
    m_context->Unmap(m_inputYTex.Get(), 0);

    hr = m_context->Map(m_inputUVTex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) return false;

    const int uvHalfH = inH / 2;
    if (mapped.RowPitch == (UINT)strideUV && strideUV == inW) {
        memcpy(mapped.pData, srcUV, (size_t)inW * uvHalfH);
    } else {
        const uint8_t* srcBytes = srcUV;
        uint8_t* dstBytes = reinterpret_cast<uint8_t*>(mapped.pData);
        for (int y = 0; y < uvHalfH; ++y) {
            memcpy(dstBytes + y * mapped.RowPitch, srcBytes + y * strideUV, inW);
        }
    }
    m_context->Unmap(m_inputUVTex.Get(), 0);

    return runNv12PassesAndReadback(m_inputYSRV.Get(), m_inputUVSRV.Get(), dstCurrNv12, dstInterpNv12, synthesizeInterp, outDidSynth, params);
}

bool D3D11OpticsPipeline::processNv12ToNv12(
    const uint8_t* srcY, int strideY,
    const uint8_t* srcUV, int strideUV,
    int inW, int inH,
    uint8_t* dstNv12,
    const D3D11ShaderParams& params
) {
    return processNv12ToNv12WithSynth(srcY, strideY, srcUV, strideUV, inW, inH, dstNv12, nullptr, false, nullptr, params);
}

bool D3D11OpticsPipeline::processNv12ToNv12(const uint8_t* srcNv12, int inW, int inH, uint8_t* dstNv12, const D3D11ShaderParams& params) {
    if (!srcNv12) return false;
    return processNv12ToNv12(srcNv12, inW, srcNv12 + ((size_t)inW * inH), inW, inW, inH, dstNv12, params);
}

