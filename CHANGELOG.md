# Changelog — ZeroCam

Todos los cambios relevantes del proyecto se documentan en este archivo.

## [1.2.4] - 2026-11-21
### Mejorado
- **UI Windows sin congelamientos (`MainWindow`):** ciclo de vida de cámara virtual (`MFCreateVirtualCamera`), motor de ingesta (`StreamIngest`), registro elevado y reconexión USB movidos a un hilo trabajador MTA dedicado con indicadores transitorios inmediatos (`Iniciando...`, `Deteniendo...`, `Actualizando...`).
- **Cero I/O de disco en el timer de UI:** eliminada la relectura repetida de `ZeroCamSource.dll` cada 500 ms usando caché de estado COM/DLL evaluada al iniciar o registrar.

## [1.2.3] - 2026-11-21
### Mejorado
- **Cierre determinista del daemon `adb.exe` embebido:** `AdbBridge` asigna un `Job Object` (`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`) y ejecuta `ShutdownBundledAdb()` al salir de `ZeroCam.exe` (apagando solo la instancia embebida por ruta exacta sin afectar otros ADB del sistema), validado con nueva prueba automatizada en `zerocam_selftest`.

## [1.2.2] - 2026-11-21
### Seguridad y Empaquetado
- **Firma Windows ECDSA P-256 (`sha256ECDSA`):** soporte prioritario de certificado `CN=Morales Dev Code Signing ECC` en `package_release.ps1` para `.exe`, `.dll` e instalador.
- **APK Release depurado:** exclusión de `**/*.kotlin_builtins` en Gradle reduciendo metadatos residuales a `0`.

## [1.2.1] - 2026-11-21
### Corregido
- **Prioridad Cable USB real sobre Wi-Fi:** aumentado timeout de arranque en frío para `adb devices` (`12000 ms`) y `reverse` (`8000 ms`) con cancelación cooperativa; evaluación en vivo posterior a `SetupUsbTunnel` para emitir `{"cmd":"prefer_usb"}` si Wi-Fi ganó la carrera inicial; ráfaga de reintentos tras `WM_DEVICECHANGE` y fallback al puerto base `5000` al intentar túnel `127.0.0.1` desde Android.

## [1.2.0] - 2026-11-21
### Seguridad (`ZeroCam Wire v2`) y Experiencia 1-Clic
- **Criptografía extremo a extremo:** handshake autenticado de 4 mensajes **ECDH / ECDSA P-256**, derivación **PBKDF2-HMAC-SHA256 (100k)** + **HKDF-SHA256**, y sesión cifrada **AES-256-GCM** con cabecera de 20 B como AAD; Discovery UDP v2 firmado (`ZCAM_DISCOVER_V2` / `ZCAM_HOST_V2`) y bloqueo por IP tras 5 fallos de PIN.
- **Protección en reposo y plataforma:** PIN Wi-Fi enmascarado protegido con **Windows DPAPI** y **AndroidKeyStore AES-256-GCM**; carga de modelo RVM ONNX desde memoria (`LoadFromStream`), hardening IPC en `ipc_ring.bin` (DACL + Mandatory Label + anti-reparse/hardlink), mitigaciones PE (`/guard:cf`, `/CETCOMPAT`, `/DEPENDENTLOADFLAG:0x800`, `/MT`) y APK firmado con **ECDSA P-256 (`v2+v3`, `v1=false`)**.
- **Control 1-Clic y failover automático:** arranque/parada remotos desde PC (`stop_stream`, `prefer_usb`), salto automático **Cable USB ↔ Wi-Fi** y vista previa móvil activa por defecto.

## [1.1.0] - 2026-11-20
### Añadido
- **Retrato IA RVM MobileNetV3 FP16 + Bokeh Óptico GPU:** segmentación temporal GPU DirectML (`512×288`) con bokeh de disco óptico Vogel (`960×540`), modo óptico puro `f/1.8` y modo `IA Sin Halo`.
- **Control remoto de cámara e interfaz:** prioridad de rostros, enfoque táctil/clic, bloqueo AF, zoom por rueda del mouse, compensación de exposición (EV), cambio de lente, linterna y cajón de filtros.

## [1.0.0] - 2026-11-20
### Inicial
- Primera versión funcional: pipeline H.264 hardware Zero-Copy en Windows 11 (`CLSID_MSH264DecoderMFT` + `D3D11VideoProcessor` + `MFCreateVirtualCamera` / `ZeroCamSource.dll`) y Android (`Camera2` → `MediaCodec` Surface-to-Surface), con auto-rotación y control remoto de resolución, FPS y bitrate.
