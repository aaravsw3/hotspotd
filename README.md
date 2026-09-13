# hotspot-on-sleep

Keep the Windows Mobile Hotspot serving clients 24/7 while the machine *looks and behaves*
like it is asleep: pitch black screen, monitor powered down, audio muted, CPU and disks
throttled. Press any key to come back.

Native Win32 / C++20. No runtime dependencies, no background interpreter, no scheduled
PowerShell polling.

---

## Why this exists

Windows cannot serve a Wi-Fi hotspot while genuinely asleep. On an S3 machine the sleep
transition cuts power to the Wi-Fi radio, so every connected client drops. There is no
software fix for that — it is a hardware power-state problem.

Check what your machine supports:

```
powercfg /a
```

If the only line under *available* is `Standby (S3)`, real sleep will always kill the
hotspot. Modern Standby (`S0 Low Power Idle`) machines can keep a radio alive in S0ix, but
most desktops cannot.

So this project does the only thing that actually works: it **prevents the sleep
transition** and replaces it with a visually identical low-power idle state.

| | Real S3 sleep | This project |
|---|---|---|
| Screen | Off | Off (DPMS) plus a black overlay |
| Audio | Silent | Muted |
| CPU / disks | Powered down | Capped at 30%, disks spun down, PCIe ASPM max |
| Wi-Fi radio | **Dead** | **Live, serving clients** |
| Wake | Key press | Key press |
| Idle draw | Lowest | Higher — see [Power cost](#power-cost) |

---

## Components

| Binary | Runs as | Job |
|---|---|---|
| `hotspotd.exe` | `LocalSystem` service, auto-start at boot | Holds the power request that blocks sleep; watchdog restarts the hotspot if it ever drops |
| `blackout.exe` | Interactive user, started at logon | The fake-sleep state: black screen, monitor off, mute, low-power plan, wake on input |

Two processes because a session-0 service has no desktop and cannot black out a screen,
while a user-session app cannot run before anyone logs in. The service half is what makes
the hotspot survive a reboot with nobody logged in.

---

## Fail-safes

The hotspot staying up is the whole point, so failure paths are covered at several levels:

- **`PeerlessTimeoutEnabled = 0`** — Windows stops the Mobile Hotspot after a few minutes
  when no client is connected. This registry value disables that timeout.
- **Watchdog loop** — polls tethering state every 5s and calls `StartTetheringAsync` on
  anything that is not `On`. Backs off to 15s / 30s / 60s after repeated failures so a
  broken uplink cannot hammer `icssvc`.
- **Service recovery** — the SCM restarts the process 5s after a crash, indefinitely,
  including on clean-but-unexpected exit (`SERVICE_FAILURE_ACTIONS_FLAG`).
- **Support services** — `icssvc` and `WlanSvc` are set to Automatic, and the watchdog
  restarts either one if it finds it stopped.
- **Power request** — `PowerCreateRequest` + `PowerRequestSystemRequired` blocks idle
  sleep. Visible in `powercfg /requests`, so it is diagnosable rather than magic.
- **Away mode** — `PowerRequestAwayModeRequired` converts a *forced* sleep (Start menu →
  Sleep) into "screen off, machine running" instead of a real S3 transition.
- **Radio power management** — the Wi-Fi adapter is excluded from "allow the computer to
  turn off this device", and Wi-Fi power-saving mode is pinned to Maximum Performance in
  both power schemes, including the low-power one.
- **Resume hook** — if the machine suspends anyway, the service catches
  `PBT_APMRESUMEAUTOMATIC` and re-checks the hotspot immediately on resume.

---

## Build

Requires Visual Studio with the C++ workload and a Windows 10/11 SDK.

```
build.cmd
```

Outputs `bin\hotspotd.exe` and `bin\blackout.exe`.

`build.cmd` pins the toolchain near the top — adjust if your install differs:

```bat
set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
set "WINSDK=10.0.26100.0"
```

C++/WinRT needs `/std:c++20`. Building at `c++17` fails with a deprecation error from
`<experimental/coroutine>`.

---

## Install

From an elevated PowerShell:

```powershell
.\install.ps1
```

The installer is self-elevating — it relaunches itself via UAC if needed.

```powershell
.\install.ps1 -MapPowerButton
```

Also remaps the power button, sleep button and lid to *Turn off the display* instead of
sleeping, so the physical button triggers the black screen. Off by default, because it
changes how the power button behaves everywhere.

What it changes:

- Copies binaries to `%ProgramFiles%\HotspotGuard`
- Registers and starts the `hotspotd` service
- Sets `PeerlessTimeoutEnabled = 0`, `icssvc` and `WlanSvc` to Automatic
- Disables sleep and hibernate timeouts on the active scheme, enables away mode
- Creates a **Hotspot Idle** power scheme (CPU 30%, disks 1 min, PCIe ASPM max, Wi-Fi at
  full power) used only while blacked out
- Registers `HotspotGuard-Blackout` to run at logon
- **Backs the original power scheme up to `original-scheme.pow` before touching anything**

---

## Usage

**Ctrl + Alt + Shift + S** — black out. Press any key or click to wake.

The tray icon also offers *Sleep now* / *Wake* / *Open log folder*.

Anything that turns the display off — the hotkey, the power button when remapped, the
idle display timeout — triggers the same path, because `blackout.exe` listens for
`GUID_CONSOLE_DISPLAY_STATE` rather than only its own hotkey.

### Configuration

`C:\ProgramData\HotspotGuard\blackout.ini`:

```ini
[blackout]
mute=1                 ; mute the default audio endpoint while blacked out
overlay=1              ; draw a black window across all monitors
switch_plan=1          ; switch to the Hotspot Idle power scheme
hotkey=ctrl+alt+shift+s
idle_scheme={GUID}     ; written by install.ps1
```

Restart `blackout.exe` after editing.

---

## Verify it is working

```powershell
# hotspot state, as the service sees it
& "$env:ProgramFiles\HotspotGuard\hotspotd.exe" --selftest

# the sleep block, in Windows' own words
powercfg /requests

# service health
Get-Service hotspotd
```

`powercfg /requests` should list the hotspotd reason string under `SYSTEM:`.

Logs are in `C:\ProgramData\HotspotGuard\` (`hotspotd.log`, `blackout.log`), rotated at 1 MB.

### Diagnostic modes

```
hotspotd.exe --selftest         # query state and exit
hotspotd.exe --selftest-start   # query, then start if it is off
hotspotd.exe --selftest-cycle   # stop then restart, proving the start path works
hotspotd.exe --console          # run the watchdog in the foreground
```

`--selftest-cycle` is the useful one when debugging context problems: run it as SYSTEM via
a scheduled task to confirm tethering works from session 0, which is where the service
lives.

---

## Power cost

This is the honest tradeoff. A desktop held awake with the screen off draws roughly
**30–60 W** depending on hardware, against **2–5 W** in real S3. The low-power scheme
reduces that but cannot close the gap — the machine is genuinely running.

You are trading idle watts for a hotspot that never drops. If nothing needs the hotspot
overnight, real sleep is cheaper.

---

## Uninstall

```powershell
.\uninstall.ps1
.\uninstall.ps1 -PurgeLogs    # also delete C:\ProgramData\HotspotGuard
```

Removes the service and the logon task, deletes the Hotspot Idle scheme, restores
`PeerlessTimeoutEnabled` and the Wi-Fi adapter's power management, and re-imports
`original-scheme.pow` so the power configuration is byte-for-byte what it was. If the
backup is missing it falls back to Windows defaults.

---

## Notes and gotchas

- `TetheringOperationalState` is `Unknown=0, On=1, Off=2, InTransition=3`. **`On` is 1 and
  `Off` is 2**, which is the opposite of the obvious guess and silently inverts every
  state check if you get it wrong.
- `StartTetheringAsync` / `StopTetheringAsync` report `Success` before the radio has
  finished changing state. Poll `TetheringOperationalState` until it settles rather than
  trusting the immediate return — `WaitForState` in `hotspot.cpp` does this.
- `SC_MONITORPOWER` is unreliable from a background window. `blackout.exe` shows its
  overlay and takes the foreground *before* asking for the monitor to power down.
- `NetworkOperatorTetheringManager` works fine from `LocalSystem` in session 0, despite
  being a WinRT API. Verified with `--selftest-cycle` under a SYSTEM scheduled task.
- Legacy `netsh wlan set hostednetwork` is unrelated to Mobile Hotspot. A driver reporting
  `Hosted network supported: No` still supports this API.

---

## License

MIT
