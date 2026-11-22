param(
    [string]$Version = "1.2.5",
    [string]$SignThumbprint = "",
    [switch]$NoSign,
    [switch]$SkipAndroid,
    [switch]$SkipInstaller
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$distDir = Join-Path $root "dist"
$pkgDir = Join-Path $distDir "ZeroCam-Windows-x64"

# Asegura que Ninja recompile en app, vcam_source, shared y tests si algún header cambió recientemente (MSVC /showIncludes en español).
$latestHeader = Get-ChildItem (Join-Path $root "windows") -Include *.h,*.hpp -Recurse |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($latestHeader) {
    foreach ($subDir in @("windows\app", "windows\vcam_source", "windows\shared", "windows\tests")) {
        $fullSub = Join-Path $root $subDir
        if (Test-Path $fullSub) {
            Get-ChildItem $fullSub -Filter *.cpp | ForEach-Object {
                if ($_.LastWriteTime -lt $latestHeader.LastWriteTime) {
                    $_.LastWriteTime = Get-Date
                }
            }
        }
    }
}

# Localiza signtool.exe y certificado Authenticode local opcional (prioriza CN=Morales Dev Code Signing ECC).
$signtool = $null
$resolvedThumb = ""
if (-not $NoSign) {
    $stItem = Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\bin\*\x64\signtool.exe" -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | Select-Object -First 1
    if ($stItem) { $signtool = $stItem.FullName }

    if ($signtool) {
        if (-not [string]::IsNullOrWhiteSpace($SignThumbprint)) {
            $cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Thumbprint -eq $SignThumbprint.Replace(" ", "") -and $_.HasPrivateKey } | Select-Object -First 1
            if (-not $cert) { throw "No se encontró certificado con huella $SignThumbprint y clave privada en Cert:\CurrentUser\My." }
            $resolvedThumb = $cert.Thumbprint
        } else {
            $cert = Get-ChildItem Cert:\CurrentUser\My |
                Where-Object { $_.Subject -eq "CN=Morales Dev Code Signing ECC" -and $_.HasPrivateKey } |
                Sort-Object NotAfter -Descending | Select-Object -First 1
            if (-not $cert) {
                $cert = Get-ChildItem Cert:\CurrentUser\My |
                    Where-Object { $_.Subject -eq "CN=Morales Dev Code Signing" -and $_.HasPrivateKey } |
                    Sort-Object NotAfter -Descending | Select-Object -First 1
            }
            if ($cert) { $resolvedThumb = $cert.Thumbprint }
        }
    }
}

function Invoke-AuthenticodeSign([string]$FilePath) {
    if (-not $signtool -or -not $resolvedThumb) { return }
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & $signtool sign /sha1 $resolvedThumb /fd SHA256 /d "ZeroCam" /tr "http://timestamp.digicert.com" /td SHA256 $FilePath | Out-Null
    $rc = $LASTEXITCODE
    if ($rc -ne 0) {
        Write-Host "[ZeroCam] Aviso: timestamp RFC3161 no disponible; firmando SHA-256 local sobre $(Split-Path -Leaf $FilePath)..."
        & $signtool sign /sha1 $resolvedThumb /fd SHA256 /d "ZeroCam" $FilePath | Out-Null
        $rc = $LASTEXITCODE
    }
    $ErrorActionPreference = $prevEap
    if ($rc -ne 0) {
        throw "signtool fallo al firmar $FilePath (exit $rc)."
    }
}

Write-Host "[ZeroCam] 1/5 Compilando Windows Release x64..."
& (Join-Path $PSScriptRoot "build_windows.ps1") -Config Release

$winBuildDir = Join-Path $root "build\windows-x64-release"
$exeSrc = Join-Path $winBuildDir "ZeroCam.exe"
$dllSrc = Join-Path $winBuildDir "ZeroCamSource.dll"
if (-not (Test-Path $exeSrc) -or -not (Test-Path $dllSrc)) {
    throw "Faltan ZeroCam.exe o ZeroCamSource.dll en $winBuildDir"
}

Write-Host "[ZeroCam] 2/5 Localizando adb.exe portable..."
$adbCandidates = @(
    (Join-Path $env:LOCALAPPDATA "Android\Sdk\platform-tools")
)
$wingetPkg = Get-ChildItem (Join-Path $env:LOCALAPPDATA "Microsoft\WinGet\Packages\Google.PlatformTools*\platform-tools") -ErrorAction SilentlyContinue | Select-Object -First 1
if ($wingetPkg) { $adbCandidates += $wingetPkg.FullName }
$adbCmd = Get-Command "adb.exe" -ErrorAction SilentlyContinue
if ($adbCmd) { $adbCandidates += (Split-Path $adbCmd.Source) }

