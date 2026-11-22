#include "ZeroCamMediaStream.h"
#include "ZeroCamMediaSource.h"
#include "../shared/ZeroCamGuids.h"
#include <algorithm>
#include <cstring>

namespace zerocam {

namespace {

// Guardia RAII para garantizar UnlockDevice sobre IMFDXGIDeviceManager en todos los caminos de retorno.
class DxgiDeviceLock {
public:
    DxgiDeviceLock(IMFDXGIDeviceManager* mgr, HANDLE handle) noexcept : mgr_(mgr), handle_(handle) {}
    ~DxgiDeviceLock() {
        if (locked_ && mgr_ && handle_) {
            mgr_->UnlockDevice(handle_, FALSE);
        }
    }
    HRESULT Lock(ID3D11Device** ppDev) noexcept {
        if (!mgr_ || !handle_ || !ppDev) {
            return E_POINTER;
        }
        HRESULT hr = mgr_->LockDevice(handle_, IID_PPV_ARGS(ppDev), TRUE);
        locked_ = SUCCEEDED(hr);
        return hr;
    }

private:
    IMFDXGIDeviceManager* mgr_ = nullptr;
    HANDLE handle_ = nullptr;
    bool locked_ = false;
};

    // Reescala o copia plano NV12 (Y + UV entrelazado) entre resoluciones arbitrarias pares con validación de cotas.
    void ScaleNv12Cpu(
        const uint8_t* src,
        uint32_t srcStride,
        uint32_t srcW,
        uint32_t srcH,
        uint8_t* dst,
        uint32_t dstStride,
        uint32_t dstW,
        uint32_t dstH) {
        if (!src || !dst || srcW < 2u || srcH < 2u || dstW < 2u || dstH < 2u ||
            (srcW & 1u) != 0u || (srcH & 1u) != 0u || (dstW & 1u) != 0u || (dstH & 1u) != 0u ||
            srcStride < srcW || dstStride < dstW) {
            return;
        }
        const uint8_t* srcUv = src + static_cast<size_t>(srcStride) * srcH;
        uint8_t* dstUv = dst + static_cast<size_t>(dstStride) * dstH;

        if (srcW == dstW && srcH == dstH) {
            const size_t rowBytes = std::min<size_t>(srcW, dstStride);
            for (uint32_t y = 0; y < dstH; ++y) {
                std::memcpy(dst + static_cast<size_t>(y) * dstStride, src + static_cast<size_t>(y) * srcStride, rowBytes);
            }
            for (uint32_t y = 0; y < dstH / 2u; ++y) {
                std::memcpy(dstUv + static_cast<size_t>(y) * dstStride, srcUv + static_cast<size_t>(y) * srcStride, rowBytes);
            }
            return;
        }

        struct BilinearTap { uint32_t i0, i1; uint16_t w1, w0; };
        auto buildLut = [](uint32_t cropOffset, uint32_t cropLen, uint32_t dstLen, uint32_t stride, std::vector<BilinearTap>& lut) {
            lut.resize(dstLen);
            if (cropLen <= 1u) {
                const uint32_t idx = cropOffset * stride;
                for (uint32_t d = 0; d < dstLen; ++d) lut[d] = { idx, idx, 0u, 256u };
                return;
            }
            const int64_t maxFp = static_cast<int64_t>(cropLen - 1u) << 8;
            const uint32_t maxSample = cropOffset + cropLen - 1u;
            for (uint32_t d = 0; d < dstLen; ++d) {
                const int64_t fp = std::clamp<int64_t>(
                    ((2LL * static_cast<int64_t>(d) + 1LL) * static_cast<int64_t>(cropLen) * 128LL) /
                        static_cast<int64_t>(dstLen) - 128LL,
                    0LL, maxFp);
                const uint32_t base = cropOffset + static_cast<uint32_t>(fp >> 8);
                const uint32_t next = std::min(maxSample, base + 1u);
                const uint16_t w1 = static_cast<uint16_t>(fp & 0xFFLL);
                lut[d] = { base * stride, next * stride, w1, static_cast<uint16_t>(256u - w1) };
            }
        };

        uint32_t cropX = 0, cropY = 0, cropW = srcW, cropH = srcH;
        const uint64_t lhs = static_cast<uint64_t>(srcW) * dstH;
        const uint64_t rhs = static_cast<uint64_t>(dstW) * srcH;
        if (lhs > rhs) {
            uint32_t cw = static_cast<uint32_t>((rhs + dstH / 2u) / dstH) & ~1u;
            cropW = std::clamp(cw, 2u, srcW & ~1u);
            cropX = ((srcW - cropW) / 2u) & ~1u;
        } else if (rhs > lhs) {
            uint32_t ch = static_cast<uint32_t>((lhs + dstW / 2u) / dstW) & ~1u;
            cropH = std::clamp(ch, 2u, srcH & ~1u);
            cropY = ((srcH - cropH) / 2u) & ~1u;
        }

        std::vector<BilinearTap> xLutY, yLutY, xLutUv, yLutUv;
        buildLut(cropX, cropW, dstW, 1u, xLutY);
        buildLut(cropY, cropH, dstH, 1u, yLutY);
        buildLut(cropX / 2u, cropW / 2u, dstW / 2u, 2u, xLutUv);
        buildLut(cropY / 2u, cropH / 2u, dstH / 2u, 1u, yLutUv);

        for (uint32_t y = 0; y < dstH; ++y) {
            const BilinearTap& yt = yLutY[y];
            const uint8_t* s0 = src + static_cast<size_t>(yt.i0) * srcStride;
            const uint8_t* s1 = src + static_cast<size_t>(yt.i1) * srcStride;
            uint8_t* dRow = dst + static_cast<size_t>(y) * dstStride;
            for (uint32_t x = 0; x < dstW; ++x) {
                const BilinearTap& xt = xLutY[x];
                const uint32_t top = static_cast<uint32_t>(s0[xt.i0]) * xt.w0 + static_cast<uint32_t>(s0[xt.i1]) * xt.w1;
                const uint32_t bot = static_cast<uint32_t>(s1[xt.i0]) * xt.w0 + static_cast<uint32_t>(s1[xt.i1]) * xt.w1;
                dRow[x] = static_cast<uint8_t>((top * yt.w0 + bot * yt.w1 + 32768u) >> 16);
            }
        }

        const uint32_t dstPairsW = dstW / 2u;
        const uint32_t dstPairsH = dstH / 2u;
        for (uint32_t cy = 0; cy < dstPairsH; ++cy) {
            const BilinearTap& yt = yLutUv[cy];
            const uint8_t* s0 = srcUv + static_cast<size_t>(yt.i0) * srcStride;
            const uint8_t* s1 = srcUv + static_cast<size_t>(yt.i1) * srcStride;
            uint8_t* dRow = dstUv + static_cast<size_t>(cy) * dstStride;
            for (uint32_t cx = 0; cx < dstPairsW; ++cx) {
                const BilinearTap& xt = xLutUv[cx];
                const size_t dx = static_cast<size_t>(cx) * 2u;
                const uint32_t uTop = static_cast<uint32_t>(s0[xt.i0]) * xt.w0 + static_cast<uint32_t>(s0[xt.i1]) * xt.w1;
                const uint32_t uBot = static_cast<uint32_t>(s1[xt.i0]) * xt.w0 + static_cast<uint32_t>(s1[xt.i1]) * xt.w1;
                const uint32_t vTop = static_cast<uint32_t>(s0[xt.i0 + 1u]) * xt.w0 + static_cast<uint32_t>(s0[xt.i1 + 1u]) * xt.w1;
                const uint32_t vBot = static_cast<uint32_t>(s1[xt.i0 + 1u]) * xt.w0 + static_cast<uint32_t>(s1[xt.i1 + 1u]) * xt.w1;
                dRow[dx] = static_cast<uint8_t>((uTop * yt.w0 + uBot * yt.w1 + 32768u) >> 16);
                dRow[dx + 1u] = static_cast<uint8_t>((vTop * yt.w0 + vBot * yt.w1 + 32768u) >> 16);
            }
        }
    }

} // namespace

ZeroCamMediaStream::ZeroCamMediaStream(ZeroCamMediaSource* parent) : parent_(parent) {}

ZeroCamMediaStream::~ZeroCamMediaStream() {
    Shutdown();
}

HRESULT ZeroCamMediaStream::Initialize() {
    HRESULT hr = MFCreateEventQueue(&eventQueue_);
    if (FAILED(hr)) {
        return hr;
    }

    hr = MFCreateAttributes(&streamAttributes_, 8);
    if (FAILED(hr)) {
        return hr;
    }

    // Configura atributos requeridos por Windows Frame Server y soporte D3D11 zero-copy.
    auto applyStreamAttrs = [](IMFAttributes* attrs) -> HRESULT {
        HRESULT r = attrs->SetUINT32(MF_DEVICESTREAM_STREAM_ID, 0);
        if (FAILED(r)) return r;
        r = attrs->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, kPinNameVideoCapture);
        if (FAILED(r)) return r;
        r = attrs->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
        if (FAILED(r)) return r;
        r = attrs->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, static_cast<UINT32>(MFFrameSourceTypes_Color));
        if (FAILED(r)) return r;
        r = attrs->SetUINT32(MF_SA_D3D11_AWARE, 1);
        return r;
    };

    hr = applyStreamAttrs(streamAttributes_.Get());
    if (FAILED(hr)) {
        return hr;
    }

    // Tipos NV12 soportados (incluye 480p30 requerido por Discord/WebRTC además de 720p y 1080p).
    struct ModeDesc { UINT32 w, h, fps; };
    constexpr ModeDesc kModes[] = {
        { 1280, 720,  30 },
        { 1280, 720,  60 },
        { 1920, 1080, 30 },
        { 1920, 1080, 60 },
        { 640,  480,  30 }
    };

    Microsoft::WRL::ComPtr<IMFMediaType> types[std::size(kModes)];
    IMFMediaType* rawTypes[std::size(kModes)]{};
    for (size_t i = 0; i < std::size(kModes); ++i) {
        hr = CreateNv12MediaType(kModes[i].w, kModes[i].h, kModes[i].fps, 1, &types[i]);
        if (FAILED(hr)) {
            return hr;
        }
        rawTypes[i] = types[i].Get();
    }

    hr = MFCreateStreamDescriptor(0, static_cast<DWORD>(std::size(kModes)), rawTypes, &streamDescriptor_);
    if (FAILED(hr)) {
        return hr;
    }

    hr = applyStreamAttrs(streamDescriptor_.Get());
    if (FAILED(hr)) {
        return hr;
    }

    Microsoft::WRL::ComPtr<IMFMediaTypeHandler> handler;
    hr = streamDescriptor_->GetMediaTypeHandler(&handler);
    if (FAILED(hr)) {
        return hr;
    }

    hr = handler->SetCurrentMediaType(rawTypes[0]);
    if (FAILED(hr)) {
        return hr;
    }

    ipcOpened_ = ipc_.Open(false);
    if (ipcOpened_ && ipc_.Data()) {
        ipc_.Data()->consumerCount.fetch_add(1, std::memory_order_acq_rel);
        consumerRegistered_ = true;
    }

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    paceTimer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!paceTimer_) {
        paceTimer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }
    return S_OK;
}

