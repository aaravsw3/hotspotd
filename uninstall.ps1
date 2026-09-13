<#
    Hotspot Guard - uninstaller

    Removes the service and the blackout helper, and puts the power configuration
    back the way it was. Logs are kept unless -PurgeLogs is passed.
#>
[CmdletBinding()]
param(
    [switch]$PurgeLogs,
    [string]$InstallDir = "$env:ProgramFiles\HotspotGuard"
)

$ErrorActionPreference = 'Continue'

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "Elevation required - relaunching as administrator..." -ForegroundColor Yellow
    $argList = @('-NoProfile','-ExecutionPolicy','Bypass','-File',"`"$PSCommandPath`"")
    if ($PurgeLogs) { $argList += '-PurgeLogs' }
    Start-Process powershell.exe -Verb RunAs -ArgumentList $argList
    return
}

function Step($msg) { Write-Host "[*] $msg" -ForegroundColor Cyan }
function Ok  ($msg) { Write-Host "    $msg" -ForegroundColor Green }
function Warn($msg) { Write-Host "    $msg" -ForegroundColor Yellow }

$dataDir = "$env:ProgramData\HotspotGuard"

# ---------------------------------------------------------------- service + helper

Step "Removing the service"
if (Test-Path "$InstallDir\hotspotd.exe") {
    & "$InstallDir\hotspotd.exe" --uninstall 2>&1 | ForEach-Object { Ok $_ }
} else {
    & sc.exe stop   hotspotd 2>&1 | Out-Null
    & sc.exe delete hotspotd 2>&1 | Out-Null
    Ok "service removed via sc.exe"
}

Step "Removing the blackout helper"
Unregister-ScheduledTask -TaskName 'HotspotGuard-Blackout' -Confirm:$false -ErrorAction SilentlyContinue
Get-Process -Name blackout -ErrorAction SilentlyContinue | Stop-Process -Force
Ok "scheduled task removed and process stopped"

# ---------------------------------------------------------------- power config

Step "Restoring power configuration"

$backupPow  = "$dataDir\original-scheme.pow"
$backupGuid = "$dataDir\original-scheme.guid"

if ((Test-Path $backupPow) -and (Test-Path $backupGuid)) {
    $guid = (Get-Content $backupGuid -Raw).Trim()
    & powercfg /import $backupPow $guid 2>&1 | Out-Null
    & powercfg /setactive $guid 2>&1 | Out-Null
    Ok "original power scheme restored from backup"
} else {
    Warn "no backup found - restoring Windows defaults instead"
    $SUB_SLEEP   = '238c9fa8-0aad-41ed-83f4-97be242c8f20'
    $STANDBYIDLE = '29f6c1db-86da-48c5-9fdb-f2b67b1f44da'
    $SUB_BUTTONS = '4f971e89-eebd-4455-a8de-9e59040e7347'
    $PBUTTON     = '7648efa3-dd9c-4e3e-b566-50f929386280'
    $SBUTTON     = '96996bc0-ad50-47ec-923b-6f41874dd9eb'
    $LIDACTION   = '5ca83367-6e45-459f-a27b-476b1d01c936'

    & powercfg /setacvalueindex SCHEME_CURRENT $SUB_SLEEP   $STANDBYIDLE 1800 2>&1 | Out-Null
    & powercfg /setdcvalueindex SCHEME_CURRENT $SUB_SLEEP   $STANDBYIDLE 900  2>&1 | Out-Null
    & powercfg /setacvalueindex SCHEME_CURRENT $SUB_BUTTONS $PBUTTON     3    2>&1 | Out-Null
    & powercfg /setacvalueindex SCHEME_CURRENT $SUB_BUTTONS $SBUTTON     1    2>&1 | Out-Null
    & powercfg /setacvalueindex SCHEME_CURRENT $SUB_BUTTONS $LIDACTION   1    2>&1 | Out-Null
    & powercfg /setactive SCHEME_CURRENT 2>&1 | Out-Null
    Ok "sleep timeouts and button actions reset"
}

# Delete the dedicated idle scheme.
$ini = "$dataDir\blackout.ini"
if (Test-Path $ini) {
    $m = Select-String -Path $ini -Pattern 'idle_scheme=\{([0-9a-fA-F-]{36})\}'
    if ($m) {
        $idleGuid = $m.Matches[0].Groups[1].Value
        & powercfg /delete $idleGuid 2>&1 | Out-Null
        Ok "'Hotspot Idle' power scheme deleted"
    }
}

# ---------------------------------------------------------------- hotspot settings

Step "Reverting hotspot settings"

# Restore the stock "stop the hotspot when nobody is connected" behaviour.
$icsSettings = 'HKLM:\SYSTEM\CurrentControlSet\Services\icssvc\Settings'
if (Test-Path $icsSettings) {
    Remove-ItemProperty -Path $icsSettings -Name 'PeerlessTimeoutEnabled' -ErrorAction SilentlyContinue
    Ok "PeerlessTimeoutEnabled removed (Windows default restored)"
}

try {
    Set-Service -Name icssvc -StartupType Manual
    Ok "icssvc start type back to Manual"
} catch {
    Warn "could not reset icssvc start type"
}

foreach ($a in (Get-NetAdapter | Where-Object { $_.MediaType -eq 'Native 802.11' })) {
    try {
        $pm = Get-NetAdapterPowerManagement -Name $a.Name -ErrorAction Stop
        if ($pm.AllowComputerToTurnOffDevice -ne 'Unsupported') {
            $pm.AllowComputerToTurnOffDevice = 'Enabled'
            Set-NetAdapterPowerManagement -InputObject $pm
            Ok "'$($a.Name)': radio power management restored"
        }
    } catch { }
}

# ---------------------------------------------------------------- files

Step "Removing files"
if (Test-Path $InstallDir) {
    Remove-Item $InstallDir -Recurse -Force -ErrorAction SilentlyContinue
    Ok "$InstallDir removed"
}
if ($PurgeLogs -and (Test-Path $dataDir)) {
    Remove-Item $dataDir -Recurse -Force -ErrorAction SilentlyContinue
    Ok "$dataDir removed"
} else {
    Ok "logs kept in $dataDir"
}

Write-Host ""
Write-Host "Uninstalled. Sleep behaviour is back to normal." -ForegroundColor Green
