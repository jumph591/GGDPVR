#pragma once
#include <openxr/openxr.h>

// Stage 3: head-tracked (mono) view inside the game world.
//  - ULocalPlayer::CalcSceneView (rva 0x7F58F0) is hooked to know when the engine builds a view.
//  - While inside it, PlayerController.GetPlayerViewPoint's result (the game camera) gets the recentred
//    HMD pose composed on top (rotation + world_scale * position), and GetFOVAngle returns an FOV that
//    covers the headset's per-eye FOV.
//  - The pose used by the main viewport draw is handed to the OpenXR layer submission.
namespace ggdpvr::vr {
bool InstallHeadTracking();  // after the UE3 bootstrap + ProcessEvent hook
void SetFakeHead(int mode);     // debug: synthetic head pose (0 = off)
void SetStereoVariant(int v);   // debug: 0 normal, 1 swapped eyes, 2 mono
void ResetDeprojLog();          // debug: log the next cursor deprojections again
void ResetProjectLog();         // debug: log the next Canvas.Project calls again
float ZoomFactor();             // current right-click zoom (1 = none), for the UI panel and aim
int AeSkinShipType();                              // minigame gesture phase type (0 push up, 1 pull down, 2 alternate, 3/4 circle, 5 taps, 6 scratch; -1)
bool AeSkinShipParams(float& radius, float pos[2]);
bool AeCircleSpeed(float& border, float& best, float& worst);  // the circle phase's angular speed window (game units)  // the gesture's circle radius / tap target (stage units)
bool BossMarkerDiag();  // dev: !bossmarkers running
bool InFinalDoki();
bool AimPanelStage(float& x, float& y);  // stages: where the aim ray crosses the HUD panel (stage units)
void DevEngineFps(int smooth, float maxFps);  // dev: log (and with smooth >= 0 set) GEngine's frame-rate limiter  // the final Doki Doki Mode (GG2DokiSp* game) is up
int DrawingEye();
float FlatProportion();  // tan(game FOV / 2) / tan(eye FOV / 2): a flat-screen-sized thing at the same share of the view
uintptr_t CurrentPc();  // game thread: the local PlayerController last seen (0 = none)
void ShakeDiag(int seconds);  // dev: !shakediag
float StereoDistanceM(double xLeft, double xRight);  // game thread: distance (m) of a point the game placed per eye; <0 unknown                                  // game thread: 0 / 1 while that eye's view is drawn, else -1
float BandTopStage();                              // the HUD band's top offset in the eye image (stage units)
bool HudProjectionToPanel(float& sx, float& sy);  // stage coords of a 3D projection -> stage coords on the panel
bool PanelToHudProjection(float& sx, float& sy);  // the inverse: a panel point -> the 3D projection seen through it
unsigned long long DrawCount(); // main viewport draws so far (2 per frame in stereo)
// Dressing Room: the vanilla backdrop is hidden and a room is drawn around the player (UI layer). Recentered frame
// (the game camera's), XR axes, metres: up = world up, centre = the room's centre on the floor. Sizes in Unreal units.
struct DressingRoom {
    bool on = false;
    float up[3] = {0, 1, 0}, centre[3] = {0, 0, 0};
    float unitsPerMetre = 42.5f, radius = 0, wallH = 0;
    float halfWidth = 0;  // corridor: half-width of the room (uu)
    float base = 0;       // corridor: the booth's visible base above the floor (uu); below it the booth fades into the floor
    float fwd[3] = {0, 0, 0};  // corridor: horizontal direction into the booth (recentered frame); 0 = straight ahead
    int kind = 1;  // 1 = round room (Dressing Room), 2 = corridor (Academy Store: centre = the booth front at the floor,
                   // radius = the distance to the wall behind the player, wallH = ceiling height)
};
bool GetDressingRoom(DressingRoom& out);
void LogCamera();  // dev: !camera
const char* GameClassDiag();  // freeze report
bool PausedDiag();
bool ZoomInterrupted();  // something interrupts the zoom (pause, continue prompt / results, Mom is coming, game over)
void TraceTest();  // dev: !tracetest
void SetAimDiag(bool on);
void SetAePin(int mode, float amp, float hz);
void DevSetPcProperty(const char* name, int v);  // dev: !pcset
void DevSetHeartGauge(int v);                     // dev: !heart
void DevDkdkStart();                             // dev: !dkdkstart
void DevKeyDiag(int seconds);                    // dev: !keydiag
void KeyDiagTick();                              // dev: per frame, logs custom key bit changes  // dev: !aepin - the pinned minigame pointer still / moving
void LogBossMarkers(int seconds);  // dev: !bossmarkers <s>
void LogAeDiag(int seconds);       // dev: !aediag <s>  // dev: !aimdiag - the game's shot ray vs the controller ray, every second
// The shot's aim ray in raw LOCAL space for a raw controller aim pose (aim_origin: from the head, or the hand).
void AimRayRaw(const XrPosef& aimRaw, XrVector3f& org, XrVector3f& dir);
void SetCursorSpace(int s);     // dev: 0 panel, 1 eye-view cursor (default with controllers)
void SetFakeAimAtGirl(bool on, float yawOffset = 0);
void SetFakeAimAtClass(const char* cls);  // dev: !fakectrl at <class>  // dev: simulated controller aims at the nearest girl
void SetIpdScale(float k);      // debug: multiply the eye separation
}
