#include "StreamIngest.h"
#include "AdbBridge.h"
#include "../shared/WireCrypto.h"
#include "../shared/ZeroCamProtocol.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace zerocam {

namespace {

constexpr DWORD kSioUdpConnReset = IOC_IN | IOC_VENDOR | 12;
constexpr ULONGLONG kAuthRateWindowMs = 5ull * 60ull * 1000ull; // 5 minutos
constexpr int kAuthRateMaxFailures = 5;

// Desactiva WSAECONNRESET en sockets UDP ante mensajes ICMP Port Unreachable en Windows.
void DisableUdpConnReset(SOCKET s) noexcept {
    BOOL newBehavior = FALSE;
    DWORD bytesReturned = 0;
    WSAIoctl(s, kSioUdpConnReset, &newBehavior, sizeof(newBehavior), nullptr, 0, &bytesReturned, nullptr, nullptr);
}

// Lee exactamente len bytes desde un socket TCP respetando el flag de parada.
bool RecvExact(SOCKET s, void* dst, size_t len, const std::atomic<bool>& stopFlag) {
    uint8_t* ptr = static_cast<uint8_t*>(dst);
    size_t got = 0;
    while (got < len && !stopFlag.load(std::memory_order_acquire)) {
        const int chunk = static_cast<int>(std::min<size_t>(len - got, 65536));
        const int r = recv(s, reinterpret_cast<char*>(ptr + got), chunk, 0);
        if (r <= 0) {
            return false;
        }
        got += static_cast<size_t>(r);
    }
    return got == len;
}

// Envía exactamente len bytes por un socket TCP.
bool SendExact(SOCKET s, const uint8_t* src, size_t len) noexcept {
    if (s == INVALID_SOCKET) return false;
    size_t sent = 0;
    while (sent < len) {
        const int chunk = static_cast<int>(std::min<size_t>(len - sent, 65536));
        const int r = send(s, reinterpret_cast<const char*>(src + sent), chunk, 0);
        if (r <= 0) return false;
        sent += static_cast<size_t>(r);
    }
    return true;
}

// Envía un paquete de handshake Wire v2 en claro (cabecera 20 B + payload).
bool SendWireV2PlainPacket(SOCKET s, WirePacketType type, uint16_t flags, const uint8_t* payload, uint32_t payloadSize) {
    WirePacketHeader hdr{};
    hdr.magic = kWireMagic;
    hdr.version = kWireVersion;
    hdr.type = static_cast<uint8_t>(type);
    hdr.flags = flags;
    hdr.ptsUs = 0;
    hdr.payloadSize = payloadSize;

    std::vector<uint8_t> pkt(sizeof(hdr) + payloadSize);
    std::memcpy(pkt.data(), &hdr, sizeof(hdr));
    if (payloadSize > 0 && payload) {
        std::memcpy(pkt.data() + sizeof(hdr), payload, payloadSize);
    }
    return SendExact(s, pkt.data(), pkt.size());
}

// Envía un paquete HandshakeError de 1 byte y cierra el canal de escritura.
void SendHandshakeError(SOCKET s, uint8_t errCode) {
    SendWireV2PlainPacket(s, WirePacketType::HandshakeError, 0, &errCode, 1u);
    shutdown(s, SD_SEND);
}

// Parsea un número de puerto válido con valor por defecto si el texto es inválido.
uint16_t ParsePortOrDefault(const std::wstring& text, uint16_t fallback) {
    if (text.empty()) return fallback;
    wchar_t* end = nullptr;
    unsigned long val = wcstoul(text.c_str(), &end, 10);
    if (end == text.c_str() || val == 0 || val > 65535) return fallback;
    return static_cast<uint16_t>(val);
}

inline bool IsHexCharW(wchar_t c) noexcept {
    return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
}

inline bool IsHexCharA(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// Verifica que un archivo exista como regular sin reparse points.
bool IsExistingRegularExe(const std::wstring& path) noexcept {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
           (attrs & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

// Localiza ffmpeg.exe buscando junto a ZeroCam.exe y en directorios absolutos de %PATH% (nunca CWD).
std::wstring FindFfmpegExecutable() {
    wchar_t exePath[MAX_PATH]{};
    const DWORD exeLen = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (exeLen > 0 && exeLen < MAX_PATH) {
        std::wstring dir(exePath, exeLen);
        const size_t slash = dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            dir.resize(slash + 1);
            const std::wstring local1 = dir + L"ffmpeg.exe";
            if (IsExistingRegularExe(local1)) return local1;
            const std::wstring local2 = dir + L"ffmpeg\\ffmpeg.exe";
            if (IsExistingRegularExe(local2)) return local2;
        }
    }

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
        if (dir.size() < 3 || dir[1] != L':' || (dir[2] != L'\\' && dir[2] != L'/')) continue;
        if (dir.back() != L'\\' && dir.back() != L'/') dir.push_back(L'\\');
        const std::wstring cand = dir + L"ffmpeg.exe";
        if (IsExistingRegularExe(cand)) return cand;
    }
    return {};
}

// Crea un socket IPv4 no heredable por subprocesos (adb/ffmpeg) para evitar puertos bloqueados.
SOCKET CreateNonInheritableSocket(int type, int protocol) {
    return WSASocketW(AF_INET, type, protocol, nullptr, 0, WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
}

// Consulta la IP LAN/Wi-Fi local sin enviar paquetes externos para mostrarla al usuario.
std::wstring DetectLocalLanIp() {
    SOCKET s = CreateNonInheritableSocket(SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return L"";

    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(53);
    InetPtonW(AF_INET, L"8.8.8.8", &remote.sin_addr);
    connect(s, reinterpret_cast<const sockaddr*>(&remote), sizeof(remote));

    sockaddr_in local{};
    int localLen = sizeof(local);
    std::wstring ipStr;
    if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &localLen) == 0) {
        wchar_t buf[INET_ADDRSTRLEN]{};
        if (InetNtopW(AF_INET, &local.sin_addr, buf, INET_ADDRSTRLEN) != nullptr &&
            (ntohl(local.sin_addr.s_addr) >> 24) != 127u) {
            ipStr = buf;
        }
    }
    closesocket(s);
    return ipStr;
}

// Obtiene el nombre del equipo Windows en ASCII seguro para la respuesta de discovery UDP v2.
std::string GetLocalPcHostName() {
    char nameBuf[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD sz = MAX_COMPUTERNAME_LENGTH + 1;
    if (!GetComputerNameA(nameBuf, &sz) || sz == 0) {
        return "ZeroCam-PC";
    }
    std::string out(nameBuf, sz);
    for (char& ch : out) {
        if (ch == '|' || ch == '\r' || ch == '\n' || static_cast<unsigned char>(ch) < 0x20u || static_cast<unsigned char>(ch) >= 0x7Fu) {
            ch = '-';
        }
    }
    return out;
}

// Parsea una solicitud "ZCAM_DISCOVER_V2|NONCE=<16..32 hex>" validando estrictamente el nonce.
bool ParseDiscoveryV2Request(const char* msg, size_t len, std::string& outNonce) {
    outNonce.clear();
    const std::string s(msg, len);
    static constexpr const char* kPrefix = "ZCAM_DISCOVER_V2|NONCE=";
    constexpr size_t kPrefixLen = 23u;
    if (s.size() < kPrefixLen + 16u || s.compare(0, kPrefixLen, kPrefix) != 0) {
        return false;
    }
    std::string nonce = s.substr(kPrefixLen);
    while (!nonce.empty() && (nonce.back() == '\r' || nonce.back() == '\n' || nonce.back() == ' ')) {
        nonce.pop_back();
    }
    if (nonce.size() < 16u || nonce.size() > 32u || (nonce.size() & 1u) != 0u) {
        return false;
    }
    for (char& ch : nonce) {
        if (!IsHexCharA(ch)) return false;
        if (ch >= 'a' && ch <= 'f') ch = static_cast<char>(ch - 'a' + 'A');
    }
    outNonce = std::move(nonce);
    return true;
}

// Guardia RAII genérica para asegurar liberación de recursos al salir de un ámbito.
template <typename Fn>
struct ScopeExit {
    Fn fn;
    ~ScopeExit() { fn(); }
};

template <typename Fn>
ScopeExit<Fn> MakeScopeExit(Fn fn) {
    return ScopeExit<Fn>{ std::move(fn) };
}

} // namespace

bool StreamIngest::IsAllowedMediaUrl(const std::wstring& url) noexcept {
    if (url.empty() || url.size() > 2048u) return false;

    size_t schemeEnd = 0;
    if (url.rfind(L"rtsp://", 0) == 0) schemeEnd = 7u;
    else if (url.rfind(L"rtsps://", 0) == 0) schemeEnd = 8u;
    else if (url.rfind(L"srt://", 0) == 0) schemeEnd = 6u;
    else if (url.rfind(L"udp://", 0) == 0) schemeEnd = 6u;
    else if (url.rfind(L"tcp://", 0) == 0) schemeEnd = 6u;
    else return false;

    if (schemeEnd >= url.size()) return false;
    const wchar_t firstHostChar = url[schemeEnd];
    if (firstHostChar == L'-' || firstHostChar == L'/' || firstHostChar == L'@' ||
        firstHostChar == L'?' || firstHostChar == L'#' || firstHostChar == L':') {
        return false;
    }

    // Valida caracteres permitidos por RFC 3986 sin comillas, espacios, redirecciones ni secuencias de control.
    for (size_t i = schemeEnd; i < url.size(); ++i) {
        const wchar_t c = url[i];
        if (c <= 0x20 || c >= 0x7F) return false;
        const bool ok = (c >= L'A' && c <= L'Z') ||
                        (c >= L'a' && c <= L'z') ||
                        (c >= L'0' && c <= L'9') ||
                        c == L'-' || c == L'.' || c == L'_' || c == L'~' ||
                        c == L':' || c == L'/' || c == L'?' || c == L'#' ||
                        c == L'[' || c == L']' || c == L'@' || c == L'!' ||
                        c == L'&' || c == L'+' || c == L',' || c == L'=' ||
                        c == L'%';
        if (!ok) return false;
        if (c == L'%') {
            if (i + 2u >= url.size() || !IsHexCharW(url[i + 1u]) || !IsHexCharW(url[i + 2u])) {
                return false;
            }
            const wchar_t h1 = url[i + 1u];
            const wchar_t h2 = url[i + 2u];
            if (h1 == L'0' && (h2 == L'0' || h2 == L'a' || h2 == L'A' || h2 == L'd' || h2 == L'D')) {
                return false;
            }
        }
    }

    // Bloquea travesías o sub-esquemas peligrosos de FFmpeg embebidos.
    if (url.find(L"..") != std::wstring::npos) return false;
    std::wstring lower = url.substr(schemeEnd);
    for (wchar_t& ch : lower) {
        if (ch >= L'A' && ch <= L'Z') ch = static_cast<wchar_t>(ch - L'A' + L'a');
    }
    if (lower.find(L"file:") != std::wstring::npos ||
        lower.find(L"concat:") != std::wstring::npos ||
        lower.find(L"subfile:") != std::wstring::npos ||
        lower.find(L"pipe:") != std::wstring::npos) {
        return false;
    }
    return true;
}

bool StreamIngest::EnsureHostIdentity() {
    std::lock_guard<std::mutex> lock(identityMutex_);
    if (hostIdentity_.IsValid()) return true;
    return hostIdentity_.LoadOrCreatePersistent();
}

std::wstring StreamIngest::HostFingerprintDisplay() {
    if (!EnsureHostIdentity()) return L"----";
    std::lock_guard<std::mutex> lock(identityMutex_);
    return hostIdentity_.FingerprintDisplay();
}

std::string StreamIngest::HostFingerprintHex() {
    if (!EnsureHostIdentity()) return {};
    std::lock_guard<std::mutex> lock(identityMutex_);
    return hostIdentity_.FingerprintHex();
}

bool StreamIngest::IsPeerRateLimited(uint32_t peerIpKey) {
    const ULONGLONG now = GetTickCount64();
    std::lock_guard<std::mutex> lock(authRateMutex_);
    const auto it = authRateMap_.find(peerIpKey);
    if (it == authRateMap_.end()) return false;
    if (it->second.blockedUntilMs > now) {
        return true;
    }
    if (it->second.blockedUntilMs != 0 && it->second.blockedUntilMs <= now) {
        authRateMap_.erase(it);
    }
    return false;
}

bool StreamIngest::RecordAuthFailure(uint32_t peerIpKey) {
    const ULONGLONG now = GetTickCount64();
    std::lock_guard<std::mutex> lock(authRateMutex_);
    if (authRateMap_.size() > 1024u) {
        for (auto it = authRateMap_.begin(); it != authRateMap_.end();) {
            const bool expired = (it->second.blockedUntilMs <= now) &&
                                 (now - it->second.windowStartMs > kAuthRateWindowMs);
            it = expired ? authRateMap_.erase(it) : std::next(it);
        }
    }
    auto& entry = authRateMap_[peerIpKey];
    if (entry.windowStartMs == 0 || (now - entry.windowStartMs) > kAuthRateWindowMs) {
        entry.failures = 0;
        entry.windowStartMs = now;
        entry.blockedUntilMs = 0;
    }
    ++entry.failures;
    if (entry.failures >= kAuthRateMaxFailures) {
        entry.blockedUntilMs = now + kAuthRateWindowMs;
        return true;
    }
    return false;
}

void StreamIngest::ClearAuthFailures(uint32_t peerIpKey) {
    std::lock_guard<std::mutex> lock(authRateMutex_);
    authRateMap_.erase(peerIpKey);
}

StreamIngest::~StreamIngest() {
    Stop();
}

bool StreamIngest::Start(SourceMode mode, const std::wstring& endpoint, D3D11Context* d3dCtx, PreviewRenderer* preview) {
    Stop();
    if (!d3dCtx) return false;

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        SetStatus(L"Error inicializando Winsock");
        return false;
    }

    d3dCtx_ = d3dCtx;
    preview_ = preview;
    stopRequested_.store(false, std::memory_order_release);
    clientConnected_.store(false, std::memory_order_release);
    clientIsLoopback_.store(false, std::memory_order_release);
    hwDecodeActive_.store(false, std::memory_order_release);
    totalFrames_.store(0, std::memory_order_release);
    currentFps_.store(0.0, std::memory_order_release);
    fpsWindowStartMs_ = GetTickCount64();
    fpsWindowFrames_ = 0;

    const uint16_t basePort = ParsePortOrDefault(endpoint, kDefaultStreamPort);
    boundPort_.store(basePort, std::memory_order_release);
    portBoundReady_.store(false, std::memory_order_release);
    usbWakeRequested_.store(false, std::memory_order_release);
    SetConnectionLabel(L"Sin conexión");

    running_.store(true, std::memory_order_release);
    workerThread_ = std::thread(&StreamIngest::WorkerMain, this, mode, endpoint);
    // Discovery UDP (:5001) y monitor ADB permanecen activos para salto USB <-> Wi-Fi y prioridad por cable.
    if (mode == SourceMode::ZeroCamWifi || mode == SourceMode::ZeroCamUsb) {
        discoveryThread_ = std::thread(&StreamIngest::DiscoveryMain, this, basePort);
        usbMonitorThread_ = std::thread(&StreamIngest::UsbMonitorMain, this, basePort);
    }
    return true;
}

void StreamIngest::Stop() {
    bool sentRemoteStop = false;
    if (clientConnected_.load(std::memory_order_acquire)) {
        sentRemoteStop = SendControlJson("{\"cmd\":\"stop_stream\"}");
    }
    if (sentRemoteStop) {
        {
            std::lock_guard<std::mutex> lock(socketMutex_);
            // Cierra solo escritura para entregar stop_stream + FIN sin emitir TCP RST mientras llegan frames.
            if (activeClientSocket_ != INVALID_SOCKET) {
                shutdown(activeClientSocket_, SD_SEND);
            }
        }
        for (int i = 0; i < 15 && clientConnected_.load(std::memory_order_acquire); ++i) {
            Sleep(10);
        }
    }
    stopRequested_.store(true, std::memory_order_release);
    portBoundReady_.store(true, std::memory_order_release);
    usbWakeRequested_.store(true, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lock(socketMutex_);
        // Solo shutdown() sobre activeClientSocket_ para destrabar recv/send sin cerrar el handle mientras sendMutex_ lo usa.
        if (activeClientSocket_ != INVALID_SOCKET) {
            shutdown(activeClientSocket_, SD_BOTH);
        }
        if (listenSocket_ != INVALID_SOCKET) {
            closesocket(listenSocket_);
            listenSocket_ = INVALID_SOCKET;
        }
        if (udpSocket_ != INVALID_SOCKET) {
            closesocket(udpSocket_);
            udpSocket_ = INVALID_SOCKET;
        }
        if (discoverySocket_ != INVALID_SOCKET) {
            closesocket(discoverySocket_);
            discoverySocket_ = INVALID_SOCKET;
        }
        if (ffmpegProcess_) {
            TerminateProcess(ffmpegProcess_, 0);
            CloseHandle(ffmpegProcess_);
            ffmpegProcess_ = nullptr;
        }
    }

    if (workerThread_.joinable()) workerThread_.join();
    if (discoveryThread_.joinable()) discoveryThread_.join();
    if (usbMonitorThread_.joinable()) usbMonitorThread_.join();

    {
        std::lock_guard<std::mutex> sendLock(sendMutex_);
        sendCipher_.Reset();
        sessionEncrypted_ = false;
        std::lock_guard<std::mutex> sockLock(socketMutex_);
        if (activeClientSocket_ != INVALID_SOCKET) {
            closesocket(activeClientSocket_);
            activeClientSocket_ = INVALID_SOCKET;
        }
    }

    if (running_.exchange(false, std::memory_order_acq_rel)) {
        WSACleanup();
    }
    clientConnected_.store(false, std::memory_order_release);
    clientIsLoopback_.store(false, std::memory_order_release);
    SetConnectionLabel(L"Sin conexión");
    SetStatus(L"Detenido");
}

bool StreamIngest::SendControlJson(const std::string& json) {
    if (json.size() > kMaxWirePayloadBytes) return false;

    // Cifrado AES-256-GCM + incremento monotónico de seq64 + escritura al socket atómicos bajo sendMutex_.
    std::lock_guard<std::mutex> sendLock(sendMutex_);
    if (!sessionEncrypted_ || !sendCipher_.IsReady() || activeClientSocket_ == INVALID_SOCKET) {
        return false;
    }

    WirePacketHeader hdr{};
    hdr.magic = kWireMagic;
    hdr.version = kWireVersion;
    hdr.type = static_cast<uint8_t>(WirePacketType::ControlJson);
    hdr.flags = 0;
    hdr.ptsUs = 0;
    hdr.payloadSize = static_cast<uint32_t>(json.size() + kWireAeadTagBytes);

    std::vector<uint8_t> ctAndTag;
    if (!sendCipher_.EncryptNext(
            reinterpret_cast<const uint8_t*>(&hdr),
            sizeof(hdr),
            reinterpret_cast<const uint8_t*>(json.data()),
            json.size(),
            ctAndTag)) {
        return false;
    }

    std::vector<uint8_t> pkt(sizeof(hdr) + ctAndTag.size());
    std::memcpy(pkt.data(), &hdr, sizeof(hdr));
    std::memcpy(pkt.data() + sizeof(hdr), ctAndTag.data(), ctAndTag.size());

    if (!SendExact(activeClientSocket_, pkt.data(), pkt.size())) {
        sessionEncrypted_ = false;
        shutdown(activeClientSocket_, SD_BOTH);
        return false;
    }
    return true;
}

std::string StreamIngest::BuildSetVideoJson(const VideoConfig& cfg) {
    return "{\"cmd\":\"set_video\",\"width\":" + std::to_string(cfg.width) +
           ",\"height\":" + std::to_string(cfg.height) +
           ",\"fps\":" + std::to_string(cfg.fps) +
           ",\"bitrate\":" + std::to_string(cfg.bitrateBps) + "}";
}

std::string StreamIngest::BuildCameraControlJson(const CameraRemoteConfig& cfg) {
    char buf[256]{};
    const char* fMode = (cfg.focusMode == 1) ? "lock" : "face";
    std::snprintf(
        buf,
        sizeof(buf),
        "{\"cmd\":\"cam_ctrl\",\"front\":%d,\"torch\":%d,\"zoom\":%.2f,\"ev\":%d,\"prefer_fps\":%d,\"focus_mode\":\"%s\"}",
        cfg.frontCamera ? 1 : 0,
        cfg.torchEnabled ? 1 : 0,
        std::clamp(cfg.lensZoom, 1.0f, 3.0f),
        std::clamp(cfg.exposureEv, -2, 2),
        cfg.preferFps ? 1 : 0,
        fMode);
    return std::string(buf);
}

bool StreamIngest::SetCameraControl(const CameraRemoteConfig& cfg, bool pushIfConnected) {
    CameraRemoteConfig safe = cfg;
    safe.focusMode = (cfg.focusMode == 1) ? 1 : 0;
    safe.lensZoom = std::clamp(cfg.lensZoom, 1.0f, 3.0f);
    safe.exposureEv = std::clamp(cfg.exposureEv, -2, 2);

    {
        std::lock_guard<std::mutex> lock(camCtrlMutex_);
        camCtrl_ = safe;
        camCtrlUserSet_ = true;
    }

    if (pushIfConnected && clientConnected_.load(std::memory_order_acquire)) {
        return SendControlJson(BuildCameraControlJson(safe));
    }
    return false;
}

bool StreamIngest::SendFocusPoint(float normX, float normY) {
    if (!clientConnected_.load(std::memory_order_acquire)) return false;
    const float nx = std::clamp(normX, 0.02f, 0.98f);
    const float ny = std::clamp(normY, 0.02f, 0.98f);
    char buf[128]{};
    std::snprintf(buf, sizeof(buf), "{\"cmd\":\"focus\",\"mode\":\"point\",\"x\":%.3f,\"y\":%.3f}", nx, ny);
    return SendControlJson(std::string(buf));
}

bool StreamIngest::SetVideoConfig(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrateBps, bool pushIfConnected) {
    VideoConfig cfg{};
    cfg.width = std::clamp(width, 320u, 3840u) & ~1u;
    cfg.height = std::clamp(height, 240u, 2160u) & ~1u;
    cfg.fps = std::clamp(fps, 15u, 60u);
    cfg.bitrateBps = std::clamp(bitrateBps, 500000u, 25000000u);

    {
        std::lock_guard<std::mutex> lock(videoCfgMutex_);
        videoCfg_ = cfg;
        videoCfgUserSet_ = true;
    }
    targetFps_.store(cfg.fps, std::memory_order_release);

    if (pushIfConnected && clientConnected_.load(std::memory_order_acquire)) {
        return SendControlJson(BuildSetVideoJson(cfg));
    }
    return false;
}

VideoConfig StreamIngest::GetVideoConfig() {
    std::lock_guard<std::mutex> lock(videoCfgMutex_);
    return videoCfg_;
}

TelemetrySnapshot StreamIngest::GetTelemetry() {
    TelemetrySnapshot snap;
    snap.running = running_.load(std::memory_order_acquire);
    snap.clientConnected = clientConnected_.load(std::memory_order_acquire);
    snap.hwDecodeActive = hwDecodeActive_.load(std::memory_order_acquire);
    snap.width = curWidth_.load(std::memory_order_acquire);
    snap.height = curHeight_.load(std::memory_order_acquire);
    snap.fps = currentFps_.load(std::memory_order_acquire);
    snap.totalFrames = totalFrames_.load(std::memory_order_acquire);
    if (d3dCtx_ && d3dCtx_->Ipc()) {
        snap.consumerCount = d3dCtx_->Ipc()->consumerCount.load(std::memory_order_acquire);
        snap.zeroCopyActive = (d3dCtx_->Ipc()->clientNeedsCpuFallback.load(std::memory_order_acquire) == 0);
    }
    snap.rotationDeg = activeRotationDeg_.load(std::memory_order_acquire);
    snap.rotationIsAuto = (rotationOverride_.load(std::memory_order_acquire) < 0);
    std::lock_guard<std::mutex> lock(statusMutex_);
    snap.connectionLabel = connectionLabel_;
    snap.statusText = statusText_;
    return snap;
}

uint8_t StreamIngest::ResolveRotationCode(uint8_t packetRotCode) noexcept {
    const int ov = rotationOverride_.load(std::memory_order_acquire);
    const uint8_t code = static_cast<uint8_t>((ov >= 0) ? (ov & 0x3) : (packetRotCode & 0x3));
    activeRotationDeg_.store(static_cast<int>(code) * 90, std::memory_order_release);
    return code;
}

void StreamIngest::SetStatus(const std::wstring& text) {
    std::lock_guard<std::mutex> lock(statusMutex_);
    statusText_ = text;
}

void StreamIngest::SetConnectionLabel(const std::wstring& label) {
    std::lock_guard<std::mutex> lock(statusMutex_);
    connectionLabel_ = label;
}

void StreamIngest::RecordFrame(uint32_t w, uint32_t h, bool hwDecode) {
    curWidth_.store(w, std::memory_order_release);
    curHeight_.store(h, std::memory_order_release);
    hwDecodeActive_.store(hwDecode, std::memory_order_release);
    totalFrames_.fetch_add(1, std::memory_order_relaxed);
    ++fpsWindowFrames_;

    const ULONGLONG now = GetTickCount64();
    const ULONGLONG elapsed = now - fpsWindowStartMs_;
    if (elapsed >= 500) {
        currentFps_.store((static_cast<double>(fpsWindowFrames_) * 1000.0) / static_cast<double>(elapsed), std::memory_order_release);
        fpsWindowFrames_ = 0;
        fpsWindowStartMs_ = now;
    }
}

void StreamIngest::WorkerMain(SourceMode mode, std::wstring endpoint) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    switch (mode) {
    case SourceMode::ZeroCamUsb:
        RunTcpServer(ParsePortOrDefault(endpoint, kDefaultStreamPort), true);
        break;
    case SourceMode::ZeroCamWifi:
        RunTcpServer(ParsePortOrDefault(endpoint, kDefaultStreamPort), false);
        break;
    case SourceMode::UdpH264:
        RunUdpReceiver(ParsePortOrDefault(endpoint, kDefaultStreamPort));
        break;
    case SourceMode::RtspSrtRemux:
        RunRtspSrtRemux(endpoint);
        break;
    case SourceMode::TestPattern:
        RunTestPattern();
        break;
    }
    CoUninitialize();
}