HRESULT ZeroCamMediaStream::CreateNv12MediaType(UINT32 width, UINT32 height, UINT32 fpsNum, UINT32 fpsDen, IMFMediaType** ppType) const {
    if (!ppType) return E_POINTER;
    Microsoft::WRL::ComPtr<IMFMediaType> type;
    HRESULT hr = MFCreateMediaType(&type);
    if (FAILED(hr)) return hr;

    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    type->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    type->SetUINT32(MF_MT_DEFAULT_STRIDE, width);
    type->SetUINT32(MF_MT_SAMPLE_SIZE, width * height * 3u / 2u);
    type->SetUINT32(MF_MT_AVG_BITRATE, (width * height * 12u * fpsNum) / std::max<UINT32>(1, fpsDen));
    MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, fpsNum, fpsDen);
    MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE_RANGE_MIN, fpsNum, fpsDen);
    MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE_RANGE_MAX, fpsNum, fpsDen);
    MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    *ppType = type.Detach();
    return S_OK;
}

IFACEMETHODIMP ZeroCamMediaStream::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEventGenerator) ||
        riid == __uuidof(IMFMediaStream) || riid == __uuidof(IMFMediaStream2)) {
        *ppv = static_cast<IMFMediaStream2*>(this);
    } else if (riid == __uuidof(IKsControl)) {
        *ppv = static_cast<IKsControl*>(this);
    } else if (riid == __uuidof(IMFGetService)) {
        *ppv = static_cast<IMFGetService*>(this);
    } else {
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

IFACEMETHODIMP_(ULONG) ZeroCamMediaStream::AddRef() {
    return refCount_.fetch_add(1, std::memory_order_relaxed) + 1;
}

IFACEMETHODIMP_(ULONG) ZeroCamMediaStream::Release() {
    ULONG count = refCount_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (count == 0) {
        delete this;
    }
    return count;
}

IFACEMETHODIMP ZeroCamMediaStream::GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent) {
    if (!eventQueue_) return MF_E_SHUTDOWN;
    return eventQueue_->GetEvent(dwFlags, ppEvent);
}

IFACEMETHODIMP ZeroCamMediaStream::BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState) {
    if (!eventQueue_) return MF_E_SHUTDOWN;
    return eventQueue_->BeginGetEvent(pCallback, punkState);
}

