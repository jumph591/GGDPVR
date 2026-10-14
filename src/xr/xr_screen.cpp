#include "xr_screen.h"
#include "xr_shared.h"
#include "xr_input.h"
#include "../vr/controllers.h"
#include "../vr/ui_layer.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_PLATFORM_WIN32  // XR_OCULUS_audio_device_guid
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include "../core/config.h"
#include "../core/log.h"
#include "../ue3/render_cmd.h"
#include "../ue3/console.h"

namespace ggdpvr::xr {
namespace {
struct State {
    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace local = XR_NULL_HANDLE, view = XR_NULL_HANDLE;
    struct Swap {
        XrSwapchain handle = XR_NULL_HANDLE;
        std::vector<XrSwapchainImageD3D11KHR> images;
        uint32_t width = 0, height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        ULONGLONG lastUse = 0;  // GetTickCount64 of the last image acquired (idle ones are released: video memory)
    } sc[4];  // [0] single width (screen / mono), [1] double width (stereo side by side), [2] UI panel,
              // [3] a small solid colour (the surround of loading screens in VR scenes)
    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
    bool running = false;
    ID3D11Device* device = nullptr;  // weak: the game's device (or the ReShade proxy of it)
    ULONGLONG nextRetry = 0;
    bool needRecenter = true;
    XrTime recenterAt = 0;  // follow_system_recenter: recentre on the first frame at / after this (LOCAL space change)
    XrPosef quadPose{{0, 0, 0, 1}, {0, 0, -2}};
    XrPosef uiPose{{0, 0, 0, 1}, {0, 0, -1.6f}};
    uint64_t frames = 0;
} s;
std::atomic<bool> g_recenter{false};
std::atomic<int> g_dumpPending{0};  // debug: eye images still to dump (2 = whole stereo pair)
bool g_disabled = false;

// --- shared with the game thread ---
std::shared_mutex g_sessionMu;  // unique: create/destroy (render thread); shared: SampleHead (game thread)
std::atomic<int64_t> g_lastDisplayTime{0}, g_displayPeriod{0};
std::atomic<bool> g_running{false};
std::atomic<float> g_aspect{16.f / 9.f};
std::atomic<uint32_t> g_recW{0}, g_recH{0};
std::mutex g_refMu;
XrPosef g_ref{{0, 0, 0, 1}, {0, 0, 0}};  // recenter reference (yaw-only orientation)

XrQuaternionf QMul(XrQuaternionf a, XrQuaternionf b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
XrVector3f QRot(XrQuaternionf q, XrVector3f v) {
    XrQuaternionf p{v.x, v.y, v.z, 0}, qc{-q.x, -q.y, -q.z, q.w};
    XrQuaternionf r = QMul(QMul(q, p), qc);
    return {r.x, r.y, r.z};
}

#define XR_CHECK(call)                                                          \
    do {                                                                        \
        XrResult r_ = (call);                                                   \
        if (XR_FAILED(r_)) {                                                    \
            LOG("[xr] %s failed: %d", #call, int(r_));                          \
            return false;                                                       \
        }                                                                       \
    } while (0)

// Per-process runtime selection via XR_RUNTIME_JSON (honoured by the OpenXR loader); the system default is untouched.
void SelectRuntime() {
    const std::wstring& r = config::Get().vrRuntime;
    std::wstring json;
    if (r == L"oculus") json = LR"(C:\Program Files\Oculus\Support\oculus-runtime\oculus_openxr_64.json)";
    else if (r == L"steamvr") json = LR"(C:\Program Files (x86)\Steam\steamapps\common\SteamVR\steamxr_win64.json)";
    else if (r == L"virtualdesktop") json = LR"(C:\Program Files\Virtual Desktop Streamer\OpenXR\virtualdesktop-openxr.json)";
    else if (r != L"system" && !r.empty()) json = r;
    static bool s_logged = false;  // (called by the audio probe and the XR start: logged once)
    const bool log = !s_logged;
    s_logged = true;
    if (json.empty()) {
        if (log) LOGP("[xr] runtime: system default (active OpenXR runtime)");
        return;
    }
    SetEnvironmentVariableW(L"XR_RUNTIME_JSON", json.c_str());
    if (log)
        LOGP("[xr] runtime override: %ls%s", json.c_str(),
             GetFileAttributesW(json.c_str()) == INVALID_FILE_ATTRIBUTES ? " (FILE NOT FOUND)" : "");
}

bool g_hpControllerExt = false, g_frameControllerExt = false;
bool CreateInstance() {
    static bool selected = false;
    if (!selected) SelectRuntime(), selected = true;
    uint32_t n = 0;
    if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr))) {
        LOG("[xr] no OpenXR runtime available (loader could not enumerate extensions)");
        return false;
    }
    std::vector<XrExtensionProperties> exts(n, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, n, &n, exts.data());
    bool hasD3D11 = false, hasHp = false, hasFrame = false;
    for (auto& e : exts) {
        hasD3D11 |= !strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
        hasHp |= !strcmp(e.extensionName, "XR_EXT_hp_mixed_reality_controller");  // HP Reverb G2 controller bindings
        hasFrame |= !strcmp(e.extensionName, "XR_VALVE_frame_controller_interaction");  // Steam Frame controllers
    }
    if (!hasD3D11) {
        LOG("[xr] runtime lacks %s", XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
        return false;
    }
    std::vector<const char*> enabled{XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    if (hasHp) enabled.push_back("XR_EXT_hp_mixed_reality_controller");
    if (hasFrame) enabled.push_back("XR_VALVE_frame_controller_interaction");
    g_hpControllerExt = hasHp;
    g_frameControllerExt = hasFrame;
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ci.applicationInfo.applicationName, "GGDPVR (Gal*Gun: Double Peace)");
    ci.applicationInfo.applicationVersion = 1;
    strcpy_s(ci.applicationInfo.engineName, "Unreal Engine 3");
    ci.applicationInfo.engineVersion = 10897;
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ci.enabledExtensionCount = uint32_t(enabled.size());
    ci.enabledExtensionNames = enabled.data();
    XR_CHECK(xrCreateInstance(&ci, &s.instance));
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(s.instance, &ip);
    LOGP("[xr] instance created: runtime '%s' %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
        XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));
    return true;
}

bool GetSystem() {
    XrSystemGetInfo gi{XR_TYPE_SYSTEM_GET_INFO};
    gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    static uint32_t failures = 0;
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    XrResult r = xrGetSystem(s.instance, &gi, &s.system);
    QueryPerformanceCounter(&t1);
    if (XR_FAILED(r)) {
        if (failures++ % 12 == 0)
            LOG("[xr] xrGetSystem failed (%d) - headset not connected / streaming not active? retrying every 5 s "
                "(call took %.2f ms, %u failures)",
                int(r), double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart), failures);
        return false;
    }
    failures = 0;
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(s.instance, s.system, &sp);
    LOGP("[xr] system '%s' vendor 0x%X maxLayers %u", sp.systemName, sp.vendorId, sp.graphicsProperties.maxLayerCount);
    return true;
}

bool CreateSession(ID3D11Device* dev) {
    PFN_xrGetD3D11GraphicsRequirementsKHR getReq = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(s.instance, "xrGetD3D11GraphicsRequirementsKHR",
                                   reinterpret_cast<PFN_xrVoidFunction*>(&getReq)));
    XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    XR_CHECK(getReq(s.instance, s.system, &req));
    // Compare adapters
    IDXGIDevice* dxgiDev = nullptr;
    if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDev)))) {
        IDXGIAdapter* ad = nullptr;
        if (SUCCEEDED(dxgiDev->GetAdapter(&ad))) {
            DXGI_ADAPTER_DESC d{};
            ad->GetDesc(&d);
            const bool same = !memcmp(&d.AdapterLuid, &req.adapterLuid, sizeof(LUID));
            LOG("[xr] game adapter '%ls' %s the runtime's required adapter (min FL 0x%X, device FL 0x%X)",
                d.Description, same ? "MATCHES" : "DOES NOT MATCH", req.minFeatureLevel, dev->GetFeatureLevel());
            ad->Release();
        }
        dxgiDev->Release();
    }
    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = dev;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = s.system;
    std::unique_lock lock(g_sessionMu);
    XR_CHECK(xrCreateSession(s.instance, &sci, &s.session));
    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.poseInReferenceSpace.orientation.w = 1;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    XR_CHECK(xrCreateReferenceSpace(s.session, &rs, &s.local));
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XR_CHECK(xrCreateReferenceSpace(s.session, &rs, &s.view));
    s.device = dev;
    if (config::Get().vrControllers) input::Create(s.instance, s.session);
    {
        uint32_t n = 0;
        XrViewConfigurationView vcv[2]{{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
        if (XR_SUCCEEDED(xrEnumerateViewConfigurationViews(s.instance, s.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                           2, &n, vcv)) && n == 2) {
            g_recW = vcv[0].recommendedImageRectWidth;
            g_recH = vcv[0].recommendedImageRectHeight;
            LOG("[xr] recommended eye image %ux%u (max %ux%u)", vcv[0].recommendedImageRectWidth,
                vcv[0].recommendedImageRectHeight, vcv[0].maxImageRectWidth, vcv[0].maxImageRectHeight);
        }
    }
    LOG("[xr] session created");
    return true;
}

void DestroySwapchain() {
    for (auto& w : s.sc) {
        if (w.handle) xrDestroySwapchain(w.handle);
        w = {};
    }
}

void DestroySession() {
    std::unique_lock lock(g_sessionMu);
    g_running = false;
    DestroySwapchain();
    input::Destroy();
    if (s.local) xrDestroySpace(s.local);
    if (s.view) xrDestroySpace(s.view);
    if (s.session) xrDestroySession(s.session);
    s.local = s.view = XR_NULL_HANDLE;
    s.session = XR_NULL_HANDLE;
    s.running = false;
    s.sessionState = XR_SESSION_STATE_UNKNOWN;
    s.device = nullptr;
}

void DestroyAll() {
    DestroySession();
    if (s.instance) xrDestroyInstance(s.instance);
    s.instance = XR_NULL_HANDLE;
    s.system = XR_NULL_SYSTEM_ID;
}

// Same-typeless-group sRGB variant: the game's backbuffer holds gamma-encoded values, so it must be
// presented to the runtime as an sRGB format (raw copy, no conversion).
DXGI_FORMAT SrgbOf(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case DXGI_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        default: return f;
    }
}

