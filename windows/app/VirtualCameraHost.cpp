#include "VirtualCameraHost.h"
#include "../shared/ZeroCamGuids.h"
#include "../shared/ZeroCamIpc.h"
#include "../shared/ZeroCamProtocol.h"
#include <aclapi.h>
#include <netfw.h>
#include <oleauto.h>
#include <sddl.h>
#include <cstdio>
#include <fstream>
#include <vector>

namespace zerocam {

namespace {

constexpr const wchar_t* kFwRuleTcpName       = L"ZeroCam Wi-Fi Video (TCP 5000-5010)";
constexpr const wchar_t* kFwRuleLegacyTcpName = L"ZeroCam Wi-Fi Video (TCP 5000)";
constexpr const wchar_t* kFwRuleUdpName       = L"ZeroCam Wi-Fi Discovery (UDP 5001)";

struct MfVirtualCamApi {
    HMODULE mod = nullptr;
    using FnIsSupported = HRESULT(STDAPICALLTYPE*)(MFVirtualCameraType, BOOL*);
    using FnCreate = HRESULT(STDAPICALLTYPE*)(
        MFVirtualCameraType,
        MFVirtualCameraLifetime,
        MFVirtualCameraAccess,
        LPCWSTR,
        LPCWSTR,
        const GUID*,
        ULONG,
        IMFVirtualCamera**);
    FnIsSupported isSupported = nullptr;
    FnCreate create = nullptr;
};

// Resuelve dinámicamente mfsensorgroup.dll (solo Windows 11) para conservar compatibilidad binaria con Windows 10.
const MfVirtualCamApi& GetMfVirtualCamApi() noexcept {
    static const MfVirtualCamApi api = []() {
        MfVirtualCamApi res{};
        res.mod = LoadLibraryExW(L"mfsensorgroup.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (res.mod) {
            res.isSupported = reinterpret_cast<MfVirtualCamApi::FnIsSupported>(
                GetProcAddress(res.mod, "MFIsVirtualCameraTypeSupported"));
            res.create = reinterpret_cast<MfVirtualCamApi::FnCreate>(
                GetProcAddress(res.mod, "MFCreateVirtualCamera"));
        }
        return res;
    }();
    return api;
}

// Inicializa COM en el hilo actual solo si aún no fue inicializado.
struct ScopedCoInit {
    HRESULT hr = E_FAIL;
    ScopedCoInit() { hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); }
    ~ScopedCoInit() { if (hr == S_OK || hr == S_FALSE) CoUninitialize(); }
};

// Administrador RAII simple para cadenas BSTR usadas por la API COM del Firewall.
struct ScopedBstr {
    BSTR val = nullptr;
    explicit ScopedBstr(const std::wstring& s) : val(SysAllocString(s.c_str())) {}
    ~ScopedBstr() { if (val) SysFreeString(val); }
    ScopedBstr(const ScopedBstr&) = delete;
    ScopedBstr& operator=(const ScopedBstr&) = delete;
};

// Crea o actualiza una regla entrante en Windows Defender Firewall para ZeroCam.exe.
bool UpsertFirewallRule(const wchar_t* ruleName, const wchar_t* desc, const std::wstring& exePath, long protocol, const std::wstring& localPorts) {
    ScopedCoInit co;
    if (FAILED(co.hr) && co.hr != RPC_E_CHANGED_MODE) return false;

    Microsoft::WRL::ComPtr<INetFwPolicy2> policy;
    if (FAILED(CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&policy)))) {
        return false;
    }
    Microsoft::WRL::ComPtr<INetFwRules> rules;
    if (FAILED(policy->get_Rules(&rules)) || !rules) return false;

    ScopedBstr bName(ruleName);
    ScopedBstr bLegacy(kFwRuleLegacyTcpName);
    ScopedBstr bDesc(desc);
    ScopedBstr bApp(exePath);
    ScopedBstr bPorts(localPorts);
    ScopedBstr bRemote(L"LocalSubnet");
    if (!bName.val || !bDesc.val || !bApp.val || !bPorts.val || !bRemote.val) return false;

    rules->Remove(bName.val);
    if (bLegacy.val) rules->Remove(bLegacy.val);

    Microsoft::WRL::ComPtr<INetFwRule> rule;
    if (FAILED(CoCreateInstance(__uuidof(NetFwRule), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&rule)))) {
        return false;
    }
    rule->put_Name(bName.val);
    rule->put_Description(bDesc.val);
    rule->put_ApplicationName(bApp.val);
    rule->put_Protocol(protocol);
    rule->put_LocalPorts(bPorts.val);
    rule->put_RemoteAddresses(bRemote.val);
    rule->put_Direction(NET_FW_RULE_DIR_IN);
    rule->put_Action(NET_FW_ACTION_ALLOW);
    // Restringe exposición exclusivamente a redes Privadas y de Dominio en la subred local (nunca Pública).
    rule->put_Profiles(NET_FW_PROFILE2_PRIVATE | NET_FW_PROFILE2_DOMAIN);
    rule->put_Enabled(VARIANT_TRUE);
    return SUCCEEDED(rules->Add(rule.Get()));
}

