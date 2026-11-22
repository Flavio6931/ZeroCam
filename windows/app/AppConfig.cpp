#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "AppConfig.h"
#include "../shared/WireCrypto.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace zerocam {

namespace {

constexpr const char* kPinDpapiEntropy = "ZeroCamWifiPinDpapiV2";

std::wstring FormatFloatDot(float v) {
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%.3f", v);
    std::wstring w;
    for (char c : std::string(buf)) {
        w.push_back(c == ',' ? L'.' : static_cast<wchar_t>(c));
    }
    return w;
}

float LoadFloatKey(const std::wstring& path, const wchar_t* key, float defVal) {
    wchar_t buf[48]{};
    GetPrivateProfileStringW(L"ZeroCam", key, L"", buf, 48, path.c_str());
    if (buf[0] == L'\0') return defVal;
    std::string ascii;
    for (wchar_t ch : std::wstring(buf)) {
        if (ch == L',') ascii.push_back('.');
        else if (ch >= 0 && ch < 128) ascii.push_back(static_cast<char>(ch));
    }
    char* end = nullptr;
    float parsed = std::strtof(ascii.c_str(), &end);
    return (end != ascii.c_str()) ? parsed : defVal;
}

// Resuelve la ruta %APPDATA%\ZeroCam\config.ini creando su carpeta padre si aún no existe.
std::wstring ResolveConfigPath(bool ensureDir) {
    wchar_t appData[MAX_PATH]{};
    DWORD len = GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        return {};
    }
    std::wstring dir = std::wstring(appData) + L"\\ZeroCam";
    if (ensureDir) {
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    return dir + L"\\config.ini";
}

} // namespace

