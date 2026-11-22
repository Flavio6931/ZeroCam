#include "SettingsDrawer.h"
#include <commctrl.h>
#include <algorithm>

namespace zerocam {

namespace {

enum DrawerCtrlId : int {
    kIdEditPin  = 401
};

enum HoverTarget : int {
    kHoverNone = 0,
    kHoverClose,
    kHoverDropRes,
    kHoverDropFps,
    kHoverDropBitrate,
    kHoverDropRot,
    kHoverSwitchPrev,
    kHoverSwitchTray,
    kHoverPinToggle,
    kHoverBtnUsb
};

std::wstring TrimWide(std::wstring s) {
    while (!s.empty() && (s.front() == L' ' || s.front() == L'\t')) s.erase(s.begin());
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t' || s.back() == L'\r' || s.back() == L'\n')) s.pop_back();
    return s;
}

std::wstring FormatResLabel(uint32_t w) {
    if (w == 640) return L"480p SD";
    if (w == 1920) return L"1080p FHD";
    return L"720p HD";
}

std::wstring FormatFpsLabel(uint32_t fps) {
    return std::to_wstring(fps) + L" FPS";
}

std::wstring FormatQualityLabel(uint32_t bitrate) {
    if (bitrate <= 2000000) return L"Baja 2 Mbps";
    if (bitrate >= 12000000) return L"Ultra 12 Mbps";
    if (bitrate >= 8000000) return L"Alta 8 Mbps";
    return L"Media 4 Mbps";
}

std::wstring FormatRotLabel(int rotOverride) {
    switch (rotOverride) {
    case 0: return L"Rot: 0°";
    case 1: return L"Rot: 90°";
    case 2: return L"Rot: 180°";
    case 3: return L"Rot: 270°";
    default: return L"Rot: Auto";
    }
}

} // namespace

SettingsDrawer::~SettingsDrawer() {
    if (monoFont_) DeleteObject(monoFont_);
    if (inputBrush_) DeleteObject(inputBrush_);
}

bool SettingsDrawer::Create(HWND parent, HINSTANCE hInstance, float dpiScale, SettingsDrawerCallbacks callbacks) {
    hInstance_ = hInstance;
    dpiScale_ = dpiScale;
    callbacks_ = std::move(callbacks);
    inputBrush_ = CreateSolidBrush(ui::kRefBgInput);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &SettingsDrawer::WndProc;
    wc.hInstance = hInstance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"ZeroCamSettingsDrawerClass";
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(
        0,
        wc.lpszClassName,
        L"",
        WS_CHILD | WS_CLIPCHILDREN,
        0, 0, ui::ScaleDpi(304, dpiScale_), 400,
        parent, nullptr, hInstance_, this);
    if (!hwnd_) return false;

    editPin_ = CreateWindowExW(
        0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_CENTER | ES_PASSWORD,
        0, 0, 60, 20, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdEditPin)), hInstance_, nullptr);
    SendMessageW(editPin_, EM_SETLIMITTEXT, 32, 0);
    SendMessageW(editPin_, EM_SETPASSWORDCHAR, static_cast<WPARAM>(L'\u2022'), 0);

    SetWindowSubclass(editPin_, &SettingsDrawer::EditSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));
    UpdateFontSizes();
    return true;
}

