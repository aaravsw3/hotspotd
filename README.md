# hotspotd

execute hotspot functions while "asleep"
---

hotspotd keeps the windows hotspot client serving 24/7 while the machine behaves
like it is asleep

pitch black screen, monitor powered down, audio muted, cpu and disks
throttled. to come back, like typical windows sleep, press any key

## building

 - Visual Studio C++ workload
 - Windows 10/11 SDK

run:
```
build.cmd
```

receive:
`bin\hotspotd.exe` and `bin\blackout.exe`.

---

## install

run:
```powershell
.\install.ps1
```

---

## usage

**Ctrl + Alt + Shift + S** = black out

The tray icon also offers *Sleep now* / *Wake* / *Open log folder*.

Anything that turns the display off (the hotkey, the power button when remapped, the
idle display timeout) triggers the same path

### config
`C:\ProgramData\HotspotGuard\blackout.ini`:

```ini
[blackout]
mute=1                 ; mute the default audio endpoint while blacked out
overlay=1              ; draw a black window across all monitors
switch_plan=1          ; switch to the Hotspot Idle power scheme
hotkey=ctrl+alt+shift+s
idle_scheme={GUID}     ; written by install.ps1
```
---
## License

MIT
