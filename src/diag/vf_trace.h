#pragma once
#include <cstdint>

// E1b: per-object lifecycle timeline for the GPU-skin vertex factory class that leaks under
// D3D11 (vtable rva 0x2471B00, ctor 0x61C1A0, 0x340 bytes) and for the owning skeletal mesh
// objects (vtable 0x251A0C0). Observation only.
namespace ggdpvr::diag::vf {
constexpr uint32_t kVFVtable = 0x2471B00;
bool Install();
void OnInit(uintptr_t res, uint32_t vtblRva);
void OnRelease(uintptr_t res, uint32_t vtblRva);
void OnDtor(uintptr_t res, uint32_t vtblRva);
void PrintHistory(uintptr_t res);
}  // namespace ggdpvr::diag::vf