void StreamIngest::SetWifiPin(const std::string& pin) {
    std::lock_guard<std::mutex> lock(pinMutex_);
    wifiPin_ = pin;
}

std::string StreamIngest::GetWifiPin() {
    std::lock_guard<std::mutex> lock(pinMutex_);
    return wifiPin_;
}

// Mantiene el túnel ADB en segundo plano mientras no exista conexión USB activa.
void StreamIngest::UsbMonitorMain(uint16_t mobilePort) {
    while (!stopRequested_.load(std::memory_order_acquire) &&
           !portBoundReady_.load(std::memory_order_acquire)) {
        Sleep(20);
    }

    bool firstRun = true;
    int usbRetryBurst = 0;
    ULONGLONG lastWifiProbeMs = 0;
    while (!stopRequested_.load(std::memory_order_acquire)) {
        const bool connectedNow = clientConnected_.load(std::memory_order_acquire);
        const bool connectedUsb = connectedNow && clientIsLoopback_.load(std::memory_order_acquire);
        if (connectedUsb) {
            usbRetryBurst = 0;
            Sleep(250);
            continue;
        }

        const bool wakeTriggered = usbWakeRequested_.exchange(false, std::memory_order_acq_rel);
        const ULONGLONG nowMs = GetTickCount64();
        const bool periodicWifiProbe = connectedNow && (lastWifiProbeMs == 0 || (nowMs - lastWifiProbeMs) >= 5000ull);
        const bool shouldProbe = firstRun || wakeTriggered || (usbRetryBurst > 0) || !connectedNow || periodicWifiProbe;

        if (shouldProbe) {
            const bool shouldWakeApp = firstRun && !connectedNow;
            firstRun = false;
            const uint16_t pcPort = boundPort_.load(std::memory_order_acquire);
            std::wstring adbMsg;
            // Prioriza Cable USB tanto en arranque como al reconectar cable estando en Wi-Fi.
            const bool usbReady = AdbBridge::SetupUsbTunnel(
                mobilePort, pcPort, adbMsg, shouldWakeApp, &stopRequested_);
            lastWifiProbeMs = GetTickCount64();
            if (usbReady) {
                usbRetryBurst = 0;
                // Evaluar EN VIVO tras SetupUsbTunnel por si Wi-Fi conectó mientras arrancaba adb.
                if (clientConnected_.load(std::memory_order_acquire) &&
                    !clientIsLoopback_.load(std::memory_order_acquire)) {
                    (void)SendControlJson("{\"cmd\":\"prefer_usb\"}");
                }
            } else if (wakeTriggered && connectedNow) {
                // WM_DEVICECHANGE suele llegar 1-2 s antes de que el driver USB ADB enumere el teléfono.
                usbRetryBurst = 2;
            } else if (usbRetryBurst > 0) {
                --usbRetryBurst;
            }
        }

        const bool inWifiNow = clientConnected_.load(std::memory_order_acquire) &&
                               !clientIsLoopback_.load(std::memory_order_acquire);
        const int waitTicks = (usbRetryBurst > 0) ? 15 : (inWifiNow ? 50 : 30);
        for (int i = 0; i < waitTicks && !stopRequested_.load(std::memory_order_acquire); ++i) {
            const bool nowConn = clientConnected_.load(std::memory_order_acquire);
            const bool nowUsb = nowConn && clientIsLoopback_.load(std::memory_order_acquire);
            if (nowUsb || usbWakeRequested_.load(std::memory_order_acquire) || (inWifiNow && !nowConn)) {
                break;
            }
            Sleep(100);
        }
    }
}

