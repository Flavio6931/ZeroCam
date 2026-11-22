# Changelog — ZeroCam

Todos los cambios relevantes del proyecto se documentan en este archivo.

## [1.3.5] - 2026-11-21
### Corregido
- **Instalador sin aviso de reinicio de Windows tras reemplazo en caliente de DLL:**
  - `CopyAndHardenDll()` (`windows/app/VirtualCameraHost.cpp`) deja de llamar a `MoveFileExW(..., nullptr, MOVEFILE_DELAY_UNTIL_REBOOT)` sobre backups `.old.*` cuando Discord/OBS mantiene abierta una DLL anterior; `CleanupStaleOldDlls()` limpia los `.old*` residuales en arranques o desinstalaciones posteriores sin ensuciar `PendingFileRenameOperations`.
  - `installer/ZeroCam.iss` añade `RestartIfNeededByRun=no` para suprimir el cuadro final *"su sistema debe reiniciarse"* al completar la instalación.

## [1.3.4] - 2026-11-21
### Corregido
- **Fluidez real a 30/60 fps en Discord (sin tirones a ~15 fps):**
  - `ZeroCamDShowFilter` y `ZeroCamMediaStream`: sustituido el pacing tosco de `GetTickCount64()` (`15.6 ms`) y el timeout de `33 ms` en vivo (que inyectaba un frame duplicado viejo justo `1–3 ms` antes de llegar el frame real del sensor a `33.8 ms`, haciendo que WebRTC/Discord descartara los frames nuevos) por **pacing QPC sub-milisegundo** (`QueryPerformanceCounter` + timer de `1 ms`) y **keepalive en vivo de `120 ms`**, reservando el refresco de `33 ms` solo para standby.

## [1.3.3] - 2026-11-21
### Corregido y Mejorado
- **Negociación HD 16:9 garantizada en Discord (sin zoom ni borrosidad `640×480`):**
  - `ZeroCamDShowFilter`: eliminados los 3 modos `640×480` (`4:3`) de las capacidades anunciadas (`kDShowModes[]`), dejando exclusivamente los **12 modos HD `16:9`** (`1280×720` y `1920×1080` a `30/60 fps` en `I420`, `NV12` y `YUY2`), con `MinOutputSize = 1280×720`. Así Discord abre la cámara mínimo en `1280×720` encuadre completo sin recortar bordes.
- **Reemplazo en caliente de DLLs bloqueadas en actualización (`VirtualCameraHost::CopyAndHardenDll`):**
  - Si Discord u OBS mantiene mapeada `ZeroCamDShow64/32.dll` al actualizar, la DLL bloqueada se renombra automáticamente a `*.old.<tick>` (marcada con `MOVEFILE_DELAY_UNTIL_REBOOT` y limpieza previa `CleanupStaleOldDlls`) para copiar la nueva versión al instante sin mostrar el aviso `Permiso omitido`.

## [1.3.2] - 2026-11-21
### Mejorado
- **Calidad visual nítida en Discord (`I420` nativo + supersampling bilineal sin GPU extra):**
  - `ZeroCamDShow64/32.dll` (`ZeroCamDShowFilter` + `DllMain`): expone **`I420` (`kMediaSubtypeI420`)** como formato nativo prioritario para WebRTC/Discord además de `NV12` y `YUY2`, evitando doble conversión de espacio de color.
  - Sustituido el muestreo *nearest-neighbor* por **filtro bilineal entero rápido** alineado al centro (luma y croma sin saltos de bloque en `I420`, `NV12` y `YUY2`) con caché de tablas y recorte centrado por relación de aspecto (evita estiramiento en `4:3`) en `ZeroCamDShow64/32.dll` y el fallback CPU de `ZeroCamSource.dll`.

## [1.3.1] - 2026-11-21
### Corregido y Endurecido
- **Detección garantizada en Discord (Windows 11 + Windows 10):**
  - `VirtualCameraHost`: ahora registra el filtro DirectShow (`ZeroCamDShow64.dll` 64-bit + `ZeroCamDShow32.dll` 32-bit) tanto en Windows 10 como en Windows 11, dado que el módulo de video de Discord (`discord_voice.node`) enumera únicamente DirectShow (`CLSID_VideoInputDeviceCategory` / `devenum.dll`) en ambas versiones de Windows, manteniendo en paralelo Media Foundation (`ZeroCamSource.dll`) para Cámara de Windows y apps UWP.
  - Eliminación automática de cámaras MF duplicadas en Windows 11 corrigiendo `IsSystemMfCameraPresent()` por prefijo (`"ZeroCam Virtual Camera"`) y limpiando variantes anteriores en registro/desregistro.
- **Hardening DirectShow (`ZeroCamDShowFilter`):** negociación exacta de `cbBuffer` en `Connect()` con limpieza de `pReceivePin->Disconnect()` ante fallos, `VFW_S_CANT_CUE` en `GetState(State_Paused)` y timestamps monotónicos basados en `QueryPerformanceCounter`.

## [1.3.0] - 2026-11-21
### Añadido y Mejorado
- **Compatibilidad universal Windows 10 + Windows 11 y detección fiable en Discord:**
  - Carga dinámica de `mfsensorgroup.dll` eliminando el enlace estático exclusivo de Windows 11 en `ZeroCam.exe`.
  - **Windows 11:** registro persistente de sistema (`MFVirtualCameraLifetime_System` + `MFVirtualCameraAccess_AllUsers` + `{ KSCATEGORY_VIDEO_CAMERA, KSCATEGORY_CAPTURE }`) evitando aislamiento UAC con Discord, más `runasoriginaluser` en Inno Setup.
  - **Windows 10:** nuevo filtro DirectShow clean-room sin BaseClasses (`ZeroCamDShow64.dll` x64 y `ZeroCamDShow32.dll` x86) con `IAMStreamConfig`, `IKsPropertySet` (`PIN_CATEGORY_CAPTURE`) y soporte **NV12 + YUY2**, sin generar cámaras duplicadas en Windows 11.
- **Pipeline Anti-Lag extremo a extremo (`0%` GPU extra):**
  - Android: `KEY_PRIORITY = 0` en MediaCodec H.264, `sendBufferSize` acotado (`128 KB` Wi-Fi / `256 KB` USB) sin buffer intermedio de `128 KB`, y cola de `4` frames en hilo emisor dedicado (`ZeroCamTcpSender`) con descarte de P-frames atrasados y solicitud automática de IDR ante congestión.
  - Windows PC y cámara virtual: espera reactiva de `2 ms` (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`) en `ZeroCamMediaStream` y DirectShow, salto de renderizado de frames atrasados en backlog TCP (`FIONREAD` + `skipPublish`), liberación temprana de `KeyedMutex` en fallback CPU y sondeo Wi-Fi→USB relajado a `15 s`.

## [1.2.5] - 2026-11-21
### Mejorado
- **Minimizar directo a bandeja del sistema (`MainWindow`):** pulsar el botón `—` (`SC_MINIMIZE`) oculta la ventana directamente al icono del área de notificación y detiene el renderizado de vista previa (`0.0% GPU 3D`) manteniendo activa la cámara virtual.
- **Restauración rápida con 1 clic:** un clic izquierdo (`WM_LBUTTONUP`) o doble clic en el icono de bandeja restaura la ventana preservando su estado previo (normal o maximizado).

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