AppConfig LoadAppConfig() {
    AppConfig cfg{};
    const std::wstring path = ResolveConfigPath(false);
    if (path.empty()) return cfg;

    cfg.wifiMode = (GetPrivateProfileIntW(L"ZeroCam", L"WifiMode", 0, path.c_str()) != 0);
    cfg.minimizeToTrayOnClose = (GetPrivateProfileIntW(L"ZeroCam", L"MinimizeToTray", 1, path.c_str()) != 0);

    const UINT w = GetPrivateProfileIntW(L"ZeroCam", L"Width", 1280, path.c_str());
    const UINT h = GetPrivateProfileIntW(L"ZeroCam", L"Height", 720, path.c_str());
    if ((w == 640 && h == 480) || (w == 1280 && h == 720) || (w == 1920 && h == 1080)) {
        cfg.width = w;
        cfg.height = h;
    }

    const UINT fps = GetPrivateProfileIntW(L"ZeroCam", L"Fps", 30, path.c_str());
    if (fps == 15 || fps == 24 || fps == 30 || fps == 60) {
        cfg.fps = fps;
    }

    const UINT br = GetPrivateProfileIntW(L"ZeroCam", L"BitrateBps", 4000000, path.c_str());
    if (br == 2000000 || br == 4000000 || br == 8000000 || br == 12000000) {
        cfg.bitrateBps = br;
    }

    const int rot = static_cast<int>(GetPrivateProfileIntW(L"ZeroCam", L"RotationOverride", static_cast<UINT>(-1), path.c_str()));
    cfg.rotationOverride = (rot >= 0 && rot <= 3) ? rot : -1;

    const UINT port = GetPrivateProfileIntW(L"ZeroCam", L"InternalPort", kDefaultStreamPort, path.c_str());
    if (port >= 1024 && port <= 65535) {
        cfg.internalPort = static_cast<uint16_t>(port);
    }

    bool needPersistPin = false;
    // Lee el PIN Wi-Fi cifrado con DPAPI (búfer >= 2048 wchars); migra desde texto claro o auto-genera 6 dígitos.
    std::vector<wchar_t> encPinBuf(4096, L'\0');
    const DWORD encLen = GetPrivateProfileStringW(
        L"ZeroCam", L"WifiPinEnc", L"", encPinBuf.data(), static_cast<DWORD>(encPinBuf.size()), path.c_str());
    if (encLen > 0) {
        std::string plainUtf8;
        if (crypto::DpapiDecryptFromBase64(std::wstring(encPinBuf.data(), encLen), kPinDpapiEntropy, plainUtf8) &&
            !plainUtf8.empty()) {
            cfg.wifiPin = Utf8ToWide(plainUtf8);
            SecureZeroMemory(plainUtf8.data(), plainUtf8.size());
        }
    }

    if (cfg.wifiPin.empty()) {
        wchar_t legacyPinBuf[128]{};
        const DWORD legacyLen = GetPrivateProfileStringW(L"ZeroCam", L"WifiPin", L"", legacyPinBuf, 128, path.c_str());
        if (legacyLen > 0) {
            cfg.wifiPin.assign(legacyPinBuf, legacyLen);
            SecureZeroMemory(legacyPinBuf, sizeof(legacyPinBuf));
            needPersistPin = true; // Migra de inmediato a WifiPinEnc y borra WifiPin en texto claro.
        } else {
            cfg.wifiPin = crypto::GenerateRandomPin6();
            needPersistPin = true; // Auto-genera PIN Wi-Fi de 6 dígitos en el primer arranque.
        }
    }

    // Carga de filtros de imagen GPU / IA Bokeh
    cfg.effects.preset      = static_cast<int>(GetPrivateProfileIntW(L"ZeroCam", L"FxPreset", 0, path.c_str()));
    cfg.effects.brightness  = LoadFloatKey(path, L"FxBright", 0.0f);
    cfg.effects.contrast    = LoadFloatKey(path, L"FxContrast", 1.0f);
    cfg.effects.saturation  = LoadFloatKey(path, L"FxSat", 1.0f);
    cfg.effects.warmth      = LoadFloatKey(path, L"FxWarmth", 0.0f);
    cfg.effects.gamma       = LoadFloatKey(path, L"FxGamma", 1.0f);
    cfg.effects.sharpness   = LoadFloatKey(path, L"FxSharp", 0.0f);
    cfg.effects.mirrorH     = (GetPrivateProfileIntW(L"ZeroCam", L"FxMirror", 0, path.c_str()) != 0);
    cfg.effects.digitalZoom = LoadFloatKey(path, L"FxZoom", 1.0f);
    cfg.effects.blurMode    = static_cast<int>(GetPrivateProfileIntW(L"ZeroCam", L"FxBlurMode", 0, path.c_str()));
    cfg.effects.blurStyle   = static_cast<int>(GetPrivateProfileIntW(L"ZeroCam", L"FxBlurStyle", 1, path.c_str()));
    cfg.effects.blurUseGpu  = (GetPrivateProfileIntW(L"ZeroCam", L"FxBlurGpu", 1, path.c_str()) != 0);
    cfg.effects.focusX      = LoadFloatKey(path, L"FxFocusX", 0.5f);
    cfg.effects.focusY      = LoadFloatKey(path, L"FxFocusY", 0.45f);
    ClampEffectsConfig(cfg.effects);

    // Carga de controles remotos de cámara en Android
    cfg.camera.focusMode    = (GetPrivateProfileIntW(L"ZeroCam", L"CamFocusMode", 0, path.c_str()) == 1) ? 1 : 0;
    cfg.camera.frontCamera  = (GetPrivateProfileIntW(L"ZeroCam", L"CamFront", 0, path.c_str()) != 0);
    cfg.camera.torchEnabled = (GetPrivateProfileIntW(L"ZeroCam", L"CamTorch", 0, path.c_str()) != 0);
    cfg.camera.lensZoom     = std::clamp(LoadFloatKey(path, L"CamZoom", 1.0f), 1.0f, 3.0f);
    const UINT evRaw        = GetPrivateProfileIntW(L"ZeroCam", L"CamEv", 2, path.c_str()); // almacenado como ev + 2
    cfg.camera.exposureEv   = std::clamp(static_cast<int>(evRaw) - 2, -2, 2);
    cfg.camera.preferFps    = (GetPrivateProfileIntW(L"ZeroCam", L"CamPreferFps", 1, path.c_str()) != 0);
    if (needPersistPin) {
        SaveAppConfig(cfg);
    }
    return cfg;
}

