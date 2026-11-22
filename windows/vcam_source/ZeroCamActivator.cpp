#include "ZeroCamActivator.h"
#include "ZeroCamMediaSource.h"
#include "../shared/ZeroCamGuids.h"

namespace zerocam {

extern void ModuleAddRef() noexcept;
extern void ModuleRelease() noexcept;

ZeroCamActivator::ZeroCamActivator() {
    ModuleAddRef();
}

ZeroCamActivator::~ZeroCamActivator() {
    // No llamar ShutdownObject aquí: FrameServer libera el activador justo tras ActivateObject().
    activeSource_.Reset();
    ModuleRelease();
}

HRESULT ZeroCamActivator::Initialize() {
    HRESULT hr = MFCreateAttributes(&attributes_, 4);
    if (FAILED(hr)) return hr;
    hr = attributes_->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (FAILED(hr)) return hr;
    return attributes_->SetString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, kZeroCamFriendlyName);
}

IFACEMETHODIMP ZeroCamActivator::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFActivate)) {
        *ppv = static_cast<IMFActivate*>(this);
    } else if (riid == __uuidof(IMFAttributes)) {
        *ppv = static_cast<IMFAttributes*>(this);
    } else {
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

IFACEMETHODIMP_(ULONG) ZeroCamActivator::AddRef() {
    return refCount_.fetch_add(1, std::memory_order_relaxed) + 1;
}

IFACEMETHODIMP_(ULONG) ZeroCamActivator::Release() {
    ULONG count = refCount_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (count == 0) {
        delete this;
    }
    return count;
}

IFACEMETHODIMP ZeroCamActivator::ActivateObject(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    std::lock_guard<std::mutex> lock(mutex_);

    if (!activeSource_) {
        Microsoft::WRL::ComPtr<ZeroCamMediaSource> src;
        src.Attach(new ZeroCamMediaSource());
        HRESULT hr = src->Initialize();
        if (FAILED(hr)) return hr;

        Microsoft::WRL::ComPtr<IMFAttributes> srcAttrs;
        if (attributes_ && SUCCEEDED(src->GetSourceAttributes(&srcAttrs))) {
            attributes_->CopyAllItems(srcAttrs.Get());
            srcAttrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        }
        activeSource_ = src;
    }
    return activeSource_->QueryInterface(riid, ppv);
}

IFACEMETHODIMP ZeroCamActivator::ShutdownObject() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (activeSource_) {
        activeSource_->Shutdown();
        activeSource_.Reset();
    }
    return S_OK;
}

IFACEMETHODIMP ZeroCamActivator::DetachObject() {
    std::lock_guard<std::mutex> lock(mutex_);
    activeSource_.Reset();
    return S_OK;
}

IFACEMETHODIMP ZeroCamActivator::GetItem(REFGUID k, PROPVARIANT* v) { return attributes_->GetItem(k, v); }
IFACEMETHODIMP ZeroCamActivator::GetItemType(REFGUID k, MF_ATTRIBUTE_TYPE* t) { return attributes_->GetItemType(k, t); }
IFACEMETHODIMP ZeroCamActivator::CompareItem(REFGUID k, REFPROPVARIANT v, BOOL* r) { return attributes_->CompareItem(k, v, r); }
IFACEMETHODIMP ZeroCamActivator::Compare(IMFAttributes* t, MF_ATTRIBUTES_MATCH_TYPE m, BOOL* r) { return attributes_->Compare(t, m, r); }
IFACEMETHODIMP ZeroCamActivator::GetUINT32(REFGUID k, UINT32* v) { return attributes_->GetUINT32(k, v); }
IFACEMETHODIMP ZeroCamActivator::GetUINT64(REFGUID k, UINT64* v) { return attributes_->GetUINT64(k, v); }
IFACEMETHODIMP ZeroCamActivator::GetDouble(REFGUID k, double* v) { return attributes_->GetDouble(k, v); }
IFACEMETHODIMP ZeroCamActivator::GetGUID(REFGUID k, GUID* v) { return attributes_->GetGUID(k, v); }
IFACEMETHODIMP ZeroCamActivator::GetStringLength(REFGUID k, UINT32* v) { return attributes_->GetStringLength(k, v); }
IFACEMETHODIMP ZeroCamActivator::GetString(REFGUID k, LPWSTR s, UINT32 b, UINT32* l) { return attributes_->GetString(k, s, b, l); }
IFACEMETHODIMP ZeroCamActivator::GetAllocatedString(REFGUID k, LPWSTR* s, UINT32* l) { return attributes_->GetAllocatedString(k, s, l); }
IFACEMETHODIMP ZeroCamActivator::GetBlobSize(REFGUID k, UINT32* s) { return attributes_->GetBlobSize(k, s); }
IFACEMETHODIMP ZeroCamActivator::GetBlob(REFGUID k, UINT8* b, UINT32 s, UINT32* r) { return attributes_->GetBlob(k, b, s, r); }
IFACEMETHODIMP ZeroCamActivator::GetAllocatedBlob(REFGUID k, UINT8** b, UINT32* s) { return attributes_->GetAllocatedBlob(k, b, s); }
IFACEMETHODIMP ZeroCamActivator::GetUnknown(REFGUID k, REFIID r, LPVOID* p) { return attributes_->GetUnknown(k, r, p); }
IFACEMETHODIMP ZeroCamActivator::SetItem(REFGUID k, REFPROPVARIANT v) { return attributes_->SetItem(k, v); }
IFACEMETHODIMP ZeroCamActivator::DeleteItem(REFGUID k) { return attributes_->DeleteItem(k); }
IFACEMETHODIMP ZeroCamActivator::DeleteAllItems() { return attributes_->DeleteAllItems(); }
IFACEMETHODIMP ZeroCamActivator::SetUINT32(REFGUID k, UINT32 v) { return attributes_->SetUINT32(k, v); }
IFACEMETHODIMP ZeroCamActivator::SetUINT64(REFGUID k, UINT64 v) { return attributes_->SetUINT64(k, v); }
IFACEMETHODIMP ZeroCamActivator::SetDouble(REFGUID k, double v) { return attributes_->SetDouble(k, v); }
IFACEMETHODIMP ZeroCamActivator::SetGUID(REFGUID k, REFGUID v) { return attributes_->SetGUID(k, v); }
IFACEMETHODIMP ZeroCamActivator::SetString(REFGUID k, LPCWSTR v) { return attributes_->SetString(k, v); }
IFACEMETHODIMP ZeroCamActivator::SetBlob(REFGUID k, const UINT8* b, UINT32 s) { return attributes_->SetBlob(k, b, s); }
IFACEMETHODIMP ZeroCamActivator::SetUnknown(REFGUID k, IUnknown* p) { return attributes_->SetUnknown(k, p); }
IFACEMETHODIMP ZeroCamActivator::LockStore() { return attributes_->LockStore(); }
IFACEMETHODIMP ZeroCamActivator::UnlockStore() { return attributes_->UnlockStore(); }
IFACEMETHODIMP ZeroCamActivator::GetCount(UINT32* c) { return attributes_->GetCount(c); }
IFACEMETHODIMP ZeroCamActivator::GetItemByIndex(UINT32 i, GUID* k, PROPVARIANT* v) { return attributes_->GetItemByIndex(i, k, v); }
IFACEMETHODIMP ZeroCamActivator::CopyAllItems(IMFAttributes* d) { return attributes_->CopyAllItems(d); }

} // namespace zerocam
