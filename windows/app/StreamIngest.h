#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include "AnnexBParser.h"
#include "D3D11Context.h"
#include "MfHwDecoder.h"
#include "PreviewRenderer.h"
#include "VideoEffects.h"
#include "../shared/WireCrypto.h"
#include "../shared/ZeroCamProtocol.h"

namespace zerocam {

// Modos de entrada soportados por el motor de ingesta.
enum class SourceMode {
    ZeroCamUsb   = 0,
    ZeroCamWifi  = 1,
    RtspSrtRemux = 2,
    UdpH264      = 3,
    TestPattern  = 4
};

// Instantánea thread-safe de telemetría para mostrar en la interfaz sin bloqueos.
struct TelemetrySnapshot {
    bool running = false;
    bool clientConnected = false;
    bool hwDecodeActive = false;
    bool zeroCopyActive = false;
    uint32_t width = 0;
    uint32_t height = 0;
    double fps = 0.0;
    uint64_t totalFrames = 0;
    uint32_t consumerCount = 0;
    int rotationDeg = 0;
    bool rotationIsAuto = true;
    std::wstring connectionLabel = L"Sin conexión";
    std::wstring statusText;
};

// Configuración de video deseada (resolución, FPS y bitrate) enviada al móvil o usada en patrón de prueba.
struct VideoConfig {
    uint32_t width = 1280;
    uint32_t height = 720;
    uint32_t fps = 30;
    uint32_t bitrateBps = 4000000;
};

// Orquestador de red, remux RTSP/SRT y patrón de prueba hacia el decodificador HW D3D11VA.
class StreamIngest {
public:
    StreamIngest() = default;
    ~StreamIngest();

    // Inicia los hilos de ingesta y descubrimiento UDP según el modo seleccionado.
    bool Start(SourceMode mode, const std::wstring& endpoint, D3D11Context* d3dCtx, PreviewRenderer* preview);
    // Detiene ordenadamente sockets, subproceso ffmpeg y hilos activos.
    void Stop();
    // Configura rotación manual (0..3) o automática desde el teléfono (-1).
    void SetRotationOverride(int code) noexcept { rotationOverride_.store(code, std::memory_order_release); }
    // Configura resolución, FPS y bitrate deseados; los envía de inmediato si hay cliente conectado.
    bool SetVideoConfig(uint32_t width, uint32_t height, uint32_t fps, uint32_t bitrateBps, bool pushIfConnected = true);
    // Obtiene una copia segura de la configuración de video activa.
    VideoConfig GetVideoConfig();
    // Configura un PIN opcional requerido a clientes Wi-Fi en el handshake inicial.
    void SetWifiPin(const std::string& pin);
    // Configura los controles remotos de enfoque/cámara y los envía si hay móvil conectado.
    bool SetCameraControl(const CameraRemoteConfig& cfg, bool pushIfConnected = true);
    // Envía un punto normalizado [0..1] para enfocar desde clic en la vista previa.
    bool SendFocusPoint(float normX, float normY);
    // Obtiene el PIN Wi-Fi activo de forma segura entre hilos.
    std::string GetWifiPin();
    // Despierta de inmediato al monitor USB (p. ej. tras evento WM_DEVICECHANGE al conectar cable).
    void TriggerUsbReconnect() noexcept { usbWakeRequested_.store(true, std::memory_order_release); }
    // Devuelve el puerto TCP interno actualmente enlazado (5000 o respaldo).
    uint16_t BoundPort() const noexcept { return boundPort_.load(std::memory_order_acquire); }
#ifdef ZEROCAM_ALLOW_IPC_ENV_OVERRIDE
    /// Permite fijar un puerto UDP de discovery alternativo exclusivamente en pruebas automatizadas.
    void SetDiscoveryPortForTest(uint16_t port) noexcept { discoveryPort_.store(port, std::memory_order_release); }
#endif
    // Valida estrictamente una URL multimedia RFC 3986 permitida para remux con FFmpeg.
    static bool IsAllowedMediaUrl(const std::wstring& url) noexcept;
    // Devuelve la huella criptográfica FP del PC formateada (XXXX-XXXX-XXXX-XXXX) para verificación en Android.
    std::wstring HostFingerprintDisplay();
    // Devuelve la huella criptográfica FP en 16 hex mayúsculas.
    std::string HostFingerprintHex();
    // Envía un comando JSON de control cifrado hacia la app móvil conectada por ZeroCam Wire v2.
    bool SendControlJson(const std::string& json);
    // Devuelve métricas actuales del pipeline sin bloquear la decodificación.
    TelemetrySnapshot GetTelemetry();

private:
    struct AuthRateEntry {
        int failures = 0;
        ULONGLONG windowStartMs = 0;
        ULONGLONG blockedUntilMs = 0;
    };

