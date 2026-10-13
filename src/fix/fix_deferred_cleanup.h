#pragma once

// E1b fix: D3D11 level-change crash ("An FRenderResource was deleted without being released first!").
//
// Root cause: FEngineLoop::Tick deletes the previous frame's deferred-cleanup
// list (FPendingCleanupObjects) every tick, but only performs FFrameEndSync::Sync when the flag at
// rva 0x30571C0 is set. LoadMap clears that flag while the loading movie plays ("LoadMapMovie"), so
// during a map transition objects whose BeginReleaseResource is still queued on the rendering thread
// get deleted. Under D3D11 the rendering thread is slow enough at that moment to lose the race
// (FSkeletalMeshObjectGPUSkin -> GPU skin vertex factories, vtable 0x2471B00).
//
// Fix: at that single call site, when the frame sync was skipped and the list is non-empty, call the
// engine's own FlushRenderingCommands() first (fence + wait), restoring the ordering guarantee.
// Toggle: [fix] deferred_cleanup_flush=1 in ggdpvr.ini. With the fix off, unsafe windows are only logged.
namespace ggdpvr::fix::cleanup {
bool Install();
}
