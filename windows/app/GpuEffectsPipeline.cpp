#include "GpuEffectsPipeline.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>

namespace zerocam {

namespace {

struct alignas(16) FxConstants {
    float texelX;
    float texelY;
    float blurInvX;
    float blurInvY;

    float brightness;
    float contrast;
    float saturation;
    float warmth;

    float gammaInv;
    float sharpness;
    float digitalZoom;
    float mirrorH;

    float blurMode;
    float blurStyle;
    float focusX;
    float focusY;

    float bgPremul;
    float aspect;
    float bokehRadiusPx;
    float pad0;
};

static_assert(sizeof(FxConstants) == 80, "FxConstants debe mantener alineacion de 16 bytes");

constexpr const char* kShadersHlsl = R"HLSL(
cbuffer FxParams : register(b0) {
    float2 texelSize;
    float2 blurInvSize;
    float  brightness;
    float  contrast;
    float  saturation;
    float  warmth;
    float  gammaInv;
    float  sharpness;
    float  digitalZoom;
    float  mirrorH;
    float  blurMode;
    float  blurStyle;
    float2 focusPt;
    float  bgPremul;
    float  aspect;
    float  bokehRadiusPx;
    float  _pad0;
};

Texture2D    texMain  : register(t0);
Texture2D    texBlur  : register(t1);
Texture2D    texMatte : register(t2);
SamplerState samLin   : register(s0);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut MainVS(uint vid : SV_VertexID) {
    VSOut o;
    float2 p = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = p;
    return o;
}

float2 ApplyFrameTransform(float2 uv) {
    if (mirrorH > 0.5) uv.x = 1.0 - uv.x;
    return (uv - 0.5) / max(digitalZoom, 1.0) + 0.5;
}

float4 DownsampleTransformPS(VSOut input) : SV_Target {
    float4 col = texMain.SampleLevel(samLin, ApplyFrameTransform(input.uv), 0);
    if (bgPremul > 0.5) {
        // Suprime gradualmente al sujeto en el buffer de fondo antes del disco bokeh para evitar halos.
        float pha = texMatte.SampleLevel(samLin, input.uv, 0).a;
        float bgW = saturate(1.0 - smoothstep(0.03, 0.42, pha));
        return float4(col.rgb * bgW, bgW);
    }
    return float4(col.rgb, 1.0);
}

// Bokeh de disco óptico por espiral de ángulo áureo (Vogel 36 muestras) a 1/2 resolución.
float4 BokehDiscPS(VSOut input) : SV_Target {
    static const int kTaps = 36;
    static const float kInvTaps = 1.0 / 36.0;
    static const float kCosGA = -0.73736888; // cos(2.39996323)
    static const float kSinGA =  0.67549029; // sin(2.39996323)

    float3 accRgb = float3(0.0, 0.0, 0.0);
    float  accW   = 0.0;
    float  covSum = 0.0;

    // Muestra central para estabilidad en aperturas suaves.
    {
        float4 s0 = texMain.SampleLevel(samLin, input.uv, 0);
        float w0 = s0.a;
        if (w0 > 1e-4) {
            float3 rgb0 = saturate(s0.rgb / w0);
            float luma0 = dot(rgb0, float3(0.2126, 0.7152, 0.0722));
            float hi0 = 1.0 + 1.15 * luma0 * luma0 * luma0;
            accRgb += s0.rgb * hi0;
            accW   += w0 * hi0;
            covSum += w0;
        }
    }

    float2 dir = float2(1.0, 0.0);
    [loop]
    for (int i = 0; i < kTaps; ++i) {
        float r = sqrt((float(i) + 0.5) * kInvTaps) * bokehRadiusPx;
        float2 suv = saturate(input.uv + dir * r * blurInvSize);
        float4 s = texMain.SampleLevel(samLin, suv, 0);
        float w = s.a;
        if (w > 1e-4) {
            float3 sRgb = saturate(s.rgb / w);
            float luma = dot(sRgb, float3(0.2126, 0.7152, 0.0722));
            // Realce óptico suave en altas luces especulares (discos de bokeh fotográfico).
            float hi = 1.0 + 1.15 * luma * luma * luma;
            accRgb += s.rgb * hi;
            accW   += w * hi;
            covSum += w;
        }
        dir = float2(dir.x * kCosGA - dir.y * kSinGA,
                     dir.x * kSinGA + dir.y * kCosGA);
    }

    if (accW > 1e-4) {
        float3 col = saturate(accRgb / accW);
        float cov = saturate(covSum * 0.20);
        return float4(col * cov, cov);
    }
    return float4(0.0, 0.0, 0.0, 0.0);
}

// Suavizado premultiplicado 3x3 tipo tienda que elimina grano discreto del disco en aperturas grandes.
float4 BokehPolishPS(VSOut input) : SV_Target {
    static const float2 offs[9] = {
        float2(-1.0, -1.0), float2(0.0, -1.0), float2(1.0, -1.0),
        float2(-1.0,  0.0), float2(0.0,  0.0), float2(1.0,  0.0),
        float2(-1.0,  1.0), float2(0.0,  1.0), float2(1.0,  1.0)
    };
    static const float wts[9] = {
        0.075, 0.125, 0.075,
        0.125, 0.200, 0.125,
        0.075, 0.125, 0.075
    };
    float2 stepUv = blurInvSize * 1.15;
    float3 accRgb = float3(0.0, 0.0, 0.0);
    float  accW   = 0.0;
    [unroll]
    for (int i = 0; i < 9; ++i) {
        float4 s = texMain.SampleLevel(samLin, saturate(input.uv + offs[i] * stepUv), 0);
        accRgb += s.rgb * wts[i];
        accW   += s.a   * wts[i];
    }
    return float4(accRgb, accW);
}

float4 CompositeEnhancePS(VSOut input) : SV_Target {
    float2 srcUv = ApplyFrameTransform(input.uv);
    float3 rawC = texMain.SampleLevel(samLin, srcUv, 0).rgb;
    float3 sharpC = rawC;

    if (sharpness > 0.001) {
        float3 n = texMain.SampleLevel(samLin, srcUv + float2(0.0, -texelSize.y), 0).rgb;
        float3 s = texMain.SampleLevel(samLin, srcUv + float2(0.0,  texelSize.y), 0).rgb;
        float3 l = texMain.SampleLevel(samLin, srcUv + float2(-texelSize.x, 0.0), 0).rgb;
        float3 r = texMain.SampleLevel(samLin, srcUv + float2( texelSize.x, 0.0), 0).rgb;
        float3 blur4 = 0.25 * (n + s + l + r);
        float3 minC = min(rawC, min(min(n, s), min(l, r)));
        float3 maxC = max(rawC, max(max(n, s), max(l, r)));
        sharpC = clamp(rawC + (rawC - blur4) * (sharpness * 1.15), minC - 0.06, maxC + 0.06);
    }

    float3 c = sharpC;
    if (blurMode > 0.5) {
        float4 bgSample = texBlur.SampleLevel(samLin, input.uv, 0);
        float3 bgCol = (bgSample.a > 0.01) ? saturate(bgSample.rgb / bgSample.a) : rawC;

        if (blurStyle < 0.5) {
            // Óptico Lente f/1.8 (0% IA): caída elíptica suave centrada en focusPt sobre disco bokeh.
            float2 d = input.uv - focusPt;
            d.x *= aspect * 0.82;
            d.y *= 0.68;
            float dist = length(d);
            float innerR = (blurMode > 2.5) ? 0.22 : ((blurMode > 1.5) ? 0.27 : 0.32);
            float blurAmt = smoothstep(innerR, innerR + 0.42, dist);
            c = lerp(bgCol, sharpC, 1.0 - blurAmt);
        } else {
            // Retrato IA RVM (512x288): alpha matting continuo [0..1] + foreground descontaminado (fgr).
            float2 mStep = float2(0.75 / 512.0, 0.75 / 288.0);
            float4 mC = texMatte.SampleLevel(samLin, input.uv, 0);
            float accA = mC.a;
            float accW = 1.0;
            static const float2 crossOff[4] = {
                float2(-1.0, 0.0), float2(1.0, 0.0), float2(0.0, -1.0), float2(0.0, 1.0)
            };
            [unroll]
            for (int k = 0; k < 4; ++k) {
                float2 qUv = saturate(input.uv + crossOff[k] * mStep);
                float3 qCol = texMain.SampleLevel(samLin, ApplyFrameTransform(qUv), 0).rgb;
                float3 cd = qCol - rawC;
                float wq = 0.35 * exp(-dot(cd, cd) * 28.0);
                accA += texMatte.SampleLevel(samLin, qUv, 0).a * wq;
                accW += wq;
            }
            // Mezcla alpha continua sin umbral duro (solo limpia suelo de cuantización 1.5%).
            float alpha = saturate((accA / accW - 0.015) / 0.97);
            // Aplica foreground descontaminado (fgr) en cabello/bordes semitransparentes; conserva 1080p nítido en el rostro.
            float fringeW = saturate(4.0 * alpha * (1.0 - alpha));
            float3 fgCol = lerp(sharpC, mC.rgb, fringeW * 0.85);
            c = lerp(bgCol, fgCol, alpha);
        }
    }

    c = pow(max(c, 1e-4), gammaInv);
    c = (c - 0.5) * contrast + 0.5 + brightness;
    c.r += warmth * 0.14;
    c.b -= warmth * 0.14;
    float luma = dot(c, float3(0.2126, 0.7152, 0.0722));
    c = saturate(lerp(float3(luma, luma, luma), c, saturation));

    return float4(saturate(c), 1.0);
}
)HLSL";

bool CompileShader(const char* entry, const char* target, ID3DBlob** outBlob) {
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
    Microsoft::WRL::ComPtr<ID3DBlob> err;
    HRESULT hr = D3DCompile(kShadersHlsl, std::strlen(kShadersHlsl), "ZeroCamFx.hlsl", nullptr, nullptr,
                            entry, target, flags, 0, outBlob, &err);
    return SUCCEEDED(hr);
}

} // namespace

