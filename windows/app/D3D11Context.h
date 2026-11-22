#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <mfidl.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include "GpuEffectsPipeline.h"
#include "VideoEffects.h"
#include "../shared/ZeroCamIpc.h"

namespace zerocam {

// Contexto D3D11 centralizado: gestiona el dispositivo HW, IMFDXGIDeviceManager y el ring compartido NV12.
class D3D11Context {
public:
    D3D11Context() = default;
    ~D3D11Context();

    D3D11Context(const D3D11Context&) = delete;
    D3D11Context& operator=(const D3D11Context&) = delete;

    // Inicializa D3D11 con soporte de video, IMFDXGIDeviceManager y el canal IPC.
    bool Initialize(bool allowTempIpcFallback = true);
    // Libera recursos D3D11 y marca el productor como inactivo en IPC.
    void Shutdown();
    // Actualiza el estado activo/standby del productor para cámaras virtuales persistentes.
    void SetProducerActive(bool active);

    // Publica un frame NV12 directamente VRAM->VRAM (o aplica filtros GPU D3D11 + IA si están activos).
    bool PublishGpuFrame(
        ID3D11Texture2D* srcTex,
        UINT subresourceIndex,
        uint32_t visibleWidth,
        uint32_t visibleHeight,
        int64_t ptsHns,
        uint32_t fpsNum,
        uint32_t fpsDen,
        uint8_t rotationCode = 0,
        ID3D11Texture2D** outPublishedRingTex = nullptr,
        bool* outAlreadyRotated = nullptr);

    // Configura los filtros GPU y segmentación IA aplicados antes de publicar en el ring compartido.
    void SetEffectsConfig(const VideoEffectsConfig& cfg);
    VideoEffectsConfig GetEffectsConfig() const;
    void SetOptionalModelPath(const std::wstring& path);
    double LastAiInferenceMs() const noexcept;
    bool IsAiBlurAvailable() const noexcept;
    /// Indica si la segmentación IA activa está ejecutando sobre DirectML GPU (false en CPU o respaldo).
    bool IsAiUsingGpu() const noexcept;

    // Publica un frame NV12 que proviene de memoria RAM (fallback si un MFT no entrega DXGI).
    bool PublishCpuNv12Frame(
        const uint8_t* data,
        uint32_t stride,
        uint32_t width,
        uint32_t height,
        int64_t ptsHns,
        uint32_t fpsNum,
        uint32_t fpsDen);

    // Genera y publica un patrón de prueba NV12 animado para verificar cámara sin móvil conectado.
    bool PublishTestPattern(
        uint32_t width,
        uint32_t height,
        uint64_t frameIdx,
        uint32_t fpsNum,
        uint32_t fpsDen,
        ID3D11Texture2D** outGpuTex = nullptr);

    ID3D11Device* Device() const noexcept { return device_.Get(); }
    ID3D11DeviceContext* Context() const noexcept { return context_.Get(); }
    IMFDXGIDeviceManager* DxgiManager() const noexcept { return dxgiManager_.Get(); }
    ZeroCamSharedMemory* Ipc() noexcept { return ipc_.Data(); }
    const std::wstring& AdapterName() const noexcept { return adapterName_; }
    LUID AdapterLuid() const noexcept { return adapterLuid_; }
    bool UsesKeyedMutex() const noexcept { return usesKeyedMutex_; }
    bool SupportsHwRotation() const noexcept { return supportsHwRotation_; }
    std::mutex& ContextMutex() noexcept { return mutex_; }

private:
    // (Re)crea el ring de 3 texturas NV12 compartidas cuando cambia la resolución.
    bool EnsureSharedRing(uint32_t width, uint32_t height);
    // Inicializa perezosamente ID3D11VideoProcessor para rotación HW NV12->NV12 sin 3D shaders.
    bool EnsureRotationProcessor(uint32_t srcW, uint32_t srcH, uint32_t dstW, uint32_t dstH);
    // Ejecuta la rotación HW en bloque de video hacia ringTextures_[slot].
    bool BlitRotatedHw(ID3D11Texture2D* srcTex, UINT subresourceIdx, ID3D11Texture2D* dstTex, uint32_t w, uint32_t h, uint8_t rotCode);
    // (Re)crea la textura staging bajo demanda solo si el consumidor solicitó fallback CPU.
    bool EnsureStagingTexture(uint32_t width, uint32_t height);
    // Copia VRAM->staging bajo KeyedMutex antes de liberarlo (sin Map bajo lock compartido).
    bool StageSlotToCpuTexture(ID3D11Texture2D* gpuTex, uint32_t width, uint32_t height);
    // Lee staging->cpuNv12Buffer fuera del KeyedMutex compartido.
    void ReadStagedCpuFallback(uint32_t width, uint32_t height);

    std::mutex mutex_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> dxgiManager_;
    UINT resetToken_ = 0;

    std::wstring adapterName_;
    LUID adapterLuid_{};
    SharedIpcChannel ipc_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> ringTextures_[kRingSlots];
    Microsoft::WRL::ComPtr<IDXGIKeyedMutex> ringMutexes_[kRingSlots];
    HANDLE ringHandles_[kRingSlots]{};
    bool usesKeyedMutex_ = false;
    uint32_t ringW_ = 0;
    uint32_t ringH_ = 0;
    uint32_t nextSlot_ = 0;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> stagingTex_;
    uint32_t stagingW_ = 0;
    uint32_t stagingH_ = 0;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> testUploadTex_;
    std::vector<uint8_t> testNv12Scratch_;
    uint32_t testW_ = 0;
    uint32_t testH_ = 0;

    Microsoft::WRL::ComPtr<ID3D11VideoDevice> videoDevice_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> videoContext_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> rotEnum_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> rotVp_;
    uint32_t rotSrcW_ = 0;
    uint32_t rotSrcH_ = 0;
    uint32_t rotDstW_ = 0;
    uint32_t rotDstH_ = 0;
    bool supportsHwRotation_ = false;

    mutable std::mutex effectsMutex_;
    VideoEffectsConfig effectsCfg_{};
    std::wstring optionalModelPath_;
    GpuEffectsPipeline effectsPipeline_;
    bool effectsPipelineInitialized_ = false;
    bool effectsPipelineInitAttempted_ = false;
};

} // namespace zerocam