void InvalidateHeld();  // below
bool EnsureSwapchain(const D3D11_TEXTURE2D_DESC& bb, int which) {
    auto& w = s.sc[which];
    const uint32_t width = which >= 2 ? bb.Width : bb.Width * (which ? 2 : 1);
    const DXGI_FORMAT want = SrgbOf(bb.Format);
    if (w.handle && w.width == width && w.height == bb.Height && w.format == want) return true;
    if (w.handle) xrDestroySwapchain(w.handle), InvalidateHeld();  // the held layers may name it
    w = {};
    uint32_t n = 0;
    xrEnumerateSwapchainFormats(s.session, 0, &n, nullptr);
    std::vector<int64_t> fmts(n);
    xrEnumerateSwapchainFormats(s.session, n, &n, fmts.data());
    DXGI_FORMAT use = DXGI_FORMAT_UNKNOWN;
    for (auto f : fmts)
        if (f == want) use = want;
    if (use == DXGI_FORMAT_UNKNOWN)
        for (auto f : fmts)
            if (f == bb.Format) use = bb.Format;
    if (use == DXGI_FORMAT_UNKNOWN) {
        LOG("[xr] runtime supports neither format %d nor %d for the backbuffer copy", want, bb.Format);
        return false;
    }
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                    XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = use;
    ci.sampleCount = 1;
    ci.width = width;
    ci.height = bb.Height;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    XR_CHECK(xrCreateSwapchain(s.session, &ci, &w.handle));
    xrEnumerateSwapchainImages(w.handle, 0, &n, nullptr);
    w.images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    xrEnumerateSwapchainImages(w.handle, n, &n, reinterpret_cast<XrSwapchainImageBaseHeader*>(w.images.data()));
    w.width = width, w.height = bb.Height, w.format = use, w.lastUse = GetTickCount64();
    LOG("[xr] swapchain[%d] %ux%u format %d (backbuffer format %d), %u images", which, width, bb.Height, use, bb.Format, n);
    return true;
}

