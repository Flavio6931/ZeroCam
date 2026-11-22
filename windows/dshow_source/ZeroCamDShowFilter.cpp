#include "ZeroCamDShowFilter.h"
#include "../shared/ZeroCamGuids.h"
#include <algorithm>
#include <cstring>
#include <cwchar>
#include <vector>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace zerocam {

extern void DShowModuleAddRef() noexcept;
extern void DShowModuleRelease() noexcept;

namespace {

struct DShowModeDesc {
    UINT32 w;
    UINT32 h;
    UINT32 fps;
    bool yuy2;
};

constexpr DShowModeDesc kDShowModes[] = {
    { 1280, 720,  30, false },
    { 1280, 720,  30, true  },
    { 1920, 1080, 30, false },
    { 1920, 1080, 30, true  },
    { 1280, 720,  60, false },
    { 1280, 720,  60, true  },
    { 1920, 1080, 60, false },
    { 1920, 1080, 60, true  },
    { 640,  480,  30, false },
    { 640,  480,  30, true  }
};

void FreeMediaTypeFields(AM_MEDIA_TYPE& mt) noexcept {
    if (mt.cbFormat != 0 && mt.pbFormat != nullptr) {
        CoTaskMemFree(mt.pbFormat);
        mt.pbFormat = nullptr;
        mt.cbFormat = 0;
    }
    if (mt.pUnk != nullptr) {
        mt.pUnk->Release();
        mt.pUnk = nullptr;
    }
}

void DeleteMediaTypePtr(AM_MEDIA_TYPE* pmt) noexcept {
    if (!pmt) return;
    FreeMediaTypeFields(*pmt);
    CoTaskMemFree(pmt);
}

HRESULT CopyMediaTypeFields(AM_MEDIA_TYPE& dst, const AM_MEDIA_TYPE& src) {
    FreeMediaTypeFields(dst);
    dst = src;
    dst.pbFormat = nullptr;
    dst.pUnk = nullptr;
    if (src.cbFormat > 0 && src.pbFormat != nullptr) {
        dst.pbFormat = static_cast<BYTE*>(CoTaskMemAlloc(src.cbFormat));
        if (!dst.pbFormat) {
            dst.cbFormat = 0;
            return E_OUTOFMEMORY;
        }
        std::memcpy(dst.pbFormat, src.pbFormat, src.cbFormat);
    } else {
        dst.cbFormat = 0;
    }
    if (src.pUnk) {
        dst.pUnk = src.pUnk;
        dst.pUnk->AddRef();
    }
    return S_OK;
}

AM_MEDIA_TYPE* CloneMediaTypePtr(const AM_MEDIA_TYPE& src) {
    auto* mt = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
    if (!mt) return nullptr;
    std::memset(mt, 0, sizeof(AM_MEDIA_TYPE));
    if (FAILED(CopyMediaTypeFields(*mt, src))) {
        CoTaskMemFree(mt);
        return nullptr;
    }
    return mt;
}

LPWSTR DupWideStringCoTask(const wchar_t* src) {
    const size_t len = std::wcslen(src) + 1u;
    auto* out = static_cast<LPWSTR>(CoTaskMemAlloc(len * sizeof(wchar_t)));
    if (out) {
        std::memcpy(out, src, len * sizeof(wchar_t));
    }
    return out;
}

// Convierte y reescala un frame NV12 en CPU hacia un buffer NV12 destino con stride dstStride.
void WriteNv12FromCpu(
    const uint8_t* src,
    uint32_t srcStride,
    uint32_t srcW,
    uint32_t srcH,
    uint8_t* dst,
    uint32_t dstStride,
    uint32_t dstW,
    uint32_t dstH,
    std::vector<uint32_t>& xMapY,
    std::vector<uint32_t>& xMapUv) {
    if (!src || !dst || srcW < 2u || srcH < 2u || dstW < 2u || dstH < 2u ||
        (srcW & 1u) != 0u || (srcH & 1u) != 0u || (dstW & 1u) != 0u || (dstH & 1u) != 0u ||
        srcStride < srcW || dstStride < dstW) {
        return;
    }
    const uint8_t* srcUv = src + static_cast<size_t>(srcStride) * srcH;
    uint8_t* dstUv = dst + static_cast<size_t>(dstStride) * dstH;

    if (srcW == dstW && srcH == dstH) {
        if (srcStride == dstW && dstStride == dstW) {
            const size_t yBytes = static_cast<size_t>(dstW) * dstH;
            std::memcpy(dst, src, yBytes);
            std::memcpy(dstUv, srcUv, yBytes / 2u);
            return;
        }
        for (uint32_t y = 0; y < dstH; ++y) {
            std::memcpy(dst + static_cast<size_t>(y) * dstStride, src + static_cast<size_t>(y) * srcStride, dstW);
        }
        for (uint32_t y = 0; y < dstH / 2u; ++y) {
            std::memcpy(dstUv + static_cast<size_t>(y) * dstStride, srcUv + static_cast<size_t>(y) * srcStride, dstW);
        }
        return;
    }

    if (xMapY.size() != dstW) {
        xMapY.resize(dstW);
        for (uint32_t x = 0; x < dstW; ++x) {
            xMapY[x] = std::min(srcW - 1u, static_cast<uint32_t>((static_cast<uint64_t>(x) * srcW) / dstW));
        }
    }
    for (uint32_t y = 0; y < dstH; ++y) {
        const uint32_t sy = std::min(srcH - 1u, static_cast<uint32_t>((static_cast<uint64_t>(y) * srcH) / dstH));
        const uint8_t* sRow = src + static_cast<size_t>(sy) * srcStride;
        uint8_t* dRow = dst + static_cast<size_t>(y) * dstStride;
        for (uint32_t x = 0; x < dstW; ++x) {
            dRow[x] = sRow[xMapY[x]];
        }
    }

    const uint32_t srcPairsW = std::max(1u, srcW / 2u);
    const uint32_t srcPairsH = std::max(1u, srcH / 2u);
    const uint32_t dstPairsW = dstW / 2u;
    const uint32_t dstPairsH = dstH / 2u;
    if (xMapUv.size() != dstPairsW) {
        xMapUv.resize(dstPairsW);
        for (uint32_t cx = 0; cx < dstPairsW; ++cx) {
            xMapUv[cx] = std::min(srcPairsW - 1u, static_cast<uint32_t>((static_cast<uint64_t>(cx) * srcPairsW) / std::max(1u, dstPairsW))) * 2u;
        }
    }
    for (uint32_t cy = 0; cy < dstPairsH; ++cy) {
        const uint32_t sy = std::min(srcPairsH - 1u, static_cast<uint32_t>((static_cast<uint64_t>(cy) * srcPairsH) / std::max(1u, dstPairsH)));
        const uint8_t* sRow = srcUv + static_cast<size_t>(sy) * srcStride;
        uint8_t* dRow = dstUv + static_cast<size_t>(cy) * dstStride;
        for (uint32_t cx = 0; cx < dstPairsW; ++cx) {
            const size_t sx = xMapUv[cx];
            const size_t dx = static_cast<size_t>(cx) * 2u;
            dRow[dx] = sRow[sx];
            dRow[dx + 1u] = sRow[sx + 1u];
        }
    }
}

// Convierte y reescala un frame NV12 en CPU directamente a YUY2 empaquetado (Y0, U, Y1, V).
void WriteYuy2FromNv12Cpu(
    const uint8_t* src,
    uint32_t srcStride,
    uint32_t srcW,
    uint32_t srcH,
    uint8_t* dst,
    uint32_t dstStrideBytes,
    uint32_t dstW,
    uint32_t dstH,
    std::vector<uint32_t>& xMapPairs) {
    if (!src || !dst || srcW < 2u || srcH < 2u || dstW < 2u || dstH < 2u ||
        (srcW & 1u) != 0u || (srcH & 1u) != 0u || (dstW & 1u) != 0u || (dstH & 1u) != 0u ||
        srcStride < srcW || dstStrideBytes < dstW * 2u) {
        return;
    }
    const uint8_t* srcUv = src + static_cast<size_t>(srcStride) * srcH;
    const uint32_t dstPairsW = dstW / 2u;

    if (srcW == dstW && srcH == dstH) {
        for (uint32_t y = 0; y < dstH; ++y) {
            const uint8_t* sY = src + static_cast<size_t>(y) * srcStride;
            const uint8_t* sUv = srcUv + static_cast<size_t>(y >> 1u) * srcStride;
            uint8_t* dRow = dst + static_cast<size_t>(y) * dstStrideBytes;
            for (uint32_t cx = 0; cx < dstPairsW; ++cx) {
                const size_t px = static_cast<size_t>(cx) * 2u;
                const size_t dx = static_cast<size_t>(cx) * 4u;
                dRow[dx] = sY[px];
                dRow[dx + 1u] = sUv[px];
                dRow[dx + 2u] = sY[px + 1u];
                dRow[dx + 3u] = sUv[px + 1u];
            }
        }
        return;
    }

    const uint32_t srcPairsW = std::max(1u, srcW / 2u);
    if (xMapPairs.size() != dstPairsW) {
        xMapPairs.resize(dstPairsW);
        for (uint32_t cx = 0; cx < dstPairsW; ++cx) {
            xMapPairs[cx] = std::min(srcPairsW - 1u, static_cast<uint32_t>((static_cast<uint64_t>(cx) * srcPairsW) / std::max(1u, dstPairsW))) * 2u;
        }
    }

    for (uint32_t y = 0; y < dstH; ++y) {
        const uint32_t sy = std::min(srcH - 1u, static_cast<uint32_t>((static_cast<uint64_t>(y) * srcH) / dstH));
        const uint8_t* sY = src + static_cast<size_t>(sy) * srcStride;
        const uint8_t* sUv = srcUv + static_cast<size_t>(sy >> 1u) * srcStride;
        uint8_t* dRow = dst + static_cast<size_t>(y) * dstStrideBytes;
        for (uint32_t cx = 0; cx < dstPairsW; ++cx) {
            const size_t sx = xMapPairs[cx];
            const size_t dx = static_cast<size_t>(cx) * 4u;
            dRow[dx] = sY[sx];
            dRow[dx + 1u] = sUv[sx];
            dRow[dx + 2u] = sY[sx + 1u];
            dRow[dx + 3u] = sUv[sx + 1u];
        }
    }
}

void FillStandbyFrame(uint8_t* dst, UINT32 dstW, UINT32 dstH, UINT32 dstStridePixels, bool yuy2) noexcept {
    if (!dst || dstW == 0 || dstH == 0) return;
    if (yuy2) {
        const uint32_t rowStride = std::max(dstW, dstStridePixels) * 2u;
        const uint32_t words = dstW / 2u;
        for (UINT32 y = 0; y < dstH; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(dst + static_cast<size_t>(y) * rowStride);
            for (uint32_t x = 0; x < words; ++x) {
                row[x] = 0x80108010u; // Y0=16, U=128, Y1=16, V=128 (little-endian)
            }
        }
    } else {
        const uint32_t stride = std::max(dstW, dstStridePixels);
        for (UINT32 y = 0; y < dstH; ++y) {
            std::memset(dst + static_cast<size_t>(y) * stride, 16, dstW);
        }
        uint8_t* uv = dst + static_cast<size_t>(stride) * dstH;
        for (UINT32 y = 0; y < dstH / 2u; ++y) {
            std::memset(uv + static_cast<size_t>(y) * stride, 128, dstW);
        }
    }
}

// Enumerador COM para los pines del filtro DirectShow.
class ZeroCamPinEnum final : public IEnumPins {
public:
    explicit ZeroCamPinEnum(IPin* pin, ULONG pos = 0) : pin_(pin), pos_(pos) {
        DShowModuleAddRef();
        if (pin_) pin_->AddRef();
    }
    ~ZeroCamPinEnum() {
        if (pin_) pin_->Release();
        DShowModuleRelease();
    }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IEnumPins)) {
            *ppv = static_cast<IEnumPins*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return refCount_.fetch_add(1, std::memory_order_relaxed) + 1; }
    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG c = refCount_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (c == 0) delete this;
        return c;
    }

    IFACEMETHODIMP Next(ULONG cPins, IPin** ppPins, ULONG* pcFetched) override {
        if (!ppPins) return E_POINTER;
        if (cPins != 1 && !pcFetched) return E_POINTER;
        ULONG fetched = 0;
        if (cPins > 0 && pos_ == 0 && pin_) {
            ppPins[0] = pin_;
            pin_->AddRef();
            pos_ = 1;
            fetched = 1;
        }
        if (pcFetched) *pcFetched = fetched;
        return (fetched == cPins) ? S_OK : S_FALSE;
    }
    IFACEMETHODIMP Skip(ULONG cPins) override {
        if (pos_ == 0 && cPins > 0) pos_ = 1;
        return (cPins == 0 || (pos_ == 1 && cPins == 1)) ? S_OK : S_FALSE;
    }
    IFACEMETHODIMP Reset() override { pos_ = 0; return S_OK; }
    IFACEMETHODIMP Clone(IEnumPins** ppEnum) override {
        if (!ppEnum) return E_POINTER;
        *ppEnum = new ZeroCamPinEnum(pin_, pos_);
        return S_OK;
    }

