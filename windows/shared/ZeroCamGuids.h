#pragma once
#include <guiddef.h>

// CLSID del servidor COM de la cámara virtual ZeroCam ({8F4A9C12-6E3B-4D71-9A25-3C8E71B40D62}).
// {8F4A9C12-6E3B-4D71-9A25-3C8E71B40D62}
static constexpr GUID CLSID_ZeroCamSource = {
    0x8f4a9c12, 0x6e3b, 0x4d71, { 0x9a, 0x25, 0x3c, 0x8e, 0x71, 0xb4, 0x0d, 0x62 }
};

// CLSID del filtro DirectShow de cámara virtual para Windows 10 ({6E3C7B24-1F89-4A52-B4C1-9D2E5F8A3107}).
static constexpr GUID CLSID_ZeroCamDShow = {
    0x6e3c7b24, 0x1f89, 0x4a52, { 0xb4, 0xc1, 0x9d, 0x2e, 0x5f, 0x8a, 0x31, 0x07 }
};

// GUID de categoría PINNAME_VIDEO_CAPTURE definido localmente para evitar conflicto de enlace con ksguid.
static constexpr GUID kPinNameVideoCapture = {
    0xfb6c4281, 0x0353, 0x11d1, { 0x90, 0x5f, 0x00, 0x00, 0xc0, 0xcc, 0x16, 0xba }
};

// Categorías de interfaz para cámara virtual MF y DirectShow (KSCATEGORY_VIDEO_CAMERA / KSCATEGORY_CAPTURE).
static constexpr GUID kKsCategoryVideoCamera = {
    0xe5323777, 0xf976, 0x4f5b, { 0x9b, 0x55, 0xb9, 0x46, 0x99, 0xc4, 0x6e, 0x44 }
};

static constexpr GUID kKsCategoryCapture = {
    0x65e8773d, 0x8f56, 0x11d0, { 0xa3, 0xb9, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96 }
};

// Subtipo DirectShow MEDIASUBTYPE_I420 ('I420', no incluido en uuids.h del SDK de Windows), nativo de WebRTC/Discord.
static constexpr GUID kMediaSubtypeI420 = {
    0x30323449, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
};

// Constantes de identidad e IPC compartidas entre el productor (ZeroCam.exe) y los servidores COM.
inline constexpr const wchar_t* kZeroCamClsidString      = L"{8F4A9C12-6E3B-4D71-9A25-3C8E71B40D62}";
inline constexpr const wchar_t* kZeroCamDShowClsidString = L"{6E3C7B24-1F89-4A52-B4C1-9D2E5F8A3107}";
inline constexpr const wchar_t* kZeroCamFriendlyName     = L"ZeroCam Virtual Camera";
inline constexpr const wchar_t* kZeroCamDefaultIpcPath   = L"C:\\ProgramData\\ZeroCam\\ipc_ring.bin";
#ifdef ZEROCAM_ALLOW_IPC_ENV_OVERRIDE
inline constexpr const wchar_t* kZeroCamIpcEnvOverride   = L"ZEROCAM_IPC_PATH";
#endif
inline constexpr const wchar_t* kZeroCamRegistryDesc     = L"ZeroCam Virtual Camera Source";
inline constexpr const wchar_t* kZeroCamDShowRegDesc     = L"ZeroCam Virtual Camera (DirectShow)";