void PollEvents() {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (s.instance && xrPollEvent(s.instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto& e = *reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
            s.sessionState = e.state;
            LOGP("[xr] session state -> %d", int(e.state));
            if (e.state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                XrResult r = xrBeginSession(s.session, &bi);
                s.running = XR_SUCCEEDED(r);
                g_running = s.running;
                s.needRecenter = true;
                LOGP("[xr] xrBeginSession -> %d", int(r));
            } else if (e.state == XR_SESSION_STATE_STOPPING) {
                g_running = false;
                xrEndSession(s.session);
                s.running = false;
            } else if (e.state == XR_SESSION_STATE_EXITING && config::Get().vrQuitWithRuntime) {
                // the runtime asks the app to end ("Quit app" in Link; 0.9.76: the game kept running). Only
                // EXITING - headset off / dashboard / passthrough are STOPPING / IDLE / VISIBLE, Link lost is
                // LOSS_PENDING. The game's own exit command: its normal shutdown (config saved, settings restored).
                LOGP("[xr] session EXITING (the runtime's Quit app): closing the game");
                DestroySession();
                static bool s_sent = false;
                if (!s_sent) s_sent = true, ue3::console::Enqueue("exit");
                s.nextRetry = GetTickCount64() + 600000;  // no new session while the game shuts down
                return;
            } else if (e.state == XR_SESSION_STATE_EXITING || e.state == XR_SESSION_STATE_LOSS_PENDING) {
                LOG("[xr] session ending (%d); will retry", int(e.state));
                DestroySession();
                if (e.state == XR_SESSION_STATE_LOSS_PENDING) DestroyAll();
                s.nextRetry = GetTickCount64() + 5000;
                return;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
            input::ProfilesChanged(s.session);  // which controller type the runtime picked (for bug reports)
        } else if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            // the headset's own recenter (0.9.78: "doesn't re-center the game to the default position"): the
            // runtime moves LOCAL to the head; the mod's reference, taken in the old LOCAL, no longer matches
            const auto& e = *reinterpret_cast<XrEventDataReferenceSpaceChangePending*>(&ev);
            LOG("[xr] reference space %d changes at %lld (pose valid %d)%s", int(e.referenceSpaceType), (long long)e.changeTime,
                int(e.poseValid), config::Get().vrFollowSystemRecenter ? ": the game re-centres with it" : "");
            if (e.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL && config::Get().vrFollowSystemRecenter)
                s.recenterAt = e.changeTime > 0 ? e.changeTime : 1;
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            LOGP("[xr] instance loss pending");
            DestroyAll();
            s.nextRetry = GetTickCount64() + 5000;
            return;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// xr_clean_exit (0.9.77): every exit crash dump was a fail-fast in the Meta runtime's client DLL inside ExitProcess,
// with the session still alive. The engine has shut down (render thread stopped) by the time ExitProcess runs.
void ShutdownForExitImpl() {
    __try {
        DestroyAll();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void Recenter(XrTime t) {
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(s.view, s.local, t, &loc)) ||
        !(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ||
        !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
        return;
    const auto& q = loc.pose.orientation;
    // yaw of the head (rotation about +Y); forward is -Z
    const float yaw = std::atan2(2.f * (q.w * q.y + q.x * q.z), 1.f - 2.f * (q.y * q.y + q.x * q.x));
    const float dist = config::Get().vrScreenDistance;
    s.quadPose.orientation = {0, std::sin(yaw / 2), 0, std::cos(yaw / 2)};
    s.quadPose.position = {loc.pose.position.x - std::sin(yaw) * dist, loc.pose.position.y,
                           loc.pose.position.z - std::cos(yaw) * dist};
    const float uiDist = config::Get().vrUiDistance;
    s.uiPose.orientation = s.quadPose.orientation;
    s.uiPose.position = {loc.pose.position.x - std::sin(yaw) * uiDist, loc.pose.position.y,
                         loc.pose.position.z - std::cos(yaw) * uiDist};
    {
        std::lock_guard l(g_refMu);
        g_ref.orientation = {0, std::sin(yaw / 2), 0, std::cos(yaw / 2)};
        g_ref.position = loc.pose.position;
    }
    s.needRecenter = false;
    LOGP("[xr] recentered: head (%.2f, %.2f, %.2f) yaw %.1f deg; screen %.2f m ahead", loc.pose.position.x,
        loc.pose.position.y, loc.pose.position.z, yaw * 57.2958f, dist);
}

// ---- frame submission (render thread) ----
FrameRecord g_rec;          // last record delivered by render command
ID3D11Texture2D* g_uiTex = nullptr;  // UI layer image (owned by the UI layer module)
std::atomic<float> g_uiPanelW{0.f};
std::atomic<bool> g_immersiveScene{false};  // the game is in a VR scene (game thread; kept through blocking loads)
std::atomic<float> g_uiPanelDrop{0.f};  // metres below the recentered panel place  // the panel quad's width (metres; 0 = ui_width)
uint64_t g_uiFrame = ~0ull;
bool g_recFresh = false;    // a record arrived since the last Present
struct OpenStereo {
    bool open = false;
    bool begun = false;  // early wait: this stereo frame's xrWaitFrame / xrBeginFrame already ran (eye 0's first command)
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    uint32_t idx = 0;
    FrameRecord eye[2];
    uint32_t eyeW = 0, eyeH = 0;
} g_st;
bool g_loggedViews = false;
// The last frame the headset was given, whatever kind (stereo + panel, loading surround + panel, flat screen):
// resubmitted for a frame-lock hold and whenever a begun frame has no fresh image - an empty submit is black in the
// headset (0.9.58: still black at transitions - the loading screen had cleared the stereo-only copy). Its layers
// name the swapchains, whose last released images the compositor shows.
struct HeldLayer {
    int type = 0;  // 0 none, 1 projection, 2 quad
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView pv[2]{};
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
};
struct HeldLayers {
    bool valid = false;
    int n = 0;
    HeldLayer l[2];
} g_held;
void InvalidateHeld() { g_held.valid = false; }
std::atomic<XrTime> g_lastRealDisplay{0};  // display time of the last real (rendered) stereo frame (RT writes, game thread reads)
std::atomic<float> g_slotEst{1.f};  // display slots per real frame (smoothed): what the head pose is predicted for
uint64_t g_heldFrames = 0, g_emptyFrames = 0;  // frames submitted with no layers (black in the headset)
void FlushRT();  // below
uint64_t g_stereoFrames = 0, g_monoFrames = 0, g_screenFrames = 0, g_droppedStereo = 0;

bool LoadingFrame(ID3D11Device* dev, ID3D11Texture2D* bb, const D3D11_TEXTURE2D_DESC& d);  // below
bool BeginXrFrame(XrFrameState& fs) {
    fs = {XR_TYPE_FRAME_STATE};
    if (XR_FAILED(xrWaitFrame(s.session, nullptr, &fs))) return false;
    g_lastDisplayTime = fs.predictedDisplayTime;
    g_displayPeriod = fs.predictedDisplayPeriod;
    if (XR_FAILED(xrBeginFrame(s.session, nullptr))) return false;
    if (g_recenter.exchange(false)) s.needRecenter = true;
    if (s.recenterAt && fs.predictedDisplayTime >= s.recenterAt) s.recenterAt = 0, s.needRecenter = true;
    if (s.needRecenter) Recenter(fs.predictedDisplayTime);
    if (config::Get().vrControllers) {
        XrPosef ref;
        {
            std::lock_guard l(g_refMu);
            ref = g_ref;
        }
        input::Sync(s.session, s.local, fs.predictedDisplayTime, ref);
        vr::ctrl::UpdateFromXr();
    }
    return true;
}

ID3D11Device* g_watchDev = nullptr;  // the device of the frame being submitted (stall watchdog)
void WatchFrame(ID3D11Device* dev);
void EndXrFrame(const XrFrameState& fs, XrCompositionLayerBaseHeader* layer, XrCompositionLayerBaseHeader* layer2 = nullptr,
                bool keep = true) {
    if (keep && layer) {  // remember what the headset is shown (EndHeld)
        int n = 0;
        for (XrCompositionLayerBaseHeader* h : {layer, layer2}) {
            if (!h || n >= 2) continue;
            HeldLayer& d = g_held.l[n];
            if (h->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
                const auto* p = reinterpret_cast<const XrCompositionLayerProjection*>(h);
                if (p->viewCount != 2 || !p->views) continue;
                d.type = 1, d.proj = *p, d.pv[0] = p->views[0], d.pv[1] = p->views[1];
                d.proj.views = d.pv, d.proj.next = nullptr;
            } else if (h->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
                d.type = 2, d.quad = *reinterpret_cast<const XrCompositionLayerQuad*>(h), d.quad.next = nullptr;
            } else {
                continue;
            }
            ++n;
        }
        g_held.n = n, g_held.valid = n > 0;
    }
    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    XrCompositionLayerBaseHeader* layers[2] = {layer, layer2};
    ei.layerCount = layer ? (layer2 ? 2 : 1) : 0;
    ei.layers = layer ? layers : nullptr;
    if (!layer) ++g_emptyFrames;
    XrResult r = xrEndFrame(s.session, &ei);
    WatchFrame(g_watchDev);
    if (XR_FAILED(r)) LOG("[xr] xrEndFrame failed: %d", int(r));
    if (++s.frames % 900 == 1)
        LOGP("[xr] frame %llu (period %.2f ms): stereo %llu, mono %llu, screen %llu, dropped stereo %llu, empty (black) %llu",
            s.frames, double(fs.predictedDisplayPeriod) / 1e6, g_stereoFrames, g_monoFrames, g_screenFrames, g_droppedStereo,
            g_emptyFrames);
}

// Video memory (0.9.16: GPU stalls, likely a full card): a swapchain unused for 10 s is released - the
// single-width one (3 x eye size, ~165 MB at 3640x3782) is only used for loading moments in VR scenes, the stereo
// and panel ones (~420 MB) not at all in virtual-screen scenes. Recreated on the next use (EnsureSwapchain).
// Off by default (release_idle_swapchains=1 enables it): capture layers such as OpenXR OBS Mirror stop publishing when
// a swapchain is destroyed until another is created (0.9.23: "0/3 shared texture handles"), and the saving made
// no visible difference.
void ReleaseIdleSwapchains(bool stereoOpen) {
    if (!config::Get().vrReleaseIdleSwapchains) return;
    const ULONGLONG now = GetTickCount64();
    for (int i = 0; i < 4; ++i) {
        auto& w = s.sc[i];
        if (!w.handle || (i == 1 && stereoOpen) || now - w.lastUse < 10000) continue;
        LOG("[xr] swapchain[%d] %ux%u unused for %.0f s: released", i, w.width, w.height, (now - w.lastUse) / 1000.0);
        xrDestroySwapchain(w.handle);
        w = {};
        g_held.valid = false;
    }
}
bool AcquireImage(int which, uint32_t& idx) {
    s.sc[which].lastUse = GetTickCount64();
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    return XR_SUCCEEDED(xrAcquireSwapchainImage(s.sc[which].handle, nullptr, &idx)) &&
           XR_SUCCEEDED(xrWaitSwapchainImage(s.sc[which].handle, &wi));
}

void CopyTo(ID3D11Device* dev, ID3D11Texture2D* dst, ID3D11Texture2D* src, uint32_t dstX) {
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    if (dstX == 0) {
        D3D11_TEXTURE2D_DESC dd{}, sd{};
        dst->GetDesc(&dd);
        src->GetDesc(&sd);
        if (dd.Width == sd.Width && dd.Height == sd.Height) {
            ctx->CopyResource(dst, src);
            ctx->Release();
            return;
        }
    }
    ctx->CopySubresourceRegion(dst, 0, dstX, 0, 0, src, 0, nullptr);
    ctx->Release();
}

// Stall watchdog (0.9.15/0.9.16: the view "froze" - the middle stopped updating, the edges followed the head - in
// a stage, on the title's save screen and in the Academy Store). Cause found in Launch.log: "Timed out while waiting
// for GPU to catch up (500 ms)" 3-6 times in a row each time - the GPU stalled 1-2.5 s, no frames reached the headset
// and the compositor reprojected the last one. Logged here: any gap of 0.4 s or more between submitted frames, with
// the video memory this process uses against its budget (DXGI; also every 30 s), and the eye pose not changing.
void (*g_freezeReporter)(const char*) = nullptr;
IDXGIAdapter3* g_adapter3 = nullptr;
void LogVram(const char* when, ID3D11Device* dev) {
    if (!g_adapter3 && dev) {
        IDXGIDevice* dd = nullptr;
        IDXGIAdapter* ad = nullptr;
        if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dd))) && SUCCEEDED(dd->GetAdapter(&ad)))
            ad->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&g_adapter3));
        if (ad) ad->Release();
        if (dd) dd->Release();
    }
    DXGI_QUERY_VIDEO_MEMORY_INFO li{}, nl{};
    if (!g_adapter3 || FAILED(g_adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &li))) return;
    g_adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nl);
    LOG("[watch] %s: video memory used %llu MB of a %llu MB budget (%.0f%%); shared memory used %llu MB", when,
        li.CurrentUsage >> 20, li.Budget >> 20, li.Budget ? 100.0 * double(li.CurrentUsage) / double(li.Budget) : 0.0,
        nl.CurrentUsage >> 20);
}
struct Watch {
    ULONGLONG lastFrame = 0, lastVram = 0, tPose = 0, poseSince = 0;
    XrPosef pose{};
    bool poseFrozen = false;
    int stalls = 0;
} g_watch;
void WatchFrame(ID3D11Device* dev) {  // every submitted XR frame
    if (!config::Get().vrFreezeWatch) return;
    Watch& w = g_watch;
    const ULONGLONG now = GetTickCount64();
    if (w.lastFrame && now - w.lastFrame >= 400 && w.stalls < 200) {
        ++w.stalls;
        LOG("[watch] no new frame for %llu ms (the game's frame/GPU stalled; the headset reprojected the last frame)", now - w.lastFrame);
        LogVram("after the stall", dev);
        w.lastVram = now;
    }
    w.lastFrame = now;
    if (now - w.lastVram >= 30000) w.lastVram = now, LogVram("every 30 s", dev);
}
void WatchPose(const XrPosef& pose) {  // stereo frames: a tracked head never holds perfectly still
    if (!config::Get().vrFreezeWatch) return;
    Watch& w = g_watch;
    const ULONGLONG now = GetTickCount64();
    if (!w.tPose || memcmp(&pose, &w.pose, sizeof pose) != 0) w.pose = pose, w.tPose = now;
    if (now - w.tPose > 1000 && !w.poseFrozen) {
        w.poseFrozen = true, w.poseSince = w.tPose;
        LOG("[watch] eye pose unchanged for %.1f s", (now - w.tPose) / 1000.0);
        if (g_freezeReporter) g_freezeReporter("eye pose frozen");
    } else if (w.poseFrozen && now - w.tPose < 1000) {
        w.poseFrozen = false;
        LOG("[watch] eye pose moving again after %.1f s", (now - w.poseSince) / 1000.0);
    }
}