// Elimina las reglas entrantes de ZeroCam en Windows Defender Firewall.
void RemoveFirewallRules() {
    ScopedCoInit co;
    if (FAILED(co.hr) && co.hr != RPC_E_CHANGED_MODE) return;

    Microsoft::WRL::ComPtr<INetFwPolicy2> policy;
    if (FAILED(CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&policy)))) {
        return;
    }
    Microsoft::WRL::ComPtr<INetFwRules> rules;
    if (FAILED(policy->get_Rules(&rules)) || !rules) return;

    ScopedBstr bTcp(kFwRuleTcpName);
    ScopedBstr bLegacy(kFwRuleLegacyTcpName);
    ScopedBstr bUdp(kFwRuleUdpName);
    if (bTcp.val) rules->Remove(bTcp.val);
    if (bLegacy.val) rules->Remove(bLegacy.val);
    if (bUdp.val) rules->Remove(bUdp.val);
}

// Formatea un HRESULT como cadena hexadecimal legible (0xXXXXXXXX).
std::wstring FormatHrHex(HRESULT hr) {
    wchar_t buf[32]{};
    swprintf_s(buf, L"0x%08X", static_cast<unsigned int>(hr));
    return buf;
}

// Verifica por handle que un archivo existente (si existe) no sea reparse point ni hardlink antes de sobrescribirlo.
bool ValidateExistingFileOrMissing(const std::wstring& path) noexcept {
    HANDLE h = CreateFileW(
        path.c_str(),
        READ_CONTROL,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND;
    }
    const bool valid = ValidateOpenedIpcFileHandle(h);
    CloseHandle(h);
    return valid;
}

