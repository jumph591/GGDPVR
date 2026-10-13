# How GGDPVR works

A tour of the mod for contributors. Gal\*Gun: Double Peace is an Unreal Engine 3 game (64-bit, build 10897, Steam).
GGDPVR runs inside the game process, renders every frame twice (once per eye) through the engine's own renderer, and
hands the results to the headset with OpenXR.

> Many hooks use fixed addresses (RVAs) in `GG2Game.exe` build 10897. Each hook checks the target's first bytes before
> patching, so on a different build it is skipped (and logged) instead of corrupting anything. Supporting another build
> means re-finding those addresses.

## Loading

- `loader/dinput8_proxy.cpp` is a `dinput8.dll` proxy placed next to the game: it forwards every DirectInput export to the
  system DLL and, unless the mod is switched off (`GGDPVR\disabled` file, `GGDPVR_DISABLE=1`, or `enabled=0`), loads
  `GGDPVR\GGDPVR.dll`.
- Before the game starts its renderer the loader patches two imports of `GG2Game.exe`:
  - `GetCommandLineW` - adds `-d3d11` unless a renderer was chosen (the mod needs the D3D11 renderer);
  - the delay-loaded `CreateDXGIFactory` - returns a DXGI 1.1 factory, so the game's device can create keyed-mutex shared
    textures (OpenXR capture layers such as OBS Mirror need them).
- `src/dllmain.cpp` starts an init thread: settings (`core/config`), logging (`core/log`, opt-in), hooks (MinHook,
  `core/hooks`), then the Unreal bootstrap.

## Unreal Engine 3 access (`src/ue3`)

- `objects.cpp` finds `GNames` / `GObjects` and works out the `UObject` header layout at runtime, then provides
  reflection: find objects, classes, properties (offsets, bool bit masks), function parameters.
- A `ProcessEvent` hook (`ue3::pe`) lets the mod run code before / after script events and functions (e.g.
  `GetPlayerViewPoint`, `GetFOVAngle`).
- Native `exec` functions are hooked where script logic needs a VR-aware answer; the executing script function is read
  from the `FFrame` (`Node` at +0x14, `Object` +0x1C, `Code` +0x24) so a fix applies only to one caller. Examples:
  `UCanvas::execProject` (shout attacks' on-screen test), `UObject::execDot_VectorVector` (the HUD's "in front of the
  camera" tests for weak-spot pop-ups and Doki Doki name tags use the eye's direction).
- `console.cpp` runs engine console commands from the game thread. Engine `set` commands are refused: UE3 saves them
  into the player's config files.

## Stereo rendering (`src/vr/head_tracking.cpp`, `src/xr/xr_screen.cpp`)

- `FViewport::Draw` is hooked. In VR the main game viewport is drawn (and presented) **twice** per game frame, once per
  eye. Script that runs during the draw (the HUD) therefore runs twice too; input polled from the HUD is filtered so
  menus step once per press.
- During each draw, `ULocalPlayer::CalcSceneView` / `GetPlayerViewPoint` / `GetFOVAngle` are adjusted: the game camera
  gets the eye's pose composed on top (`world_scale` Unreal units per metre) and a field of view covering that eye's
  OpenXR FOV. The engine's render resolution is switched to the headset's (the player's own settings are restored on
  exit by `core/settings_guard`).
- Each draw ends in the game's `Present`, which is hooked: instead of going to the monitor, the backbuffer is copied into
  that eye's OpenXR swapchain image (the exact per-eye sub-rectangle) and submitted as a projection layer. The OpenXR
  session runs on the game's own D3D11 device (`XR_KHR_D3D11_enable`).
- Frame pacing: `xrWaitFrame` runs on the render thread just before eye 0 renders; the pose used for a frame is the one
  submitted with it. The engine's own frame-rate smoothing is turned off while VR runs.

## The HUD and menus (`src/vr/ui_layer.cpp`)

The game's HUD is Scaleform GFx (Flash) movies drawn after the 3D scene.

- The GFx render command is intercepted and every movie is classified (`Classify`): the **panel** (HUD, menus), the
  **full-screen** layer (fades, damage flashes - shown across the whole view), the **per-eye** layer (weak-spot pop-ups, boss
  markers - drawn into each eye at the game's own per-eye positions), tint movies, the zoom scope, and the game's sight.
- Each group is rendered twice, over black and over white; combining the two recovers colour and alpha exactly
  (`Combine`), since the movies are drawn with blending onto the backbuffer.
- The panel image goes to an OpenXR **quad layer** in front of the player (`ui_distance`, `ui_width`). The rest is
  composited into each eye image by one pixel shader (`Composite`): effects, the per-eye layer (shifted for stereo depth),
  the reticle, the zoom scope, backdrops for menus, and name tags drawn in 3D.
- ActionScript calls into the movies are hooked (`hk_Invoke`): positions the game computed for a flat screen (name tags,
  minigame icons, Doki Doki markers, boss markers) are re-projected onto the panel or into the eye view, and sizes are
  adjusted for the headset's wider field of view.

## Controllers and aiming (`src/vr/controllers.cpp`, `src/vr/menu_pointer.cpp`, `src/xr/xr_input.cpp`)

- OpenXR actions with suggested bindings for several controller profiles; the `[controls]` section maps game buttons to
  inputs.
- Buttons and sticks reach the game through a **virtual XInput pad** (`src/vr/vpad.cpp`).
- Aiming: the shot ray is the controller ray (from the head through the point `reticle_distance` along it), applied in
  the engine's screen-to-world deprojection. The game's 2D sight cursor is set to the aim point's position in the eye
  view, so the game's screen-space tests (weak spots, reveals) agree with what the player sees.
- Menus: the controller ray is intersected with the panel / virtual screen; the hit becomes the game's mouse position
  (`GetCursorPos` hook and the game's own virtual mouse) and the trigger a click (DirectInput mouse records); the right
  stick scrolls (mouse wheel records).

## Other pieces

- `src/vr/headset_audio.cpp` - routes the game's XAudio2 output to the headset (Meta runtime) and keeps the sound playing
  when the window loses focus.
- `src/core/settings_guard.cpp` - the graphics settings the mod changes in VR are saved first and restored on exit (and
  after a crash, on the next start).
- `package/setup_app` - the Win32 setup program; the mod files are embedded as resources.

## Developer mode

`[debug] developer=1` in `ggdpvr.ini` (undocumented for players) enables a live console: lines appended to
`%LOCALAPPDATA%\GGDPVR\exec\live.txt` run as engine console commands, and `!` commands run mod diagnostics (eye /
panel image dumps, a simulated head and controller for testing without a headset, ActionScript call traces, ...).
See `src/ue3/console.cpp`. With `log=1` the log at `%LOCALAPPDATA%\GGDPVR\logs\ggdpvr.log` includes performance lines
every 5 s.
