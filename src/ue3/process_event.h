#pragma once
#include <cstdint>

namespace ggdpvr::ue3::pe {
// Hooks every distinct implementation found at UObject vtable slot 0x220 (ProcessEvent)
// across live objects. Logs filtered events and periodic per-function call counts.
bool Install();
// Called after the original ProcessEvent for `fn` (a UFunction*) returns; may modify the params block.
using PostFn = void (*)(uintptr_t self, void* parms);
bool RegisterPost(uintptr_t fn, PostFn cb);
// Called before every ProcessEvent; returning true skips the call (the event does not run).
using SkipFn = bool (*)(uintptr_t self, uintptr_t fn);
void SetSkip(SkipFn cb);
}  // namespace ggdpvr::ue3::pe