// Verifica si una ruta existente corresponde a un reparse point (symlink o junction).
bool IsPathReparsePoint(const std::wstring& path) noexcept {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool IsExistingRegularFile(const std::wstring& path) noexcept {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
           (attrs & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

// Aplica una DACL protegida (y opcionalmente Mandatory Label SACL) por handle evitando TOCTOU por nombre.
bool ApplySddlByHandle(HANDLE handle, const wchar_t* sddl, bool includeMandatoryLabel) {
    if (handle == INVALID_HANDLE_VALUE || handle == nullptr) {
        return false;
    }
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, nullptr)) {
        return false;
    }

    BOOL daclPresent = FALSE;
    BOOL daclDefaulted = FALSE;
    PACL dacl = nullptr;
    BOOL saclPresent = FALSE;
    BOOL saclDefaulted = FALSE;
    PACL sacl = nullptr;

    bool ok = false;
    const bool hasDacl = (GetSecurityDescriptorDacl(sd, &daclPresent, &dacl, &daclDefaulted) == TRUE && daclPresent == TRUE);
    const bool hasSacl = !includeMandatoryLabel ||
        (GetSecurityDescriptorSacl(sd, &saclPresent, &sacl, &saclDefaulted) == TRUE && saclPresent == TRUE);

    if (hasDacl && hasSacl) {
        SECURITY_INFORMATION secInfo = DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION;
        if (includeMandatoryLabel) {
            secInfo |= LABEL_SECURITY_INFORMATION;
        }
        const DWORD rc = SetSecurityInfo(
            handle,
            SE_FILE_OBJECT,
            secInfo,
            nullptr,
            nullptr,
            dacl,
            includeMandatoryLabel ? sacl : nullptr);
        ok = (rc == ERROR_SUCCESS);
    }
    LocalFree(sd);
    return ok;
}

// Crea/abre C:\ProgramData\ZeroCam, valida que no sea reparse point y corta herencia antes de escribir archivos.
bool EnsureProtectedProgramDataDir(const std::wstring& dirPath) {
    CreateDirectoryW(dirPath.c_str(), nullptr);
    HANDLE hDir = CreateFileW(
        dirPath.c_str(),
        READ_CONTROL | WRITE_DAC,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    if (hDir == INVALID_HANDLE_VALUE) {
        return false;
    }

    BY_HANDLE_FILE_INFORMATION info{};
    const bool validDir = GetFileInformationByHandle(hDir, &info) == TRUE &&
                          (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                          (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
    const bool aclOk = validDir && ApplySddlByHandle(
        hDir,
        L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FRFX;;;LS)(A;OICI;FRFX;;;IU)",
        false);
    CloseHandle(hDir);
    return aclOk;
}

// Lee un archivo completo en binario para comparación exacta de bytes entre DLLs.
bool ReadBinaryFile(const std::wstring& path, std::vector<char>& outBytes) {
    std::ifstream ifs(path, std::ios::binary | std::ios::ate);
    if (!ifs.good()) return false;
    const std::streamsize sz = ifs.tellg();
    if (sz < 0) return false;
    outBytes.resize(static_cast<size_t>(sz));
    ifs.seekg(0, std::ios::beg);
    return sz == 0 || ifs.read(outBytes.data(), sz).good();
}

bool AreFilesIdentical(const std::wstring& localPath, const std::wstring& installedPath) {
    WIN32_FILE_ATTRIBUTE_DATA localInfo{}, instInfo{};
    if (!GetFileAttributesExW(localPath.c_str(), GetFileExInfoStandard, &localInfo)) {
        return true;
    }
    if (!GetFileAttributesExW(installedPath.c_str(), GetFileExInfoStandard, &instInfo)) {
        return false;
    }
    if (localInfo.nFileSizeHigh != instInfo.nFileSizeHigh ||
        localInfo.nFileSizeLow != instInfo.nFileSizeLow) {
        return false;
    }
    std::vector<char> localBytes, instBytes;
    if (!ReadBinaryFile(localPath, localBytes)) return true;
    if (!ReadBinaryFile(installedPath, instBytes)) return false;
    return localBytes == instBytes;
}

bool QueryClsidInprocPath(const wchar_t* clsidString, REGSAM wowFlag, std::wstring& outPath) {
    const std::wstring keyPath = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + clsidString + L"\\InprocServer32";
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyPath.c_str(), 0, KEY_READ | wowFlag, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    wchar_t pathBuf[MAX_PATH]{};
    DWORD cbData = sizeof(pathBuf);
    DWORD type = 0;
    const LONG rc = RegQueryValueExW(hKey, nullptr, nullptr, &type, reinterpret_cast<BYTE*>(pathBuf), &cbData);
    RegCloseKey(hKey);
    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        return false;
    }
    outPath = pathBuf;
    return GetFileAttributesW(pathBuf) != INVALID_FILE_ATTRIBUTES;
}

void CleanupStaleOldDlls(const std::wstring& dirPath) noexcept {
    if (dirPath.empty() || IsPathReparsePoint(dirPath)) return;
    const std::wstring pattern = dirPath + L"\\*.old*";
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;
    do {
        if ((fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0) {
            const std::wstring full = dirPath + L"\\" + fd.cFileName;
            DeleteFileW(full.c_str());
        }
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
}

bool CopyAndHardenDll(const std::wstring& srcPath, const std::wstring& dstPath) {
    if (!ValidateExistingFileOrMissing(dstPath)) return false;
    bool copied = false;
    for (int tries = 0; tries < 3; ++tries) {
        if (CopyFileW(srcPath.c_str(), dstPath.c_str(), FALSE)) {
            copied = true;
            break;
        }
        Sleep(100);
    }
    if (!copied && IsExistingRegularFile(dstPath)) {
        // Si otro proceso (Discord/OBS) tiene mapeada la DLL en uso, renombra el bloqueo a .old.* y escribe la nueva.
        const std::wstring oldPath = dstPath + L".old." + std::to_wstring(GetTickCount64());
        if (!IsPathReparsePoint(oldPath) &&
            MoveFileExW(dstPath.c_str(), oldPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            MoveFileExW(oldPath.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
            for (int tries = 0; tries < 5; ++tries) {
                if (CopyFileW(srcPath.c_str(), dstPath.c_str(), FALSE)) {
                    copied = true;
                    break;
                }
                Sleep(100);
            }
        }
    }
    if (!copied) return false;

    HANDLE hCheck = CreateFileW(
        dstPath.c_str(),
        READ_CONTROL | WRITE_DAC,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    const bool valid = (hCheck != INVALID_HANDLE_VALUE) &&
                       ValidateOpenedIpcFileHandle(hCheck) &&
                       ApplySddlByHandle(
                           hCheck,
                           L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FRFX;;;LS)(A;;FRFX;;;IU)",
                           false);
    if (hCheck != INVALID_HANDLE_VALUE) CloseHandle(hCheck);
    return valid;
}

HRESULT CallDllRegEntry(const std::wstring& dllPath, const char* entryName) {
    HRESULT hr = E_FAIL;
    if (HMODULE mod = LoadLibraryW(dllPath.c_str())) {
        using RegFn = HRESULT(STDAPICALLTYPE*)();
        if (auto pFn = reinterpret_cast<RegFn>(GetProcAddress(mod, entryName))) {
            hr = pFn();
        }
        FreeLibrary(mod);
    }
    return hr;
}

bool RunWow64Regsvr32(const std::wstring& dllPath, bool unregister) {
    wchar_t sysDir[MAX_PATH]{};
    const UINT n = GetSystemWow64DirectoryW(sysDir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    const std::wstring regsvr = std::wstring(sysDir, n) + L"\\regsvr32.exe";
    if (!IsExistingRegularFile(regsvr)) return false;

    std::wstring cmd = L"\"" + regsvr + L"\" /s " + (unregister ? L"/u " : L"") + L"\"" + dllPath + L"\"";
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(regsvr.c_str(), cmdBuf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return false;
    }
    CloseHandle(pi.hThread);
    const DWORD waitRc = WaitForSingleObject(pi.hProcess, 10000);
    DWORD exitCode = 1;
    if (waitRc == WAIT_OBJECT_0) {
        GetExitCodeProcess(pi.hProcess, &exitCode);
    } else {
        TerminateProcess(pi.hProcess, 1);
    }
    CloseHandle(pi.hProcess);
    return waitRc == WAIT_OBJECT_0 && exitCode == 0;
}

HRESULT CreateMfCamWithCategories(
    const MfVirtualCamApi& api,
    MFVirtualCameraLifetime lifetime,
    MFVirtualCameraAccess access,
    IMFVirtualCamera** ppCam) {
    if (!api.create || !ppCam) return E_POINTER;
    const GUID cats[2] = { kKsCategoryVideoCamera, kKsCategoryCapture };
    HRESULT hr = api.create(
        MFVirtualCameraType_SoftwareCameraSource,
        lifetime,
        access,
        kZeroCamFriendlyName,
        kZeroCamClsidString,
        cats,
        2,
        ppCam);
    if (FAILED(hr)) {
        hr = api.create(
            MFVirtualCameraType_SoftwareCameraSource,
            lifetime,
            access,
            kZeroCamFriendlyName,
            kZeroCamClsidString,
            cats,
            1,
            ppCam);
    }
    if (FAILED(hr)) {
        hr = api.create(
            MFVirtualCameraType_SoftwareCameraSource,
            lifetime,
            access,
            kZeroCamFriendlyName,
            kZeroCamClsidString,
            nullptr,
            0,
            ppCam);
    }
    return hr;
}

void RemoveAllSystemMfCameras(const MfVirtualCamApi& api) {
    if (!api.create) return;
    const GUID cats[2] = { kKsCategoryVideoCamera, kKsCategoryCapture };
    const struct {
        const GUID* ptr;
        ULONG count;
    } variants[] = {
        { cats, 2 },
        { cats, 1 },
        { nullptr, 0 }
    };
    for (const auto& v : variants) {
        Microsoft::WRL::ComPtr<IMFVirtualCamera> cam;
        if (SUCCEEDED(api.create(
                MFVirtualCameraType_SoftwareCameraSource,
                MFVirtualCameraLifetime_System,
                MFVirtualCameraAccess_AllUsers,
                kZeroCamFriendlyName,
                kZeroCamClsidString,
                v.ptr,
                v.count,
                &cam)) && cam) {
            cam->Stop();
            cam->Remove();
            cam->Shutdown();
        }
    }
}

void UnregisterDShowBestEffort() {
    std::wstring dll64;
    if (QueryClsidInprocPath(kZeroCamDShowClsidString, 0, dll64) && IsExistingRegularFile(dll64)) {
        (void)CallDllRegEntry(dll64, "DllUnregisterServer");
    } else if (IsExistingRegularFile(L"C:\\ProgramData\\ZeroCam\\ZeroCamDShow64.dll")) {
        (void)CallDllRegEntry(L"C:\\ProgramData\\ZeroCam\\ZeroCamDShow64.dll", "DllUnregisterServer");
    }

    std::wstring dll32;
    if (QueryClsidInprocPath(kZeroCamDShowClsidString, KEY_WOW64_32KEY, dll32) && IsExistingRegularFile(dll32)) {
        (void)RunWow64Regsvr32(dll32, true);
    } else if (IsExistingRegularFile(L"C:\\ProgramData\\ZeroCam\\ZeroCamDShow32.dll")) {
        (void)RunWow64Regsvr32(L"C:\\ProgramData\\ZeroCam\\ZeroCamDShow32.dll", true);
    }

    const std::wstring clsidDShow = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kZeroCamDShowClsidString;
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, clsidDShow.c_str());
    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, clsidDShow.c_str(), KEY_WOW64_32KEY, 0);
}

std::wstring GetExecutableDirectory() {
    wchar_t exePath[MAX_PATH]{};
    const DWORD n = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring dir(exePath, n);
    const size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    dir.resize(slash + 1);
    return dir;
}

} // namespace

VirtualCameraHost::~VirtualCameraHost() {
    Stop();
}

bool VirtualCameraHost::IsMfVirtualCameraSupported() {
    const MfVirtualCamApi& api = GetMfVirtualCamApi();
    if (!api.isSupported || !api.create) {
        return false;
    }
    BOOL supported = FALSE;
    return SUCCEEDED(api.isSupported(MFVirtualCameraType_SoftwareCameraSource, &supported)) && supported == TRUE;
}

bool VirtualCameraHost::IsPlatformSupported() {
    // Soporta Windows 11 (MFCreateVirtualCamera) y Windows 10 (DirectShow).
    return true;
}

bool VirtualCameraHost::IsDShowRegistered(std::wstring* outDll64Path) {
    std::wstring path64;
    if (!QueryClsidInprocPath(kZeroCamDShowClsidString, 0, path64)) {
        return false;
    }
    if (outDll64Path) *outDll64Path = path64;
    return true;
}

bool VirtualCameraHost::IsSystemMfCameraPresent() {
    if (!IsMfVirtualCameraSupported()) return false;
    ScopedCoInit co;
    if (FAILED(co.hr) && co.hr != RPC_E_CHANGED_MODE) return false;

    Microsoft::WRL::ComPtr<IMFAttributes> attrs;
    if (FAILED(MFCreateAttributes(&attrs, 1)) ||
        FAILED(attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID))) {
        return false;
    }

    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    if (FAILED(MFEnumDeviceSources(attrs.Get(), &devices, &count)) || !devices) {
        return false;
    }

    bool found = false;
    const size_t prefixLen = wcslen(kZeroCamFriendlyName);
    for (UINT32 i = 0; i < count; ++i) {
        if (devices[i]) {
            wchar_t nameBuf[256]{};
            UINT32 len = 0;
            if (!found && SUCCEEDED(devices[i]->GetString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, nameBuf, 256, &len))) {
                // Windows 11 anexa " (Windows Virtual Camera)" al nombre visible del dispositivo MF.
                if (_wcsnicmp(nameBuf, kZeroCamFriendlyName, prefixLen) == 0) {
                    found = true;
                }
            }
            devices[i]->Release();
        }
    }
    CoTaskMemFree(devices);
    return found;
}

bool VirtualCameraHost::IsComRegistered(std::wstring* outDllPath) {
    if (IsMfVirtualCameraSupported()) {
        std::wstring path;
        if (!QueryClsidInprocPath(kZeroCamClsidString, 0, path)) {
            return false;
        }
        // En Windows 11 Discord requiere DirectShow ademas de Media Foundation.
        if (!IsDShowRegistered()) {
            return false;
        }
        if (outDllPath) *outDllPath = path;
        return true;
    }
    return IsDShowRegistered(outDllPath);
}

bool VirtualCameraHost::IsInstalledDllUpToDate() {
    const std::wstring localDir = GetExecutableDirectory();
    if (localDir.empty()) return true;

    if (IsMfVirtualCameraSupported()) {
        std::wstring installedMf;
        if (!QueryClsidInprocPath(kZeroCamClsidString, 0, installedMf)) return false;
        if (!AreFilesIdentical(localDir + L"ZeroCamSource.dll", installedMf)) return false;
    }

    std::wstring installed64;
    if (!IsDShowRegistered(&installed64)) return false;
    if (!AreFilesIdentical(localDir + L"ZeroCamDShow64.dll", installed64)) return false;

    const std::wstring local32 = localDir + L"ZeroCamDShow32.dll";
    if (IsExistingRegularFile(local32)) {
        std::wstring installed32;
        if (!QueryClsidInprocPath(kZeroCamDShowClsidString, KEY_WOW64_32KEY, installed32)) {
            return false;
        }
        if (!AreFilesIdentical(local32, installed32)) {
            return false;
        }
    }
    return true;
}

bool VirtualCameraHost::InstallAndRegisterElevated(std::wstring* outError) {
    auto fail = [&](const std::wstring& msg) -> bool {
        if (outError) *outError = msg;
        return false;
    };

    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    const std::wstring localExe(exePath);
    const std::wstring localDir = GetExecutableDirectory();
    if (localDir.empty()) {
        return fail(L"No se pudo resolver la carpeta del ejecutable.");
    }

    const bool useMfWin11 = IsMfVirtualCameraSupported();
    const std::wstring localMfDll = localDir + L"ZeroCamSource.dll";
    const std::wstring localDs64 = localDir + L"ZeroCamDShow64.dll";
    const std::wstring localDs32 = localDir + L"ZeroCamDShow32.dll";

    if (!IsExistingRegularFile(localDs64)) {
        return fail(L"No se encontró ZeroCamDShow64.dll junto a ZeroCam.exe.");
    }
    if (useMfWin11 && !IsExistingRegularFile(localMfDll)) {
        return fail(L"No se encontró ZeroCamSource.dll junto a ZeroCam.exe.");
    }

    // Detiene FrameServer temporalmente si está activo en Win11 para reemplazar DLLs sin bloqueo.
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    SC_HANDLE svc = (useMfWin11 && scm) ? OpenServiceW(scm, L"FrameServer", SERVICE_QUERY_STATUS | SERVICE_STOP | SERVICE_START) : nullptr;
    bool wasRunning = false;
    if (svc) {
        SERVICE_STATUS_PROCESS ssp{};
        DWORD needed = 0;
        if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp), &needed)) {
            wasRunning = (ssp.dwCurrentState == SERVICE_RUNNING || ssp.dwCurrentState == SERVICE_START_PENDING);
            if (wasRunning) {
                SERVICE_STATUS ignored{};
                ControlService(svc, SERVICE_CONTROL_STOP, &ignored);
                for (int i = 0; i < 25; ++i) {
                    Sleep(200);
                    if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp), &needed) &&
                        ssp.dwCurrentState == SERVICE_STOPPED) {
                        break;
                    }
                }
            }
        }
    }

    const std::wstring dirPath = L"C:\\ProgramData\\ZeroCam";
    if (!EnsureProtectedProgramDataDir(dirPath)) {
        if (wasRunning && svc) StartServiceW(svc, 0, nullptr);
        if (svc) CloseServiceHandle(svc);
        if (scm) CloseServiceHandle(scm);
        return fail(L"No se pudo proteger C:\\ProgramData\\ZeroCam (posible reparse point o permiso insuficiente).");
    }
    CleanupStaleOldDlls(dirPath);

    // Pre-crea/valida ipc_ring.bin por handle con tamaño exacto y DACL endurecida (SY/BA/LS/IU, sin AC/BU).
    bool ipcOk = false;
    if (ValidateExistingFileOrMissing(kZeroCamDefaultIpcPath)) {
        HANDLE hIpc = CreateFileW(
            kZeroCamDefaultIpcPath,
            GENERIC_READ | GENERIC_WRITE | WRITE_DAC | WRITE_OWNER,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr);
        if (hIpc != INVALID_HANDLE_VALUE) {
            if (ValidateOpenedIpcFileHandle(hIpc)) {
                LARGE_INTEGER curSz{};
                const LONGLONG reqSz = static_cast<LONGLONG>(sizeof(ZeroCamSharedMemory));
                bool sizeOk = GetFileSizeEx(hIpc, &curSz) == TRUE;
                if (sizeOk && curSz.QuadPart != reqSz) {
                    LARGE_INTEGER targetSz{};
                    targetSz.QuadPart = reqSz;
                    sizeOk = (SetFilePointerEx(hIpc, targetSz, nullptr, FILE_BEGIN) == TRUE &&
                              SetEndOfFile(hIpc) == TRUE);
                }
                if (sizeOk) {
                    ipcOk = ApplySddlByHandle(
                        hIpc,
                        L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FRFW;;;LS)(A;;FRFW;;;IU)S:(ML;;NWNR;;;ME)",
                        true);
                }
            }
            CloseHandle(hIpc);
        }
    }
    if (!ipcOk) {
        if (wasRunning && svc) StartServiceW(svc, 0, nullptr);
        if (svc) CloseServiceHandle(svc);
        if (scm) CloseServiceHandle(scm);
        return fail(L"No se pudo crear o endurecer los permisos de ipc_ring.bin.");
    }

    if (useMfWin11) {
        const std::wstring installedMf = dirPath + L"\\ZeroCamSource.dll";
        if (!CopyAndHardenDll(localMfDll, installedMf)) {
            if (wasRunning && svc) StartServiceW(svc, 0, nullptr);
            if (svc) CloseServiceHandle(svc);
            if (scm) CloseServiceHandle(scm);
            return fail(L"No se pudo copiar o proteger ZeroCamSource.dll en ProgramData.");
        }

        const HRESULT regHr = CallDllRegEntry(installedMf, "DllRegisterServer");
        if (wasRunning && svc) StartServiceW(svc, 0, nullptr);
        if (svc) CloseServiceHandle(svc);
        if (scm) CloseServiceHandle(scm);

        if (FAILED(regHr)) {
            return fail(L"DllRegisterServer falló (HRESULT " + FormatHrHex(regHr) + L").");
        }
        std::wstring mfPath;
        if (!QueryClsidInprocPath(kZeroCamClsidString, 0, mfPath)) {
            return fail(L"El registro COM de ZeroCamSource no pudo verificarse en HKLM.");
        }

        // Limpia variantes duplicadas y registra una única cámara persistente AllUsers en Win11.
        ScopedCoInit co;
        const MfVirtualCamApi& api = GetMfVirtualCamApi();
        if (api.create && (SUCCEEDED(co.hr) || co.hr == RPC_E_CHANGED_MODE)) {
            RemoveAllSystemMfCameras(api);
            Sleep(100);
            Microsoft::WRL::ComPtr<IMFVirtualCamera> sysCam;
            if (SUCCEEDED(CreateMfCamWithCategories(api, MFVirtualCameraLifetime_System, MFVirtualCameraAccess_AllUsers, &sysCam)) && sysCam) {
                (void)sysCam->Start(nullptr);
                (void)sysCam->Shutdown();
            }
        }
    } else {
        if (svc) CloseServiceHandle(svc);
        if (scm) CloseServiceHandle(scm);
    }

    // Registra filtro DirectShow 64-bit y 32-bit tanto en Windows 10 como en Windows 11 (Discord/OBS/Zoom).
    const std::wstring installed64 = dirPath + L"\\ZeroCamDShow64.dll";
    if (!CopyAndHardenDll(localDs64, installed64)) {
        return fail(L"No se pudo copiar o proteger ZeroCamDShow64.dll en ProgramData.");
    }
    const HRESULT hr64 = CallDllRegEntry(installed64, "DllRegisterServer");
    if (FAILED(hr64)) {
        return fail(L"DllRegisterServer DirectShow 64-bit falló (HRESULT " + FormatHrHex(hr64) + L").");
    }

    if (IsExistingRegularFile(localDs32)) {
        const std::wstring installed32 = dirPath + L"\\ZeroCamDShow32.dll";
        if (!CopyAndHardenDll(localDs32, installed32) || !RunWow64Regsvr32(installed32, false)) {
            return fail(L"No se pudo registrar ZeroCamDShow32.dll (compatibilidad apps 32-bit).");
        }
    }

    if (!IsComRegistered() || !IsDShowRegistered() || !IsInstalledDllUpToDate()) {
        return fail(L"No se pudo verificar el registro COM/DirectShow en HKLM.");
    }

    UpsertFirewallRule(
        kFwRuleTcpName,
        L"Permite recibir video ZeroCam desde Android por Wi-Fi (TCP)",
        localExe,
        NET_FW_IP_PROTOCOL_TCP,
        L"5000-5010");
    UpsertFirewallRule(
        kFwRuleUdpName,
        L"Permite descubrimiento automático de ZeroCam en red local (UDP)",
        localExe,
        NET_FW_IP_PROTOCOL_UDP,
        std::to_wstring(kDiscoveryUdpPort));

    if (outError) outError->clear();
    return true;
}

