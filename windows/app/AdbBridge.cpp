#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include "AdbBridge.h"
#include <algorithm>
#include <cwchar>
#include <mutex>
#include <vector>

namespace zerocam {

namespace {

std::mutex g_adbJobMutex;
HANDLE g_adbJob = nullptr;

// Convierte una cadena UTF-8 (p. ej. serial ADB) a std::wstring.
std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed);
    return out;
}

// Verifica que el archivo exista como ejecutable regular y no sea un directorio ni reparse point.
bool IsExistingRegularFile(const std::wstring& path) noexcept {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
           (attrs & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

// Busca un ejecutable recorriendo exclusivamente directorios absolutos de %PATH% (nunca CWD).
std::wstring SearchAbsolutePathEnv(const wchar_t* exeName) {
    const DWORD needed = GetEnvironmentVariableW(L"PATH", nullptr, 0);
    if (needed == 0 || needed > 65536) return {};
    std::wstring pathEnv(needed, L'\0');
    const DWORD got = GetEnvironmentVariableW(L"PATH", pathEnv.data(), needed);
    if (got == 0) return {};
    pathEnv.resize(got);

    size_t start = 0;
    while (start <= pathEnv.size()) {
        size_t sep = pathEnv.find(L';', start);
        if (sep == std::wstring::npos) sep = pathEnv.size();
        std::wstring dir = pathEnv.substr(start, sep - start);
        start = sep + 1;

        while (!dir.empty() && (dir.front() == L' ' || dir.front() == L'"')) dir.erase(dir.begin());
        while (!dir.empty() && (dir.back() == L' ' || dir.back() == L'"')) dir.pop_back();
        if (dir.size() < 3 || dir[1] != L':' || (dir[2] != L'\\' && dir[2] != L'/')) {
            continue; // Ignora rutas relativas y "."
        }
        if (dir.back() != L'\\' && dir.back() != L'/') dir.push_back(L'\\');
        const std::wstring cand = dir + exeName;
        if (IsExistingRegularFile(cand)) {
            return cand;
        }
    }
    return {};
}

std::wstring NormalizePathForCompare(std::wstring p) {
    for (wchar_t& ch : p) {
        if (ch == L'/') ch = L'\\';
    }
    return p;
}

bool PathsEqualIgnoreCase(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty()) return false;
    return _wcsicmp(NormalizePathForCompare(a).c_str(), NormalizePathForCompare(b).c_str()) == 0;
}

std::wstring FindBundledAdbExecutable() {
    wchar_t exePath[MAX_PATH]{};
    const DWORD exeLen = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (exeLen > 0 && exeLen < MAX_PATH) {
        std::wstring dir(exePath);
        const size_t slash = dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            dir.resize(slash + 1);
            const std::wstring bundledSub = dir + L"adb\\adb.exe";
            if (IsExistingRegularFile(bundledSub)) {
                return bundledSub;
            }
            const std::wstring bundledRoot = dir + L"adb.exe";
            if (IsExistingRegularFile(bundledRoot)) {
                return bundledRoot;
            }
        }
    }
    return L"";
}

HANDLE EnsureAdbJobLocked() {
    if (g_adbJob != nullptr) {
        return g_adbJob;
    }
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return nullptr;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info))) {
        CloseHandle(job);
        return nullptr;
    }
    g_adbJob = job;
    return g_adbJob;
}

bool InspectOrTerminateByExactPath(const std::wstring& targetExePath, bool terminateAll) {
    if (targetExePath.empty()) return false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;

    bool found = false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"adb.exe") != 0) {
                continue;
            }
            const DWORD access = terminateAll
                ? (PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE | SYNCHRONIZE)
                : PROCESS_QUERY_LIMITED_INFORMATION;
            HANDLE proc = OpenProcess(access, FALSE, pe.th32ProcessID);
            if (!proc) continue;

            wchar_t imagePath[MAX_PATH * 2]{};
            DWORD imageLen = static_cast<DWORD>(sizeof(imagePath) / sizeof(imagePath[0]));
            const BOOL gotName = QueryFullProcessImageNameW(proc, 0, imagePath, &imageLen);
            if (gotName && imageLen > 0 && PathsEqualIgnoreCase(std::wstring(imagePath, imageLen), targetExePath)) {
                found = true;
                if (terminateAll) {
                    TerminateProcess(proc, 0);
                    WaitForSingleObject(proc, 1000);
                }
            }
            CloseHandle(proc);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

} // namespace

