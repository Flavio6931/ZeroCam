#pragma once
#include <guiddef.h>

// CLSID del servidor COM de la cámara virtual ZeroCam ({8F4A9C12-6E3B-4D71-9A25-3C8E71B40D62}).
// {8F4A9C12-6E3B-4D71-9A25-3C8E71B40D62}
static constexpr GUID CLSID_ZeroCamSource = {
    0x8f4a9c12, 0x6e3b, 0x4d71, { 0x9a, 0x25, 0x3c, 0x8e, 0x71, 0xb4, 0x0d, 0x62 }
};

// GUID de categoría PINNAME_VIDEO_CAPTURE definido localmente para evitar conflicto de enlace con ksguid.
static constexpr GUID kPinNameVideoCapture = {
    0xfb6c4281, 0x0353, 0x11d1, { 0x90, 0x5f, 0x00, 0x00, 0xc0, 0xcc, 0x16, 0xba }
};

// Constantes de identidad e IPC compartidas entre el productor (ZeroCam.exe) y FrameServer (ZeroCamSource.dll).
inline constexpr const wchar_t* kZeroCamClsidString    = L"{8F4A9C12-6E3B-4D71-9A25-3C8E71B40D62}";
inline constexpr const wchar_t* kZeroCamFriendlyName   = L"ZeroCam Virtual Camera";
inline constexpr const wchar_t* kZeroCamDefaultIpcPath = L"C:\\ProgramData\\ZeroCam\\ipc_ring.bin";
#ifdef ZEROCAM_ALLOW_IPC_ENV_OVERRIDE
inline constexpr const wchar_t* kZeroCamIpcEnvOverride = L"ZEROCAM_IPC_PATH";
#endif
inline constexpr const wchar_t* kZeroCamRegistryDesc   = L"ZeroCam Virtual Camera Source";