GpuEffectsPipeline::~GpuEffectsPipeline() {
    Shutdown();
}

bool GpuEffectsPipeline::Initialize(ID3D11Device* dev, ID3D11DeviceContext* ctx, const std::wstring& optionalModelPath) {
    Shutdown();
    if (!dev || !ctx) return false;
    device_ = dev;
    context_ = ctx;

    Microsoft::WRL::ComPtr<ID3DBlob> vsBlob, dsBlob, bokehBlob, polishBlob, compBlob;
    if (!CompileShader("MainVS", "vs_5_0", &vsBlob) ||
        !CompileShader("DownsampleTransformPS", "ps_5_0", &dsBlob) ||
        !CompileShader("BokehDiscPS", "ps_5_0", &bokehBlob) ||
        !CompileShader("BokehPolishPS", "ps_5_0", &polishBlob) ||
        !CompileShader("CompositeEnhancePS", "ps_5_0", &compBlob)) {
        return false;
    }

    if (FAILED(device_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vsFullscreen_)) ||
        FAILED(device_->CreatePixelShader(dsBlob->GetBufferPointer(), dsBlob->GetBufferSize(), nullptr, &psDownsample_)) ||
        FAILED(device_->CreatePixelShader(bokehBlob->GetBufferPointer(), bokehBlob->GetBufferSize(), nullptr, &psBokehDisc_)) ||
        FAILED(device_->CreatePixelShader(polishBlob->GetBufferPointer(), polishBlob->GetBufferSize(), nullptr, &psBokehPolish_)) ||
        FAILED(device_->CreatePixelShader(compBlob->GetBufferPointer(), compBlob->GetBufferSize(), nullptr, &psComposite_))) {
        Shutdown();
        return false;
    }

    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = sizeof(FxConstants);
    cbDesc.Usage = D3D11_USAGE_DEFAULT;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device_->CreateBuffer(&cbDesc, nullptr, &constantBuffer_))) {
        Shutdown();
        return false;
    }

    D3D11_SAMPLER_DESC sDesc{};
    sDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sDesc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device_->CreateSamplerState(&sDesc, &samplerLinear_))) {
        Shutdown();
        return false;
    }

    D3D11_RASTERIZER_DESC rsDesc{};
    rsDesc.FillMode = D3D11_FILL_SOLID;
    rsDesc.CullMode = D3D11_CULL_NONE;
    if (FAILED(device_->CreateRasterizerState(&rsDesc, &rasterNoCull_))) {
        Shutdown();
        return false;
    }

    if (FAILED(device_->QueryInterface(IID_PPV_ARGS(&videoDev_))) ||
        FAILED(context_->QueryInterface(IID_PPV_ARGS(&videoCtx_)))) {
        Shutdown();
        return false;
    }

    // Texturas fijas RVM (512x288 BGRA8 entrada + STAGING + matte BGRA8 fgr/pha).
    D3D11_TEXTURE2D_DESC segDesc{};
    segDesc.Width = PersonSegmenter::kModelW;
    segDesc.Height = PersonSegmenter::kModelH;
    segDesc.MipLevels = 1;
    segDesc.ArraySize = 1;
    segDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    segDesc.SampleDesc.Count = 1;
    segDesc.Usage = D3D11_USAGE_DEFAULT;
    segDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(device_->CreateTexture2D(&segDesc, nullptr, &segBgra_)) ||
        FAILED(device_->CreateRenderTargetView(segBgra_.Get(), nullptr, &segRtv_))) {
        Shutdown();
        return false;
    }

    segDesc.Usage = D3D11_USAGE_STAGING;
    segDesc.BindFlags = 0;
    segDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device_->CreateTexture2D(&segDesc, nullptr, &segStaging_))) {
        Shutdown();
        return false;
    }

    std::vector<uint8_t> initMatte(static_cast<size_t>(PersonSegmenter::kModelW) * PersonSegmenter::kModelH * 4u, 255u);
    D3D11_TEXTURE2D_DESC mDesc{};
    mDesc.Width = PersonSegmenter::kModelW;
    mDesc.Height = PersonSegmenter::kModelH;
    mDesc.MipLevels = 1;
    mDesc.ArraySize = 1;
    mDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    mDesc.SampleDesc.Count = 1;
    mDesc.Usage = D3D11_USAGE_DEFAULT;
    mDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA mInit{ initMatte.data(), PersonSegmenter::kModelW * 4u, 0 };
    if (FAILED(device_->CreateTexture2D(&mDesc, &mInit, &matteTex_)) ||
        FAILED(device_->CreateShaderResourceView(matteTex_.Get(), nullptr, &matteSrv_))) {
        Shutdown();
        return false;
    }

    segmenter_.Initialize(optionalModelPath);
    return true;
}