IFACEMETHODIMP ZeroCamMediaStream::EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent) {
    if (!eventQueue_) return MF_E_SHUTDOWN;
    return eventQueue_->EndGetEvent(pResult, ppEvent);
}

IFACEMETHODIMP ZeroCamMediaStream::QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue) {
    if (!eventQueue_) return MF_E_SHUTDOWN;
    return eventQueue_->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue);
}

IFACEMETHODIMP ZeroCamMediaStream::GetMediaSource(IMFMediaSource** ppMediaSource) {
    if (!ppMediaSource) return E_POINTER;
    std::lock_guard<std::mutex> lock(mutex_);
    if (isShutdown_ || !parent_) return MF_E_SHUTDOWN;
    return parent_->QueryInterface(IID_PPV_ARGS(ppMediaSource));
}

IFACEMETHODIMP ZeroCamMediaStream::GetStreamDescriptor(IMFStreamDescriptor** ppStreamDescriptor) {
    if (!ppStreamDescriptor) return E_POINTER;
    std::lock_guard<std::mutex> lock(mutex_);
    if (isShutdown_ || !streamDescriptor_) return MF_E_SHUTDOWN;
    *ppStreamDescriptor = streamDescriptor_.Get();
    (*ppStreamDescriptor)->AddRef();
    return S_OK;
}

IFACEMETHODIMP ZeroCamMediaStream::SetStreamState(MF_STREAM_STATE value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (isShutdown_) return MF_E_SHUTDOWN;
    streamState_ = value;
    return S_OK;
}

IFACEMETHODIMP ZeroCamMediaStream::GetStreamState(MF_STREAM_STATE* value) {
    if (!value) return E_POINTER;
    std::lock_guard<std::mutex> lock(mutex_);
    if (isShutdown_) return MF_E_SHUTDOWN;
    *value = streamState_;
    return S_OK;
}

IFACEMETHODIMP ZeroCamMediaStream::KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG* BytesReturned) {
    if (BytesReturned) *BytesReturned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

IFACEMETHODIMP ZeroCamMediaStream::KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG* BytesReturned) {
    if (BytesReturned) *BytesReturned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

IFACEMETHODIMP ZeroCamMediaStream::KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG* BytesReturned) {
    if (BytesReturned) *BytesReturned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

IFACEMETHODIMP ZeroCamMediaStream::GetService(REFGUID guidService, REFIID riid, LPVOID* ppvObject) {
    if (!ppvObject) return E_POINTER;
    if (guidService == MF_MEDIASOURCE_SERVICE || guidService == GUID_NULL) {
        return QueryInterface(riid, ppvObject);
    }
    return MF_E_UNSUPPORTED_SERVICE;
}

HRESULT ZeroCamMediaStream::GetStreamAttributes(IMFAttributes** ppAttributes) {
    if (!ppAttributes) return E_POINTER;
    std::lock_guard<std::mutex> lock(mutex_);
    if (isShutdown_ || !streamAttributes_) return MF_E_SHUTDOWN;
    *ppAttributes = streamAttributes_.Get();
    (*ppAttributes)->AddRef();
    return S_OK;
}

HRESULT ZeroCamMediaStream::SetAllocator(IUnknown* pAllocator) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (isShutdown_) return MF_E_SHUTDOWN;
    if (allocator_) {
        allocator_->UninitializeSampleAllocator();
        allocator_.Reset();
    }
    allocatorInitialized_ = false;
    allocW_ = 0;
    allocH_ = 0;
    if (!pAllocator) {
        return S_OK;
    }
    HRESULT hr = pAllocator->QueryInterface(IID_PPV_ARGS(&allocator_));
    if (SUCCEEDED(hr) && dxgiManager_) {
        allocator_->SetDirectXManager(dxgiManager_.Get());
    }
    if (SUCCEEDED(hr) && streamState_ == MF_STREAM_STATE_RUNNING) {
        EnsureAllocatorInitialized();
    }
    return hr;
}

HRESULT ZeroCamMediaStream::SetD3DManager(IUnknown* pManager) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (isShutdown_) return MF_E_SHUTDOWN;

    if (dxgiManager_ && d3dDeviceHandle_) {
        dxgiManager_->CloseDeviceHandle(d3dDeviceHandle_);
        d3dDeviceHandle_ = nullptr;
    }
    dxgiManager_.Reset();
    if (allocator_) {
        allocator_->UninitializeSampleAllocator();
        allocatorInitialized_ = false;
        allocW_ = allocH_ = 0;
    }
    standbyGpuTex_.Reset();
    standbyDevice_.Reset();
    standbyW_ = standbyH_ = 0;
    cpuUploadTex_.Reset();
    cpuUploadDevice_.Reset();
    cpuUploadW_ = cpuUploadH_ = 0;
    cpuUploadIsStaging_ = false;
    scaleVp_.Reset();
    scaleEnum_.Reset();
    scaleVideoCtx_.Reset();
    scaleVideoDev_.Reset();
    scaleDevice_.Reset();
    scaleSrcW_ = scaleSrcH_ = scaleDstW_ = scaleDstH_ = 0;
    cachedDevice_.Reset();
    for (auto& t : sharedTextures_) t.Reset();
    for (auto& m : sharedMutexes_) m.Reset();
    for (auto& l : localPool_) l.Reset();
    cachedGeneration_ = 0;

    if (!pManager) {
        return S_OK;
    }

    HRESULT hr = pManager->QueryInterface(IID_PPV_ARGS(&dxgiManager_));
    if (FAILED(hr)) {
        return hr;
    }
    hr = dxgiManager_->OpenDeviceHandle(&d3dDeviceHandle_);
    if (FAILED(hr)) {
        dxgiManager_.Reset();
        return hr;
    }
    if (allocator_) {
        allocator_->SetDirectXManager(dxgiManager_.Get());
    }
    return S_OK;
}

