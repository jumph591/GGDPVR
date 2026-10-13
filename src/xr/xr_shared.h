#pragma once
#include <cstdint>
#include <openxr/openxr.h>

struct IDXGISwapChain;
struct ID3D11Texture2D;

// Bridge between the OpenXR session (render thread, inside Present) and the game thread.
namespace ggdpvr::xr {
struct HeadSample {
    XrPosef raw{};       // head (VIEW space) in LOCAL space at the predicted display time
    XrPosef rel{};       // raw relative to the recenter reference (yaw + position)
    XrPosef eyeRaw[2]{}; // per-eye poses in LOCAL
    XrPosef eyeRel[2]{}; // per-eye poses relative to the recenter reference
    XrFovf fov[2]{};     // per-eye FOVs
};
bool Running();                    // session is running (frames are being submitted)
// Freeze watchdog (render thread): called once when the submitted view looks frozen, with a short reason; the
// callback logs the mod's state (registered by the UI layer)
void SetFreezeReporter(void (*fn)(const char* why));
bool SampleHead(HeadSample& out);  // game thread; predicted for the next display time
float BackbufferAspect();          // width / height of the last presented backbuffer
uint64_t BackbufferSize();         // last presented backbuffer: (width << 32) | height, 0 if unknown
bool RecommendedEyeSize(uint32_t& w, uint32_t& h);  // runtime-recommended per-eye image size

// Describes the next Present: queued from the game thread as a UE3 render command right before the
// frame's draw, so it reaches the render thread in order with that frame's Present.
struct FrameRecord {
    int32_t eye = -1;     // -1 mono, 0 left, 1 right, 2 zoom scope render (kept for the UI layer, not submitted)
    int32_t valid = 0;    // pose/fov valid (else the frame is shown on the virtual screen)
    XrPosef pose{};       // pose the frame was rendered with (LOCAL space)
    float halfH = 0, halfV = 0;  // symmetric half-angles (radians) of the rendered frustum
    uint64_t frameId = 0;
    XrFovf fov[2]{};      // the headset's per-eye FOVs: the image is cropped to exactly these when submitted
};
void QueueFrameRecord(const FrameRecord& rec);

// render thread helpers (UI layer)
int CurrentEyeRT();                 // eye of the draw being executed (-1: mono / virtual screen)
IDXGISwapChain* SwapChainRT();      // the game's swapchain (last presented)
void DumpTexture(ID3D11Texture2D* tex, const wchar_t* stem);  // debug BMP (+ _alpha) in the log directory
bool DumpPending();
uint64_t CurrentFrameIdRT();        // frameId of the draw being executed
bool CurrentRecordRT(FrameRecord& out);  // record of the draw being executed (false: none / mono)
XrPosef RecenterRefRT();            // the recenter reference (LOCAL space, yaw-only orientation)
XrPosef UiPanelPoseRT();            // UI panel centre pose (LOCAL space)
// UI layer image for frame `frameId` (unpremultiplied RGBA 8-bit, 16:9); submitted as a quad layer with
// that frame's stereo projection. The texture must stay valid until the frame's Present.
void SetUiImageRT(ID3D11Texture2D* tex, uint64_t frameId);
void SetUiPanelWidth(float metres);
void PerfGame(double frameMs, double tickMs, double eye0Ms, double eye1Ms);
void PerfUiMark(int eye, bool start);  // render thread: the mod UI pass of an eye begins / ends  // game thread, per stereo frame
void SetImmersiveScene(bool on);    // game thread: a VR scene (its loading screens: a surround + the panel)
void SetUiPanelDrop(float metres);  // the panel quad this far below its recentered place (minigames)  // the panel quad's width (minigames: larger)
}  // namespace ggdpvr::xr