bool AdbBridge::IsValidAdbSerial(const std::string& serial) noexcept {
    if (serial.empty() || serial.size() > 128u || serial[0] == '-') {
        return false;
    }
    for (unsigned char ch : serial) {
        const bool ok = (ch >= 'A' && ch <= 'Z') ||
                        (ch >= 'a' && ch <= 'z') ||
                        (ch >= '0' && ch <= '9') ||
                        ch == '.' || ch == '_' || ch == ':' || ch == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

// Localiza adb.exe priorizando la carpeta portable junto a ZeroCam.exe sin consultar jamás el CWD.
std::wstring AdbBridge::FindAdbExecutable() {
    const std::wstring bundled = FindBundledAdbExecutable();
    if (!bundled.empty()) {
        return bundled;
    }

    const std::wstring fromPath = SearchAbsolutePathEnv(L"adb.exe");
    if (!fromPath.empty()) {
        return fromPath;
    }

    wchar_t localApp[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localApp, MAX_PATH) > 0) {
        std::wstring sdkAdb = std::wstring(localApp) + L"\\Android\\Sdk\\platform-tools\\adb.exe";
        if (IsExistingRegularFile(sdkAdb)) {
            return sdkAdb;
        }
    }
    return L"";
}

bool AdbBridge::RunProcessCapture(
    const std::wstring& exePath,
    const std::wstring& args,
    std::string& outStdout,
    unsigned long& outExitCode,
    uint64_t timeoutMs,
    const std::atomic<bool>* cancelFlag,
    bool attachToAdbJob) {
    outStdout.clear();
    outExitCode = 1;
    if (exePath.empty()) return false;

    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE rPipe = nullptr, wPipe = nullptr;
    if (!CreatePipe(&rPipe, &wPipe, &sa, 0)) return false;
    SetHandleInformation(rPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.StartupInfo.hStdOutput = wPipe;
    si.StartupInfo.hStdError = wPipe;
    si.StartupInfo.wShowWindow = SW_HIDE;

    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<BYTE> attrBuf(attrSize);
    auto* attrList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    if (attrSize == 0 || !InitializeProcThreadAttributeList(attrList, 1, 0, &attrSize)) {
        CloseHandle(wPipe);
        CloseHandle(rPipe);
        return false;
    }

    HANDLE inheritHandles[1] = { wPipe };
    if (!UpdateProcThreadAttribute(
            attrList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inheritHandles, sizeof(inheritHandles), nullptr, nullptr)) {
        DeleteProcThreadAttributeList(attrList);
        CloseHandle(wPipe);
        CloseHandle(rPipe);
        return false;
    }
    si.lpAttributeList = attrList;

    PROCESS_INFORMATION pi{};
    const std::wstring cmdLine = L"\"" + exePath + L"\" " + args;
    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(L'\0');

    DWORD creationFlags = CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT;
    if (attachToAdbJob) {
        creationFlags |= CREATE_SUSPENDED;
    }

    // Usa lpApplicationName explícito y hereda exclusivamente wPipe para evitar hijacking o sockets zombies.
    const BOOL ok = CreateProcessW(
        exePath.c_str(), cmdBuf.data(), nullptr, nullptr, TRUE,
        creationFlags,
        nullptr, nullptr, &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attrList);
    CloseHandle(wPipe);
    if (!ok) {
        CloseHandle(rPipe);
        return false;
    }
    if (attachToAdbJob) {
        {
            std::lock_guard<std::mutex> lock(g_adbJobMutex);
            HANDLE job = EnsureAdbJobLocked();
            if (job) {
                (void)AssignProcessToJobObject(job, pi.hProcess);
            }
        }
        ResumeThread(pi.hThread);
    }

    // Lectura no bloqueante con PeekNamedPipe, verificación estricta de deadline por iteración y tope de 64 KB.
    const ULONGLONG effectiveTimeoutMs = std::clamp<ULONGLONG>(timeoutMs, 50ull, 30000ull);
    constexpr size_t kMaxStdoutBytes = 64u * 1024u;
    const ULONGLONG deadline = GetTickCount64() + effectiveTimeoutMs;
    bool timedOut = false;
    char chunk[1024];

    for (;;) {
        if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) {
            timedOut = true;
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 500);
            break;
        }
        if (GetTickCount64() >= deadline) {
            timedOut = true;
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 1000);
            break;
        }
        DWORD avail = 0;
        if (!PeekNamedPipe(rPipe, nullptr, 0, nullptr, &avail, nullptr)) {
            break;
        }
        if (avail > 0) {
            DWORD got = 0;
            const DWORD toRead = std::min<DWORD>(avail, static_cast<DWORD>(sizeof(chunk)));
            if (!ReadFile(rPipe, chunk, toRead, &got, nullptr) || got == 0) {
                break;
            }
            if (outStdout.size() < kMaxStdoutBytes) {
                const size_t take = std::min<size_t>(got, kMaxStdoutBytes - outStdout.size());
                outStdout.append(chunk, take);
            }
            continue;
        }
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
            DWORD tailAvail = 0;
            while (GetTickCount64() < deadline &&
                   PeekNamedPipe(rPipe, nullptr, 0, nullptr, &tailAvail, nullptr) &&
                   tailAvail > 0) {
                DWORD got = 0;
                const DWORD toRead = std::min<DWORD>(tailAvail, static_cast<DWORD>(sizeof(chunk)));
                if (!ReadFile(rPipe, chunk, toRead, &got, nullptr) || got == 0) break;
                if (outStdout.size() < kMaxStdoutBytes) {
                    const size_t take = std::min<size_t>(got, kMaxStdoutBytes - outStdout.size());
                    outStdout.append(chunk, take);
                }
            }
            break;
        }
        Sleep(15);
    }
    CloseHandle(rPipe);

    if (!timedOut) {
        const ULONGLONG now = GetTickCount64();
        const DWORD rem = (now < deadline) ? static_cast<DWORD>(deadline - now) : 0u;
        if (WaitForSingleObject(pi.hProcess, rem) == WAIT_TIMEOUT) {
            timedOut = true;
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 1000);
        }
    }
    DWORD rawCode = 1;
    GetExitCodeProcess(pi.hProcess, &rawCode);
    outExitCode = rawCode;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return !timedOut;
}

