#pragma once
#include <cstdint>

// Render resolution management.
//  - Lifts Windows' window-size clamp (WM_GETMINMAXINFO) on the game window so the UE3 viewport (and
//    its D3D backbuffer) can be larger than the desktop.
//  - Immersive scenes: per-eye-shaped resolution derived from the headset's recommended image size;
//    virtual-screen scenes: the normal desktop resolution. Applied with the engine's `setres` command.
namespace ggdpvr::vr {
bool InstallWindowHook();                 // call once the game window exists (from the render path)
void SetRenderMode(bool immersive);      // game thread; issues setres when the target changes
void SetVrRenderSize(uint32_t w, uint32_t h);  // computed from the headset (render thread)
void ForceResolutionTest(uint32_t w, uint32_t h);  // debug: apply a VR-style resolution now
// true once the presented backbuffer has the size the current mode wants (or the fallback is active).
// Stereo must not start before that: a resize issued while stereo frames are running never reaches the
// D3D backbuffer.
bool RenderSizeSettled();
void* GameWindow();                     // HWND of the game window once subclassed (else nullptr)
}  // namespace ggdpvr::vr
