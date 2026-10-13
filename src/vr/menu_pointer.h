#pragma once
#include <windows.h>
#include <cstdint>

// Stage 7: point at menus with the controller and pull the trigger to select ("shoot the menu").
// In menu contexts (virtual-screen scenes, and the in-stage pause on the UI panel) the dominant-hand ray is
// intersected with the screen / panel; the hit becomes the game's mouse position (GetCursorPos hook) and the
// trigger a left click, both delivered as records of the engine's DirectInput mouse. Nothing leaves the process.
namespace ggdpvr::vr::pointer {
void Install();                                  // GetCursorPos hook (idempotent)
void SetContext(bool menus, bool onPanel, uintptr_t pc, bool stickMenus, bool sticky);  // game thread, every frame
bool SticksAsDpad();                             // pure menu scenes (pause, virtual screen): both sticks -> D-pad
bool Active();                                   // the controller drives the mouse right now
bool MenuScene();  // a menu scene (the controller is the pointer there whether or not its ray is on the panel)
void Update(bool trigger, bool touch = false);   // game thread, every frame after SetContext (touch: minigames)
void DevClick();                                 // dev: one click at the current pointer position
void DevPin(float x, float y);                   // dev: pointer at this panel point (stage coords) for 300 ms
void DevRightClick(int frames);                  // dev: right button held for that many frames
void Scroll(float stickY, bool dpadMode);        // menus: the aim-hand stick's tilt as mouse-wheel notches (repeating)
void DevWheel(int notches);                      // dev: wheel notches at the pointer (+ up)
void SetMapping(int m);                          // dev: 0 client-linear, 1 HUD-band normalized
float HitDistance();                             // metres to the menu surface while active (else 0)
void SetClickPath(int p);                        // dev: 0 DirectInput records, 1 window messages
ULONGLONG LastButtonTick();                      // GetTickCount64 of the last pointer press / release sent
bool PanelStage(float& x, float& y);             // pointer on the panel, in HUD stage coords (1920x1080); false if inactive
bool TouchModeActive();                          // minigames: the pointer is the touch (grip) on the panel
void SetTouchInEyeView(bool on);                 // minigames: the virtual mouse = the 3D projection under the pointer
bool TouchStage(float& x, float& y);             // minigames: that position (stage units), false when not in use
void TouchCounts(uint32_t& presses, uint32_t& releases, bool& down);  // minigames: trigger presses / releases so far
}  // namespace ggdpvr::vr::pointer
