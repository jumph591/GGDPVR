#pragma once
#include <string>

// Triggers RenderDoc frame captures through RenderDoc's in-application API when the game was
// launched under RenderDoc (renderdoc.dll loaded). Captures go to %LOCALAPPDATA%\GGDPVR\captures\.
namespace ggdpvr::diag::renderdoc {
bool Available();
bool TriggerCapture(unsigned frames = 1);
std::string LastCapturePath();
}  // namespace ggdpvr::diag::renderdoc
