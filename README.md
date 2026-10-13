<p align="center">
  <img src="docs/images/logo.png" alt="GGDPVR - Gal*Gun: Double Peace VR" width="640">
</p>

<h1 align="center">GGDPVR</h1>
<p align="center"><b>Play Gal*Gun: Double Peace in VR</b> - full stereo 3D, head tracking and motion-controller aiming.</p>

---

GGDPVR is an unofficial fan mod for the PC version of **Gal\*Gun: Double Peace**. It shows the game in your VR headset
in true stereo 3D: you look around with your head, aim and shoot by pointing a VR controller, and the game's HUD and
menus float on a panel in front of you.

It works with any headset that supports OpenXR - Meta Quest (Link / Air Link), SteamVR headsets, Virtual Desktop,
Windows Mixed Reality, Pimax and others.

## Features

- **Stereo 3D with head tracking** in stages, boss battles, minigames, cutscenes, the title screen, the hub, the
  Collection and the Dressing Room.
- **Motion-controller aiming**: point to aim, pull the trigger to shoot. Zoom, Doki Doki Mode, "Mom is coming!" and
  every other action are on the controllers, and all buttons can be remapped.
- **Point-and-click menus**: aim at any menu and pull the trigger; the right stick scrolls long lists.
- **The game's HUD on a floating panel**, with weak-spot pop-ups, name tags and boss markers placed in 3D at the girls -
  also on girls you turn your head to look at, outside the panel.
- **Minigames adapted for VR**: touch and rub with the grip, motion gestures with your hands or the thumbsticks.
- **Comfort options**: the game's hit shake and walking bob removed (on by default), recenter on a button.
- **Game sound in the headset**, kept playing when another window has the focus.
- **A small setup program** to install, repair, switch off and uninstall - it never touches the game's own files or
  your save data.
- **No network code**: the mod never connects to anything. Logging is off unless you turn it on.

## Requirements

- **Gal\*Gun: Double Peace for Windows.** Made and tested with the Steam version; another PC copy of the same game
  version should work too.
- **A PC VR headset** and the software that connects it to your PC, set as the active OpenXR runtime: Meta Quest Link /
  Air Link, SteamVR, Virtual Desktop, Windows Mixed Reality, Pimax Play, ...
- Windows 10 or 11, 64-bit. Nothing else to install - the mod even starts the game in DirectX 11 mode by itself.

## Install

### The easy way

1. Download **`GGDPVR-<version>.zip`** from the [Releases](../../releases) page and unpack it anywhere.
2. Run **`GGDPVR-Setup.exe`**. It finds a Steam copy of the game by itself; for other copies click *Change...* and pick
   the game folder (the one with the `Binaries` folder in it).
   If Windows shows "Windows protected your PC": click *More info*, then *Run anyway*.
