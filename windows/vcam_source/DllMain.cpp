#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <wrl/client.h>
#include <atomic>
#include <string>
#include "../shared/ZeroCamGuids.h"
#include "ZeroCamActivator.h"
#include "ZeroCamMediaSource.h"

namespace zerocam {

static std::atomic<ULONG> g_moduleRefs{ 0 };
static HMODULE g_moduleHandle = nullptr;

// Incrementa el contador de objetos activos en la DLL.
void ModuleAddRef() noexcept { g_moduleRefs.fetch_add(1, std::memory_order_relaxed); }
// Decrementa el contador de objetos activos en la DLL.
void ModuleRelease() noexcept { g_moduleRefs.fetch_sub(1, std::memory_order_acq_rel); }

// Fábrica COM para instanciar ZeroCamActivator o ZeroCamMediaSource según el IID solicitado.
class ZeroCamClassFactory final : public IClassFactory {
public:
    ZeroCamClassFactory() { ModuleAddRef(); }
    ~ZeroCamClassFactory() { ModuleRelease(); }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IClassFactory)) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    IFACEMETHODIMP_(ULONG) AddRef() override {
        return refCount_.fetch_add(1, std::memory_order_relaxed) + 1;
    }

    IFACEMETHODIMP_(ULONG) Release() override {
        ULONG count = refCount_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (count == 0) delete this;
        return count;
    }

    IFACEMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) override {
        if (pUnkOuter) return CLASS_E_NOAGGREGATION;
        if (!ppv) return E_POINTER;
        *ppv = nullptr;

        if (riid == __uuidof(IMFMediaSource) || riid == __uuidof(IMFMediaSourceEx)) {
            Microsoft::WRL::ComPtr<ZeroCamMediaSource> src;
            src.Attach(new ZeroCamMediaSource());
            HRESULT hr = src->Initialize();
            if (FAILED(hr)) return hr;
            return src->QueryInterface(riid, ppv);
        }

        Microsoft::WRL::ComPtr<ZeroCamActivator> act;
        act.Attach(new ZeroCamActivator());
        HRESULT hr = act->Initialize();
        if (FAILED(hr)) return hr;
        return act->QueryInterface(riid, ppv);
    }

    IFACEMETHODIMP LockServer(BOOL fLock) override {
        if (fLock) ModuleAddRef();
        else ModuleRelease();
        return S_OK;
    }

private:
    std::atomic<ULONG> refCount_{ 1 };
};

} // namespace zerocam

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        zerocam::g_moduleHandle = hModule;
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}

STDAPI DllCanUnloadNow() {
    return zerocam::g_moduleRefs.load(std::memory_order_acquire) == 0 ? S_OK : S_FALSE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (rclsid != CLSID_ZeroCamSource) return CLASS_E_CLASSNOTAVAILABLE;

    auto* factory = new zerocam::ZeroCamClassFactory();
    HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

STDAPI DllRegisterServer() {
    wchar_t modulePath[MAX_PATH]{};
    DWORD len = GetModuleFileNameW(zerocam::g_moduleHandle, modulePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return HRESULT_FROM_WIN32(GetLastError());

    const std::wstring clsidPath = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kZeroCamClsidString;
    const std::wstring inprocPath = clsidPath + L"\\InprocServer32";

    HKEY hClsid = nullptr;
    LONG rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, clsidPath.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hClsid, nullptr);
    if (rc != ERROR_SUCCESS) return HRESULT_FROM_WIN32(rc);
    RegSetValueExW(hClsid, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(kZeroCamRegistryDesc),
                   static_cast<DWORD>((wcslen(kZeroCamRegistryDesc) + 1) * sizeof(wchar_t)));
    RegCloseKey(hClsid);

    HKEY hInproc = nullptr;
    rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, inprocPath.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hInproc, nullptr);
    if (rc != ERROR_SUCCESS) return HRESULT_FROM_WIN32(rc);
    RegSetValueExW(hInproc, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(modulePath),
                   static_cast<DWORD>((wcslen(modulePath) + 1) * sizeof(wchar_t)));
    constexpr const wchar_t* kThreading = L"Both";
    RegSetValueExW(hInproc, L"ThreadingModel", 0, REG_SZ, reinterpret_cast<const BYTE*>(kThreading),
                   static_cast<DWORD>((wcslen(kThreading) + 1) * sizeof(wchar_t)));
    RegCloseKey(hInproc);
    return S_OK;
}

STDAPI DllUnregisterServer() {
    const std::wstring clsidPath = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kZeroCamClsidString;
    LONG rc = RegDeleteTreeW(HKEY_LOCAL_MACHINE, clsidPath.c_str());
    if (rc == ERROR_FILE_NOT_FOUND) return S_OK;
    return rc == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(rc);
}
