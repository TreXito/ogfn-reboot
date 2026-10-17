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

# Boot arguments the 2018 client needs to skip the real Epic ecosystem. Without these
# the naked shipping client phones home, spawns the installed Epic Games launcher, and
# bows out (seen live on 2026-10-17). Mirrors what Reboot-Launcher itself passes:
# -AUTH_LOGIN=<username>, -AUTH_PASSWORD=Rebooted, -AUTH_TYPE=epic, -epicapp=Fortnite.
$args = @(
    "-AUTH_LOGIN=TreXito",
    "-AUTH_PASSWORD=Rebooted",
    "-AUTH_TYPE=epic",
    "-epicapp=Fortnite"
)
if (-not $NoLog) { $args = $args + @("-log") }

# --- infrastructure: backend on 3551, mitmproxy on 8980 (both scheduled tasks) ---
function Test-Port([int]$p) {
    try {
        $c = New-Object Net.Sockets.TcpClient("127.0.0.1", $p)
        $c.Close()
        return $true
    } catch { return $false }
}

if (-not (Test-Port 3551)) { schtasks /run /tn OGFN-BACKEND | Out-Null; Start-Sleep 4 }
if (-not (Test-Port 3551)) { Write-Host "backend on 3551 still down; run schtasks /run /tn OGFN-BACKEND"; exit 1 }
if (-not (Test-Port 8980)) { schtasks /run /tn OGFN-PROXY | Out-Null; Start-Sleep 5 }
if (-not (Test-Port 8980)) { Write-Host "proxy on 8980 still down; run schtasks /run /tn OGFN-PROXY"; exit 1 }

# Session-scoped system proxy: every HTTPS app on the box routes through mitmproxy while
# this script runs; it is restored when the game exits. Do not close this window.
$proxyKey = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Internet Settings"
$oldProxyEnable = (Get-ItemProperty -Path $proxyKey -Name ProxyEnable -ErrorAction SilentlyContinue).ProxyEnable
$oldProxyServer = (Get-ItemProperty -Path $proxyKey -Name ProxyServer -ErrorAction SilentlyContinue).ProxyServer
Set-ItemProperty -Path $proxyKey -Name ProxyEnable -Value 1
Set-ItemProperty -Path $proxyKey -Name ProxyServer -Value "127.0.0.1:8980"

try {
    Write-Host "client:  $client"
    Write-Host "dll:     $dll"
    Write-Host "proxy:   127.0.0.1:8980 (restored when the game exits)"
    Write-Host ""
    & $injector $dll $client @args

    Write-Host ""
    Write-Host "in-game: the console overlay toggles with your GUI key if configured;"
    Write-Host "match starts on its own (warmup 1 player, 20 bots). The 'OGFN Locker' tab has all"
    Write-Host "Season 3 skins + emotes -- pick one, Apply, live."
    Write-Host ""
    Write-Host "Waiting for FortniteClient to exit (proxy stays on until then)..."
    while (Get-Process -Name "FortniteClient-Win64-Shipping" -ErrorAction SilentlyContinue) {
        Start-Sleep -Seconds 10
    }
}
finally {
    Set-ItemProperty -Path $proxyKey -Name ProxyEnable -Value ([int]($oldProxyEnable))
    if ($oldProxyServer) { Set-ItemProperty -Path $proxyKey -Name ProxyServer -Value $oldProxyServer }
    else { Remove-ItemProperty -Path $proxyKey -Name ProxyServer -ErrorAction SilentlyContinue }
    Write-Host "proxy restored."
}
