#include "D3D11Context.h"
#include <algorithm>
#include <cstring>

namespace zerocam {

D3D11Context::~D3D11Context() {
    Shutdown();
}

bool D3D11Context::Initialize(bool allowTempIpcFallback) {
    Shutdown();

    const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0
    };
    const UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        levels,
        static_cast<UINT>(std::size(levels)),
        D3D11_SDK_VERSION,
        &device_,
        nullptr,
        &context_);
    if (FAILED(hr)) {
        return false;
    }

    Microsoft::WRL::ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(device_.As(&multithread))) {
        multithread->SetMultithreadProtected(TRUE);
    }

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDev;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    if (SUCCEEDED(device_.As(&dxgiDev)) && SUCCEEDED(dxgiDev->GetAdapter(&adapter)) && SUCCEEDED(adapter->GetDesc(&desc))) {
        adapterName_ = desc.Description;
        adapterLuid_ = desc.AdapterLuid;
    }

    hr = MFCreateDXGIDeviceManager(&resetToken_, &dxgiManager_);
    if (FAILED(hr)) {
        Shutdown();
        return false;
    }
    hr = dxgiManager_->ResetDevice(device_.Get(), resetToken_);
    if (FAILED(hr)) {
        Shutdown();
        return false;
    }

    if (!ipc_.Open(true, allowTempIpcFallback)) {
        Shutdown();
        return false;
    }

    ZeroCamSharedMemory* mem = ipc_.Data();
    mem->adapterLuidLow.store(adapterLuid_.LowPart, std::memory_order_release);
    mem->adapterLuidHigh.store(adapterLuid_.HighPart, std::memory_order_release);
    mem->clientNeedsCpuFallback.store(0, std::memory_order_release);

    // Consulta si el driver soporta rotación en bloque de video D3D11 (sin 3D shaders).
    supportsHwRotation_ = EnsureRotationProcessor(1280, 720, 1280, 720);
    return true;
}

void D3D11Context::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    effectsPipeline_.Shutdown();
    effectsPipelineInitialized_ = false;
    effectsPipelineInitAttempted_ = false;
    ipc_.Close(true);
    for (auto& t : ringTextures_) t.Reset();
    for (auto& m : ringMutexes_) m.Reset();
    for (auto& h : ringHandles_) h = nullptr;
    rotVp_.Reset();
    rotEnum_.Reset();
    videoContext_.Reset();
    videoDevice_.Reset();
    rotSrcW_ = rotSrcH_ = rotDstW_ = rotDstH_ = 0;
    supportsHwRotation_ = false;
    stagingTex_.Reset();
    testUploadTex_.Reset();
    testNv12Scratch_.clear();
    dxgiManager_.Reset();
    context_.Reset();
    device_.Reset();
    ringW_ = ringH_ = stagingW_ = stagingH_ = testW_ = testH_ = 0;
}

void D3D11Context::SetProducerActive(bool active) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ZeroCamSharedMemory* mem = ipc_.Data()) {
        if (active) {
            mem->producerAlive.store(GetCurrentProcessId(), std::memory_order_release);
        } else {
            mem->producerAlive.store(0u, std::memory_order_release);
            mem->frameSequence.store(0ull, std::memory_order_release);
            mem->cpuFrameMeta.store(0ull, std::memory_order_release);
        }
    }
}