// A started frame without a fresh stereo pair: the last complete frame again (its layers name the swapchains, whose
// last released images the compositor shows) instead of an empty submit - empty is black in the headset (0.9.57:
// a black screen at every menu / stage / loading transition, up to ~150 frames at a stage load)
void EndHeld(const XrFrameState& fs) {
    if (!g_held.valid) {
        EndXrFrame(fs, nullptr);
        return;
    }
    XrCompositionLayerBaseHeader* h[2]{};
    for (int i = 0; i < g_held.n && i < 2; ++i) {
        HeldLayer& l = g_held.l[i];
        l.proj.views = l.pv;  // (the copy owns its views)
        h[i] = l.type == 1 ? reinterpret_cast<XrCompositionLayerBaseHeader*>(&l.proj)
                           : reinterpret_cast<XrCompositionLayerBaseHeader*>(&l.quad);
    }
    EndXrFrame(fs, h[0], h[1], false);
}
void CloseOpenStereo() {  // a stereo frame was started but its right eye never arrived
    xrReleaseSwapchainImage(s.sc[1].handle, nullptr);
    EndHeld(g_st.fs);
    g_st.open = false;
    ++g_droppedStereo;
}

// The frame was rendered with a symmetric frustum (half-angles r.halfH/halfV) that covers the eye's
// asymmetric FOV. Submit exactly the eye's FOV and the matching sub-rectangle of the image: runtimes
// (Quest Link) map the image rect onto the submitted FOV, and may clamp FOVs beyond the eye's own.
void FillView(XrCompositionLayerProjectionView& v, const FrameRecord& r, int eye, int which, int32_t x, uint32_t w,
              uint32_t h) {
    v = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    v.pose = r.pose;
    v.subImage.swapchain = s.sc[which].handle;
    XrFovf f = r.fov[eye];
    const float th = std::tan(r.halfH), tv = std::tan(r.halfV);
    const bool cropOk = th > 0 && tv > 0 && config::Get().vrCropToEyeFov && f.angleRight > f.angleLeft &&
                        -std::tan(f.angleLeft) <= th + 1e-4f && std::tan(f.angleRight) <= th + 1e-4f &&
                        std::tan(f.angleUp) <= tv + 1e-4f && -std::tan(f.angleDown) <= tv + 1e-4f;
    if (!cropOk) {  // fall back to the full symmetric image
        v.fov = {-r.halfH, r.halfH, r.halfV, -r.halfV};
        v.subImage.imageRect = {{x, 0}, {int32_t(w), int32_t(h)}};
        return;
    }
    // pixel = centre + tan(angle)/tan(half) * half-size; image y grows downwards
    const float x0 = 0.5f * w * (1.f + std::tan(f.angleLeft) / th), x1 = 0.5f * w * (1.f + std::tan(f.angleRight) / th);
    const float y0 = 0.5f * h * (1.f - std::tan(f.angleUp) / tv), y1 = 0.5f * h * (1.f - std::tan(f.angleDown) / tv);
    const int32_t ix0 = int32_t(std::floor(x0)), iy0 = int32_t(std::floor(y0));
    const int32_t ix1 = int32_t(std::ceil(x1)), iy1 = int32_t(std::ceil(y1));
    v.subImage.imageRect = {{x + ix0, iy0}, {ix1 - ix0, iy1 - iy0}};
    // FOV of the integer rect (exactly consistent with the pixels submitted)
    v.fov.angleLeft = std::atan((2.f * ix0 / w - 1.f) * th);
    v.fov.angleRight = std::atan((2.f * ix1 / w - 1.f) * th);
    v.fov.angleUp = std::atan((1.f - 2.f * iy0 / h) * tv);
    v.fov.angleDown = std::atan((1.f - 2.f * iy1 / h) * tv);
}