private:
    std::atomic<ULONG> refCount_{ 1 };
    IPin* pin_ = nullptr;
    ULONG pos_ = 0;
};

// Enumerador COM de formatos soportados (NV12 y YUY2).
class ZeroCamMediaTypeEnum final : public IEnumMediaTypes {
public:
    explicit ZeroCamMediaTypeEnum(const AM_MEDIA_TYPE* preferredMt, ULONG pos = 0) : pos_(pos) {
        DShowModuleAddRef();
        if (preferredMt) {
            hasPref_ = SUCCEEDED(CopyMediaTypeFields(prefMt_, *preferredMt));
        }
    }
    ~ZeroCamMediaTypeEnum() {
        FreeMediaTypeFields(prefMt_);
        DShowModuleRelease();
    }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IEnumMediaTypes)) {
            *ppv = static_cast<IEnumMediaTypes*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return refCount_.fetch_add(1, std::memory_order_relaxed) + 1; }
    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG c = refCount_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (c == 0) delete this;
        return c;
    }

    IFACEMETHODIMP Next(ULONG cMediaTypes, AM_MEDIA_TYPE** ppMediaTypes, ULONG* pcFetched) override {
        if (!ppMediaTypes) return E_POINTER;
        if (cMediaTypes != 1 && !pcFetched) return E_POINTER;
        const ULONG total = static_cast<ULONG>(ZeroCamDShowPin::ModeCount()) + (hasPref_ ? 1u : 0u);
        ULONG fetched = 0;
        while (fetched < cMediaTypes && pos_ < total) {
            AM_MEDIA_TYPE* mt = nullptr;
            if (hasPref_ && pos_ == 0) {
                mt = CloneMediaTypePtr(prefMt_);
            } else {
                const size_t modeIdx = static_cast<size_t>(pos_ - (hasPref_ ? 1u : 0u));
                (void)ZeroCamDShowPin::CreateModeMediaType(modeIdx, &mt);
            }
            if (!mt) break;
            ppMediaTypes[fetched++] = mt;
            ++pos_;
        }
        if (pcFetched) *pcFetched = fetched;
        return (fetched == cMediaTypes) ? S_OK : S_FALSE;
    }

    IFACEMETHODIMP Skip(ULONG cMediaTypes) override {
        const ULONG total = static_cast<ULONG>(ZeroCamDShowPin::ModeCount()) + (hasPref_ ? 1u : 0u);
        if (pos_ + cMediaTypes > total) {
            pos_ = total;
            return S_FALSE;
        }
        pos_ += cMediaTypes;
        return S_OK;
    }

    IFACEMETHODIMP Reset() override { pos_ = 0; return S_OK; }

    IFACEMETHODIMP Clone(IEnumMediaTypes** ppEnum) override {
        if (!ppEnum) return E_POINTER;
        *ppEnum = new ZeroCamMediaTypeEnum(hasPref_ ? &prefMt_ : nullptr, pos_);
        return S_OK;
    }