void StreamIngest::DiscoveryMain(uint16_t) {
    if (!EnsureHostIdentity()) return;

    // Espera brevemente a que RunTcpServer enlace el puerto definitivo (5000..5010) antes de anunciar por UDP.
    for (int i = 0; i < 100 && !stopRequested_.load(std::memory_order_acquire) &&
                    !portBoundReady_.load(std::memory_order_acquire); ++i) {
        Sleep(10);
    }
    if (stopRequested_.load(std::memory_order_acquire)) return;

    SOCKET s = CreateNonInheritableSocket(SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return;

    DisableUdpConnReset(s);
    BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(discoveryPort_.load(std::memory_order_acquire));
    if (bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(socketMutex_);
        discoverySocket_ = s;
    }
    const auto cleanupDiscovery = MakeScopeExit([&]() {
        std::lock_guard<std::mutex> lock(socketMutex_);
        if (discoverySocket_ == s) {
            closesocket(discoverySocket_);
            discoverySocket_ = INVALID_SOCKET;
        }
    });

    uint8_t pubSec1[kWirePubKeySec1Bytes]{};
    std::string fpHex;
    {
        std::lock_guard<std::mutex> idLock(identityMutex_);
        if (!hostIdentity_.ExportPublicSec1(pubSec1)) return;
        fpHex = hostIdentity_.FingerprintHex();
    }
    const std::string pubHex = crypto::BytesToHexUpper(pubSec1, sizeof(pubSec1));
    const std::string hostName = GetLocalPcHostName();

    char buf[1024]{};
    while (!stopRequested_.load(std::memory_order_acquire)) {
        sockaddr_in from{};
        int fromLen = sizeof(from);
        const int r = recvfrom(s, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (r < 0) {
            if (stopRequested_.load(std::memory_order_acquire)) break;
            const int err = WSAGetLastError();
            // En UDP no orientado a conexión, datagramas sobredimensionados o ICMP transitorios no deben matar el hilo.
            if (err == WSAEMSGSIZE || err == WSAECONNRESET || err == WSAEINTR || err == WSAEWOULDBLOCK) {
                continue;
            }
            break;
        }
        if (r == 0) {
            continue;
        }
        buf[r] = '\0';

        std::string reqNonce;
        if (!ParseDiscoveryV2Request(buf, static_cast<size_t>(r), reqNonce)) {
            continue; // Ignora solicitudes ZCAM_DISCOVER_V1 sin autenticar o malformadas.
        }

        const uint16_t activePort = boundPort_.load(std::memory_order_acquire);
        const bool pinReq = !GetWifiPin().empty();
        // Respuesta Discovery UDP v2 firmada con ECDSA P-256 (sin ninguna información ni MAC derivada del PIN).
        const std::string canonical =
            std::string(kDiscoveryHostV2) +
            "|PORT=" + std::to_string(activePort) +
            "|NAME=" + hostName +
            "|PIN=" + (pinReq ? "1" : "0") +
            "|FP=" + fpHex +
            "|NONCE=" + reqNonce +
            "|PUB=" + pubHex;

        uint8_t sig64[kWireEcdsaSigBytes]{};
        bool signedOk = false;
        {
            std::lock_guard<std::mutex> idLock(identityMutex_);
            signedOk = hostIdentity_.SignP1363(
                reinterpret_cast<const uint8_t*>(canonical.data()), canonical.size(), sig64);
        }
        if (!signedOk) continue;

        const std::string reply = canonical + "|SIG=" + crypto::BytesToHexUpper(sig64, sizeof(sig64));
        sendto(s, reply.data(), static_cast<int>(reply.size()), 0, reinterpret_cast<const sockaddr*>(&from), fromLen);
    }
}

void StreamIngest::RunTestPattern() {
    SetConnectionLabel(L"Local (Patrón)");
    clientConnected_.store(true, std::memory_order_release);

    HANDLE timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    LARGE_INTEGER due{};
    due.QuadPart = -100000LL; // 10 ms inicial
    uint32_t lastW = 0, lastH = 0, lastFps = 0;

    uint64_t frameIdx = 0;
    while (!stopRequested_.load(std::memory_order_acquire)) {
        const VideoConfig cfg = GetVideoConfig();
        const uint32_t fps = std::max(15u, cfg.fps);
        const LONG periodMs = static_cast<LONG>(std::max(10u, 1000u / fps));
        if (cfg.width != lastW || cfg.height != lastH || fps != lastFps) {
            lastW = cfg.width;
            lastH = cfg.height;
            lastFps = fps;
            SetStatus(
                L"Patrón de prueba activo (" + std::to_wstring(lastW) + L"x" + std::to_wstring(lastH) +
                L" @ " + std::to_wstring(lastFps) + L"fps NV12)");
            if (timer) {
                SetWaitableTimer(timer, &due, periodMs, nullptr, nullptr, FALSE);
            }
        }

        const bool needPrev = preview_ && preview_->IsEnabled();
        ID3D11Texture2D* previewRaw = nullptr;
        const bool pubOk = d3dCtx_->PublishTestPattern(cfg.width, cfg.height, frameIdx, fps, 1, needPrev ? &previewRaw : nullptr);
        Microsoft::WRL::ComPtr<ID3D11Texture2D> previewTex;
        previewTex.Attach(previewRaw);
        if (pubOk) {
            ++frameIdx;
            RecordFrame(cfg.width, cfg.height, true);
            if (needPrev && previewTex) {
                preview_->PresentNv12(previewTex.Get(), 0, cfg.width, cfg.height, 0);
            }
        }
        if (timer) {
            WaitForSingleObject(timer, static_cast<DWORD>(periodMs + 10));
        } else {
            Sleep(static_cast<DWORD>(periodMs));
        }
    }

    if (timer) {
        CancelWaitableTimer(timer);
        CloseHandle(timer);
    }
}

void StreamIngest::RunTcpServer(uint16_t basePort, bool usbOnly) {
    SOCKET srv = INVALID_SOCKET;
    uint16_t actualPort = basePort;
    for (uint16_t offset = 0; offset < kStreamPortFallbackCount; ++offset) {
        const uint32_t cand = static_cast<uint32_t>(basePort) + offset;
        if (cand > 65535u) break;

        SOCKET candidate = CreateNonInheritableSocket(SOCK_STREAM, IPPROTO_TCP);
        if (candidate == INVALID_SOCKET) break;

        BOOL reuse = TRUE;
        setsockopt(candidate, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY; // Escucha 127.0.0.1 (USB) + LAN (Wi-Fi) para salto automático.
        addr.sin_port = htons(static_cast<uint16_t>(cand));

        if (bind(candidate, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0 &&
            listen(candidate, 1) == 0) {
            srv = candidate;
            actualPort = static_cast<uint16_t>(cand);
            break;
        }
        closesocket(candidate);
    }

    if (srv == INVALID_SOCKET) {
        portBoundReady_.store(true, std::memory_order_release);
        SetStatus(L"No se pudo abrir puerto TCP interno disponible");
        return;
    }

    boundPort_.store(actualPort, std::memory_order_release);
    portBoundReady_.store(true, std::memory_order_release);

    {
        std::lock_guard<std::mutex> lock(socketMutex_);
        listenSocket_ = srv;
    }
    const auto cleanupListen = MakeScopeExit([&]() {
        std::lock_guard<std::mutex> lock(socketMutex_);
        if (listenSocket_ == srv) {
            closesocket(listenSocket_);
            listenSocket_ = INVALID_SOCKET;
        }
    });

    const std::wstring lanIp = DetectLocalLanIp();
    const std::wstring waitLabel = usbOnly
        ? L"Esperando móvil (USB / Wi-Fi)"
        : (lanIp.empty() ? L"Esperando Wi-Fi" : (L"Esperando Wi-Fi (" + lanIp + L")"));
    const std::wstring waitStatus = usbOnly
        ? (lanIp.empty() ? L"Esperando móvil por USB o Wi-Fi automático" : (L"Listo USB / Wi-Fi automático (" + lanIp + L")"))
        : (lanIp.empty() ? L"Esperando móvil por Wi-Fi (red local)" : (L"Esperando móvil por Wi-Fi — PC lista (" + lanIp + L")"));

    while (!stopRequested_.load(std::memory_order_acquire)) {
        SetConnectionLabel(waitLabel);
        SetStatus(waitStatus);
        clientConnected_.store(false, std::memory_order_release);
        clientIsLoopback_.store(false, std::memory_order_release);

        sockaddr_in peerAddr{};
        int peerLen = sizeof(peerAddr);
        SOCKET client = accept(srv, reinterpret_cast<sockaddr*>(&peerAddr), &peerLen);
        if (client == INVALID_SOCKET) break;
        SetHandleInformation(reinterpret_cast<HANDLE>(client), HANDLE_FLAG_INHERIT, 0);

        // Timeout amplio (6 s) durante el handshake PBKDF2-100k en móviles de gama media; baja a 2.5 s al iniciar video.
        const DWORD handshakeTimeoutMs = 6000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&handshakeTimeoutMs), sizeof(handshakeTimeoutMs));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&handshakeTimeoutMs), sizeof(handshakeTimeoutMs));
        const BOOL keepAlive = TRUE;
        setsockopt(client, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&keepAlive), sizeof(keepAlive));

        // Loopback 127.x corresponde al túnel USB adb reverse; cualquier otra IP corresponde a Wi-Fi LAN.
        const uint32_t peerIpKey = ntohl(peerAddr.sin_addr.s_addr);
        const bool isLoopback = (peerIpKey >> 24) == 127u;
        clientIsLoopback_.store(isLoopback, std::memory_order_release);
        wchar_t ipBuf[INET_ADDRSTRLEN]{};
        std::wstring connLabel;
        if (isLoopback) {
            connLabel = L"Cable USB (ADB)";
        } else if (InetNtopW(AF_INET, &peerAddr.sin_addr, ipBuf, INET_ADDRSTRLEN) != nullptr) {
            connLabel = std::wstring(L"Wi-Fi (") + ipBuf + L")";
        } else {
            connLabel = L"Wi-Fi";
        }

        BOOL noDelay = TRUE;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
        {
            std::lock_guard<std::mutex> sendLock(sendMutex_);
            sendCipher_.Reset();
            sessionEncrypted_ = false;
            std::lock_guard<std::mutex> sockLock(socketMutex_);
            activeClientSocket_ = client;
        }

        MfHwDecoder decoder;
        if (decoder.Initialize(d3dCtx_, preview_)) {
            SetConnectionLabel(connLabel);
            SetStatus(L"Autenticando " + connLabel + L" (Wire v2)...");
            HandleTcpClient(client, peerIpKey, decoder, !isLoopback);
            decoder.DrainAll();
            decoder.Shutdown();
            SetConnectionLabel(waitLabel);
        } else {
            SetStatus(L"Error inicializando decodificador HW H.264");
        }

        clientConnected_.store(false, std::memory_order_release);
        clientIsLoopback_.store(false, std::memory_order_release);
        {
            std::lock_guard<std::mutex> sendLock(sendMutex_);
            sendCipher_.Reset();
            sessionEncrypted_ = false;
            std::lock_guard<std::mutex> sockLock(socketMutex_);
            if (activeClientSocket_ != INVALID_SOCKET) {
                closesocket(activeClientSocket_);
                activeClientSocket_ = INVALID_SOCKET;
            }
        }
    }
}

