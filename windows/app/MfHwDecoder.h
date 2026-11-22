#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <icodecapi.h>
#include <codecapi.h>
#include <wrl/client.h>
#include <cstdint>
#include <functional>
#include "D3D11Context.h"
#include "PreviewRenderer.h"

namespace zerocam {

// Decodificador H.264 por hardware mediante Media Foundation MFT (D3D11VA zero-copy NV12).
class MfHwDecoder {
public:
    MfHwDecoder() = default;
    ~MfHwDecoder();

    // Inicializa CLSID_MSH264DecoderMFT en modo Low-Latency y conectado al IMFDXGIDeviceManager.
    bool Initialize(D3D11Context* d3dCtx, PreviewRenderer* preview = nullptr);
    // Cierra y libera el transformador de Media Foundation.
    void Shutdown();

    // Envía un Access Unit H.264 Annex-B y publica cada frame decodificado (o salta publicación si skipPublish=true).
    bool DecodeAccessUnit(
        const uint8_t* data,
        size_t size,
        int64_t ptsUs,
        uint32_t fpsNum = 30,
        uint32_t fpsDen = 1,
        uint8_t rotationCode = 0,
        bool skipPublish = false);
    // Drena frames pendientes al finalizar un archivo o flujo.
    void DrainAll(uint32_t fpsNum = 30, uint32_t fpsDen = 1);

    uint64_t DecodedFrames() const noexcept { return decodedFrames_; }
    uint64_t HwZeroCopyFrames() const noexcept { return hwZeroCopyFrames_; }
    uint64_t CpuFallbackFrames() const noexcept { return cpuFallbackFrames_; }
    uint32_t Width() const noexcept { return width_; }
    uint32_t Height() const noexcept { return height_; }
    bool IsHwAccelerated() const noexcept { return hwAccelerated_; }

private:
    // Selecciona NV12 como tipo de salida del MFT y lee el tamaño visible sin padding de macrobloques.
    bool ConfigureOutputType();
    // Extrae todas las muestras listas del MFT tras ProcessInput o Drain.
    bool PullOutputs(uint32_t fpsNum, uint32_t fpsDen, bool skipPublish = false);

    D3D11Context* d3dCtx_ = nullptr;
    PreviewRenderer* preview_ = nullptr;
    Microsoft::WRL::ComPtr<IMFTransform> decoder_;

    bool hwAccelerated_ = false;
    bool initialized_ = false;
    uint32_t width_ = 1280;
    uint32_t height_ = 720;
    uint8_t currentRotationCode_ = 0;
    uint64_t decodedFrames_ = 0;
    uint64_t hwZeroCopyFrames_ = 0;
    uint64_t cpuFallbackFrames_ = 0;
};

} // namespace zerocam