// ---- performance telemetry (every 5 s while stereo): frame rate, game-thread and render-thread times, GPU time per
// eye (D3D11 timestamps at each eye's first render command and at its Present). To find what limits the frame rate.
struct PerfAcc {
    double n = 0, xrWait = 0, gameFrame = 0, gameTick = 0, gameEye[2]{}, rtEye[2]{}, gpuEye[2]{}, gpuN = 0, maxFrame = 0, maxGpu = 0;
    double gpuUi[2]{}, gpuUiN[2]{}, rtUi[2]{};
};
std::mutex g_perfMu;
PerfAcc g_perf;
ULONGLONG g_perfNext = 0;
double g_rtEyeStart = 0;
double QpcMs() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) * 1000.0 / double(f.QuadPart);
}
struct GpuSet {
    ID3D11Query* disjoint = nullptr;
    ID3D11Query* ts[4]{};  // eye 0 start, eye 0 present, eye 1 start, eye 1 present
    ID3D11Query* ui[4]{};  // the mod's UI pass per eye: start, end
    bool open = false, pending = false, uiUsed[2]{};
};
GpuSet g_gq[4];
int g_gqCur = 0;
uint32_t g_gpuRejDisjoint = 0, g_gpuRejOrder = 0, g_gpuBusy = 0;  // why timestamp sets weren't used (perf report)
ID3D11DeviceContext* PerfCtx() {
    static ID3D11DeviceContext* ctx = nullptr;
    if (!ctx && s.device) s.device->GetImmediateContext(&ctx);
    return ctx;
}
void GpuCollect() {
    ID3D11DeviceContext* ctx = PerfCtx();
    if (!ctx) return;
    for (auto& q : g_gq) {
        if (!q.pending) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        if (ctx->GetData(q.disjoint, &dj, sizeof dj, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        UINT64 t[4]{};
        bool ok = true;
        for (int i = 0; i < 4 && ok; ++i) ok = ctx->GetData(q.ts[i], &t[i], sizeof t[i], D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK;
        if (!ok) continue;
        q.pending = false;
        if (dj.Disjoint || !dj.Frequency) {
            ++g_gpuRejDisjoint;
            continue;
        }
        if (t[1] < t[0] || t[3] < t[2] || t[2] < t[1]) {  // scene changes: skip
            ++g_gpuRejOrder;
            continue;
        }
        const double k = 1000.0 / double(dj.Frequency), e0 = double(t[1] - t[0]) * k, e1 = double(t[3] - t[2]) * k;
        double ui[2]{-1, -1};
        for (int e = 0; e < 2; ++e) {
            UINT64 u0 = 0, u1 = 0;
            if (q.uiUsed[e] && ctx->GetData(q.ui[e * 2], &u0, sizeof u0, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                ctx->GetData(q.ui[e * 2 + 1], &u1, sizeof u1, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && u1 >= u0)
                ui[e] = double(u1 - u0) * k;
        }
        std::lock_guard l(g_perfMu);
        for (int e = 0; e < 2; ++e)
            if (ui[e] >= 0) g_perf.gpuUi[e] += ui[e], g_perf.gpuUiN[e] += 1;
        g_perf.gpuEye[0] += e0, g_perf.gpuEye[1] += e1, g_perf.gpuN += 1, g_perf.maxGpu = std::fmax(g_perf.maxGpu, e0 + e1);
    }
}
void GpuMark(int eye, bool start) {  // render thread
    ID3D11DeviceContext* ctx = PerfCtx();
    if (!ctx || !s.device) return;
    if (eye == 0 && start) {
        GpuCollect();
        GpuSet& q = g_gq[g_gqCur];
        if (q.pending || q.open) {  // still in flight: skip this frame
            ++g_gpuBusy;
            return;
        }
        if (!q.disjoint) {
            D3D11_QUERY_DESC d{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
            s.device->CreateQuery(&d, &q.disjoint);
            d.Query = D3D11_QUERY_TIMESTAMP;
            for (auto& t : q.ts) s.device->CreateQuery(&d, &t);
            for (auto& t : q.ui) s.device->CreateQuery(&d, &t);
        }
        if (!q.disjoint || !q.ts[3] || !q.ui[3]) return;
        ctx->Begin(q.disjoint);
        q.open = true, q.uiUsed[0] = q.uiUsed[1] = false;
    }
    GpuSet& q = g_gq[g_gqCur];
    if (!q.open) return;
    ctx->End(q.ts[eye * 2 + (start ? 0 : 1)]);
    if (eye == 1 && !start) {
        ctx->End(q.disjoint);
        q.open = false, q.pending = true;
        g_gqCur = (g_gqCur + 1) % 4;
    }
}
double g_uiStartMs[2]{};
void UiMarkImpl(int eye, bool start) {  // render thread: the mod's UI pass of this eye begins / ends
    if (eye < 0 || eye > 1) return;
    ID3D11DeviceContext* ctx = PerfCtx();
    GpuSet& q = g_gq[g_gqCur];
    if (ctx && q.open) {
        ctx->End(q.ui[eye * 2 + (start ? 0 : 1)]);
        if (!start) q.uiUsed[eye] = true;
    }
    if (start) {
        g_uiStartMs[eye] = QpcMs();
    } else {
        std::lock_guard l(g_perfMu);
        g_perf.rtUi[eye] += QpcMs() - g_uiStartMs[eye];
    }
}
void PerfReport() {
    const ULONGLONG now = GetTickCount64();
    if (now < g_perfNext) return;
    const bool first = g_perfNext == 0;
    g_perfNext = now + 5000;
    PerfAcc a;
    {
        std::lock_guard l(g_perfMu);
        a = g_perf;
        g_perf = {};
    }
    if (first || a.n < 1) return;
    const double n = a.n, g = a.gpuN > 0 ? a.gpuN : 1;
    const double u0 = a.gpuUiN[0] > 0 ? a.gpuUi[0] / a.gpuUiN[0] : 0, u1 = a.gpuUiN[1] > 0 ? a.gpuUi[1] / a.gpuUiN[1] : 0;
    LOG("[perf] mod UI pass: GPU %.2f + %.2f ms, render thread %.2f + %.2f ms per eye | %.2f display slots per frame, %llu held",
        u0, u1, a.rtUi[0] / n, a.rtUi[1] / n, g_slotEst.load(), (unsigned long long)g_heldFrames);
    LOG("[perf] %.1f fps (frame %.1f ms avg, %.1f max) | game: logic %.1f ms, eye draws %.1f + %.1f ms | render thread: "
        "eyes %.1f + %.1f ms, headset wait %.1f ms | GPU: eyes %.1f + %.1f ms (%.1f max/frame) | render %ux%u",
        1000.0 * n / (a.gameFrame > 0 ? a.gameFrame : 1), a.gameFrame / n, a.maxFrame, a.gameTick / n, a.gameEye[0] / n,
        a.gameEye[1] / n, a.rtEye[0] / n, a.rtEye[1] / n, a.xrWait / n, a.gpuEye[0] / g, a.gpuEye[1] / g, a.maxGpu, g_recW.load(), g_recH.load());
    if (a.gpuN < 1) {  // no GPU timing this period: why
        static int s_logs = 0;
        if (s_logs++ < 20) LOG("[perf] GPU timing unavailable: %u sets disjoint, %u out of order, %u frames skipped (sets busy)",
                               g_gpuRejDisjoint, g_gpuRejOrder, g_gpuBusy);
    }
    g_gpuRejDisjoint = g_gpuRejOrder = g_gpuBusy = 0;
}

void Frame(IDXGISwapChain* sc, ID3D11Device* dev) {
    g_watchDev = dev;
    ReleaseIdleSwapchains(g_st.open);
    const bool have = g_recFresh;
    const FrameRecord rec = g_rec;
    g_recFresh = false;
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb)))) return;
    D3D11_TEXTURE2D_DESC d{};
    bb->GetDesc(&d);
    g_aspect = float(d.Width) / float(d.Height);
    if (d.SampleDesc.Count != 1) {
        bb->Release();
        return;
    }
    if (have && rec.valid && rec.eye == 2) {  // zoom scope render (kept in OnPresent): not submitted
        bb->Release();
        return;
    }
    const bool stereoEye = have && rec.valid && (rec.eye == 0 || rec.eye == 1);
    if (config::Get().vrPerfLog && stereoEye) {  // the eye's draw ends here
        GpuMark(rec.eye, false);
        std::lock_guard l(g_perfMu);
        g_perf.rtEye[rec.eye] += QpcMs() - g_rtEyeStart;
    }
    if (config::Get().vrPerfLog && stereoEye && rec.eye == 1) PerfReport();
    if (stereoEye && rec.eye == 0) {
        if (g_st.open) CloseOpenStereo();
        bool began = false;
        if (g_st.begun) {  // waited and begun before eye 0 rendered (EarlyWait)
            began = true, g_st.begun = false;
        } else {
            FlushRT();  // eye 0's work must reach the GPU before this thread blocks in the wait
            const double tw = QpcMs();
            began = BeginXrFrame(g_st.fs);
            if (config::Get().vrPerfLog) {
                std::lock_guard l(g_perfMu);
                g_perf.xrWait += QpcMs() - tw;
            }
        }
        if (began) {
            if (g_st.fs.shouldRender && EnsureSwapchain(d, 1) && AcquireImage(1, g_st.idx)) {
                CopyTo(dev, s.sc[1].images[g_st.idx].texture, bb, 0);
                g_st.eye[0] = rec;
                g_st.eyeW = d.Width, g_st.eyeH = d.Height;
                g_st.open = true;
            } else {
                EndHeld(g_st.fs);
            }
        }
        bb->Release();
        return;
    }
    if (stereoEye && rec.eye == 1) {
        if (g_st.open && d.Width == g_st.eyeW && d.Height == g_st.eyeH) {
            CopyTo(dev, s.sc[1].images[g_st.idx].texture, bb, d.Width);
            xrReleaseSwapchainImage(s.sc[1].handle, nullptr);
            g_st.eye[1] = rec;
            XrCompositionLayerProjectionView pv[2];
            FillView(pv[0], g_st.eye[0], 0, 1, 0, d.Width, d.Height);
            FillView(pv[1], g_st.eye[1], 1, 1, int32_t(d.Width), d.Width, d.Height);
            if (!g_loggedViews) {
                g_loggedViews = true;
                for (int e = 0; e < 2; ++e)
                    LOG("[xr] view %d: rect (%d,%d %dx%d) fov L%.1f R%.1f U%.1f D%.1f deg (render half H%.1f V%.1f)", e,
                        pv[e].subImage.imageRect.offset.x, pv[e].subImage.imageRect.offset.y,
                        pv[e].subImage.imageRect.extent.width, pv[e].subImage.imageRect.extent.height,
                        pv[e].fov.angleLeft * 57.2958f, pv[e].fov.angleRight * 57.2958f, pv[e].fov.angleUp * 57.2958f,
                        pv[e].fov.angleDown * 57.2958f, g_st.eye[e].halfH * 57.2958f, g_st.eye[e].halfV * 57.2958f);
            }
            XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            proj.space = s.local;
            proj.viewCount = 2;
            proj.views = pv;
            ++g_stereoFrames;
            // Stage 5: the frame's UI on a floating panel
            XrCompositionLayerQuad uiQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
            XrCompositionLayerBaseHeader* uiLayer = nullptr;
            if (g_uiTex && g_uiFrame == rec.frameId) {
                D3D11_TEXTURE2D_DESC ud{};
                g_uiTex->GetDesc(&ud);
                uint32_t uidx = 0;
                if (EnsureSwapchain(ud, 2) && AcquireImage(2, uidx)) {
                    CopyTo(dev, s.sc[2].images[uidx].texture, g_uiTex, 0);
                    xrReleaseSwapchainImage(s.sc[2].handle, nullptr);
                    const float w = g_uiPanelW.load() > 0.f ? g_uiPanelW.load() : config::Get().vrUiWidth;
                    uiQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
                                        XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
                    uiQuad.space = s.local;
                    uiQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                    uiQuad.subImage.swapchain = s.sc[2].handle;
                    uiQuad.subImage.imageRect = {{0, 0}, {int32_t(ud.Width), int32_t(ud.Height)}};
                    uiQuad.pose = UiPanelPoseRT();
                    uiQuad.size = {w, w * float(ud.Height) / float(ud.Width)};
                    uiLayer = reinterpret_cast<XrCompositionLayerBaseHeader*>(&uiQuad);
                    // the desktop window shows this eye: the panel goes into it too (the headset's copy is made above)
                    if (config::Get().vrDesktopMirror) vr::ui::MirrorPanelRT(dev, bb, rec);
                }
            }
            EndXrFrame(g_st.fs, reinterpret_cast<XrCompositionLayerBaseHeader*>(&proj), uiLayer);
            g_st.open = false;
            WatchPose(g_st.eye[1].pose);
        }
        bb->Release();
        return;
    }
    if (g_st.open) CloseOpenStereo();
    if (g_st.begun) EndHeld(g_st.fs), g_st.begun = false;  // begun early, but no stereo frame followed
    // a loading screen in a VR scene (0.9.42: "turn it full white ... a white surrounding thing with the Now
    // Loading text and chibi girl in front of you"): the loading image's own background colour all around, its 16:9
    // part on the HUD panel - its background blends into the surround, so no screen edge shows
    if (!have && g_immersiveScene.load() && config::Get().vrImmersiveLoading && LoadingFrame(dev, bb, d)) {
        bb->Release();
        return;
    }
    // mono head-tracked frame, or the virtual screen (menus without a view, loading movies)
    XrFrameState fs;
    if (!BeginXrFrame(fs)) {
        bb->Release();
        return;
    }
    XrCompositionLayerBaseHeader* layer = nullptr;
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView pv[2];
    uint32_t idx = 0;
    if (fs.shouldRender && EnsureSwapchain(d, 0) && AcquireImage(0, idx)) {
        CopyTo(dev, s.sc[0].images[idx].texture, bb, 0);
        xrReleaseSwapchainImage(s.sc[0].handle, nullptr);
        if (have && rec.eye == -1 && rec.valid) {
            FillView(pv[0], rec, 0, 0, 0, d.Width, d.Height);
            FillView(pv[1], rec, 1, 0, 0, d.Width, d.Height);
            proj.space = s.local;
            proj.viewCount = 2;
            proj.views = pv;
            layer = reinterpret_cast<XrCompositionLayerBaseHeader*>(&proj);
            ++g_monoFrames;
        } else {
            const float w = config::Get().vrScreenWidth;
            quad.space = s.local;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = s.sc[0].handle;
            quad.subImage.imageRect = {{0, 0}, {int32_t(d.Width), int32_t(d.Height)}};
            quad.pose = s.quadPose;
            quad.size = {w, w * float(d.Height) / float(d.Width)};
            layer = reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad);
            ++g_screenFrames;
        }
    }
    EndXrFrame(fs, layer);
    bb->Release();
}

// The loading frame: a solid surround (swapchain 3, cleared to the image's corner colour, as a head-posed projection
// layer with a wide FOV - the same colour everywhere, so the pose need not be exact) + the image's 16:9 band on a quad
// at the panel's place and size.
bool LoadingFrame(ID3D11Device* dev, ID3D11Texture2D* bb, const D3D11_TEXTURE2D_DESC& d) {
    uint32_t bx, by, bw, bh;
    vr::ui::BandOf(d.Width, d.Height, bx, by, bw, bh);
    if (!bw || !bh) return false;
    // the corner colour of the band (read back: loading frames only)
    static ID3D11Texture2D* s_stage = nullptr;
    static DXGI_FORMAT s_stageFmt = DXGI_FORMAT_UNKNOWN;
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    if (s_stage && s_stageFmt != d.Format) s_stage->Release(), s_stage = nullptr;
    if (!s_stage) {
        D3D11_TEXTURE2D_DESC sd{};
        sd.Width = 4, sd.Height = 1, sd.MipLevels = sd.ArraySize = 1, sd.Format = d.Format, sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_STAGING, sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev->CreateTexture2D(&sd, nullptr, &s_stage))) s_stage = nullptr;
        s_stageFmt = d.Format;
    }
    float col[4] = {1.f, 1.f, 1.f, 1.f};
    bool stale = false;  // loading_skip_stale: a half-drawn frame just after the resize
    if (s_stage) {
        // samples: the band's top-left (the surround colour), centre, bottom-left, bottom-right
        const uint32_t px[4][2] = {{bx + 6, by + 6}, {bx + bw / 2, by + bh / 2}, {bx + 6, by + bh - 7}, {bx + bw - 7, by + bh - 7}};
        for (uint32_t k = 0; k < 4; ++k) {
            D3D11_BOX box{px[k][0], px[k][1], 0, px[k][0] + 1, px[k][1] + 1, 1};
            ctx->CopySubresourceRegion(s_stage, 0, k, 0, 0, bb, 0, &box);
        }
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(s_stage, 0, D3D11_MAP_READ, 0, &m))) {
            const uint8_t* p = static_cast<const uint8_t*>(m.pData);
            const bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                              d.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
            // (0.9.74: after Start Game the resized backbuffer is black but for the old 1920x1080 image at its
            // top left - a black box with a cut-out on the panel in the white void)
            int black = 0;
            for (int k = 1; k < 4; ++k) black += p[k * 4] <= 3 && p[k * 4 + 1] <= 3 && p[k * 4 + 2] <= 3;
            const bool bright = p[0] > 200 && p[1] > 200 && p[2] > 200;
            stale = config::Get().vrLoadingSkipStale && bright && black >= 2;
            static int s_staleLogs = 0;
            if (stale && s_staleLogs++ < 5)
                LOG("[xr] loading frame half drawn (top-left bright, %d of 3 other samples black): surround only, no panel", black);
            const uint8_t r = bgra ? p[2] : p[0], g = p[1], b = bgra ? p[0] : p[2];
            // the bytes are copied raw into the sRGB swapchains (as the game image is): the clear (linear) is the
            // value those bytes stand for
            auto lin = [](uint8_t v) {
                const float c = v / 255.f;
                return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
            };
            col[0] = lin(r), col[1] = lin(g), col[2] = lin(b);
            ctx->Unmap(s_stage, 0);
        }
    }
    XrFrameState fs;
    if (!BeginXrFrame(fs)) {
        ctx->Release();
        return true;
    }
    XrCompositionLayerBaseHeader *l0 = nullptr, *l1 = nullptr;
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView pv[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    uint32_t i3 = 0, i0 = 0;
    D3D11_TEXTURE2D_DESC fd = d;
    fd.Width = fd.Height = 64;
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    const bool head = XR_SUCCEEDED(xrLocateSpace(s.view, s.local, fs.predictedDisplayTime, &loc)) &&
                      (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
    if (fs.shouldRender && head && EnsureSwapchain(fd, 3) && AcquireImage(3, i3)) {
        ID3D11RenderTargetView* rtv = nullptr;
        D3D11_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = s.sc[3].format, rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (SUCCEEDED(dev->CreateRenderTargetView(s.sc[3].images[i3].texture, &rd, &rtv))) {
            ctx->ClearRenderTargetView(rtv, col);
            rtv->Release();
        }
        xrReleaseSwapchainImage(s.sc[3].handle, nullptr);
        for (int e = 0; e < 2; ++e) {
            pv[e].pose = loc.pose;
            pv[e].fov = {-1.4f, 1.4f, 1.4f, -1.4f};  // ~80 deg each way
            pv[e].subImage.swapchain = s.sc[3].handle;
            pv[e].subImage.imageRect = {{0, 0}, {64, 64}};
        }
        proj.space = s.local, proj.viewCount = 2, proj.views = pv;
        l0 = reinterpret_cast<XrCompositionLayerBaseHeader*>(&proj);
    }
    if (fs.shouldRender && !stale && EnsureSwapchain(d, 0) && AcquireImage(0, i0)) {
        CopyTo(dev, s.sc[0].images[i0].texture, bb, 0);
        xrReleaseSwapchainImage(s.sc[0].handle, nullptr);
        const float w = config::Get().vrUiWidth;
        quad.space = s.local;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        quad.subImage.swapchain = s.sc[0].handle;
        quad.subImage.imageRect = {{int32_t(bx), int32_t(by)}, {int32_t(bw), int32_t(bh)}};
        quad.pose = UiPanelPoseRT();
        quad.size = {w, w * float(bh) / float(bw)};
        (l0 ? l1 : l0) = reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad);
    }
    ctx->Release();
    EndXrFrame(fs, l0, l1);
    ++g_screenFrames;
    static int s_logs = 0;
    if (s_logs++ < 3)
        LOG("[xr] loading screen in a VR scene: surround colour (%.2f,%.2f,%.2f) linear, band %ux%u on the panel", col[0],
            col[1], col[2], bw, bh);
    return true;
}


