#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <wrl/client.h>
#include <atomic>
#include <mutex>

namespace zerocam {

// Activador COM (IMFActivate) que instancia ZeroCamMediaSource bajo demanda desde FrameServer.
class ZeroCamActivator final : public IMFActivate {
public:
    ZeroCamActivator();
    ~ZeroCamActivator();

    // Inicializa la tienda interna de atributos.
    HRESULT Initialize();

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    IFACEMETHODIMP_(ULONG) AddRef() override;
    IFACEMETHODIMP_(ULONG) Release() override;

    // IMFActivate
    IFACEMETHODIMP ActivateObject(REFIID riid, void** ppv) override;
    IFACEMETHODIMP ShutdownObject() override;
    IFACEMETHODIMP DetachObject() override;

    // IMFAttributes
    IFACEMETHODIMP GetItem(REFGUID guidKey, PROPVARIANT* pValue) override;
    IFACEMETHODIMP GetItemType(REFGUID guidKey, MF_ATTRIBUTE_TYPE* pType) override;
    IFACEMETHODIMP CompareItem(REFGUID guidKey, REFPROPVARIANT Value, BOOL* pbResult) override;
    IFACEMETHODIMP Compare(IMFAttributes* pTheirs, MF_ATTRIBUTES_MATCH_TYPE MatchType, BOOL* pbResult) override;
    IFACEMETHODIMP GetUINT32(REFGUID guidKey, UINT32* punValue) override;
    IFACEMETHODIMP GetUINT64(REFGUID guidKey, UINT64* punValue) override;
    IFACEMETHODIMP GetDouble(REFGUID guidKey, double* pfValue) override;
    IFACEMETHODIMP GetGUID(REFGUID guidKey, GUID* pguidValue) override;
    IFACEMETHODIMP GetStringLength(REFGUID guidKey, UINT32* pcchLength) override;
    IFACEMETHODIMP GetString(REFGUID guidKey, LPWSTR pwszValue, UINT32 cchBufSize, UINT32* pcchLength) override;
    IFACEMETHODIMP GetAllocatedString(REFGUID guidKey, LPWSTR* ppwszValue, UINT32* pcchLength) override;
    IFACEMETHODIMP GetBlobSize(REFGUID guidKey, UINT32* pcbBlobSize) override;
    IFACEMETHODIMP GetBlob(REFGUID guidKey, UINT8* pBuf, UINT32 cbBufSize, UINT32* pcbBlobSize) override;
    IFACEMETHODIMP GetAllocatedBlob(REFGUID guidKey, UINT8** ppBuf, UINT32* pcbSize) override;
    IFACEMETHODIMP GetUnknown(REFGUID guidKey, REFIID riid, LPVOID* ppv) override;
    IFACEMETHODIMP SetItem(REFGUID guidKey, REFPROPVARIANT Value) override;
    IFACEMETHODIMP DeleteItem(REFGUID guidKey) override;
    IFACEMETHODIMP DeleteAllItems() override;
    IFACEMETHODIMP SetUINT32(REFGUID guidKey, UINT32 unValue) override;
    IFACEMETHODIMP SetUINT64(REFGUID guidKey, UINT64 unValue) override;
    IFACEMETHODIMP SetDouble(REFGUID guidKey, double fValue) override;
    IFACEMETHODIMP SetGUID(REFGUID guidKey, REFGUID guidValue) override;
    IFACEMETHODIMP SetString(REFGUID guidKey, LPCWSTR wszValue) override;
    IFACEMETHODIMP SetBlob(REFGUID guidKey, const UINT8* pBuf, UINT32 cbBufSize) override;
    IFACEMETHODIMP SetUnknown(REFGUID guidKey, IUnknown* pUnknown) override;
    IFACEMETHODIMP LockStore() override;
    IFACEMETHODIMP UnlockStore() override;
    IFACEMETHODIMP GetCount(UINT32* pcItems) override;
    IFACEMETHODIMP GetItemByIndex(UINT32 unIndex, GUID* pguidKey, PROPVARIANT* pValue) override;
    IFACEMETHODIMP CopyAllItems(IMFAttributes* pDest) override;

private:
    std::atomic<ULONG> refCount_{ 1 };
    std::mutex mutex_;
    Microsoft::WRL::ComPtr<IMFAttributes> attributes_;
    Microsoft::WRL::ComPtr<IMFMediaSource> activeSource_;
};

} // namespace zerocam
