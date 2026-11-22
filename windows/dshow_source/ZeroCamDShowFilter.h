#pragma once
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
#include <cstdint>
#include <mutex>
#include <thread>
#include "../shared/ZeroCamIpc.h"

namespace zerocam {

class ZeroCamDShowFilter;

/// Pin de salida DirectShow compatible con Discord, OBS y Zoom (NV12 y YUY2 sin BaseClasses).
class ZeroCamDShowPin final
    : public IPin
    , public IAMStreamConfig
    , public IKsPropertySet
    , public IQualityControl {
public:
    explicit ZeroCamDShowPin(ZeroCamDShowFilter* filter);
    ~ZeroCamDShowPin();

    // IUnknown (delega conteo de referencias al filtro padre evitando ciclos).
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    IFACEMETHODIMP_(ULONG) AddRef() override;
    IFACEMETHODIMP_(ULONG) Release() override;

    // IPin
    IFACEMETHODIMP Connect(IPin* pReceivePin, const AM_MEDIA_TYPE* pmt) override;
    IFACEMETHODIMP ReceiveConnection(IPin* pConnector, const AM_MEDIA_TYPE* pmt) override;
    IFACEMETHODIMP Disconnect() override;
    IFACEMETHODIMP ConnectedTo(IPin** pPin) override;
    IFACEMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE* pmt) override;
    IFACEMETHODIMP QueryPinInfo(PIN_INFO* pInfo) override;
    IFACEMETHODIMP QueryDirection(PIN_DIRECTION* pPinDir) override;
    IFACEMETHODIMP QueryId(LPWSTR* Id) override;
    IFACEMETHODIMP QueryAccept(const AM_MEDIA_TYPE* pmt) override;
    IFACEMETHODIMP EnumMediaTypes(IEnumMediaTypes** ppEnum) override;
    IFACEMETHODIMP QueryInternalConnections(IPin** apPin, ULONG* nPin) override;
    IFACEMETHODIMP EndOfStream() override;
    IFACEMETHODIMP BeginFlush() override;
    IFACEMETHODIMP EndFlush() override;
    IFACEMETHODIMP NewSegment(REFERENCE_TIME tStart, REFERENCE_TIME tStop, double dRate) override;

    // IAMStreamConfig
    IFACEMETHODIMP SetFormat(AM_MEDIA_TYPE* pmt) override;
    IFACEMETHODIMP GetFormat(AM_MEDIA_TYPE** ppmt) override;
    IFACEMETHODIMP GetNumberOfCapabilities(int* piCount, int* piSize) override;
    IFACEMETHODIMP GetStreamCaps(int iIndex, AM_MEDIA_TYPE** ppmt, BYTE* pSCC) override;

    // IKsPropertySet
    IFACEMETHODIMP Set(REFGUID guidPropSet, DWORD dwPropID, LPVOID pInstanceData, DWORD cbInstanceData, LPVOID pPropData, DWORD cbPropData) override;
    IFACEMETHODIMP Get(REFGUID guidPropSet, DWORD dwPropID, LPVOID pInstanceData, DWORD cbInstanceData, LPVOID pPropData, DWORD cbPropData, DWORD* pcbReturned) override;
    IFACEMETHODIMP QuerySupported(REFGUID guidPropSet, DWORD dwPropID, DWORD* pTypeSupport) override;

    // IQualityControl
    IFACEMETHODIMP Notify(IBaseFilter* pSelf, Quality q) override;
    IFACEMETHODIMP SetSink(IQualityControl* piqc) override;

    // Control de flujo desde ZeroCamDShowFilter
    HRESULT StartStreaming();
    void StopStreaming();
    static HRESULT CreateModeMediaType(size_t index, AM_MEDIA_TYPE** ppmt);
    static size_t ModeCount() noexcept;

private:
    static bool ExtractFormatParams(
        const AM_MEDIA_TYPE* pmt,
        UINT32& outW,
        UINT32& outH,
        bool& outYuy2,
        REFERENCE_TIME& outAvgTimePerFrame) noexcept;

    void PushThreadMain(UINT32 dstW, UINT32 dstH, bool dstYuy2, REFERENCE_TIME frameDurHns);

    ZeroCamDShowFilter* filter_ = nullptr;
    std::mutex lock_;

    Microsoft::WRL::ComPtr<IPin> connectedPin_;
    Microsoft::WRL::ComPtr<IMemInputPin> memInputPin_;
    Microsoft::WRL::ComPtr<IMemAllocator> allocator_;

    AM_MEDIA_TYPE currentMt_{};
    bool hasCurrentMt_ = false;
    AM_MEDIA_TYPE connectedMt_{};
    bool hasConnectedMt_ = false;

    UINT32 fmtWidth_ = 1280;
    UINT32 fmtHeight_ = 720;
    bool fmtIsYuy2_ = false;
    REFERENCE_TIME fmtAvgTime_ = 333333; // 30 fps en unidades de 100 ns

    std::atomic<bool> stopRequested_{ false };
    std::atomic<bool> flushing_{ false };
    std::thread pushThread_;
};

/// Filtro fuente DirectShow clean-room que expone ZeroCam Virtual Camera en Windows 10.
class ZeroCamDShowFilter final : public IBaseFilter, public IAMFilterMiscFlags {
public:
    ZeroCamDShowFilter();
    ~ZeroCamDShowFilter();

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    IFACEMETHODIMP_(ULONG) AddRef() override;
    IFACEMETHODIMP_(ULONG) Release() override;

    // IPersist
    IFACEMETHODIMP GetClassID(CLSID* pClassID) override;

    // IMediaFilter
    IFACEMETHODIMP Stop() override;
    IFACEMETHODIMP Pause() override;
    IFACEMETHODIMP Run(REFERENCE_TIME tStart) override;
    IFACEMETHODIMP GetState(DWORD dwMilliSecsTimeout, FILTER_STATE* State) override;
    IFACEMETHODIMP SetSyncSource(IReferenceClock* pClock) override;
    IFACEMETHODIMP GetSyncSource(IReferenceClock** pClock) override;

    // IBaseFilter
    IFACEMETHODIMP EnumPins(IEnumPins** ppEnum) override;
    IFACEMETHODIMP FindPin(LPCWSTR Id, IPin** ppPin) override;
    IFACEMETHODIMP QueryFilterInfo(FILTER_INFO* pInfo) override;
    IFACEMETHODIMP JoinFilterGraph(IFilterGraph* pGraph, LPCWSTR pName) override;
    IFACEMETHODIMP QueryVendorInfo(LPWSTR* pVendorInfo) override;

    // IAMFilterMiscFlags
    IFACEMETHODIMP_(ULONG) GetMiscFlags() override;

    bool IsStopped() const noexcept { return state_.load(std::memory_order_acquire) == State_Stopped; }
    IPin* OutputPin() noexcept { return &pin_; }

private:
    std::atomic<ULONG> refCount_{ 1 };
    std::mutex stateLock_;
    std::atomic<FILTER_STATE> state_{ State_Stopped };
    Microsoft::WRL::ComPtr<IReferenceClock> clock_;
    IFilterGraph* graph_ = nullptr; // Referencia débil según especificación DirectShow JoinFilterGraph.
    wchar_t filterName_[128]{};
    ZeroCamDShowPin pin_;
};

} // namespace zerocam
