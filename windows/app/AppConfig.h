#pragma once
#include <cstdint>
#include <string>
#include "VideoEffects.h"
#include "../shared/ZeroCamProtocol.h"

namespace zerocam {

// Preferencias persistentes de ZeroCam Desktop almacenadas en %APPDATA%\ZeroCam\config.ini.
struct AppConfig {
    bool wifiMode = false;
    bool minimizeToTrayOnClose = true;
    uint32_t width = 1280;
    uint32_t height = 720;
    uint32_t fps = 30;
    uint32_t bitrateBps = 4000000;
    int rotationOverride = -1;
    uint16_t internalPort = kDefaultStreamPort;
    std::wstring wifiPin;
    VideoEffectsConfig effects{};
    CameraRemoteConfig camera{};
};

// Carga las preferencias desde %APPDATA%\ZeroCam\config.ini aplicando valores seguros si no existe.
AppConfig LoadAppConfig();

// Guarda las preferencias del usuario en %APPDATA%\ZeroCam\config.ini.
bool SaveAppConfig(const AppConfig& cfg);

// Convierte una cadena UTF-16 de Windows a UTF-8 para protocolos de red y JSON.
std::string WideToUtf8(const std::wstring& w);

// Convierte una cadena UTF-8 a UTF-16 de Windows.
std::wstring Utf8ToWide(const std::string& s);

} // namespace zerocam