    bool EnsureHostIdentity();
    bool IsPeerRateLimited(uint32_t peerIpKey);
    bool RecordAuthFailure(uint32_t peerIpKey);
    void ClearAuthFailures(uint32_t peerIpKey);

    static std::string BuildSetVideoJson(const VideoConfig& cfg);
    static std::string BuildCameraControlJson(const CameraRemoteConfig& cfg);
    uint8_t ResolveRotationCode(uint8_t packetRotCode) noexcept;
    void WorkerMain(SourceMode mode, std::wstring endpoint);
    void DiscoveryMain(uint16_t streamPort);
    void UsbMonitorMain(uint16_t mobilePort);
    void RunTestPattern();
    void RunTcpServer(uint16_t port, bool usbOnly);
    void RunUdpReceiver(uint16_t port);
    void RunRtspSrtRemux(const std::wstring& url);
    void HandleTcpClient(SOCKET clientSock, uint32_t peerIpKey, MfHwDecoder& decoder, bool requireWifiAuth);
    void SetStatus(const std::wstring& text);
    void SetConnectionLabel(const std::wstring& label);
    void RecordFrame(uint32_t w, uint32_t h, bool hwDecode);

    D3D11Context* d3dCtx_ = nullptr;
    PreviewRenderer* preview_ = nullptr;

    std::atomic<bool> stopRequested_{ false };
    std::atomic<bool> running_{ false };
    std::atomic<bool> clientConnected_{ false };
    std::atomic<bool> clientIsLoopback_{ false };
    std::atomic<bool> hwDecodeActive_{ false };
    std::atomic<uint32_t> curWidth_{ 0 };
    std::atomic<uint32_t> curHeight_{ 0 };
    std::atomic<uint64_t> totalFrames_{ 0 };
    std::atomic<int> rotationOverride_{ -1 };
    std::atomic<int> activeRotationDeg_{ 0 };
    std::atomic<uint16_t> boundPort_{ kDefaultStreamPort };
    std::atomic<uint16_t> discoveryPort_{ kDiscoveryUdpPort };
    std::atomic<bool> portBoundReady_{ false };
    std::atomic<bool> usbWakeRequested_{ false };

    std::mutex videoCfgMutex_;
    VideoConfig videoCfg_{};
    bool videoCfgUserSet_ = false;
    std::atomic<uint32_t> targetFps_{ 30 };

    std::mutex pinMutex_;
    std::string wifiPin_;

    std::mutex identityMutex_;
    crypto::EcdsaIdentityKey hostIdentity_;

    std::mutex authRateMutex_;
    std::unordered_map<uint32_t, AuthRateEntry> authRateMap_;

    std::mutex camCtrlMutex_;
    CameraRemoteConfig camCtrl_{};
    bool camCtrlUserSet_ = false;

    std::thread workerThread_;
    std::thread discoveryThread_;
    std::thread usbMonitorThread_;

    // Orden de bloqueo estricto: sendMutex_ -> socketMutex_ (Stop solo adquiere socketMutex_).
    std::mutex sendMutex_;
    crypto::Aes256GcmCipher sendCipher_;
    bool sessionEncrypted_ = false;

    std::mutex socketMutex_;
    SOCKET listenSocket_ = INVALID_SOCKET;
    SOCKET activeClientSocket_ = INVALID_SOCKET;
    SOCKET udpSocket_ = INVALID_SOCKET;
    SOCKET discoverySocket_ = INVALID_SOCKET;
    HANDLE ffmpegProcess_ = nullptr;

    std::mutex statusMutex_;
    std::wstring statusText_ = L"En espera";
    std::wstring connectionLabel_ = L"Sin conexión";

    ULONGLONG fpsWindowStartMs_ = 0;
    uint64_t fpsWindowFrames_ = 0;
    std::atomic<double> currentFps_{ 0.0 };
};

} // namespace zerocam
