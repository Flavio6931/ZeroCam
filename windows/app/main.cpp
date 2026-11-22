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
#include <mfapi.h>
#include <objbase.h>
#include <commctrl.h>
#include <cwchar>
#include "MainWindow.h"
#include "VirtualCameraHost.h"

// Punto de entrada de ZeroCam.exe (Win32 nativo DPI-aware con soporte de auto-registro silencioso --register-vcam).
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR lpCmdLine, int) {
    // Endurece la resolución de DLLs y ejecutables eliminando el directorio de trabajo actual (anti DLL/PATH hijacking).
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    SetSearchPathMode(BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE | BASE_SEARCH_PATH_PERMANENT);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION);

    if (lpCmdLine && wcsstr(lpCmdLine, L"--register-vcam") != nullptr) {
        const bool regOk = zerocam::VirtualCameraHost::InstallAndRegisterElevated();
        MFShutdown();
        CoUninitialize();
        return regOk ? 0 : 1;
    }

    if (lpCmdLine && wcsstr(lpCmdLine, L"--unregister-vcam") != nullptr) {
        const bool unregOk = zerocam::VirtualCameraHost::UninstallAndUnregisterElevated();
        MFShutdown();
        CoUninitialize();
        return unregOk ? 0 : 1;
    }

    Gdiplus::GdiplusStartupInput gdiplusInput;
    ULONG_PTR gdiplusToken = 0;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr);

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    int exitCode = 0;
    {
        zerocam::MainWindow window;
        if (!window.Create(hInstance)) {
            exitCode = 1;
        } else {
            exitCode = window.RunMessageLoop();
        }
    }

    if (gdiplusToken != 0) {
        Gdiplus::GdiplusShutdown(gdiplusToken);
    }
    MFShutdown();
    CoUninitialize();
    return exitCode;
}
