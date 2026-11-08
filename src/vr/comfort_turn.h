#pragma once
#include <cstdint>
#include <string>

#include "vrmath.h"

// Comfort turning in stages ([vr] turning=snap): the view no longer takes the rail camera's rotation. It faces a
// comfort yaw (level horizon) that only changes by blink-snaps - black fade out, the turn on a black frame, fade in:
//  - when the route starts a new pose and the camera's next rest direction is snap_threshold or more away (look-ahead:
//    turns that follow each other without a pause count as one sweep; a glance - shorter than snap_glance, then back -
//    gets no snap);
//  - when a Zako Rush changes direction;
//  - when the game camera turned on its own and settled far away from it (anything else that turned the camera);
//  - the off-hand stick (the one that turns in Zako Rush): left / right turn by snap_angle, pulled back = face where
//    the game camera looks; not during Zako Rush, holds, menus or route choices (the stick is the game's there);
//  - a menu or a route choice opening: face the game camera once.
// auto_snap=travel: of the automatic snaps only a walk's (>= travel_distance m, >= 0.4 s per m, view travel_threshold off
// it); off: none.
// Smaller offsets close while the player turns their head (recenter_gain of the head's turn): toward the game camera
// (route), toward the walking direction while the rail moves (travel / off).
// A girl's hold (grab / shove) turns the view to her - by a blink (grab_snap=1) or the game's own blend - and back;
// during it the sticks go to the game (escaping needs both) and stick snaps resume 0.5 s after it.
namespace ggdpvr::vr::comfort {
// Game thread, once per frame before the eye draws. gameRot / gameLoc: the game camera of the last view (shake removed).
// headYaw: the head's own yaw in the tracking space (degrees; only its changes count), headOk: it is valid this frame.
// bossPlay: a boss battle (GG2Bs*) is up; aePlay: a minigame (GG2ActionEvent*) - their own comfort (OtherTick).
void Tick(uint64_t frameId, uintptr_t pc, bool stagePlay, bool bossPlay, bool aePlay, bool paused, const Rot& gameRot,
          const V3& gameLoc, bool headOk, float headYaw);
// PostGetPlayerViewPoint: the rotation the head pose is composed onto (the game's own while comfort turning is off).
Rot ViewFrame(const Rot& gameRot);
// Render thread: how black the eye images of that frame are (0..1).
float BlinkForFrame(uint64_t frameId);
float VignetteForFrame(uint64_t frameId);  // render thread: the motion vignette's strength for that frame (0..1)
bool BlinkRecent();    // a blink or the vignette is (or was just) on: the composite pass must run
bool ConsumesStick();  // the off-hand stick turns the view (not sent to the game as the D-pad)
bool Held();           // a girl holds the player (the sticks go to the game for the escape)
bool InStageDoki();
// PostGetPlayerViewPoint: the game camera's place (shake removed) -> the view's (boss battles: held between teleports)
void ViewLocation(V3& loc);
// Game thread: a plain blink (nothing changes behind it), and whether the view is fully dark now (boss victory height)
bool BossEnding(uintptr_t pc);  // a boss battle's ending is on (its freed heroine, the result): game thread
bool HopViewZ(float& z);  // game thread: flatten_jumps - a small jump step's straight-line view height (false: none now)
bool MarksActive();    // the off-screen indicators may show (comfort turning in charge of the view, no menu up)
std::string StateOf(uintptr_t obj);  // an object's script state name ("" if none / unknown yet)
void DevSetFadeMs(float ms);  // dev: !snapfade <ms> (-1 = snap_fade_ms again)
}  // namespace ggdpvr::vr::comfort