void StreamIngest::HandleTcpClient(SOCKET clientSock, uint32_t peerIpKey, MfHwDecoder& decoder, bool requireWifiAuth) {
    if (!EnsureHostIdentity()) {
        SendHandshakeError(clientSock, kWireErrProtocol);
        SetStatus(L"Error criptográfico: identidad ECDSA del PC no disponible");
        return;
    }

    // 1. Verifica rate-limit por IP (5 fallos / 5 minutos) antes de procesar cualquier intento Wi-Fi.
    if (requireWifiAuth && IsPeerRateLimited(peerIpKey)) {
        SendHandshakeError(clientSock, kWireErrRateLimited);
        SetStatus(L"Wi-Fi bloqueado temporalmente (5 fallos de PIN en 5 min)");
        for (int i = 0; i < 12 && !stopRequested_.load(std::memory_order_acquire); ++i) Sleep(100);
        return;
    }

    std::string expectedPin = requireWifiAuth ? GetWifiPin() : std::string();
    const auto wipePin = MakeScopeExit([&]() {
        if (!expectedPin.empty()) SecureZeroMemory(expectedPin.data(), expectedPin.size());
    });
    if (requireWifiAuth && expectedPin.empty()) {
        SendHandshakeError(clientSock, kWireErrPinRequired);
        SetStatus(L"Wi-Fi rechazado: configura un PIN Wi-Fi en Ajustes");
        for (int i = 0; i < 12 && !stopRequested_.load(std::memory_order_acquire); ++i) Sleep(100);
        return;
    }

    // 2. Mensaje 1 (ClientHello): exige cabecera ZeroCam Wire v2 (rechaza Wire v1 y Annex-B crudo en Wi-Fi y USB).
    WirePacketHeader helloHdr{};
    if (!RecvExact(clientSock, &helloHdr, sizeof(helloHdr), stopRequested_)) return;
    if (helloHdr.magic != kWireMagic ||
        helloHdr.version != kWireVersion ||
        helloHdr.type != static_cast<uint8_t>(WirePacketType::HandshakeClientHello) ||
        helloHdr.payloadSize != kWireClientHelloBytes) {
        SendHandshakeError(clientSock, kWireErrProtocol);
        SetStatus(requireWifiAuth
            ? L"Wi-Fi rechazado: actualiza ZeroCam Mobile a v1.2+ (Wire v2 cifrado)"
            : L"USB rechazado: actualiza ZeroCam Mobile a v1.2+ (Wire v2)");
        for (int i = 0; i < 12 && !stopRequested_.load(std::memory_order_acquire); ++i) Sleep(100);
        return;
    }

    uint8_t chPayload[kWireClientHelloBytes]{};
    if (!RecvExact(clientSock, chPayload, sizeof(chPayload), stopRequested_)) return;
    const uint8_t* clientNonce = chPayload;
    const uint8_t* clientEphPub = chPayload + kWireNonceBytes;
    if (clientEphPub[0] != 0x04u) {
        SendHandshakeError(clientSock, kWireErrProtocol);
        return;
    }

    // 3. Mensaje 2 (ServerHello): genera clave efímera ECDH P-256 + nonce y firma con identidad ECDSA persistente.
    const uint8_t authMode = requireWifiAuth ? kWireAuthModeWifiPin : kWireAuthModeUsbLocal;
    uint8_t serverNonce[kWireNonceBytes]{};
    crypto::EcdhP256KeyPair serverEph;
    uint8_t serverEphPub[kWirePubKeySec1Bytes]{};
    uint8_t serverIdPub[kWirePubKeySec1Bytes]{};
    if (!crypto::RandomBytes(serverNonce, sizeof(serverNonce)) ||
        !serverEph.Generate() ||
        !serverEph.ExportPublicSec1(serverEphPub)) {
        SendHandshakeError(clientSock, kWireErrProtocol);
        return;
    }

    uint8_t sigEcdsa[kWireEcdsaSigBytes]{};
    {
        std::lock_guard<std::mutex> idLock(identityMutex_);
        if (!hostIdentity_.ExportPublicSec1(serverIdPub)) {
            SendHandshakeError(clientSock, kWireErrProtocol);
            return;
        }
        const std::vector<uint8_t> signedMsg = crypto::BuildServerHelloSignedBytes(
            authMode, clientNonce, clientEphPub, serverNonce, serverEphPub, serverIdPub);
        if (!hostIdentity_.SignP1363(signedMsg.data(), signedMsg.size(), sigEcdsa)) {
            SendHandshakeError(clientSock, kWireErrProtocol);
            return;
        }
    }

    uint8_t shPayload[kWireServerHelloBytes]{};
    shPayload[0] = authMode;
    std::memcpy(shPayload + 1u, serverNonce, kWireNonceBytes);
    std::memcpy(shPayload + 17u, serverEphPub, kWirePubKeySec1Bytes);
    std::memcpy(shPayload + 82u, serverIdPub, kWirePubKeySec1Bytes);
    std::memcpy(shPayload + 147u, sigEcdsa, kWireEcdsaSigBytes);
    if (!SendWireV2PlainPacket(clientSock, WirePacketType::HandshakeServerHello, 0, shPayload, sizeof(shPayload))) {
        return;
    }

    // 4. Derivación ECDH + PBKDF2(100k) + HKDF-SHA256 antes de recibir ClientFinish.
    uint8_t ecdhSecretBE[32]{};
    const bool ecdhOk = serverEph.ComputeSharedSecretBE(clientEphPub, ecdhSecretBE);
    serverEph.Reset();
    if (!ecdhOk) {
        SecureZeroMemory(ecdhSecretBE, sizeof(ecdhSecretBE));
        SendHandshakeError(clientSock, kWireErrProtocol);
        return;
    }

    crypto::WireV2DerivedKeys keys;
    const bool derivedOk = crypto::DeriveWireV2Keys(
        authMode, clientNonce, clientEphPub, serverNonce, serverEphPub, serverIdPub, sigEcdsa,
        ecdhSecretBE, expectedPin, keys);
    SecureZeroMemory(ecdhSecretBE, sizeof(ecdhSecretBE));
    if (!derivedOk) {
        SendHandshakeError(clientSock, kWireErrProtocol);
        return;
    }

    // 5. Mensaje 3 (ClientFinish): verifica prueba HMAC-SHA256 en tiempo constante (cero oráculo offline).
    WirePacketHeader cfHdr{};
    if (!RecvExact(clientSock, &cfHdr, sizeof(cfHdr), stopRequested_)) return;
    if (cfHdr.magic != kWireMagic ||
        cfHdr.version != kWireVersion ||
        cfHdr.type != static_cast<uint8_t>(WirePacketType::HandshakeClientFinish) ||
        cfHdr.payloadSize != kWireProofBytes) {
        SendHandshakeError(clientSock, kWireErrProtocol);
        return;
    }
    uint8_t recvClientProof[kWireProofBytes]{};
    if (!RecvExact(clientSock, recvClientProof, sizeof(recvClientProof), stopRequested_)) return;

    uint8_t expectedClientProof[kWireProofBytes]{};
    if (!crypto::ComputeHandshakeProof(keys.authKey, keys.transcriptHash, "client", expectedClientProof) ||
        !crypto::ConstantTimeEquals(expectedClientProof, recvClientProof, kWireProofBytes)) {
        bool nowBlocked = false;
        if (requireWifiAuth) {
            nowBlocked = RecordAuthFailure(peerIpKey);
        }
        SendHandshakeError(clientSock, nowBlocked ? kWireErrRateLimited : kWireErrPinInvalid);
        SetStatus(nowBlocked
            ? L"Wi-Fi bloqueado 5 min: demasiados intentos de PIN incorrecto"
            : L"Wi-Fi rechazado: PIN incorrecto");
        for (int i = 0; i < 15 && !stopRequested_.load(std::memory_order_acquire); ++i) Sleep(100);
        return;
    }

    if (requireWifiAuth) {
        ClearAuthFailures(peerIpKey);
    }

    // 6. Mensaje 4 (ServerFinish): confirma identidad mutua y activa canal AEAD AES-256-GCM.
    uint8_t serverProof[kWireProofBytes]{};
    if (!crypto::ComputeHandshakeProof(keys.authKey, keys.transcriptHash, "server", serverProof) ||
        !SendWireV2PlainPacket(clientSock, WirePacketType::HandshakeServerFinish, 0, serverProof, sizeof(serverProof))) {
        return;
    }

    crypto::Aes256GcmCipher recvCipher;
    if (!recvCipher.Init(keys.c2sKey, keys.c2sIvBase)) {
        return;
    }
    {
        std::lock_guard<std::mutex> sendLock(sendMutex_);
        if (!sendCipher_.Init(keys.s2cKey, keys.s2cIvBase)) {
            return;
        }
        sessionEncrypted_ = true;
    }

    // Restaura timeout corto de 2.5 s una vez completado el handshake para soltar rápido desconexiones USB.
    const DWORD streamTimeoutMs = 2500;
    setsockopt(clientSock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&streamTimeoutMs), sizeof(streamTimeoutMs));
    setsockopt(clientSock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&streamTimeoutMs), sizeof(streamTimeoutMs));

    clientConnected_.store(true, std::memory_order_release);
    SetStatus(requireWifiAuth
        ? L"Transmitiendo por Wi-Fi cifrado (AES-256-GCM)"
        : L"Transmitiendo por Cable USB cifrado (Wire v2)");

    // Sincroniza configuración de video y cámara inmediatamente sobre el canal AEAD ya autenticado.
    {
        bool sendCfg = false;
        VideoConfig initialCfg{};
        {
            std::lock_guard<std::mutex> lock(videoCfgMutex_);
            sendCfg = videoCfgUserSet_;
            initialCfg = videoCfg_;
        }
        if (sendCfg) {
            SendControlJson(BuildSetVideoJson(initialCfg));
        }
        bool sendCam = false;
        CameraRemoteConfig initialCam{};
        {
            std::lock_guard<std::mutex> lock(camCtrlMutex_);
            sendCam = camCtrlUserSet_;
            initialCam = camCtrl_;
        }
        if (sendCam) {
            SendControlJson(BuildCameraControlJson(initialCam));
        }
    }

    // 7. Bucle de recepción AEAD AES-256-GCM (cabecera 20 B como AAD + ct||tag[16]).
    AnnexBParser parser;
    std::vector<uint8_t> normalizedAu;
    std::vector<uint8_t> encPayload;
    std::vector<uint8_t> plainPayload;

    while (!stopRequested_.load(std::memory_order_acquire)) {
        WirePacketHeader hdr{};
        if (!RecvExact(clientSock, &hdr, sizeof(hdr), stopRequested_)) break;
        if (hdr.magic != kWireMagic || hdr.version != kWireVersion) break;
        if (hdr.payloadSize < kWireAeadTagBytes || hdr.payloadSize > kMaxWirePayloadBytes + kWireAeadTagBytes) {
            break;
        }

        encPayload.resize(hdr.payloadSize);
        if (!RecvExact(clientSock, encPayload.data(), hdr.payloadSize, stopRequested_)) break;

        if (!recvCipher.DecryptNext(
                reinterpret_cast<const uint8_t*>(&hdr),
                sizeof(hdr),
                encPayload.data(),
                encPayload.size(),
                plainPayload)) {
            SetStatus(L"Sesión cerrada: fallo de autenticación AEAD");
            break;
        }

        if (hdr.type == static_cast<uint8_t>(WirePacketType::VideoH264) && !plainPayload.empty()) {
            bool kf = (hdr.flags & kWireFlagKeyframe) != 0;
            const uint8_t rot = ResolveRotationCode(ExtractRotationCode(hdr.flags));
            const uint32_t fps = targetFps_.load(std::memory_order_acquire);
            parser.NormalizeAccessUnit(plainPayload.data(), plainPayload.size(), normalizedAu, kf);
            if (decoder.DecodeAccessUnit(normalizedAu.data(), normalizedAu.size(), static_cast<int64_t>(hdr.ptsUs), fps, 1, rot)) {
                RecordFrame(decoder.Width(), decoder.Height(), decoder.IsHwAccelerated() && decoder.HwZeroCopyFrames() > 0);
            }
        }
    }
}

