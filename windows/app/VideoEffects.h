#pragma once

namespace zerocam {

/// Parámetros de filtros de imagen GPU, espejo, zoom y desenfoque de fondo IA.
struct VideoEffectsConfig {
    int preset = 0;            // 0=Natural, 1=Estudio, 2=Luz Baja, 3=Cálido
    float brightness = 0.0f;   // -0.30f .. +0.30f
    float contrast = 1.0f;     // 0.70f .. 1.40f
    float saturation = 1.0f;   // 0.50f .. 1.50f
    float warmth = 0.0f;       // -0.30f .. +0.30f
    float gamma = 1.0f;        // 0.70f .. 1.40f
    float sharpness = 0.0f;    // 0.00f .. 1.00f
    bool mirrorH = false;
    float digitalZoom = 1.0f;  // 1.00f .. 2.00f
    int blurMode = 0;          // 0=Apagado, 1=Suave, 2=Medio, 3=Fuerte
    int blurStyle = 1;         // 0=Óptico Lente f/1.8 (0% IA), 1=Retrato IA RVM (Alpha Matting 512x288)
    bool blurUseGpu = true;    // true=DirectML GPU (~14 ms, recomendado), false=CPU (~32 ms, 0% GPU juegos)
    float focusX = 0.5f;       // Centro óptico X normalizado [0.05..0.95]
    float focusY = 0.45f;      // Centro óptico Y normalizado [0.05..0.95]

    /// Indica si la configuración equivale a paso directo sin procesamiento (0% coste GPU).
    bool IsIdentity() const noexcept;
};

/// Estado remoto de enfoque y controles físicos enviados desde Windows hacia Android.
struct CameraRemoteConfig {
    int focusMode = 0;         // 0=Rostro (Auto), 1=Bloqueado (AF Lock)
    bool frontCamera = false;
    bool torchEnabled = false;
    float lensZoom = 1.0f;     // 1.00f .. 3.00f
    int exposureEv = 0;        // -2 .. +2
    bool preferFps = true;
};

/// Limita todos los campos de filtros a rangos seguros de operación.
void ClampEffectsConfig(VideoEffectsConfig& cfg) noexcept;

/// Aplica un preset rápido de color y nitidez manteniendo espejo, zoom y desenfoque.
void ApplyEffectsPreset(VideoEffectsConfig& cfg, int presetId) noexcept;

/// Restablece todos los filtros, espejo, zoom y desenfoque a su estado neutral.
void ResetAllEffects(VideoEffectsConfig& cfg) noexcept;

} // namespace zerocam
