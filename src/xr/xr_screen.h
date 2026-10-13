#pragma once
#include <string>
#include <dxgi.h>

// Stage 2: OpenXR "virtual screen". Each game Present copies the backbuffer into an OpenXR
// swapchain and submits it as a world-locked quad layer (LOCAL space). Everything runs on the
// thread that calls Present (UE3 render thread). If no runtime/headset is available the game runs
// normally and OpenXR is retried periodically.
namespace ggdpvr::xr {
bool OnPresent(IDXGISwapChain* swapchain);  // true = skip the desktop Present
void RequestRecenter();
void ShutdownForExit();  // process exit: destroy the OpenXR session and instance (xr_clean_exit)
// Through a short-lived OpenXR instance (any thread, before the session): whether a headset is connected, and - on
// runtimes with XR_OCULUS_audio_device_guid (Meta's, some others) - its Windows audio output endpoint id (else empty).
bool ProbeHeadsetAudio(std::wstring& endpointId);
bool HasHpControllerExt();
bool HasFrameControllerExt();  // the instance enabled XR_VALVE_frame_controller_interaction (Steam Frame controllers)  // the instance enabled XR_EXT_hp_mixed_reality_controller (HP Reverb G2 controllers)
void RequestStereoDump();
void RequestShareTest();  // dev: can the game's device make OBS Mirror's shared copy texture? (log [sharetest])  // debug: write both eye images of the next stereo frame to BMP
}  // namespace ggdpvr::xr