bool D3D11Context::EnsureSharedRing(uint32_t width, uint32_t height) {
    if (ringW_ == width && ringH_ == height && ringTextures_[0] != nullptr) {
        return true;
    }
    if (!device_ || width == 0 || height == 0) {
        return false;
    }

    for (auto& t : ringTextures_) t.Reset();
    for (auto& m : ringMutexes_) m.Reset();
    for (auto& h : ringHandles_) h = nullptr;

    auto tryCreateRing = [&](UINT miscFlags, bool keyed) -> bool {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_NV12;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        desc.MiscFlags = miscFlags;

        for (uint32_t i = 0; i < kRingSlots; ++i) {
            if (FAILED(device_->CreateTexture2D(&desc, nullptr, &ringTextures_[i]))) {
                for (auto& t : ringTextures_) t.Reset();
                return false;
            }
            Microsoft::WRL::ComPtr<IDXGIResource> res;
            HANDLE sharedHandle = nullptr;
            if (FAILED(ringTextures_[i].As(&res)) || FAILED(res->GetSharedHandle(&sharedHandle)) || !sharedHandle) {
                for (auto& t : ringTextures_) t.Reset();
                return false;
            }
            ringHandles_[i] = sharedHandle;
            if (keyed && FAILED(ringTextures_[i].As(&ringMutexes_[i]))) {
                for (auto& t : ringTextures_) t.Reset();
                for (auto& m : ringMutexes_) m.Reset();
                return false;
            }
        }
        usesKeyedMutex_ = keyed;
        return true;
    };

    if (!tryCreateRing(D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX, true)) {
        if (!tryCreateRing(D3D11_RESOURCE_MISC_SHARED, false)) {
            return false;
        }
    }

    ringW_ = width;
    ringH_ = height;

    if (ZeroCamSharedMemory* mem = ipc_.Data()) {
        for (uint32_t i = 0; i < kRingSlots; ++i) {
            mem->dxgiSharedHandles[i].store(
                static_cast<uint64_t>(reinterpret_cast<uintptr_t>(ringHandles_[i])),
                std::memory_order_release);
        }
        mem->usesKeyedMutex.store(usesKeyedMutex_ ? 1u : 0u, std::memory_order_release);
        mem->width.store(width, std::memory_order_release);
        mem->height.store(height, std::memory_order_release);
        mem->gpuHandlesGeneration.fetch_add(1, std::memory_order_acq_rel);
    }
    return true;
}

bool D3D11Context::EnsureStagingTexture(uint32_t width, uint32_t height) {
    if (stagingW_ == width && stagingH_ == height && stagingTex_ != nullptr) {
        return true;
    }
    stagingTex_.Reset();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &stagingTex_))) {
        return false;
    }
    stagingW_ = width;
    stagingH_ = height;
    return true;
}

bool D3D11Context::StageSlotToCpuTexture(ID3D11Texture2D* gpuTex, uint32_t width, uint32_t height) {
    if (!gpuTex || !context_ || width < 2u || height < 2u || (width & 1u) != 0u || (height & 1u) != 0u ||
        width > kMaxCpuWidth || height > kMaxCpuHeight) {
        return false;
    }
    if (!EnsureStagingTexture(width, height)) {
        return false;
    }
    context_->CopyResource(stagingTex_.Get(), gpuTex);
    return true;
}

void D3D11Context::ReadStagedCpuFallback(uint32_t width, uint32_t height) {
    ZeroCamSharedMemory* mem = ipc_.Data();
    if (!mem || !stagingTex_ || !context_ || width < 2u || height < 2u ||
        (width & 1u) != 0u || (height & 1u) != 0u || width > kMaxCpuWidth || height > kMaxCpuHeight) {
        return;
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(stagingTex_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return;
    }
    if (!mapped.pData || mapped.RowPitch < width) {
        context_->Unmap(stagingTex_.Get(), 0);
        return;
    }

    const uint32_t cpuSlot = (mem->activeCpuSlot.load(std::memory_order_relaxed) + 1) % kCpuFallbackSlots;
    uint8_t* dst = mem->cpuNv12Buffer[cpuSlot];
    const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);

    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(dst + static_cast<size_t>(y) * width, src + static_cast<size_t>(y) * mapped.RowPitch, width);
    }
    const uint8_t* srcUv = src + static_cast<size_t>(mapped.RowPitch) * height;
    uint8_t* dstUv = dst + static_cast<size_t>(width) * height;
    for (uint32_t y = 0; y < height / 2; ++y) {
        std::memcpy(dstUv + static_cast<size_t>(y) * width, srcUv + static_cast<size_t>(y) * mapped.RowPitch, width);
    }

    context_->Unmap(stagingTex_.Get(), 0);
    mem->cpuStride.store(width, std::memory_order_release);
    mem->activeCpuSlot.store(cpuSlot, std::memory_order_release);
    mem->cpuFrameMeta.store(PackCpuFrameMeta(width, height, width, cpuSlot), std::memory_order_release);
}