void GpuEffectsPipeline::Shutdown() {
    segmenter_.Shutdown();
    matteSrv_.Reset();
    matteTex_.Reset();
    segStaging_.Reset();
    segRtv_.Reset();
    segBgra_.Reset();
    blurSrvB_.Reset();
    blurRtvB_.Reset();
    blurTexB_.Reset();
    blurSrvA_.Reset();
    blurRtvA_.Reset();
    blurTexA_.Reset();
    previewNv12OutView_.Reset();
    previewNv12_.Reset();
    outBgraInView_.Reset();
    outBgraRtv_.Reset();
    outBgra_.Reset();
    workBgraVpOut_.Reset();
    workBgraSrv_.Reset();
    workBgra_.Reset();
    outVp_.Reset();
    outEnum_.Reset();
    inCopyNv12_.Reset();
    inVp_.Reset();
    inEnum_.Reset();
    videoCtx_.Reset();
    videoDev_.Reset();
    rasterNoCull_.Reset();
    samplerLinear_.Reset();
    constantBuffer_.Reset();
    psComposite_.Reset();
    psBokehPolish_.Reset();
    psBokehDisc_.Reset();
    psDownsample_.Reset();
    vsFullscreen_.Reset();
    device_ = nullptr;
    context_ = nullptr;
    resSrcTexW_ = resSrcTexH_ = resW_ = resH_ = blurW_ = blurH_ = 0;
    supportsInRotation_ = false;
    matteGen_ = 0;
    frameCounter_ = 0;
    hasValidMatte_ = false;
    prevUseAiBlur_ = false;
    prevMirrorH_ = false;
    prevRotCode_ = 0;
}

