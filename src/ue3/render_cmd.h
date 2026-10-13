#pragma once
#include <cstddef>

// Enqueues a mod callback on UE3's rendering thread, in order with the engine's own render
// commands (same mechanism as ENQUEUE_UNIQUE_RENDER_COMMAND: GRenderCommandBuffer ring buffer).
// If threaded rendering is off, the callback runs immediately.
namespace ggdpvr::ue3::render {
using Fn = void (*)(const void* payload);
constexpr size_t kMaxPayload = 192;
bool Enqueue(Fn fn, const void* payload, size_t size);
}  // namespace ggdpvr::ue3::render
