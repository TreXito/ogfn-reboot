# ogfn-diag.ps1 - crash forensics for the first-boot failure
$ErrorActionPreference = "Continue"

Write-Output "=== PROCESSES ==="
Get-Process | Where-Object { $_.Name -match "Fortnite|Ogfn|WerFault" } | Select-Object Name, Id | Format-Table -AutoSize | Out-String

Write-Output "=== APPLICATION ERROR EVENTS (last 3h) ==="
try {
  Get-WinEvent -FilterHashtable @{ LogName = "Application"; StartTime = (Get-Date).AddHours(-3) } -ErrorAction Stop |
    Where-Object { $_.Message -match "Fortnite|Ogfn" } |
    Select-Object -First 3 | ForEach-Object {
      Write-Output ("---- " + $_.TimeCreated + " id=" + $_.Id + " " + $_.ProviderName)
      Write-Output ($_.Message)
    }
} catch { Write-Output ("no events: " + $_.Exception.Message) }

Write-Output "=== WER REPORTS (last 3h) ==="
Get-WinEvent -FilterHashtable @{ LogName = "Application"; ProviderName = "Windows Error Reporting"; StartTime = (Get-Date).AddHours(-3) } -ErrorAction SilentlyContinue |
  Select-Object -First 2 | ForEach-Object { Write-Output ("---- " + $_.TimeCreated); Write-Output ($_.Message) }

Write-Output "=== UE LOGS on disk ==="
$roots = @(
  "D:\ogfn\client\3.5-CL-4008490\Fortnite\FortniteGame\Saved\Logs",
  "D:\ogfn\client\3.5-CL-4008490\Fortnite\Engine\Programs",
  "$env:LOCALAPPDATA\FortniteGame\Saved\Logs",
  "$env:USERPROFILE\Documents\Fortnite"
)
foreach ($r in $roots) {
  if (Test-Path $r) {
    Get-ChildItem $r -Recurse -Filter *.log -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending |
      Select-Object -First 4 | ForEach-Object { Write-Output ($_.FullName + "  " + $_.Length + "  " + $_.LastWriteTime) }
  } else { Write-Output ("missing: " + $r) }
}

Write-Output "=== Reboot log files anywhere in ogfn trees ==="
Get-ChildItem "C:\ogfn", "D:\ogfn" -Recurse -Include *.log, *.txt -ErrorAction SilentlyContinue |
  Where-Object { $_.Name -match "reboot|log" } | Sort-Object LastWriteTime -Descending |
  Select-Object -First 10 | ForEach-Object { Write-Output ($_.FullName + "  " + $_.Length + "  " + $_.LastWriteTime) }

Write-Output "=== crash dumps already collected? ==="
"$env:LOCALAPPDATA\CrashDumps", "C:\Windows\Minidump" | ForEach-Object {
  if (Test-Path $_) { Get-ChildItem $_ -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 5 | ForEach-Object { Write-Output ($_.FullName + "  " + $_.Length) } }
  else { Write-Output ("missing: " + $_) }
}
