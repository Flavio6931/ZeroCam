#include "EffectsDrawer.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace zerocam {

namespace {

enum FxHoverId : int {
    kFxHoverNone = 0,
    kFxHoverClose,
    kFxHoverReset,
    kFxHoverPreset0,
    kFxHoverPreset1,
    kFxHoverPreset2,
    kFxHoverPreset3,
    kFxHoverDropBlur,
    kFxHoverDropEngine,
    kFxHoverSlider0,
    kFxHoverSlider1,
    kFxHoverSlider2,
    kFxHoverSlider3,
    kFxHoverSlider4,
    kFxHoverSwitchMirror,
    kFxHoverDropFocus,
    kFxHoverBtnLens,
    kFxHoverDropEv,
    kFxHoverBtnTorch,
    kFxHoverSwitchPreferFps
};

std::wstring FormatBlurLabel(int blurMode) {
    switch (blurMode) {
    case 1: return L"Suave";
    case 2: return L"Medio";
    case 3: return L"Fuerte";
    default: return L"Apagado";
    }
}

std::wstring FormatPercentSigned(float v, float scale) {
    const int p = static_cast<int>(std::lround(v * scale));
    wchar_t buf[32]{};
    swprintf_s(buf, L"%+d%%", p);
    return buf;
}

std::wstring FormatPercentUnsigned(float v) {
    const int p = static_cast<int>(std::lround(v * 100.0f));
    return std::to_wstring(p) + L"%";
}

std::wstring FormatEvLabel(int ev) {
    wchar_t buf[32]{};
    swprintf_s(buf, L"Exposición: %+d EV", ev);
    return buf;
}

} // namespace

bool EffectsDrawer::Create(HWND parent, HINSTANCE hInstance, float dpiScale, EffectsDrawerCallbacks callbacks) {
    hInstance_ = hInstance;
    dpiScale_ = dpiScale;
    callbacks_ = std::move(callbacks);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &EffectsDrawer::WndProc;
    wc.hInstance = hInstance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"ZeroCamEffectsDrawerClass";
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(
        0,
        wc.lpszClassName,
        L"",
        WS_CHILD | WS_CLIPCHILDREN,
        0, 0, ui::ScaleDpi(316, dpiScale_), 400,
        parent, nullptr, hInstance_, this);
    return hwnd_ != nullptr;
}

