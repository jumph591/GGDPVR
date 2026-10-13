#pragma once

namespace ggdpvr::xr {
// Diagnostic hooks on the game's device (given any device's vtable): logs the depth buffers it creates.
bool InstallDepthProbe(void** deviceVtable);
}  // namespace ggdpvr::xr
