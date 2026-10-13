#pragma once
#include <cstddef>
#include <cstdint>

namespace ggdpvr::hooks {
bool Init();
// Hooks the function at game RVA `rva` after verifying its first bytes. Returns false (and logs)
// on any mismatch/failure. `original` receives the trampoline.
bool Create(const char* name, uint32_t rva, const uint8_t* expect, size_t expectLen, void* detour, void** original);
// Hooks an absolute address (e.g. a vtable target) without byte verification.
bool CreateAt(const char* name, void* target, void* detour, void** original);
bool EnableAll();
void DisableAll();
}  // namespace ggdpvr::hooks