void StreamIngest::RunUdpReceiver(uint16_t port) {
    SOCKET s = CreateNonInheritableSocket(SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        SetStatus(L"Error creando socket UDP");
        return;
    }

    DisableUdpConnReset(s);
    int rcvBuf = 4 * 1024 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvBuf), sizeof(rcvBuf));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s);
        SetStatus(L"No se pudo abrir puerto UDP :" + std::to_wstring(port));
        return;
    }

    {
        std::lock_guard<std::mutex> lock(socketMutex_);
        udpSocket_ = s;
    }
    const auto cleanupUdp = MakeScopeExit([&]() {
        std::lock_guard<std::mutex> lock(socketMutex_);
        if (udpSocket_ == s) {
            closesocket(udpSocket_);
            udpSocket_ = INVALID_SOCKET;
        }
    });

    MfHwDecoder decoder;
    if (!decoder.Initialize(d3dCtx_, preview_)) {
        SetStatus(L"Error inicializando decodificador HW H.264");
        return;
    }

    SetConnectionLabel(L"Escuchando UDP :" + std::to_wstring(port));
    SetStatus(L"Escuchando flujo H.264 Annex-B en UDP :" + std::to_wstring(port));
    AnnexBParser parser;
    std::vector<uint8_t> pkt(65536);
    const ULONGLONG t0 = GetTickCount64();

    while (!stopRequested_.load(std::memory_order_acquire)) {
        const int r = recvfrom(s, reinterpret_cast<char*>(pkt.data()), static_cast<int>(pkt.size()), 0, nullptr, nullptr);
        if (r < 0) {
            if (stopRequested_.load(std::memory_order_acquire)) break;
            const int err = WSAGetLastError();
            if (err == WSAEMSGSIZE || err == WSAECONNRESET || err == WSAEINTR || err == WSAEWOULDBLOCK) {
                continue;
            }
            break;
        }
        if (r == 0) {
            continue;
        }
        if (!clientConnected_.exchange(true, std::memory_order_acq_rel)) {
            SetConnectionLabel(L"Red UDP :" + std::to_wstring(port));
        }
        const int64_t ptsUs = static_cast<int64_t>((GetTickCount64() - t0) * 1000ull);
        parser.Feed(pkt.data(), static_cast<size_t>(r), ptsUs, [&](const uint8_t* au, size_t len, int64_t p, bool) {
            if (decoder.DecodeAccessUnit(au, len, p)) {
                RecordFrame(decoder.Width(), decoder.Height(), decoder.IsHwAccelerated() && decoder.HwZeroCopyFrames() > 0);
            }
        });
    }
    decoder.DrainAll();
    decoder.Shutdown();
}

