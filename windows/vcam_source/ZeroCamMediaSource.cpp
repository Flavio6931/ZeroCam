#include "ZeroCamMediaSource.h"
#include "../shared/ZeroCamGuids.h"

namespace zerocam {

extern void ModuleAddRef() noexcept;
extern void ModuleRelease() noexcept;

ZeroCamMediaSource::ZeroCamMediaSource() {
    ModuleAddRef();
}

ZeroCamMediaSource::~ZeroCamMediaSource() {
    Shutdown();
    ModuleRelease();
}

HRESULT ZeroCamMediaSource::Initialize() {
    HRESULT hr = MFCreateAttributes(&sourceAttributes_, 6);
    if (FAILED(hr)) return hr;

    hr = sourceAttributes_->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (FAILED(hr)) return hr;
    sourceAttributes_->SetString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, kZeroCamFriendlyName);

    hr = MFCreateEventQueue(&eventQueue_);
    if (FAILED(hr)) return hr;

    stream_.Attach(new ZeroCamMediaStream(this));
    hr = stream_->Initialize();
    if (FAILED(hr)) return hr;

    Microsoft::WRL::ComPtr<IMFStreamDescriptor> sd;
    hr = stream_->GetStreamDescriptor(&sd);
    if (FAILED(hr)) return hr;

    IMFStreamDescriptor* sds[1] = { sd.Get() };
    hr = MFCreatePresentationDescriptor(1, sds, &presentationDesc_);
    if (FAILED(hr)) return hr;

    return presentationDesc_->SelectStream(0);
}

IFACEMETHODIMP ZeroCamMediaSource::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEventGenerator) ||
        riid == __uuidof(IMFMediaSource) || riid == __uuidof(IMFMediaSourceEx)) {
        *ppv = static_cast<IMFMediaSourceEx*>(this);
    } else if (riid == __uuidof(IMFGetService)) {
        *ppv = static_cast<IMFGetService*>(this);
    } else if (riid == __uuidof(IKsControl)) {
        *ppv = static_cast<IKsControl*>(this);
    } else if (riid == __uuidof(IMFSampleAllocatorControl)) {
        *ppv = static_cast<IMFSampleAllocatorControl*>(this);
    } else if (riid == __uuidof(IMFAttributes)) {
        *ppv = static_cast<IMFAttributes*>(this);
    } else {
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

IFACEMETHODIMP_(ULONG) ZeroCamMediaSource::AddRef() {
    return refCount_.fetch_add(1, std::memory_order_relaxed) + 1;
}

IFACEMETHODIMP_(ULONG) ZeroCamMediaSource::Release() {
    ULONG count = refCount_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (count == 0) {
        delete this;
    }
    return count;
}

IFACEMETHODIMP ZeroCamMediaSource::GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent) {
    if (!eventQueue_) return MF_E_SHUTDOWN;
    return eventQueue_->GetEvent(dwFlags, ppEvent);
}

IFACEMETHODIMP ZeroCamMediaSource::BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState) {
    if (!eventQueue_) return MF_E_SHUTDOWN;
    return eventQueue_->BeginGetEvent(pCallback, punkState);
}

IFACEMETHODIMP ZeroCamMediaSource::EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent) {
    if (!eventQueue_) return MF_E_SHUTDOWN;
    return eventQueue_->EndGetEvent(pResult, ppEvent);
}

IFACEMETHODIMP ZeroCamMediaSource::QueueEvent(MediaEventType met, REFGUID guidExtendedType, HRESULT hrStatus, const PROPVARIANT* pvValue) {
    if (!eventQueue_) return MF_E_SHUTDOWN;
    return eventQueue_->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue);
}

IFACEMETHODIMP ZeroCamMediaSource::GetCharacteristics(DWORD* pdwCharacteristics) {
    if (!pdwCharacteristics) return E_POINTER;
    std::lock_guard<std::mutex> lock(mutex_);
    if (isShutdown_) return MF_E_SHUTDOWN;
    *pdwCharacteristics = MFMEDIASOURCE_IS_LIVE;
    return S_OK;
}