void GpuEffectsPipeline::ResetAiState() noexcept {
    if (prevUseAiBlur_ || hasValidMatte_) {
        segmenter_.ResetTemporalState();
    }
    prevUseAiBlur_ = false;
    hasValidMatte_ = false;
}

bool GpuEffectsPipeline::EnsureResources(uint32_t srcTexW, uint32_t srcTexH, uint32_t w, uint32_t h) {
    if (!device_ || !videoDev_ || srcTexW == 0 || srcTexH == 0 || w == 0 || h == 0) return false;
    if (workBgra_ && resSrcTexW_ == srcTexW && resSrcTexH_ == srcTexH && resW_ == w && resH_ == h) {
        return true;
    }

    if ((resW_ != 0 && resH_ != 0) && (resW_ != w || resH_ != h)) {
        ResetAiState();
    }

    blurSrvB_.Reset(); blurRtvB_.Reset(); blurTexB_.Reset();
    blurSrvA_.Reset(); blurRtvA_.Reset(); blurTexA_.Reset();
    previewNv12OutView_.Reset(); previewNv12_.Reset();
    outBgraInView_.Reset(); outBgraRtv_.Reset(); outBgra_.Reset();
    workBgraVpOut_.Reset(); workBgraSrv_.Reset(); workBgra_.Reset();
    outVp_.Reset(); outEnum_.Reset();
    inCopyNv12_.Reset();
    inVp_.Reset(); inEnum_.Reset();

    auto makeBgra = [&](uint32_t tw, uint32_t th, UINT bind, ID3D11Texture2D** outTex) -> bool {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = tw;
        d.Height = th;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = bind;
        return SUCCEEDED(device_->CreateTexture2D(&d, nullptr, outTex));
    };

    if (!makeBgra(w, h, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, &workBgra_) ||
        FAILED(device_->CreateShaderResourceView(workBgra_.Get(), nullptr, &workBgraSrv_)) ||
        !makeBgra(w, h, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, &outBgra_) ||
        FAILED(device_->CreateRenderTargetView(outBgra_.Get(), nullptr, &outBgraRtv_))) {
        return false;
    }

    // Bokeh de disco óptico a 1/2 resolución (960x540 en 1080p, 640x360 en 720p).
    blurW_ = std::max<uint32_t>(320u, (w / 2u) & ~1u);
    blurH_ = std::max<uint32_t>(180u, (h / 2u) & ~1u);
    if (!makeBgra(blurW_, blurH_, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, &blurTexA_) ||
        FAILED(device_->CreateRenderTargetView(blurTexA_.Get(), nullptr, &blurRtvA_)) ||
        FAILED(device_->CreateShaderResourceView(blurTexA_.Get(), nullptr, &blurSrvA_)) ||
        !makeBgra(blurW_, blurH_, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, &blurTexB_) ||
        FAILED(device_->CreateRenderTargetView(blurTexB_.Get(), nullptr, &blurRtvB_)) ||
        FAILED(device_->CreateShaderResourceView(blurTexB_.Get(), nullptr, &blurSrvB_))) {
        return false;
    }

    D3D11_TEXTURE2D_DESC nvDesc{};
    nvDesc.Width = w;
    nvDesc.Height = h;
    nvDesc.MipLevels = 1;
    nvDesc.ArraySize = 1;
    nvDesc.Format = DXGI_FORMAT_NV12;
    nvDesc.SampleDesc.Count = 1;
    nvDesc.Usage = D3D11_USAGE_DEFAULT;
    nvDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&nvDesc, nullptr, &previewNv12_))) {
        return false;
    }

    // VideoProcessor de entrada: NV12 (srcTexW x srcTexH) -> BGRA8 (w x h)
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC inCd{};
    inCd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    inCd.InputWidth = srcTexW;
    inCd.InputHeight = srcTexH;
    inCd.OutputWidth = w;
    inCd.OutputHeight = h;
    inCd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    if (FAILED(videoDev_->CreateVideoProcessorEnumerator(&inCd, &inEnum_)) ||
        FAILED(videoDev_->CreateVideoProcessor(inEnum_.Get(), 0, &inVp_))) {
        return false;
    }
    D3D11_VIDEO_PROCESSOR_CAPS caps{};
    supportsInRotation_ = SUCCEEDED(inEnum_->GetVideoProcessorCaps(&caps)) &&
                          ((caps.FeatureCaps & D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_ROTATION) != 0);

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovDesc{};
    ovDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    if (FAILED(videoDev_->CreateVideoProcessorOutputView(workBgra_.Get(), inEnum_.Get(), &ovDesc, &workBgraVpOut_))) {
        return false;
    }

    // VideoProcessor de salida: BGRA8 (w x h) -> NV12 (w x h)
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC outCd{};
    outCd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    outCd.InputWidth = w;
    outCd.InputHeight = h;
    outCd.OutputWidth = w;
    outCd.OutputHeight = h;
    outCd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    if (FAILED(videoDev_->CreateVideoProcessorEnumerator(&outCd, &outEnum_)) ||
        FAILED(videoDev_->CreateVideoProcessor(outEnum_.Get(), 0, &outVp_))) {
        return false;
    }

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivDesc{};
    ivDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    if (FAILED(videoDev_->CreateVideoProcessorInputView(outBgra_.Get(), outEnum_.Get(), &ivDesc, &outBgraInView_)) ||
        FAILED(videoDev_->CreateVideoProcessorOutputView(previewNv12_.Get(), outEnum_.Get(), &ovDesc, &previewNv12OutView_))) {
        return false;
    }

    resSrcTexW_ = srcTexW;
    resSrcTexH_ = srcTexH;
    resW_ = w;
    resH_ = h;
    return true;
}