HRESULT ZeroCamMediaStream::EnsureAllocatorInitialized() {
    if (!allocator_) {
        return S_OK;
    }
    if (allocatorInitialized_ && allocW_ == negotiatedWidth_ && allocH_ == negotiatedHeight_) {
        return S_OK;
    }

    Microsoft::WRL::ComPtr<IMFMediaType> curType;
    if (streamDescriptor_) {
        Microsoft::WRL::ComPtr<IMFMediaTypeHandler> handler;
        if (SUCCEEDED(streamDescriptor_->GetMediaTypeHandler(&handler))) {
            handler->GetCurrentMediaType(&curType);
        }
    }
    if (!curType) {
        HRESULT hrMt = CreateNv12MediaType(negotiatedWidth_, negotiatedHeight_, negotiatedFpsNum_, negotiatedFpsDen_, &curType);
        if (FAILED(hrMt) || !curType) {
            return FAILED(hrMt) ? hrMt : E_FAIL;
        }
    }

    allocator_->UninitializeSampleAllocator();
    if (dxgiManager_) {
        allocator_->SetDirectXManager(dxgiManager_.Get());
    }

    HRESULT hr = E_FAIL;
    Microsoft::WRL::ComPtr<IMFAttributes> allocAttrs;
    if (SUCCEEDED(MFCreateAttributes(&allocAttrs, 4))) {
        allocAttrs->SetUINT32(MF_SA_BUFFERS_PER_SAMPLE, 1);
        allocAttrs->SetUINT32(MF_SA_D3D11_USAGE, D3D11_USAGE_DEFAULT);
        allocAttrs->SetUINT32(MF_SA_D3D11_BINDFLAGS, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
        hr = allocator_->InitializeSampleAllocatorEx(1, 10, allocAttrs.Get(), curType.Get());
        if (FAILED(hr)) {
            allocAttrs->SetUINT32(MF_SA_D3D11_BINDFLAGS, D3D11_BIND_SHADER_RESOURCE);
            hr = allocator_->InitializeSampleAllocatorEx(1, 10, allocAttrs.Get(), curType.Get());
        }
    }
    if (FAILED(hr)) {
        hr = allocator_->InitializeSampleAllocatorEx(1, 10, nullptr, curType.Get());
    }
    if (FAILED(hr)) {
        hr = allocator_->InitializeSampleAllocator(10, curType.Get());
    }

    if (SUCCEEDED(hr)) {
        allocatorInitialized_ = true;
        allocW_ = negotiatedWidth_;
        allocH_ = negotiatedHeight_;
    }
    return hr;
}

HRESULT ZeroCamMediaStream::Start(const PROPVARIANT* pvarStartPosition) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (isShutdown_ || !eventQueue_) return MF_E_SHUTDOWN;

    // Lee el MediaType negociado del StreamDescriptor.
    if (streamDescriptor_) {
        Microsoft::WRL::ComPtr<IMFMediaTypeHandler> handler;
        Microsoft::WRL::ComPtr<IMFMediaType> curType;
        if (SUCCEEDED(streamDescriptor_->GetMediaTypeHandler(&handler)) &&
            SUCCEEDED(handler->GetCurrentMediaType(&curType))) {
            UINT32 w = 0, h = 0, fn = 30, fd = 1;
            if (SUCCEEDED(MFGetAttributeSize(curType.Get(), MF_MT_FRAME_SIZE, &w, &h)) && w > 0 && h > 0) {
                negotiatedWidth_  = w;
                negotiatedHeight_ = h;
            }
            if (SUCCEEDED(MFGetAttributeRatio(curType.Get(), MF_MT_FRAME_RATE, &fn, &fd)) && fn > 0 && fd > 0) {
                negotiatedFpsNum_ = fn;
                negotiatedFpsDen_ = fd;
            }
        }
    }

    if (allocator_) {
        HRESULT hrAlloc = EnsureAllocatorInitialized();
        if (FAILED(hrAlloc)) {
            return hrAlloc;
        }
    }

    streamState_ = MF_STREAM_STATE_RUNNING;
    nextSampleTimeHns_ = 0;
    lastDeliverTickMs_ = 0;
    return eventQueue_->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, pvarStartPosition);
}

HRESULT ZeroCamMediaStream::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (isShutdown_ || !eventQueue_) return MF_E_SHUTDOWN;
    streamState_ = MF_STREAM_STATE_STOPPED;
    return eventQueue_->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
}

HRESULT ZeroCamMediaStream::Pause() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (isShutdown_ || !eventQueue_) return MF_E_SHUTDOWN;
    streamState_ = MF_STREAM_STATE_PAUSED;
    return eventQueue_->QueueEventParamVar(MEStreamPaused, GUID_NULL, S_OK, nullptr);
}

HRESULT ZeroCamMediaStream::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (isShutdown_) return S_OK;
    isShutdown_ = true;
    streamState_ = MF_STREAM_STATE_STOPPED;

    if (consumerRegistered_ && ipc_.IsOpen() && ipc_.Data()) {
        ipc_.Data()->consumerCount.fetch_sub(1, std::memory_order_acq_rel);
        consumerRegistered_ = false;
    }
    ipc_.Close(false);
    ipcOpened_ = false;

    if (dxgiManager_ && d3dDeviceHandle_) {
        dxgiManager_->CloseDeviceHandle(d3dDeviceHandle_);
        d3dDeviceHandle_ = nullptr;
    }
    dxgiManager_.Reset();
    if (allocator_) {
        allocator_->UninitializeSampleAllocator();
        allocator_.Reset();
    }
    allocatorInitialized_ = false;
    allocW_ = allocH_ = 0;
    standbyGpuTex_.Reset();
    standbyDevice_.Reset();
    standbyW_ = standbyH_ = 0;
    cpuUploadTex_.Reset();
    cpuUploadDevice_.Reset();
    cpuUploadW_ = cpuUploadH_ = 0;
    cpuUploadIsStaging_ = false;
    scaleVp_.Reset();
    scaleEnum_.Reset();
    scaleVideoCtx_.Reset();
    scaleVideoDev_.Reset();
    scaleDevice_.Reset();
    scaleSrcW_ = scaleSrcH_ = scaleDstW_ = scaleDstH_ = 0;
    cachedDevice_.Reset();
    for (auto& t : sharedTextures_) t.Reset();
    for (auto& m : sharedMutexes_) m.Reset();
    for (auto& l : localPool_) l.Reset();

    if (eventQueue_) {
        eventQueue_->Shutdown();
        eventQueue_.Reset();
    }
    if (paceTimer_) {
        CloseHandle(paceTimer_);
        paceTimer_ = nullptr;
    }
    streamDescriptor_.Reset();
    streamAttributes_.Reset();
    parent_ = nullptr;
    return S_OK;
}

