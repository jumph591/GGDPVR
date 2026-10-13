#pragma once
#include <openxr/openxr.h>

// Stage 7: VR controllers -> game.
//  - Buttons/sticks -> virtual XInput gamepad (vpad) with GG:DP's own gamepad meanings (see MapToPad).
//  - Dominant hand aim pose -> the shot ray (replaces the 2D sight) and a world-space reticle.
// Default mapping (right-handed; [vr] dominant_hand=left mirrors trigger/grip/stick roles):
//   dominant trigger -> X (shoot)          dominant grip -> RB (zoom / camera right; minigames: touch)
//   other grip       -> LB (slow / camera left; both grips = LB+RB camera reset)
//   other trigger    -> RT (touch)         A / B -> A / B        Y -> Y (Doki-Doki)    X -> Back (panic)
//   left menu        -> Start (pause)      other stick -> left stick + D-pad (route choice, menus)
//   dominant stick   -> right stick, click -> R3 (glasses)        other stick click -> recenter view
namespace ggdpvr::vr::ctrl {
struct Aim {
    XrPosef raw{};  // LOCAL space
    XrPosef rel{};  // recentered frame (same as HeadSample::rel)
};
void UpdateFromXr();      // render thread, after xr::input::Sync each XR frame
bool GetAim(Aim& out);    // dominant-hand aim (or the simulated controller - dev)
bool AimActive();         // controller aim is driving the game this frame
void FakeTrigger(unsigned ms);  // dev: a simulated trigger pull (menu pointer click)
void SetFake(bool on, float yawDeg, float pitchDeg);  // dev: simulated dominant-hand controller
void SetHitDistance(float metres);   // game thread: distance to what the shot ray hits (0 = nothing)
float HitDistance();                 // for the reticle
bool TriggerHeld();
bool GripHeld();
void Sticks(float dom[2], float off[2]);  // thumbstick deflections (x right, y up), dominant / other hand
bool OffTriggerHeld();                     // dominant grip (latched): the minigames' touch
void FakeGrip(unsigned ms);          // dev: a simulated grip squeeze
void HitPulse();                     // a shot hit something (stronger vibration on the aiming hand)                  // dominant trigger (latched), for the menu pointer
void SetFakeHand(float x, float y, float z);              // dev: simulated hand position (recentered frame, m)
void FakeHand(float out[3]);
void SetFakeQuiet(float yawDeg, float pitchDeg);       // dev: update the simulated aim (no log)
}  // namespace ggdpvr::vr::ctrl