$adbDir = $adbCandidates | Where-Object { $_ -and (Test-Path (Join-Path $_ "adb.exe")) } | Select-Object -First 1
if (-not $adbDir) {
    throw "No se encontró adb.exe para empaquetar distribución portable."
}

Write-Host "[ZeroCam] 3/5 Preparando paquete Windows x64 en $pkgDir..."
if (Test-Path $pkgDir) { Remove-Item $pkgDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $pkgDir, (Join-Path $pkgDir "adb") | Out-Null

$pkgExe = Join-Path $pkgDir "ZeroCam.exe"
$pkgDll = Join-Path $pkgDir "ZeroCamSource.dll"
Copy-Item $exeSrc $pkgExe -Force
Copy-Item $dllSrc $pkgDll -Force

if ($signtool -and $resolvedThumb) {
    Write-Host "[ZeroCam] Firmando Authenticode SHA-256 (thumbprint $resolvedThumb)..."
    Invoke-AuthenticodeSign $pkgExe
    Invoke-AuthenticodeSign $pkgDll
}

foreach ($adbFile in @("adb.exe", "AdbWinApi.dll", "AdbWinUsbApi.dll")) {
    $srcFile = Join-Path $adbDir $adbFile
    if (-not (Test-Path $srcFile)) {
        throw "Falta archivo requerido de ADB: $srcFile"
    }
    Copy-Item $srcFile (Join-Path $pkgDir "adb\$adbFile") -Force
}

$readmeText = @"
ZeroCam v$Version — Guía rápida para cualquier usuario
======================================================

Requisitos:
- PC: Windows 11 x64 para Cámara Virtual nativa en OBS, Discord, Zoom o Meet.
  (En Windows 10 funciona como Receptor + Vista Previa en pantalla).
- Móvil: Android 8.0+ con ZeroCam-Android-v$Version.apk instalado.
  (Al migrar desde v1.1.0 desinstala la versión anterior en Android antes de instalar v$Version por el cambio a firma ECDSA P-256).

Seguridad Wire v2 (cifrado extremo a extremo ECC P-256 + AES-256-GCM):
- En el primer arranque, ZeroCam PC genera automáticamente un PIN Wi-Fi de 6 dígitos
  y una identidad criptográfica única (huella FP visible en Ajustes -> Red y Seguridad).

Conexión por Cable USB (recomendada, cero latencia, sin PIN):
1. En Android activa Opciones de desarrollador -> Depuración USB.
2. Conecta el cable y acepta el aviso "¿Permitir depuración USB?" en el teléfono.
3. En ZeroCam (PC) selecciona "Cable USB" y pulsa "Iniciar Cámara".
4. En ZeroCam Mobile elige "Cable USB" y pulsa "Iniciar Transmisión".
   (La huella FP del PC se vincula automáticamente para permitir salto seguro a Wi-Fi).

Conexión por Wi-Fi (cifrada AES-256-GCM):
1. Asegúrate de que PC y teléfono estén en la misma red Wi-Fi privada.
2. En ZeroCam PC abre Ajustes -> Red y Seguridad -> "Ver PIN" para consultar tu PIN Wi-Fi y huella FP.
3. En ZeroCam Mobile introduce el PIN en Ajustes, elige "Wi-Fi", confirma la huella FP del PC e inicia.
"@
Set-Content -Path (Join-Path $pkgDir "LEEME.txt") -Value $readmeText -Encoding UTF8

$zipPath = Join-Path $distDir "ZeroCam-Windows-x64-Portable-v$Version.zip"
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
Compress-Archive -Path (Join-Path $pkgDir "*") -DestinationPath $zipPath -CompressionLevel Optimal

if (-not $SkipAndroid) {
    Write-Host "[ZeroCam] 4/5 Compilando y firmando APK Release (ECDSA P-256 v2+v3)..."
    $jdk21 = Get-ChildItem "C:\Program Files\Microsoft\jdk-21*" -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending | Select-Object -First 1
    if (-not $jdk21) { throw "No se encontró JDK 21 en C:\Program Files\Microsoft\jdk-21*" }

    $gradleBat = Get-ChildItem "$env:USERPROFILE\.gradle\wrapper\dists\gradle-8.11.1-bin\*\gradle-8.11.1\bin\gradle.bat" -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if (-not $gradleBat) { throw "No se encontró gradle-8.11.1 en $env:USERPROFILE\.gradle\wrapper\dists" }

    $androidDir = Join-Path $root "android"
    $prevJavaHome = $env:JAVA_HOME
    try {
        $env:JAVA_HOME = $jdk21.FullName
        Push-Location $androidDir
        # PowerShell 5.1 convierte avisos de stderr (p. ej. "SDK XML version 4") en errores fatales
        # bajo $ErrorActionPreference=Stop si la salida está redirigida; se decide por el exit code.
        $prevEap = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        & $gradleBat.FullName testDebugUnitTest assembleRelease --no-daemon
        $ErrorActionPreference = $prevEap
        if ($LASTEXITCODE -ne 0) {
            throw "Fallo compilación Gradle Release (exit $LASTEXITCODE)."
        }
    } finally {
        Pop-Location
        $env:JAVA_HOME = $prevJavaHome
    }

    $apkOut = Join-Path $androidDir "app\build\outputs\apk\release\app-release.apk"
    if (-not (Test-Path $apkOut)) {
        $unsignedApk = Join-Path $androidDir "app\build\outputs\apk\release\app-release-unsigned.apk"
        if (Test-Path $unsignedApk) {
            throw "Se generó APK sin firmar ($unsignedApk). Verifica android/keystore.properties y android/zerocam-release-ec.jks."
        }
        throw "No se encontró el APK generado en $apkOut"
    }

    $destApk = Join-Path $distDir "ZeroCam-Android-v$Version.apk"
    Copy-Item $apkOut $destApk -Force

    $apksigner = Get-ChildItem (Join-Path $env:LOCALAPPDATA "Android\Sdk\build-tools\*\apksigner.bat") -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $apksigner) {
        throw "No se encontró apksigner.bat en Android SDK build-tools para verificar $destApk"
    }
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $verifyOut = & $apksigner.FullName verify --verbose --print-certs $destApk 2>&1
    $verifyRc = $LASTEXITCODE
    $ErrorActionPreference = $prevEap
    $verifyOut | ForEach-Object { Write-Host $_ }
    if ($verifyRc -ne 0) {
        throw "Falló verificación apksigner sobre $destApk (exit $verifyRc)"
    }
    $verifyJoined = ($verifyOut | Out-String)
    if ($verifyJoined -notmatch "Verified using v1 scheme \(JAR signing\): false" -or
        $verifyJoined -notmatch "Verified using v2 scheme \(APK Signature Scheme v2\): true" -or
        $verifyJoined -notmatch "Verified using v3 scheme \(APK Signature Scheme v3\): true") {
        throw "Esquemas de firma APK inesperados (se requiere v1=false, v2=true, v3=true)."
    }
}