IFACEMETHODIMP ZeroCamMediaSource::CreatePresentationDescriptor(IMFPresentationDescriptor** ppPresentationDescriptor) {
    if (!ppPresentationDescriptor) return E_POINTER;
    std::lock_guard<std::mutex> lock(mutex_);
    if (isShutdown_ || !presentationDesc_) return MF_E_SHUTDOWN;
    return presentationDesc_->Clone(ppPresentationDescriptor);
}

IFACEMETHODIMP ZeroCamMediaSource::Start(IMFPresentationDescriptor* pPresentationDescriptor, const GUID*, const PROPVARIANT* pvarStartPosition) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (isShutdown_ || !eventQueue_ || !stream_) return MF_E_SHUTDOWN;

    // Aplica el MediaType elegido al StreamDescriptor activo si el cliente solicitó cambio.
    if (pPresentationDescriptor) {
        BOOL selected = FALSE;
        Microsoft::WRL::ComPtr<IMFStreamDescriptor> reqSd;
        if (SUCCEEDED(pPresentationDescriptor->GetStreamDescriptorByIndex(0, &selected, &reqSd)) && selected) {
            Microsoft::WRL::ComPtr<IMFMediaTypeHandler> reqHandler;
            Microsoft::WRL::ComPtr<IMFMediaType> reqType;
            if (SUCCEEDED(reqSd->GetMediaTypeHandler(&reqHandler)) &&
                SUCCEEDED(reqHandler->GetCurrentMediaType(&reqType))) {
                Microsoft::WRL::ComPtr<IMFStreamDescriptor> ownSd;
                Microsoft::WRL::ComPtr<IMFMediaTypeHandler> ownHandler;
                if (SUCCEEDED(stream_->GetStreamDescriptor(&ownSd)) &&
                    SUCCEEDED(ownSd->GetMediaTypeHandler(&ownHandler))) {
                    ownHandler->SetCurrentMediaType(reqType.Get());
                }
            }
        }
    }

    const MediaEventType streamEvt = streamNotified_ ? MEUpdatedStream : MENewStream;
    streamNotified_ = true;
    HRESULT hr = eventQueue_->QueueEventParamUnk(streamEvt, GUID_NULL, S_OK, static_cast<IMFMediaStream*>(stream_.Get()));
    if (FAILED(hr)) return hr;

    hr = eventQueue_->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, pvarStartPosition);
    if (FAILED(hr)) return hr;

    return stream_->Start(pvarStartPosition);
}

IFACEMETHODIMP ZeroCamMediaSource::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (isShutdown_ || !eventQueue_ || !stream_) return MF_E_SHUTDOWN;
    HRESULT hr = stream_->Stop();
    if (FAILED(hr)) return hr;
    return eventQueue_->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, nullptr);
}

IFACEMETHODIMP ZeroCamMediaSource::Pause() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (isShutdown_ || !eventQueue_ || !stream_) return MF_E_SHUTDOWN;
    HRESULT hr = stream_->Pause();
    if (FAILED(hr)) return hr;
    return eventQueue_->QueueEventParamVar(MESourcePaused, GUID_NULL, S_OK, nullptr);
}

IFACEMETHODIMP ZeroCamMediaSource::Shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (isShutdown_) return S_OK;
    isShutdown_ = true;
    if (stream_) {
        stream_->Shutdown();
        stream_.Reset();
    }
    if (eventQueue_) {
        eventQueue_->Shutdown();
        eventQueue_.Reset();
    }
    presentationDesc_.Reset();
    return S_OK;
}

IFACEMETHODIMP ZeroCamMediaSource::GetSourceAttributes(IMFAttributes** ppAttributes) {
    if (!ppAttributes) return E_POINTER;
    std::lock_guard<std::mutex> lock(mutex_);
    if (isShutdown_ || !sourceAttributes_) return MF_E_SHUTDOWN;
    *ppAttributes = sourceAttributes_.Get();
    (*ppAttributes)->AddRef();
    return S_OK;
}

