#pragma once
#include <cstdint>
#include <openxr/openxr.h>

// VR controller input (OpenXR actions). Bindings are suggested for Meta Touch, Valve Index, Windows Mixed Reality,
// HP Reverb G2, HTC Vive wands and the generic "simple controller" (0.9.106); each hand reports the same set of
// inputs whatever the controller. Created with the session, synced at the start of every XR frame (render thread);
// the latest snapshot is readable from any thread.
namespace ggdpvr::xr::input {
struct Hand {
    bool active = false;   // pose tracked
    XrPosef aimRaw{};      // aim pose in LOCAL space
    XrPosef aimRel{};      // aim pose relative to the recenter reference (same frame as HeadSample::rel)
    float trigger = 0, grip = 0;
    XrVector2f stick{};    // thumbstick (Vive wands: the trackpad)
    bool stickClick = false;
    bool lower = false;    // lower face button: A (right) / X (left) on Touch, A on Index; WMR / Vive: trackpad, bottom
    bool upper = false;    // upper face button: B / Y on Touch, B on Index; WMR / Vive: trackpad, top
    bool menu = false;     // menu button: left on Touch, both on WMR / Vive / G2; Index: press the trackpad firmly
};
struct Snapshot {
    bool valid = false;    // actions synced this frame (session focused)
    Hand hand[2];          // 0 left, 1 right
    uint64_t seq = 0;
};
bool Create(XrInstance instance, XrSession session);
void Destroy();
void Sync(XrSession session, XrSpace local, XrTime time, const XrPosef& recenterRef);
bool Get(Snapshot& out);
// Controller vibration, any thread (applied at the next Sync): hand 0 left, 1 right, 2 both; amplitude 0..1
// (<= 0 stops), duration in seconds.
void Rumble(int hand, float amplitude, float seconds);
// The runtime picked a (new) controller type for a hand (XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED): logs it,
// and sets how a trackpad press is read.
void ProfilesChanged(XrSession session);
}  // namespace ggdpvr::xr::input
