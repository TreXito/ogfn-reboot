# OGFN client — downloads the Season 3 client 3.5.0 (CL-4008490, UE4.20) and extracts it.
# Usage:  powershell -ExecutionPolicy Bypass -File .\download-client.ps1 [-Dest "C:\ogfn\3.5"]
# ~6 GB; BITS survives interruption. 3.5.2 CL-4008490 ships as this 3.5 archive (S3-era files).

param(
    [Parameter()]
    [string]$Dest = "C:\ogfn\3.5",
    [Parameter()]
    [string]$Url = "https://galaxiafn.co.uk/3.5.zip"   # verified reachable 2026-10-17 (HTTP 200)
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $Dest)) { New-Item -ItemType Directory -Force -Path $Dest | Out-Null }

$zip = Join-Path $Dest "ogfn-3.5.zip"

if (Test-Path $zip) {
    if ((Get-Item $zip).Length -gt 100MB) {
        Write-Host "existing archive found, resuming: $zip"
        try { Start-BitsTransfer -Source $Url -Destination $zip -DisplayName "ogfn client 3.5" -Continue -ErrorAction Stop }
        catch { Write-Host "BITS resume failed: $($_.Exception.Message)"; Write-Host "delete $zip and retry for a fresh download."; exit 1 }
    } else {
        Remove-Item $zip -Force   # partial junk from a failed attempt
    }
}

if (-not (Test-Path $zip)) {
    Write-Host "downloading ≈6 GB from $Url ..."
    Start-BitsTransfer -Source $Url -Destination $zip -DisplayName "ogfn client 3.5"
}

Write-Host "extracting (7-Zip required) ..."

$7z = Get-Command 7z -ErrorAction SilentlyContinue
if (-not $7z) {
    $cand = "C:\Program Files\7-Zip\7z.exe"
    if (Test-Path $cand) { $7z = $cand } else {
        Write-Host "7-Zip not found — installing via winget ..."
        winget install --id 7zip.7zip -e --accept-source-agreements --accept-package-agreements
        if (Test-Path $cand) { $7z = $cand } else { $7z = Get-Command 7z -ErrorAction Stop }
    }
}

& $7z x $zip "-o$Dest" -y | Out-Null
Remove-Item $zip -Force

$exe = Get-ChildItem -Path $Dest -Recurse -Filter "FortniteClient-Win64-Shipping.exe" | Select-Object -First 1
if (-not $exe) {
    Write-Host "warning: client exe not found after extraction — check what the archive laid out under $Dest"
    exit 1
}

Write-Host "ready: $($exe.FullName)"
Write-Host "next: .\start.ps1 -GameDir `"$Dest`""