private:
    std::atomic<ULONG> refCount_{ 1 };
    AM_MEDIA_TYPE prefMt_{};
    bool hasPref_ = false;
    ULONG pos_ = 0;
};

} // namespace

// ============================================================================
// ZeroCamDShowPin
// ============================================================================

ZeroCamDShowPin::ZeroCamDShowPin(ZeroCamDShowFilter* filter) : filter_(filter) {
    AM_MEDIA_TYPE* defMt = nullptr;
    if (SUCCEEDED(CreateModeMediaType(0, &defMt)) && defMt) {
        hasCurrentMt_ = SUCCEEDED(CopyMediaTypeFields(currentMt_, *defMt));
        DeleteMediaTypePtr(defMt);
    }
}

ZeroCamDShowPin::~ZeroCamDShowPin() {
    StopStreaming();
    FreeMediaTypeFields(currentMt_);
    FreeMediaTypeFields(connectedMt_);
}

size_t ZeroCamDShowPin::ModeCount() noexcept {
    return std::size(kDShowModes);
}

HRESULT ZeroCamDShowPin::CreateModeMediaType(size_t index, AM_MEDIA_TYPE** ppmt) {
    if (!ppmt) return E_POINTER;
    *ppmt = nullptr;
    if (index >= std::size(kDShowModes)) return E_INVALIDARG;

    const DShowModeDesc& m = kDShowModes[index];
    auto* mt = static_cast<AM_MEDIA_TYPE*>(CoTaskMemAlloc(sizeof(AM_MEDIA_TYPE)));
    if (!mt) return E_OUTOFMEMORY;
    std::memset(mt, 0, sizeof(AM_MEDIA_TYPE));

    auto* vih = static_cast<VIDEOINFOHEADER*>(CoTaskMemAlloc(sizeof(VIDEOINFOHEADER)));
    if (!vih) {
        CoTaskMemFree(mt);
        return E_OUTOFMEMORY;
    }
    std::memset(vih, 0, sizeof(VIDEOINFOHEADER));

    const DWORD imgSize = m.yuy2 ? (m.w * m.h * 2u) : (m.w * m.h * 3u / 2u);
    vih->rcSource = { 0, 0, static_cast<LONG>(m.w), static_cast<LONG>(m.h) };
    vih->rcTarget = { 0, 0, static_cast<LONG>(m.w), static_cast<LONG>(m.h) };
    vih->AvgTimePerFrame = 10000000LL / static_cast<REFERENCE_TIME>(m.fps);
    vih->dwBitRate = imgSize * 8u * m.fps;
    vih->bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    vih->bmiHeader.biWidth = static_cast<LONG>(m.w);
    vih->bmiHeader.biHeight = static_cast<LONG>(m.h);
    vih->bmiHeader.biPlanes = 1;
    vih->bmiHeader.biBitCount = m.yuy2 ? 16 : 12;
    vih->bmiHeader.biCompression = m.yuy2 ? MAKEFOURCC('Y', 'U', 'Y', '2') : MAKEFOURCC('N', 'V', '1', '2');
    vih->bmiHeader.biSizeImage = imgSize;

    mt->majortype = MEDIATYPE_Video;
    mt->subtype = m.yuy2 ? MEDIASUBTYPE_YUY2 : MEDIASUBTYPE_NV12;
    mt->bFixedSizeSamples = TRUE;
    mt->bTemporalCompression = FALSE;
    mt->lSampleSize = imgSize;
    mt->formattype = FORMAT_VideoInfo;
    mt->cbFormat = sizeof(VIDEOINFOHEADER);
    mt->pbFormat = reinterpret_cast<BYTE*>(vih);
    *ppmt = mt;
    return S_OK;
}

