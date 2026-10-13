#pragma once
#include <cstdint>
#include <string>

namespace ggdpvr::diag {
// Captures another thread's call stack (suspend -> GetThreadContext -> RtlVirtualUnwind -> resume).
// Diagnostics only: frames inside GG2Game.exe are printed as RVAs.
std::string RemoteStack(uint32_t tid, int maxFrames = 32);
}