if (-not $SkipInstaller) {
    Write-Host "[ZeroCam] 5/5 Generando instalador Inno Setup..."
    $isccCandidates = @(
        "C:\Program Files (x86)\Inno Setup 6\ISCC.exe",
        "C:\Program Files\Inno Setup 6\ISCC.exe",
        (Join-Path $env:LOCALAPPDATA "Programs\Inno Setup 6\ISCC.exe")
    )
    $isccCmd = Get-Command "ISCC.exe" -ErrorAction SilentlyContinue
    if ($isccCmd) { $isccCandidates = @($isccCmd.Source) + $isccCandidates }

    $iscc = $isccCandidates | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
    if (-not $iscc) {
        throw "No se encontró ISCC.exe (Inno Setup 6). Instálalo o usa -SkipInstaller."
    }

    $isccArgs = @("/DMyAppVersion=$Version")
    if ($signtool -and $resolvedThumb) {
        $isccArgs += "/DEnableSign"
        $isccArgs += "/Szc=`$q$signtool`$q sign /sha1 $resolvedThumb /fd SHA256 /d `$qZeroCam`$q `$f"
    }
    $isccArgs += (Join-Path $root "installer\ZeroCam.iss")

    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    & $iscc @isccArgs
    $ErrorActionPreference = $prevEap
    if ($LASTEXITCODE -ne 0) {
        throw "ISCC.exe falló con exit code $LASTEXITCODE"
    }
}

Write-Host "`n[ZeroCam] Entregables generados en ${distDir}:"
Get-ChildItem $distDir -File | Sort-Object Name | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize
