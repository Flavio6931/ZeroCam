#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>
#include "AppConfig.h"
#include "UiTheme.h"

namespace zerocam {

/// Callbacks emitidos por el drawer lateral de Ajustes hacia MainWindow.
struct SettingsDrawerCallbacks {
    std::function<void(const AppConfig& newCfg, bool portChanged)> onConfigApplied;
    std::function<void(bool enabled)> onPreviewSwitchChanged;
    std::function<void(std::wstring& outStatus)> onReconnectUsb;
    std::function<void()> onCloseRequested;
};

/// Panel lateral integrado de Ajustes (réplica de Figma "Win11 / Drawer Ajustes").
class SettingsDrawer {
public:
    SettingsDrawer() = default;
    ~SettingsDrawer();

    /// Crea la ventana hija del drawer lateral e inicializa los controles de edición internos.
    bool Create(HWND parent, HINSTANCE hInstance, float dpiScale, SettingsDrawerCallbacks callbacks);

    /// Sincroniza el estado mostrado con la configuración activa, estado del preview, puerto enlazado y huella FP.
    void SyncFromConfig(const AppConfig& cfg, bool previewOn, uint16_t activeBoundPort, const std::wstring& hostFpDisplay = L"");

    /// Vuelve a enmascarar el campo PIN Wi-Fi (p. ej. al cerrar el drawer).
    void ResetPinVisibility();

    /// Actualiza la escala DPI cuando la ventana cambia de monitor.
    void SetDpiScale(float dpiScale);

    /// Redimensiona y posiciona los controles internos al tamaño actual del drawer.
    void LayoutControls(int width, int height);

    /// Devuelve el HWND del drawer lateral.
    HWND Handle() const noexcept { return hwnd_; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK EditSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    void Paint(HDC targetDc);
    void ComputeRects(int width, int height);
    void ShowChoiceMenu(int menuIndex, const Gdiplus::RectF& anchorRc);
    void CommitPinEdit(bool notifyParent);
    void UpdateFontSizes();

    HWND hwnd_ = nullptr;
    HINSTANCE hInstance_ = nullptr;
    float dpiScale_ = 1.0f;
    SettingsDrawerCallbacks callbacks_{};

    AppConfig cfg_{};
    bool previewOn_ = false;
    uint16_t boundPort_ = kDefaultStreamPort;
    std::wstring hostFpDisplay_;
    std::wstring statusNote_;

    HWND editPin_ = nullptr;
    HFONT monoFont_ = nullptr;
    HBRUSH inputBrush_ = nullptr;
    bool syncingEdits_ = false;
    bool pinVisible_ = false;

    // Zonas interactivas recalculadas en cada layout.
    Gdiplus::RectF rcClose_{};
    Gdiplus::RectF rcDropRes_{};
    Gdiplus::RectF rcDropFps_{};
    Gdiplus::RectF rcDropBitrate_{};
    Gdiplus::RectF rcDropRot_{};
    Gdiplus::RectF rcSwitchPrev_{};
    Gdiplus::RectF rcSwitchTray_{};
    Gdiplus::RectF rcPinBox_{};
    Gdiplus::RectF rcPinBadge_{};
    Gdiplus::RectF rcFpRow_{};
    Gdiplus::RectF rcBtnUsb_{};

    // Rectángulos auxiliares para texto y divisores.
    float yHeaderBottom_ = 0.0f;
    float yDiv1_ = 0.0f;
    float yDiv2_ = 0.0f;
    float yStatus_ = 0.0f;

    int hoverId_ = 0;
    bool trackingLeave_ = false;
};

} // namespace zerocam