bool ZeroCamMediaStream::EnsureSharedTextures(ID3D11Device* dev, ZeroCamSharedMemory* ipc) {
    const uint32_t gen = ipc->gpuHandlesGeneration.load(std::memory_order_acquire);
    if (gen == 0) {
        return false;
    }
    if (cachedDevice_.Get() == dev && cachedGeneration_ == gen && sharedTextures_[0] != nullptr) {
        return true;
    }

    // Verifica que ambos ID3D11Device estén en el mismo adaptador físico (LUID).
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDev;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dxgiDev))) ||
        FAILED(dxgiDev->GetAdapter(&adapter)) ||
        FAILED(adapter->GetDesc(&desc))) {
        return false;
    }
    if (desc.AdapterLuid.LowPart != ipc->adapterLuidLow.load(std::memory_order_acquire) ||
        desc.AdapterLuid.HighPart != ipc->adapterLuidHigh.load(std::memory_order_acquire)) {
        return false;
    }

    for (auto& t : sharedTextures_) t.Reset();
    for (auto& m : sharedMutexes_) m.Reset();
    cachedDevice_ = dev;
    sharedIsKeyed_ = (ipc->usesKeyedMutex.load(std::memory_order_acquire) != 0);

    for (uint32_t i = 0; i < kRingSlots; ++i) {
        const HANDLE h = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(ipc->dxgiSharedHandles[i].load(std::memory_order_acquire)));
        if (!h || FAILED(dev->OpenSharedResource(h, IID_PPV_ARGS(&sharedTextures_[i])))) {
            for (auto& t : sharedTextures_) t.Reset();
            for (auto& m : sharedMutexes_) m.Reset();
            return false;
        }
        if (sharedIsKeyed_) {
            sharedTextures_[i].As(&sharedMutexes_[i]);
        }
    }
    cachedGeneration_ = gen;
    return true;
}

bool ZeroCamMediaStream::EnsureLocalPool(ID3D11Device* dev, UINT32 width, UINT32 height) {
    if (localPoolW_ == width && localPoolH_ == height && localPool_[0] != nullptr) {
        return true;
    }
    for (auto& l : localPool_) l.Reset();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    for (uint32_t i = 0; i < kLocalPoolCount; ++i) {
        if (FAILED(dev->CreateTexture2D(&desc, nullptr, &localPool_[i]))) {
            for (auto& l : localPool_) l.Reset();
            return false;
        }
    }
    localPoolW_ = width;
    localPoolH_ = height;
    return true;
}

bool ZeroCamMediaStream::EnsureStandbyGpuTex(ID3D11Device* dev, UINT32 width, UINT32 height) {
    if (standbyDevice_.Get() == dev && standbyW_ == width && standbyH_ == height && standbyGpuTex_ != nullptr) {
        return true;
    }
    standbyGpuTex_.Reset();
    standbyDevice_ = dev;

    std::vector<uint8_t> initBuf(static_cast<size_t>(width) * height * 3u / 2u);
    std::memset(initBuf.data(), 16, static_cast<size_t>(width) * height);
    std::memset(initBuf.data() + static_cast<size_t>(width) * height, 128, (static_cast<size_t>(width) * height) / 2);

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA initData{};
    initData.pSysMem = initBuf.data();
    initData.SysMemPitch = width;

    if (FAILED(dev->CreateTexture2D(&desc, &initData, &standbyGpuTex_))) {
        standbyW_ = standbyH_ = 0;
        return false;
    }
    standbyW_ = width;
    standbyH_ = height;
    return true;
}

bool ZeroCamMediaStream::EnsureCpuUploadTex(ID3D11Device* dev, UINT32 width, UINT32 height, bool& outIsStaging) {
    if (cpuUploadDevice_.Get() == dev && cpuUploadW_ == width && cpuUploadH_ == height && cpuUploadTex_ != nullptr) {
        outIsStaging = cpuUploadIsStaging_;
        return true;
    }
    cpuUploadTex_.Reset();
    cpuUploadDevice_ = dev;
    cpuUploadW_ = 0;
    cpuUploadH_ = 0;
    cpuUploadIsStaging_ = false;

    D3D11_TEXTURE2D_DESC stDesc{};
    stDesc.Width = width;
    stDesc.Height = height;
    stDesc.MipLevels = 1;
    stDesc.ArraySize = 1;
    stDesc.Format = DXGI_FORMAT_NV12;
    stDesc.SampleDesc.Count = 1;
    stDesc.Usage = D3D11_USAGE_STAGING;
    stDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    if (SUCCEEDED(dev->CreateTexture2D(&stDesc, nullptr, &cpuUploadTex_))) {
        cpuUploadW_ = width;
        cpuUploadH_ = height;
        cpuUploadIsStaging_ = true;
        outIsStaging = true;
        return true;
    }

    D3D11_TEXTURE2D_DESC defDesc{};
    defDesc.Width = width;
    defDesc.Height = height;
    defDesc.MipLevels = 1;
    defDesc.ArraySize = 1;
    defDesc.Format = DXGI_FORMAT_NV12;
    defDesc.SampleDesc.Count = 1;
    defDesc.Usage = D3D11_USAGE_DEFAULT;
    defDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    if (SUCCEEDED(dev->CreateTexture2D(&defDesc, nullptr, &cpuUploadTex_))) {
        cpuUploadW_ = width;
        cpuUploadH_ = height;
        cpuUploadIsStaging_ = false;
        outIsStaging = false;
        return true;
    }
    return false;
}

bool ZeroCamMediaStream::EnsureGpuScaleProcessor(ID3D11Device* dev, UINT32 srcW, UINT32 srcH, UINT32 dstW, UINT32 dstH) {
    if (!dev || srcW == 0 || srcH == 0 || dstW == 0 || dstH == 0) return false;
    if (scaleDevice_.Get() == dev && scaleVp_ && scaleEnum_ &&
        scaleSrcW_ == srcW && scaleSrcH_ == srcH && scaleDstW_ == dstW && scaleDstH_ == dstH) {
        return true;
    }

    if (scaleDevice_.Get() != dev || !scaleVideoDev_ || !scaleVideoCtx_) {
        scaleVideoDev_.Reset();
        scaleVideoCtx_.Reset();
        scaleDevice_ = dev;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx;
        dev->GetImmediateContext(&ctx);
        if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&scaleVideoDev_))) || !ctx ||
            FAILED(ctx.As(&scaleVideoCtx_))) {
            scaleVideoDev_.Reset();
            scaleVideoCtx_.Reset();
            return false;
        }
    }

    scaleVp_.Reset();
    scaleEnum_.Reset();

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cDesc{};
    cDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cDesc.InputWidth = srcW;
    cDesc.InputHeight = srcH;
    cDesc.OutputWidth = dstW;
    cDesc.OutputHeight = dstH;
    cDesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    if (FAILED(scaleVideoDev_->CreateVideoProcessorEnumerator(&cDesc, &scaleEnum_)) ||
        FAILED(scaleVideoDev_->CreateVideoProcessor(scaleEnum_.Get(), 0, &scaleVp_))) {
        scaleVp_.Reset();
        scaleEnum_.Reset();
        return false;
    }
    scaleSrcW_ = srcW;
    scaleSrcH_ = srcH;
    scaleDstW_ = dstW;
    scaleDstH_ = dstH;
    return true;
}

