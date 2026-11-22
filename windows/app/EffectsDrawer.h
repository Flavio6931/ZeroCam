#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <functional>
#include <string>
#include "UiTheme.h"
#include "VideoEffects.h"

namespace zerocam {

/// Callbacks emitidos por el panel lateral de Filtros, IA Bokeh y Cámara Remota hacia MainWindow.
struct EffectsDrawerCallbacks {
    std::function<void(const VideoEffectsConfig& fx, bool persistNow)> onEffectsChanged;
    std::function<void(const CameraRemoteConfig& cam)> onCameraChanged;
    std::function<void()> onCloseRequested;
};

/// Panel lateral integrado de Filtros GPU, Desenfoque IA y Control Remoto de Enfoque/Cámara.
class EffectsDrawer {
public:
    EffectsDrawer() = default;
    ~EffectsDrawer() = default;

    /// Crea la ventana hija del drawer de Filtros y Cámara.
    bool Create(HWND parent, HINSTANCE hInstance, float dpiScale, EffectsDrawerCallbacks callbacks);

    /// Sincroniza el estado mostrado con las preferencias guardadas de filtros y cámara.
    void SyncFromConfig(const VideoEffectsConfig& fx, const CameraRemoteConfig& cam);

    /// Actualiza en vivo la disponibilidad, motor real (GPU/CPU) y latencia medida de segmentación IA.
    void UpdateLiveStats(bool streamConnected, bool aiAvailable, bool aiUsingGpu, double aiMs);

    /// Actualiza la escala DPI cuando cambia el monitor.
    void SetDpiScale(float dpiScale);

    /// Devuelve el HWND del drawer lateral.
    HWND Handle() const noexcept { return hwnd_; }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam);

    void ComputeRects(int width, int height);
    void Paint(HDC targetDc);
    void ShowChoiceMenu(int menuKind, const Gdiplus::RectF& anchorRc);
    void UpdateSliderFromX(int sliderIdx, int clientX, bool persistNow);

    HWND hwnd_ = nullptr;
    HINSTANCE hInstance_ = nullptr;
    float dpiScale_ = 1.0f;
    EffectsDrawerCallbacks callbacks_{};

    VideoEffectsConfig fx_{};
    CameraRemoteConfig cam_{};
    bool streamConnected_ = false;
    bool aiAvailable_ = false;
    bool aiUsingGpu_ = false;
    double aiMs_ = 0.0;

    float scrollY_ = 0.0f;
    float maxScroll_ = 0.0f;
    int dragSlider_ = -1;
    int hoverId_ = 0;
    bool trackingLeave_ = false;

    Gdiplus::RectF rcClose_{};
    Gdiplus::RectF rcBtnReset_{};
    Gdiplus::RectF rcPresets_[4]{};
    Gdiplus::RectF rcDropBlur_{};
    Gdiplus::RectF rcDropBlurEngine_{};
    Gdiplus::RectF rcSliders_[5]{};
    Gdiplus::RectF rcSwitchMirror_{};
    Gdiplus::RectF rcDropFocusMode_{};
    Gdiplus::RectF rcBtnLens_{};
    Gdiplus::RectF rcDropEv_{};
    Gdiplus::RectF rcBtnTorch_{};
    Gdiplus::RectF rcSwitchPreferFps_{};
};

} // namespace zerocam