bool ZeroCamDShowPin::ExtractFormatParams(
    const AM_MEDIA_TYPE* pmt,
    UINT32& outW,
    UINT32& outH,
    bool& outYuy2,
    REFERENCE_TIME& outAvgTimePerFrame) noexcept {
    if (!pmt || pmt->majortype != MEDIATYPE_Video || !pmt->pbFormat) return false;
    if (pmt->subtype == MEDIASUBTYPE_NV12) outYuy2 = false;
    else if (pmt->subtype == MEDIASUBTYPE_YUY2) outYuy2 = true;
    else return false;

    BITMAPINFOHEADER bmi{};
    REFERENCE_TIME avg = 333333;
    if (pmt->formattype == FORMAT_VideoInfo && pmt->cbFormat >= sizeof(VIDEOINFOHEADER)) {
        const auto* vih = reinterpret_cast<const VIDEOINFOHEADER*>(pmt->pbFormat);
        bmi = vih->bmiHeader;
        avg = vih->AvgTimePerFrame;
    } else if (pmt->formattype == FORMAT_VideoInfo2 && pmt->cbFormat >= sizeof(VIDEOINFOHEADER2)) {
        const auto* vih2 = reinterpret_cast<const VIDEOINFOHEADER2*>(pmt->pbFormat);
        bmi = vih2->bmiHeader;
        avg = vih2->AvgTimePerFrame;
    } else {
        return false;
    }

    const LONG rawW = bmi.biWidth;
    const LONG rawH = (bmi.biHeight < 0) ? -bmi.biHeight : bmi.biHeight;
    if (rawW < 160 || rawW > 1920 || rawH < 120 || rawH > 1080) return false;
    const UINT32 uw = static_cast<UINT32>(rawW);
    const UINT32 uh = static_cast<UINT32>(rawH);
    if ((uw & 1u) != 0u || (uh & 1u) != 0u) return false;

    outW = uw;
    outH = uh;
    if (avg <= 0) avg = 333333;
    outAvgTimePerFrame = std::clamp<REFERENCE_TIME>(avg, 166666, 666667);
    return true;
}

IFACEMETHODIMP ZeroCamDShowPin::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IPin)) {
        *ppv = static_cast<IPin*>(this);
    } else if (riid == __uuidof(IAMStreamConfig)) {
        *ppv = static_cast<IAMStreamConfig*>(this);
    } else if (riid == __uuidof(IKsPropertySet)) {
        *ppv = static_cast<IKsPropertySet*>(this);
    } else if (riid == __uuidof(IQualityControl)) {
        *ppv = static_cast<IQualityControl*>(this);
    } else {
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

IFACEMETHODIMP_(ULONG) ZeroCamDShowPin::AddRef() {
    return filter_->AddRef();
}

IFACEMETHODIMP_(ULONG) ZeroCamDShowPin::Release() {
    return filter_->Release();
}

IFACEMETHODIMP ZeroCamDShowPin::Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt) {
    if (!pReceivePin) return E_POINTER;
    std::lock_guard<std::mutex> lock(lock_);
    if (!filter_->IsStopped()) return VFW_E_NOT_STOPPED;
    if (connectedPin_) return VFW_E_ALREADY_CONNECTED;

    PIN_DIRECTION dir = PINDIR_OUTPUT;
    if (FAILED(pReceivePin->QueryDirection(&dir)) || dir != PINDIR_INPUT) {
        return VFW_E_INVALID_DIRECTION;
    }

    Microsoft::WRL::ComPtr<IMemInputPin> memIn;
    if (FAILED(pReceivePin->QueryInterface(IID_PPV_ARGS(&memIn)))) {
        return VFW_E_NO_TRANSPORT;
    }

    std::vector<AM_MEDIA_TYPE*> candidates;
    auto clearCandidates = [&]() {
        for (AM_MEDIA_TYPE* c : candidates) DeleteMediaTypePtr(c);
        candidates.clear();
    };

    if (pmt && pmt->majortype != GUID_NULL) {
        if (pmt->majortype != MEDIATYPE_Video) return VFW_E_TYPE_NOT_ACCEPTED;
        if (pmt->formattype != GUID_NULL && pmt->pbFormat != nullptr) {
            UINT32 w = 0, h = 0;
            bool yuy2 = false;
            REFERENCE_TIME avg = 0;
            if (!ExtractFormatParams(pmt, w, h, yuy2, avg)) {
                return VFW_E_TYPE_NOT_ACCEPTED;
            }
            if (AM_MEDIA_TYPE* copy = CloneMediaTypePtr(*pmt)) {
                candidates.push_back(copy);
            }
        }
    }

    if (candidates.empty()) {
        auto matchesReq = [&](const AM_MEDIA_TYPE* m) -> bool {
            if (!m) return false;
            if (!pmt) return true;
            if (pmt->majortype != GUID_NULL && pmt->majortype != m->majortype) return false;
            if (pmt->subtype != GUID_NULL && pmt->subtype != m->subtype) return false;
            return true;
        };
        if (hasCurrentMt_ && matchesReq(&currentMt_)) {
            if (AM_MEDIA_TYPE* copy = CloneMediaTypePtr(currentMt_)) {
                candidates.push_back(copy);
            }
        }
        for (size_t i = 0; i < ModeCount(); ++i) {
            AM_MEDIA_TYPE* m = nullptr;
            if (SUCCEEDED(CreateModeMediaType(i, &m)) && m) {
                if (matchesReq(m)) candidates.push_back(m);
                else DeleteMediaTypePtr(m);
            }
        }
    }

    for (AM_MEDIA_TYPE* cand : candidates) {
        if (FAILED(pReceivePin->ReceiveConnection(this, cand))) {
            continue;
        }
        UINT32 w = 1280, h = 720;
        bool yuy2 = false;
        REFERENCE_TIME avg = 333333;
        if (!ExtractFormatParams(cand, w, h, yuy2, avg)) {
            pReceivePin->Disconnect();
            continue;
        }

        Microsoft::WRL::ComPtr<IMemAllocator> alloc;
        if (FAILED(memIn->GetAllocator(&alloc)) || !alloc) {
            if (FAILED(CoCreateInstance(CLSID_MemoryAllocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&alloc)))) {
                pReceivePin->Disconnect();
                continue;
            }
        }

        const size_t minFrameBytes = yuy2
            ? (static_cast<size_t>(w) * h * 2u)
            : (static_cast<size_t>(w) * h * 3u / 2u);
        const long bufSize = static_cast<long>(std::max<size_t>(cand->lSampleSize, minFrameBytes));
        ALLOCATOR_PROPERTIES req{};
        req.cBuffers = 3;
        req.cbBuffer = bufSize;
        req.cbAlign = 16;
        req.cbPrefix = 0;

        ALLOCATOR_PROPERTIES actual{};
        HRESULT hrProps = alloc->SetProperties(&req, &actual);
        if (FAILED(hrProps)) {
            req.cbAlign = 1;
            hrProps = alloc->SetProperties(&req, &actual);
        }
        if (FAILED(hrProps) || actual.cbBuffer < bufSize) {
            pReceivePin->Disconnect();
            continue;
        }
        if (FAILED(memIn->NotifyAllocator(alloc.Get(), FALSE))) {
            pReceivePin->Disconnect();
            continue;
        }

        if (FAILED(CopyMediaTypeFields(connectedMt_, *cand))) {
            pReceivePin->Disconnect();
            clearCandidates();
            return E_OUTOFMEMORY;
        }
        hasConnectedMt_ = true;
        connectedPin_ = pReceivePin;
        memInputPin_ = memIn;
        allocator_ = alloc;
        fmtWidth_ = w;
        fmtHeight_ = h;
        fmtIsYuy2_ = yuy2;
        fmtAvgTime_ = avg;
        clearCandidates();
        return S_OK;
    }

    clearCandidates();
    return VFW_E_NO_ACCEPTABLE_TYPES;
}

