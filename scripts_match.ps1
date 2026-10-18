param([int]$TimeoutSec = 1080, [int]$BusAtSec = 55, [int]$SampleSec = 15)
$ErrorActionPreference = "SilentlyContinue"
$rb = "D:\ogfn\client\3.5-CL-4008490\Fortnite\FortniteGame\Binaries\Win64\reboot.log"
$ue = "$env:LOCALAPPDATA\FortniteGame\Saved\Logs\FortniteGame.log"
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$out = "C:\ogfn\matches\match-$stamp"
New-Item -ItemType Directory -Path $out -Force | Out-Null

Get-Process -Name FortniteClient-Win64-Shipping | Stop-Process -Force
Start-Sleep 2
if (Test-Path $rb) { Move-Item $rb "$rb.pre" -Force }
if (Test-Path $ue) { Move-Item $ue "$ue.pre" -Force }
Remove-Item C:\ogfn\FORCE_BUS.txt -Force

$samples = New-Object System.Collections.Generic.List[string]
$samples.Add("elapsed_s,cpu_s,ws_mb,phase,elims,fired,alive,deployed")
$sw = [Diagnostics.Stopwatch]::StartNew()
Start-ScheduledTask -TaskName OGFN-PLAY
Write-Output "t=0 boot started; archive=$out"

$busDropped = $false
$end = "timeout"
$lastPhase = "?"

# Boot wait: Start-ScheduledTask returns before the exe exists.
$bootTries = 0
while ($bootTries -lt 60) {
  $p = Get-Process -Name FortniteClient-Win64-Shipping | Select-Object -First 1
  if ($p) { break }
  $bootTries++
  Start-Sleep 1
}
if (-not (Get-Process -Name FortniteClient-Win64-Shipping | Select-Object -First 1)) {
  $end = "boot-failed"
}

while ($end -ne "boot-failed" -and $sw.Elapsed.TotalSeconds -lt $TimeoutSec) {
  $p = Get-Process -Name FortniteClient-Win64-Shipping | Select-Object -First 1
  if (-not $p) { $end = "process-died"; break }

  if (-not $busDropped -and $sw.Elapsed.TotalSeconds -ge $BusAtSec) {
    Set-Content C:\ogfn\FORCE_BUS.txt -Value go
    $busDropped = $true
    Write-Output ("t=" + [int]$sw.Elapsed.TotalSeconds + "s FORCE_BUS dropped")
  }

  $elims = 0; $fired = -1; $alive = -1; $deployed = -1
  if (Test-Path $ue) {
    $tail = Get-Content $ue -Tail 500
    $ph = $tail | Select-String "Set AthenaGamePhase to" | Select-Object -Last 1
    if ($ph) { $lastPhase = ("[" + ($ph -replace '.*to \[','') -replace '\].*',']') }
    if ($tail | Select-String "AthenaGamePhase to \[EndGame\]") { $end = "endgame" }
  }
  if (Test-Path $rb) {
    $rt = Get-Content $rb -Tail 400
    $elims = ($rt | Select-String "ELIMINATED bot#").Count
    $fl = $rt | Select-String "fleet t=" | Select-Object -Last 1
    if ($fl -and ($fl -match "fired=(\d+)")) { $fired = [int]$Matches[1] }
    if ($fl -and ($fl -match "alive=(\d+)")) { $alive = [int]$Matches[1] }
    if ($fl -and ($fl -match "deployed=(\d+)")) { $deployed = [int]$Matches[1] }
  }
  $samples.Add(("{0},{1},{2},{3},{4},{5},{6},{7}" -f [int]$sw.Elapsed.TotalSeconds, [math]::Round($p.CPU,1), [math]::Round($p.WorkingSet64/1MB), $lastPhase, $elims, $fired, $alive, $deployed))
  if ($end -eq "timeout" -and $alive -eq 0 -and $deployed -gt 0) { $end = "fleet-wiped" }
  if ($end -eq "timeout" -and $elims -ge 49) { $end = "last-stand" }
  if ($end -eq "endgame") { break }
  Start-Sleep $SampleSec
}

$dur = [int]$sw.Elapsed.TotalSeconds
Get-Process -Name FortniteClient-Win64-Shipping | Stop-Process -Force
Stop-ScheduledTask -TaskName OGFN-PLAY -EA SilentlyContinue
# start.ps1 is a full-stack launcher: it boots OGFN-BACKEND + OGFN-PROXY and sets the
# session system proxy. Automated matches end with NONE of that running.
Stop-ScheduledTask -TaskName OGFN-BACKEND -EA SilentlyContinue
Stop-ScheduledTask -TaskName OGFN-PROXY -EA SilentlyContinue
Start-Sleep 2
Get-Process -Name node, mitmdump -EA SilentlyContinue | Stop-Process -Force
Set-ItemProperty "HKCU:\Software\Microsoft\Windows\CurrentVersion\Internet Settings" -Name ProxyEnable -Value 0
Start-Sleep 1

$samples | Out-File "$out\samples.csv" -Encoding ascii
if (Test-Path $rb) { Copy-Item $rb "$out\reboot.log" -Force }
if (Test-Path $ue) { Copy-Item $ue "$out\FortniteGame.log" -Force }

if (Test-Path $rb) {
  Select-String -Path $rb -SimpleMatch "[ogfn]" | ForEach-Object { $_.Line } | Out-File "$out\ogfn-timeline.txt" -Encoding ascii
}
if (Test-Path $ue) {
  Select-String -Path $ue -Pattern "AthenaGamePhase to","PickSafeZoneLocation","[Ee]liminat","LogGameplayEvent.*Damage","Storm","EndGame","Access violation","fatal" |
    ForEach-Object { $_.Line } | Out-File "$out\ue-key.txt" -Encoding ascii
}
$lastFleet = ""
if (Test-Path $rb) { $lastFleet = (Select-String -Path $rb -SimpleMatch "fleet t=" | Select-Object -Last 1).Line }
$elimTotal = 0
if (Test-Path $rb) { $elimTotal = (Select-String -Path $rb -SimpleMatch "ELIMINATED bot#" | Measure-Object).Count }
@"
end=$end duration_s=$dur samples=$($samples.Count)
last_phase=$lastPhase elim_total=$elimTotal
last_fleet=$lastFleet
"@ | Out-File "$out\summary.txt" -Encoding ascii
Write-Output "MATCH $end dur=${dur}s elims=$elimTotal"
Write-Output "archive=$out"
