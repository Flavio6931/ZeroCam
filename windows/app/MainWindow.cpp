#include "MainWindow.h"
#include "AdbBridge.h"
#include "resource.h"
#include "../shared/ZeroCamGuids.h"
#include "../shared/ZeroCamProtocol.h"
#include <commctrl.h>
#include <dbt.h>
#include <dwmapi.h>
#include <mfapi.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace zerocam {

namespace {

constexpr UINT kTimerTelemetryId    = 1001;
constexpr UINT kWmTrayCallback      = WM_APP + 42;
constexpr UINT kWmAutoRegister      = WM_APP + 43;
constexpr UINT kWmPreviewFocusClick = WM_APP + 44;
constexpr UINT kWmPreviewWheelZoom  = WM_APP + 45;
constexpr UINT kWmPreviewZoomReset  = WM_APP + 46;
constexpr UINT kWmCameraOpDone      = WM_APP + 47;

enum TrayMenuId : int {
    kIdPreviewHost = 109,
    kIdTrayShow = 201,
    kIdTrayTogglePrev = 202,
    kIdTrayExit = 203,
    kIdTrayRepair = 204,
    kIdTrayTestPattern = 205
};

enum MainHoverId : int {
    kMainHoverNone = 0,
    kMainHoverTabUsb,
    kMainHoverTabWifi,
    kMainHoverBtnSettings,
    kMainHoverBtnEffects,
    kMainHoverBtnCamera,
    kMainHoverEcoBtn
};

/// Carga el icono de la marca al tamaño del sistema; LR_SHARED evita tener que liberarlo.
/// Nota: no llamar "small" al parámetro; <rpcndr.h> lo define como macro de 'char'.
HICON LoadAppIcon(HINSTANCE instance, bool smallIcon) {
    const int cx = GetSystemMetrics(smallIcon ? SM_CXSMICON : SM_CXICON);
    const int cy = GetSystemMetrics(smallIcon ? SM_CYSMICON : SM_CYICON);
    HANDLE icon = LoadImageW(instance, MAKEINTRESOURCEW(IDI_ZEROCAM), IMAGE_ICON, cx, cy, LR_DEFAULTCOLOR | LR_SHARED);
    return icon ? static_cast<HICON>(icon) : LoadIconW(nullptr, IDI_APPLICATION);
}

void ApplyDwmTheme(HWND hwnd) {
    const BOOL darkMode = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &darkMode, sizeof(darkMode));
    const DWORD cornerRound = 2; // DWMWCP_ROUND
    DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &cornerRound, sizeof(cornerRound));
    const COLORREF borderCol = ui::kRefBorderSub;
    DwmSetWindowAttribute(hwnd, 34 /* DWMWA_BORDER_COLOR */, &borderCol, sizeof(borderCol));
    const COLORREF captionCol = ui::kRefBgSurface;
    DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &captionCol, sizeof(captionCol));
}

} // namespace

struct MainWindow::PreviewPlaceholderState {
    PreviewRenderer* preview = nullptr;
    bool hasLiveVideo = false;
    float dpiScale = 1.0f;
    std::wstring badgeText = L"LIVE  Esperando señal...";
    HWND mainHwnd = nullptr;
    UINT focusMsg = 0;
    UINT wheelMsg = 0;
    UINT zoomResetMsg = 0;
};

MainWindow::MainWindow() = default;

MainWindow::~MainWindow() {
    closing_.store(true, std::memory_order_release);
    StopCameraWorker();
    ingest_.Stop();
    d3dCtx_.SetProducerActive(false);
    AdbBridge::ShutdownBundledAdb();
    vcam_.Stop();
    preview_.SetEnabled(false);
    d3dCtx_.Shutdown();
    RemoveTrayIcon();
    if (previewHost_ && IsWindow(previewHost_)) {
        SetWindowLongPtrW(previewHost_, GWLP_USERDATA, 0);
    }
    previewState_.reset();
}

bool MainWindow::Create(HINSTANCE hInstance) {
    hInstance_ = hInstance;
    cfg_ = LoadAppConfig();
    headerIcon_ = LoadAppIcon(hInstance_, true);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &MainWindow::WindowProc;
    wc.hInstance = hInstance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadAppIcon(hInstance_, false);
    wc.hIconSm = headerIcon_;
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"ZeroCamMainWindowClass";
    RegisterClassExW(&wc);

    WNDCLASSEXW pwc{};
    pwc.cbSize = sizeof(pwc);
    pwc.lpfnWndProc = &MainWindow::PreviewHostProc;
    pwc.hInstance = hInstance_;
    pwc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    pwc.hbrBackground = nullptr;
    pwc.lpszClassName = L"ZeroCamPreviewHostClass";
    RegisterClassExW(&pwc);

    hwnd_ = CreateWindowExW(
        0,
        wc.lpszClassName,
        L"ZeroCam v1.3.2 — Cámara Virtual Zero-Copy",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 960, 680,
        nullptr, nullptr, hInstance_, this);
    if (!hwnd_) return false;

    const UINT dpi = GetDpiForWindow(hwnd_);
    dpiScale_ = (dpi > 0) ? (static_cast<float>(dpi) / 96.0f) : 1.0f;
    ApplyDwmTheme(hwnd_);

    // Ajusta el tamaño inicial según el DPI real del monitor principal.
    if (dpiScale_ > 1.01f) {
        SetWindowPos(hwnd_, nullptr, 0, 0, ui::ScaleDpi(960, dpiScale_), ui::ScaleDpi(680, dpiScale_),
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    if (!d3dCtx_.Initialize(true)) {
        lastActionMsg_ = L"Advertencia: no se pudo inicializar D3D11 hardware.";
    }

    previewHost_ = CreateWindowExW(
        0, pwc.lpszClassName, L"", WS_CHILD | WS_CLIPCHILDREN,
        0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdPreviewHost)), hInstance_, nullptr);
    previewState_ = std::make_unique<PreviewPlaceholderState>(PreviewPlaceholderState{
        &preview_, false, dpiScale_, L"LIVE  Esperando señal...",
        hwnd_, kWmPreviewFocusClick, kWmPreviewWheelZoom, kWmPreviewZoomReset
    });
    if (previewHost_) {
        SetWindowLongPtrW(previewHost_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(previewState_.get()));
    }
    preview_.Attach(&d3dCtx_, previewHost_);
    preview_.SetEnabled(userPreviewWanted_);

    SettingsDrawerCallbacks drawerCb{};
    drawerCb.onConfigApplied = [this](const AppConfig& updated, bool portChanged) {
        cfg_ = updated;
        SaveAppConfig(cfg_);
        ApplyConfigToIngest(cameraRunning_, portChanged);
        RefreshTelemetry();
    };
    drawerCb.onPreviewSwitchChanged = [this](bool enabled) {
        SetPreviewWanted(enabled);
    };
    drawerCb.onReconnectUsb = [this](std::wstring& outStatus) {
        if (cameraRunning_ && !testPatternActive_) {
            ingest_.TriggerUsbReconnect();
            outStatus = L"Verificando túnel USB ADB en segundo plano...";
        } else if (cameraTransition_ == CameraTransition::Idle) {
            CameraOpRequest req{};
            req.kind = CameraOpKind::ReconnectUsb;
            req.internalPort = cfg_.internalPort;
            EnqueueCameraOp(std::move(req));
            outStatus = L"Preparando túnel USB ADB en segundo plano...";
        } else {
            outStatus = L"Operación de cámara en curso...";
        }
    };
    drawerCb.onCloseRequested = [this]() {
        ToggleSettingsDrawer();
    };
    settingsDrawer_.Create(hwnd_, hInstance_, dpiScale_, std::move(drawerCb));

    EffectsDrawerCallbacks fxCb{};
    fxCb.onEffectsChanged = [this](const VideoEffectsConfig& fx, bool persistNow) {
        cfg_.effects = fx;
        d3dCtx_.SetEffectsConfig(cfg_.effects);
        if (persistNow) {
            SaveAppConfig(cfg_);
        }
    };
    fxCb.onCameraChanged = [this](const CameraRemoteConfig& cam) {
        cfg_.camera = cam;
        SaveAppConfig(cfg_);
        ingest_.SetCameraControl(cfg_.camera, cameraRunning_);
    };
    fxCb.onCloseRequested = [this]() {
        ToggleEffectsDrawer();
    };
    effectsDrawer_.Create(hwnd_, hInstance_, dpiScale_, std::move(fxCb));

    ApplyConfigToIngest(false, false);
    AddTrayIcon();

    // Evalúa COM/DLL una sola vez al iniciar (cero I/O repetido en el timer de UI).
    lastMfSupported_ = VirtualCameraHost::IsMfVirtualCameraSupported();
    lastPlatformSupported_ = VirtualCameraHost::IsPlatformSupported();
    lastComReady_ = lastPlatformSupported_ &&
                    VirtualCameraHost::IsComRegistered() &&
                    VirtualCameraHost::IsInstalledDllUpToDate();
    StartCameraWorker();

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    LayoutControls(rc.right - rc.left, rc.bottom - rc.top);
    SetTimer(hwnd_, kTimerTelemetryId, 500, nullptr);
    RefreshTelemetry();

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);

    if (lastPlatformSupported_ && !lastComReady_) {
        PostMessageW(hwnd_, kWmAutoRegister, 0, 0);
    }
    return true;
}