bool SaveAppConfig(const AppConfig& cfg) {
    const std::wstring path = ResolveConfigPath(true);
    if (path.empty()) return false;

    const std::wstring wifiStr = cfg.wifiMode ? L"1" : L"0";
    const std::wstring trayStr = cfg.minimizeToTrayOnClose ? L"1" : L"0";
    const std::wstring wStr = std::to_wstring(cfg.width);
    const std::wstring hStr = std::to_wstring(cfg.height);
    const std::wstring fpsStr = std::to_wstring(cfg.fps);
    const std::wstring brStr = std::to_wstring(cfg.bitrateBps);
    const std::wstring rotStr = std::to_wstring(cfg.rotationOverride);
    const std::wstring portStr = std::to_wstring(cfg.internalPort);

    WritePrivateProfileStringW(L"ZeroCam", L"WifiMode", wifiStr.c_str(), path.c_str());
    WritePrivateProfileStringW(L"ZeroCam", L"MinimizeToTray", trayStr.c_str(), path.c_str());
    WritePrivateProfileStringW(L"ZeroCam", L"Width", wStr.c_str(), path.c_str());
    WritePrivateProfileStringW(L"ZeroCam", L"Height", hStr.c_str(), path.c_str());
    WritePrivateProfileStringW(L"ZeroCam", L"Fps", fpsStr.c_str(), path.c_str());
    WritePrivateProfileStringW(L"ZeroCam", L"BitrateBps", brStr.c_str(), path.c_str());
    WritePrivateProfileStringW(L"ZeroCam", L"RotationOverride", rotStr.c_str(), path.c_str());
    WritePrivateProfileStringW(L"ZeroCam", L"InternalPort", portStr.c_str(), path.c_str());

    // Elimina siempre cualquier clave heredada en texto claro y persiste únicamente el blob DPAPI.
    WritePrivateProfileStringW(L"ZeroCam", L"WifiPin", nullptr, path.c_str());
    if (!cfg.wifiPin.empty()) {
        std::string pinUtf8 = WideToUtf8(cfg.wifiPin);
        std::wstring pinB64;
        if (crypto::DpapiEncryptToBase64(pinUtf8, kPinDpapiEntropy, pinB64)) {
            WritePrivateProfileStringW(L"ZeroCam", L"WifiPinEnc", pinB64.c_str(), path.c_str());
        }
        if (!pinUtf8.empty()) {
            SecureZeroMemory(pinUtf8.data(), pinUtf8.size());
        }
    } else {
        WritePrivateProfileStringW(L"ZeroCam", L"WifiPinEnc", nullptr, path.c_str());
    }

    WritePrivateProfileStringW(L"ZeroCam", L"FxPreset", std::to_wstring(cfg.effects.preset).c_str(), path.c_str());
    WritePrivateProfileStringW(L"ZeroCam", L"FxBright", FormatFloatDot(cfg.effects.brightness).c_str(), path.c_str());
    WritePrivateProfileStringW(L"ZeroCam", L"FxContrast", FormatFloatDot(cfg.effects.contrast).c_str(), path.c_str());
    WritePrivateProfileStringW(L"ZeroCam", L"FxSat", FormatFloatDot(cfg.effects.saturation).c_str(), path.c_str());
    WritePrivateProfileStringW(L"ZeroCam", L"FxWarmth", FormatFloatDot(cfg.effects.warmth).c_str(), path.c_str());
    WritePrivateProfileStringW(L"ZeroCam", L"FxGamma", FormatFloatDot(cfg.effects.gamma).c_str(), path.c_str());
    WritePrivateProfileStringW(L"ZeroCam", L"FxSharp", FormatFloatDot(cfg.effects.sharpness).c_str(), path.c_str());
    WritePrivateProfileStringW(L"ZeroCam", L"FxMirror", cfg.effects.mirrorH ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"ZeroCam", L"FxZoom", FormatFloatDot(cfg.effects.digitalZoom).c_str(), path.c_str());
    WritePrivateProfileStringW(L"ZeroCam", L"FxBlurMode", std::to_wstring(cfg.effects.blurMode).c_str(), path.c_str());
    WritePrivateProfileStringW(L"ZeroCam", L"FxBlurStyle", std::to_wstring(cfg.effects.blurStyle).c_str(), path.c_str());
    WritePrivateProfileStringW(L"ZeroCam", L"FxBlurGpu", cfg.effects.blurUseGpu ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"ZeroCam", L"FxFocusX", FormatFloatDot(cfg.effects.focusX).c_str(), path.c_str());
    WritePrivateProfileStringW(L"ZeroCam", L"FxFocusY", FormatFloatDot(cfg.effects.focusY).c_str(), path.c_str());

    WritePrivateProfileStringW(L"ZeroCam", L"CamFocusMode", std::to_wstring(cfg.camera.focusMode).c_str(), path.c_str());
    WritePrivateProfileStringW(L"ZeroCam", L"CamFront", cfg.camera.frontCamera ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"ZeroCam", L"CamTorch", cfg.camera.torchEnabled ? L"1" : L"0", path.c_str());
    WritePrivateProfileStringW(L"ZeroCam", L"CamZoom", FormatFloatDot(cfg.camera.lensZoom).c_str(), path.c_str());
    WritePrivateProfileStringW(L"ZeroCam", L"CamEv", std::to_wstring(cfg.camera.exposureEv + 2).c_str(), path.c_str());
    return WritePrivateProfileStringW(L"ZeroCam", L"CamPreferFps", cfg.camera.preferFps ? L"1" : L"0", path.c_str()) == TRUE;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed);
    return out;
}

} // namespace zerocam