3. Click **Install**.
4. Start your VR software, then start the game (from Steam, or the setup's *Start game* button).

`GGDPVR-Setup.exe` is also how you repair, switch the mod off and on, open your settings and logs, and uninstall. It
always keeps your settings.

### Updating

GGDPVR never checks for updates by itself - it has no network code. To update, download the new zip from
[Releases](../../releases) and run its `GGDPVR-Setup.exe`: it updates the installed mod and keeps your settings.
Watch the repository (*Watch > Custom > Releases*) to be told about new versions.

### By hand

Copy the zip's `Binaries` folder into the game folder (merge the folders). That adds exactly:

```
Binaries\Win64\dinput8.dll          the loader that starts the mod with the game
Binaries\Win64\GGDPVR\GGDPVR.dll    the mod
Binaries\Win64\GGDPVR\ggdpvr.ini    your settings
```

No game file is replaced. When updating by hand, keep your own `ggdpvr.ini`.

## Controls

Right-handed player on Meta Quest controllers (left-handed: set `dominant_hand=left`).

| Action | Button |
|---|---|
| Aim | point your right controller |
| Pheromone shot (X) | right trigger (hold to charge) - in menus it clicks what you point at |
| Zoom (RB) | right grip |
| Sight speed (LB) | left grip |
| Special requests (RT) | left trigger |
| Doki-Doki Mode (Y) | Y |
| Mom is coming! (BACK) | X |
| Confirm (A) / Cancel (B) | A / B |
| Pause (START) | left menu button |
| Hide / show the HUD | B, during stages and boss battles |
| Menus and route choice | left thumbstick; right thumbstick scrolls lists |
| Re-center the view | press the left thumbstick (your headset's own re-center works too) |

Valve Index, HTC Vive, Windows Mixed Reality, HP Reverb G2 and Steam Frame controllers are supported too - the settings
file explains where their buttons are. To play with a gamepad or mouse and keyboard while still seeing the game in VR,
set `controllers=0`.

> **Quest / Rift controllers through SteamVR** (Steam Link, Virtual Desktop in SteamVR mode): SteamVR takes the left
> menu button for its own menu, so set another pause button, e.g. `pause=main.stick_click` in `[controls]`, or rebind
> it in SteamVR's controller settings.

## Settings

Everything is in one file, `Binaries\Win64\GGDPVR\ggdpvr.ini` in the game folder. Every option is explained there and
shows its default; changes apply the next time you start the game. The everyday options come first (HUD panel size
and distance, weak-spot pop-up size, comfort, audio), then the controller buttons in `[controls]`, then `[advanced]`.

To go back to the defaults, delete `ggdpvr.ini` and click *Repair* in the setup.

## Playing without VR / uninstalling

- *Turn mod off* in the setup: the game starts as if the mod wasn't installed. *Turn mod on* brings VR back.
  (By hand: create an empty file called `disabled` in the `GGDPVR` folder.)
- *Uninstall* in the setup removes everything the mod added. By hand: delete `dinput8.dll` and the `GGDPVR` folder
  from `Binaries\Win64`.

While VR runs, the mod temporarily changes a few of the game's graphics settings (for example motion blur,
anti-aliasing and the resolution) and puts your own values back when the game closes - after a crash, the next start with the mod
puts them back. Before uninstalling by hand after a crash, start the game once and quit normally.

## Recording and streaming

Recording tools that capture the headset view (for example the *OpenXR OBS Mirror* layer) work with the mod.
Capture **one eye** rather than both - a blend of both eyes looks doubled.

## Troubleshooting

- **Nothing in the headset**: start your VR software before the game, and make sure it is the active OpenXR runtime
  (SteamVR: *Settings > OpenXR*; Meta Quest Link: *Settings > General > OpenXR Runtime*).
- **The game stays on the monitor only**: check that `enabled=1` in `ggdpvr.ini` and that there is no `disabled` file
  in the `GGDPVR` folder, then click *Repair* in the setup.
- **The game crashes when choosing your character at New Game**: this can happen without the mod too, after other mods
  (for example costume packs) have changed the game's files. Restore the original game files (Steam: *Verify integrity
  of game files*), then reinstall GGDPVR with the setup's *Repair*.
- **Reporting a problem**: set `log=1` in `ggdpvr.ini`, play until it happens, quit, and attach
  `%LOCALAPPDATA%\GGDPVR\logs\ggdpvr.log` to an [issue](../../issues). The log never contains your user name or device
  names.

## Known limitations

- **Performance**: the game's rendering cost is per view, so busy stages can run at half your headset's refresh rate on
  mid-range GPUs. A lower refresh rate (72 / 80 Hz on Quest) gives the most headroom. Reports from other setups are
  welcome.
- Steam Frame controller bindings follow Valve's input documentation but are untested on real hardware.
- Reports from other headsets and setups are welcome.

## Building from source

Requirements:

- **Visual Studio 2022 Build Tools** with *Desktop development with C++* (MSVC v143, a Windows 10/11 SDK, and the
  CMake + Ninja components). The build script expects the default install location.
- **Python 3** (optional): checks that the UI shader compiles before building.

```bat
build.bat                                 :: -> out\RelWithDebInfo\  (dinput8.dll, GGDPVR.dll, GGDPVR-Setup.exe)
powershell -File package\make_package.ps1 :: -> dist\GGDPVR-<version>\ and dist\GGDPVR-<version>.zip
```

`install\install.ps1 -KeepIni` copies a fresh build into a Steam install of the game (developer use).

Layout:

| Folder | Contents |
|---|---|
| `loader/` | the `dinput8.dll` proxy that loads the mod (and forces DirectX 11 / a DXGI 1.1 factory) |
| `src/` | the mod: `xr/` OpenXR session, swapchains, input; `vr/` stereo, HUD panel, controllers, minigames; `ue3/` Unreal Engine 3 reflection and hooks; `core/` config, logging, hooks |
| `config/` | the settings file template |
| `package/` | the setup program and the player README |
| `third_party/` | OpenXR SDK (loader + headers), MinHook, RenderDoc app header |
| `docs/` | [technical notes](docs/TECHNICAL.md) |

## Privacy

GGDPVR has no network code: it never connects to the internet and never sends anything anywhere. It writes no log
unless you set `log=1`; the log stays on your PC and never contains your Windows user name or device names.

## License

GGDPVR is released under the [MIT License](LICENSE). Third-party components keep their own licenses - see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Disclaimer

GGDPVR is an unofficial fan project, not affiliated with or endorsed by INTI CREATES, PQube or any other rights holder.
Gal\*Gun: Double Peace and its logo are trademarks of their respective owners; the GGDPVR logo is fan art based on the
game's logo. The mod contains no game files - you need your own copy of the game.