void GpuEffectsPipeline::DrawFullscreen(
    ID3D11PixelShader* ps,
    ID3D11ShaderResourceView* const* srvs,
    uint32_t srvCount,
    ID3D11RenderTargetView* rtv,
    uint32_t vpW,
    uint32_t vpH) {
    D3D11_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(vpW), static_cast<float>(vpH), 0.0f, 1.0f };
    context_->RSSetViewports(1, &vp);
    context_->RSSetState(rasterNoCull_.Get());
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    context_->OMSetDepthStencilState(nullptr, 0);
    context_->OMSetRenderTargets(1, &rtv, nullptr);

    context_->VSSetShader(vsFullscreen_.Get(), nullptr, 0);
    context_->PSSetShader(ps, nullptr, 0);
    ID3D11Buffer* cbs[1] = { constantBuffer_.Get() };
    context_->PSSetConstantBuffers(0, 1, cbs);
    ID3D11SamplerState* sams[1] = { samplerLinear_.Get() };
    context_->PSSetSamplers(0, 1, sams);
    if (srvs && srvCount > 0) {
        context_->PSSetShaderResources(0, srvCount, srvs);
    }

    context_->Draw(3, 0);

    ID3D11ShaderResourceView* nullSrvs[4]{};
    context_->PSSetShaderResources(0, 4, nullSrvs);
    context_->OMSetRenderTargets(0, nullptr, nullptr);
}

