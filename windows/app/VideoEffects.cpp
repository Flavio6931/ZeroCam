#include "VideoEffects.h"
#include <algorithm>
#include <cmath>

namespace zerocam {

bool VideoEffectsConfig::IsIdentity() const noexcept {
    return blurMode == 0 &&
           !mirrorH &&
           std::fabs(digitalZoom - 1.0f) < 0.01f &&
           std::fabs(brightness) < 0.005f &&
           std::fabs(contrast - 1.0f) < 0.005f &&
           std::fabs(saturation - 1.0f) < 0.005f &&
           std::fabs(warmth) < 0.005f &&
           std::fabs(gamma - 1.0f) < 0.005f &&
           sharpness < 0.005f;
}

void ClampEffectsConfig(VideoEffectsConfig& cfg) noexcept {
    cfg.preset = std::clamp(cfg.preset, 0, 3);
    cfg.brightness = std::clamp(cfg.brightness, -0.30f, 0.30f);
    cfg.contrast = std::clamp(cfg.contrast, 0.70f, 1.40f);
    cfg.saturation = std::clamp(cfg.saturation, 0.50f, 1.50f);
    cfg.warmth = std::clamp(cfg.warmth, -0.30f, 0.30f);
    cfg.gamma = std::clamp(cfg.gamma, 0.70f, 1.40f);
    cfg.sharpness = std::clamp(cfg.sharpness, 0.0f, 1.0f);
    cfg.digitalZoom = std::clamp(cfg.digitalZoom, 1.0f, 2.0f);
    cfg.blurMode = std::clamp(cfg.blurMode, 0, 3);
    cfg.blurStyle = std::clamp(cfg.blurStyle, 0, 1);
    cfg.focusX = std::clamp(cfg.focusX, 0.05f, 0.95f);
    cfg.focusY = std::clamp(cfg.focusY, 0.05f, 0.95f);
}

void ApplyEffectsPreset(VideoEffectsConfig& cfg, int presetId) noexcept {
    cfg.preset = std::clamp(presetId, 0, 3);
    switch (cfg.preset) {
    case 1: // Estudio (nítido)
        cfg.brightness = 0.03f;
        cfg.contrast   = 1.12f;
        cfg.saturation = 1.08f;
        cfg.warmth     = 0.02f;
        cfg.gamma      = 1.04f;
        cfg.sharpness  = 0.55f;
        break;
    case 2: // Luz Baja
        cfg.brightness = 0.08f;
        cfg.contrast   = 1.08f;
        cfg.saturation = 1.04f;
        cfg.warmth     = 0.04f;
        cfg.gamma      = 1.22f;
        cfg.sharpness  = 0.35f;
        break;
    case 3: // Cálido
        cfg.brightness = 0.02f;
        cfg.contrast   = 1.08f;
        cfg.saturation = 1.14f;
        cfg.warmth     = 0.18f;
        cfg.gamma      = 1.02f;
        cfg.sharpness  = 0.30f;
        break;
    default: // Natural
        cfg.brightness = 0.0f;
        cfg.contrast   = 1.0f;
        cfg.saturation = 1.0f;
        cfg.warmth     = 0.0f;
        cfg.gamma      = 1.0f;
        cfg.sharpness  = 0.0f;
        break;
    }
    ClampEffectsConfig(cfg);
}

void ResetAllEffects(VideoEffectsConfig& cfg) noexcept {
    ApplyEffectsPreset(cfg, 0);
    cfg.mirrorH = false;
    cfg.digitalZoom = 1.0f;
    cfg.blurMode = 0;
    cfg.blurStyle = 1;
    cfg.blurUseGpu = true;
    cfg.focusX = 0.5f;
    cfg.focusY = 0.45f;
}

} // namespace zerocam