bool D3D11Context::EnsureRotationProcessor(uint32_t srcW, uint32_t srcH, uint32_t dstW, uint32_t dstH) {
    if (!device_ || !context_ || srcW == 0 || srcH == 0) return false;
    if (rotVp_ && rotEnum_ && rotSrcW_ == srcW && rotSrcH_ == srcH && rotDstW_ == dstW && rotDstH_ == dstH) {
        return supportsHwRotation_;
    }

    if (!videoDevice_ || !videoContext_) {
        if (FAILED(device_.As(&videoDevice_)) || FAILED(context_.As(&videoContext_))) {
            supportsHwRotation_ = false;
            return false;
        }
    }

    rotVp_.Reset();
    rotEnum_.Reset();

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cDesc{};
    cDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cDesc.InputWidth = srcW;
    cDesc.InputHeight = srcH;
    cDesc.OutputWidth = dstW;
    cDesc.OutputHeight = dstH;
    cDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    if (FAILED(videoDevice_->CreateVideoProcessorEnumerator(&cDesc, &rotEnum_))) {
        supportsHwRotation_ = false;
        return false;
    }

    D3D11_VIDEO_PROCESSOR_CAPS caps{};
    if (FAILED(rotEnum_->GetVideoProcessorCaps(&caps)) ||
        (caps.FeatureCaps & D3D11_VIDEO_PROCESSOR_FEATURE_CAPS_ROTATION) == 0) {
        rotEnum_.Reset();
        supportsHwRotation_ = false;
        return false;
    }

    if (FAILED(videoDevice_->CreateVideoProcessor(rotEnum_.Get(), 0, &rotVp_))) {
        rotEnum_.Reset();
        supportsHwRotation_ = false;
        return false;
    }

    rotSrcW_ = srcW;
    rotSrcH_ = srcH;
    rotDstW_ = dstW;
    rotDstH_ = dstH;
    supportsHwRotation_ = true;
    return true;
}

bool D3D11Context::BlitRotatedHw(
    ID3D11Texture2D* srcTex,
    UINT subresourceIdx,
    ID3D11Texture2D* dstTex,
    uint32_t w,
    uint32_t h,
    uint8_t rotCode) {
    D3D11_TEXTURE2D_DESC srcDesc{};
    srcTex->GetDesc(&srcDesc);
    if (!EnsureRotationProcessor(srcDesc.Width, srcDesc.Height, w, h) || !rotVp_ || !rotEnum_) {
        return false;
    }

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivDesc{};
    ivDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ivDesc.Texture2D.MipSlice = 0;
    ivDesc.Texture2D.ArraySlice = subresourceIdx;

    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> inView;
    if (FAILED(videoDevice_->CreateVideoProcessorInputView(srcTex, rotEnum_.Get(), &ivDesc, &inView))) {
        return false;
    }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovDesc{};
    ovDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> outView;
    if (FAILED(videoDevice_->CreateVideoProcessorOutputView(dstTex, rotEnum_.Get(), &ovDesc, &outView))) {
        return false;
    }

    D3D11_VIDEO_COLOR bgColor{};
    bgColor.YCbCr = { 0.0627f, 0.5f, 0.5f, 1.0f };
    videoContext_->VideoProcessorSetOutputBackgroundColor(rotVp_.Get(), TRUE, &bgColor);
    videoContext_->VideoProcessorSetStreamFrameFormat(rotVp_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);

    RECT srcRect{ 0, 0, static_cast<LONG>(w), static_cast<LONG>(h) };
    videoContext_->VideoProcessorSetStreamSourceRect(rotVp_.Get(), 0, TRUE, &srcRect);

    const bool swapWh = (rotCode == 1 || rotCode == 3);
    RECT dstRect{ 0, 0, static_cast<LONG>(w), static_cast<LONG>(h) };
    if (swapWh && w > h) {
        // Centra vertical 9:16 dentro del lienzo 16:9 manteniendo coordenadas pares para chroma NV12.
        const uint32_t scaledW = std::max<uint32_t>(2u, ((h * h) / w) & ~1u);
        const uint32_t offsetX = ((w - scaledW) / 2u) & ~1u;
        dstRect = { static_cast<LONG>(offsetX), 0, static_cast<LONG>(offsetX + scaledW), static_cast<LONG>(h) };
    }
    videoContext_->VideoProcessorSetStreamDestRect(rotVp_.Get(), 0, TRUE, &dstRect);
    videoContext_->VideoProcessorSetStreamRotation(rotVp_.Get(), 0, TRUE, static_cast<D3D11_VIDEO_PROCESSOR_ROTATION>(rotCode & 0x3u));

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = inView.Get();

    return SUCCEEDED(videoContext_->VideoProcessorBlt(rotVp_.Get(), outView.Get(), 0, 1, &stream));
}