void EffectsDrawer::SyncFromConfig(const VideoEffectsConfig& fx, const CameraRemoteConfig& cam) {
    fx_ = fx;
    ClampEffectsConfig(fx_);
    cam_ = cam;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void EffectsDrawer::UpdateLiveStats(bool streamConnected, bool aiAvailable, bool aiUsingGpu, double aiMs) {
    const bool changed = (streamConnected_ != streamConnected) || (aiAvailable_ != aiAvailable) ||
                         (aiUsingGpu_ != aiUsingGpu) || (std::fabs(aiMs_ - aiMs) > 0.2);
    streamConnected_ = streamConnected;
    aiAvailable_ = aiAvailable;
    aiUsingGpu_ = aiUsingGpu;
    aiMs_ = aiMs;
    if (changed && hwnd_ && IsWindowVisible(hwnd_)) {
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void EffectsDrawer::SetDpiScale(float dpiScale) {
    dpiScale_ = dpiScale;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void EffectsDrawer::ComputeRects(int width, int height) {
    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);
    const float pad = ui::ScaleDpif(14.0f, dpiScale_);
    const float innerW = std::max(140.0f, w - pad * 2.0f);
    const float gap = ui::ScaleDpif(8.0f, dpiScale_);
    const float s = -scrollY_;

    const float headerY = pad + s;
    const float closeSz = ui::ScaleDpif(24.0f, dpiScale_);
    rcClose_ = Gdiplus::RectF(w - pad - closeSz, headerY - ui::ScaleDpif(2.0f, dpiScale_), closeSz, closeSz);
    const float rstW = ui::ScaleDpif(78.0f, dpiScale_);
    rcBtnReset_ = Gdiplus::RectF(rcClose_.X - gap - rstW, headerY - ui::ScaleDpif(2.0f, dpiScale_), rstW, closeSz);

    // 1. Presets rápidos (2x2)
    float y = headerY + ui::ScaleDpif(50.0f, dpiScale_);
    const float colW = (innerW - gap) * 0.5f;
    const float btnH = ui::ScaleDpif(30.0f, dpiScale_);
    rcPresets_[0] = Gdiplus::RectF(pad, y, colW, btnH);
    rcPresets_[1] = Gdiplus::RectF(pad + colW + gap, y, colW, btnH);
    y += btnH + gap;
    rcPresets_[2] = Gdiplus::RectF(pad, y, colW, btnH);
    rcPresets_[3] = Gdiplus::RectF(pad + colW + gap, y, colW, btnH);
    y += btnH + ui::ScaleDpif(34.0f, dpiScale_);

    // 2. Fondo (IA Bokeh)
    const float dropH = ui::ScaleDpif(32.0f, dpiScale_);
    rcDropBlur_       = Gdiplus::RectF(pad, y, colW, dropH);
    rcDropBlurEngine_ = Gdiplus::RectF(pad + colW + gap, y, colW, dropH);
    y += dropH + ui::ScaleDpif(48.0f, dpiScale_);

    // 3. Sliders de mejora de imagen (5) + Switch espejo
    const float sliderRowH = ui::ScaleDpif(38.0f, dpiScale_);
    const float sliderTrackH = ui::ScaleDpif(18.0f, dpiScale_);
    for (int i = 0; i < 5; ++i) {
        rcSliders_[i] = Gdiplus::RectF(pad, y + ui::ScaleDpif(18.0f, dpiScale_), innerW, sliderTrackH);
        y += sliderRowH;
    }

    const float swW = ui::ScaleDpif(44.0f, dpiScale_);
    const float swH = ui::ScaleDpif(24.0f, dpiScale_);
    y += ui::ScaleDpif(4.0f, dpiScale_);
    rcSwitchMirror_ = Gdiplus::RectF(w - pad - swW, y, swW, swH);
    y += swH + ui::ScaleDpif(34.0f, dpiScale_);

    // 4. Enfoque y Cámara Remota
    rcDropFocusMode_ = Gdiplus::RectF(pad, y, innerW, dropH);
    y += dropH + ui::ScaleDpif(24.0f, dpiScale_);

    const float ctrlH = ui::ScaleDpif(32.0f, dpiScale_);
    rcBtnLens_  = Gdiplus::RectF(pad, y, colW, ctrlH);
    rcBtnTorch_ = Gdiplus::RectF(pad + colW + gap, y, colW, ctrlH);
    y += ctrlH + gap;
    rcDropEv_   = Gdiplus::RectF(pad, y, innerW, ctrlH);
    y += ctrlH + ui::ScaleDpif(10.0f, dpiScale_);

    rcSwitchPreferFps_ = Gdiplus::RectF(w - pad - swW, y, swW, swH);
    y += swH + pad;

    const float contentBottom = y - s;
    maxScroll_ = std::max(0.0f, contentBottom - h);
    if (scrollY_ > maxScroll_) {
        scrollY_ = maxScroll_;
    }
}

void EffectsDrawer::Paint(HDC targetDc) {
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

    Gdiplus::SolidBrush canvasBr(ui::kBgCanvas);
    g.FillRectangle(&canvasBr, 0, 0, width, height);

    const float w = static_cast<float>(width);
    const float h = static_cast<float>(height);
    const float pad = ui::ScaleDpif(14.0f, dpiScale_);
    const float radius = ui::ScaleDpif(12.0f, dpiScale_);
    const Gdiplus::RectF cardRc(0.5f, 0.5f, w - 1.5f, h - 1.5f);
    ui::FillRoundRect(g, cardRc, radius, ui::kBgSurface);

    const Gdiplus::RectF clipRc(ui::ScaleDpif(2.0f, dpiScale_), ui::ScaleDpif(4.0f, dpiScale_),
                                w - ui::ScaleDpif(4.0f, dpiScale_), h - ui::ScaleDpif(8.0f, dpiScale_));
    g.SetClip(clipRc);

    const float s = -scrollY_;
    const float headerY = pad + s;
    ui::DrawTextUi(g, L"Filtros y Cámara", Gdiplus::RectF(pad, headerY, w - pad * 3.0f, ui::ScaleDpif(20.0f, dpiScale_)),
                   L"Segoe UI", ui::ScaleDpif(15.0f, dpiScale_), true, ui::kTextPrimary);
    ui::DrawButton(g, rcBtnReset_, L"Reiniciar", ui::ButtonKind::Secondary, ui::IconKind::None,
                   hoverId_ == kFxHoverReset, false, dpiScale_);
    ui::DrawCloseIcon(g, rcClose_, ui::kTextSecondary, hoverId_ == kFxHoverClose, dpiScale_);

    // 1. Presets rápidos
    ui::DrawTextUi(g, L"PRESETS DE IMAGEN", Gdiplus::RectF(pad, rcPresets_[0].Y - ui::ScaleDpif(20.0f, dpiScale_), w - pad * 2.0f, ui::ScaleDpif(16.0f, dpiScale_)),
                   L"Segoe UI", ui::ScaleDpif(10.0f, dpiScale_), true, ui::kTextSecondary);
    const wchar_t* presetNames[4] = { L"Natural", L"Estudio", L"Luz Baja", L"Cálido" };
    for (int i = 0; i < 4; ++i) {
        ui::DrawButton(g, rcPresets_[i], presetNames[i], ui::ButtonKind::Secondary, ui::IconKind::None,
                       hoverId_ == (kFxHoverPreset0 + i), fx_.preset == i, dpiScale_);
    }

    // 2. Desenfoque de fondo (Retrato IA RVM 512x288 u Óptico f/1.8)
    ui::DrawTextUi(g, L"DESENFOQUE DE FONDO", Gdiplus::RectF(pad, rcDropBlur_.Y - ui::ScaleDpif(20.0f, dpiScale_), w - pad * 2.0f, ui::ScaleDpif(16.0f, dpiScale_)),
                   L"Segoe UI", ui::ScaleDpif(10.0f, dpiScale_), true, ui::kTextSecondary);
    ui::DrawDropdownBox(g, rcDropBlur_, FormatBlurLabel(fx_.blurMode), hoverId_ == kFxHoverDropBlur, dpiScale_);
    const std::wstring engineLabel = (fx_.blurStyle == 0)
        ? L"Óptico (Lente)"
        : (fx_.blurUseGpu ? L"Retrato IA (GPU)" : L"Retrato IA (CPU)");
    ui::DrawDropdownBox(g, rcDropBlurEngine_, engineLabel, hoverId_ == kFxHoverDropEngine, dpiScale_);

    wchar_t aiNote[96]{};
    if (fx_.blurMode == 0) {
        wcscpy_s(aiNote, L"Desactivado (0.0% carga extra en tu PC)");
    } else if (fx_.blurStyle == 0) {
        wcscpy_s(aiNote, L"Óptico f/1.8 (0% IA • clic para enfocar)");
    } else if (!aiAvailable_) {
        wcscpy_s(aiNote, L"Cargando RVM Alpha Matting (512×288)...");
    } else if (aiMs_ > 0.05) {
        const wchar_t* backend = aiUsingGpu_
            ? L"GPU DirectML"
            : (fx_.blurUseGpu ? L"CPU (respaldo)" : L"CPU");
        swprintf_s(aiNote, L"RVM Matting 512×288 (%s • %.1f ms)", backend, aiMs_);
    } else {
        swprintf_s(aiNote, L"RVM Matting listo (%s)", fx_.blurUseGpu ? L"GPU DirectML" : L"CPU 0% juegos");
    }
    ui::DrawTextUi(g, aiNote, Gdiplus::RectF(pad, rcDropBlur_.GetBottom() + ui::ScaleDpif(4.0f, dpiScale_), w - pad * 2.0f, ui::ScaleDpif(16.0f, dpiScale_)),
                   L"Consolas", ui::ScaleDpif(10.0f, dpiScale_), false, fx_.blurMode > 0 ? ui::kAccentPrimary : ui::kTextMuted);

    // 3. Mejora de imagen (Sliders)
    ui::DrawTextUi(g, L"MEJORA DE IMAGEN", Gdiplus::RectF(pad, rcSliders_[0].Y - ui::ScaleDpif(36.0f, dpiScale_), w - pad * 2.0f, ui::ScaleDpif(16.0f, dpiScale_)),
                   L"Segoe UI", ui::ScaleDpif(10.0f, dpiScale_), true, ui::kTextSecondary);

    struct SliderMeta {
        const wchar_t* label;
        float norm;
        std::wstring valStr;
    } metas[5] = {
        { L"Nitidez",     fx_.sharpness,                             FormatPercentUnsigned(fx_.sharpness) },
        { L"Brillo",      (fx_.brightness + 0.30f) / 0.60f,         FormatPercentSigned(fx_.brightness, 100.0f) },
        { L"Contraste",   (fx_.contrast - 0.70f) / 0.70f,           FormatPercentSigned(fx_.contrast - 1.0f, 100.0f) },
        { L"Saturación",  (fx_.saturation - 0.50f) / 1.00f,         FormatPercentSigned(fx_.saturation - 1.0f, 100.0f) },
        { L"Temperatura", (fx_.warmth + 0.30f) / 0.60f,             FormatPercentSigned(fx_.warmth, 100.0f) }
    };

    for (int i = 0; i < 5; ++i) {
        const float lblY = rcSliders_[i].Y - ui::ScaleDpif(18.0f, dpiScale_);
        const float lblH = ui::ScaleDpif(16.0f, dpiScale_);
        ui::DrawTextUi(g, metas[i].label, Gdiplus::RectF(pad, lblY, ui::ScaleDpif(150.0f, dpiScale_), lblH),
                       L"Segoe UI", ui::ScaleDpif(11.5f, dpiScale_), false, ui::kTextPrimary);
        ui::DrawTextUi(g, metas[i].valStr, Gdiplus::RectF(w - pad - ui::ScaleDpif(90.0f, dpiScale_), lblY, ui::ScaleDpif(90.0f, dpiScale_), lblH),
                       L"Consolas", ui::ScaleDpif(11.0f, dpiScale_), false, ui::kTextSecondary, Gdiplus::StringAlignmentFar);
        ui::DrawSliderControl(g, rcSliders_[i], metas[i].norm, hoverId_ == (kFxHoverSlider0 + i), dragSlider_ == i, dpiScale_);
    }

    ui::DrawTextUi(g, L"Espejo horizontal",
                   Gdiplus::RectF(pad, rcSwitchMirror_.Y, rcSwitchMirror_.X - pad - ui::ScaleDpif(8.0f, dpiScale_), rcSwitchMirror_.Height),
                   L"Segoe UI", ui::ScaleDpif(12.0f, dpiScale_), false, ui::kTextPrimary);
    ui::DrawToggleSwitch(g, rcSwitchMirror_, fx_.mirrorH, hoverId_ == kFxHoverSwitchMirror, dpiScale_);

    // 4. Enfoque y Cámara Remota
    ui::DrawTextUi(g, L"ENFOQUE Y CÁMARA MÓVIL", Gdiplus::RectF(pad, rcDropFocusMode_.Y - ui::ScaleDpif(20.0f, dpiScale_), w - pad * 2.0f, ui::ScaleDpif(16.0f, dpiScale_)),
                   L"Segoe UI", ui::ScaleDpif(10.0f, dpiScale_), true, ui::kTextSecondary);
    ui::DrawDropdownBox(g, rcDropFocusMode_,
                        cam_.focusMode == 1 ? L"Enfoque: Bloqueado (AF Lock)" : L"Enfoque: Rostro automático",
                        hoverId_ == kFxHoverDropFocus, dpiScale_);
    ui::DrawTextUi(g, L"Clic en visor: enfocar • Rueda: zoom (clic central: 1.0×)",
                   Gdiplus::RectF(pad, rcDropFocusMode_.GetBottom() + ui::ScaleDpif(3.0f, dpiScale_), w - pad * 2.0f, ui::ScaleDpif(15.0f, dpiScale_)),
                   L"Segoe UI", ui::ScaleDpif(10.0f, dpiScale_), false, ui::kTextMuted);

    ui::DrawButton(g, rcBtnLens_, cam_.frontCamera ? L"Lente: Frontal" : L"Lente: Trasera",
                   ui::ButtonKind::Secondary, ui::IconKind::None, hoverId_ == kFxHoverBtnLens, cam_.frontCamera, dpiScale_);
    ui::DrawButton(g, rcBtnTorch_, cam_.torchEnabled ? L"Linterna: On" : L"Linterna: Off",
                   ui::ButtonKind::Secondary, ui::IconKind::None, hoverId_ == kFxHoverBtnTorch, cam_.torchEnabled, dpiScale_);
    ui::DrawDropdownBox(g, rcDropEv_, FormatEvLabel(cam_.exposureEv), hoverId_ == kFxHoverDropEv, dpiScale_);

    ui::DrawTextUi(g, L"Priorizar FPS estables (interior)",
                   Gdiplus::RectF(pad, rcSwitchPreferFps_.Y, rcSwitchPreferFps_.X - pad - ui::ScaleDpif(8.0f, dpiScale_), rcSwitchPreferFps_.Height),
                   L"Segoe UI", ui::ScaleDpif(12.0f, dpiScale_), false, ui::kTextPrimary);
    ui::DrawToggleSwitch(g, rcSwitchPreferFps_, cam_.preferFps, hoverId_ == kFxHoverSwitchPreferFps, dpiScale_);

    g.ResetClip();
    ui::DrawRoundRect(g, cardRc, radius, ui::kBorderAccent, std::max(1.0f, dpiScale_));
}

void EffectsDrawer::ShowChoiceMenu(int menuKind, const Gdiplus::RectF& anchorRc) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    if (menuKind == 0) {
        AppendMenuW(menu, MF_STRING | (fx_.blurMode == 0 ? MF_CHECKED : 0), 2000, L"Apagado (0% extra)");
        AppendMenuW(menu, MF_STRING | (fx_.blurMode == 1 ? MF_CHECKED : 0), 2001, L"Desenfoque Suave");
        AppendMenuW(menu, MF_STRING | (fx_.blurMode == 2 ? MF_CHECKED : 0), 2002, L"Desenfoque Medio");
        AppendMenuW(menu, MF_STRING | (fx_.blurMode == 3 ? MF_CHECKED : 0), 2003, L"Desenfoque Fuerte");
    } else if (menuKind == 1) {
        AppendMenuW(menu, MF_STRING | (fx_.blurStyle == 1 && fx_.blurUseGpu ? MF_CHECKED : 0), 2012, L"Retrato IA RVM — GPU DirectML (recomendado)");
        AppendMenuW(menu, MF_STRING | (fx_.blurStyle == 1 && !fx_.blurUseGpu ? MF_CHECKED : 0), 2011, L"Retrato IA RVM — CPU (0% GPU juegos)");
        AppendMenuW(menu, MF_STRING | (fx_.blurStyle == 0 ? MF_CHECKED : 0), 2010, L"Óptico — Lente f/1.8 (0% IA)");
    } else if (menuKind == 2) {
        AppendMenuW(menu, MF_STRING | (cam_.focusMode == 0 ? MF_CHECKED : 0), 2020, L"Rostro automático (Face-Priority AF)");
        AppendMenuW(menu, MF_STRING | (cam_.focusMode == 1 ? MF_CHECKED : 0), 2021, L"Bloqueado (AF Lock)");
    } else {
        for (int ev = -2; ev <= 2; ++ev) {
            wchar_t lbl[32]{};
            swprintf_s(lbl, L"%+d EV", ev);
            AppendMenuW(menu, MF_STRING | (cam_.exposureEv == ev ? MF_CHECKED : 0), static_cast<UINT_PTR>(2042 + ev), lbl);
        }
    }

    POINT pt{ static_cast<LONG>(anchorRc.X), static_cast<LONG>(anchorRc.GetBottom() + 2.0f) };
    ClientToScreen(hwnd_, &pt);
    SetForegroundWindow(hwnd_);
    const UINT cmd = TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
    if (cmd == 0) return;

    if (cmd >= 2000 && cmd <= 2003) {
        fx_.blurMode = static_cast<int>(cmd - 2000);
        if (callbacks_.onEffectsChanged) callbacks_.onEffectsChanged(fx_, true);
    } else if (cmd == 2010) {
        fx_.blurStyle = 0;
        if (callbacks_.onEffectsChanged) callbacks_.onEffectsChanged(fx_, true);
    } else if (cmd == 2011 || cmd == 2012) {
        fx_.blurStyle = 1;
        fx_.blurUseGpu = (cmd == 2012);
        if (callbacks_.onEffectsChanged) callbacks_.onEffectsChanged(fx_, true);
    } else if (cmd == 2020 || cmd == 2021) {
        cam_.focusMode = (cmd == 2021) ? 1 : 0;
        if (callbacks_.onCameraChanged) callbacks_.onCameraChanged(cam_);
    } else if (cmd >= 2040 && cmd <= 2044) {
        cam_.exposureEv = static_cast<int>(cmd) - 2042;
        if (callbacks_.onCameraChanged) callbacks_.onCameraChanged(cam_);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void EffectsDrawer::UpdateSliderFromX(int sliderIdx, int clientX, bool persistNow) {
    if (sliderIdx < 0 || sliderIdx >= 5) return;
    const Gdiplus::RectF& rc = rcSliders_[sliderIdx];
    const float t = std::clamp((static_cast<float>(clientX) - rc.X) / std::max(1.0f, rc.Width), 0.0f, 1.0f);
    switch (sliderIdx) {
    case 0: fx_.sharpness = t; break;
    case 1: fx_.brightness = -0.30f + t * 0.60f; break;
    case 2: fx_.contrast = 0.70f + t * 0.70f; break;
    case 3: fx_.saturation = 0.50f + t * 1.00f; break;
    case 4: fx_.warmth = -0.30f + t * 0.60f; break;
    }
    ClampEffectsConfig(fx_);
    if (callbacks_.onEffectsChanged) callbacks_.onEffectsChanged(fx_, persistNow);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

LRESULT CALLBACK EffectsDrawer::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    EffectsDrawer* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<EffectsDrawer*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<EffectsDrawer*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return self ? self->HandleMessage(msg, wParam, lParam) : DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT EffectsDrawer::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd_, &ps);
        Paint(dc);
        EndPaint(hwnd_, &ps);
        return 0;
    }

    case WM_MOUSEWHEEL: {
        POINT pt{ static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)) };
        RECT myRc{};
        GetWindowRect(hwnd_, &myRc);
        if (!PtInRect(&myRc, pt) && GetParent(hwnd_)) {
            return SendMessageW(GetParent(hwnd_), WM_MOUSEWHEEL, wParam, lParam);
        }
        const short delta = GET_WHEEL_DELTA_WPARAM(wParam);
        scrollY_ = std::clamp(scrollY_ - (static_cast<float>(delta) / 120.0f) * ui::ScaleDpif(36.0f, dpiScale_), 0.0f, maxScroll_);
        InvalidateRect(hwnd_, nullptr, FALSE);
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
        if (dragSlider_ >= 0) {
            UpdateSliderFromX(dragSlider_, x, false);
            return 0;
        }

        int nextHover = kFxHoverNone;
        if (ui::HitTest(rcClose_, x, y)) nextHover = kFxHoverClose;
        else if (ui::HitTest(rcBtnReset_, x, y)) nextHover = kFxHoverReset;
        else if (ui::HitTest(rcDropBlur_, x, y)) nextHover = kFxHoverDropBlur;
        else if (ui::HitTest(rcDropBlurEngine_, x, y)) nextHover = kFxHoverDropEngine;
        else if (ui::HitTest(rcSwitchMirror_, x, y)) nextHover = kFxHoverSwitchMirror;
        else if (ui::HitTest(rcDropFocusMode_, x, y)) nextHover = kFxHoverDropFocus;
        else if (ui::HitTest(rcBtnLens_, x, y)) nextHover = kFxHoverBtnLens;
        else if (ui::HitTest(rcDropEv_, x, y)) nextHover = kFxHoverDropEv;
        else if (ui::HitTest(rcBtnTorch_, x, y)) nextHover = kFxHoverBtnTorch;
        else if (ui::HitTest(rcSwitchPreferFps_, x, y)) nextHover = kFxHoverSwitchPreferFps;
        else {
            for (int i = 0; i < 4; ++i) {
                if (ui::HitTest(rcPresets_[i], x, y)) { nextHover = kFxHoverPreset0 + i; break; }
            }
            if (nextHover == kFxHoverNone) {
                for (int i = 0; i < 5; ++i) {
                    if (ui::HitTest(rcSliders_[i], x, y)) { nextHover = kFxHoverSlider0 + i; break; }
                }
            }
        }
        if (nextHover != hoverId_) {
            hoverId_ = nextHover;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        trackingLeave_ = false;
        if (hoverId_ != kFxHoverNone) {
            hoverId_ = kFxHoverNone;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && (hoverId_ != kFxHoverNone || dragSlider_ >= 0)) {
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
            return 0;
        }
        if (ui::HitTest(rcBtnReset_, x, y)) {
            ResetAllEffects(fx_);
            if (callbacks_.onEffectsChanged) callbacks_.onEffectsChanged(fx_, true);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        for (int i = 0; i < 4; ++i) {
            if (ui::HitTest(rcPresets_[i], x, y)) {
                ApplyEffectsPreset(fx_, i);
                if (callbacks_.onEffectsChanged) callbacks_.onEffectsChanged(fx_, true);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
        }
        for (int i = 0; i < 5; ++i) {
            if (ui::HitTest(rcSliders_[i], x, y)) {
                dragSlider_ = i;
                SetCapture(hwnd_);
                UpdateSliderFromX(i, x, false);
                return 0;
            }
        }
        if (ui::HitTest(rcDropBlur_, x, y)) ShowChoiceMenu(0, rcDropBlur_);
        else if (ui::HitTest(rcDropBlurEngine_, x, y)) ShowChoiceMenu(1, rcDropBlurEngine_);
        else if (ui::HitTest(rcSwitchMirror_, x, y)) {
            fx_.mirrorH = !fx_.mirrorH;
            if (callbacks_.onEffectsChanged) callbacks_.onEffectsChanged(fx_, true);
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (ui::HitTest(rcDropFocusMode_, x, y)) ShowChoiceMenu(2, rcDropFocusMode_);
        else if (ui::HitTest(rcBtnLens_, x, y)) {
            cam_.frontCamera = !cam_.frontCamera;
            if (callbacks_.onCameraChanged) callbacks_.onCameraChanged(cam_);
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (ui::HitTest(rcDropEv_, x, y)) ShowChoiceMenu(4, rcDropEv_);
        else if (ui::HitTest(rcBtnTorch_, x, y)) {
            cam_.torchEnabled = !cam_.torchEnabled;
            if (callbacks_.onCameraChanged) callbacks_.onCameraChanged(cam_);
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (ui::HitTest(rcSwitchPreferFps_, x, y)) {
            cam_.preferFps = !cam_.preferFps;
            if (callbacks_.onCameraChanged) callbacks_.onCameraChanged(cam_);
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONUP:
        if (dragSlider_ >= 0) {
            const int ended = dragSlider_;
            dragSlider_ = -1;
            ReleaseCapture();
            UpdateSliderFromX(ended, static_cast<short>(LOWORD(lParam)), true);
        }
        return 0;

    case WM_CAPTURECHANGED:
        if (dragSlider_ >= 0) {
            dragSlider_ = -1;
            if (callbacks_.onEffectsChanged) callbacks_.onEffectsChanged(fx_, true);
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

} // namespace zerocam
