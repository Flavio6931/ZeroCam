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

} // namespace

VirtualCameraHost::~VirtualCameraHost() {
    Stop();
}

bool VirtualCameraHost::IsPlatformSupported() {
    BOOL supported = FALSE;
    return SUCCEEDED(MFIsVirtualCameraTypeSupported(MFVirtualCameraType_SoftwareCameraSource, &supported)) && supported == TRUE;
}

bool VirtualCameraHost::IsComRegistered(std::wstring* outDllPath) {
    const std::wstring keyPath = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kZeroCamClsidString + L"\\InprocServer32";
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, keyPath.c_str(), 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
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
    if (outDllPath) {
        *outDllPath = pathBuf;
    }
    return GetFileAttributesW(pathBuf) != INVALID_FILE_ATTRIBUTES;
}

bool VirtualCameraHost::IsInstalledDllUpToDate() {
    std::wstring installedPath;
    if (!IsComRegistered(&installedPath)) {
        return false;
    }

    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring localDll(exePath);
    const size_t slash = localDll.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return true;
    }
    localDll = localDll.substr(0, slash + 1) + L"ZeroCamSource.dll";

    WIN32_FILE_ATTRIBUTE_DATA localInfo{}, instInfo{};
    if (!GetFileAttributesExW(localDll.c_str(), GetFileExInfoStandard, &localInfo)) {
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
    if (!ReadBinaryFile(localDll, localBytes)) {
        return true;
    }
    if (!ReadBinaryFile(installedPath, instBytes)) {
        return false;
    }
    return localBytes == instBytes;
}