void D3D11Context::SetEffectsConfig(const VideoEffectsConfig& cfg) {
    VideoEffectsConfig safe = cfg;
    ClampEffectsConfig(safe);
    std::lock_guard<std::mutex> lock(effectsMutex_);
    effectsCfg_ = safe;
}

VideoEffectsConfig D3D11Context::GetEffectsConfig() const {
    std::lock_guard<std::mutex> lock(effectsMutex_);
    return effectsCfg_;
}

void D3D11Context::SetOptionalModelPath(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(effectsMutex_);
    optionalModelPath_ = path;
}

double D3D11Context::LastAiInferenceMs() const noexcept {
    return effectsPipeline_.LastAiMs();
}

bool D3D11Context::IsAiBlurAvailable() const noexcept {
    return effectsPipeline_.IsAiAvailable();
}

bool D3D11Context::IsAiUsingGpu() const noexcept {
    return effectsPipeline_.IsAiUsingGpu();
}

bool D3D11Context::PublishGpuFrame(
    ID3D11Texture2D* srcTex,
    UINT subresourceIndex,
    uint32_t visibleWidth,
    uint32_t visibleHeight,
    int64_t ptsHns,
    uint32_t fpsNum,
    uint32_t fpsDen,
    uint8_t rotationCode,
    ID3D11Texture2D** outPublishedRingTex,
    bool* outAlreadyRotated) {
    if (!srcTex || !device_ || !context_) return false;
    if (outAlreadyRotated) *outAlreadyRotated = false;

    VideoEffectsConfig fxCfg{};
    std::wstring modelPath;
    {
        std::lock_guard<std::mutex> fxLock(effectsMutex_);
        fxCfg = effectsCfg_;
        modelPath = optionalModelPath_;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!EnsureSharedRing(visibleWidth, visibleHeight)) {
        return false;
    }

    const uint32_t slot = nextSlot_ % kRingSlots;
    ++nextSlot_;

    if (usesKeyedMutex_ && ringMutexes_[slot]) {
        if (FAILED(ringMutexes_[slot]->AcquireSync(0, 16))) {
            return false;
        }
    }

    if (!fxCfg.IsIdentity()) {
        if (!effectsPipelineInitAttempted_) {
            effectsPipelineInitAttempted_ = true;
            effectsPipelineInitialized_ = effectsPipeline_.Initialize(device_.Get(), context_.Get(), modelPath);
        }
        Microsoft::WRL::ComPtr<ID3D11Texture2D> fxPreview;
        if (effectsPipelineInitialized_ &&
            effectsPipeline_.Process(
                srcTex, subresourceIndex, visibleWidth, visibleHeight, rotationCode,
                fxCfg, ringTextures_[slot].Get(), &fxPreview)) {
            ZeroCamSharedMemory* mem = ipc_.Data();
            bool stagedCpu = false;
            if (mem && mem->clientNeedsCpuFallback.load(std::memory_order_acquire) != 0) {
                stagedCpu = StageSlotToCpuTexture(ringTextures_[slot].Get(), visibleWidth, visibleHeight);
            }
            if (!usesKeyedMutex_) {
                context_->Flush();
            } else if (ringMutexes_[slot]) {
                ringMutexes_[slot]->ReleaseSync(0);
            }

            if (mem) {
                if (mem->producerAlive.load(std::memory_order_relaxed) == 0u) {
                    mem->producerAlive.store(GetCurrentProcessId(), std::memory_order_release);
                }
                if (stagedCpu) {
                    ReadStagedCpuFallback(visibleWidth, visibleHeight);
                }
                mem->width.store(visibleWidth, std::memory_order_release);
                mem->height.store(visibleHeight, std::memory_order_release);
                mem->fpsNum.store(fpsNum, std::memory_order_release);
                mem->fpsDen.store(fpsDen, std::memory_order_release);
                mem->timestampHns.store(static_cast<uint64_t>(std::max<int64_t>(0, ptsHns)), std::memory_order_release);
                mem->activeSlot.store(slot, std::memory_order_release);
                mem->frameSequence.fetch_add(1, std::memory_order_release);
            }

            if (outAlreadyRotated) *outAlreadyRotated = true;
            if (outPublishedRingTex && fxPreview) {
                *outPublishedRingTex = fxPreview.Detach();
            }
            return true;
        }
    } else if (effectsPipelineInitialized_) {
        effectsPipeline_.ResetAiState();
    }

    const uint8_t rot = rotationCode & 0x3u;
    bool blitted = false;
    if (rot != 0) {
        blitted = BlitRotatedHw(srcTex, subresourceIndex, ringTextures_[slot].Get(), visibleWidth, visibleHeight, rot);
    }
    if (!blitted) {
        D3D11_BOX srcBox{ 0, 0, 0, visibleWidth, visibleHeight, 1 };
        context_->CopySubresourceRegion(ringTextures_[slot].Get(), 0, 0, 0, 0, srcTex, subresourceIndex, &srcBox);
    }

    ZeroCamSharedMemory* mem = ipc_.Data();
    bool stagedCpu = false;
    if (mem && mem->clientNeedsCpuFallback.load(std::memory_order_acquire) != 0) {
        stagedCpu = StageSlotToCpuTexture(ringTextures_[slot].Get(), visibleWidth, visibleHeight);
    }
    if (!usesKeyedMutex_) {
        context_->Flush();
    } else if (ringMutexes_[slot]) {
        ringMutexes_[slot]->ReleaseSync(0);
    }

    if (mem) {
        if (mem->producerAlive.load(std::memory_order_relaxed) == 0u) {
            mem->producerAlive.store(GetCurrentProcessId(), std::memory_order_release);
        }
        if (stagedCpu) {
            ReadStagedCpuFallback(visibleWidth, visibleHeight);
        }
        mem->width.store(visibleWidth, std::memory_order_release);
        mem->height.store(visibleHeight, std::memory_order_release);
        mem->fpsNum.store(fpsNum, std::memory_order_release);
        mem->fpsDen.store(fpsDen, std::memory_order_release);
        mem->timestampHns.store(static_cast<uint64_t>(std::max<int64_t>(0, ptsHns)), std::memory_order_release);
        mem->activeSlot.store(slot, std::memory_order_release);
        mem->frameSequence.fetch_add(1, std::memory_order_release);
    }

    // En modo sin filtros NO devolvemos la textura del ring (tiene IDXGIKeyedMutex);
    // el llamador presenta directo la textura NV12 del decodificador MFT sin contención.
    if (outPublishedRingTex) {
        *outPublishedRingTex = nullptr;
    }
    return true;
}

