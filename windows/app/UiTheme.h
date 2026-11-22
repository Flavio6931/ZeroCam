#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objidl.h>
#include <algorithm>
namespace Gdiplus {
    using std::min;
    using std::max;
}
#include <gdiplus.h>
#include <string>

namespace zerocam::ui {

// Paleta exacta de ZeroCam Tokens (Figma).
inline const Gdiplus::Color kBgCanvas(255, 15, 17, 23);         // #0F1117
inline const Gdiplus::Color kBgSurface(255, 23, 26, 33);        // #171A21
inline const Gdiplus::Color kBgCard(255, 31, 36, 46);           // #1F242E
inline const Gdiplus::Color kBgInput(255, 38, 44, 56);          // #262C38
inline const Gdiplus::Color kBgInputHover(255, 48, 56, 72);     // Hover sutil sobre #262C38
inline const Gdiplus::Color kBgOled(255, 0, 0, 0);              // #000000
inline const Gdiplus::Color kBgPreview(255, 13, 18, 28);        // #0D121C
inline const Gdiplus::Color kBorderSubtle(255, 46, 53, 69);     // #2E3545
inline const Gdiplus::Color kBorderHover(255, 70, 82, 108);     // Hover en bordes
inline const Gdiplus::Color kBorderAccent(255, 45, 125, 255);   // #2D7DFF
inline const Gdiplus::Color kTextPrimary(255, 241, 243, 247);   // #F1F3F7
inline const Gdiplus::Color kTextSecondary(255, 154, 164, 178); // #9AA4B2
inline const Gdiplus::Color kTextMuted(255, 107, 116, 133);     // #6B7485
inline const Gdiplus::Color kAccentPrimary(255, 45, 125, 255);  // #2D7DFF
inline const Gdiplus::Color kAccentHover(255, 66, 140, 255);    // Hover #2D7DFF
inline const Gdiplus::Color kStatusSuccess(255, 34, 197, 94);   // #22C55E
inline const Gdiplus::Color kStatusWarning(255, 245, 158, 11);  // #F59E0B
inline const Gdiplus::Color kStatusDanger(255, 239, 68, 68);    // #EF4444
inline const Gdiplus::Color kStatusDangerHover(255, 248, 92, 92);

constexpr COLORREF kRefBgCanvas  = RGB(15, 17, 23);
constexpr COLORREF kRefBgSurface = RGB(23, 26, 33);
constexpr COLORREF kRefBgInput   = RGB(38, 44, 56);
constexpr COLORREF kRefBorderSub = RGB(46, 53, 69);
constexpr COLORREF kRefTextPri   = RGB(241, 243, 247);
constexpr COLORREF kRefTextMuted = RGB(154, 164, 178);

/// Estilo visual de un botón dibujado con GDI+.
enum class ButtonKind {
    Primary,
    Secondary,
    Danger
};

/// Ícono vectorial integrado en botones y tarjetas.
enum class IconKind {
    None,
    Eye,
    EyeOff,
    Gear,
    Close,
    Sliders
};

/// Escala una medida lógica en píxeles (96 DPI) según el factor DPI de la ventana.
int ScaleDpi(int px, float dpiScale);

/// Escala un tamaño en punto flotante según el factor DPI de la ventana.
float ScaleDpif(float px, float dpiScale);

/// Verifica si un punto de cliente cae dentro de un RectF.
inline bool HitTest(const Gdiplus::RectF& rc, int x, int y) noexcept {
    const float fx = static_cast<float>(x);
    const float fy = static_cast<float>(y);
    return fx >= rc.X && fx <= (rc.X + rc.Width) && fy >= rc.Y && fy <= (rc.Y + rc.Height);
}

/// Lienzo en memoria RAII con doble buffer GDI para pintar interfaces GDI+ sin parpadeo.
class DoubleBufferPaint {
public:
    /// Inicializa el buffer compatible a partir de un HDC de destino y su tamaño cliente.
    DoubleBufferPaint(HDC targetDc, int width, int height);
    /// Vuelca el buffer al HDC de destino y libera los objetos GDI.
    ~DoubleBufferPaint();

