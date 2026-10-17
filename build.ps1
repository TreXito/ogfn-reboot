# OGFN build -- builds Reboot.dll (gameserver) + OgfnInjector.exe.
# Requires Visual Studio 2022 with the "Desktop development with C++" workload (v143 toolset).
# Usage:  powershell -ExecutionPolicy Bypass -File .\build.ps1

$ErrorActionPreference = "Stop"
$here    = Split-Path -Parent $MyInvocation.MyCommand.Path
$sln     = Join-Path $here "Project Reboot 3.0.sln"
$vcxproj = Join-Path $here "Injector\OgfnInjector.vcxproj"

if (-not (Test-Path $sln))     { Write-Host "missing: $sln"; exit 1 }
if (-not (Test-Path $vcxproj)) { Write-Host "missing: $vcxproj"; exit 1 }

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    Write-Host "vswhere not found. Install VS2022 (v17) with 'Desktop development with C++'."
    Write-Host "  https://visualstudio.microsoft.com/downloads/  -> 'Visual Studio Community 2022'"
    exit 1
}

$msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild `
             -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
if (-not $msbuild) {
    Write-Host "MSBuild not found. Install the VS2022 C++ workload first."
    exit 1
}

Write-Host "== [1/2] Reboot.dll (gameserver) =="
& $msbuild $sln /p:Configuration=Release /p:Platform=x64 /m /v:m /nologo
if ($LASTEXITCODE -ne 0) { Write-Host "game DLL build failed ($LASTEXITCODE)."; exit $LASTEXITCODE }

Write-Host "== [2/2] OgfnInjector.exe =="
& $msbuild $vcxproj /p:Configuration=Release /p:Platform=x64 /m /v:m /nologo
if ($LASTEXITCODE -ne 0) { Write-Host "injector build failed ($LASTEXITCODE)."; exit $LASTEXITCODE }

$out = Join-Path $here "out"
New-Item -ItemType Directory -Force -Path $out | Out-Null

$dll = Get-ChildItem -Path $here -Recurse -Filter "Project Reboot 3.0.dll" -ErrorAction SilentlyContinue | Select-Object -First 1
$exe = Get-ChildItem -Path $here -Recurse -Filter "OgfnInjector.exe" -ErrorAction SilentlyContinue | Select-Object -First 1

if (-not $dll -or -not $exe) {
    Write-Host "built, but could not locate outputs -- build config changed upstream?"; exit 1
}

Copy-Item $dll.FullName (Join-Path $out "Reboot.dll")      -Force
Copy-Item $exe.FullName (Join-Path $out "OgfnInjector.exe") -Force

Write-Host ""
Write-Host "done. outputs:"
Write-Host ("  " + (Join-Path $out "Reboot.dll"))
Write-Host ("  " + (Join-Path $out "OgfnInjector.exe"))
Write-Host "next: .\start.ps1 -GameDir <your 3.5 client dir>"