bool D3D11Context::PublishCpuNv12Frame(
    const uint8_t* data,
    uint32_t stride,
    uint32_t width,
    uint32_t height,
    int64_t ptsHns,
    uint32_t fpsNum,
    uint32_t fpsDen) {
    if (!data || !device_ || !context_ || stride < width) return false;

    std::lock_guard<std::mutex> lock(mutex_);
    if (testW_ != width || testH_ != height || !testUploadTex_) {
        testUploadTex_.Reset();
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_NV12;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &testUploadTex_))) {
            return false;
        }
        testW_ = width;
        testH_ = height;
    }

    context_->UpdateSubresource(testUploadTex_.Get(), 0, nullptr, data, stride, 0);

    if (!EnsureSharedRing(width, height)) return false;
    const uint32_t slot = nextSlot_ % kRingSlots;
    ++nextSlot_;

    if (usesKeyedMutex_ && ringMutexes_[slot]) {
        if (FAILED(ringMutexes_[slot]->AcquireSync(0, 16))) return false;
    }
    context_->CopySubresourceRegion(ringTextures_[slot].Get(), 0, 0, 0, 0, testUploadTex_.Get(), 0, nullptr);
    if (!usesKeyedMutex_) context_->Flush();
    else if (ringMutexes_[slot]) ringMutexes_[slot]->ReleaseSync(0);

    if (ZeroCamSharedMemory* mem = ipc_.Data()) {
        if (mem->producerAlive.load(std::memory_order_relaxed) == 0u) {
            mem->producerAlive.store(GetCurrentProcessId(), std::memory_order_release);
        }
        if (mem->clientNeedsCpuFallback.load(std::memory_order_acquire) != 0 &&
            width >= 2u && height >= 2u && (width & 1u) == 0u && (height & 1u) == 0u &&
            width <= kMaxCpuWidth && height <= kMaxCpuHeight) {
            const uint32_t cpuSlot = (mem->activeCpuSlot.load(std::memory_order_relaxed) + 1) % kCpuFallbackSlots;
            uint8_t* dst = mem->cpuNv12Buffer[cpuSlot];
            for (uint32_t y = 0; y < height; ++y) {
                std::memcpy(dst + static_cast<size_t>(y) * width, data + static_cast<size_t>(y) * stride, width);
            }
            const uint8_t* srcUv = data + static_cast<size_t>(stride) * height;
            uint8_t* dstUv = dst + static_cast<size_t>(width) * height;
            for (uint32_t y = 0; y < height / 2; ++y) {
                std::memcpy(dstUv + static_cast<size_t>(y) * width, srcUv + static_cast<size_t>(y) * stride, width);
            }
            mem->cpuStride.store(width, std::memory_order_release);
            mem->activeCpuSlot.store(cpuSlot, std::memory_order_release);
            mem->cpuFrameMeta.store(PackCpuFrameMeta(width, height, width, cpuSlot), std::memory_order_release);
        }
        mem->width.store(width, std::memory_order_release);
        mem->height.store(height, std::memory_order_release);
        mem->fpsNum.store(fpsNum, std::memory_order_release);
        mem->fpsDen.store(fpsDen, std::memory_order_release);
        mem->timestampHns.store(static_cast<uint64_t>(std::max<int64_t>(0, ptsHns)), std::memory_order_release);
        mem->activeSlot.store(slot, std::memory_order_release);
        mem->frameSequence.fetch_add(1, std::memory_order_release);
    }
    return true;
}

