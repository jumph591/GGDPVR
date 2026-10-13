#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// Devil-aura "green blob" fix (D3D11).
// The aura's blending can leave slightly negative values in the HDR scene colour. UE3's final
// colour-grading pass applies gamma as exp(log(x) * 0.4545) followed by min(x, 1): log of a negative is NaN and
// min(NaN, 1) returns 1, so the channel becomes fully lit (a solid green blob after the colour-grading LUT).
// With motion blur on (the flat game's default) an earlier pass hides this; the mod disables motion blur in VR.
// Fix: when the game creates a pixel shader containing that gamma sequence (log -> mul -> exp -> min 1.0),
// set _sat on the instruction producing the gamma input and recompute the DXBC checksum. For valid values the
// result is identical (values above 1 already end up as 1); negative/NaN input now gives 0 instead of 1.
namespace ggdpvr::fix {
// Patches `blob` in place; returns the number of instructions changed (0: shader left as is).
int PatchGammaSaturate(std::vector<uint8_t>& blob);
// Hooks ID3D11Device::CreatePixelShader given any device's vtable (same implementation as the game's device).
bool InstallShaderNanFix(void** deviceVtable);
}  // namespace ggdpvr::fix