uint64_t g_recordsRT = 0;
void FlushRT() {
    if (!s.device) return;
    ID3D11DeviceContext* ctx = nullptr;
    s.device->GetImmediateContext(&ctx);
    if (ctx) ctx->Flush(), ctx->Release();
}
// Pacing (perf 0.9.57): the headset wait (xrWaitFrame + xrBeginFrame) runs when eye 0's first render command runs,
// on the render thread - the thread of every other OpenXR call - instead of at eye 0's Present. Waiting there held
// eye 0's unflushed tail back from the GPU for the whole wait (wait and work in series: 36 fps at 72 Hz).
// frame_lock_hz: the previous frame is resubmitted for the extra slots so every frame is held equally (even cadence).
void EarlyWait() {
    if (g_st.open) CloseOpenStereo();
    if (g_st.begun) EndHeld(g_st.fs), g_st.begun = false, ++g_droppedStereo;
    FlushRT();
    const double tw = QpcMs();
    bool began = BeginXrFrame(g_st.fs);
    // frame_hold_slots (refresh-independent, 0.9.80: "not everybody's headset is set to 72 Hz") wins over the
    // absolute frame_lock_hz
    const int hold = std::min(config::Get().vrFrameHoldSlots, 6);
    const float lock = config::Get().vrFrameLockHz;
    if (began && (hold > 1 || lock > 1.f) && g_held.valid && g_lastRealDisplay && g_st.fs.predictedDisplayPeriod > 0) {
        const double refresh = 1e9 / double(g_st.fs.predictedDisplayPeriod);
        const int n = hold > 1 ? hold : std::max(1, int(std::lround(refresh / lock)));
        for (int guard = 0; began && n > 1 && guard < n &&
                            g_st.fs.predictedDisplayTime < g_lastRealDisplay.load() + XrTime((double(n) - 0.5) * g_st.fs.predictedDisplayPeriod);
             ++guard) {
            EndHeld(g_st.fs);
            ++g_heldFrames;
            began = BeginXrFrame(g_st.fs);
        }
        static int s_logs = 0;
        if (s_logs++ < 1) LOG("[xr] frame lock: every frame held %d slots at %.0f Hz = %.1f fps", n, refresh, refresh / n);
    }
    if (config::Get().vrPerfLog) {
        std::lock_guard l(g_perfMu);
        g_perf.xrWait += QpcMs() - tw;
    }
    g_st.begun = began;
    if (began) {
        if (g_lastRealDisplay && g_st.fs.predictedDisplayPeriod > 0) {
            const float sl = float(double(g_st.fs.predictedDisplayTime - g_lastRealDisplay.load()) / double(g_st.fs.predictedDisplayPeriod));
            if (sl > 0.5f && sl < 8.f) g_slotEst = g_slotEst.load() * 0.85f + sl * 0.15f;
        }
        g_lastRealDisplay = g_st.fs.predictedDisplayTime;
    }
    static int s_logs = 0;
    if (s_logs++ < 1) LOG("[xr] early wait: the headset wait now runs before eye 0 renders (early_wait=1)");
}