bool D3D11Context::PublishTestPattern(
    uint32_t width,
    uint32_t height,
    uint64_t frameIdx,
    uint32_t fpsNum,
    uint32_t fpsDen,
    ID3D11Texture2D** outGpuTex) {
    const size_t totalBytes = static_cast<size_t>(width) * height * 3u / 2u;
    if (testNv12Scratch_.size() != totalBytes) {
        testNv12Scratch_.resize(totalBytes);
        // Plano UV estático suave (tono azulado profesional) + plano Y base.
        std::memset(testNv12Scratch_.data(), 26, static_cast<size_t>(width) * height);
        uint8_t* uv = testNv12Scratch_.data() + static_cast<size_t>(width) * height;
        for (size_t i = 0; i < (static_cast<size_t>(width) * height) / 2; i += 2) {
            uv[i] = 152;     // U
            uv[i + 1] = 116; // V
        }
    }

    // Dibuja una banda luminosa en movimiento sobre luma Y para confirmar fluidez visual sin 3D shaders.
    uint8_t* yPlane = testNv12Scratch_.data();
    const uint32_t barW = std::max<uint32_t>(24, width / 16);
    const uint32_t prevX = static_cast<uint32_t>(((frameIdx > 0 ? frameIdx - 1 : 0) * 12) % (width - barW));
    const uint32_t curX  = static_cast<uint32_t>((frameIdx * 12) % (width - barW));

    for (uint32_t y = height / 4; y < (height * 3) / 4; ++y) {
        std::memset(yPlane + static_cast<size_t>(y) * width + prevX, 26, barW);
        std::memset(yPlane + static_cast<size_t>(y) * width + curX, 210, barW);
    }

    const int64_t ptsHns = static_cast<int64_t>((frameIdx * 10000000ull * fpsDen) / std::max<uint32_t>(1, fpsNum));
    const bool ok = PublishCpuNv12Frame(testNv12Scratch_.data(), width, width, height, ptsHns, fpsNum, fpsDen);
    if (ok && outGpuTex && testUploadTex_) {
        std::lock_guard<std::mutex> lock(mutex_);
        *outGpuTex = testUploadTex_.Get();
        (*outGpuTex)->AddRef();
    }
    return ok;
}

} // namespace zerocam
