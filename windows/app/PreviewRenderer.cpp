#include "PreviewRenderer.h"
#include <d3d11_1.h>
#include <algorithm>
#include <cmath>

namespace zerocam {

PreviewRenderer::~PreviewRenderer() {
    ReleasePipeline();
}

void PreviewRenderer::Attach(D3D11Context* d3dCtx, HWND hwnd) {
    std::lock_guard<std::mutex> lock(mutex_);
    d3dCtx_ = d3dCtx;
    hwnd_ = hwnd;
}

void PreviewRenderer::SetEnabled(bool enabled) {
    enabled_.store(enabled, std::memory_order_release);
    if (!enabled) {
        std::lock_guard<std::mutex> lock(mutex_);
        ReleasePipeline();
    }
}

void PreviewRenderer::Resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!swapChain_) return;

    std::unique_lock<std::mutex> ctxLock;
    if (d3dCtx_) {
        ctxLock = std::unique_lock<std::mutex>(d3dCtx_->ContextMutex());
    }
    outputView_.Reset();
    videoProcessor_.Reset();
    vpEnum_.Reset();
    if (SUCCEEDED(swapChain_->ResizeBuffers(2, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 0))) {
        dstW_ = width;
        dstH_ = height;
    } else {
        ReleasePipeline();
    }
}

void PreviewRenderer::ReleasePipeline() {
    outputView_.Reset();
    videoProcessor_.Reset();
    vpEnum_.Reset();
    videoContext_.Reset();
    videoDevice_.Reset();
    swapChain_.Reset();
    dstW_ = dstH_ = srcW_ = srcH_ = 0;
    supportsRotation_ = false;
}

bool PreviewRenderer::EnsurePipeline(uint32_t srcW, uint32_t srcH, uint32_t dstW, uint32_t dstH) {
    if (!d3dCtx_ || !d3dCtx_->Device() || !hwnd_ || dstW == 0 || dstH == 0) {
        return false;
    }
    if (swapChain_ && outputView_ && srcW_ == srcW && srcH_ == srcH && dstW_ == dstW && dstH_ == dstH) {
        return true;
    }

    ID3D11Device* dev = d3dCtx_->Device();
    if (!swapChain_) {
        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDev;
        Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
        if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dxgiDev))) ||
            FAILED(dxgiDev->GetAdapter(&adapter)) ||
            FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
            return false;
        }

        DXGI_SWAP_CHAIN_DESC1 scDesc{};
        scDesc.Width = dstW;
        scDesc.Height = dstH;
        scDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        scDesc.SampleDesc.Count = 1;
        scDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scDesc.BufferCount = 2;
        scDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        if (FAILED(factory->CreateSwapChainForHwnd(dev, hwnd_, &scDesc, nullptr, nullptr, &swapChain_))) {
            return false;
        }
        dstW_ = dstW;
        dstH_ = dstH;
    }

    if (!videoDevice_ || !videoContext_) {
        if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&videoDevice_))) ||
            FAILED(d3dCtx_->Context()->QueryInterface(IID_PPV_ARGS(&videoContext_)))) {
            ReleasePipeline();
            return false;
        }
    }

    if (!videoProcessor_ || srcW_ != srcW || srcH_ != srcH || dstW_ != dstW || dstH_ != dstH) {
        vpEnum_.Reset();
        videoProcessor_.Reset();
        outputView_.Reset();

        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cDesc{};
        cDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        cDesc.InputWidth = srcW;
        cDesc.InputHeight = srcH;
        cDesc.OutputWidth = dstW;
        cDesc.OutputHeight = dstH;
        cDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

        if (FAILED(videoDevice_->CreateVideoProcessorEnumerator(&cDesc, &vpEnum_)) ||
            FAILED(videoDevice_->CreateVideoProcessor(vpEnum_.Get(), 0, &videoProcessor_))) {
            ReleasePipeline();
            return false;
        }
        D3D11_VIDEO_PROCESSOR_CAPS caps{};
        supportsRotation_ = SUCCEEDED(vpEnum_->GetVideoProcessorCaps(&caps)) &&
                            ((caps.FeatureCaps & D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_ROTATION) != 0);
        srcW_ = srcW;
        srcH_ = srcH;
    }

    if (!outputView_) {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
        if (FAILED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
            ReleasePipeline();
            return false;
        }
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovDesc{};
        ovDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        if (FAILED(videoDevice_->CreateVideoProcessorOutputView(backBuffer.Get(), vpEnum_.Get(), &ovDesc, &outputView_))) {
            ReleasePipeline();
            return false;
        }
    }
    return true;
}