IFACEMETHODIMP ZeroCamDShowPin::ReceiveConnection(IPin*, const AM_MEDIA_TYPE*) {
    return E_UNEXPECTED;
}

IFACEMETHODIMP ZeroCamDShowPin::Disconnect() {
    std::lock_guard<std::mutex> lock(lock_);
    if (!filter_->IsStopped()) return VFW_E_NOT_STOPPED;
    if (!connectedPin_) return S_FALSE;
    connectedPin_.Reset();
    memInputPin_.Reset();
    allocator_.Reset();
    FreeMediaTypeFields(connectedMt_);
    hasConnectedMt_ = false;
    return S_OK;
}

IFACEMETHODIMP ZeroCamDShowPin::ConnectedTo(IPin** pPin) {
    if (!pPin) return E_POINTER;
    std::lock_guard<std::mutex> lock(lock_);
    *pPin = connectedPin_.Get();
    if (!*pPin) return VFW_E_NOT_CONNECTED;
    (*pPin)->AddRef();
    return S_OK;
}

IFACEMETHODIMP ZeroCamDShowPin::ConnectionMediaType(AM_MEDIA_TYPE* pmt) {
    if (!pmt) return E_POINTER;
    std::lock_guard<std::mutex> lock(lock_);
    if (!connectedPin_ || !hasConnectedMt_) {
        std::memset(pmt, 0, sizeof(AM_MEDIA_TYPE));
        return VFW_E_NOT_CONNECTED;
    }
    std::memset(pmt, 0, sizeof(AM_MEDIA_TYPE));
    return CopyMediaTypeFields(*pmt, connectedMt_);
}

IFACEMETHODIMP ZeroCamDShowPin::QueryPinInfo(PIN_INFO* pInfo) {
    if (!pInfo) return E_POINTER;
    pInfo->pFilter = filter_;
    filter_->AddRef();
    pInfo->dir = PINDIR_OUTPUT;
    wcscpy_s(pInfo->achName, L"Capture");
    return S_OK;
}

IFACEMETHODIMP ZeroCamDShowPin::QueryDirection(PIN_DIRECTION* pPinDir) {
    if (!pPinDir) return E_POINTER;
    *pPinDir = PINDIR_OUTPUT;
    return S_OK;
}

IFACEMETHODIMP ZeroCamDShowPin::QueryId(LPWSTR* Id) {
    if (!Id) return E_POINTER;
    *Id = DupWideStringCoTask(L"Capture");
    return *Id ? S_OK : E_OUTOFMEMORY;
}

IFACEMETHODIMP ZeroCamDShowPin::QueryAccept(const AM_MEDIA_TYPE* pmt) {
    UINT32 w = 0, h = 0;
    bool yuy2 = false;
    REFERENCE_TIME avg = 0;
    return ExtractFormatParams(pmt, w, h, yuy2, avg) ? S_OK : S_FALSE;
}

IFACEMETHODIMP ZeroCamDShowPin::EnumMediaTypes(IEnumMediaTypes** ppEnum) {
    if (!ppEnum) return E_POINTER;
    std::lock_guard<std::mutex> lock(lock_);
    *ppEnum = new ZeroCamMediaTypeEnum(hasCurrentMt_ ? &currentMt_ : nullptr);
    return S_OK;
}

IFACEMETHODIMP ZeroCamDShowPin::QueryInternalConnections(IPin**, ULONG*) { return E_NOTIMPL; }
IFACEMETHODIMP ZeroCamDShowPin::EndOfStream() { return E_UNEXPECTED; }
IFACEMETHODIMP ZeroCamDShowPin::BeginFlush() { return E_UNEXPECTED; }
IFACEMETHODIMP ZeroCamDShowPin::EndFlush() { return E_UNEXPECTED; }
IFACEMETHODIMP ZeroCamDShowPin::NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) { return S_OK; }

IFACEMETHODIMP ZeroCamDShowPin::SetFormat(AM_MEDIA_TYPE* pmt) {
    if (!pmt) return E_POINTER;
    std::lock_guard<std::mutex> lock(lock_);
    if (!filter_->IsStopped()) return VFW_E_NOT_STOPPED;

    UINT32 w = 0, h = 0;
    bool yuy2 = false;
    REFERENCE_TIME avg = 0;
    if (!ExtractFormatParams(pmt, w, h, yuy2, avg)) {
        return VFW_E_INVALIDMEDIATYPE;
    }
    if (connectedPin_ && connectedPin_->QueryAccept(pmt) != S_OK) {
        return VFW_E_TYPE_NOT_ACCEPTED;
    }
    if (FAILED(CopyMediaTypeFields(currentMt_, *pmt))) {
        return E_OUTOFMEMORY;
    }
    hasCurrentMt_ = true;
    fmtWidth_ = w;
    fmtHeight_ = h;
    fmtIsYuy2_ = yuy2;
    fmtAvgTime_ = avg;
    return S_OK;
}

IFACEMETHODIMP ZeroCamDShowPin::GetFormat(AM_MEDIA_TYPE** ppmt) {
    if (!ppmt) return E_POINTER;
    std::lock_guard<std::mutex> lock(lock_);
    *ppmt = hasCurrentMt_ ? CloneMediaTypePtr(currentMt_) : nullptr;
    if (*ppmt) return S_OK;
    return CreateModeMediaType(0, ppmt);
}

