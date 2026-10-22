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

# Boot arguments the 2018 client needs. Discovered the hard way 2026-10-17:
# without -obfuscationid and the AUTH_* set the shipping client exits cleanly at ~2s
# (launcher-style bail, exit code 0, four threads); with them it boots fully.
# AUTH_TYPE=exchangecode + the epic* family mirrors what the installed launcher passes.
$bootArgs = @(
    "-obfuscationid=4EzfLt3PhdU2nP5SiEu_vtZHdsaMRQ",
    "-AUTH_LOGIN=TreXito",
    "-AUTH_PASSWORD=Rebooted",
    "-AUTH_TYPE=exchangecode",
    "-epicapp=Fortnite",
    "-epicenv=Prod",
    "-EpicPortal",
    "-epicusername=TreXito",
    "-epiclocale=en",
    "-epicsandboxid=fn"
)
if (-not $NoLog) { $bootArgs = $bootArgs + @("-log") }

# --- infrastructure: backend on 3551, mitmproxy on 8980 (both scheduled tasks) ---
function Test-Port([int]$p) {
    try {
        $c = New-Object Net.Sockets.TcpClient("127.0.0.1", $p)
        $c.Close()
        return $true
    } catch { return $false }
}

# Services are started DIRECTLY, not via schtasks.
#
# The OGFN-* tasks are registered RunLevel=HighestAvailable (they were created over an
# elevated SSH session). That combination is what made double-clicking the shortcut fail
# with "Access is denied": an unelevated PowerShell cannot start a HighestAvailable task.
# Nothing in this path actually needs elevation -- node, mitmdump, an HKCU proxy write and
# an injector targeting a process this script spawned itself are all user-level -- so the
# tasks are bypassed entirely and the same commands are launched as plain child processes.
# The tasks remain registered for headless/automated runs (scripts/match.ps1 uses them).
function Start-OgfnService([string]$name, [string]$cmd, [int]$port, [int]$waitSec) {
    Write-Host "starting $name ($cmd) ..."
    Start-Process -FilePath "cmd.exe" -ArgumentList "/c", $cmd -WindowStyle Hidden | Out-Null
    for ($i = 0; $i -lt $waitSec; $i++) {
        if (Test-Port $port) { Write-Host "  $name is up on $port"; return $true }
        Start-Sleep 1
    }
    Write-Host "  $name did NOT come up on $port within ${waitSec}s"
    return $false
}

# Prefer the DEPLOYED launchers at C:\ogfn: they hardcode the real nvm node path and the
# real mitm venv, while ops/*.cmd in the repo assume the layout of a fresh checkout.
$backendCmd = "C:\ogfn\backend-start.cmd"
$proxyCmd   = "C:\ogfn\proxy-start.cmd"
if (-not (Test-Path $backendCmd)) { $backendCmd = "$PSScriptRoot\ops\backend-start.cmd" }
if (-not (Test-Path $proxyCmd))   { $proxyCmd   = "$PSScriptRoot\ops\proxy-start.cmd" }

if (-not (Test-Port 3551)) {
    if (-not (Start-OgfnService "backend" $backendCmd 3551 20)) {
        Write-Host "backend on 3551 is down. Start it with: C:\ogfn\backend (node index.js), or"
        Write-Host "run the OGFN-BACKEND task from an ELEVATED prompt: schtasks /run /tn OGFN-BACKEND"
        exit 1
    }
} else { Write-Host "backend already up on 3551" }

if (-not (Test-Port 8980)) {
    if (-not (Start-OgfnService "proxy" $proxyCmd 8980 25)) {
        Write-Host "mitmproxy on 8980 is down. Start it with: C:\ogfn\mitm\Scripts\mitmdump.exe -s C:\ogfn\ogfn_redirect.py --listen-port 8980"
        Write-Host "or run the OGFN-PROXY task from an ELEVATED prompt: schtasks /run /tn OGFN-PROXY"
        exit 1
    }
} else { Write-Host "proxy already up on 8980" }

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
    # reboot.log is appended across runs, so a stale "brains online" from the LAST match
    # would read as instant readiness. Remember where the file ends now and only consider
    # lines written after this point.
    $rebootLog = Join-Path (Split-Path $client) "reboot.log"
    $logFrom = 0
    if (Test-Path $rebootLog) { $logFrom = (Get-Item $rebootLog).Length }
    & $injector $dll $client @bootArgs

    # --- your client, the plain instance (no DLL) ---
    # This used to be left to a second scheduled task, so the shortcut only ever gave you
    # the DEAD server window and it looked like nothing had launched. The server is the
    # injected instance above; this is the window you actually play in, plain, same auth
    # argv, and it reaches the server through the backend's matchmaker (127.0.0.1:7777).
    # Wait for EVIDENCE that the server is listening, not a magic sleep. reboot.log sits
    # next to the exe and the fork logs the fleet coming online plus the arm line; either
    # is proof the UDP listener is up. Falls through after the budget so a changed log
    # format degrades to "start anyway" rather than hanging forever.
    $serverReady = $false
    for ($i = 0; $i -lt 60; $i++) {
        if (Test-Path $rebootLog) {
            $fresh = ""
            try {
                $fs = [IO.File]::Open($rebootLog, 'Open', 'Read', 'ReadWrite')
                if ($fs.Length -gt $logFrom) {
                    [void]$fs.Seek($logFrom, 'Begin')
                    $sr = New-Object IO.StreamReader($fs)
                    $fresh = $sr.ReadToEnd()
                }
                $fs.Close()
            } catch { }
            if ($fresh -match 'brains online|armed:|zone fields:') {
                $serverReady = $true
                Write-Host ("  server ready after {0}s" -f $i)
                break
            }
        }
        Start-Sleep 1
    }
    if (-not $serverReady) {
        Write-Host "  server readiness not confirmed in 60s; starting the client anyway."
        Write-Host "  check $rebootLog and C:\ogfn\src\out\ if the match does not appear."
    }
    Start-Sleep -Seconds 2
    Write-Host ""
    Write-Host "launching YOUR client (plain instance, no DLL) ..."
    $clientArgs = @($bootArgs | Where-Object { $_ -ne "-log" })
    try {
        Start-Process -FilePath $client -ArgumentList $clientArgs `
            -WorkingDirectory (Split-Path $client) | Out-Null
        Write-Host "client started: $client"
    } catch {
        Write-Host "could not start the client window: $($_.Exception.Message)"
        Write-Host "start it manually: $client"
    }

    Write-Host ""
    Write-Host "in-game: the console overlay toggles with your GUI key if configured;"
    Write-Host "match starts on its own (warmup 1 player, 20 bots). The 'OGFN Locker' tab has all"
    Write-Host "Season 3 skins + emotes -- pick one, Apply, live."
    Write-Host ""
    Write-Host "TWO windows is correct: the frozen one is the SERVER (minimise it),"
    Write-Host "the responsive one is YOUR game. Both close before the proxy is restored."
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
