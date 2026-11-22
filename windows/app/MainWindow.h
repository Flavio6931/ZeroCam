#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include "AppConfig.h"
#include "D3D11Context.h"
#include "EffectsDrawer.h"
#include "PreviewRenderer.h"
#include "SettingsDrawer.h"
#include "StreamIngest.h"
#include "UiTheme.h"
#include "VirtualCameraHost.h"

namespace zerocam {

/// Ventana principal Win32 nativa fiel al diseño Dark de Figma (vista previa bajo demanda y drawer lateral).
class MainWindow {
public:
    MainWindow();
    ~MainWindow();

    /// Crea la ventana principal, carga preferencias, inicializa D3D11, drawer de Ajustes y bandeja del sistema.
    bool Create(HINSTANCE hInstance);

    /// Ejecuta el bucle de mensajes estándar de Windows.
    int RunMessageLoop();

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK PreviewHostProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    void ComputeRects(int width, int height);
    void LayoutControls(int width, int height);
    void Paint(HDC targetDc);
    void RefreshTelemetry();
    void ToggleCamera();
    void SetPreviewWanted(bool wanted);
    void SetWifiMode(bool wifi);
    void ToggleSettingsDrawer();
    void ToggleEffectsDrawer();
    void AdjustPreviewZoom(short wheelDelta);
    void ResetPreviewZoom();
    void ApplyConfigToIngest(bool pushIfConnected, bool portChanged);
    bool RunElevatedSelfRegister();
    bool EnsureVirtualCameraReady();
    void MinimizeToTray();
    void RestoreFromTray();
    void ShowTrayMenu();
    void AddTrayIcon();
    void RemoveTrayIcon();
    SourceMode SelectedSourceMode() const noexcept;

    enum class CameraTransition { Idle, Starting, Stopping, Switching, Registering };
    enum class CameraOpKind { Start, Stop, RestartIngest, RegisterVcam, ReconnectUsb };
    struct CameraOpRequest {
        CameraOpKind kind = CameraOpKind::Stop;
        SourceMode mode = SourceMode::ZeroCamUsb;
        std::wstring endpoint;
        bool testPattern = false;
        bool forceElevate = false;
        uint16_t internalPort = 5000;
    };
    struct CameraOpResult {
        CameraOpKind kind = CameraOpKind::Stop;
        bool cameraRunning = false;
        bool vcamActive = false;
        bool comReady = false;
        bool testPattern = false;
        bool forcePreviewOn = false;
        std::wstring actionMsg;
    };

    void StartCameraWorker();
    void StopCameraWorker();
    void EnqueueCameraOp(CameraOpRequest req);
    void CameraWorkerMain();
    void HandleCameraOpDone(CameraOpResult* rawResult);

    HINSTANCE hInstance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND previewHost_ = nullptr;
    struct PreviewPlaceholderState;
    std::unique_ptr<PreviewPlaceholderState> previewState_;
    SettingsDrawer settingsDrawer_{};
    EffectsDrawer effectsDrawer_{};

    float dpiScale_ = 1.0f;
    HICON headerIcon_ = nullptr;
    NOTIFYICONDATAW nid_{};
    bool trayAdded_ = false;

    D3D11Context d3dCtx_;
    PreviewRenderer preview_;
    StreamIngest ingest_;
    VirtualCameraHost vcam_;
    AppConfig cfg_{};
    TelemetrySnapshot lastTelemetry_{};
    bool lastComReady_ = false;
    bool lastPlatformSupported_ = true;

    enum class ActiveDrawer { None, Settings, Effects };
    bool cameraRunning_ = false;
    bool vcamRunningCached_ = false;
    CameraTransition cameraTransition_ = CameraTransition::Idle;
    bool userPreviewWanted_ = true;
    ActiveDrawer activeDrawer_ = ActiveDrawer::None;
    bool autoRegAttempted_ = false;
    bool testPatternActive_ = false;
    std::wstring lastActionMsg_;

    std::atomic<bool> closing_{ false };
    std::mutex opMutex_;
    std::condition_variable opCv_;
    std::queue<CameraOpRequest> opQueue_;
    bool opExit_ = false;
    std::thread cameraOpThread_;

    // Geometría calculada por LayoutControls / ComputeRects.
    Gdiplus::RectF rcHeaderPill_{};
    Gdiplus::RectF rcTabUsb_{};
    Gdiplus::RectF rcTabWifi_{};
    Gdiplus::RectF rcTransportBox_{};
    Gdiplus::RectF rcBtnSettings_{};
    Gdiplus::RectF rcBtnEffects_{};
    Gdiplus::RectF rcBtnCamera_{};
    Gdiplus::RectF rcPreviewCard_{};
    Gdiplus::RectF rcEcoBtn_{};
    Gdiplus::RectF rcDrawerArea_{};
    Gdiplus::RectF rcStatusStrip_{};
    float yHeaderLine_ = 0.0f;

    int hoverId_ = 0;
    bool trackingLeave_ = false;
};

} // namespace zerocam