bool PreviewRenderer::PresentNv12(
    ID3D11Texture2D* srcTex,
    UINT subresourceIdx,
    uint32_t srcW,
    uint32_t srcH,
    uint8_t rotationCode,
    bool mirrorH,
    uint8_t effectiveRotForMapping) {
    // Corte rápido sin coste: 0% GPU cuando la vista previa está apagada, oculta o minimizada.
    if (!enabled_.load(std::memory_order_acquire) || !hwnd_ || !IsWindowVisible(hwnd_) || IsIconic(GetAncestor(hwnd_, GA_ROOT))) {
        return true;
    }
    if (!srcTex || srcW == 0 || srcH == 0) {
        return false;
    }

    RECT rc{};
    if (!GetClientRect(hwnd_, &rc) || rc.right <= rc.left || rc.bottom <= rc.top) {
        return true;
    }
    const uint32_t dstW = static_cast<uint32_t>(rc.right - rc.left);
    const uint32_t dstH = static_cast<uint32_t>(rc.bottom - rc.top);

    std::lock_guard<std::mutex> lock(mutex_);
    if (!d3dCtx_) return false;
    std::lock_guard<std::mutex> ctxLock(d3dCtx_->ContextMutex());

    // Usa dimensiones reales de superficie (ej. 1920x1088 del MFT) para soportar recorte 1920x1080 en srcRect.
    D3D11_TEXTURE2D_DESC srcDesc{};
    srcTex->GetDesc(&srcDesc);
    if (!EnsurePipeline(srcDesc.Width, srcDesc.Height, dstW, dstH)) {
        return false;
    }

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivDesc{};
    ivDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ivDesc.Texture2D.MipSlice = 0;
    ivDesc.Texture2D.ArraySlice = subresourceIdx;

    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> inputView;
    if (FAILED(videoDevice_->CreateVideoProcessorInputView(srcTex, vpEnum_.Get(), &ivDesc, &inputView))) {
        return false;
    }

    D3D11_VIDEO_COLOR bgColor{};
    bgColor.RGBA = { 0.0f, 0.0f, 0.0f, 1.0f };
    videoContext_->VideoProcessorSetOutputBackgroundColor(videoProcessor_.Get(), FALSE, &bgColor);
    videoContext_->VideoProcessorSetStreamFrameFormat(videoProcessor_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);

    RECT srcRect{ 0, 0, static_cast<LONG>(srcW), static_cast<LONG>(srcH) };
    videoContext_->VideoProcessorSetStreamSourceRect(videoProcessor_.Get(), 0, TRUE, &srcRect);

    const uint8_t rot = rotationCode & 0x3u;
    const bool applyRot = supportsRotation_ && rot != 0;
    const bool swapWh = applyRot && (rot == 1 || rot == 3);
    const double effW = static_cast<double>(swapWh ? srcH : srcW);
    const double effH = static_cast<double>(swapWh ? srcW : srcH);
    const double scale = std::min(static_cast<double>(dstW) / effW, static_cast<double>(dstH) / effH);
    const LONG drawW = std::max<LONG>(2, static_cast<LONG>(effW * scale) & ~1L);
    const LONG drawH = std::max<LONG>(2, static_cast<LONG>(effH * scale) & ~1L);
    const LONG offX = (static_cast<LONG>(dstW) - drawW) / 2;
    const LONG offY = (static_cast<LONG>(dstH) - drawH) / 2;
    RECT dstRect{ offX, offY, offX + drawW, offY + drawH };
    videoContext_->VideoProcessorSetStreamDestRect(videoProcessor_.Get(), 0, TRUE, &dstRect);
    videoContext_->VideoProcessorSetStreamRotation(
        videoProcessor_.Get(), 0, applyRot ? TRUE : FALSE,
        static_cast<D3D11_VIDEO_PROCESSOR_ROTATION>(applyRot ? rot : 0));

    lastDstRect_ = dstRect;
    lastRotCode_ = effectiveRotForMapping & 0x3u;
    lastMirrorH_ = mirrorH;

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = inputView.Get();

    if (FAILED(videoContext_->VideoProcessorBlt(videoProcessor_.Get(), outputView_.Get(), 0, 1, &stream))) {
        return false;
    }

    if (focusVisible_) {
        const ULONGLONG now = GetTickCount64();
        if (now < focusUntilMs_) {
            Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuf;
            Microsoft::WRL::ComPtr<ID3D11RenderTargetView> bbRtv;
            Microsoft::WRL::ComPtr<ID3D11DeviceContext1> ctx1;
            if (SUCCEEDED(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuf))) &&
                SUCCEEDED(d3dCtx_->Device()->CreateRenderTargetView(backBuf.Get(), nullptr, &bbRtv)) &&
                SUCCEEDED(d3dCtx_->Context()->QueryInterface(IID_PPV_ARGS(&ctx1)))) {
                const float blue[4] = { 0.176f, 0.490f, 1.0f, 1.0f };
                const LONG boxR = 22;
                const LONG thick = 2;
                const LONG x0 = std::clamp<LONG>(focusPx_ - boxR, 0, static_cast<LONG>(dstW) - 1);
                const LONG y0 = std::clamp<LONG>(focusPy_ - boxR, 0, static_cast<LONG>(dstH) - 1);
                const LONG x1 = std::clamp<LONG>(focusPx_ + boxR, x0 + 2, static_cast<LONG>(dstW));
                const LONG y1 = std::clamp<LONG>(focusPy_ + boxR, y0 + 2, static_cast<LONG>(dstH));
                D3D11_RECT bars[4] = {
                    { x0, y0, x1, std::min(y1, y0 + thick) },
                    { x0, std::max(y0, y1 - thick), x1, y1 },
                    { x0, y0, std::min(x1, x0 + thick), y1 },
                    { std::max(x0, x1 - thick), y0, x1, y1 }
                };
                ctx1->ClearView(bbRtv.Get(), blue, bars, 4);
            }
        } else {
            focusVisible_ = false;
        }
    }

    swapChain_->Present(0, 0);
    return true;
}

