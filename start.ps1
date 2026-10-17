# OGFN play -- one action: spins up FortniteClient with the gameserver DLL injected.
# Usage:  powershell -ExecutionPolicy Bypass -File .\start.ps1 -GameDir "C:\ogfn\3.5" [-NoLog]
# Tip:    leave -NoLog off to get a readable server log window (UE -log).

param(
    [string]$GameDir = "C:\ogfn\3.5",
    [switch]$NoLog,
    [string]$DllPath = "$PSScriptRoot\out\Reboot.dll"
)

$ErrorActionPreference = "Stop"

$injector = Join-Path $PSScriptRoot "out\OgfnInjector.exe"
if (-not (Test-Path $injector)) { Write-Host "OgfnInjector.exe missing. Run .\build.ps1 first."; exit 1 }
if (-not (Test-Path $DllPath)) { Write-Host "Reboot.dll missing at '$DllPath'. Run .\build.ps1 first."; exit 1 }

if (-not (Test-Path $GameDir)) {
    Write-Host "client dir '$GameDir' not found. Run .\download-client.ps1 or pass -GameDir."
    exit 1
}

$exe = Get-ChildItem -Path $GameDir -Recurse -Filter "FortniteClient-Win64-Shipping.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $exe) { Write-Host "FortniteClient-Win64-Shipping.exe not found under '$GameDir'."; exit 1 }

$dll = (Resolve-Path $DllPath).Path
$client = $exe.FullName
$args = @()
if (-not $NoLog) { $args = @("-log") }

Write-Host "client:  $client"
Write-Host "dll:     $dll"
Write-Host ""
& $injector $dll $client @args

Write-Host ""
Write-Host "in-game: the console overlay toggles with your GUI key if configured;"
Write-Host "match starts on its own (warmup 1 player, 20 bots). The 'OGFN Locker' tab has all "
Write-Host "Season 3 skins + emotes -- pick one, Apply, live."