bool VirtualCameraHost::UninstallAndUnregisterElevated(std::wstring* outError) {
    const bool useMfWin11 = IsMfVirtualCameraSupported();
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    SC_HANDLE svc = (useMfWin11 && scm) ? OpenServiceW(scm, L"FrameServer", SERVICE_QUERY_STATUS | SERVICE_STOP | SERVICE_START) : nullptr;

    if (useMfWin11) {
        ScopedCoInit co;
        const MfVirtualCamApi& api = GetMfVirtualCamApi();
        if (api.create && (SUCCEEDED(co.hr) || co.hr == RPC_E_CHANGED_MODE)) {
            RemoveAllSystemMfCameras(api);
        }
    }

    UnregisterDShowBestEffort();

    bool wasRunning = false;
    if (svc) {
        SERVICE_STATUS_PROCESS ssp{};
        DWORD needed = 0;
        if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp), &needed)) {
            wasRunning = (ssp.dwCurrentState == SERVICE_RUNNING || ssp.dwCurrentState == SERVICE_START_PENDING);
            if (wasRunning) {
                SERVICE_STATUS ignored{};
                ControlService(svc, SERVICE_CONTROL_STOP, &ignored);
                for (int i = 0; i < 25; ++i) {
                    Sleep(200);
                    if (QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp), &needed) &&
                        ssp.dwCurrentState == SERVICE_STOPPED) {
                        break;
                    }
                }
            }
        }
    }

    const std::wstring clsidMf = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kZeroCamClsidString;
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, clsidMf.c_str());

    RemoveFirewallRules();

    if (!IsPathReparsePoint(L"C:\\ProgramData\\ZeroCam")) {
        for (int tries = 0; tries < 10; ++tries) {
            CleanupStaleOldDlls(L"C:\\ProgramData\\ZeroCam");
            if (!IsPathReparsePoint(L"C:\\ProgramData\\ZeroCam\\ZeroCamSource.dll")) {
                DeleteFileW(L"C:\\ProgramData\\ZeroCam\\ZeroCamSource.dll");
            }
            if (!IsPathReparsePoint(L"C:\\ProgramData\\ZeroCam\\ZeroCamDShow64.dll")) {
                DeleteFileW(L"C:\\ProgramData\\ZeroCam\\ZeroCamDShow64.dll");
            }
            if (!IsPathReparsePoint(L"C:\\ProgramData\\ZeroCam\\ZeroCamDShow32.dll")) {
                DeleteFileW(L"C:\\ProgramData\\ZeroCam\\ZeroCamDShow32.dll");
            }
            if (!IsPathReparsePoint(kZeroCamDefaultIpcPath)) {
                DeleteFileW(kZeroCamDefaultIpcPath);
            }
            if (RemoveDirectoryW(L"C:\\ProgramData\\ZeroCam") || GetLastError() == ERROR_FILE_NOT_FOUND) {
                break;
            }
            Sleep(150);
        }
    }

    if (wasRunning && svc) StartServiceW(svc, 0, nullptr);
    if (svc) CloseServiceHandle(svc);
    if (scm) CloseServiceHandle(scm);

    if (IsComRegistered() || IsDShowRegistered()) {
        if (outError) *outError = L"No se pudo eliminar el registro COM de ZeroCam en HKLM.";
        return false;
    }
    if (outError) outError->clear();
    return true;
}