HRESULT ZeroCamMediaStream::TryCreateGpuSample(ZeroCamSharedMemory* ipc, IMFSample** ppSample, bool& usedGpu) {
    usedGpu = false;
    *ppSample = nullptr;

    if (!dxgiManager_ || !d3dDeviceHandle_ || !ipc) {
        return S_FALSE;
    }
    if (ipc->producerAlive.load(std::memory_order_acquire) == 0 ||
        ipc->frameSequence.load(std::memory_order_acquire) == 0) {
        return S_FALSE;
    }

    DxgiDeviceLock lock(dxgiManager_.Get(), d3dDeviceHandle_);
    Microsoft::WRL::ComPtr<ID3D11Device> dev;
    if (FAILED(lock.Lock(&dev))) {
        return S_FALSE;
    }

    if (!EnsureSharedTextures(dev.Get(), ipc)) {
        return S_FALSE;
    }

    const uint32_t slot = ipc->activeSlot.load(std::memory_order_acquire) % kRingSlots;
    ID3D11Texture2D* srcTex = sharedTextures_[slot].Get();
    if (!srcTex) {
        return S_FALSE;
    }

    D3D11_TEXTURE2D_DESC srcDesc{};
    srcTex->GetDesc(&srcDesc);
    if (srcDesc.Format != DXGI_FORMAT_NV12 || srcDesc.Width < 2u || srcDesc.Height < 2u) {
        return S_FALSE;
    }

    Microsoft::WRL::ComPtr<IMFSample> sample;
    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> dstTex;
    UINT dstSub = 0;

    if (allocator_) {
        if (!allocatorInitialized_ && FAILED(EnsureAllocatorInitialized())) {
            return S_FALSE;
        }
        HRESULT hrAlloc = allocator_->AllocateSample(&sample);
        if (hrAlloc == MF_E_NOT_INITIALIZED) {
            allocatorInitialized_ = false;
            if (SUCCEEDED(EnsureAllocatorInitialized())) {
                hrAlloc = allocator_->AllocateSample(&sample);
            }
        }
        if (FAILED(hrAlloc) || !sample) {
            return S_FALSE;
        }
        Microsoft::WRL::ComPtr<IMFDXGIBuffer> dxgiBuf;
        if (FAILED(sample->GetBufferByIndex(0, &buffer)) || FAILED(buffer.As(&dxgiBuf)) ||
            FAILED(dxgiBuf->GetResource(IID_PPV_ARGS(&dstTex))) || FAILED(dxgiBuf->GetSubresourceIndex(&dstSub))) {
            return S_FALSE;
        }
    } else {
        if (!EnsureLocalPool(dev.Get(), srcDesc.Width, srcDesc.Height)) {
            return S_FALSE;
        }
        dstTex = localPool_[localPoolIdx_ % kLocalPoolCount];
        localPoolIdx_ = (localPoolIdx_ + 1) % kLocalPoolCount;
    }

    D3D11_TEXTURE2D_DESC dstDesc{};
    dstTex->GetDesc(&dstDesc);
    const UINT copyW = std::min(srcDesc.Width, dstDesc.Width);
    const UINT copyH = std::min(srcDesc.Height, dstDesc.Height);

    IDXGIKeyedMutex* km = sharedMutexes_[slot].Get();
    if (sharedIsKeyed_ && km) {
        if (FAILED(km->AcquireSync(0, 16))) {
            return S_FALSE;
        }
    }

    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx;
    dev->GetImmediateContext(&ctx);
    bool gpuCopied = false;
    if (srcDesc.Width != dstDesc.Width || srcDesc.Height != dstDesc.Height) {
        if (EnsureGpuScaleProcessor(dev.Get(), srcDesc.Width, srcDesc.Height, dstDesc.Width, dstDesc.Height)) {
            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivDesc{};
            ivDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
            ivDesc.Texture2D.MipSlice = 0;
            ivDesc.Texture2D.ArraySlice = 0;

            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovDesc{};
            if (dstDesc.ArraySize > 1) {
                ovDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2DARRAY;
                ovDesc.Texture2DArray.MipSlice = 0;
                ovDesc.Texture2DArray.FirstArraySlice = dstSub;
                ovDesc.Texture2DArray.ArraySize = 1;
            } else {
                ovDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
                ovDesc.Texture2D.MipSlice = 0;
            }

            Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> inView;
            Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> outView;
            if (SUCCEEDED(scaleVideoDev_->CreateVideoProcessorInputView(srcTex, scaleEnum_.Get(), &ivDesc, &inView)) &&
                SUCCEEDED(scaleVideoDev_->CreateVideoProcessorOutputView(dstTex.Get(), scaleEnum_.Get(), &ovDesc, &outView))) {
                RECT sRc{ 0, 0, static_cast<LONG>(srcDesc.Width), static_cast<LONG>(srcDesc.Height) };
                RECT dRc{ 0, 0, static_cast<LONG>(dstDesc.Width), static_cast<LONG>(dstDesc.Height) };
                scaleVideoCtx_->VideoProcessorSetStreamFrameFormat(scaleVp_.Get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
                scaleVideoCtx_->VideoProcessorSetStreamSourceRect(scaleVp_.Get(), 0, TRUE, &sRc);
                scaleVideoCtx_->VideoProcessorSetStreamDestRect(scaleVp_.Get(), 0, TRUE, &dRc);

                D3D11_VIDEO_PROCESSOR_STREAM stream{};
                stream.Enable = TRUE;
                stream.pInputSurface = inView.Get();
                gpuCopied = SUCCEEDED(scaleVideoCtx_->VideoProcessorBlt(scaleVp_.Get(), outView.Get(), 0, 1, &stream));
            }
        }
    }
    if (!gpuCopied) {
        D3D11_BOX srcBox{ 0, 0, 0, copyW, copyH, 1 };
        ctx->CopySubresourceRegion(dstTex.Get(), dstSub, 0, 0, 0, srcTex, 0, &srcBox);
    }
    ctx->Flush();

    if (sharedIsKeyed_ && km) {
        km->ReleaseSync(0);
    }

    if (!allocator_) {
        HRESULT hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), dstTex.Get(), 0, FALSE, &buffer);
        if (FAILED(hr)) {
            return S_FALSE;
        }
        Microsoft::WRL::ComPtr<IMF2DBuffer> buf2d;
        if (SUCCEEDED(buffer.As(&buf2d))) {
            DWORD contigLen = 0;
            if (SUCCEEDED(buf2d->GetContiguousLength(&contigLen))) {
                buffer->SetCurrentLength(contigLen);
            }
        }
        hr = MFCreateSample(&sample);
        if (FAILED(hr)) return hr;
        hr = sample->AddBuffer(buffer.Get());
        if (FAILED(hr)) return hr;
    } else if (buffer) {
        Microsoft::WRL::ComPtr<IMF2DBuffer> buf2d;
        if (SUCCEEDED(buffer.As(&buf2d))) {
            DWORD contigLen = 0;
            if (SUCCEEDED(buf2d->GetContiguousLength(&contigLen))) {
                buffer->SetCurrentLength(contigLen);
            }
        }
    }

    usedGpu = true;
    *ppSample = sample.Detach();
    return S_OK;
}