    DoubleBufferPaint(const DoubleBufferPaint&) = delete;
    DoubleBufferPaint& operator=(const DoubleBufferPaint&) = delete;

    /// Devuelve el contexto de memoria para construir un objeto Gdiplus::Graphics.
    HDC MemoryDc() const noexcept { return memDc_; }

private:
    HDC targetDc_ = nullptr;
    HDC memDc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ oldBitmap_ = nullptr;
    int width_ = 0;
    int height_ = 0;
};

/// Agrega un rectángulo con esquinas redondeadas a un GraphicsPath.
void AddRoundedRectPath(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& rc, float radius);

/// Rellena un rectángulo redondeado con color sólido anti-aliased.
void FillRoundRect(Gdiplus::Graphics& g, const Gdiplus::RectF& rc, float radius, Gdiplus::Color fill);

/// Dibuja el contorno de un rectángulo redondeado con color y grosor configurables.
void DrawRoundRect(Gdiplus::Graphics& g, const Gdiplus::RectF& rc, float radius, Gdiplus::Color stroke, float width = 1.0f);

/// Renderiza texto nítido con fuente moderna (Segoe UI / Consolas) usando medidas en píxeles.
void DrawTextUi(
    Gdiplus::Graphics& g,
    const std::wstring& text,
    const Gdiplus::RectF& bounds,
    const wchar_t* fontName,
    float fontSizePx,
    bool bold,
    Gdiplus::Color color,
    Gdiplus::StringAlignment hAlign = Gdiplus::StringAlignmentNear,
    Gdiplus::StringAlignment vAlign = Gdiplus::StringAlignmentCenter,
    bool allowWrap = false);

/// Dibuja una cápsula (pill badge) con punto indicador de estado y texto compacto.
void DrawPillBadge(
    Gdiplus::Graphics& g,
    const Gdiplus::RectF& rc,
    Gdiplus::Color dotColor,
    const std::wstring& label,
    float dpiScale);

/// Mide el ancho recomendado para una cápsula de estado según su texto.
float MeasurePillBadgeWidth(Gdiplus::Graphics& g, const std::wstring& label, float dpiScale);

/// Dibuja un interruptor moderno redondeado (Switch On / Switch Off de Figma).
void DrawToggleSwitch(Gdiplus::Graphics& g, const Gdiplus::RectF& rc, bool on, bool hover, float dpiScale);

/// Dibuja una caja desplegable oscura con valor y chevron inferior vectorial.
void DrawDropdownBox(Gdiplus::Graphics& g, const Gdiplus::RectF& rc, const std::wstring& value, bool hover, float dpiScale);

/// Dibuja un botón redondeado con relleno semántico, borde opcional e ícono vectorial centrado.
void DrawButton(
    Gdiplus::Graphics& g,
    const Gdiplus::RectF& rc,
    const std::wstring& label,
    ButtonKind kind,
    IconKind icon,
    bool hover,
    bool activeBorder,
    float dpiScale);

/// Dibuja el ícono de rayo en el centro de la tarjeta de vista previa oculta (0.0% GPU 3D).
void DrawBoltIcon(Gdiplus::Graphics& g, const Gdiplus::RectF& bounds, Gdiplus::Color color);

/// Dibuja una X minimalista para cerrar el drawer lateral de Ajustes.
void DrawCloseIcon(Gdiplus::Graphics& g, const Gdiplus::RectF& bounds, Gdiplus::Color color, bool hover, float dpiScale);

/// Dibuja el visor limpio cuando la vista previa está habilitada pero aún no llegan frames del móvil.
void DrawViewfinderPlaceholder(Gdiplus::Graphics& g, int width, int height, const std::wstring& subtitle, float dpiScale);

/// Dibuja un slider horizontal moderno con pista oscura, progreso azul y perilla circular.
void DrawSliderControl(Gdiplus::Graphics& g, const Gdiplus::RectF& trackRc, float norm01, bool hover, bool active, float dpiScale);

} // namespace zerocam::ui