bool VirtualCameraHost::Start(std::wstring& outError) {
    Stop();

    if (IsMfVirtualCameraSupported()) {
        if (IsSystemMfCameraPresent()) {
            systemCameraActive_ = true;
            dshowActive_ = IsDShowRegistered();
            outError.clear();
            return true;
        }

        std::wstring ignored;
        if (!QueryClsidInprocPath(kZeroCamClsidString, 0, ignored)) {
            outError = L"Falta configurar la cámara virtual (acepta el permiso de administrador).";
            return false;
        }

        const MfVirtualCamApi& api = GetMfVirtualCamApi();
        HRESULT hr = CreateMfCamWithCategories(
            api,
            MFVirtualCameraLifetime_Session,
            MFVirtualCameraAccess_CurrentUser,
            &virtualCamera_);
        if (FAILED(hr) || !virtualCamera_) {
            outError = L"MFCreateVirtualCamera falló (HRESULT " + FormatHrHex(hr) + L").";
            return false;
        }

        hr = virtualCamera_->Start(nullptr);
        if (FAILED(hr)) {
            virtualCamera_->Shutdown();
            virtualCamera_.Reset();
            outError = L"IMFVirtualCamera::Start falló (HRESULT " + FormatHrHex(hr) + L").";
            return false;
        }

        dshowActive_ = IsDShowRegistered();
        outError.clear();
        return true;
    }

    // Windows 10: opera mediante filtro DirectShow registrado en ProgramData.
    if (!IsDShowRegistered()) {
        outError = L"Falta registrar la cámara DirectShow para Windows 10 (acepta el permiso de administrador).";
        return false;
    }
    dshowActive_ = true;
    outError.clear();
    return true;
}

void VirtualCameraHost::Stop() {
    systemCameraActive_ = false;
    dshowActive_ = false;
    if (virtualCamera_) {
        virtualCamera_->Stop();
        virtualCamera_->Remove();
        virtualCamera_->Shutdown();
        virtualCamera_.Reset();
    }
}

} // namespace zerocam
