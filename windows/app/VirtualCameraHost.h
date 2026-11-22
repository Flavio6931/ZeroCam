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

// Gestor del ciclo de vida de la cámara virtual (MFCreateVirtualCamera en Win11 y DirectShow en Win10).
class VirtualCameraHost {
public:
    VirtualCameraHost() = default;
    ~VirtualCameraHost();

    // Verifica si la cámara virtual correspondiente a esta versión de Windows está registrada.
    static bool IsComRegistered(std::wstring* outDllPath = nullptr);
    // Verifica específicamente si el filtro DirectShow 64-bit está registrado en HKLM.
    static bool IsDShowRegistered(std::wstring* outDll64Path = nullptr);
    // Enumera dispositivos MF para confirmar si la cámara persistente de sistema (Win11) está registrada.
    static bool IsSystemMfCameraPresent();
    // Comprueba si las DLL instaladas coinciden byte a byte con las ubicadas junto a ZeroCam.exe.
    static bool IsInstalledDllUpToDate();
    // Consulta si Windows soporta cámara virtual (true: Win11 MF nativo + Win10 DirectShow).
    static bool IsPlatformSupported();
    // Consulta dinámicamente si Windows 11 MFCreateVirtualCamera está disponible en mfsensorgroup.dll.
    static bool IsMfVirtualCameraSupported();
    // Instala DLLs en ProgramData, aplica ACLs, registra cámara virtual (Win11 MF / Win10 DShow), Firewall y FrameServer.
    static bool InstallAndRegisterElevated(std::wstring* outError = nullptr);
    // Desregistra cámara virtual (MF + DirectShow), elimina reglas Firewall y limpia C:\ProgramData\ZeroCam.
    static bool UninstallAndUnregisterElevated(std::wstring* outError = nullptr);

    // Activa o valida la cámara virtual visible en OBS, Zoom, Discord y Cámara de Windows.
    bool Start(std::wstring& outError);
    // Detiene limpiamente la sesión de cámara virtual activa.
    void Stop();
    bool IsRunning() const noexcept {
        return virtualCamera_ != nullptr || systemCameraActive_ || dshowActive_;
    }

private:
    Microsoft::WRL::ComPtr<IMFVirtualCamera> virtualCamera_;
    bool systemCameraActive_ = false;
    bool dshowActive_ = false;
};

} // namespace zerocam