IFACEMETHODIMP ZeroCamMediaSource::GetStreamAttributes(DWORD dwStreamIdentifier, IMFAttributes** ppAttributes) {
    if (!ppAttributes) return E_POINTER;
    if (dwStreamIdentifier != 0) return MF_E_INVALIDSTREAMNUMBER;
    std::lock_guard<std::mutex> lock(mutex_);
    if (isShutdown_ || !stream_) return MF_E_SHUTDOWN;
    return stream_->GetStreamAttributes(ppAttributes);
}

IFACEMETHODIMP ZeroCamMediaSource::SetD3DManager(IUnknown* pManager) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (isShutdown_ || !stream_) return MF_E_SHUTDOWN;
    return stream_->SetD3DManager(pManager);
}

IFACEMETHODIMP ZeroCamMediaSource::GetService(REFGUID guidService, REFIID riid, LPVOID* ppvObject) {
    if (!ppvObject) return E_POINTER;
    if (guidService == MF_MEDIASOURCE_SERVICE || guidService == GUID_NULL) {
        return QueryInterface(riid, ppvObject);
    }
    return MF_E_UNSUPPORTED_SERVICE;
}

IFACEMETHODIMP ZeroCamMediaSource::KsProperty(PKSPROPERTY, ULONG, LPVOID, ULONG, ULONG* BytesReturned) {
    if (BytesReturned) *BytesReturned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

IFACEMETHODIMP ZeroCamMediaSource::KsMethod(PKSMETHOD, ULONG, LPVOID, ULONG, ULONG* BytesReturned) {
    if (BytesReturned) *BytesReturned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

IFACEMETHODIMP ZeroCamMediaSource::KsEvent(PKSEVENT, ULONG, LPVOID, ULONG, ULONG* BytesReturned) {
    if (BytesReturned) *BytesReturned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

IFACEMETHODIMP ZeroCamMediaSource::SetDefaultAllocator(DWORD dwOutputStreamID, IUnknown* pAllocator) {
    if (dwOutputStreamID != 0) return MF_E_INVALIDSTREAMNUMBER;
    std::lock_guard<std::mutex> lock(mutex_);
    if (isShutdown_ || !stream_) return MF_E_SHUTDOWN;
    return stream_->SetAllocator(pAllocator);
}

IFACEMETHODIMP ZeroCamMediaSource::GetAllocatorUsage(DWORD dwOutputStreamID, DWORD* pdwInputStreamID, MFSampleAllocatorUsage* peUsage) {
    if (dwOutputStreamID != 0) return MF_E_INVALIDSTREAMNUMBER;
    if (!peUsage) return E_POINTER;
    if (pdwInputStreamID) *pdwInputStreamID = 0;
    *peUsage = MFSampleAllocatorUsage_UsesProvidedAllocator;
    return S_OK;
}

// Delegación directa de IMFAttributes a sourceAttributes_
IFACEMETHODIMP ZeroCamMediaSource::GetItem(REFGUID k, PROPVARIANT* v) { return sourceAttributes_->GetItem(k, v); }
IFACEMETHODIMP ZeroCamMediaSource::GetItemType(REFGUID k, MF_ATTRIBUTE_TYPE* t) { return sourceAttributes_->GetItemType(k, t); }
IFACEMETHODIMP ZeroCamMediaSource::CompareItem(REFGUID k, REFPROPVARIANT v, BOOL* r) { return sourceAttributes_->CompareItem(k, v, r); }
IFACEMETHODIMP ZeroCamMediaSource::Compare(IMFAttributes* t, MF_ATTRIBUTES_MATCH_TYPE m, BOOL* r) { return sourceAttributes_->Compare(t, m, r); }
IFACEMETHODIMP ZeroCamMediaSource::GetUINT32(REFGUID k, UINT32* v) { return sourceAttributes_->GetUINT32(k, v); }
IFACEMETHODIMP ZeroCamMediaSource::GetUINT64(REFGUID k, UINT64* v) { return sourceAttributes_->GetUINT64(k, v); }
IFACEMETHODIMP ZeroCamMediaSource::GetDouble(REFGUID k, double* v) { return sourceAttributes_->GetDouble(k, v); }
IFACEMETHODIMP ZeroCamMediaSource::GetGUID(REFGUID k, GUID* v) { return sourceAttributes_->GetGUID(k, v); }
IFACEMETHODIMP ZeroCamMediaSource::GetStringLength(REFGUID k, UINT32* v) { return sourceAttributes_->GetStringLength(k, v); }
IFACEMETHODIMP ZeroCamMediaSource::GetString(REFGUID k, LPWSTR s, UINT32 b, UINT32* l) { return sourceAttributes_->GetString(k, s, b, l); }
IFACEMETHODIMP ZeroCamMediaSource::GetAllocatedString(REFGUID k, LPWSTR* s, UINT32* l) { return sourceAttributes_->GetAllocatedString(k, s, l); }
IFACEMETHODIMP ZeroCamMediaSource::GetBlobSize(REFGUID k, UINT32* s) { return sourceAttributes_->GetBlobSize(k, s); }
IFACEMETHODIMP ZeroCamMediaSource::GetBlob(REFGUID k, UINT8* b, UINT32 s, UINT32* r) { return sourceAttributes_->GetBlob(k, b, s, r); }
IFACEMETHODIMP ZeroCamMediaSource::GetAllocatedBlob(REFGUID k, UINT8** b, UINT32* s) { return sourceAttributes_->GetAllocatedBlob(k, b, s); }
IFACEMETHODIMP ZeroCamMediaSource::GetUnknown(REFGUID k, REFIID r, LPVOID* p) { return sourceAttributes_->GetUnknown(k, r, p); }
IFACEMETHODIMP ZeroCamMediaSource::SetItem(REFGUID k, REFPROPVARIANT v) { return sourceAttributes_->SetItem(k, v); }
IFACEMETHODIMP ZeroCamMediaSource::DeleteItem(REFGUID k) { return sourceAttributes_->DeleteItem(k); }
IFACEMETHODIMP ZeroCamMediaSource::DeleteAllItems() { return sourceAttributes_->DeleteAllItems(); }
IFACEMETHODIMP ZeroCamMediaSource::SetUINT32(REFGUID k, UINT32 v) { return sourceAttributes_->SetUINT32(k, v); }
IFACEMETHODIMP ZeroCamMediaSource::SetUINT64(REFGUID k, UINT64 v) { return sourceAttributes_->SetUINT64(k, v); }
IFACEMETHODIMP ZeroCamMediaSource::SetDouble(REFGUID k, double v) { return sourceAttributes_->SetDouble(k, v); }
IFACEMETHODIMP ZeroCamMediaSource::SetGUID(REFGUID k, REFGUID v) { return sourceAttributes_->SetGUID(k, v); }
IFACEMETHODIMP ZeroCamMediaSource::SetString(REFGUID k, LPCWSTR v) { return sourceAttributes_->SetString(k, v); }
IFACEMETHODIMP ZeroCamMediaSource::SetBlob(REFGUID k, const UINT8* b, UINT32 s) { return sourceAttributes_->SetBlob(k, b, s); }
IFACEMETHODIMP ZeroCamMediaSource::SetUnknown(REFGUID k, IUnknown* p) { return sourceAttributes_->SetUnknown(k, p); }
IFACEMETHODIMP ZeroCamMediaSource::LockStore() { return sourceAttributes_->LockStore(); }
IFACEMETHODIMP ZeroCamMediaSource::UnlockStore() { return sourceAttributes_->UnlockStore(); }
IFACEMETHODIMP ZeroCamMediaSource::GetCount(UINT32* c) { return sourceAttributes_->GetCount(c); }
IFACEMETHODIMP ZeroCamMediaSource::GetItemByIndex(UINT32 i, GUID* k, PROPVARIANT* v) { return sourceAttributes_->GetItemByIndex(i, k, v); }
IFACEMETHODIMP ZeroCamMediaSource::CopyAllItems(IMFAttributes* d) { return sourceAttributes_->CopyAllItems(d); }

} // namespace zerocam
