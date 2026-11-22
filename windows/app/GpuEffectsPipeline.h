#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include <vector>
#include "PersonSegmenter.h"
#include "VideoEffects.h"

namespace zerocam {

/// Pipeline de shaders D3D11 + segmentación IA WinML sobre NV12 -> BGRA8 -> NV12.
class GpuEffectsPipeline {
public:
    GpuEffectsPipeline() = default;
    ~GpuEffectsPipeline();

    GpuEffectsPipeline(const GpuEffectsPipeline&) = delete;
    GpuEffectsPipeline& operator=(const GpuEffectsPipeline&) = delete;

    /// Compila los shaders HLSL, crea estados D3D11 y prepara el segmentador de retrato.
    bool Initialize(ID3D11Device* dev, ID3D11DeviceContext* ctx, const std::wstring& optionalModelPath = L"");

    /// Libera recursos GPU y detiene la inferencia IA en segundo plano.
    void Shutdown();

    /// Reinicia el estado temporal IA y descarta el último matte al desactivar el desenfoque o pasar a bypass.
    void ResetAiState() noexcept;

    /// Procesa un frame NV12 (con rotación opcional), escribe el resultado al ring y retorna textura para vista previa.
    bool Process(
        ID3D11Texture2D* srcNv12,
        UINT srcSubIdx,
        uint32_t w,
        uint32_t h,
        uint8_t rotCode,
        const VideoEffectsConfig& cfg,
        ID3D11Texture2D* dstRingNv12,
        ID3D11Texture2D** outPreviewNv12);

    bool IsAiAvailable() const noexcept { return segmenter_.IsAvailable(); }
    bool IsAiUsingGpu() const noexcept { return segmenter_.IsUsingGpu(); }
    /// Indica si existe un matte IA vigente listo para componer el bokeh.
    bool HasValidMatte() const noexcept { return hasValidMatte_; }
    double LastAiMs() const noexcept { return segmenter_.LastInferenceMs(); }

private:
    bool EnsureResources(uint32_t srcTexW, uint32_t srcTexH, uint32_t w, uint32_t h);
    void DrawFullscreen(ID3D11PixelShader* ps, ID3D11ShaderResourceView* const* srvs, uint32_t srvCount,
                        ID3D11RenderTargetView* rtv, uint32_t vpW, uint32_t vpH);

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> vsFullscreen_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> psDownsample_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> psBokehDisc_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> psBokehPolish_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> psComposite_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constantBuffer_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> samplerLinear_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> rasterNoCull_;

    Microsoft::WRL::ComPtr<ID3D11VideoDevice> videoDev_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> videoCtx_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> inEnum_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> inVp_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> outEnum_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> outVp_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> outBgraInView_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> previewNv12OutView_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> workBgra_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> workBgraSrv_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> workBgraVpOut_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> outBgra_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> outBgraRtv_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> previewNv12_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> blurTexA_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> blurRtvA_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> blurSrvA_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> blurTexB_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> blurRtvB_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> blurSrvB_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> segBgra_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> segRtv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> segStaging_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> matteTex_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> matteSrv_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> inCopyNv12_;

    uint32_t resSrcTexW_ = 0;
    uint32_t resSrcTexH_ = 0;
    uint32_t resW_ = 0;
    uint32_t resH_ = 0;
    uint32_t blurW_ = 0;
    uint32_t blurH_ = 0;
    bool supportsInRotation_ = false;

    PersonSegmenter segmenter_;
    std::vector<uint8_t> matteCpu_;
    uint64_t matteGen_ = 0;
    uint64_t frameCounter_ = 0;
    bool hasValidMatte_ = false;
    bool prevUseAiBlur_ = false;
    bool prevMirrorH_ = false;
    uint8_t prevRotCode_ = 0;
};

} // namespace zerocam