void OnFrameRecordRT(const void* p) {  // render thread, via UE3 render command
    memcpy(&g_rec, p, sizeof g_rec);
    if (config::Get().vrEarlyWait && g_rec.valid && g_rec.eye == 0 && s.session && s.running && !g_disabled) EarlyWait();
    if (config::Get().vrPerfLog && (g_rec.eye == 0 || g_rec.eye == 1)) {
        g_rtEyeStart = QpcMs();
        GpuMark(g_rec.eye, true);
    }
    g_recFresh = true;
    if (++g_recordsRT % 1800 == 1)
        LOG("[xr] frame record #%llu on thread %lu: eye %d valid %d frame %llu", g_recordsRT, GetCurrentThreadId(),
            g_rec.eye, g_rec.valid, g_rec.frameId);
}
}  // namespace

void OnPresentImpl(IDXGISwapChain* sc);

void RequestRecenter() { g_recenter = true; }
bool ProbeHeadsetAudio(std::wstring& out) {
    out.clear();
    static bool selected = false;
    if (!selected) SelectRuntime(), selected = true;
    uint32_t n = 0;
    if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr))) return false;
    std::vector<XrExtensionProperties> exts(n, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, n, &n, exts.data());
    bool has = false;
    for (auto& e : exts) has |= !strcmp(e.extensionName, XR_OCULUS_AUDIO_DEVICE_GUID_EXTENSION_NAME);
    if (!has) LOG("[audio] the OpenXR runtime has no %s (not Meta's): matching device names", XR_OCULUS_AUDIO_DEVICE_GUID_EXTENSION_NAME);
    const char* enabled[] = {XR_OCULUS_AUDIO_DEVICE_GUID_EXTENSION_NAME};
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ci.applicationInfo.applicationName, "GGDPVR audio probe");
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ci.enabledExtensionCount = has ? 1 : 0;
    ci.enabledExtensionNames = has ? enabled : nullptr;
    XrInstance inst = XR_NULL_HANDLE;
    const ULONGLONG t0 = GetTickCount64();
    if (XR_FAILED(xrCreateInstance(&ci, &inst))) return false;
    bool connected = false;
    XrSystemGetInfo gi{XR_TYPE_SYSTEM_GET_INFO};
    gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId sys = XR_NULL_SYSTEM_ID;
    PFN_xrGetAudioOutputDeviceGuidOculus fn = nullptr;
    if (XR_FAILED(xrGetSystem(inst, &gi, &sys))) {
        LOG("[audio] no headset connected (xrGetSystem failed)");
    } else {
        connected = true;
        XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
        xrGetInstanceProperties(inst, &ip);
        LOGP("[audio] headset connected (runtime '%s')", ip.runtimeName);
        if (has && XR_SUCCEEDED(xrGetInstanceProcAddr(inst, "xrGetAudioOutputDeviceGuidOculus", reinterpret_cast<PFN_xrVoidFunction*>(&fn))) && fn) {
            wchar_t buf[XR_MAX_AUDIO_DEVICE_STR_SIZE_OCULUS]{};
            const XrResult r = fn(inst, buf);
            buf[XR_MAX_AUDIO_DEVICE_STR_SIZE_OCULUS - 1] = 0;
            if (XR_SUCCEEDED(r) && buf[0]) out = buf;
            LOGP("[audio] headset audio output from the runtime: %s (result %d)", out.empty() ? "none" : "found", int(r));  // (no device ID in the log)
        }
    }
    xrDestroyInstance(inst);
    LOG("[audio] runtime query took %llu ms", GetTickCount64() - t0);
    return connected;
}
void RequestStereoDump() { g_dumpPending = 2; LOG("[xr] stereo dump requested"); }

bool Running() { return g_running; }
float BackbufferAspect() { return g_aspect; }
bool RecommendedEyeSize(uint32_t& w, uint32_t& h) {
    w = g_recW, h = g_recH;
    return w && h;
}

bool SampleHead(HeadSample& out) {
    std::shared_lock lock(g_sessionMu);
    if (!g_running || !s.session || !g_lastDisplayTime) return false;
    // the next frame's display time: one slot after the last one - or, when frames take more than a slot (or a frame
    // lock holds them), as many slots as recent frames took (predict_frame_slots)
    int slots = 1;
    if (config::Get().vrPredictSlots) slots = std::max(1, std::min(4, int(std::lround(g_slotEst.load()))));
    // The last real stereo frame's display time is only advanced by stereo frames: after a loading stretch on the flat
    // screen it is seconds old. Some runtimes give no pose that far back (WiVRn: none beyond 1 s from its samples),
    // and without a pose stereo never restarted - the stage stayed flat (1.0.0, Linux + WiVRn). It is used only while
    // recent; otherwise the newest predicted display time is the base.
    const XrTime period = g_displayPeriod.load(), fresh = g_lastDisplayTime.load();
    const XrTime lr = g_lastRealDisplay.load();
    const XrTime base = lr && fresh - lr <= 8 * period ? lr : fresh;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    XrView views[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    auto locate = [&](XrTime t) {
        XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
        li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        li.displayTime = t;
        li.space = s.local;
        vs = {XR_TYPE_VIEW_STATE};
        views[0] = {XR_TYPE_VIEW}, views[1] = {XR_TYPE_VIEW};
        loc = {XR_TYPE_SPACE_LOCATION};
        uint32_t n = 0;
        XrResult r = xrLocateViews(s.session, &li, &vs, 2, &n, views);
        if (XR_SUCCEEDED(r) && n != 2) r = XR_ERROR_VALIDATION_FAILURE;
        if (XR_SUCCEEDED(r)) r = xrLocateSpace(s.view, s.local, t, &loc);
        const bool ok = XR_SUCCEEDED(r) && (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
        if (!ok) {
            static ULONGLONG s_next = 0;
            static int s_logs = 0;
            if (s_logs < 20 && GetTickCount64() >= s_next)
                ++s_logs, s_next = GetTickCount64() + 2000,
                    LOG("[xr] no head pose for %.1f ms ahead of the newest frame (result %d, flags 0x%llX)",
                        double(t - fresh) / 1e6, int(r), (unsigned long long)loc.locationFlags);
        }
        return ok;
    };
    // a refused prediction is retried once at the newest display time
    if (!locate(base + period * slots) && (base == fresh || !locate(fresh + period * slots))) return false;
    if (!(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) loc.pose.position = {0, 0, 0};
    out.raw = loc.pose;
    out.fov[0] = views[0].fov;
    out.fov[1] = views[1].fov;
    XrPosef ref;
    {
        std::lock_guard l(g_refMu);
        ref = g_ref;
    }
    const XrQuaternionf inv{-ref.orientation.x, -ref.orientation.y, -ref.orientation.z, ref.orientation.w};
    out.rel.orientation = QMul(inv, loc.pose.orientation);
    out.rel.position = QRot(inv, {loc.pose.position.x - ref.position.x, loc.pose.position.y - ref.position.y,
                                  loc.pose.position.z - ref.position.z});
    for (int e = 0; e < 2; ++e) {
        const XrPosef& ep = views[e].pose;
        out.eyeRaw[e] = ep;
        out.eyeRel[e].orientation = QMul(inv, ep.orientation);
        out.eyeRel[e].position = QRot(inv, {ep.position.x - ref.position.x, ep.position.y - ref.position.y,
                                            ep.position.z - ref.position.z});
    }
    return true;
}

void QueueFrameRecord(const FrameRecord& rec) { ue3::render::Enqueue(&OnFrameRecordRT, &rec, sizeof rec); }

// Debug: dump the next stereo pair (both eye backbuffers) as BMPs to %LOCALAPPDATA%\GGDPVR\dump_eye{0,1}.bmp
void DumpTextureImpl(ID3D11Texture2D* bb, const std::wstring& stem) {
    ID3D11Device* dev = nullptr;
    bb->GetDevice(&dev);
    D3D11_TEXTURE2D_DESC d{};
    bb->GetDesc(&d);
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ID3D11Texture2D* st = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    dev->GetImmediateContext(&ctx);
    if (SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &st))) {
        ctx->CopyResource(st, bb);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            const bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            std::wstring path = log::Directory() + L"\\" + stem + L".bmp";
            FILE* f = nullptr;
            _wfopen_s(&f, path.c_str(), L"wb");
            if (f) {
                const uint32_t w = d.Width, h = d.Height, row = w * 3, pad = (4 - row % 4) % 4, img = (row + pad) * h;
                uint8_t hdr[54]{'B', 'M'};
                auto put32 = [&](int o, uint32_t v) { memcpy(hdr + o, &v, 4); };
                put32(2, 54 + img); put32(10, 54); put32(14, 40); put32(18, w); put32(22, h);
                hdr[26] = 1; hdr[28] = 24; put32(34, img);
                fwrite(hdr, 1, 54, f);
                std::vector<uint8_t> line(row + pad, 0);
                for (int y = int(h) - 1; y >= 0; --y) {
                    const uint8_t* src = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
                    for (uint32_t x = 0; x < w; ++x) {
                        const uint8_t* px = src + x * 4;
                        line[x * 3 + 0] = bgra ? px[0] : px[2];
                        line[x * 3 + 1] = px[1];
                        line[x * 3 + 2] = bgra ? px[2] : px[0];
                    }
                    fwrite(line.data(), 1, line.size(), f);
                }
                fclose(f);
                LOG("[xr] dumped %ls (%ux%u) -> %ls", stem.c_str(), w, h, path.c_str());
                // alpha channel as greyscale (UI-layer experiments)
                std::wstring apath = log::Directory() + L"\\" + stem + L"_alpha.bmp";
                FILE* fa = nullptr;
                _wfopen_s(&fa, apath.c_str(), L"wb");
                if (fa) {
                    fwrite(hdr, 1, 54, fa);
                    for (int y = int(h) - 1; y >= 0; --y) {
                        const uint8_t* src = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
                        for (uint32_t x = 0; x < w; ++x) line[x * 3 + 0] = line[x * 3 + 1] = line[x * 3 + 2] = src[x * 4 + 3];
                        fwrite(line.data(), 1, line.size(), fa);
                    }
                    fclose(fa);
                }
            }
            ctx->Unmap(st, 0);
        }
        st->Release();
    }
    ctx->Release();
    dev->Release();
}
void DumpBackbuffer(IDXGISwapChain* sc, int eye) {
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb)))) return;
    DumpTextureImpl(bb, L"dump_eye" + std::to_wstring(eye));
    bb->Release();
}
void DumpTexture(ID3D11Texture2D* tex, const wchar_t* stem) { DumpTextureImpl(tex, stem); }
std::atomic<IDXGISwapChain*> g_lastSc{nullptr};
IDXGISwapChain* SwapChainRT() { return g_lastSc; }
int CurrentEyeRT() { return g_recFresh && g_rec.valid ? g_rec.eye : -1; }
XrPosef RecenterRefRT() {
    std::lock_guard l(g_refMu);
    return g_ref;
}
bool DumpPending() { return g_dumpPending.load() > 0; }
uint64_t CurrentFrameIdRT() { return g_rec.frameId; }
bool CurrentRecordRT(FrameRecord& out) {
    if (!g_recFresh || !g_rec.valid) return false;
    out = g_rec;
    return true;
}
XrPosef UiPanelPoseRT() {
    XrPosef p = s.uiPose;
    p.position.y -= g_uiPanelDrop.load();
    return p;
}
void SetUiImageRT(ID3D11Texture2D* tex, uint64_t frameId) { g_uiTex = tex, g_uiFrame = frameId; }
void SetUiPanelWidth(float metres) { g_uiPanelW = metres; }
void PerfUiMark(int eye, bool start) {
    if (config::Get().vrPerfLog) UiMarkImpl(eye, start);
}
void PerfGame(double frameMs, double tickMs, double eye0Ms, double eye1Ms) {
    std::lock_guard l(g_perfMu);
    g_perf.n += 1, g_perf.gameFrame += frameMs, g_perf.gameTick += tickMs, g_perf.gameEye[0] += eye0Ms, g_perf.gameEye[1] += eye1Ms;
    g_perf.maxFrame = std::fmax(g_perf.maxFrame, frameMs);
}
void SetImmersiveScene(bool on) { g_immersiveScene = on; }
void SetUiPanelDrop(float metres) { g_uiPanelDrop = metres; }
void SetFreezeReporter(void (*fn)(const char*)) { g_freezeReporter = fn; }

