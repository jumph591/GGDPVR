#pragma once
#include <dxgi.h>

// Hooks IDXGISwapChain::Present (vtable slot 8) by creating a throw-away device + swapchain on a
// hidden window to learn the Present implementation. Works whether the game's swapchain is the
// real DXGI object or a ReShade proxy (the logged module tells which one was hooked).
namespace ggdpvr::xr {
// Returns true to skip the real Present (frame consumed by VR only, e.g. the left eye).
using PresentCallback = bool (*)(IDXGISwapChain* swapchain);
bool InstallPresentHook(PresentCallback cb);
}  // namespace ggdpvr::xr
