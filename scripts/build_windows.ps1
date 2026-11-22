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

$vcvars = "$vsBase\VC\Auxiliary\Build\vcvars64.bat"
$cmake  = "$vsBase\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$ninja  = "$vsBase\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
$buildDir = Join-Path $root "build\windows-x64-$($Config.ToLower())"

Write-Host "[ZeroCam] Compilando ($Config x64) en $buildDir..."
$cmd = "`"$vcvars`" >nul && `"$cmake`" -S `"$root`" -B `"$buildDir`" -G Ninja -DCMAKE_BUILD_TYPE=$Config -DCMAKE_MAKE_PROGRAM=`"$ninja`" && `"$cmake`" --build `"$buildDir`" --parallel"
cmd /c $cmd
if ($LASTEXITCODE -ne 0) {
    throw "Fallo la compilacion CMake/Ninja (exit $LASTEXITCODE)."
}

Get-ChildItem $buildDir -Include "ZeroCam.exe","ZeroCamSource.dll","zerocam_selftest.exe" -Recurse |
    Select-Object Name, Length, FullName | Format-Table -AutoSize
