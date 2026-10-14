# Changelog

## 1.0.1

- **Stages staying flat after a loading screen on some VR runtimes** (seen with WiVRn on Linux): the head pose was
  asked for the time of the last 3D frame, seconds old after a long load, and runtimes that give no pose that far
  back never let 3D start again - the stage stayed on the flat screen and shots missed. The pose is now asked for
  the current frame.
- **Your graphics settings could be replaced by the mod's VR values** (resolution, motion blur, anti-aliasing,
  shadows) when the game's settings file didn't exist yet as the mod started, e.g. the first launch after installing
  the game. The startup logos then showed as a squeezed square. The mod now guards the file as soon as the game
  creates it, and a resolution no display has (a VR render size) is put back to the game's default 1280x720.
  If your motion blur, anti-aliasing or shadows look different in the flat game, set them again in the game's
  options once.
- **Desktop mirror shows the HUD**: with `desktop_mirror=1` the desktop window (and recordings of it, e.g. Steam's)
  now has the HUD panel where you see it in the headset.
- **Logging is on by default**, so a problem's log already exists when you report it. It stays on your PC (the last
  8 sessions, at most 100 MB each). *Turn logging off* / *on* in `GGDPVR-Setup.exe`, or `log=` in `ggdpvr.ini`.
  Updating keeps your own setting.
- Better logs for bug reports: why 3D is off when it is (flat scene, no head pose, ...), the platform (Windows, or
  Wine / Proton on Linux), less noise.
- Controls in the README are listed for each part of the game.

## 1.0.0 - first public release

Everything below came together over the 0.x development builds; this is the first release for everyone.

- **Name tags and weak-spot pop-ups for girls outside the HUD panel**: when you turn your head (not the game camera)
  to a girl beside or behind you, her name tag is drawn above her head in 3D and her weak spots show their pop-ups.
- **Smoother frame pacing**: the game's own frame-rate smoothing (a 62 fps cap) is turned off while VR runs, so the
  headset's refresh rate paces the game.
- **Zoom is dropped on interruptions** (pause, continue prompt, Mom is coming!, game over), like the flat game.
- **Menus**: the pointer stays on a menu while it is up, wherever the controller points - the trigger clicks and the
  right stick scrolls lists even when the ray drifts off the menu.
- **Minigame icons** no longer fly out of place for a moment when they appear.
- **Kiss marks and shoe prints** from attacks are shown by default, like the flat game (`screen_marks=0` hides them).
- The sight cursor follows the controller at any angle.
- Settings: the pop-up size options are now `weak_spot_size`, `weak_spot_full_size_m` and `weak_spot_min_size`.
- Cleanup: dead experiments removed, binaries no longer carry build paths.

## Development history (0.x)

### 0.9.100 - 0.9.114: getting ready to share
- One plain-language settings file; logging off by default and free of user / device names; no network code.
- A native setup program (`GGDPVR-Setup.exe`): install, update, repair, switch off, uninstall, manual folder choice.
- DirectX 11 forced automatically; the game gets a DXGI 1.1 factory, so OpenXR capture layers (OBS Mirror) work.
- Remappable controller buttons; bindings for Meta Touch, Valve Index, HTC Vive, Windows Mixed Reality, HP Reverb G2,
  Steam Frame and simple controllers.
- Game sound follows the headset (Meta runtime) or the Windows default (SteamVR, Virtual Desktop); it keeps playing when
  another window has the focus.

### 0.9.50 - 0.9.99: polish and comfort
- Camera-shake and walking-bob removal (comfort, on by default); headset recenter followed.
- The game's own sight as the reticle; weak-spot pop-ups sized by distance and drawn with stereo depth.
- Name tags that stay on the girls; boss "!" markers that can be shot; HUD toggle button.
- Frame pacing work (early headset wait, held frames at transitions); dynamic shadows off in VR (big frame-rate gain).
- Final Doki Doki Mode, the Double Peace effect, Mom is coming! and loading screens adapted.
- Clean exit with Meta's runtime; "Quit app" from the headset closes the game.

### 0.9.20 - 0.9.49: minigames and Doki Doki Mode
- Minigames playable with the controllers: shooting hidden icons, touch / rub / poke requests on the grip, timed
  gesture phases with the hands or the thumbsticks, a panel that follows your gaze up and down.
- Mid-stage Doki Doki Mode (girl selection, the hand cursor), boss battles, immersive loading screens.
- Game sound through the headset.

### 0.8 - 0.9.19: menus and the world around them
- Point-and-click menus ("shoot the menus"), immersive title screen, hub, Score Attack menus, Academy Store,
  Collection and Dressing Room.
- Zoom scope at the reticle, aiming from the head along the controller ray.

### 0.4 - 0.7: stereo and controllers
- True stereo 3D with exact per-eye fields of view, head tracking, VR render resolution.
- The HUD on a floating panel with per-movie routing (panel, full-screen effects, per-eye pop-ups).
- VR controller aiming and shooting.

### 0.1 - 0.3: foundations
- DirectX 11 renderer fixes, the loader and in-game hooks, an OpenXR virtual screen.
