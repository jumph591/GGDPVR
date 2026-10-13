#pragma once
#include <cstdint>
#include <string>

// Runs engine console commands on the game thread via Engine.PlayerController.ConsoleCommand.
// Sources: %LOCALAPPDATA%\GGDPVR\exec\live.txt (each line once, file removed after reading) and
// [hotkeys] F1..F12 entries in ggdpvr.ini (only while the game window is in the foreground).
namespace ggdpvr::ue3::console {
bool Init(uint32_t gameThreadId);        // resolves the UFunction/param layout; starts the watcher thread
void Enqueue(const std::string& cmd);
// Called from the ProcessEvent hook; executes queued commands when on the game thread.
void Pump();
}  // namespace ggdpvr::ue3::console