uint32_t g_lastBbW = 0, g_lastBbH = 0;
std::atomic<uint64_t> g_bbSize{0};
uint64_t BackbufferSize() { return g_bbSize; }
// dev (!sharetest): OBS Mirror's copy texture (its layer.cpp: swapchain size + format, SRV, SHARED_KEYEDMUTEX) and
// variants on the game's device - which part does the device refuse? Runs on the render thread (single-threaded device).
std::atomic<bool> g_shareTest{false};
void RunShareTest(IDXGISwapChain* sc) {
    ID3D11Device* dev = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev)))) return;
    LOG("[sharetest] device FL 0x%X creation flags 0x%X", dev->GetFeatureLevel(), dev->GetCreationFlags());
    struct V { const char* name; DXGI_FORMAT fmt; UINT bind, misc, w, h; };
    const V vs[] = {
        {"mirror (sRGB, SRV, keyed mutex)", DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX, 1920, 1080},
        {"UNORM, SRV, keyed mutex", DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX, 1920, 1080},
        {"sRGB, SRV, shared (no mutex)", DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_SHARED, 1920, 1080},
        {"sRGB, SRV, not shared", DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, D3D11_BIND_SHADER_RESOURCE, 0, 1920, 1080},
        {"sRGB, SRV+RT, keyed mutex", DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX, 1920, 1080},
        {"sRGB, SRV, keyed mutex + NT handle", DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE, 1920, 1080},
        {"mirror, 64x64", DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX, 64, 64},
    };
    for (const V& v : vs) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = v.w, d.Height = v.h, d.MipLevels = 1, d.ArraySize = 1, d.Format = v.fmt, d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT, d.BindFlags = v.bind, d.MiscFlags = v.misc;
        ID3D11Texture2D* t = nullptr;
        const HRESULT hr = dev->CreateTexture2D(&d, nullptr, &t);
        LOG("[sharetest] %-40s -> hr 0x%08lX", v.name, hr);
        if (t) t->Release();
    }
    dev->Release();
}
void RequestShareTest() { g_shareTest = true; }

bool OnPresent(IDXGISwapChain* sc) {
    g_lastSc = sc;
    if (g_shareTest.exchange(false)) RunShareTest(sc);
    {
        DXGI_SWAP_CHAIN_DESC sd{};
        if (SUCCEEDED(sc->GetDesc(&sd)) && (sd.BufferDesc.Width != g_lastBbW || sd.BufferDesc.Height != g_lastBbH)) {
            g_lastBbW = sd.BufferDesc.Width, g_lastBbH = sd.BufferDesc.Height;
            g_bbSize = (uint64_t(g_lastBbW) << 32) | g_lastBbH;
            if (g_lastBbH) g_aspect = float(g_lastBbW) / float(g_lastBbH);  // also without a session (testing without a headset)
            LOG("[xr] backbuffer now %ux%u", g_lastBbW, g_lastBbH);
        }
    }
    if (g_dumpPending.load() > 0 && g_recFresh && g_rec.valid && (g_rec.eye == 0 || g_rec.eye == 1)) {
        // dump eye 0 first, then the matching eye 1
        if ((g_dumpPending == 2 && g_rec.eye == 0) || (g_dumpPending == 1 && g_rec.eye == 1)) {
            DumpBackbuffer(sc, g_rec.eye);
            --g_dumpPending;
        }
    }
    // Stereo: the left eye's frame only goes to the headset; presenting it to the desktop as well would
    // throttle every eye to the monitor refresh (two desktop presents per game tick).
    if (g_recFresh && g_rec.valid && g_rec.eye == 2) {  // zoom scope render: kept for the eyes' scope circle
        ID3D11Texture2D* bb = nullptr;
        if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb)))) {
            vr::ui::KeepScopeViewRT(bb, g_rec.frameId);
            bb->Release();
        }
    }
    const bool skipDesktop = g_recFresh && g_rec.valid &&
                             ((g_rec.eye == 0 && config::Get().vrMirrorLeftEyeSkip) || g_rec.eye == 2 ||
                              (g_rec.eye == 1 && !config::Get().vrDesktopMirror));
    OnPresentImpl(sc);
    return skipDesktop;
}

void OnPresentImpl(IDXGISwapChain* sc) {
    if (g_disabled) return;
    if (!config::Get().vrEnabled) {
        g_disabled = true;
        LOG("[xr] disabled by config ([vr] enabled=0)");
        return;
    }
    __try {
        ID3D11Device* dev = nullptr;
        if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev)))) return;  // not D3D11
        dev->Release();  // the swapchain keeps it alive
        if (!s.session) {
            if (GetTickCount64() < s.nextRetry) return;
            s.nextRetry = GetTickCount64() + 5000;
            if (!s.instance && !CreateInstance()) return;
            if (!s.system && !GetSystem()) return;
            if (!CreateSession(dev)) {
                DestroySession();
                return;
            }
        }
        if (dev != s.device) {
            LOG("[xr] D3D11 device changed; recreating session");
            DestroySession();
            return;
        }
        PollEvents();
        if (s.session && s.running) Frame(sc, dev);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG("[xr] exception 0x%08lX in OnPresent; OpenXR disabled for this run", GetExceptionCode());
        g_disabled = true;
    }
}
void ShutdownForExit() {
    if (!config::Get().vrXrCleanExit || !s.instance) return;
    LOGP("[xr] process exit: closing the OpenXR session and instance");
    ShutdownForExitImpl();
    LOGP("[xr] OpenXR closed");
}
bool HasHpControllerExt() { return g_hpControllerExt; }
bool HasFrameControllerExt() { return g_frameControllerExt; }
}  // namespace ggdpvr::xr