#ifdef ZEROCAM_ALLOW_IPC_ENV_OVERRIDE
bool AdbBridge::RunProcessCaptureForTest(
    const std::wstring& exePath,
    const std::wstring& args,
    uint64_t timeoutMs,
    std::string& outStdout,
    unsigned long& outExitCode) {
    return RunProcessCapture(exePath, args, outStdout, outExitCode, timeoutMs, nullptr, false);
}

void* AdbBridge::SpawnInAdbJobForTest(const std::wstring& exePath, const std::wstring& args) {
    if (exePath.empty()) return nullptr;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    const std::wstring cmdLine = args.empty()
        ? (L"\"" + exePath + L"\"")
        : (L"\"" + exePath + L"\" " + args);
    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(L'\0');
    const BOOL ok = CreateProcessW(
        exePath.c_str(), cmdBuf.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED,
        nullptr, nullptr, &si, &pi);
    if (!ok) return nullptr;
    bool assigned = false;
    {
        std::lock_guard<std::mutex> lock(g_adbJobMutex);
        HANDLE job = EnsureAdbJobLocked();
        if (job) {
            assigned = (AssignProcessToJobObject(job, pi.hProcess) != FALSE);
        }
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    if (!assigned) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        return nullptr;
    }
    return pi.hProcess;
}
#endif

void AdbBridge::ShutdownBundledAdb() {
    const std::wstring bundledAdb = FindBundledAdbExecutable();
    if (!bundledAdb.empty() && InspectOrTerminateByExactPath(bundledAdb, false)) {
        std::string out;
        unsigned long code = 0;
        (void)RunProcessCapture(bundledAdb, L"kill-server", out, code, 2500ull, nullptr, false);
    }

    {
        std::lock_guard<std::mutex> lock(g_adbJobMutex);
        if (g_adbJob != nullptr) {
            CloseHandle(g_adbJob);
            g_adbJob = nullptr;
        }
    }

    if (!bundledAdb.empty() && InspectOrTerminateByExactPath(bundledAdb, false)) {
        (void)InspectOrTerminateByExactPath(bundledAdb, true);
    }
}