HRESULT ZeroCamMediaStream::CreateCpuOrStandbySample(ZeroCamSharedMemory* ipc, IMFSample** ppSample) {
    *ppSample = nullptr;
    UINT32 w = negotiatedWidth_;
    UINT32 h = negotiatedHeight_;
    bool hasCpuFrame = false;
    uint32_t cpuSlot = 0;
    uint32_t srcStride = w;

    if (ipc && ipc->producerAlive.load(std::memory_order_acquire) != 0 &&
        ipc->frameSequence.load(std::memory_order_acquire) != 0) {
        // Lectura única atómica de (width|height|stride|slot) para prevenir carreras 720p↔1080p y lecturas OOB.
        const uint64_t meta = ipc->cpuFrameMeta.load(std::memory_order_acquire);
        uint32_t mw = 0, mh = 0, mStride = 0, mSlot = 0;
        if (UnpackCpuFrameMeta(meta, mw, mh, mStride, mSlot) &&
            mw >= 2u && mw <= kMaxCpuWidth && (mw & 1u) == 0u &&
            mh >= 2u && mh <= kMaxCpuHeight && (mh & 1u) == 0u &&
            mStride >= mw && mStride <= kMaxCpuWidth &&
            static_cast<size_t>(mStride) * (static_cast<size_t>(mh) + static_cast<size_t>(mh) / 2u) <= kCpuSlotBytes) {
            w = mw;
            h = mh;
            srcStride = mStride;
            cpuSlot = mSlot;
            hasCpuFrame = true;
        }
    }

    Microsoft::WRL::ComPtr<IMFSample> sample;
    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = S_OK;

    if (allocator_) {
        if (!allocatorInitialized_) {
            hr = EnsureAllocatorInitialized();
            if (FAILED(hr)) return hr;
        }
        hr = allocator_->AllocateSample(&sample);
        if (hr == MF_E_NOT_INITIALIZED) {
            allocatorInitialized_ = false;
            hr = EnsureAllocatorInitialized();
            if (SUCCEEDED(hr)) {
                hr = allocator_->AllocateSample(&sample);
            }
        }
        if (FAILED(hr) || !sample) return FAILED(hr) ? hr : E_FAIL;
        hr = sample->GetBufferByIndex(0, &buffer);
        if (FAILED(hr)) return hr;

        Microsoft::WRL::ComPtr<IMFDXGIBuffer> dxgiBuf;
        if (SUCCEEDED(buffer.As(&dxgiBuf))) {
            Microsoft::WRL::ComPtr<ID3D11Texture2D> dstTex;
            UINT dstSub = 0;
            if (SUCCEEDED(dxgiBuf->GetResource(IID_PPV_ARGS(&dstTex))) &&
                SUCCEEDED(dxgiBuf->GetSubresourceIndex(&dstSub))) {
                D3D11_TEXTURE2D_DESC dstDesc{};
                dstTex->GetDesc(&dstDesc);
                Microsoft::WRL::ComPtr<ID3D11Device> dev;
                DxgiDeviceLock d3dLock(dxgiManager_.Get(), d3dDeviceHandle_);
                if (!dxgiManager_ || !d3dDeviceHandle_ || FAILED(d3dLock.Lock(&dev))) {
                    dstTex->GetDevice(&dev);
                }
                if (dev && dstDesc.Width >= 2u && dstDesc.Height >= 2u &&
                    (dstDesc.Width & 1u) == 0u && (dstDesc.Height & 1u) == 0u) {
                    Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx;
                    dev->GetImmediateContext(&ctx);
                    bool uploadedFrame = false;
                    if (hasCpuFrame) {
                        bool isStaging = false;
                        if (EnsureCpuUploadTex(dev.Get(), dstDesc.Width, dstDesc.Height, isStaging)) {
                            if (isStaging) {
                                D3D11_MAPPED_SUBRESOURCE mapped{};
                                if (SUCCEEDED(ctx->Map(cpuUploadTex_.Get(), 0, D3D11_MAP_WRITE, 0, &mapped))) {
                                    const bool pitchOk = (mapped.pData != nullptr && mapped.RowPitch >= dstDesc.Width);
                                    if (pitchOk) {
                                        ScaleNv12Cpu(
                                            ipc->cpuNv12Buffer[cpuSlot], srcStride, w, h,
                                            static_cast<uint8_t*>(mapped.pData), static_cast<uint32_t>(mapped.RowPitch),
                                            dstDesc.Width, dstDesc.Height);
                                    }
                                    ctx->Unmap(cpuUploadTex_.Get(), 0);
                                    if (pitchOk) {
                                        ctx->CopySubresourceRegion(dstTex.Get(), dstSub, 0, 0, 0, cpuUploadTex_.Get(), 0, nullptr);
                                        uploadedFrame = true;
                                    }
                                }
                            }
                            if (!uploadedFrame) {
                                scaledNv12Scratch_.resize(static_cast<size_t>(dstDesc.Width) * dstDesc.Height * 3u / 2u);
                                ScaleNv12Cpu(
                                    ipc->cpuNv12Buffer[cpuSlot], srcStride, w, h,
                                    scaledNv12Scratch_.data(), dstDesc.Width, dstDesc.Width, dstDesc.Height);
                                if (!isStaging) {
                                    ctx->UpdateSubresource(cpuUploadTex_.Get(), 0, nullptr, scaledNv12Scratch_.data(), dstDesc.Width, 0);
                                    ctx->CopySubresourceRegion(dstTex.Get(), dstSub, 0, 0, 0, cpuUploadTex_.Get(), 0, nullptr);
                                } else {
                                    ctx->UpdateSubresource(dstTex.Get(), dstSub, nullptr, scaledNv12Scratch_.data(), dstDesc.Width, 0);
                                }
                                uploadedFrame = true;
                            }
                        }
                    }
                    if (!uploadedFrame && EnsureStandbyGpuTex(dev.Get(), dstDesc.Width, dstDesc.Height)) {
                        ctx->CopySubresourceRegion(dstTex.Get(), dstSub, 0, 0, 0, standbyGpuTex_.Get(), 0, nullptr);
                        uploadedFrame = true;
                    }
                    if (uploadedFrame) {
                        ctx->Flush();
                        Microsoft::WRL::ComPtr<IMF2DBuffer> buf2d;
                        if (SUCCEEDED(buffer.As(&buf2d))) {
                            DWORD contigLen = 0;
                            if (SUCCEEDED(buf2d->GetContiguousLength(&contigLen))) {
                                buffer->SetCurrentLength(contigLen);
                            }
                        }
                        *ppSample = sample.Detach();
                        return S_OK;
                    }
                }
            }
        }
    } else {
        hr = MFCreate2DMediaBuffer(w, h, MFVideoFormat_NV12.Data1, FALSE, &buffer);
        if (FAILED(hr)) return hr;
    }

    Microsoft::WRL::ComPtr<IMF2DBuffer> buf2d;
    hr = buffer.As(&buf2d);
    if (FAILED(hr)) {
        return hr;
    }

    BYTE* dstScanline = nullptr;
    LONG dstPitch = 0;
    hr = buf2d->Lock2D(&dstScanline, &dstPitch);
    if (FAILED(hr)) {
        return hr;
    }

    const UINT32 fillW = allocator_ ? negotiatedWidth_ : w;
    const UINT32 fillH = allocator_ ? negotiatedHeight_ : h;
    if (!dstScanline || dstPitch < static_cast<LONG>(fillW) ||
        fillW < 2u || fillH < 2u || (fillW & 1u) != 0u || (fillH & 1u) != 0u) {
        buf2d->Unlock2D();
        return E_UNEXPECTED;
    }

    const size_t dstPitchSz = static_cast<size_t>(dstPitch);
    if (hasCpuFrame) {
        ScaleNv12Cpu(
            ipc->cpuNv12Buffer[cpuSlot], srcStride, w, h,
            dstScanline, static_cast<uint32_t>(dstPitch), fillW, fillH);
    } else {
        // Frame standby limpio BT.709 negro sólido (Y=16, UV=128) cuando el productor está apagado.
        for (UINT32 y = 0; y < fillH; ++y) {
            std::memset(dstScanline + static_cast<size_t>(y) * dstPitchSz, 16, fillW);
        }
        BYTE* dstUv = dstScanline + dstPitchSz * fillH;
        for (UINT32 y = 0; y < fillH / 2u; ++y) {
            std::memset(dstUv + static_cast<size_t>(y) * dstPitchSz, 128, fillW);
        }
    }

    buf2d->Unlock2D();
    buffer->SetCurrentLength(static_cast<DWORD>(static_cast<size_t>(fillW) * fillH * 3u / 2u));

    if (!sample) {
        hr = MFCreateSample(&sample);
        if (FAILED(hr)) return hr;
        hr = sample->AddBuffer(buffer.Get());
        if (FAILED(hr)) return hr;
    }

    *ppSample = sample.Detach();
    return S_OK;
}