bool GpuEffectsPipeline::Process(
    ID3D11Texture2D* srcNv12,
    UINT srcSubIdx,
    uint32_t w,
    uint32_t h,
    uint8_t rotCode,
    const VideoEffectsConfig& cfg,
    ID3D11Texture2D* dstRingNv12,
    ID3D11Texture2D** outPreviewNv12) {
    if (!srcNv12 || !dstRingNv12 || !device_ || !context_) return false;

    D3D11_TEXTURE2D_DESC srcDesc{};
    srcNv12->GetDesc(&srcDesc);
    if (!EnsureResources(srcDesc.Width, srcDesc.Height, w, h)) {
        return false;
    }

    // 1. NV12 -> BGRA8 + rotación HW hacia workBgra_
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivDesc{};
    ivDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ivDesc.Texture2D.ArraySlice = srcSubIdx;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> inView;
    if (FAILED(videoDev_->CreateVideoProcessorInputView(srcNv12, inEnum_.Get(), &ivDesc, &inView))) {
        if (!inCopyNv12_) {
            D3D11_TEXTURE2D_DESC cDesc{};
            cDesc.Width = srcDesc.Width;
            cDesc.Height = srcDesc.Height;
            cDesc.MipLevels = 1;
            cDesc.ArraySize = 1;
            cDesc.Format = DXGI_FORMAT_NV12;
            cDesc.SampleDesc.Count = 1;
            cDesc.Usage = D3D11_USAGE_DEFAULT;
            cDesc.BindFlags = 0;
            if (FAILED(device_->CreateTexture2D(&cDesc, nullptr, &inCopyNv12_))) {
                return false;
            }
        }
        context_->CopySubresourceRegion(inCopyNv12_.Get(), 0, 0, 0, 0, srcNv12, srcSubIdx, nullptr);
        ivDesc.Texture2D.ArraySlice = 0;
        if (FAILED(videoDev_->CreateVideoProcessorInputView(inCopyNv12_.Get(), inEnum_.Get(), &ivDesc, &inView))) {
            return false;
        }
    }

    D3D11_VIDEO_COLOR blackBg{};
    blackBg.RGBA = { 0.0f, 0.0f, 0.0f, 1.0f };
    videoCtx_->VideoProcessorSetOutputBackgroundColor(inVp_.Get(), FALSE, &blackBg);
    videoCtx_->VideoProcessorSetStreamFrameFormat(inVp_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    RECT srcRc{ 0, 0, static_cast<LONG>(w), static_cast<LONG>(h) };
    videoCtx_->VideoProcessorSetStreamSourceRect(inVp_.Get(), 0, TRUE, &srcRc);

    const uint8_t rot = rotCode & 0x3u;
    const bool applyRot = supportsInRotation_ && rot != 0;
    RECT dstRc{ 0, 0, static_cast<LONG>(w), static_cast<LONG>(h) };
    if (applyRot && (rot == 1 || rot == 3) && w > h) {
        const uint32_t scaledW = std::max<uint32_t>(2u, ((h * h) / w) & ~1u);
        const uint32_t offX = ((w - scaledW) / 2u) & ~1u;
        dstRc = { static_cast<LONG>(offX), 0, static_cast<LONG>(offX + scaledW), static_cast<LONG>(h) };
    }
    videoCtx_->VideoProcessorSetStreamDestRect(inVp_.Get(), 0, TRUE, &dstRc);
    videoCtx_->VideoProcessorSetStreamRotation(inVp_.Get(), 0, applyRot ? TRUE : FALSE,
        static_cast<D3D11_VIDEO_PROCESSOR_ROTATION>(applyRot ? rot : 0));

    D3D11_VIDEO_PROCESSOR_STREAM st{};
    st.Enable = TRUE;
    st.pInputSurface = inView.Get();
    if (FAILED(videoCtx_->VideoProcessorBlt(inVp_.Get(), workBgraVpOut_.Get(), 0, 1, &st))) {
        return false;
    }

    VideoEffectsConfig safeCfg = cfg;
    ClampEffectsConfig(safeCfg);

    const float resScale = std::max(0.55f, static_cast<float>(blurH_) / 540.0f);
    const float baseBokehPx = (safeCfg.blurMode >= 3) ? 17.0f : ((safeCfg.blurMode == 2) ? 11.5f : 6.5f);

    FxConstants cb{};
    cb.texelX = 1.0f / static_cast<float>(w);
    cb.texelY = 1.0f / static_cast<float>(h);
    cb.blurInvX = 1.0f / static_cast<float>(blurW_);
    cb.blurInvY = 1.0f / static_cast<float>(blurH_);
    cb.brightness = safeCfg.brightness;
    cb.contrast = safeCfg.contrast;
    cb.saturation = safeCfg.saturation;
    cb.warmth = safeCfg.warmth;
    cb.gammaInv = 1.0f / std::max(0.5f, safeCfg.gamma);
    cb.sharpness = safeCfg.sharpness;
    cb.digitalZoom = safeCfg.digitalZoom;
    cb.mirrorH = safeCfg.mirrorH ? 1.0f : 0.0f;
    cb.blurStyle = static_cast<float>(safeCfg.blurStyle);
    cb.focusX = safeCfg.focusX;
    cb.focusY = safeCfg.focusY;
    cb.aspect = static_cast<float>(w) / static_cast<float>(std::max<uint32_t>(1u, h));
    cb.bokehRadiusPx = baseBokehPx * resScale;

    // 2. Desenfoque Óptico f/1.8 (0% IA) o Retrato IA RVM 512x288 bajo demanda (0% cuando blurMode == 0).
    ++frameCounter_;
    const bool blurActive = (safeCfg.blurMode > 0);
    const bool useAiBlur = blurActive && (safeCfg.blurStyle == 1) && segmenter_.IsAvailable();
    const bool useOpticalBlur = blurActive && (safeCfg.blurStyle == 0);

    if (useAiBlur) {
        const bool geometryChanged = (safeCfg.mirrorH != prevMirrorH_) || (rot != prevRotCode_);
        if (!prevUseAiBlur_ || geometryChanged) {
            segmenter_.ResetTemporalState();
            hasValidMatte_ = false;
        }
        prevMirrorH_ = safeCfg.mirrorH;
        prevRotCode_ = rot;

        // Reduce y transforma el cuadro actual a 512x288 para RVM.
        cb.bgPremul = 0.0f;
        context_->UpdateSubresource(constantBuffer_.Get(), 0, nullptr, &cb, 0, 0);
        ID3D11ShaderResourceView* workSrvArr[1] = { workBgraSrv_.Get() };
        DrawFullscreen(psDownsample_.Get(), workSrvArr, 1, segRtv_.Get(), PersonSegmenter::kModelW, PersonSegmenter::kModelH);

        if (!segmenter_.IsBusy()) {
            context_->CopyResource(segStaging_.Get(), segBgra_.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(context_->Map(segStaging_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                segmenter_.SubmitBgra512x288(static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch, safeCfg.blurUseGpu);
                context_->Unmap(segStaging_.Get(), 0);
            }
        }

        constexpr size_t expectedBytes = static_cast<size_t>(PersonSegmenter::kModelW) * PersonSegmenter::kModelH * 4u;
        if (segmenter_.CopyLatestMatte(matteCpu_, matteGen_) && matteCpu_.size() == expectedBytes) {
            context_->UpdateSubresource(matteTex_.Get(), 0, nullptr, matteCpu_.data(), PersonSegmenter::kModelW * 4u, 0);
            hasValidMatte_ = true;
        }
    } else if (prevUseAiBlur_ || hasValidMatte_) {
        ResetAiState();
    }
    prevUseAiBlur_ = useAiBlur;

    const bool runBlurPasses = useOpticalBlur || (useAiBlur && hasValidMatte_);
    if (runBlurPasses) {
        // Paso A: downsample a 1/2 resolución (960x540 en 1080p) aislando el fondo con alpha continuo de RVM.
        cb.bgPremul = useAiBlur ? 1.0f : 0.0f;
        context_->UpdateSubresource(constantBuffer_.Get(), 0, nullptr, &cb, 0, 0);
        ID3D11ShaderResourceView* dsSrvs[3] = { workBgraSrv_.Get(), nullptr, matteSrv_.Get() };
        DrawFullscreen(psDownsample_.Get(), dsSrvs, useAiBlur ? 3u : 1u, blurRtvA_.Get(), blurW_, blurH_);

        // Paso B: Bokeh de disco óptico por espiral de ángulo áureo (blurTexA_ -> blurTexB_).
        ID3D11ShaderResourceView* sA[1] = { blurSrvA_.Get() };
        DrawFullscreen(psBokehDisc_.Get(), sA, 1, blurRtvB_.Get(), blurW_, blurH_);

        // Paso C: Pulido premultiplicado 3x3 tipo tienda (blurTexB_ -> blurTexA_).
        ID3D11ShaderResourceView* sB[1] = { blurSrvB_.Get() };
        DrawFullscreen(psBokehPolish_.Get(), sB, 1, blurRtvA_.Get(), blurW_, blurH_);
    }

    // 3. Composición final (alpha continuo + foreground descontaminado fgr) + mejora de color -> outBgra_
    cb.blurMode = runBlurPasses ? static_cast<float>(safeCfg.blurMode) : 0.0f;
    context_->UpdateSubresource(constantBuffer_.Get(), 0, nullptr, &cb, 0, 0);
    ID3D11ShaderResourceView* compSrvs[3] = { workBgraSrv_.Get(), blurSrvA_.Get(), matteSrv_.Get() };
    DrawFullscreen(psComposite_.Get(), compSrvs, 3, outBgraRtv_.Get(), w, h);

    // 4. BGRA8 -> NV12 (previewNv12_) -> copiar al slot del ring compartido
    videoCtx_->VideoProcessorSetStreamFrameFormat(outVp_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    RECT fullRc{ 0, 0, static_cast<LONG>(w), static_cast<LONG>(h) };
    videoCtx_->VideoProcessorSetStreamSourceRect(outVp_.Get(), 0, TRUE, &fullRc);
    videoCtx_->VideoProcessorSetStreamDestRect(outVp_.Get(), 0, TRUE, &fullRc);
    D3D11_VIDEO_PROCESSOR_STREAM outSt{};
    outSt.Enable = TRUE;
    outSt.pInputSurface = outBgraInView_.Get();
    if (FAILED(videoCtx_->VideoProcessorBlt(outVp_.Get(), previewNv12OutView_.Get(), 0, 1, &outSt))) {
        return false;
    }

    context_->CopySubresourceRegion(dstRingNv12, 0, 0, 0, 0, previewNv12_.Get(), 0, nullptr);
    if (outPreviewNv12) {
        *outPreviewNv12 = previewNv12_.Get();
        (*outPreviewNv12)->AddRef();
    }
    return true;
}

} // namespace zerocam
