<#
    Hotspot Guard - installer

    Installs the LocalSystem watchdog service, wires the power configuration so the
    machine never drops to S3 while the hotspot is up, and registers the blackout
    ("fake sleep") helper in the interactive session.

    Run from an elevated PowerShell:
        .\install.ps1
        .\install.ps1 -MapPowerButton      # power button = screen off instead of sleep
#>
[CmdletBinding()]
param(
    [switch]$MapPowerButton,
    [string]$InstallDir = "$env:ProgramFiles\HotspotGuard"
)

$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------- elevation

$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "Elevation required - relaunching as administrator..." -ForegroundColor Yellow
    $argList = @('-NoProfile','-ExecutionPolicy','Bypass','-File',"`"$PSCommandPath`"")
    if ($MapPowerButton) { $argList += '-MapPowerButton' }
    Start-Process powershell.exe -Verb RunAs -ArgumentList $argList
    return
}

function Step($msg) { Write-Host "[*] $msg" -ForegroundColor Cyan }
function Ok  ($msg) { Write-Host "    $msg" -ForegroundColor Green }
function Warn($msg) { Write-Host "    $msg" -ForegroundColor Yellow }

$srcBin = Join-Path $PSScriptRoot 'bin'
if (-not (Test-Path (Join-Path $srcBin 'hotspotd.exe'))) {
    throw "bin\hotspotd.exe not found. Run build.cmd first."
}

# ---------------------------------------------------------------- 1. copy binaries

Step "Installing binaries to $InstallDir"
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null

# Stop an existing service so the exe is not locked.
$existing = Get-Service -Name hotspotd -ErrorAction SilentlyContinue
if ($existing) {
    if (Test-Path "$InstallDir\hotspotd.exe") {
        & "$InstallDir\hotspotd.exe" --uninstall 2>&1 | Out-Null
    } else {
        & sc.exe stop   hotspotd 2>&1 | Out-Null
        & sc.exe delete hotspotd 2>&1 | Out-Null
    }
}
Get-Process -Name blackout -ErrorAction SilentlyContinue | Stop-Process -Force

# DeleteService only deregisters. If a previous run left the watchdog running as an
# orphan it still holds the power request and locks the .exe, so clear it out.
$orphans = Get-Process -Name hotspotd -ErrorAction SilentlyContinue
if ($orphans) {
    $orphans | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 1
    Ok "stopped $($orphans.Count) orphaned hotspotd process(es)"
}

# A stopping service keeps its .exe locked for a moment after DeleteService returns,
# so copy on a retry loop rather than assuming the handle is already gone.
function Copy-Binary($src, $dest) {
    for ($i = 0; $i -lt 40; $i++) {
        try {
            Copy-Item $src $dest -Force -ErrorAction Stop
            return
        } catch {
            Start-Sleep -Milliseconds 500
        }
    }
    throw "Could not write $dest - still locked after 20s. Stop the hotspotd service and retry."
}

Copy-Binary "$srcBin\hotspotd.exe" "$InstallDir\hotspotd.exe"
if (Test-Path "$srcBin\blackout.exe") { Copy-Binary "$srcBin\blackout.exe" "$InstallDir\blackout.exe" }
Ok "binaries copied"

# ---------------------------------------------------------------- 2. hotspot fail-safes

Step "Applying hotspot fail-safes"

# Windows turns the Mobile Hotspot off after a few minutes when no client is
# connected. This disables that timeout so the AP stays broadcasting.
$icsSettings = 'HKLM:\SYSTEM\CurrentControlSet\Services\icssvc\Settings'
New-Item -Path $icsSettings -Force | Out-Null
Set-ItemProperty -Path $icsSettings -Name 'PeerlessTimeoutEnabled' -Value 0 -Type DWord
Ok "PeerlessTimeoutEnabled = 0 (hotspot no longer auto-stops with no clients)"

# The hotspot cannot come up if its backing services are not allowed to run.
foreach ($svc in 'icssvc','WlanSvc') {
    try {
        Set-Service -Name $svc -StartupType Automatic
        Ok "$svc start type = Automatic"
    } catch {
        Warn "could not set $svc start type: $($_.Exception.Message)"
    }
}

# Keep the Wi-Fi radio out of the OS power-saving path.
foreach ($a in (Get-NetAdapter | Where-Object { $_.MediaType -eq 'Native 802.11' })) {
    try {
        $pm = Get-NetAdapterPowerManagement -Name $a.Name -ErrorAction Stop
        if ($pm.AllowComputerToTurnOffDevice -ne 'Unsupported') {
            $pm.AllowComputerToTurnOffDevice = 'Disabled'
            Set-NetAdapterPowerManagement -InputObject $pm
            Ok "'$($a.Name)': computer may no longer power down this radio"
        }
    } catch {
        Warn "'$($a.Name)': power management unchanged ($($_.Exception.Message))"
    }
}

# ---------------------------------------------------------------- 3. power configuration

Step "Configuring power policy"

function Set-PowerValue($scheme, $sub, $setting, $value) {
    & powercfg /setacvalueindex $scheme $sub $setting $value 2>&1 | Out-Null
    & powercfg /setdcvalueindex $scheme $sub $setting $value 2>&1 | Out-Null
}

$SUB_SLEEP    = '238c9fa8-0aad-41ed-83f4-97be242c8f20'
$STANDBYIDLE  = '29f6c1db-86da-48c5-9fdb-f2b67b1f44da'
$HIBERNATEIDLE= '9d7815a6-7ee4-497e-8888-515a05f02364'
$ALLOWAWAY    = '25dfa149-5dd1-4736-b5ab-e8a37b5b8187'
$SUB_VIDEO    = '7516b95f-f776-4464-8c53-06167f40cc99'
$VIDEOIDLE    = '3c0bc021-c8a8-4e07-a973-6b14cbcb2b7e'
$SUB_DISK     = '0012ee47-9041-4b5d-9b77-535fba8b1442'
$DISKIDLE     = '6738e2c4-e8a5-4a42-b16a-e040e769756e'
$SUB_PROC     = '54533251-82be-4824-96c1-47b60b740d00'
$PROCMAX      = 'bc5038f7-23e0-4960-96da-33abaf5935ec'
$PROCMIN      = '893dee8e-2bef-41e0-89c6-b55d0929964c'
$SUB_PCI      = '501a4d13-42af-4429-9fd1-a8218c268e20'
$ASPM         = 'ee12f906-d277-404b-b6da-e5fa1a576df5'
$SUB_WIFI     = '19cbb8fa-5279-450e-9fac-8a3d5fedd0c1'
$WIFIPOWER    = '12bbebe6-58d6-4636-95bb-3217ef867c1a'
$SUB_BUTTONS  = '4f971e89-eebd-4455-a8de-9e59040e7347'
$PBUTTON      = '7648efa3-dd9c-4e3e-b566-50f929386280'
$SBUTTON      = '96996bc0-ad50-47ec-923b-6f41874dd9eb'
$LIDACTION    = '5ca83367-6e45-459f-a27b-476b1d01c936'

# "Allow Away Mode" is hidden by default; unhide so it can be enabled.
& powercfg -attributes $SUB_SLEEP $ALLOWAWAY -ATTRIB_HIDE 2>&1 | Out-Null

# Active scheme: never sleep, never hibernate, allow away mode, radio at full power.
$active = (& powercfg /getactivescheme) -join ' '
if ($active -match '([0-9a-fA-F-]{36})') { $activeGuid = $Matches[1] } else { throw "cannot read active power scheme" }

# Full backup of the scheme before we touch it, so uninstall restores exactly
# what was there rather than guessing at Windows defaults.
$backupDir = "$env:ProgramData\HotspotGuard"
New-Item -ItemType Directory -Force -Path $backupDir | Out-Null
$backupPow = "$backupDir\original-scheme.pow"
if (-not (Test-Path $backupPow)) {
    & powercfg /export $backupPow $activeGuid 2>&1 | Out-Null
    if (Test-Path $backupPow) {
        Set-Content -Path "$backupDir\original-scheme.guid" -Value $activeGuid -Encoding ascii
        Ok "original power scheme backed up"
    } else {
        Warn "could not back up the power scheme"
    }
}

Set-PowerValue $activeGuid $SUB_SLEEP $STANDBYIDLE   0
Set-PowerValue $activeGuid $SUB_SLEEP $HIBERNATEIDLE 0
Set-PowerValue $activeGuid $SUB_SLEEP $ALLOWAWAY     1
Set-PowerValue $activeGuid $SUB_WIFI  $WIFIPOWER     0
& powercfg /setactive $activeGuid | Out-Null
Ok "active scheme: sleep disabled, away mode allowed, Wi-Fi at maximum performance"

# Dedicated low-power scheme used only while the screen is blacked out.
Step "Creating the 'Hotspot Idle' power scheme"
$dup = (& powercfg /duplicatescheme $activeGuid) -join ' '
if ($dup -match '([0-9a-fA-F-]{36})') {
    $idleGuid = $Matches[1]
    & powercfg /changename $idleGuid "Hotspot Idle" "Low-power idle state that keeps the Mobile Hotspot online." | Out-Null

    Set-PowerValue $idleGuid $SUB_SLEEP $STANDBYIDLE   0     # never sleep
    Set-PowerValue $idleGuid $SUB_SLEEP $HIBERNATEIDLE 0     # never hibernate
    Set-PowerValue $idleGuid $SUB_SLEEP $ALLOWAWAY     1
    Set-PowerValue $idleGuid $SUB_VIDEO $VIDEOIDLE     60    # screen off after 1 min
    Set-PowerValue $idleGuid $SUB_DISK  $DISKIDLE      60    # spin disks down after 1 min
    Set-PowerValue $idleGuid $SUB_PROC  $PROCMAX       30    # cap CPU at 30%
    Set-PowerValue $idleGuid $SUB_PROC  $PROCMIN       5
    Set-PowerValue $idleGuid $SUB_PCI   $ASPM          2     # maximum PCIe power savings
    Set-PowerValue $idleGuid $SUB_WIFI  $WIFIPOWER     0     # but never throttle the radio

    Ok "scheme created: $idleGuid"
} else {
    $idleGuid = $null
    Warn "could not duplicate power scheme - blackout will skip plan switching"
}

if ($MapPowerButton) {
    # 4 = "Turn off the display"
    Set-PowerValue $activeGuid $SUB_BUTTONS $PBUTTON 4
    Set-PowerValue $activeGuid $SUB_BUTTONS $SBUTTON 4
    Set-PowerValue $activeGuid $SUB_BUTTONS $LIDACTION 4
    & powercfg /setactive $activeGuid | Out-Null
    Ok "power/sleep button and lid now turn the display off instead of sleeping"
} else {
    Ok "power button behaviour left unchanged (use -MapPowerButton to remap it)"
}

# ---------------------------------------------------------------- 4. blackout config

Step "Writing blackout configuration"
$dataDir = "$env:ProgramData\HotspotGuard"
New-Item -ItemType Directory -Force -Path $dataDir | Out-Null

$ini = @(
    '[blackout]',
    'mute=1',
    'overlay=1',
    'switch_plan=1',
    'hotkey=ctrl+alt+shift+s'
)
if ($idleGuid) { $ini += "idle_scheme={$idleGuid}" }
$ini | Set-Content -Path "$dataDir\blackout.ini" -Encoding utf8
Ok "$dataDir\blackout.ini"

# ---------------------------------------------------------------- 5. service

Step "Installing the hotspotd service"
$out = & "$InstallDir\hotspotd.exe" --install 2>&1
$out | ForEach-Object { Ok $_ }

# ---------------------------------------------------------------- 6. blackout task

if (Test-Path "$InstallDir\blackout.exe") {
    Step "Registering the blackout helper for logon"
    $taskName = 'HotspotGuard-Blackout'
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false -ErrorAction SilentlyContinue

    $targetUser = "$env:USERDOMAIN\$env:USERNAME"
    $action    = New-ScheduledTaskAction -Execute "$InstallDir\blackout.exe"
    $trigger   = New-ScheduledTaskTrigger -AtLogOn -User $targetUser
    $settings  = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries `
                    -DontStopIfGoingOnBatteries -ExecutionTimeLimit ([TimeSpan]::Zero) `
                    -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1)
    $principal = New-ScheduledTaskPrincipal -UserId $targetUser -LogonType Interactive -RunLevel Limited

    Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger `
        -Settings $settings -Principal $principal `
        -Description 'Hotspot Guard blackout helper (fake sleep / screen off).' | Out-Null
    Ok "task '$taskName' registered for $targetUser"

    Start-Process "$InstallDir\blackout.exe"
    Ok "blackout helper started"
}

# ---------------------------------------------------------------- 7. verify

Write-Host ""
Step "Verifying"
Start-Sleep -Seconds 4

$svc = Get-Service -Name hotspotd -ErrorAction SilentlyContinue
if ($svc -and $svc.Status -eq 'Running') { Ok "service: Running" } else { Warn "service is NOT running" }

$state = & "$InstallDir\hotspotd.exe" --selftest 2>&1 | Select-String 'state='
if ($state) { Ok "hotspot: $($state -replace '.*\[test\]\s*','')" }

$req = (& powercfg /requests) -join "`n"
if ($req -match 'HotspotGuard') { Ok "power request held - system sleep is blocked" }
else { Warn "no HotspotGuard power request visible yet" }

Write-Host ""
Write-Host "Done." -ForegroundColor Green
Write-Host "  Press Ctrl+Alt+Shift+S to black the screen out. Any key wakes it." -ForegroundColor Gray
Write-Host "  Logs: $dataDir" -ForegroundColor Gray