void PreviewRenderer::ShowFocusFeedback(int clientX, int clientY) {
    std::lock_guard<std::mutex> lock(mutex_);
    focusPx_ = clientX;
    focusPy_ = clientY;
    focusVisible_ = true;
    focusUntilMs_ = GetTickCount64() + 900;
}

bool PreviewRenderer::MapClientPointToSensorNorm(int clientX, int clientY, float& outNormX, float& outNormY) {
    std::lock_guard<std::mutex> lock(mutex_);
    const LONG rw = lastDstRect_.right - lastDstRect_.left;
    const LONG rh = lastDstRect_.bottom - lastDstRect_.top;
    if (rw <= 2 || rh <= 2) return false;
    if (clientX < lastDstRect_.left || clientX > lastDstRect_.right ||
        clientY < lastDstRect_.top || clientY > lastDstRect_.bottom) {
        return false;
    }

    float u = static_cast<float>(clientX - lastDstRect_.left) / static_cast<float>(rw);
    float v = static_cast<float>(clientY - lastDstRect_.top) / static_cast<float>(rh);
    u = std::clamp(u, 0.0f, 1.0f);
    v = std::clamp(v, 0.0f, 1.0f);

    if (lastMirrorH_) {
        u = 1.0f - u;
    }

    switch (lastRotCode_ & 0x3u) {
    case 1: // 90 CW: display X venía de Y origen invertido? Invierte rotación aplicada por VideoProcessor
        outNormX = v;
        outNormY = 1.0f - u;
        break;
    case 2:
        outNormX = 1.0f - u;
        outNormY = 1.0f - v;
        break;
    case 3:
        outNormX = 1.0f - v;
        outNormY = u;
        break;
    default:
        outNormX = u;
        outNormY = v;
        break;
    }
    return true;
}

bool PreviewRenderer::MapClientPointToDisplayUv(int clientX, int clientY, float& outU, float& outV) {
    std::lock_guard<std::mutex> lock(mutex_);
    const LONG rw = lastDstRect_.right - lastDstRect_.left;
    const LONG rh = lastDstRect_.bottom - lastDstRect_.top;
    if (rw <= 2 || rh <= 2) return false;
    if (clientX < lastDstRect_.left || clientX > lastDstRect_.right ||
        clientY < lastDstRect_.top || clientY > lastDstRect_.bottom) {
        return false;
    }
    outU = std::clamp(static_cast<float>(clientX - lastDstRect_.left) / static_cast<float>(rw), 0.05f, 0.95f);
    outV = std::clamp(static_cast<float>(clientY - lastDstRect_.top) / static_cast<float>(rh), 0.05f, 0.95f);
    return true;
}

} // namespace zerocam
