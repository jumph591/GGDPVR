#pragma once
#include <cstdint>

// E1b instrumentation for the D3D11 level-change crash
// ("An FRenderResource was deleted without being released first!").
// Observes only; never suppresses the engine's assertion.
namespace ggdpvr::diag::rr {
bool Install();
void SetGameThread(uint32_t tid);
void DumpCounters(const char* why);
}  // namespace ggdpvr::diag::rr