int MainWindow::RunMessageLoop() {
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK MainWindow::WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<MainWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return self ? self->HandleMessage(msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK MainWindow::PreviewHostProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCDESTROY) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    if (msg == WM_ERASEBKGND) {
        return 1;
    }
    if (msg == WM_SETCURSOR && LOWORD(lParam) == HTCLIENT) {
        auto* st = reinterpret_cast<PreviewPlaceholderState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (st && st->hasLiveVideo) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
    }
    if (msg == WM_LBUTTONDOWN) {
        auto* st = reinterpret_cast<PreviewPlaceholderState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (st && st->hasLiveVideo && st->mainHwnd && st->focusMsg != 0) {
            PostMessageW(st->mainHwnd, st->focusMsg, 0, lParam);
            return 0;
        }
    }
    if (msg == WM_MOUSEWHEEL) {
        auto* st = reinterpret_cast<PreviewPlaceholderState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (st && st->mainHwnd && st->wheelMsg != 0) {
            PostMessageW(st->mainHwnd, st->wheelMsg, wParam, lParam);
            return 0;
        }
    }
    if (msg == WM_MBUTTONDOWN) {
        auto* st = reinterpret_cast<PreviewPlaceholderState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (st && st->mainHwnd && st->zoomResetMsg != 0) {
            PostMessageW(st->mainHwnd, st->zoomResetMsg, 0, 0);
            return 0;
        }
    }
    if (msg == WM_PAINT) {
        auto* st = reinterpret_cast<PreviewPlaceholderState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        if (!st || !st->preview || !st->preview->IsEnabled() || !st->hasLiveVideo) {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            const int w = rc.right - rc.left;
            const int h = rc.bottom - rc.top;
            if (w > 0 && h > 0) {
                ui::DoubleBufferPaint db(hdc, w, h);
                Gdiplus::Graphics g(db.MemoryDc());
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
                const float scale = st ? st->dpiScale : 1.0f;
                const std::wstring badge = st ? st->badgeText : L"Vista previa lista";
                ui::DrawViewfinderPlaceholder(g, w, h, badge, scale);
            }
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void MainWindow::ComputeRects(int width, int height) {
    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);
    const float pad = ui::ScaleDpif(16.0f, dpiScale_);
    const float gap = ui::ScaleDpif(12.0f, dpiScale_);

    // 1. Header Strip (40px)
    const float headerH = ui::ScaleDpif(40.0f, dpiScale_);
    yHeaderLine_ = headerH;
    const float pillH = ui::ScaleDpif(26.0f, dpiScale_);
    rcHeaderPill_ = Gdiplus::RectF(w - pad - ui::ScaleDpif(210.0f, dpiScale_), (headerH - pillH) * 0.5f, ui::ScaleDpif(210.0f, dpiScale_), pillH);

    // 2. Transport Bar (48px)
    const float tbY = yHeaderLine_ + ui::ScaleDpif(12.0f, dpiScale_);
    const float tbH = ui::ScaleDpif(48.0f, dpiScale_);
    rcTransportBox_ = Gdiplus::RectF(pad, tbY, std::max(280.0f, w - pad * 2.0f), tbH);

    const float tbPad = ui::ScaleDpif(8.0f, dpiScale_);
    const float ctrlH = ui::ScaleDpif(34.0f, dpiScale_);
    const float ctrlY = tbY + (tbH - ctrlH) * 0.5f;

    const float segPad = ui::ScaleDpif(3.0f, dpiScale_);
    const float tabWUsb = ui::ScaleDpif(86.0f, dpiScale_);
    const float tabWWifi = ui::ScaleDpif(64.0f, dpiScale_);
    const float segX = rcTransportBox_.X + tbPad;
    rcTabUsb_  = Gdiplus::RectF(segX + segPad, ctrlY + segPad, tabWUsb, ctrlH - segPad * 2.0f);
    rcTabWifi_ = Gdiplus::RectF(rcTabUsb_.GetRight() + ui::ScaleDpif(2.0f, dpiScale_), ctrlY + segPad, tabWWifi, ctrlH - segPad * 2.0f);

    const float setW  = ui::ScaleDpif(94.0f, dpiScale_);
    const float fxW   = ui::ScaleDpif(88.0f, dpiScale_);
    const float gapBtn = ui::ScaleDpif(8.0f, dpiScale_);
    const float setX = rcTabWifi_.GetRight() + segPad + gapBtn;
    rcBtnSettings_ = Gdiplus::RectF(setX, ctrlY, setW, ctrlH);
    rcBtnEffects_  = Gdiplus::RectF(rcBtnSettings_.GetRight() + gapBtn, ctrlY, fxW, ctrlH);

    const float camBtnW = ui::ScaleDpif(144.0f, dpiScale_);
    const float camX = std::max(rcBtnEffects_.GetRight() + gapBtn, rcTransportBox_.GetRight() - tbPad - camBtnW);
    rcBtnCamera_ = Gdiplus::RectF(camX, ctrlY, camBtnW, ctrlH);

    // 3. Status Strip abajo (36px)
    const float stripH = ui::ScaleDpif(36.0f, dpiScale_);
    rcStatusStrip_ = Gdiplus::RectF(pad, std::max(tbY + tbH + gap, h - pad - stripH), rcTransportBox_.Width, stripH);

    // 4. Split Content central
    const float splitY = tbY + tbH + gap;
    const float splitH = std::max(ui::ScaleDpif(140.0f, dpiScale_), rcStatusStrip_.Y - gap - splitY);
    float drawerBaseW = 0.0f;
    if (activeDrawer_ == ActiveDrawer::Settings) drawerBaseW = 304.0f;
    else if (activeDrawer_ == ActiveDrawer::Effects) drawerBaseW = 316.0f;
    const float drawerW = (drawerBaseW > 0.0f) ? ui::ScaleDpif(drawerBaseW, dpiScale_) : 0.0f;
    const float previewW = (drawerW > 0.0f)
        ? std::max(ui::ScaleDpif(220.0f, dpiScale_), rcTransportBox_.Width - drawerW - gap)
        : rcTransportBox_.Width;

    rcPreviewCard_ = Gdiplus::RectF(pad, splitY, previewW, splitH);
    rcDrawerArea_  = Gdiplus::RectF(rcPreviewCard_.GetRight() + gap, splitY, drawerW, splitH);

    const float ecoBtnW = ui::ScaleDpif(176.0f, dpiScale_);
    const float ecoBtnH = ui::ScaleDpif(36.0f, dpiScale_);
    rcEcoBtn_ = Gdiplus::RectF(
        rcPreviewCard_.X + (rcPreviewCard_.Width - ecoBtnW) * 0.5f,
        rcPreviewCard_.Y + rcPreviewCard_.Height * 0.5f + ui::ScaleDpif(48.0f, dpiScale_),
        ecoBtnW, ecoBtnH);
}

void MainWindow::LayoutControls(int width, int height) {
    if (width <= 0 || height <= 0) return;
    ComputeRects(width, height);

    if (previewHost_) {
        const int inset = ui::ScaleDpi(2, dpiScale_);
        const int px = static_cast<int>(rcPreviewCard_.X) + inset;
        const int py = static_cast<int>(rcPreviewCard_.Y) + inset;
        const int pw = std::max(10, static_cast<int>(rcPreviewCard_.Width) - inset * 2);
        const int ph = std::max(10, static_cast<int>(rcPreviewCard_.Height) - inset * 2);
        MoveWindow(previewHost_, px, py, pw, ph, TRUE);
        preview_.Resize(static_cast<uint32_t>(pw), static_cast<uint32_t>(ph));
        ShowWindow(previewHost_, userPreviewWanted_ ? SW_SHOW : SW_HIDE);
    }

    auto placeDrawer = [&](HWND dHwnd, bool visible) {
        if (!dHwnd) return;
        if (visible) {
            const int dx = static_cast<int>(rcDrawerArea_.X);
            const int dy = static_cast<int>(rcDrawerArea_.Y);
            const int dw = std::max(100, static_cast<int>(rcDrawerArea_.Width));
            const int dh = std::max(100, static_cast<int>(rcDrawerArea_.Height));
            MoveWindow(dHwnd, dx, dy, dw, dh, TRUE);
            ShowWindow(dHwnd, SW_SHOW);
        } else {
            ShowWindow(dHwnd, SW_HIDE);
        }
    };
    placeDrawer(settingsDrawer_.Handle(), activeDrawer_ == ActiveDrawer::Settings);
    placeDrawer(effectsDrawer_.Handle(), activeDrawer_ == ActiveDrawer::Effects);

    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::Paint(HDC targetDc) {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) return;

    ComputeRects(width, height);

    ui::DoubleBufferPaint db(targetDc, width, height);
    Gdiplus::Graphics g(db.MemoryDc());
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

    // Lienzo principal #0F1117
    Gdiplus::SolidBrush canvasBrush(ui::kBgCanvas);
    g.FillRectangle(&canvasBrush, 0, 0, width, height);

    // 1. Header Strip #171A21 + divisor #2E3545
    const float pad = ui::ScaleDpif(16.0f, dpiScale_);
    Gdiplus::SolidBrush surfBrush(ui::kBgSurface);
    g.FillRectangle(&surfBrush, 0.0f, 0.0f, static_cast<float>(width), yHeaderLine_);
    Gdiplus::Pen linePen(ui::kBorderSubtle, std::max(1.0f, dpiScale_));
    g.DrawLine(&linePen, 0.0f, yHeaderLine_, static_cast<float>(width), yHeaderLine_);

    const int iconSz = ui::ScaleDpi(20, dpiScale_);
    const int iconY = static_cast<int>((yHeaderLine_ - static_cast<float>(iconSz)) * 0.5f);
    if (headerIcon_) {
        DrawIconEx(db.MemoryDc(), static_cast<int>(pad), iconY, headerIcon_, iconSz, iconSz, 0, nullptr, DI_NORMAL);
    }

    ui::DrawTextUi(g, L"ZeroCam",
                   Gdiplus::RectF(pad + iconSz + ui::ScaleDpif(10.0f, dpiScale_), 0.0f, ui::ScaleDpif(140.0f, dpiScale_), yHeaderLine_),
                   L"Segoe UI", ui::ScaleDpif(13.5f, dpiScale_), true, ui::kTextPrimary);

    // Pill de estado superior derecha (refleja salto automático USB <-> Wi-Fi en vivo)
    const bool wifiMode = cfg_.wifiMode;
    std::wstring topPillLabel;
    Gdiplus::Color topPillDot = ui::kTextMuted;
    if (cameraTransition_ == CameraTransition::Starting) {
        topPillDot = ui::kStatusWarning;
        topPillLabel = L"Iniciando cámara...";
    } else if (cameraTransition_ == CameraTransition::Stopping) {
        topPillDot = ui::kStatusWarning;
        topPillLabel = L"Deteniendo cámara...";
    } else if (cameraTransition_ == CameraTransition::Switching) {
        topPillDot = ui::kStatusWarning;
        topPillLabel = L"Cambiando transporte...";
    } else if (lastTelemetry_.clientConnected) {
        topPillDot = ui::kStatusSuccess;
        const bool isWifiLive = (lastTelemetry_.connectionLabel.find(L"Wi-Fi") != std::wstring::npos);
        topPillLabel = isWifiLive ? L"Conectado • Wi-Fi" : L"Conectado • Cable USB";
    } else if (cameraRunning_) {
        topPillDot = userPreviewWanted_ ? ui::kStatusWarning : ui::kStatusSuccess;
        topPillLabel = (wifiMode ? std::wstring(L"Wi-Fi") : std::wstring(L"USB / Wi-Fi Auto")) +
                       (userPreviewWanted_ ? L" • Esperando móvil" : L" • 0.0% GPU 3D");
    } else {
        topPillLabel = (wifiMode ? std::wstring(L"Wi-Fi") : std::wstring(L"Cable USB")) + L" • En espera";
    }
    const float measuredPillW = std::min(ui::ScaleDpif(260.0f, dpiScale_), ui::MeasurePillBadgeWidth(g, topPillLabel, dpiScale_));
    const Gdiplus::RectF actualTopPill(static_cast<float>(width) - pad - measuredPillW, rcHeaderPill_.Y, measuredPillW, rcHeaderPill_.Height);
    ui::DrawPillBadge(g, actualTopPill, topPillDot, topPillLabel, dpiScale_);

    // 2. Transport Bar (barra minimalista sin Bandeja ni Ocultar vista previa)
    const float radSm = ui::ScaleDpif(8.0f, dpiScale_);
    ui::FillRoundRect(g, rcTransportBox_, radSm, ui::kBgSurface);
    ui::DrawRoundRect(g, rcTransportBox_, radSm, ui::kBorderSubtle, std::max(1.0f, dpiScale_));

    const float segPad = ui::ScaleDpif(3.0f, dpiScale_);
    const Gdiplus::RectF segOuter(
        rcTabUsb_.X - segPad, rcTabUsb_.Y - segPad,
        (rcTabWifi_.GetRight() - rcTabUsb_.X) + segPad * 2.0f,
        rcTabUsb_.Height + segPad * 2.0f);
    ui::FillRoundRect(g, segOuter, radSm, ui::kBgCanvas);
    ui::DrawRoundRect(g, segOuter, radSm, ui::kBorderSubtle, std::max(1.0f, dpiScale_));

    const float tabRad = ui::ScaleDpif(6.0f, dpiScale_);
    const bool activeWifiTab = lastTelemetry_.clientConnected
        ? (lastTelemetry_.connectionLabel.find(L"Wi-Fi") != std::wstring::npos)
        : cfg_.wifiMode;
    if (!activeWifiTab) {
        ui::FillRoundRect(g, rcTabUsb_, tabRad, hoverId_ == kMainHoverTabUsb ? ui::kAccentHover : ui::kAccentPrimary);
    } else if (hoverId_ == kMainHoverTabUsb) {
        ui::FillRoundRect(g, rcTabUsb_, tabRad, ui::kBgInput);
    }
    if (activeWifiTab) {
        ui::FillRoundRect(g, rcTabWifi_, tabRad, hoverId_ == kMainHoverTabWifi ? ui::kAccentHover : ui::kAccentPrimary);
    } else if (hoverId_ == kMainHoverTabWifi) {
        ui::FillRoundRect(g, rcTabWifi_, tabRad, ui::kBgInput);
    }

    ui::DrawTextUi(g, L"Cable USB", rcTabUsb_, L"Segoe UI", ui::ScaleDpif(12.0f, dpiScale_), !activeWifiTab,
                   !activeWifiTab ? Gdiplus::Color(255, 255, 255, 255) : ui::kTextSecondary, Gdiplus::StringAlignmentCenter);
    ui::DrawTextUi(g, L"Wi-Fi", rcTabWifi_, L"Segoe UI", ui::ScaleDpif(12.0f, dpiScale_), activeWifiTab,
                   activeWifiTab ? Gdiplus::Color(255, 255, 255, 255) : ui::kTextSecondary, Gdiplus::StringAlignmentCenter);

    ui::DrawButton(g, rcBtnSettings_, L"Ajustes", ui::ButtonKind::Secondary, ui::IconKind::Gear,
                   hoverId_ == kMainHoverBtnSettings, activeDrawer_ == ActiveDrawer::Settings, dpiScale_);

    ui::DrawButton(g, rcBtnEffects_, L"Filtros", ui::ButtonKind::Secondary, ui::IconKind::Sliders,
                   hoverId_ == kMainHoverBtnEffects, activeDrawer_ == ActiveDrawer::Effects, dpiScale_);

    const wchar_t* camBtnLabel = cameraRunning_ ? L"Detener Cámara" : L"Iniciar Cámara";
    ui::ButtonKind camBtnKind = cameraRunning_ ? ui::ButtonKind::Danger : ui::ButtonKind::Primary;
    if (cameraTransition_ == CameraTransition::Starting) {
        camBtnLabel = L"Iniciando...";
        camBtnKind = ui::ButtonKind::Secondary;
    } else if (cameraTransition_ == CameraTransition::Stopping) {
        camBtnLabel = L"Deteniendo...";
        camBtnKind = ui::ButtonKind::Secondary;
    } else if (cameraTransition_ == CameraTransition::Switching) {
        camBtnLabel = L"Actualizando...";
        camBtnKind = ui::ButtonKind::Secondary;
    }
    ui::DrawButton(g, rcBtnCamera_, camBtnLabel, camBtnKind,
                   ui::IconKind::None,
                   cameraTransition_ == CameraTransition::Idle && hoverId_ == kMainHoverBtnCamera,
                   false, dpiScale_);

    // 3. Preview Card (Estado eco OLED #000000 o marco azul #2D7DFF alrededor de previewHost_)
    const float radMd = ui::ScaleDpif(12.0f, dpiScale_);
    if (!userPreviewWanted_) {
        ui::FillRoundRect(g, rcPreviewCard_, radMd, ui::kBgOled);
        ui::DrawRoundRect(g, rcPreviewCard_, radMd, ui::kBorderSubtle, std::max(1.0f, dpiScale_));

        const float cx = rcPreviewCard_.X + rcPreviewCard_.Width * 0.5f;
        const float cy = rcPreviewCard_.Y + rcPreviewCard_.Height * 0.5f;
        const float boltSz = ui::ScaleDpif(40.0f, dpiScale_);
        ui::DrawBoltIcon(g, Gdiplus::RectF(cx - boltSz * 0.5f, cy - ui::ScaleDpif(80.0f, dpiScale_), boltSz, boltSz), ui::kAccentPrimary);

        ui::DrawTextUi(g, L"Vista previa oculta (0.0% GPU 3D)",
                       Gdiplus::RectF(rcPreviewCard_.X + pad, cy - ui::ScaleDpif(30.0f, dpiScale_), rcPreviewCard_.Width - pad * 2.0f, ui::ScaleDpif(24.0f, dpiScale_)),
                       L"Segoe UI", ui::ScaleDpif(15.0f, dpiScale_), true, ui::kTextPrimary, Gdiplus::StringAlignmentCenter);

        ui::DrawTextUi(g,
                       L"La cámara virtual sigue enviando video a OBS, Zoom o Discord.\nÚsala solo cuando necesites encuadrar.",
                       Gdiplus::RectF(rcPreviewCard_.X + pad, cy - ui::ScaleDpif(2.0f, dpiScale_), rcPreviewCard_.Width - pad * 2.0f, ui::ScaleDpif(40.0f, dpiScale_)),
                       L"Segoe UI", ui::ScaleDpif(12.0f, dpiScale_), false, ui::kTextSecondary, Gdiplus::StringAlignmentCenter, Gdiplus::StringAlignmentNear, true);

        ui::DrawButton(g, rcEcoBtn_, L"Mostrar vista previa", ui::ButtonKind::Secondary, ui::IconKind::Eye,
                       hoverId_ == kMainHoverEcoBtn, false, dpiScale_);
    } else {
        ui::FillRoundRect(g, rcPreviewCard_, radMd, ui::kBgPreview);
        ui::DrawRoundRect(g, rcPreviewCard_, radMd, ui::kBorderAccent, ui::ScaleDpif(1.5f, dpiScale_));
    }

    // 4. StatusStrip inferior
    ui::FillRoundRect(g, rcStatusStrip_, radSm, ui::kBgSurface);
    ui::DrawRoundRect(g, rcStatusStrip_, radSm, ui::kBorderSubtle, std::max(1.0f, dpiScale_));

    Gdiplus::Color footDot = ui::kTextMuted;
    std::wstring leftStatus;
    if (cameraTransition_ != CameraTransition::Idle) {
        footDot = ui::kStatusWarning;
        leftStatus = lastActionMsg_.empty() ? L"Aplicando cambios..." : lastActionMsg_;
    } else if (vcamRunningCached_) {
        footDot = ui::kStatusSuccess;
        leftStatus = lastMfSupported_
            ? L"Cámara Virtual Activa (OBS / Zoom / Discord)"
            : L"Cámara Virtual Activa W10 (DirectShow • OBS / Zoom / Discord)";
    } else if (cameraRunning_) {
        footDot = ui::kStatusWarning;
        leftStatus = lastActionMsg_.empty() ? lastTelemetry_.statusText : lastActionMsg_;
    } else {
        leftStatus = !lastActionMsg_.empty()
            ? lastActionMsg_
            : (lastComReady_ ? L"Cámara Virtual lista ('ZeroCam Virtual Camera')" : L"Listo para iniciar cámara virtual");
    }

    const float dotSz = ui::ScaleDpif(8.0f, dpiScale_);
    const float dotX = rcStatusStrip_.X + ui::ScaleDpif(12.0f, dpiScale_);
    const float dotY = rcStatusStrip_.Y + (rcStatusStrip_.Height - dotSz) * 0.5f;
    Gdiplus::SolidBrush dotBr(footDot);
    g.FillEllipse(&dotBr, dotX, dotY, dotSz, dotSz);

    const uint32_t dispW = lastTelemetry_.width > 0 ? lastTelemetry_.width : cfg_.width;
    const uint32_t dispH = lastTelemetry_.height > 0 ? lastTelemetry_.height : cfg_.height;
    const uint32_t mbps = std::max<uint32_t>(1u, cfg_.bitrateBps / 1000000u);
    const std::wstring routeLabel = lastTelemetry_.running
        ? (lastTelemetry_.zeroCopyActive ? L"Zero-Copy" : L"CPU")
        : (d3dCtx_.Device() ? L"Zero-Copy" : L"Sin D3D11");
    wchar_t zoomTag[32]{};
    if (cfg_.camera.lensZoom > 1.04f) {
        swprintf_s(zoomTag, L"  •  Zoom %.1f×", cfg_.camera.lensZoom);
    }
    wchar_t rightBuf[280]{};
    if (lastTelemetry_.clientConnected && lastTelemetry_.fps > 0.1) {
        swprintf_s(
            rightBuf,
            L"%s  •  %ux%u @ %.0f/%u fps  •  %u Mbps%s  •  %s",
            lastTelemetry_.connectionLabel.c_str(),
            dispW, dispH, lastTelemetry_.fps, cfg_.fps, mbps, zoomTag, routeLabel.c_str());
    } else {
        swprintf_s(
            rightBuf,
            L"%s  •  %ux%u @ %u fps  •  %u Mbps%s  •  %s",
            lastTelemetry_.connectionLabel.c_str(),
            dispW, dispH, cfg_.fps, mbps, zoomTag, routeLabel.c_str());
    }

    const float rightFontPx = ui::ScaleDpif(11.0f, dpiScale_);
    Gdiplus::FontFamily rightFamily(L"Consolas");
    Gdiplus::Font rightFont(&rightFamily, rightFontPx, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::RectF measuredRight;
    g.MeasureString(rightBuf, -1, &rightFont, Gdiplus::PointF(0.0f, 0.0f), &measuredRight);
    const float maxRightW = rcStatusStrip_.Width * 0.72f;
    const float rightReserve = std::min(maxRightW, measuredRight.Width + ui::ScaleDpif(10.0f, dpiScale_));
    const float leftAvail = std::max(80.0f, rcStatusStrip_.Width - rightReserve - ui::ScaleDpif(36.0f, dpiScale_));
    if (vcamRunningCached_ && leftAvail < ui::ScaleDpif(290.0f, dpiScale_)) {
        leftStatus = L"Cámara Virtual Activa";
    }

    ui::DrawTextUi(g, leftStatus,
                   Gdiplus::RectF(dotX + dotSz + ui::ScaleDpif(8.0f, dpiScale_), rcStatusStrip_.Y,
                                  leftAvail, rcStatusStrip_.Height),
                   L"Segoe UI", ui::ScaleDpif(12.0f, dpiScale_), true, ui::kTextPrimary);

    ui::DrawTextUi(g, rightBuf,
                   Gdiplus::RectF(rcStatusStrip_.GetRight() - rightReserve - ui::ScaleDpif(12.0f, dpiScale_), rcStatusStrip_.Y,
                                  rightReserve, rcStatusStrip_.Height),
                   L"Consolas", rightFontPx, false, ui::kTextSecondary, Gdiplus::StringAlignmentFar);
}

void MainWindow::RefreshTelemetry() {
    lastTelemetry_ = ingest_.GetTelemetry();

    if (lastTelemetry_.clientConnected && !testPatternActive_) {
        const bool liveIsWifi = (lastTelemetry_.connectionLabel.find(L"Wi-Fi") != std::wstring::npos);
        const bool liveIsUsb = (lastTelemetry_.connectionLabel.find(L"USB") != std::wstring::npos);
        if ((liveIsWifi && !cfg_.wifiMode) || (liveIsUsb && cfg_.wifiMode)) {
            cfg_.wifiMode = liveIsWifi;
            SaveAppConfig(cfg_);
            settingsDrawer_.SyncFromConfig(cfg_, userPreviewWanted_, ingest_.BoundPort(), ingest_.HostFingerprintDisplay());
        }
    }

    if (previewHost_) {
        auto* st = reinterpret_cast<PreviewPlaceholderState*>(GetWindowLongPtrW(previewHost_, GWLP_USERDATA));
        if (st) {
            const bool nowLive = lastTelemetry_.running && lastTelemetry_.clientConnected && (lastTelemetry_.totalFrames > 0);
            const std::wstring newBadge = lastTelemetry_.running
                ? (L"LIVE  " + std::to_wstring(lastTelemetry_.width > 0 ? lastTelemetry_.width : cfg_.width) +
                   L"×" + std::to_wstring(lastTelemetry_.height > 0 ? lastTelemetry_.height : cfg_.height) + L"  •  " +
                   (lastTelemetry_.statusText.empty() ? L"Esperando móvil" : lastTelemetry_.statusText))
                : L"Vista previa lista • Pulsa Iniciar Cámara";

            const bool transitionToIdle = st->hasLiveVideo && !nowLive;
            st->hasLiveVideo = nowLive;
            st->dpiScale = dpiScale_;
            st->badgeText = newBadge;
            if (!nowLive || transitionToIdle) {
                InvalidateRect(previewHost_, nullptr, FALSE);
            }
        }
    }

    effectsDrawer_.UpdateLiveStats(
        lastTelemetry_.running && lastTelemetry_.clientConnected,
        d3dCtx_.IsAiBlurAvailable(),
        d3dCtx_.IsAiUsingGpu(),
        d3dCtx_.LastAiInferenceMs());

    InvalidateRect(hwnd_, nullptr, FALSE);
}

SourceMode MainWindow::SelectedSourceMode() const noexcept {
    return cfg_.wifiMode ? SourceMode::ZeroCamWifi : SourceMode::ZeroCamUsb;
}

void MainWindow::StartCameraWorker() {
    if (cameraOpThread_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(opMutex_);
        opExit_ = false;
    }
    cameraOpThread_ = std::thread(&MainWindow::CameraWorkerMain, this);
}

void MainWindow::StopCameraWorker() {
    {
        std::lock_guard<std::mutex> lock(opMutex_);
        opExit_ = true;
        while (!opQueue_.empty()) {
            opQueue_.pop();
        }
    }
    opCv_.notify_all();
    if (cameraOpThread_.joinable()) {
        cameraOpThread_.join();
    }
}

void MainWindow::EnqueueCameraOp(CameraOpRequest req) {
    {
        std::lock_guard<std::mutex> lock(opMutex_);
        if (opExit_) return;
        opQueue_.push(std::move(req));
    }
    opCv_.notify_one();
}

void MainWindow::CameraWorkerMain() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    bool workerRunning = false;
    bool workerTestPattern = false;

    for (;;) {
        CameraOpRequest req{};
        {
            std::unique_lock<std::mutex> lock(opMutex_);
            opCv_.wait(lock, [&]() { return opExit_ || !opQueue_.empty(); });
            if (opExit_) break;
            req = std::move(opQueue_.front());
            opQueue_.pop();
        }

        auto* res = new CameraOpResult();
        res->kind = req.kind;
        res->cameraRunning = workerRunning;
        res->vcamActive = vcam_.IsRunning();
        res->testPattern = workerTestPattern;

        switch (req.kind) {
        case CameraOpKind::Start: {
            const bool ready = EnsureVirtualCameraReady();
            std::wstring err;
            const bool vcamStarted = ready && vcam_.Start(err);
            d3dCtx_.SetProducerActive(true);
            const bool ingOk = ingest_.Start(req.mode, req.endpoint, &d3dCtx_, &preview_);
            if (!vcamStarted) {
                if (!ingOk) {
                    d3dCtx_.SetProducerActive(false);
                    workerRunning = false;
                    workerTestPattern = false;
                    res->actionMsg = L"No se pudo iniciar el motor de recepción.";
                } else {
                    workerRunning = true;
                    workerTestPattern = req.testPattern;
                    res->forcePreviewOn = true;
                    res->actionMsg = (ready && !err.empty())
                        ? (L"Error cámara virtual: " + err + L" (Modo Vista Previa activo)")
                        : (!err.empty() ? err : L"Permiso omitido: Modo Receptor + Vista Previa activo.");
                }
            } else if (!ingOk) {
                d3dCtx_.SetProducerActive(false);
                vcam_.Stop();
                workerRunning = false;
                workerTestPattern = false;
                res->actionMsg = L"No se pudo iniciar el motor de recepción.";
            } else {
                workerRunning = true;
                workerTestPattern = req.testPattern;
                res->actionMsg = VirtualCameraHost::IsMfVirtualCameraSupported()
                    ? L"Cámara virtual activa ('ZeroCam Virtual Camera')."
                    : L"Cámara DirectShow activa en Windows 10 ('ZeroCam Virtual Camera').";
            }
            res->cameraRunning = workerRunning;
            res->vcamActive = vcam_.IsRunning();
            res->comReady = lastPlatformSupported_ &&
                            VirtualCameraHost::IsComRegistered() &&
                            VirtualCameraHost::IsInstalledDllUpToDate();
            res->testPattern = workerTestPattern;
            break;
        }
        case CameraOpKind::Stop:
            ingest_.Stop();
            d3dCtx_.SetProducerActive(false);
            vcam_.Stop();
            workerRunning = false;
            workerTestPattern = false;
            res->cameraRunning = false;
            res->vcamActive = false;
            res->testPattern = false;
            res->actionMsg = L"Cámara virtual detenida.";
            break;

        case CameraOpKind::RestartIngest: {
            const bool ingOk = ingest_.Start(req.mode, req.endpoint, &d3dCtx_, &preview_);
            if (!ingOk) {
                vcam_.Stop();
                workerRunning = false;
                workerTestPattern = false;
                res->actionMsg = L"No se pudo reiniciar el motor de recepción.";
            } else {
                workerRunning = true;
                workerTestPattern = req.testPattern;
                res->actionMsg = workerTestPattern
                    ? L"Patrón de prueba activo."
                    : (vcam_.IsRunning()
                        ? L"Cámara virtual activa ('ZeroCam Virtual Camera')."
                        : L"Modo Receptor + Vista Previa activo.");
            }
            res->cameraRunning = workerRunning;
            res->vcamActive = vcam_.IsRunning();
            res->testPattern = workerTestPattern;
            break;
        }
        case CameraOpKind::RegisterVcam: {
            const bool ok = req.forceElevate ? RunElevatedSelfRegister() : EnsureVirtualCameraReady();
            res->comReady = lastPlatformSupported_ &&
                            VirtualCameraHost::IsComRegistered() &&
                            VirtualCameraHost::IsInstalledDllUpToDate();
            if (req.forceElevate) {
                res->actionMsg = ok
                    ? L"Cámara virtual reparada y lista ('ZeroCam Virtual Camera')."
                    : L"No se completó el registro elevado.";
            } else {
                res->actionMsg = ok
                    ? L"Cámara virtual configurada automáticamente ('ZeroCam Virtual Camera')."
                    : L"Permiso de administrador omitido (se usará Modo Vista Previa).";
            }
            break;
        }
        case CameraOpKind::ReconnectUsb: {
            std::wstring adbMsg;
            (void)AdbBridge::SetupUsbTunnel(req.internalPort, ingest_.BoundPort(), adbMsg);
            res->actionMsg = std::move(adbMsg);
            break;
        }
        }

        if (closing_.load(std::memory_order_acquire) || !hwnd_ ||
            !PostMessageW(hwnd_, kWmCameraOpDone, 0, reinterpret_cast<LPARAM>(res))) {
            delete res;
        }
    }

    ingest_.Stop();
    d3dCtx_.SetProducerActive(false);
    AdbBridge::ShutdownBundledAdb();
    vcam_.Stop();
    MFShutdown();
    CoUninitialize();
}

void MainWindow::HandleCameraOpDone(CameraOpResult* rawResult) {
    std::unique_ptr<CameraOpResult> res(rawResult);
    if (!res) return;

    if (res->kind == CameraOpKind::ReconnectUsb) {
        if (!res->actionMsg.empty()) {
            lastActionMsg_ = res->actionMsg;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    cameraTransition_ = CameraTransition::Idle;
    if (res->kind == CameraOpKind::RegisterVcam) {
        lastComReady_ = res->comReady;
        lastActionMsg_ = res->actionMsg;
        RefreshTelemetry();
        return;
    }

    cameraRunning_ = res->cameraRunning;
    vcamRunningCached_ = res->vcamActive;
    if (res->kind == CameraOpKind::Start) {
        lastComReady_ = res->comReady;
    }
    testPatternActive_ = res->testPattern;
    lastActionMsg_ = res->actionMsg;
    if (res->forcePreviewOn) {
        SetPreviewWanted(true);
    }
    settingsDrawer_.SyncFromConfig(cfg_, userPreviewWanted_, ingest_.BoundPort(), ingest_.HostFingerprintDisplay());
    RefreshTelemetry();
}

void MainWindow::ToggleCamera() {
    if (cameraTransition_ != CameraTransition::Idle) return;

    if (cameraRunning_) {
        cameraTransition_ = CameraTransition::Stopping;
        lastActionMsg_ = L"Deteniendo cámara virtual...";
        InvalidateRect(hwnd_, nullptr, FALSE);
        CameraOpRequest req{};
        req.kind = CameraOpKind::Stop;
        EnqueueCameraOp(std::move(req));
        return;
    }

    cameraTransition_ = CameraTransition::Starting;
    lastActionMsg_ = L"Iniciando cámara virtual...";
    InvalidateRect(hwnd_, nullptr, FALSE);

    CameraOpRequest req{};
    req.kind = CameraOpKind::Start;
    req.mode = testPatternActive_ ? SourceMode::TestPattern : SelectedSourceMode();
    req.endpoint = testPatternActive_ ? L"" : std::to_wstring(cfg_.internalPort);
    req.testPattern = testPatternActive_;
    EnqueueCameraOp(std::move(req));
}

void MainWindow::SetPreviewWanted(bool wanted) {
    if (userPreviewWanted_ == wanted) return;
    userPreviewWanted_ = wanted;
    preview_.SetEnabled(userPreviewWanted_ && IsWindowVisible(hwnd_));
    if (previewHost_) {
        ShowWindow(previewHost_, userPreviewWanted_ ? SW_SHOW : SW_HIDE);
        if (userPreviewWanted_) InvalidateRect(previewHost_, nullptr, FALSE);
    }
    settingsDrawer_.SyncFromConfig(cfg_, userPreviewWanted_, ingest_.BoundPort(), ingest_.HostFingerprintDisplay());
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::SetWifiMode(bool wifi) {
    if (cameraTransition_ != CameraTransition::Idle) return;
    if (cfg_.wifiMode == wifi && !testPatternActive_) return;
    testPatternActive_ = false;
    cfg_.wifiMode = wifi;
    SaveAppConfig(cfg_);
    if (cameraRunning_) {
        cameraTransition_ = CameraTransition::Switching;
        lastActionMsg_ = wifi ? L"Cambiando transporte a Wi-Fi..." : L"Cambiando transporte a Cable USB...";
        CameraOpRequest req{};
        req.kind = CameraOpKind::RestartIngest;
        req.mode = SelectedSourceMode();
        req.endpoint = std::to_wstring(cfg_.internalPort);
        req.testPattern = false;
        EnqueueCameraOp(std::move(req));
    }
    settingsDrawer_.SyncFromConfig(cfg_, userPreviewWanted_, ingest_.BoundPort(), ingest_.HostFingerprintDisplay());
    RefreshTelemetry();
}

void MainWindow::ToggleSettingsDrawer() {
    activeDrawer_ = (activeDrawer_ == ActiveDrawer::Settings) ? ActiveDrawer::None : ActiveDrawer::Settings;
    if (activeDrawer_ == ActiveDrawer::Settings) {
        settingsDrawer_.SyncFromConfig(cfg_, userPreviewWanted_, ingest_.BoundPort(), ingest_.HostFingerprintDisplay());
    } else {
        settingsDrawer_.ResetPinVisibility();
    }
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    LayoutControls(rc.right - rc.left, rc.bottom - rc.top);
}

void MainWindow::ToggleEffectsDrawer() {
    if (activeDrawer_ == ActiveDrawer::Settings) {
        settingsDrawer_.ResetPinVisibility();
    }
    activeDrawer_ = (activeDrawer_ == ActiveDrawer::Effects) ? ActiveDrawer::None : ActiveDrawer::Effects;
    if (activeDrawer_ == ActiveDrawer::Effects) {
        effectsDrawer_.SyncFromConfig(cfg_.effects, cfg_.camera);
    }
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    LayoutControls(rc.right - rc.left, rc.bottom - rc.top);
}

void MainWindow::AdjustPreviewZoom(short wheelDelta) {
    if (wheelDelta == 0) return;
    const float steps = static_cast<float>(wheelDelta) / 120.0f;
    float nextZoom = std::clamp(cfg_.camera.lensZoom + steps * 0.1f, 1.0f, 3.0f);
    nextZoom = std::round(nextZoom * 10.0f) * 0.1f;
    nextZoom = std::clamp(nextZoom, 1.0f, 3.0f);
    if (std::fabs(nextZoom - cfg_.camera.lensZoom) < 0.01f) return;

    cfg_.camera.lensZoom = nextZoom;
    SaveAppConfig(cfg_);
    ingest_.SetCameraControl(cfg_.camera, cameraRunning_);
    effectsDrawer_.SyncFromConfig(cfg_.effects, cfg_.camera);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::ResetPreviewZoom() {
    if (cfg_.camera.lensZoom <= 1.04f) return;
    cfg_.camera.lensZoom = 1.0f;
    SaveAppConfig(cfg_);
    ingest_.SetCameraControl(cfg_.camera, cameraRunning_);
    effectsDrawer_.SyncFromConfig(cfg_.effects, cfg_.camera);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::ApplyConfigToIngest(bool pushIfConnected, bool portChanged) {
    d3dCtx_.SetEffectsConfig(cfg_.effects);
    ingest_.SetRotationOverride(cfg_.rotationOverride);
    ingest_.SetWifiPin(WideToUtf8(cfg_.wifiPin));
    ingest_.SetVideoConfig(cfg_.width, cfg_.height, cfg_.fps, cfg_.bitrateBps, pushIfConnected);
    ingest_.SetCameraControl(cfg_.camera, pushIfConnected);

    if (portChanged && cameraRunning_ && !testPatternActive_ && cameraTransition_ == CameraTransition::Idle) {
        cameraTransition_ = CameraTransition::Switching;
        lastActionMsg_ = L"Aplicando nuevo puerto TCP...";
        CameraOpRequest req{};
        req.kind = CameraOpKind::RestartIngest;
        req.mode = SelectedSourceMode();
        req.endpoint = std::to_wstring(cfg_.internalPort);
        req.testPattern = false;
        EnqueueCameraOp(std::move(req));
    }
    settingsDrawer_.SyncFromConfig(cfg_, userPreviewWanted_, ingest_.BoundPort(), ingest_.HostFingerprintDisplay());
    effectsDrawer_.SyncFromConfig(cfg_.effects, cfg_.camera);
}

bool MainWindow::RunElevatedSelfRegister() {
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = nullptr;
    sei.lpVerb = L"runas";
    sei.lpFile = exePath;
    sei.lpParameters = L"--register-vcam";
    sei.nShow = SW_HIDE;

    if (!ShellExecuteExW(&sei)) return false;

    DWORD exitCode = 1;
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 20000);
        GetExitCodeProcess(sei.hProcess, &exitCode);
        CloseHandle(sei.hProcess);
    }
    return exitCode == 0 &&
           VirtualCameraHost::IsComRegistered() &&
           VirtualCameraHost::IsInstalledDllUpToDate();
}

bool MainWindow::EnsureVirtualCameraReady() {
    if (!VirtualCameraHost::IsPlatformSupported()) return false;
    if (VirtualCameraHost::IsComRegistered() && VirtualCameraHost::IsInstalledDllUpToDate()) {
        return true;
    }
    return RunElevatedSelfRegister();
}

void MainWindow::AddTrayIcon() {
    if (trayAdded_) return;
    nid_.cbSize = sizeof(nid_);
    nid_.hWnd = hwnd_;
    nid_.uID = 1;
    nid_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid_.uCallbackMessage = kWmTrayCallback;
    nid_.hIcon = LoadAppIcon(hInstance_, true);
    wcscpy_s(nid_.szTip, L"ZeroCam — Cámara Virtual");
    trayAdded_ = (Shell_NotifyIconW(NIM_ADD, &nid_) == TRUE);
}

void MainWindow::RemoveTrayIcon() {
    if (!trayAdded_) return;
    Shell_NotifyIconW(NIM_DELETE, &nid_);
    trayAdded_ = false;
}

void MainWindow::MinimizeToTray() {
    preview_.SetEnabled(false);
    ShowWindow(hwnd_, SW_HIDE);
}

void MainWindow::RestoreFromTray() {
    ShowWindow(hwnd_, SW_SHOW);
    if (IsIconic(hwnd_)) {
        ShowWindow(hwnd_, SW_RESTORE);
    }
    SetForegroundWindow(hwnd_);
    preview_.SetEnabled(userPreviewWanted_);
    if (previewHost_) InvalidateRect(previewHost_, nullptr, FALSE);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void MainWindow::ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kIdTrayShow, L"Mostrar ZeroCam");
    AppendMenuW(menu, MF_STRING, kIdTrayTogglePrev, userPreviewWanted_ ? L"Desactivar Vista Previa" : L"Activar Vista Previa");
    AppendMenuW(menu, MF_STRING, kIdTrayRepair, L"Reparar / Re-registrar Cámara (Admin)");
    AppendMenuW(menu, MF_STRING | (testPatternActive_ ? MF_CHECKED : 0), kIdTrayTestPattern, L"Patrón de Prueba (Diagnóstico)");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kIdTrayExit, L"Detener y Salir");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
}

LRESULT MainWindow::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        mmi->ptMinTrackSize.x = ui::ScaleDpi(760, dpiScale_);
        mmi->ptMinTrackSize.y = ui::ScaleDpi(520, dpiScale_);
        return 0;
    }

    case WM_DPICHANGED: {
        const UINT newDpi = HIWORD(wParam);
        dpiScale_ = (newDpi > 0) ? (static_cast<float>(newDpi) / 96.0f) : 1.0f;
        settingsDrawer_.SetDpiScale(dpiScale_);
        effectsDrawer_.SetDpiScale(dpiScale_);
        const auto* suggested = reinterpret_cast<const RECT*>(lParam);
        if (suggested) {
            SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    }

    case WM_SYSCOMMAND:
        if ((wParam & 0xFFF0) == SC_MINIMIZE && trayAdded_) {
            MinimizeToTray();
            return 0;
        }
        break;

    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) {
            preview_.SetEnabled(false);
        } else {
            LayoutControls(LOWORD(lParam), HIWORD(lParam));
            if (userPreviewWanted_ && IsWindowVisible(hwnd_)) {
                preview_.SetEnabled(true);
            }
        }
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd_, &ps);
        Paint(dc);
        EndPaint(hwnd_, &ps);
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (!trackingLeave_) {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd_, 0 };
            TrackMouseEvent(&tme);
            trackingLeave_ = true;
        }
        const int x = static_cast<short>(LOWORD(lParam));
        const int y = static_cast<short>(HIWORD(lParam));
        int nextHover = kMainHoverNone;
        if (ui::HitTest(rcTabUsb_, x, y)) nextHover = kMainHoverTabUsb;
        else if (ui::HitTest(rcTabWifi_, x, y)) nextHover = kMainHoverTabWifi;
        else if (ui::HitTest(rcBtnSettings_, x, y)) nextHover = kMainHoverBtnSettings;
        else if (ui::HitTest(rcBtnEffects_, x, y)) nextHover = kMainHoverBtnEffects;
        else if (ui::HitTest(rcBtnCamera_, x, y)) nextHover = kMainHoverBtnCamera;
        else if (!userPreviewWanted_ && ui::HitTest(rcEcoBtn_, x, y)) nextHover = kMainHoverEcoBtn;

        if (nextHover != hoverId_) {
            hoverId_ = nextHover;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        trackingLeave_ = false;
        if (hoverId_ != kMainHoverNone) {
            hoverId_ = kMainHoverNone;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && hoverId_ != kMainHoverNone) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;

    case WM_LBUTTONDOWN: {
        SetFocus(hwnd_);
        const int x = static_cast<short>(LOWORD(lParam));
        const int y = static_cast<short>(HIWORD(lParam));
        if (ui::HitTest(rcTabUsb_, x, y)) SetWifiMode(false);
        else if (ui::HitTest(rcTabWifi_, x, y)) SetWifiMode(true);
        else if (ui::HitTest(rcBtnSettings_, x, y)) ToggleSettingsDrawer();
        else if (ui::HitTest(rcBtnEffects_, x, y)) ToggleEffectsDrawer();
        else if (ui::HitTest(rcBtnCamera_, x, y)) ToggleCamera();
        else if (!userPreviewWanted_ && ui::HitTest(rcEcoBtn_, x, y)) SetPreviewWanted(true);
        return 0;
    }

    case WM_MOUSEWHEEL: {
        if (userPreviewWanted_ && previewHost_ && IsWindowVisible(previewHost_)) {
            POINT pt{ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
            RECT prevRc{};
            GetWindowRect(previewHost_, &prevRc);
            if (PtInRect(&prevRc, pt)) {
                AdjustPreviewZoom(GET_WHEEL_DELTA_WPARAM(wParam));
                return 0;
            }
        }
        break;
    }

    case kWmPreviewFocusClick: {
        if (cameraRunning_ && lastTelemetry_.clientConnected) {
            const int px = static_cast<short>(LOWORD(lParam));
            const int py = static_cast<short>(HIWORD(lParam));
            float nx = 0.5f;
            float ny = 0.5f;
            if (preview_.MapClientPointToSensorNorm(px, py, nx, ny)) {
                preview_.ShowFocusFeedback(px, py);
                ingest_.SendFocusPoint(nx, ny);
            }
            float du = 0.5f;
            float dv = 0.45f;
            if (preview_.MapClientPointToDisplayUv(px, py, du, dv)) {
                cfg_.effects.focusX = du;
                cfg_.effects.focusY = dv;
                d3dCtx_.SetEffectsConfig(cfg_.effects);
                SaveAppConfig(cfg_);
                effectsDrawer_.SyncFromConfig(cfg_.effects, cfg_.camera);
            }
        }
        return 0;
    }

    case kWmPreviewWheelZoom:
        AdjustPreviewZoom(GET_WHEEL_DELTA_WPARAM(wParam));
        return 0;

    case kWmPreviewZoomReset:
        ResetPreviewZoom();
        return 0;

    case WM_TIMER:
        if (wParam == kTimerTelemetryId) {
            RefreshTelemetry();
        }
        return 0;

    case kWmAutoRegister:
        if (!autoRegAttempted_ && cameraTransition_ == CameraTransition::Idle) {
            autoRegAttempted_ = true;
            cameraTransition_ = CameraTransition::Registering;
            lastActionMsg_ = L"Configurando cámara virtual automáticamente...";
            InvalidateRect(hwnd_, nullptr, FALSE);
            CameraOpRequest req{};
            req.kind = CameraOpKind::RegisterVcam;
            req.forceElevate = false;
            EnqueueCameraOp(std::move(req));
        }
        return 0;

    case kWmCameraOpDone:
        HandleCameraOpDone(reinterpret_cast<CameraOpResult*>(lParam));
        return 0;

    case WM_DEVICECHANGE:
        if ((wParam == DBT_DEVNODES_CHANGED || wParam == DBT_DEVICEARRIVAL) &&
            cameraRunning_ && !testPatternActive_) {
            ingest_.TriggerUsbReconnect();
        }
        return 0;

    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        if (id == kIdTrayShow) RestoreFromTray();
        else if (id == kIdTrayRepair) {
            if (cameraTransition_ == CameraTransition::Idle) {
                cameraTransition_ = CameraTransition::Registering;
                lastActionMsg_ = L"Reparando registro de cámara virtual...";
                InvalidateRect(hwnd_, nullptr, FALSE);
                CameraOpRequest req{};
                req.kind = CameraOpKind::RegisterVcam;
                req.forceElevate = true;
                EnqueueCameraOp(std::move(req));
            }
        } else if (id == kIdTrayTestPattern) {
            if (cameraTransition_ != CameraTransition::Idle) {
                return 0;
            }
            if (testPatternActive_) {
                if (cameraRunning_) {
                    cameraTransition_ = CameraTransition::Switching;
                    lastActionMsg_ = L"Cambiando a cámara móvil...";
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    CameraOpRequest req{};
                    req.kind = CameraOpKind::RestartIngest;
                    req.mode = SelectedSourceMode();
                    req.endpoint = std::to_wstring(cfg_.internalPort);
                    req.testPattern = false;
                    EnqueueCameraOp(std::move(req));
                } else {
                    testPatternActive_ = false;
                    RefreshTelemetry();
                }
            } else {
                testPatternActive_ = true;
                if (!cameraRunning_) {
                    ToggleCamera();
                } else {
                    cameraTransition_ = CameraTransition::Switching;
                    lastActionMsg_ = L"Activando patrón de prueba...";
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    CameraOpRequest req{};
                    req.kind = CameraOpKind::RestartIngest;
                    req.mode = SourceMode::TestPattern;
                    req.endpoint = L"";
                    req.testPattern = true;
                    EnqueueCameraOp(std::move(req));
                }
            }
        } else if (id == kIdTrayTogglePrev) {
            SetPreviewWanted(!userPreviewWanted_);
        } else if (id == kIdTrayExit) {
            DestroyWindow(hwnd_);
        }
        return 0;
    }

    case kWmTrayCallback:
        if (lParam == WM_LBUTTONUP || lParam == WM_LBUTTONDBLCLK) RestoreFromTray();
        else if (lParam == WM_RBUTTONUP) ShowTrayMenu();
        return 0;

    case WM_CLOSE:
        if ((cameraRunning_ || cameraTransition_ == CameraTransition::Starting) && cfg_.minimizeToTrayOnClose) {
            MinimizeToTray();
            return 0;
        }
        DestroyWindow(hwnd_);
        return 0;

    case WM_DESTROY: {
        closing_.store(true, std::memory_order_release);
        MSG pending{};
        while (PeekMessageW(&pending, hwnd_, kWmCameraOpDone, kWmCameraOpDone, PM_REMOVE)) {
            delete reinterpret_cast<CameraOpResult*>(pending.lParam);
        }
        KillTimer(hwnd_, kTimerTelemetryId);
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

} // namespace zerocam
