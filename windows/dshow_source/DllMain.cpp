#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NO_DSHOW_STRSAFE
#define NO_DSHOW_STRSAFE
#endif
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <dshow.h>
#include <wrl/client.h>
#include <atomic>
#include <string>
#include "../shared/ZeroCamGuids.h"
#include "ZeroCamDShowFilter.h"

namespace zerocam {

static std::atomic<ULONG> g_dshowModuleRefs{ 0 };
static HMODULE g_dshowModuleHandle = nullptr;

void DShowModuleAddRef() noexcept { g_dshowModuleRefs.fetch_add(1, std::memory_order_relaxed); }
void DShowModuleRelease() noexcept { g_dshowModuleRefs.fetch_sub(1, std::memory_order_acq_rel); }

namespace {

struct ScopedDShowCoInit {
    HRESULT hr = E_FAIL;
    ScopedDShowCoInit() { hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); }
    ~ScopedDShowCoInit() { if (hr == S_OK || hr == S_FALSE) CoUninitialize(); }
};

class ZeroCamDShowClassFactory final : public IClassFactory {
public:
    ZeroCamDShowClassFactory() { DShowModuleAddRef(); }
    ~ZeroCamDShowClassFactory() { DShowModuleRelease(); }

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
        const ULONG count = refCount_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (count == 0) delete this;
        return count;
    }

    IFACEMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) override {
        if (pUnkOuter) return CLASS_E_NOAGGREGATION;
        if (!ppv) return E_POINTER;
        *ppv = nullptr;

        Microsoft::WRL::ComPtr<ZeroCamDShowFilter> filter;
        filter.Attach(new ZeroCamDShowFilter());
        return filter->QueryInterface(riid, ppv);
    }

    IFACEMETHODIMP LockServer(BOOL fLock) override {
        if (fLock) DShowModuleAddRef();
        else DShowModuleRelease();
        return S_OK;
    }

private:
    std::atomic<ULONG> refCount_{ 1 };
};

} // namespace

} // namespace zerocam

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        zerocam::g_dshowModuleHandle = hModule;
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}

STDAPI DllCanUnloadNow() {
    return zerocam::g_dshowModuleRefs.load(std::memory_order_acquire) == 0 ? S_OK : S_FALSE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    if (rclsid != CLSID_ZeroCamDShow) return CLASS_E_CLASSNOTAVAILABLE;

    auto* factory = new zerocam::ZeroCamDShowClassFactory();
    const HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

STDAPI DllRegisterServer() {
    wchar_t modulePath[MAX_PATH]{};
    const DWORD len = GetModuleFileNameW(zerocam::g_dshowModuleHandle, modulePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return HRESULT_FROM_WIN32(GetLastError());

    const std::wstring clsidPath = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kZeroCamDShowClsidString;
    const std::wstring inprocPath = clsidPath + L"\\InprocServer32";

    HKEY hClsid = nullptr;
    LONG rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, clsidPath.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hClsid, nullptr);
    if (rc != ERROR_SUCCESS) return HRESULT_FROM_WIN32(rc);
    RegSetValueExW(hClsid, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(kZeroCamDShowRegDesc),
                   static_cast<DWORD>((wcslen(kZeroCamDShowRegDesc) + 1) * sizeof(wchar_t)));
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

    zerocam::ScopedDShowCoInit co;
    if (FAILED(co.hr) && co.hr != RPC_E_CHANGED_MODE) return co.hr;

    Microsoft::WRL::ComPtr<IFilterMapper2> mapper;
    HRESULT hr = CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mapper));
    if (FAILED(hr)) return hr;

    (void)mapper->UnregisterFilter(&CLSID_VideoInputDeviceCategory, kZeroCamFriendlyName, CLSID_ZeroCamDShow);

    const REGPINTYPES pinTypes[3] = {
        { &MEDIATYPE_Video, &kMediaSubtypeI420 },
        { &MEDIATYPE_Video, &MEDIASUBTYPE_NV12 },
        { &MEDIATYPE_Video, &MEDIASUBTYPE_YUY2 }
    };
    REGFILTERPINS2 pinInfo{};
    pinInfo.dwFlags = REG_PINFLAG_B_OUTPUT;
    pinInfo.cInstances = 1;
    pinInfo.nMediaTypes = 3;
    pinInfo.lpMediaType = pinTypes;
    pinInfo.nMediums = 0;
    pinInfo.lpMedium = nullptr;
    pinInfo.clsPinCategory = &kPinNameVideoCapture;

    REGFILTER2 rf2{};
    rf2.dwVersion = 2;
    rf2.dwMerit = MERIT_DO_NOT_USE;
    rf2.cPins2 = 1;
    rf2.rgPins2 = &pinInfo;

    Microsoft::WRL::ComPtr<IMoniker> moniker;
    hr = mapper->RegisterFilter(
        CLSID_ZeroCamDShow,
        kZeroCamFriendlyName,
        &moniker,
        &CLSID_VideoInputDeviceCategory,
        kZeroCamFriendlyName,
        &rf2);
    if (FAILED(hr)) return hr;

    if (moniker) {
        Microsoft::WRL::ComPtr<IPropertyBag> propBag;
        if (SUCCEEDED(moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&propBag))) && propBag) {
            VARIANT var{};
            VariantInit(&var);
            var.vt = VT_BSTR;
            var.bstrVal = SysAllocString(kZeroCamFriendlyName);
            if (var.bstrVal) {
                propBag->Write(L"FriendlyName", &var);
            }
            VariantClear(&var);
        }
    }
    return S_OK;
}

STDAPI DllUnregisterServer() {
    zerocam::ScopedDShowCoInit co;
    if (SUCCEEDED(co.hr) || co.hr == RPC_E_CHANGED_MODE) {
        Microsoft::WRL::ComPtr<IFilterMapper2> mapper;
        if (SUCCEEDED(CoCreateInstance(CLSID_FilterMapper2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mapper))) && mapper) {
            (void)mapper->UnregisterFilter(&CLSID_VideoInputDeviceCategory, kZeroCamFriendlyName, CLSID_ZeroCamDShow);
        }
    }

    const std::wstring clsidPath = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kZeroCamDShowClsidString;
    const LONG rc = RegDeleteTreeW(HKEY_LOCAL_MACHINE, clsidPath.c_str());
    if (rc == ERROR_FILE_NOT_FOUND || rc == ERROR_SUCCESS) return S_OK;
    return HRESULT_FROM_WIN32(rc);
}