void SettingsDrawer::ResetPinVisibility() {
    if (!pinVisible_) return;
    pinVisible_ = false;
    if (editPin_) {
        SendMessageW(editPin_, EM_SETPASSWORDCHAR, static_cast<WPARAM>(L'\u2022'), 0);
        InvalidateRect(editPin_, nullptr, TRUE);
    }
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsDrawer::UpdateFontSizes() {
    if (monoFont_) DeleteObject(monoFont_);
    monoFont_ = CreateFontW(
        -ui::ScaleDpi(13, dpiScale_), 0, 0, 0, FW_MEDIUM, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    if (editPin_) SendMessageW(editPin_, WM_SETFONT, reinterpret_cast<WPARAM>(monoFont_), TRUE);
}

void SettingsDrawer::SetDpiScale(float dpiScale) {
    dpiScale_ = dpiScale;
    UpdateFontSizes();
    if (hwnd_) {
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        LayoutControls(rc.right - rc.left, rc.bottom - rc.top);
    }
}

void SettingsDrawer::SyncFromConfig(const AppConfig& cfg, bool previewOn, uint16_t activeBoundPort, const std::wstring& hostFpDisplay) {
    cfg_ = cfg;
    previewOn_ = previewOn;
    boundPort_ = activeBoundPort;
    if (!hostFpDisplay.empty()) {
        hostFpDisplay_ = hostFpDisplay;
    }

    syncingEdits_ = true;
    if (editPin_) SetWindowTextW(editPin_, cfg_.wifiPin.c_str());
    syncingEdits_ = false;

    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsDrawer::ComputeRects(int width, int height) {
    const float w = static_cast<float>(width);
    const float pad = ui::ScaleDpif(14.0f, dpiScale_);
    const float innerW = std::max(120.0f, w - pad * 2.0f);
    const float gap = ui::ScaleDpif(8.0f, dpiScale_);

    // Header
    const float headerY = pad;
    const float closeSz = ui::ScaleDpif(24.0f, dpiScale_);
    rcClose_ = Gdiplus::RectF(w - pad - closeSz, headerY - ui::ScaleDpif(2.0f, dpiScale_), closeSz, closeSz);
    yHeaderBottom_ = headerY + ui::ScaleDpif(28.0f, dpiScale_);

    // Sección 1: CÁMARA Y VIDEO (Grid 2x2)
    float y = yHeaderBottom_ + ui::ScaleDpif(20.0f, dpiScale_);
    const float colW = (innerW - gap) * 0.5f;
    const float dropH = ui::ScaleDpif(32.0f, dpiScale_);
    rcDropRes_     = Gdiplus::RectF(pad, y, colW, dropH);
    rcDropFps_     = Gdiplus::RectF(pad + colW + gap, y, colW, dropH);
    y += dropH + gap;
    rcDropBitrate_ = Gdiplus::RectF(pad, y, colW, dropH);
    rcDropRot_     = Gdiplus::RectF(pad + colW + gap, y, colW, dropH);
    y += dropH + ui::ScaleDpif(12.0f, dpiScale_);

    yDiv1_ = y;
    y += ui::ScaleDpif(12.0f, dpiScale_) + ui::ScaleDpif(20.0f, dpiScale_);

    // Sección 2: RENDIMIENTO (2 switches)
    const float swW = ui::ScaleDpif(44.0f, dpiScale_);
    const float swH = ui::ScaleDpif(24.0f, dpiScale_);
    rcSwitchPrev_ = Gdiplus::RectF(w - pad - swW, y, swW, swH);
    y += ui::ScaleDpif(32.0f, dpiScale_);
    rcSwitchTray_ = Gdiplus::RectF(w - pad - swW, y, swW, swH);
    y += ui::ScaleDpif(32.0f, dpiScale_);

    yDiv2_ = y;
    y += ui::ScaleDpif(12.0f, dpiScale_) + ui::ScaleDpif(20.0f, dpiScale_);

    // Sección 3: RED Y SEGURIDAD (PIN Wi-Fi enmascarado + Huella criptográfica FP)
    const float boxW = ui::ScaleDpif(86.0f, dpiScale_);
    const float boxH = ui::ScaleDpif(30.0f, dpiScale_);
    rcPinBox_ = Gdiplus::RectF(w - pad - boxW, y, boxW, boxH);
    const float badgeW = ui::ScaleDpif(78.0f, dpiScale_);
    rcPinBadge_ = Gdiplus::RectF(rcPinBox_.X - gap - badgeW, y + ui::ScaleDpif(3.0f, dpiScale_), badgeW, ui::ScaleDpif(24.0f, dpiScale_));
    y += boxH + ui::ScaleDpif(10.0f, dpiScale_);

    const float fpH = ui::ScaleDpif(26.0f, dpiScale_);
    rcFpRow_ = Gdiplus::RectF(pad, y, innerW, fpH);
    y += fpH + ui::ScaleDpif(10.0f, dpiScale_);

    const float btnH = ui::ScaleDpif(34.0f, dpiScale_);
    rcBtnUsb_ = Gdiplus::RectF(pad, y, innerW, btnH);
    yStatus_ = std::min(static_cast<float>(height) - ui::ScaleDpif(20.0f, dpiScale_), rcBtnUsb_.GetBottom() + ui::ScaleDpif(6.0f, dpiScale_));
}

void SettingsDrawer::LayoutControls(int width, int height) {
    ComputeRects(width, height);
    if (editPin_) {
        const int px = static_cast<int>(rcPinBox_.X + ui::ScaleDpif(6.0f, dpiScale_));
        const int pw = std::max(20, static_cast<int>(rcPinBox_.Width - ui::ScaleDpif(12.0f, dpiScale_)));
        const int eh = ui::ScaleDpi(18, dpiScale_);
        const int py = static_cast<int>(rcPinBox_.Y + (rcPinBox_.Height - eh) * 0.5f);
        MoveWindow(editPin_, px, py, pw, eh, TRUE);
    }
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsDrawer::Paint(HDC targetDc) {
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

    // Fondo canvas alrededor del drawer redondeado
    Gdiplus::SolidBrush canvasBrush(ui::kBgCanvas);
    g.FillRectangle(&canvasBrush, 0, 0, width, height);

    const float pad = ui::ScaleDpif(14.0f, dpiScale_);
    const float w = static_cast<float>(width);
    const float radius = ui::ScaleDpif(12.0f, dpiScale_);
    const Gdiplus::RectF cardRc(0.5f, 0.5f, w - 1.5f, static_cast<float>(height) - 1.5f);
    ui::FillRoundRect(g, cardRc, radius, ui::kBgSurface);
    ui::DrawRoundRect(g, cardRc, radius, ui::kBorderAccent, std::max(1.0f, dpiScale_));

    // Header
    ui::DrawTextUi(g, L"Ajustes", Gdiplus::RectF(pad, pad, w - pad * 3.0f, ui::ScaleDpif(20.0f, dpiScale_)),
                   L"Segoe UI", ui::ScaleDpif(15.0f, dpiScale_), true, ui::kTextPrimary);
    ui::DrawCloseIcon(g, rcClose_, ui::kTextSecondary, hoverId_ == kHoverClose, dpiScale_);

    // Sección 1
    ui::DrawTextUi(g, L"CÁMARA Y VIDEO", Gdiplus::RectF(pad, yHeaderBottom_, w - pad * 2.0f, ui::ScaleDpif(16.0f, dpiScale_)),
                   L"Segoe UI", ui::ScaleDpif(10.0f, dpiScale_), true, ui::kTextSecondary);
    ui::DrawDropdownBox(g, rcDropRes_, FormatResLabel(cfg_.width), hoverId_ == kHoverDropRes, dpiScale_);
    ui::DrawDropdownBox(g, rcDropFps_, FormatFpsLabel(cfg_.fps), hoverId_ == kHoverDropFps, dpiScale_);
    ui::DrawDropdownBox(g, rcDropBitrate_, FormatQualityLabel(cfg_.bitrateBps), hoverId_ == kHoverDropBitrate, dpiScale_);
    ui::DrawDropdownBox(g, rcDropRot_, FormatRotLabel(cfg_.rotationOverride), hoverId_ == kHoverDropRot, dpiScale_);

    Gdiplus::Pen divPen(ui::kBorderSubtle, std::max(1.0f, dpiScale_));
    g.DrawLine(&divPen, pad, yDiv1_, w - pad, yDiv1_);

    // Sección 2: RENDIMIENTO
    ui::DrawTextUi(g, L"RENDIMIENTO", Gdiplus::RectF(pad, yDiv1_ + ui::ScaleDpif(12.0f, dpiScale_), w - pad * 2.0f, ui::ScaleDpif(16.0f, dpiScale_)),
                   L"Segoe UI", ui::ScaleDpif(10.0f, dpiScale_), true, ui::kTextSecondary);

    ui::DrawTextUi(g, L"Vista previa activa",
                   Gdiplus::RectF(pad, rcSwitchPrev_.Y, rcSwitchPrev_.X - pad - ui::ScaleDpif(8.0f, dpiScale_), rcSwitchPrev_.Height),
                   L"Segoe UI", ui::ScaleDpif(12.5f, dpiScale_), false, ui::kTextPrimary);
    ui::DrawToggleSwitch(g, rcSwitchPrev_, previewOn_, hoverId_ == kHoverSwitchPrev, dpiScale_);

    ui::DrawTextUi(g, L"Minimizar a bandeja al cerrar",
                   Gdiplus::RectF(pad, rcSwitchTray_.Y, rcSwitchTray_.X - pad - ui::ScaleDpif(8.0f, dpiScale_), rcSwitchTray_.Height),
                   L"Segoe UI", ui::ScaleDpif(12.5f, dpiScale_), false, ui::kTextPrimary);
    ui::DrawToggleSwitch(g, rcSwitchTray_, cfg_.minimizeToTrayOnClose, hoverId_ == kHoverSwitchTray, dpiScale_);

    g.DrawLine(&divPen, pad, yDiv2_, w - pad, yDiv2_);

    // Sección 3: RED Y SEGURIDAD
    ui::DrawTextUi(g, L"RED Y SEGURIDAD (WIRE V2)", Gdiplus::RectF(pad, yDiv2_ + ui::ScaleDpif(10.0f, dpiScale_), w - pad * 2.0f, ui::ScaleDpif(16.0f, dpiScale_)),
                   L"Segoe UI", ui::ScaleDpif(10.0f, dpiScale_), true, ui::kTextSecondary);

    const std::wstring pinText = pinVisible_ ? L"Ocultar" : L"Ver PIN";
    const float pillGap = ui::ScaleDpif(8.0f, dpiScale_);
    const float pillW = std::max(ui::ScaleDpif(78.0f, dpiScale_), ui::MeasurePillBadgeWidth(g, pinText, dpiScale_));
    rcPinBadge_ = Gdiplus::RectF(rcPinBox_.X - pillGap - pillW, rcPinBadge_.Y, pillW, rcPinBadge_.Height);

    ui::DrawTextUi(g, L"PIN Wi-Fi",
                   Gdiplus::RectF(pad, rcPinBox_.Y, std::max(40.0f, rcPinBadge_.X - pad - ui::ScaleDpif(6.0f, dpiScale_)), rcPinBox_.Height),
                   L"Segoe UI", ui::ScaleDpif(12.5f, dpiScale_), false, ui::kTextSecondary);
    ui::DrawPillBadge(
        g,
        rcPinBadge_,
        hoverId_ == kHoverPinToggle ? ui::kAccentPrimary : (pinVisible_ ? ui::kStatusWarning : ui::kStatusSuccess),
        pinText,
        dpiScale_);

    const float boxRad = ui::ScaleDpif(8.0f, dpiScale_);
    ui::FillRoundRect(g, rcPinBox_, boxRad, ui::kBgInput);
    ui::DrawRoundRect(g, rcPinBox_, boxRad, ui::kBorderSubtle, std::max(1.0f, dpiScale_));

    // Huella criptográfica ECDSA P-256 del PC para verificación visual al emparejar por Wi-Fi
    ui::FillRoundRect(g, rcFpRow_, ui::ScaleDpif(6.0f, dpiScale_), ui::kBgCanvas);
    ui::DrawRoundRect(g, rcFpRow_, ui::ScaleDpif(6.0f, dpiScale_), ui::kBorderSubtle, std::max(1.0f, dpiScale_));
    ui::DrawTextUi(g, L"Huella FP:",
                   Gdiplus::RectF(rcFpRow_.X + ui::ScaleDpif(8.0f, dpiScale_), rcFpRow_.Y, ui::ScaleDpif(70.0f, dpiScale_), rcFpRow_.Height),
                   L"Segoe UI", ui::ScaleDpif(11.0f, dpiScale_), false, ui::kTextMuted);
    ui::DrawTextUi(g, hostFpDisplay_.empty() ? L"----" : hostFpDisplay_,
                   Gdiplus::RectF(rcFpRow_.X + ui::ScaleDpif(74.0f, dpiScale_), rcFpRow_.Y, rcFpRow_.Width - ui::ScaleDpif(82.0f, dpiScale_), rcFpRow_.Height),
                   L"Consolas", ui::ScaleDpif(11.0f, dpiScale_), true, ui::kTextPrimary, Gdiplus::StringAlignmentFar);

    ui::DrawButton(g, rcBtnUsb_, L"Reconectar Cable USB (ADB)", ui::ButtonKind::Secondary, ui::IconKind::None,
                   hoverId_ == kHoverBtnUsb, false, dpiScale_);

    if (!statusNote_.empty()) {
        ui::DrawTextUi(g, statusNote_, Gdiplus::RectF(pad, yStatus_, w - pad * 2.0f, ui::ScaleDpif(18.0f, dpiScale_)),
                       L"Consolas", ui::ScaleDpif(11.0f, dpiScale_), false, ui::kTextMuted);
    }
}

void SettingsDrawer::ShowChoiceMenu(int menuIndex, const Gdiplus::RectF& anchorRc) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    if (menuIndex == 0) {
        AppendMenuW(menu, MF_STRING | (cfg_.width == 640 ? MF_CHECKED : 0), 1001, L"640×480 (SD)");
        AppendMenuW(menu, MF_STRING | (cfg_.width == 1280 ? MF_CHECKED : 0), 1002, L"1280×720 (HD)");
        AppendMenuW(menu, MF_STRING | (cfg_.width == 1920 ? MF_CHECKED : 0), 1003, L"1920×1080 (Full HD)");
    } else if (menuIndex == 1) {
        AppendMenuW(menu, MF_STRING | (cfg_.fps == 15 ? MF_CHECKED : 0), 1011, L"15 FPS");
        AppendMenuW(menu, MF_STRING | (cfg_.fps == 24 ? MF_CHECKED : 0), 1012, L"24 FPS");
        AppendMenuW(menu, MF_STRING | (cfg_.fps == 30 ? MF_CHECKED : 0), 1013, L"30 FPS");
        AppendMenuW(menu, MF_STRING | (cfg_.fps == 60 ? MF_CHECKED : 0), 1014, L"60 FPS");
    } else if (menuIndex == 2) {
        AppendMenuW(menu, MF_STRING | (cfg_.bitrateBps <= 2000000 ? MF_CHECKED : 0), 1021, L"Baja (2 Mbps)");
        AppendMenuW(menu, MF_STRING | (cfg_.bitrateBps == 4000000 ? MF_CHECKED : 0), 1022, L"Media (4 Mbps)");
        AppendMenuW(menu, MF_STRING | (cfg_.bitrateBps == 8000000 ? MF_CHECKED : 0), 1023, L"Alta (8 Mbps)");
        AppendMenuW(menu, MF_STRING | (cfg_.bitrateBps >= 12000000 ? MF_CHECKED : 0), 1024, L"Ultra (12 Mbps)");
    } else {
        AppendMenuW(menu, MF_STRING | (cfg_.rotationOverride < 0 ? MF_CHECKED : 0), 1030, L"Automática (sensor)");
        AppendMenuW(menu, MF_STRING | (cfg_.rotationOverride == 0 ? MF_CHECKED : 0), 1031, L"Fija 0°");
        AppendMenuW(menu, MF_STRING | (cfg_.rotationOverride == 1 ? MF_CHECKED : 0), 1032, L"Fija 90°");
        AppendMenuW(menu, MF_STRING | (cfg_.rotationOverride == 2 ? MF_CHECKED : 0), 1033, L"Fija 180°");
        AppendMenuW(menu, MF_STRING | (cfg_.rotationOverride == 3 ? MF_CHECKED : 0), 1034, L"Fija 270°");
    }

    POINT pt{ static_cast<LONG>(anchorRc.X), static_cast<LONG>(anchorRc.GetBottom() + 2.0f) };
    ClientToScreen(hwnd_, &pt);
    SetForegroundWindow(hwnd_);
    const UINT cmd = TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
    if (cmd == 0) return;

    switch (cmd) {
    case 1001: cfg_.width = 640; cfg_.height = 480; break;
    case 1002: cfg_.width = 1280; cfg_.height = 720; break;
    case 1003: cfg_.width = 1920; cfg_.height = 1080; break;
    case 1011: cfg_.fps = 15; break;
    case 1012: cfg_.fps = 24; break;
    case 1013: cfg_.fps = 30; break;
    case 1014: cfg_.fps = 60; break;
    case 1021: cfg_.bitrateBps = 2000000; break;
    case 1022: cfg_.bitrateBps = 4000000; break;
    case 1023: cfg_.bitrateBps = 8000000; break;
    case 1024: cfg_.bitrateBps = 12000000; break;
    case 1030: cfg_.rotationOverride = -1; break;
    case 1031: cfg_.rotationOverride = 0; break;
    case 1032: cfg_.rotationOverride = 1; break;
    case 1033: cfg_.rotationOverride = 2; break;
    case 1034: cfg_.rotationOverride = 3; break;
    default: return;
    }

    if (callbacks_.onConfigApplied) callbacks_.onConfigApplied(cfg_, false);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsDrawer::CommitPinEdit(bool notifyParent) {
    if (!editPin_ || syncingEdits_) return;
    wchar_t buf[64]{};
    GetWindowTextW(editPin_, buf, 64);
    const std::wstring cleaned = TrimWide(buf);
    SecureZeroMemory(buf, sizeof(buf));
    if (cleaned.size() < 4u || cleaned.size() > 32u) {
        statusNote_ = L"PIN Wi-Fi obligatorio (4 a 32 caracteres).";
        syncingEdits_ = true;
        SetWindowTextW(editPin_, cfg_.wifiPin.c_str());
        syncingEdits_ = false;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (cleaned != cfg_.wifiPin) {
        cfg_.wifiPin = cleaned;
        statusNote_ = L"PIN Wi-Fi guardado cifrado (DPAPI).";
        InvalidateRect(hwnd_, nullptr, FALSE);
        if (notifyParent && callbacks_.onConfigApplied) {
            callbacks_.onConfigApplied(cfg_, false);
        }
    }
}

LRESULT CALLBACK SettingsDrawer::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    SettingsDrawer* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<SettingsDrawer*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<SettingsDrawer*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return self ? self->HandleMessage(msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK SettingsDrawer::EditSubclassProc(
    HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    auto* self = reinterpret_cast<SettingsDrawer*>(dwRefData);
    if (msg == WM_KEYDOWN && wParam == VK_RETURN && self) {
        self->CommitPinEdit(true);
        SetFocus(self->hwnd_);
        return 0;
    }
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, &SettingsDrawer::EditSubclassProc, uIdSubclass);
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

LRESULT SettingsDrawer::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_SIZE:
        LayoutControls(LOWORD(lParam), HIWORD(lParam));
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd_, &ps);
        Paint(dc);
        EndPaint(hwnd_, &ps);
        return 0;
    }

    case WM_CTLCOLOREDIT: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdc, ui::kRefTextPri);
        SetBkColor(hdc, ui::kRefBgInput);
        return reinterpret_cast<LRESULT>(inputBrush_);
    }

    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        const int code = HIWORD(wParam);
        if (id == kIdEditPin && code == EN_KILLFOCUS) {
            CommitPinEdit(true);
        }
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
        int nextHover = kHoverNone;
        if (ui::HitTest(rcClose_, x, y)) nextHover = kHoverClose;
        else if (ui::HitTest(rcDropRes_, x, y)) nextHover = kHoverDropRes;
        else if (ui::HitTest(rcDropFps_, x, y)) nextHover = kHoverDropFps;
        else if (ui::HitTest(rcDropBitrate_, x, y)) nextHover = kHoverDropBitrate;
        else if (ui::HitTest(rcDropRot_, x, y)) nextHover = kHoverDropRot;
        else if (ui::HitTest(rcSwitchPrev_, x, y)) nextHover = kHoverSwitchPrev;
        else if (ui::HitTest(rcSwitchTray_, x, y)) nextHover = kHoverSwitchTray;
        else if (ui::HitTest(rcPinBadge_, x, y)) nextHover = kHoverPinToggle;
        else if (ui::HitTest(rcBtnUsb_, x, y)) nextHover = kHoverBtnUsb;

        if (nextHover != hoverId_) {
            hoverId_ = nextHover;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        trackingLeave_ = false;
        if (hoverId_ != kHoverNone) {
            hoverId_ = kHoverNone;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && hoverId_ != kHoverNone) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;

    case WM_LBUTTONDOWN: {
        SetFocus(hwnd_);
        const int x = static_cast<short>(LOWORD(lParam));
        const int y = static_cast<short>(HIWORD(lParam));
        if (ui::HitTest(rcClose_, x, y)) {
            if (callbacks_.onCloseRequested) callbacks_.onCloseRequested();
        } else if (ui::HitTest(rcDropRes_, x, y)) {
            ShowChoiceMenu(0, rcDropRes_);
        } else if (ui::HitTest(rcDropFps_, x, y)) {
            ShowChoiceMenu(1, rcDropFps_);
        } else if (ui::HitTest(rcDropBitrate_, x, y)) {
            ShowChoiceMenu(2, rcDropBitrate_);
        } else if (ui::HitTest(rcDropRot_, x, y)) {
            ShowChoiceMenu(3, rcDropRot_);
        } else if (ui::HitTest(rcSwitchPrev_, x, y)) {
            previewOn_ = !previewOn_;
            InvalidateRect(hwnd_, nullptr, FALSE);
            if (callbacks_.onPreviewSwitchChanged) callbacks_.onPreviewSwitchChanged(previewOn_);
        } else if (ui::HitTest(rcSwitchTray_, x, y)) {
            cfg_.minimizeToTrayOnClose = !cfg_.minimizeToTrayOnClose;
            InvalidateRect(hwnd_, nullptr, FALSE);
            if (callbacks_.onConfigApplied) callbacks_.onConfigApplied(cfg_, false);
        } else if (ui::HitTest(rcPinBadge_, x, y) && editPin_) {
            pinVisible_ = !pinVisible_;
            SendMessageW(editPin_, EM_SETPASSWORDCHAR, pinVisible_ ? 0 : static_cast<WPARAM>(L'\u2022'), 0);
            InvalidateRect(editPin_, nullptr, TRUE);
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (ui::HitTest(rcPinBox_, x, y) && editPin_) {
            SetFocus(editPin_);
        } else if (ui::HitTest(rcBtnUsb_, x, y)) {
            if (callbacks_.onReconnectUsb) {
                std::wstring msg;
                callbacks_.onReconnectUsb(msg);
                statusNote_ = msg;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
        return 0;
    }
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

} // namespace zerocam