IFACEMETHODIMP ZeroCamDShowPin::GetNumberOfCapabilities(int* piCount, int* piSize) {
    if (!piCount || !piSize) return E_POINTER;
    *piCount = static_cast<int>(ModeCount());
    *piSize = sizeof(VIDEO_STREAM_CONFIG_CAPS);
    return S_OK;
}

IFACEMETHODIMP ZeroCamDShowPin::GetStreamCaps(int iIndex, AM_MEDIA_TYPE** ppmt, BYTE* pSCC) {
    if (!ppmt || !pSCC) return E_POINTER;
    if (iIndex < 0 || static_cast<size_t>(iIndex) >= ModeCount()) return S_FALSE;

    HRESULT hr = CreateModeMediaType(static_cast<size_t>(iIndex), ppmt);
    if (FAILED(hr)) return hr;

    const DShowModeDesc& m = kDShowModes[iIndex];
    auto* scc = reinterpret_cast<VIDEO_STREAM_CONFIG_CAPS*>(pSCC);
    std::memset(scc, 0, sizeof(VIDEO_STREAM_CONFIG_CAPS));
    scc->guid = FORMAT_VideoInfo;
    scc->VideoStandard = AnalogVideo_None;
    scc->InputSize = { static_cast<LONG>(m.w), static_cast<LONG>(m.h) };
    scc->MinCroppingSize = scc->InputSize;
    scc->MaxCroppingSize = scc->InputSize;
    scc->CropGranularityX = 1;
    scc->CropGranularityY = 1;
    scc->CropAlignX = 1;
    scc->CropAlignY = 1;
    scc->MinOutputSize = { 320, 240 };
    scc->MaxOutputSize = { 1920, 1080 };
    scc->OutputGranularityX = 2;
    scc->OutputGranularityY = 2;
    scc->StretchTapsX = 1;
    scc->StretchTapsY = 1;
    scc->ShrinkTapsX = 1;
    scc->ShrinkTapsY = 1;
    scc->MinFrameInterval = 10000000LL / 60LL;
    scc->MaxFrameInterval = 10000000LL / 15LL;
    scc->MinBitsPerSecond = 640 * 480 * 12 * 15;
    scc->MaxBitsPerSecond = 1920 * 1080 * 16 * 60;
    return S_OK;
}

IFACEMETHODIMP ZeroCamDShowPin::Set(REFGUID, DWORD, LPVOID, DWORD, LPVOID, DWORD) { return E_NOTIMPL; }