// Detecta el primer dispositivo Android autorizado, configura el túnel inverso ADB y opcionalmente despierta la app móvil.
bool AdbBridge::SetupUsbTunnel(
    uint16_t mobilePort,
    uint16_t pcPort,
    std::wstring& outMessage,
    bool wakeMobileApp,
    const std::atomic<bool>* cancelFlag) {
    if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) return false;
    const std::wstring adb = FindAdbExecutable();
    if (adb.empty()) {
        outMessage = L"No se encontró adb.exe en la carpeta adb/ junto a ZeroCam.exe.";
        return false;
    }
    const std::wstring bundledAdb = FindBundledAdbExecutable();
    const bool attachJob = !bundledAdb.empty() && PathsEqualIgnoreCase(adb, bundledAdb);

    std::string out;
    unsigned long code = 0;
    if (!RunProcessCapture(adb, L"devices", out, code, 12000ull, cancelFlag, attachJob)) {
        outMessage = L"No se pudo ejecutar adb.exe";
        return false;
    }

    std::string selectedSerial;
    bool hasUnauthorized = false;
    bool hasOffline = false;
    size_t pos = 0;
    while (pos < out.size()) {
        size_t nl = out.find('\n', pos);
        if (nl == std::string::npos) nl = out.size();
        std::string line = out.substr(pos, nl - pos);
        pos = nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
            line.pop_back();
        }
        if (line.empty() || line.rfind("List of devices", 0) == 0) {
            continue;
        }
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) {
            continue;
        }
        const std::string serial = line.substr(0, tab);
        const std::string state = line.substr(tab + 1);
        if (state.rfind("device", 0) == 0 && selectedSerial.empty() && IsValidAdbSerial(serial)) {
            selectedSerial = serial;
        } else if (state.find("unauthorized") != std::string::npos) {
            hasUnauthorized = true;
        } else if (state.find("offline") != std::string::npos) {
            hasOffline = true;
        }
    }

    if (selectedSerial.empty()) {
        if (hasUnauthorized) {
            outMessage = L"Acepta 'Permitir depuración USB' en la pantalla de tu celular y vuelve a intentarlo.";
        } else if (hasOffline) {
            outMessage = L"Celular en estado offline: desconecta y reconecta el cable USB.";
        } else {
            outMessage = L"Conecta tu Android por USB con 'Depuración USB' activada.";
        }
        return false;
    }

    const std::wstring serialW = Utf8ToWide(selectedSerial);
    const std::wstring mPortStr = std::to_wstring(mobilePort);
    const std::wstring pcPortStr = std::to_wstring(pcPort);
    const std::wstring targetPrefix = L"-s \"" + serialW + L"\" ";
    const std::wstring rmFwdArgs = targetPrefix + L"forward --remove tcp:" + mPortStr;
    const std::wstring revArgs = targetPrefix + L"reverse tcp:" + mPortStr + L" tcp:" + pcPortStr;

    if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) return false;
    // Limpia cualquier forward residual que intercepte 127.0.0.1:puerto en PC y cause bucle TCP.
    RunProcessCapture(adb, rmFwdArgs, out, code, 5000ull, cancelFlag, attachJob);
    if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) return false;
    if (!RunProcessCapture(adb, revArgs, out, code, 8000ull, cancelFlag, attachJob) || code != 0) {
        outMessage = L"Error ejecutando adb reverse tcp:" + mPortStr;
        return false;
    }
    if (wakeMobileApp) {
        if (cancelFlag && cancelFlag->load(std::memory_order_acquire)) return false;
        std::string wakeOut;
        DWORD wakeCode = 0;
        const std::wstring wakeArgs =
            targetPrefix + L"shell am start --activity-single-top -n com.zerocam.mobile/.MainActivity --ez auto_start true";
        (void)RunProcessCapture(adb, wakeArgs, wakeOut, wakeCode, 2500ull, cancelFlag, attachJob);
    }
    outMessage = L"Túnel USB ADB activo en segundo plano.";
    return true;
}

// Sobrecarga cuando ambos extremos usan el mismo puerto TCP.
bool AdbBridge::SetupUsbTunnel(uint16_t port, std::wstring& outMessage) {
    return SetupUsbTunnel(port, port, outMessage);
}

} // namespace zerocam
