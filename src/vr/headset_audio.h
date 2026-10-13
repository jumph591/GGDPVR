#pragma once

// The game's sound (UE3 XAudio2 2.7, which always uses device 0 = the Windows default output) is sent to the VR
// headset's audio device instead, as reported by the OpenXR runtime (XR_OCULUS_audio_device_guid). Only when a
// headset is connected at the time the game starts its audio; otherwise the game keeps the default device.
namespace ggdpvr::vr::audio {
void Install();  // hooks CoCreateInstance before the engine creates XAudio2 (init thread, early)
// While VR is on, the game keeps its sound when its window loses the focus (game thread, every event; cheap).
void KeepPlayingInBackground();
}  // namespace ggdpvr::vr::audio