IFACEMETHODIMP ZeroCamDShowPin::Get(
    REFGUID guidPropSet,
    DWORD dwPropID,
    LPVOID,
    DWORD,
    LPVOID pPropData,
    DWORD cbPropData,
    DWORD* pcbReturned) {
    if (guidPropSet != AMPROPSETID_Pin) return E_PROP_SET_UNSUPPORTED;
    if (dwPropID != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
    if (!pPropData && !pcbReturned) return E_POINTER;
    if (pcbReturned) *pcbReturned = sizeof(GUID);
    if (!pPropData) return S_OK;
    if (cbPropData < sizeof(GUID)) return E_UNEXPECTED;
    *reinterpret_cast<GUID*>(pPropData) = kPinNameVideoCapture;
    return S_OK;
}

IFACEMETHODIMP ZeroCamDShowPin::QuerySupported(REFGUID guidPropSet, DWORD dwPropID, DWORD* pTypeSupport) {
    if (guidPropSet != AMPROPSETID_Pin) return E_PROP_SET_UNSUPPORTED;
    if (dwPropID != AMPROPERTY_PIN_CATEGORY) return E_PROP_ID_UNSUPPORTED;
    if (pTypeSupport) *pTypeSupport = KSPROPERTY_SUPPORT_GET;
    return S_OK;
}

IFACEMETHODIMP ZeroCamDShowPin::Notify(IBaseFilter*, Quality) { return E_NOTIMPL; }
IFACEMETHODIMP ZeroCamDShowPin::SetSink(IQualityControl*) { return S_OK; }

HRESULT ZeroCamDShowPin::StartStreaming() {
    StopStreaming();
    Microsoft::WRL::ComPtr<IMemAllocator> alloc;
    Microsoft::WRL::ComPtr<IMemInputPin> memIn;
    UINT32 w = 1280, h = 720;
    bool yuy2 = false;
    REFERENCE_TIME avg = 333333;
    {
        std::lock_guard<std::mutex> lock(lock_);
        if (!connectedPin_ || !memInputPin_ || !allocator_) {
            return S_OK; // Pin desconectado no inicia hilo push pero el grafo puede arrancar sin error.
        }
        alloc = allocator_;
        memIn = memInputPin_;
        w = fmtWidth_;
        h = fmtHeight_;
        yuy2 = fmtIsYuy2_;
        avg = fmtAvgTime_;
    }

    HRESULT hr = alloc->Commit();
    if (FAILED(hr)) return hr;

    stopRequested_.store(false, std::memory_order_release);
    flushing_.store(false, std::memory_order_release);
    pushThread_ = std::thread(&ZeroCamDShowPin::PushThreadMain, this, w, h, yuy2, avg);
    return S_OK;
}

void ZeroCamDShowPin::StopStreaming() {
    stopRequested_.store(true, std::memory_order_release);
    Microsoft::WRL::ComPtr<IMemAllocator> alloc;
    {
        std::lock_guard<std::mutex> lock(lock_);
        alloc = allocator_;
    }
    if (alloc) {
        // Decommit desbloquea inmediatamente cualquier GetBuffer() esperando en el hilo push.
        alloc->Decommit();
    }
    if (pushThread_.joinable()) {
        pushThread_.join();
    }
}

void ZeroCamDShowPin::PushThreadMain(UINT32 dstW, UINT32 dstH, bool dstYuy2, REFERENCE_TIME frameDurHns) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    Microsoft::WRL::ComPtr<IMemAllocator> alloc;
    Microsoft::WRL::ComPtr<IMemInputPin> memIn;
    {
        std::lock_guard<std::mutex> lock(lock_);
        alloc = allocator_;
        memIn = memInputPin_;
    }
    if (!alloc || !memIn) {
        CoUninitialize();
        return;
    }

    SharedIpcChannel ipc;
    bool ipcOpened = ipc.Open(false);
    bool consumerRegistered = false;
    if (ipcOpened && ipc.Data()) {
        ipc.Data()->consumerCount.fetch_add(1, std::memory_order_acq_rel);
        ipc.Data()->clientNeedsCpuFallback.store(1, std::memory_order_release);
        consumerRegistered = true;
    }

    HANDLE paceTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!paceTimer) {
        paceTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }

    std::vector<uint32_t> xMapY;
    std::vector<uint32_t> xMapUv;
    std::vector<uint32_t> xMapYuy2;
    uint64_t lastSeq = 0;
    ULONGLONG lastDeliverMs = 0;
    LARGE_INTEGER qpcFreq{}, qpcStart{};
    QueryPerformanceFrequency(&qpcFreq);
    QueryPerformanceCounter(&qpcStart);
    REFERENCE_TIME lastRtStart = -1;
    bool firstSample = true;

    while (!stopRequested_.load(std::memory_order_acquire)) {
        if (!ipcOpened) {
            ipcOpened = ipc.Open(false);
            if (ipcOpened && ipc.Data() && !consumerRegistered) {
                ipc.Data()->consumerCount.fetch_add(1, std::memory_order_acq_rel);
                ipc.Data()->clientNeedsCpuFallback.store(1, std::memory_order_release);
                consumerRegistered = true;
            }
        } else if (ipc.Data() && ipc.Data()->producerAlive.load(std::memory_order_acquire) != 0) {
            ipc.Data()->clientNeedsCpuFallback.store(1, std::memory_order_release);
        }

        ZeroCamSharedMemory* mem = ipcOpened ? ipc.Data() : nullptr;
        REFERENCE_TIME activeDur = frameDurHns;
        if (mem) {
            const uint32_t fn = mem->fpsNum.load(std::memory_order_acquire);
            const uint32_t fd = mem->fpsDen.load(std::memory_order_acquire);
            if (fn >= 1u && fn <= 240u && fd >= 1u && fd <= 1001u) {
                activeDur = std::clamp<REFERENCE_TIME>(
                    (10000000LL * static_cast<REFERENCE_TIME>(fd)) / static_cast<REFERENCE_TIME>(fn),
                    41666LL, 666667LL);
            }
        }
        const ULONGLONG frameMs = std::clamp<ULONGLONG>(static_cast<ULONGLONG>(activeDur / 10000LL), 4ull, 100ull);

        // Espera reactiva de alta resolución: entrega inmediata ante frameSequence nuevo.
        while (!stopRequested_.load(std::memory_order_acquire)) {
            const uint64_t seqNow = mem ? mem->frameSequence.load(std::memory_order_acquire) : 0ull;
            if (seqNow != 0ull && seqNow != lastSeq) {
                lastSeq = seqNow;
                break;
            }
            const ULONGLONG nowMs = GetTickCount64();
            if (lastDeliverMs == 0 || nowMs >= lastDeliverMs + frameMs) {
                break;
            }
            if (paceTimer) {
                LARGE_INTEGER due{};
                due.QuadPart = -20000LL; // 2 ms
                SetWaitableTimer(paceTimer, &due, 0, nullptr, nullptr, FALSE);
                WaitForSingleObject(paceTimer, 5);
            } else {
                Sleep(2);
            }
        }
        if (stopRequested_.load(std::memory_order_acquire)) break;

        Microsoft::WRL::ComPtr<IMediaSample> sample;
        HRESULT hr = alloc->GetBuffer(&sample, nullptr, nullptr, 0);
        if (FAILED(hr) || !sample || stopRequested_.load(std::memory_order_acquire)) {
            break;
        }

        UINT32 sampleStridePx = dstW;
        AM_MEDIA_TYPE* dynMt = nullptr;
        if (sample->GetMediaType(&dynMt) == S_OK && dynMt) {
            UINT32 dw = 0, dh = 0;
            bool dy = false;
            REFERENCE_TIME davg = 0;
            if (ExtractFormatParams(dynMt, dw, dh, dy, davg) && dw >= dstW) {
                sampleStridePx = dw;
            }
            DeleteMediaTypePtr(dynMt);
        }

        BYTE* dstPtr = nullptr;
        const long bufCapacity = sample->GetSize();
        const size_t minNeeded = dstYuy2
            ? static_cast<size_t>(std::max(dstW, sampleStridePx)) * dstH * 2u
            : static_cast<size_t>(std::max(dstW, sampleStridePx)) * dstH * 3u / 2u;

        if (SUCCEEDED(sample->GetPointer(&dstPtr)) && dstPtr && bufCapacity >= static_cast<long>(minNeeded)) {
            bool wroteLive = false;
            if (mem && mem->producerAlive.load(std::memory_order_acquire) != 0 &&
                mem->frameSequence.load(std::memory_order_acquire) != 0) {
                const uint64_t meta = mem->cpuFrameMeta.load(std::memory_order_acquire);
                uint32_t mw = 0, mh = 0, mStride = 0, mSlot = 0;
                if (UnpackCpuFrameMeta(meta, mw, mh, mStride, mSlot)) {
                    const uint32_t stridePx = std::max(dstW, sampleStridePx);
                    if (dstYuy2) {
                        WriteYuy2FromNv12Cpu(
                            mem->cpuNv12Buffer[mSlot], mStride, mw, mh,
                            dstPtr, stridePx * 2u, dstW, dstH, xMapYuy2);
                    } else {
                        WriteNv12FromCpu(
                            mem->cpuNv12Buffer[mSlot], mStride, mw, mh,
                            dstPtr, stridePx, dstW, dstH, xMapY, xMapUv);
                    }
                    wroteLive = true;
                }
            }
            if (!wroteLive) {
                FillStandbyFrame(dstPtr, dstW, dstH, sampleStridePx, dstYuy2);
            }
            sample->SetActualDataLength(static_cast<long>(minNeeded));
        }

        REFERENCE_TIME rtStart = 0;
        if (!firstSample) {
            REFERENCE_TIME qpcTicks = (lastRtStart >= 0) ? (lastRtStart + activeDur) : activeDur;
            if (qpcFreq.QuadPart > 0) {
                LARGE_INTEGER qpcNow{};
                QueryPerformanceCounter(&qpcNow);
                const int64_t delta = qpcNow.QuadPart - qpcStart.QuadPart;
                if (delta > 0) {
                    const int64_t secs = delta / qpcFreq.QuadPart;
                    const int64_t rem = delta % qpcFreq.QuadPart;
                    qpcTicks = static_cast<REFERENCE_TIME>(
                        secs * 10000000LL + (rem * 10000000LL) / qpcFreq.QuadPart);
                }
            }
            rtStart = (lastRtStart >= 0 && qpcTicks <= lastRtStart) ? (lastRtStart + 10000LL) : qpcTicks;
        }
        REFERENCE_TIME rtStop = rtStart + activeDur;
        lastRtStart = rtStart;

        sample->SetTime(&rtStart, &rtStop);
        sample->SetSyncPoint(TRUE);
        sample->SetPreroll(FALSE);
        sample->SetDiscontinuity(firstSample ? TRUE : FALSE);
        firstSample = false;

        lastDeliverMs = GetTickCount64();
        hr = memIn->Receive(sample.Get());
        if (FAILED(hr)) {
            break;
        }
    }

    if (paceTimer) CloseHandle(paceTimer);
    if (consumerRegistered && ipc.IsOpen() && ipc.Data()) {
        const uint32_t prev = ipc.Data()->consumerCount.fetch_sub(1, std::memory_order_acq_rel);
        if (prev <= 1u) {
            ipc.Data()->clientNeedsCpuFallback.store(0, std::memory_order_release);
        }
    }
    ipc.Close(false);
    CoUninitialize();
}

