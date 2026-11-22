#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfvirtualcamera.h>
#include <wrl/client.h>
#include <string>

namespace zerocam {

// Gestor del ciclo de vida de la cámara virtual nativa de Windows 11 (MFCreateVirtualCamera).
class VirtualCameraHost {
public:
    VirtualCameraHost() = default;
    ~VirtualCameraHost();

    // Verifica si la DLL COM de ZeroCam figura registrada y existente en HKLM\SOFTWARE\Classes\CLSID.
    static bool IsComRegistered(std::wstring* outDllPath = nullptr);
    // Comprueba si la DLL instalada en HKLM coincide en tamaño y contenido con ZeroCamSource.dll junto al ejecutable.
    static bool IsInstalledDllUpToDate();
    // Consulta si Windows soporta MFVirtualCameraType_SoftwareCameraSource.
    static bool IsPlatformSupported();
    // Instala ZeroCamSource.dll en ProgramData, aplica ACLs, registra COM, crea reglas Firewall y reinicia FrameServer (requiere elevación).
    static bool InstallAndRegisterElevated(std::wstring* outError = nullptr);
    // Desregistra COM, elimina reglas Firewall y limpia C:\ProgramData\ZeroCam (requiere elevación).
    static bool UninstallAndUnregisterElevated(std::wstring* outError = nullptr);

    // Crea e inicia la instancia de sesión de cámara virtual visible en OBS, Zoom, Discord y Cámara Win11.
    bool Start(std::wstring& outError);
    // Detiene y remueve limpiamente la cámara virtual.
    void Stop();
    bool IsRunning() const noexcept { return virtualCamera_ != nullptr; }

private:
    Microsoft::WRL::ComPtr<IMFVirtualCamera> virtualCamera_;
};

} // namespace zerocam
