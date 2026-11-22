#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NO_DSHOW_STRSAFE
#define NO_DSHOW_STRSAFE
#endif
#include <windows.h>
#include <objbase.h>
#include <dshow.h>
#include <dvdmedia.h>
#include <wrl/client.h>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <vector>
#include "dshow_selftest_helper.h"
#include "../app/D3D11Context.h"
#include "../shared/ZeroCamGuids.h"
#include "../shared/ZeroCamIpc.h"

namespace zerocam {

namespace {

using DllGetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, LPVOID*);

class MockDShowSinkPin final : public IPin, public IMemInputPin {
public:
    MockDShowSinkPin(const GUID& expectSubtype, uint32_t w, uint32_t h)
        : expectedSubtype_(expectSubtype), w_(w), h_(h) {}

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IPin)) *ppv = static_cast<IPin*>(this);
        else if (riid == __uuidof(IMemInputPin)) *ppv = static_cast<IMemInputPin*>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return refs_.fetch_add(1) + 1; }
    IFACEMETHODIMP_(ULONG) Release() override {
        const ULONG c = refs_.fetch_sub(1) - 1;
        if (c == 0) delete this;
        return c;
    }

    IFACEMETHODIMP Connect(IPin*, const AM_MEDIA_TYPE*) override { return E_NOTIMPL; }
    IFACEMETHODIMP ReceiveConnection(IPin* pConnector, const AM_MEDIA_TYPE* pmt) override {
        if (!pmt || pmt->majortype != MEDIATYPE_Video || pmt->subtype != expectedSubtype_) return VFW_E_TYPE_NOT_ACCEPTED;
        peer_ = pConnector;
        return S_OK;
    }
    IFACEMETHODIMP Disconnect() override { peer_.Reset(); alloc_.Reset(); return S_OK; }
    IFACEMETHODIMP ConnectedTo(IPin** pPin) override {
        if (!pPin) return E_POINTER;
        *pPin = peer_.Get();
        if (*pPin) (*pPin)->AddRef();
        return *pPin ? S_OK : VFW_E_NOT_CONNECTED;
    }
    IFACEMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE*) override { return E_NOTIMPL; }
    IFACEMETHODIMP QueryPinInfo(PIN_INFO* pInfo) override {
        if (!pInfo) return E_POINTER;
        pInfo->pFilter = nullptr;
        pInfo->dir = PINDIR_INPUT;
        wcscpy_s(pInfo->achName, L"In");
        return S_OK;
    }
    IFACEMETHODIMP QueryDirection(PIN_DIRECTION* dir) override { if (!dir) return E_POINTER; *dir = PINDIR_INPUT; return S_OK; }
    IFACEMETHODIMP QueryId(LPWSTR* id) override {
        *id = static_cast<LPWSTR>(CoTaskMemAlloc(6));
        if (*id) wcscpy_s(*id, 3, L"In");
        return *id ? S_OK : E_OUTOFMEMORY;
    }
    IFACEMETHODIMP QueryAccept(const AM_MEDIA_TYPE* pmt) override {
        return (pmt && pmt->subtype == expectedSubtype_) ? S_OK : S_FALSE;
    }
    IFACEMETHODIMP EnumMediaTypes(IEnumMediaTypes**) override { return E_NOTIMPL; }
    IFACEMETHODIMP QueryInternalConnections(IPin**, ULONG*) override { return E_NOTIMPL; }
    IFACEMETHODIMP EndOfStream() override { return S_OK; }
    IFACEMETHODIMP BeginFlush() override { return S_OK; }
    IFACEMETHODIMP EndFlush() override { return S_OK; }
    IFACEMETHODIMP NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) override { return S_OK; }

    IFACEMETHODIMP GetAllocator(IMemAllocator** ppAlloc) override {
        if (!alloc_) {
            HRESULT hr = CoCreateInstance(CLSID_MemoryAllocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&alloc_));
            if (FAILED(hr)) return hr;
        }
        *ppAlloc = alloc_.Get();
        (*ppAlloc)->AddRef();
        return S_OK;
    }
    IFACEMETHODIMP NotifyAllocator(IMemAllocator* pAlloc, BOOL) override { alloc_ = pAlloc; return S_OK; }
    IFACEMETHODIMP GetAllocatorRequirements(ALLOCATOR_PROPERTIES*) override { return E_NOTIMPL; }
    IFACEMETHODIMP Receive(IMediaSample* pSample) override {
        if (!pSample) return E_POINTER;
        BYTE* ptr = nullptr;
        const long len = pSample->GetActualDataLength();
        if (SUCCEEDED(pSample->GetPointer(&ptr)) && ptr && len > 0) {
            std::lock_guard<std::mutex> lk(mu_);
            LARGE_INTEGER qpcNow{}, qpcFreq{};
            QueryPerformanceCounter(&qpcNow);
            QueryPerformanceFrequency(&qpcFreq);
            const int64_t prevQpc = lastRecvQpc_.exchange(qpcNow.QuadPart, std::memory_order_acq_rel);
            if (prevQpc > 0 && qpcNow.QuadPart > prevQpc && qpcFreq.QuadPart > 0) {
                const int64_t d = qpcNow.QuadPart - prevQpc;
                const int64_t gapUs = (d / qpcFreq.QuadPart) * 1000000LL + ((d % qpcFreq.QuadPart) * 1000000LL) / qpcFreq.QuadPart;
                int64_t curMin = minGapUs_.load(std::memory_order_acquire);
                while (gapUs < curMin && !minGapUs_.compare_exchange_weak(curMin, gapUs, std::memory_order_acq_rel)) {}
            }
            if (expectedSubtype_ == kMediaSubtypeI420) {
                const size_t yIdx = (static_cast<size_t>(h_) / 2u) * w_ + (w_ / 2u);
                const size_t yPlane = static_cast<size_t>(w_) * h_;
                const size_t uPlane = yPlane / 4u;
                const size_t uIdx = yPlane + (static_cast<size_t>(h_) / 4u) * (w_ / 2u) + (w_ / 4u);
                const size_t vIdx = uIdx + uPlane;
                if (vIdx < static_cast<size_t>(len)) {
                    lastCenterY_.store(ptr[yIdx], std::memory_order_release);
                    lastCenterU_.store(ptr[uIdx], std::memory_order_release);
                    lastCenterV_.store(ptr[vIdx], std::memory_order_release);
                }
            } else {
                const size_t idx = (expectedSubtype_ == MEDIASUBTYPE_YUY2)
                    ? ((static_cast<size_t>(h_) / 2u) * w_ + (w_ / 2u)) * 2u
                    : ((static_cast<size_t>(h_) / 2u) * w_ + (w_ / 2u));
                if (idx + 3u < static_cast<size_t>(len)) {
                    lastCenterY_.store(ptr[idx], std::memory_order_release);
                    lastCenterU_.store(ptr[idx + 1u], std::memory_order_release);
                }
            }
            lastLen_.store(len, std::memory_order_release);
            frames_.fetch_add(1, std::memory_order_release);
        }
        return S_OK;
    }
    IFACEMETHODIMP ReceiveMultiple(IMediaSample** pSamples, long nSamples, long* nProcessed) override {
        long done = 0;
        for (long i = 0; i < nSamples; ++i) {
            if (SUCCEEDED(Receive(pSamples[i]))) ++done;
        }
        if (nProcessed) *nProcessed = done;
        return S_OK;
    }
    IFACEMETHODIMP ReceiveCanBlock() override { return S_FALSE; }

    uint32_t Frames() const noexcept { return frames_.load(std::memory_order_acquire); }
    long LastLen() const noexcept { return lastLen_.load(std::memory_order_acquire); }
    uint8_t LastY() const noexcept { return lastCenterY_.load(std::memory_order_acquire); }
    uint8_t LastU() const noexcept { return lastCenterU_.load(std::memory_order_acquire); }
    uint8_t LastV() const noexcept { return lastCenterV_.load(std::memory_order_acquire); }
    int64_t MinGapMs() const noexcept {
        const int64_t u = minGapUs_.load(std::memory_order_acquire);
        return (u == 0x7FFFFFFFFFFFFFFFLL) ? 0 : (u / 1000LL);
    }