// ============================================================================
// ZeroCamDShowFilter
// ============================================================================

ZeroCamDShowFilter::ZeroCamDShowFilter() : pin_(this) {
    DShowModuleAddRef();
    wcscpy_s(filterName_, kZeroCamFriendlyName);
}

ZeroCamDShowFilter::~ZeroCamDShowFilter() {
    Stop();
    pin_.Disconnect();
    DShowModuleRelease();
}

IFACEMETHODIMP ZeroCamDShowFilter::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IPersist) ||
        riid == __uuidof(IMediaFilter) || riid == __uuidof(IBaseFilter)) {
        *ppv = static_cast<IBaseFilter*>(this);
    } else if (riid == __uuidof(IAMFilterMiscFlags)) {
        *ppv = static_cast<IAMFilterMiscFlags*>(this);
    } else {
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

IFACEMETHODIMP_(ULONG) ZeroCamDShowFilter::AddRef() {
    return refCount_.fetch_add(1, std::memory_order_relaxed) + 1;
}

IFACEMETHODIMP_(ULONG) ZeroCamDShowFilter::Release() {
    const ULONG c = refCount_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (c == 0) delete this;
    return c;
}

IFACEMETHODIMP ZeroCamDShowFilter::GetClassID(CLSID* pClassID) {
    if (!pClassID) return E_POINTER;
    *pClassID = CLSID_ZeroCamDShow;
    return S_OK;
}

IFACEMETHODIMP ZeroCamDShowFilter::Stop() {
    std::lock_guard<std::mutex> lock(stateLock_);
    if (state_.load(std::memory_order_acquire) == State_Stopped) {
        return S_OK;
    }
    state_.store(State_Stopped, std::memory_order_release);
    pin_.StopStreaming();
    return S_OK;
}

IFACEMETHODIMP ZeroCamDShowFilter::Pause() {
    std::lock_guard<std::mutex> lock(stateLock_);
    const FILTER_STATE prev = state_.load(std::memory_order_acquire);
    if (prev == State_Paused) return S_OK;
    if (prev == State_Stopped) {
        state_.store(State_Paused, std::memory_order_release);
        const HRESULT hr = pin_.StartStreaming();
        if (FAILED(hr)) {
            state_.store(State_Stopped, std::memory_order_release);
            return hr;
        }
        return S_OK;
    }
    state_.store(State_Paused, std::memory_order_release);
    return S_OK;
}

IFACEMETHODIMP ZeroCamDShowFilter::Run(REFERENCE_TIME) {
    std::lock_guard<std::mutex> lock(stateLock_);
    const FILTER_STATE prev = state_.load(std::memory_order_acquire);
    if (prev == State_Stopped) {
        state_.store(State_Running, std::memory_order_release);
        const HRESULT hr = pin_.StartStreaming();
        if (FAILED(hr)) {
            state_.store(State_Stopped, std::memory_order_release);
            return hr;
        }
        return S_OK;
    }
    state_.store(State_Running, std::memory_order_release);
    return S_OK;
}

IFACEMETHODIMP ZeroCamDShowFilter::GetState(DWORD, FILTER_STATE* State) {
    if (!State) return E_POINTER;
    const FILTER_STATE s = state_.load(std::memory_order_acquire);
    *State = s;
    return (s == State_Paused) ? VFW_S_CANT_CUE : S_OK;
}

IFACEMETHODIMP ZeroCamDShowFilter::SetSyncSource(IReferenceClock* pClock) {
    std::lock_guard<std::mutex> lock(stateLock_);
    clock_ = pClock;
    return S_OK;
}

IFACEMETHODIMP ZeroCamDShowFilter::GetSyncSource(IReferenceClock** pClock) {
    if (!pClock) return E_POINTER;
    std::lock_guard<std::mutex> lock(stateLock_);
    *pClock = clock_.Get();
    if (*pClock) (*pClock)->AddRef();
    return S_OK;
}

IFACEMETHODIMP ZeroCamDShowFilter::EnumPins(IEnumPins** ppEnum) {
    if (!ppEnum) return E_POINTER;
    *ppEnum = new ZeroCamPinEnum(&pin_);
    return S_OK;
}

IFACEMETHODIMP ZeroCamDShowFilter::FindPin(LPCWSTR Id, IPin** ppPin) {
    if (!Id || !ppPin) return E_POINTER;
    if (wcscmp(Id, L"Capture") == 0) {
        *ppPin = &pin_;
        pin_.AddRef();
        return S_OK;
    }
    *ppPin = nullptr;
    return VFW_E_NOT_FOUND;
}

IFACEMETHODIMP ZeroCamDShowFilter::QueryFilterInfo(FILTER_INFO* pInfo) {
    if (!pInfo) return E_POINTER;
    std::lock_guard<std::mutex> lock(stateLock_);
    wcscpy_s(pInfo->achName, filterName_);
    pInfo->pGraph = graph_;
    if (graph_) graph_->AddRef();
    return S_OK;
}

IFACEMETHODIMP ZeroCamDShowFilter::JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR pName) {
    std::lock_guard<std::mutex> lock(stateLock_);
    graph_ = pGraph;
    if (pName) {
        wcsncpy_s(filterName_, pName, _TRUNCATE);
    } else {
        wcscpy_s(filterName_, kZeroCamFriendlyName);
    }
    return S_OK;
}

IFACEMETHODIMP ZeroCamDShowFilter::QueryVendorInfo(LPWSTR* pVendorInfo) {
    if (!pVendorInfo) return E_POINTER;
    *pVendorInfo = DupWideStringCoTask(L"ZeroCam");
    return *pVendorInfo ? S_OK : E_OUTOFMEMORY;
}

IFACEMETHODIMP_(ULONG) ZeroCamDShowFilter::GetMiscFlags() {
    return AM_FILTER_MISC_FLAGS_IS_SOURCE;
}

} // namespace zerocam