void ZeroCamMediaStream::PaceNextFrame(ZeroCamSharedMemory* ipc, UINT32 fpsNum, UINT32 fpsDen) {
    const uint64_t currentSeq = ipc ? ipc->frameSequence.load(std::memory_order_acquire) : 0ull;
    if (currentSeq != 0 && currentSeq != lastSeqDelivered_) {
        lastSeqDelivered_ = currentSeq;
        lastDeliverTickMs_ = GetTickCount64();
        return;
    }
    const ULONGLONG rawFrameMs = (1000ull * static_cast<ULONGLONG>(std::max<UINT32>(1u, fpsDen))) /
                                 static_cast<ULONGLONG>(std::max<UINT32>(1u, fpsNum));
    const ULONGLONG frameMs = std::clamp<ULONGLONG>(rawFrameMs, 4ull, 100ull);
    const ULONGLONG deadlineMs = (lastDeliverTickMs_ != 0) ? (lastDeliverTickMs_ + frameMs) : 0ull;

    while (deadlineMs != 0 && !isShutdown_) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadlineMs) {
            break;
        }
        const uint64_t seqNow = ipc ? ipc->frameSequence.load(std::memory_order_acquire) : 0ull;
        if (seqNow != 0 && seqNow != lastSeqDelivered_) {
            lastSeqDelivered_ = seqNow;
            break;
        }
        if (paceTimer_) {
            LARGE_INTEGER due{};
            due.QuadPart = -20000LL; // 2 ms
            SetWaitableTimer(paceTimer_, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(paceTimer_, 5);
        } else {
            Sleep(2);
        }
    }
    lastDeliverTickMs_ = GetTickCount64();
}

IFACEMETHODIMP ZeroCamMediaStream::RequestSample(IUnknown* pToken) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (isShutdown_ || !eventQueue_) return MF_E_SHUTDOWN;
    if (streamState_ == MF_STREAM_STATE_STOPPED) return MF_E_INVALIDREQUEST;

    if (!ipcOpened_) {
        ipcOpened_ = ipc_.Open(false);
        if (ipcOpened_ && ipc_.Data() && !consumerRegistered_) {
            ipc_.Data()->consumerCount.fetch_add(1, std::memory_order_acq_rel);
            consumerRegistered_ = true;
        }
    }

    ZeroCamSharedMemory* ipc = ipcOpened_ ? ipc_.Data() : nullptr;
    UINT32 fpsNum = negotiatedFpsNum_;
    UINT32 fpsDen = negotiatedFpsDen_;
    if (ipc) {
        const uint32_t rawNum = ipc->fpsNum.load(std::memory_order_acquire);
        const uint32_t rawDen = ipc->fpsDen.load(std::memory_order_acquire);
        if (rawNum >= 1u && rawNum <= 240u && rawDen >= 1u && rawDen <= 1001u) {
            fpsNum = rawNum;
            fpsDen = rawDen;
        }
    }
    PaceNextFrame(ipc, fpsNum, fpsDen);

    Microsoft::WRL::ComPtr<IMFSample> sample;
    bool usedGpu = false;
    HRESULT hr = TryCreateGpuSample(ipc, &sample, usedGpu);

    if (ipc && ipc->producerAlive.load(std::memory_order_acquire) != 0 && !usedGpu) {
        // Solicita al productor volcar NV12 solo si la ruta zero-copy GPU no está disponible en este consumidor.
        ipc->clientNeedsCpuFallback.store(1, std::memory_order_release);
    } else if (ipc && usedGpu) {
        ipc->clientNeedsCpuFallback.store(0, std::memory_order_release);
    }

    if (!sample) {
        hr = CreateCpuOrStandbySample(ipc, &sample);
        if (FAILED(hr)) {
            return hr;
        }
    }

    const int64_t durationHns = (10000000LL * fpsDen) / std::max<UINT32>(1, fpsNum);
    const LONGLONG nowHns = MFGetSystemTime();
    const LONGLONG sampleTimeHns = (nextSampleTimeHns_ > 0 && nowHns <= nextSampleTimeHns_)
        ? (nextSampleTimeHns_ + 1)
        : nowHns;
    sample->SetSampleTime(sampleTimeHns);
    sample->SetSampleDuration(durationHns);
    sample->SetUINT32(MFSampleExtension_CleanPoint, 1);
    nextSampleTimeHns_ = sampleTimeHns;

    if (pToken) {
        sample->SetUnknown(MFSampleExtension_Token, pToken);
    }

    return eventQueue_->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get());
}

} // namespace zerocam
