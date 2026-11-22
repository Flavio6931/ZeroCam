param(
    [ValidateSet("Release", "Debug")]
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

$vsCandidates = @(
    "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools",
    "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools"
)
$vsBase = $vsCandidates | Where-Object { Test-Path "$_\VC\Auxiliary\Build\vcvars64.bat" } | Select-Object -First 1
if (-not $vsBase) {
    throw "No se encontro vcvars64.bat de Visual Studio Build Tools."
}

$vcvars64 = "$vsBase\VC\Auxiliary\Build\vcvars64.bat"
$vcvars32 = "$vsBase\VC\Auxiliary\Build\vcvars32.bat"
$cmake    = "$vsBase\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja    = "$vsBase\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
$buildDir = Join-Path $root "build\windows-x64-$($Config.ToLower())"
$buildDir32 = Join-Path $root "build\windows-x86-$($Config.ToLower())"

Write-Host "[ZeroCam] Compilando ($Config x64) en $buildDir..."
$cmd64 = "`"$vcvars64`" >nul && `"$cmake`" -S `"$root`" -B `"$buildDir`" -G Ninja -DCMAKE_BUILD_TYPE=$Config -DZEROCAM_DSHOW_ONLY=OFF -DCMAKE_MAKE_PROGRAM=`"$ninja`" && `"$cmake`" --build `"$buildDir`" --parallel"
cmd /c $cmd64
if ($LASTEXITCODE -ne 0) {
    throw "Fallo la compilacion CMake/Ninja x64 (exit $LASTEXITCODE)."
}

if (Test-Path $vcvars32) {
    Write-Host "[ZeroCam] Compilando DirectShow ($Config x86) en $buildDir32..."
    $cmd32 = "`"$vcvars32`" >nul && `"$cmake`" -S `"$root`" -B `"$buildDir32`" -G Ninja -DCMAKE_BUILD_TYPE=$Config -DZEROCAM_DSHOW_ONLY=ON -DCMAKE_MAKE_PROGRAM=`"$ninja`" && `"$cmake`" --build `"$buildDir32`" --parallel"
    cmd /c $cmd32
    if ($LASTEXITCODE -ne 0) {
        throw "Fallo la compilacion CMake/Ninja x86 DirectShow (exit $LASTEXITCODE)."
    }
}

Get-ChildItem $buildDir, $buildDir32 -Include "ZeroCam.exe","ZeroCamSource.dll","ZeroCamDShow64.dll","ZeroCamDShow32.dll","zerocam_selftest.exe" -Recurse -ErrorAction SilentlyContinue |
    Select-Object Name, Length, FullName | Format-Table -AutoSize
