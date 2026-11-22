cmake_minimum_required(VERSION 3.24)
project(ZeroCam LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Usar CRT estático (/MT) en todos los binarios para evitar dependencias externas en FrameServer.exe.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00
    NTDDI_VERSION=0x0A00000B
)

if (MSVC)
    add_compile_definitions(_HAS_STATIC_RTTI=0)
    add_compile_options(/W3 /EHsc /permissive- /utf-8 /guard:cf /GS /sdl /GR-)
    add_link_options(/GUARD:CF /CETCOMPAT /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /DEPENDENTLOADFLAG:0x800)
endif()

set(ZEROCAM_VCAM_SOURCES
    windows/vcam_source/DllMain.cpp
    windows/vcam_source/ZeroCamActivator.cpp
    windows/vcam_source/ZeroCamMediaSource.cpp
    windows/vcam_source/ZeroCamMediaStream.cpp
    windows/vcam_source/ZeroCamSource.def
)

# Target 1: Servidor COM In-Proc de Cámara Virtual (ZeroCamSource.dll — producción sin ZEROCAM_IPC_PATH)
add_library(ZeroCamSource SHARED
    ${ZEROCAM_VCAM_SOURCES}
)

target_include_directories(ZeroCamSource PRIVATE
    windows/shared
    windows/vcam_source
)

target_link_libraries(ZeroCamSource PRIVATE
    mfplat
    mf
    mfuuid
    d3d11
    dxgi
    ole32
    oleaut32
    advapi32
)

# Target 1b: Variante exclusiva de pruebas automatizadas con ZEROCAM_ALLOW_IPC_ENV_OVERRIDE=1
add_library(ZeroCamSourceTest SHARED
    ${ZEROCAM_VCAM_SOURCES}
)

target_compile_definitions(ZeroCamSourceTest PRIVATE
    ZEROCAM_ALLOW_IPC_ENV_OVERRIDE=1
)

target_include_directories(ZeroCamSourceTest PRIVATE
    windows/shared
    windows/vcam_source
)

target_link_libraries(ZeroCamSourceTest PRIVATE
    mfplat
    mf
    mfuuid
    d3d11
    dxgi
    ole32
    oleaut32
    advapi32
)

# Fuentes compartidas del motor de decodificación, red, criptografía Wire v2, GPU y cámara virtual
set(ZEROCAM_ENGINE_SOURCES
    windows/shared/WireCrypto.cpp
    windows/app/VideoEffects.cpp
    windows/app/PersonSegmenter.cpp
    windows/app/GpuEffectsPipeline.cpp
    windows/app/D3D11Context.cpp
    windows/app/AnnexBParser.cpp
    windows/app/MfHwDecoder.cpp
    windows/app/StreamIngest.cpp
    windows/app/AdbBridge.cpp
    windows/app/PreviewRenderer.cpp
    windows/app/VirtualCameraHost.cpp
)

# Target 2: Aplicación Windows nativa Win32 Dark Mode + Tray (ZeroCam.exe)
add_executable(ZeroCam WIN32
    ${ZEROCAM_ENGINE_SOURCES}
    windows/app/AppConfig.cpp
    windows/app/UiTheme.cpp
    windows/app/SettingsDrawer.cpp
    windows/app/EffectsDrawer.cpp
    windows/app/MainWindow.cpp
    windows/app/main.cpp
    windows/app/ZeroCam.rc
)

target_include_directories(ZeroCam PRIVATE
    windows/shared
    windows/app
)

target_link_libraries(ZeroCam PRIVATE
    mfplat
    mf
    mfuuid
    mfsensorgroup
    d3d11
    d3dcompiler
    dxgi
    dwmapi
    windowsapp
    ws2_32
    bcrypt
    crypt32
    comctl32
    gdiplus
    shlwapi
    shell32
    ole32
    oleaut32
    user32
    gdi32
    uxtheme
    advapi32
)

# Target 3: Test automatizado end-to-end (zerocam_selftest.exe)
add_executable(zerocam_selftest
    ${ZEROCAM_ENGINE_SOURCES}
    windows/app/AppConfig.cpp
    windows/tests/zerocam_selftest.cpp
)

target_compile_definitions(zerocam_selftest PRIVATE
    ZEROCAM_ALLOW_IPC_ENV_OVERRIDE=1
)

target_include_directories(zerocam_selftest PRIVATE
    windows/shared
    windows/app
    windows/vcam_source
)

target_link_libraries(zerocam_selftest PRIVATE
    mfplat
    mf
    mfuuid
    mfsensorgroup
    d3d11
    d3dcompiler
    dxgi
    dwmapi
    windowsapp
    ws2_32
    bcrypt
    crypt32
    comctl32
    shlwapi
    shell32
    ole32
    oleaut32
    user32
    gdi32
    uxtheme
    advapi32
)