bool VirtualCameraHost::InstallAndRegisterElevated(std::wstring* outError) {
    auto fail = [&](const std::wstring& msg) -> bool {
        if (outError) *outError = msg;
        return false;
    };

    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    const std::wstring localExe(exePath);
    std::wstring localDll(localExe);
    const size_t slash = localDll.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return fail(L"No se pudo resolver la carpeta del ejecutable.");
    }
    localDll = localDll.substr(0, slash + 1) + L"ZeroCamSource.dll";
    if (GetFileAttributesW(localDll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return fail(L"No se encontró ZeroCamSource.dll junto a ZeroCam.exe.");
    }

    // Detiene FrameServer si estaba activo para poder reemplazar ZeroCamSource.dll sin bloqueo.
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    SC_HANDLE svc = scm ? OpenServiceW(scm, L"FrameServer", SERVICE_QUERY_STATUS | SERVICE_STOP | SERVICE_START) : nullptr;
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
    const std::wstring installedDll = dirPath + L"\\ZeroCamSource.dll";

    // Endurece la carpeta ANTES de copiar la DLL o crear ipc_ring.bin (corta herencia Users:(WD,AD) de ProgramData).
    if (!EnsureProtectedProgramDataDir(dirPath)) {
        if (wasRunning && svc) StartServiceW(svc, 0, nullptr);
        if (svc) CloseServiceHandle(svc);
        if (scm) CloseServiceHandle(scm);
        return fail(L"No se pudo proteger C:\\ProgramData\\ZeroCam (posible reparse point o permiso insuficiente).");
    }

    // Valida ausencia de reparse points y hardlinks (nNumberOfLinks == 1) ANTES de copiar/sobrescribir la DLL o abrir IPC.
    if (!ValidateExistingFileOrMissing(installedDll) || !ValidateExistingFileOrMissing(kZeroCamDefaultIpcPath)) {
        if (wasRunning && svc) StartServiceW(svc, 0, nullptr);
        if (svc) CloseServiceHandle(svc);
        if (scm) CloseServiceHandle(scm);
        return fail(L"Se detectó un enlace simbólico o hardlink no permitido en ProgramData\\ZeroCam.");
    }

    bool copied = false;
    for (int tries = 0; tries < 10; ++tries) {
        if (CopyFileW(localDll.c_str(), installedDll.c_str(), FALSE)) {
            copied = true;
            break;
        }
        Sleep(150);
    }
    if (!copied) {
        if (wasRunning && svc) StartServiceW(svc, 0, nullptr);
        if (svc) CloseServiceHandle(svc);
        if (scm) CloseServiceHandle(scm);
        return fail(L"No se pudo copiar ZeroCamSource.dll a ProgramData.");
    }

    // Valida la DLL instalada por handle (sin reparse points ni hardlinks) y fija DACL protegida.
    HANDLE hDllCheck = CreateFileW(
        installedDll.c_str(),
        READ_CONTROL | WRITE_DAC,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    const bool dllValid = (hDllCheck != INVALID_HANDLE_VALUE) &&
                          ValidateOpenedIpcFileHandle(hDllCheck) &&
                          ApplySddlByHandle(
                              hDllCheck,
                              L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FRFX;;;LS)(A;;FRFX;;;IU)",
                              false);
    if (hDllCheck != INVALID_HANDLE_VALUE) CloseHandle(hDllCheck);
    if (!dllValid) {
        if (wasRunning && svc) StartServiceW(svc, 0, nullptr);
        if (svc) CloseServiceHandle(svc);
        if (scm) CloseServiceHandle(scm);
        return fail(L"No se pudo verificar o proteger ZeroCamSource.dll en ProgramData.");
    }

    // Pre-crea ipc_ring.bin por handle con tamaño exacto, valida hardlinks/reparse y aplica DACL + Mandatory Label Medium.
    HANDLE hIpc = CreateFileW(
        kZeroCamDefaultIpcPath,
        GENERIC_READ | GENERIC_WRITE | WRITE_DAC | WRITE_OWNER,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr);
    bool ipcOk = false;
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
                // Sin AC ni BU: solo SYSTEM, Admins, LOCAL SERVICE (FrameServer) e Interactive User (ZeroCam.exe) a nivel Medium.
                ipcOk = ApplySddlByHandle(
                    hIpc,
                    L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FRFW;;;LS)(A;;FRFW;;;IU)S:(ML;;NWNR;;;ME)",
                    true);
            }
        }
        CloseHandle(hIpc);
    }
    if (!ipcOk) {
        if (wasRunning && svc) StartServiceW(svc, 0, nullptr);
        if (svc) CloseServiceHandle(svc);
        if (scm) CloseServiceHandle(scm);
        return fail(L"No se pudo crear o endurecer los permisos de ipc_ring.bin.");
    }

    HRESULT regHr = E_FAIL;
    if (HMODULE mod = LoadLibraryW(installedDll.c_str())) {
        using DllRegisterFn = HRESULT(STDAPICALLTYPE*)();
        if (auto pReg = reinterpret_cast<DllRegisterFn>(GetProcAddress(mod, "DllRegisterServer"))) {
            regHr = pReg();
        }
        FreeLibrary(mod);
    }

    if (wasRunning && svc) {
        StartServiceW(svc, 0, nullptr);
    }
    if (svc) CloseServiceHandle(svc);
    if (scm) CloseServiceHandle(scm);

    if (FAILED(regHr)) {
        return fail(L"DllRegisterServer falló (HRESULT " + FormatHrHex(regHr) + L").");
    }
    if (!IsComRegistered() || !IsInstalledDllUpToDate()) {
        return fail(L"El registro COM no pudo verificarse en HKLM.");
    }

    // Reglas automáticas de Windows Defender Firewall para ZeroCam Wi-Fi (rango de respaldo 5000-5010).
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
    // Detiene FrameServer para liberar el bloqueo sobre C:\ProgramData\ZeroCam\ZeroCamSource.dll.
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    SC_HANDLE svc = scm ? OpenServiceW(scm, L"FrameServer", SERVICE_QUERY_STATUS | SERVICE_STOP | SERVICE_START) : nullptr;
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

    // Elimina directamente la clave CLSID en HKLM sin cargar DLLs externas en un proceso elevado.
    const std::wstring clsidPath = std::wstring(L"SOFTWARE\\Classes\\CLSID\\") + kZeroCamClsidString;
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, clsidPath.c_str());

    RemoveFirewallRules();

    if (!IsPathReparsePoint(L"C:\\ProgramData\\ZeroCam")) {
        for (int tries = 0; tries < 10; ++tries) {
            if (!IsPathReparsePoint(L"C:\\ProgramData\\ZeroCam\\ZeroCamSource.dll")) {
                DeleteFileW(L"C:\\ProgramData\\ZeroCam\\ZeroCamSource.dll");
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

    if (wasRunning && svc) {
        StartServiceW(svc, 0, nullptr);
    }
    if (svc) CloseServiceHandle(svc);
    if (scm) CloseServiceHandle(scm);

    if (IsComRegistered()) {
        if (outError) *outError = L"No se pudo eliminar el registro COM de ZeroCam en HKLM.";
        return false;
    }
    if (outError) outError->clear();
    return true;
}

bool VirtualCameraHost::Start(std::wstring& outError) {
    Stop();

    if (!IsPlatformSupported()) {
        outError = L"La cámara virtual nativa requiere Windows 11 (compilación 22000 o superior).";
        return false;
    }
    if (!IsComRegistered()) {
        outError = L"Falta configurar la cámara virtual (acepta el permiso de administrador).";
        return false;
    }

    HRESULT hr = MFCreateVirtualCamera(
        MFVirtualCameraType_SoftwareCameraSource,
        MFVirtualCameraLifetime_Session,
        MFVirtualCameraAccess_CurrentUser,
        kZeroCamFriendlyName,
        kZeroCamClsidString,
        nullptr,
        0,
        &virtualCamera_);
    if (FAILED(hr)) {
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

    outError.clear();
    return true;
}

void VirtualCameraHost::Stop() {
    if (virtualCamera_) {
        virtualCamera_->Stop();
        virtualCamera_->Remove();
        virtualCamera_->Shutdown();
        virtualCamera_.Reset();
    }
}

} // namespace zerocam
