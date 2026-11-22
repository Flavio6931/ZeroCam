#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include "D3D11Context.h"

namespace zerocam {

// Renderizador de vista previa bajo demanda: consume 0% GPU 3D cuando está desactivado o minimizado.
class PreviewRenderer {
public:
    PreviewRenderer() = default;
    ~PreviewRenderer();

    // Asocia el HWND contenedor del panel de video sin crear SwapChain hasta que se active.
    void Attach(D3D11Context* d3dCtx, HWND hwnd);
    // Activa o desactiva la vista previa; al desactivar destruye SwapChain y VideoProcessor para liberar VRAM.
    void SetEnabled(bool enabled);
    bool IsEnabled() const noexcept { return enabled_.load(std::memory_order_acquire); }
    // Notifica redimensionado de la ventana para ajustar la cadena de intercambio si está activa.
    void Resize(uint32_t width, uint32_t height);
    // Presenta una textura NV12 por bloque de video D3D11 con rotación HW; retorna false si falla el blit o vista de entrada.
    bool PresentNv12(
        ID3D11Texture2D* srcTex,
        UINT subresourceIdx,
        uint32_t srcW,
        uint32_t srcH,
        uint8_t rotationCode = 0,
        bool mirrorH = false,
        uint8_t effectiveRotForMapping = 0);

    // Muestra una retícula azul breve de confirmación en las coordenadas cliente del clic.
    void ShowFocusFeedback(int clientX, int clientY);

    // Traduce un clic cliente dentro de la vista previa a coordenadas normalizadas [0..1] del sensor.
    bool MapClientPointToSensorNorm(int clientX, int clientY, float& outNormX, float& outNormY);

    // Traduce un clic cliente al espacio UV [0..1] de salida visible (centro óptico de desenfoque).
    bool MapClientPointToDisplayUv(int clientX, int clientY, float& outU, float& outV);

private:
    // Crea perezosamente el SwapChain DXGI y VideoProcessor solo cuando el preview es visible.
    bool EnsurePipeline(uint32_t srcW, uint32_t srcH, uint32_t dstW, uint32_t dstH);
    // Libera recursos D3D11 del preview.
    void ReleasePipeline();

    D3D11Context* d3dCtx_ = nullptr;
    HWND hwnd_ = nullptr;
    std::atomic<bool> enabled_{ false };
    std::mutex mutex_;

    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain_;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> videoDevice_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> videoContext_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> vpEnum_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> videoProcessor_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> outputView_;

    uint32_t dstW_ = 0;
    uint32_t dstH_ = 0;
    uint32_t srcW_ = 0;
    uint32_t srcH_ = 0;
    bool supportsRotation_ = false;

    RECT lastDstRect_{};
    uint8_t lastRotCode_ = 0;
    bool lastMirrorH_ = false;
    bool focusVisible_ = false;
    int focusPx_ = 0;
    int focusPy_ = 0;
    ULONGLONG focusUntilMs_ = 0;
};

} // namespace zerocam