private:
    std::atomic<ULONG> refs_{ 1 };
    GUID expectedSubtype_{};
    uint32_t w_ = 1280;
    uint32_t h_ = 720;
    std::mutex mu_;
    Microsoft::WRL::ComPtr<IPin> peer_;
    Microsoft::WRL::ComPtr<IMemAllocator> alloc_;
    std::atomic<uint32_t> frames_{ 0 };
    std::atomic<long> lastLen_{ 0 };
    std::atomic<uint8_t> lastCenterY_{ 0 };
    std::atomic<uint8_t> lastCenterU_{ 0 };
    std::atomic<uint8_t> lastCenterV_{ 0 };
    std::atomic<int64_t> lastRecvQpc_{ 0 };
    std::atomic<int64_t> minGapUs_{ 0x7FFFFFFFFFFFFFFFLL };
};

} // namespace

void RunDShowSelfTests(
    const wchar_t* exePath,
    D3D11Context& d3dCtx,
    bool& outMetaOk,
    bool& outYuy2Ok,
    bool& outNv12StandbyOk) {
    outMetaOk = false;
    outYuy2Ok = false;
    outNv12StandbyOk = false;

    std::filesystem::path dsTestDll = std::filesystem::path(exePath).parent_path() / "ZeroCamDShowTest.dll";
    HMODULE dsMod = LoadLibraryW(dsTestDll.c_str());
    if (!dsMod) return;

    auto pFactoryFn = reinterpret_cast<DllGetClassObjectFn>(GetProcAddress(dsMod, "DllGetClassObject"));
    Microsoft::WRL::ComPtr<IClassFactory> dsFact;
    Microsoft::WRL::ComPtr<IBaseFilter> dsFilter;
    if (pFactoryFn && SUCCEEDED(pFactoryFn(CLSID_ZeroCamDShow, IID_PPV_ARGS(&dsFact))) &&
        SUCCEEDED(dsFact->CreateInstance(nullptr, IID_PPV_ARGS(&dsFilter))) && dsFilter) {
        Microsoft::WRL::ComPtr<IAMFilterMiscFlags> miscFlags;
        const bool isSrcFlag = SUCCEEDED(dsFilter.As(&miscFlags)) &&
                               (miscFlags->GetMiscFlags() == AM_FILTER_MISC_FLAGS_IS_SOURCE);

        Microsoft::WRL::ComPtr<IEnumPins> ep;
        Microsoft::WRL::ComPtr<IPin> outPin;
        ULONG gotPins = 0;
        if (isSrcFlag && SUCCEEDED(dsFilter->EnumPins(&ep)) &&
            ep->Next(1, &outPin, &gotPins) == S_OK && gotPins == 1 && outPin) {
            Microsoft::WRL::ComPtr<IKsPropertySet> ksProp;
            GUID pinCat{};
            DWORD cbRet = 0;
            const bool ksOk = SUCCEEDED(outPin.As(&ksProp)) &&
                              SUCCEEDED(ksProp->Get(AMPROPSETID_Pin, AMPROPERTY_PIN_CATEGORY, nullptr, 0, &pinCat, sizeof(pinCat), &cbRet)) &&
                              cbRet == sizeof(GUID) && pinCat == kPinNameVideoCapture;

            Microsoft::WRL::ComPtr<IAMStreamConfig> scfg;
            int capCount = 0, capSize = 0;
            bool hasI420Cap = false, hasYuy2Cap = false, hasNv12Cap = false, hasSd480 = false;
            AM_MEDIA_TYPE* i420Mt = nullptr;
            AM_MEDIA_TYPE* yuy2Mt = nullptr;
            AM_MEDIA_TYPE* nv12Mt = nullptr;

            if (ksOk && SUCCEEDED(outPin.As(&scfg)) &&
                SUCCEEDED(scfg->GetNumberOfCapabilities(&capCount, &capSize)) &&
                capCount == 12 && capSize == sizeof(VIDEO_STREAM_CONFIG_CAPS)) {
                std::vector<BYTE> sccBuf(static_cast<size_t>(capSize));
                for (int i = 0; i < capCount; ++i) {
                    AM_MEDIA_TYPE* cmt = nullptr;
                    if (scfg->GetStreamCaps(i, &cmt, sccBuf.data()) == S_OK && cmt) {
                        if (cmt->formattype == FORMAT_VideoInfo && cmt->cbFormat >= sizeof(VIDEOINFOHEADER) && cmt->pbFormat) {
                            const auto* vih = reinterpret_cast<const VIDEOINFOHEADER*>(cmt->pbFormat);
                            if (vih->bmiHeader.biWidth == 640 && vih->bmiHeader.biHeight == 480) {
                                hasSd480 = true;
                            }
                            if (vih->bmiHeader.biWidth == 1280 && vih->bmiHeader.biHeight == 720) {
                                if (cmt->subtype == kMediaSubtypeI420 && !i420Mt) {
                                    i420Mt = cmt;
                                    hasI420Cap = true;
                                    continue;
                                }
                                if (cmt->subtype == MEDIASUBTYPE_YUY2 && !yuy2Mt) {
                                    yuy2Mt = cmt;
                                    hasYuy2Cap = true;
                                    continue;
                                }
                                if (cmt->subtype == MEDIASUBTYPE_NV12 && !nv12Mt) {
                                    nv12Mt = cmt;
                                    hasNv12Cap = true;
                                    continue;
                                }
                            }
                        }
                        if (cmt->pbFormat) CoTaskMemFree(cmt->pbFormat);
                        CoTaskMemFree(cmt);
                    }
                }
            }
            outMetaOk = ksOk && hasI420Cap && hasYuy2Cap && hasNv12Cap && !hasSd480;

            ZeroCamSharedMemory* ipc = d3dCtx.Ipc();
            bool i420BilinearOk = false;
            if (ipc && i420Mt && SUCCEEDED(scfg->SetFormat(i420Mt))) {
                // Patrón 1080p de columnas alternas Y=40/220 para verificar promedio bilineal 1080p->720p en I420.
                for (uint32_t row = 0; row < 1080u; ++row) {
                    uint8_t* line = ipc->cpuNv12Buffer[0] + static_cast<size_t>(row) * 1920u;
                    for (uint32_t col = 0; col < 1920u; ++col) {
                        line[col] = (col & 1u) ? 220u : 40u;
                    }
                }
                uint8_t* uvBase = ipc->cpuNv12Buffer[0] + static_cast<size_t>(1920u) * 1080u;
                for (size_t i = 0; i < (static_cast<size_t>(1920u) * 1080u) / 4u; ++i) {
                    uvBase[i * 2u] = 110u;
                    uvBase[i * 2u + 1u] = 150u;
                }
                ipc->cpuFrameMeta.store(PackCpuFrameMeta(1920, 1080, 1920, 0), std::memory_order_release);
                ipc->frameSequence.fetch_add(1, std::memory_order_release);
                d3dCtx.SetProducerActive(true);

                Microsoft::WRL::ComPtr<MockDShowSinkPin> sinkI420;
                sinkI420.Attach(new MockDShowSinkPin(kMediaSubtypeI420, 1280, 720));
                if (SUCCEEDED(outPin->Connect(sinkI420.Get(), i420Mt)) && SUCCEEDED(dsFilter->Run(0))) {
                    for (int w = 0; w < 20 && sinkI420->Frames() < 1u; ++w) {
                        Sleep(10);
                    }
                    for (int f = 0; f < 4; ++f) {
                        Sleep(38);
                        ipc->frameSequence.fetch_add(1, std::memory_order_release);
                    }
                    for (int w = 0; w < 20 && sinkI420->Frames() < 5u; ++w) {
                        Sleep(10);
                    }
                    const uint8_t yMid = sinkI420->LastY();
                    i420BilinearOk = (sinkI420->Frames() >= 5u &&
                                      sinkI420->Frames() <= 6u &&
                                      sinkI420->MinGapMs() >= 20 &&
                                      sinkI420->LastLen() == (1280L * 720L * 3L) / 2L &&
                                      yMid >= 70u && yMid <= 190u &&
                                      sinkI420->LastU() == 110u &&
                                      sinkI420->LastV() == 150u);
                    dsFilter->Stop();
                    outPin->Disconnect();
                    sinkI420->Disconnect();
                }
            }

            if (ipc && yuy2Mt && SUCCEEDED(scfg->SetFormat(yuy2Mt))) {
                std::memset(ipc->cpuNv12Buffer[0], 180, static_cast<size_t>(1280) * 720);
                std::memset(ipc->cpuNv12Buffer[0] + static_cast<size_t>(1280) * 720, 128, (static_cast<size_t>(1280) * 720) / 2);
                ipc->cpuFrameMeta.store(PackCpuFrameMeta(1280, 720, 1280, 0), std::memory_order_release);
                ipc->frameSequence.fetch_add(1, std::memory_order_release);
                d3dCtx.SetProducerActive(true);

                Microsoft::WRL::ComPtr<MockDShowSinkPin> sinkYuy2;
                sinkYuy2.Attach(new MockDShowSinkPin(MEDIASUBTYPE_YUY2, 1280, 720));
                if (SUCCEEDED(outPin->Connect(sinkYuy2.Get(), yuy2Mt)) && SUCCEEDED(dsFilter->Pause())) {
                    FILTER_STATE pauseSt = State_Stopped;
                    const HRESULT hrPauseState = dsFilter->GetState(0, &pauseSt);
                    const bool pauseCantCueOk = (hrPauseState == VFW_S_CANT_CUE && pauseSt == State_Paused);
                    if (SUCCEEDED(dsFilter->Run(0))) {
                        for (int w = 0; w < 40 && sinkYuy2->Frames() < 2u; ++w) {
                            Sleep(25);
                        }
                        outYuy2Ok = (i420BilinearOk &&
                                     pauseCantCueOk &&
                                     sinkYuy2->Frames() >= 2u &&
                                     sinkYuy2->LastLen() == 1280L * 720L * 2L &&
                                     sinkYuy2->LastY() >= 175 && sinkYuy2->LastY() <= 185 &&
                                     sinkYuy2->LastU() == 128);
                    }
                    dsFilter->Stop();
                    outPin->Disconnect();
                    sinkYuy2->Disconnect();
                    outYuy2Ok = outYuy2Ok && (ipc->clientNeedsCpuFallback.load(std::memory_order_acquire) == 0u);
                }
            }

            if (ipc && nv12Mt && SUCCEEDED(scfg->SetFormat(nv12Mt))) {
                std::memset(ipc->cpuNv12Buffer[0], 195, static_cast<size_t>(1280) * 720);
                std::memset(ipc->cpuNv12Buffer[0] + static_cast<size_t>(1280) * 720, 128, (static_cast<size_t>(1280) * 720) / 2);
                ipc->cpuFrameMeta.store(PackCpuFrameMeta(1280, 720, 1280, 0), std::memory_order_release);
                ipc->frameSequence.fetch_add(1, std::memory_order_release);
                d3dCtx.SetProducerActive(true);

                Microsoft::WRL::ComPtr<MockDShowSinkPin> sinkNv12;
                sinkNv12.Attach(new MockDShowSinkPin(MEDIASUBTYPE_NV12, 1280, 720));
                if (SUCCEEDED(outPin->Connect(sinkNv12.Get(), nv12Mt)) && SUCCEEDED(dsFilter->Run(0))) {
                    for (int w = 0; w < 40 && sinkNv12->Frames() < 2u; ++w) {
                        Sleep(25);
                    }
                    const bool liveNv12 = (sinkNv12->Frames() >= 2u &&
                                           sinkNv12->LastLen() == (1280L * 720L * 3L) / 2L &&
                                           sinkNv12->LastY() >= 190 && sinkNv12->LastY() <= 200);
                    d3dCtx.SetProducerActive(false);
                    const uint32_t stopF = sinkNv12->Frames();
                    for (int w = 0; w < 40 && (sinkNv12->Frames() < stopF + 2u || sinkNv12->LastY() != 16); ++w) {
                        Sleep(25);
                    }
                    outNv12StandbyOk = liveNv12 && (sinkNv12->LastY() == 16);
                    dsFilter->Stop();
                    outPin->Disconnect();
                    sinkNv12->Disconnect();
                }
            }

            if (i420Mt) {
                if (i420Mt->pbFormat) CoTaskMemFree(i420Mt->pbFormat);
                CoTaskMemFree(i420Mt);
            }
            if (yuy2Mt) {
                if (yuy2Mt->pbFormat) CoTaskMemFree(yuy2Mt->pbFormat);
                CoTaskMemFree(yuy2Mt);
            }
            if (nv12Mt) {
                if (nv12Mt->pbFormat) CoTaskMemFree(nv12Mt->pbFormat);
                CoTaskMemFree(nv12Mt);
            }
        }
        dsFilter.Reset();
    }
    dsFact.Reset();
    FreeLibrary(dsMod);
}

} // namespace zerocam