void StreamIngest::RunRtspSrtRemux(const std::wstring& url) {
    if (!IsAllowedMediaUrl(url)) {
        SetStatus(L"URL inválida: usa rtsp://, rtsps://, srt://, udp:// o tcp:// (RFC 3986)");
        return;
    }

    const std::wstring ffmpegExe = FindFfmpegExecutable();
    if (ffmpegExe.empty()) {
        SetStatus(L"No se encontró ffmpeg.exe en PATH seguro para remux RTSP/SRT");
        return;
    }

    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        SetStatus(L"Error creando pipe para FFmpeg remux");
        return;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    std::wstring cmd =
        L"\"" + ffmpegExe + L"\" -hide_banner -loglevel error -fflags nobuffer -flags low_delay "
        L"-protocol_whitelist rtp,udp,tcp,rtsp,rtsps,tls,srt -rtsp_transport tcp -i \"" +
        url + L"\" -an -c:v copy -bsf:v h264_mp4toannexb -f h264 pipe:1";

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.StartupInfo.hStdOutput = writePipe;
    si.StartupInfo.hStdError = nullptr;
    si.StartupInfo.wShowWindow = SW_HIDE;

    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<BYTE> attrBuf(attrSize);
    auto* attrList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    if (attrSize == 0 || !InitializeProcThreadAttributeList(attrList, 1, 0, &attrSize)) {
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        SetStatus(L"Error creando atributos de proceso FFmpeg");
        return;
    }
    HANDLE inheritHandles[1] = { writePipe };
    if (!UpdateProcThreadAttribute(
            attrList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inheritHandles, sizeof(inheritHandles), nullptr, nullptr)) {
        DeleteProcThreadAttributeList(attrList);
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        SetStatus(L"Error configurando lista de handles FFmpeg");
        return;
    }
    si.lpAttributeList = attrList;

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    // Usa lpApplicationName explícito y hereda exclusivamente writePipe.
    const BOOL ok = CreateProcessW(
        ffmpegExe.c_str(), cmdBuf.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
        nullptr, nullptr, &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attrList);
    CloseHandle(writePipe);
    if (!ok) {
        CloseHandle(readPipe);
        SetStatus(L"No se pudo iniciar ffmpeg.exe para remux RTSP/SRT");
        return;
    }

    CloseHandle(pi.hThread);
    HANDLE procHandle = pi.hProcess;
    {
        std::lock_guard<std::mutex> lock(socketMutex_);
        ffmpegProcess_ = procHandle;
    }
    const auto cleanupFfmpeg = MakeScopeExit([&]() {
        std::lock_guard<std::mutex> lock(socketMutex_);
        if (ffmpegProcess_ == procHandle) {
            TerminateProcess(ffmpegProcess_, 0);
            CloseHandle(ffmpegProcess_);
            ffmpegProcess_ = nullptr;
        }
    });

    MfHwDecoder decoder;
    if (!decoder.Initialize(d3dCtx_, preview_)) {
        CloseHandle(readPipe);
        SetStatus(L"Error inicializando decodificador HW H.264");
        return;
    }

    SetStatus(L"RTSP/SRT remux zero-decode conectado (HW VCN)");
    AnnexBParser parser;
    std::vector<uint8_t> buf(65536);
    DWORD bytesRead = 0;
    const ULONGLONG t0 = GetTickCount64();

    while (!stopRequested_.load(std::memory_order_acquire) &&
           ReadFile(readPipe, buf.data(), static_cast<DWORD>(buf.size()), &bytesRead, nullptr) &&
           bytesRead > 0) {
        if (!clientConnected_.exchange(true, std::memory_order_acq_rel)) {
            SetConnectionLabel(L"Red RTSP / SRT");
        }
        const int64_t ptsUs = static_cast<int64_t>((GetTickCount64() - t0) * 1000ull);
        parser.Feed(buf.data(), bytesRead, ptsUs, [&](const uint8_t* au, size_t len, int64_t p, bool) {
            if (decoder.DecodeAccessUnit(au, len, p)) {
                RecordFrame(decoder.Width(), decoder.Height(), decoder.IsHwAccelerated() && decoder.HwZeroCopyFrames() > 0);
            }
        });
    }

    CloseHandle(readPipe);
    decoder.DrainAll();
    decoder.Shutdown();
}

} // namespace zerocam
