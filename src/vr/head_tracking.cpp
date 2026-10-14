#include "head_tracking.h"

#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "../core/config.h"
#include "../core/game.h"
#include "../core/hooks.h"
#include "../core/log.h"
#include "../ue3/objects.h"
#include "../core/settings_guard.h"
#include "../ue3/console.h"
#include "../ue3/process_event.h"
#include "../xr/xr_screen.h"
#include "../xr/xr_shared.h"
#include "render_res.h"
#include "ui_layer.h"
#include "vpad.h"
#include "controllers.h"
#include "menu_pointer.h"
#include "vrmath.h"

namespace ggdpvr::vr {
namespace {
using namespace ggdpvr::game;

constexpr uint32_t kCalcSceneView = 0x7F58F0;
constexpr uint32_t kCsvFromVcDraw = 0x7FB13A;      // return address: CalcSceneView call in UGameViewportClient::Draw
constexpr uint32_t kViewportDraw = 0x56E520;       // FViewport::Draw(UBOOL bShouldPresent)
constexpr uint32_t kVpDrawFromRedraw = 0x61D8D3;   // return address: main game viewport in UGameEngine::RedrawViewports
const uint8_t kCsvBytes[] = {0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41};
const uint8_t kVpDrawBytes[] = {0x48, 0x8B, 0xC4, 0x55, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x48};

using CsvFn = void*(__fastcall*)(void*, void*, void*, void*, void*, void*);
using VpDrawFn = void(__fastcall*)(void*, uint32_t);
CsvFn o_Csv = nullptr;
VpDrawFn o_VpDraw = nullptr;
thread_local int t_inCsv = 0;

uint32_t g_offVpLoc = 0, g_offVpRot = 0, g_offFovRet = 0;

// Current view (game thread): the pose composed onto the game camera and the render frustum.
struct CurView {
    bool valid = false;
    XrPosef rel{};   // relative (recentred) pose: composed onto the game camera
    XrPosef raw{};   // raw LOCAL pose: submitted to OpenXR with the frame
    float halfH = 0, halfV = 0, fovDeg = 90;
} g_cur;
uint64_t g_frameId = 0, g_composed = 0;

// Symmetric frustum covering `fov` (both eyes when mono), horizontal derived from the backbuffer aspect
// because UE3 builds the projection from a horizontal FOV + viewport aspect (AspectRatio_MaintainXFOV).
void SetFrustum(const XrFovf* fovs, int n) {
    float h = 0, v = 0;
    for (int i = 0; i < n; ++i) {
        h = std::fmax(h, std::fmax(-fovs[i].angleLeft, fovs[i].angleRight));
        v = std::fmax(v, std::fmax(fovs[i].angleUp, -fovs[i].angleDown));
    }
    const float m = config::Get().vrFovMarginDeg * kPi / 180.f;
    h += m, v += m;
    const float aspect = xr::BackbufferAspect();
    const float hFromV = std::atan(std::tan(v) * aspect);
    g_cur.halfH = std::fmax(h, hFromV);
    g_cur.halfV = std::atan(std::tan(g_cur.halfH) / aspect);
    g_cur.fovDeg = 2.f * g_cur.halfH * 180.f / kPi;
}

// Debug: synthetic head pose (no headset needed). 1 yaw +30deg (look left), 2 pitch +20deg (look up),
// 3 roll +20deg (tilt head left), 4 position 0.3 m to the right, 5 slow yaw sweep, 9 identity (VR FOV only).
std::atomic<int> g_fakeHead{0};
bool FakeSample(xr::HeadSample& hs) {
    const int m = g_fakeHead.load();
    if (!m) return false;
    hs = {};
    hs.raw.orientation = hs.rel.orientation = {0, 0, 0, 1};
    const float d = kPi / 180.f;
    auto axisAngle = [](float x, float y, float z, float a) {
        return XrQuaternionf{x * std::sin(a / 2), y * std::sin(a / 2), z * std::sin(a / 2), std::cos(a / 2)};
    };
    if (m == 1) hs.rel.orientation = axisAngle(0, 1, 0, 30 * d);
    if (m == 2) hs.rel.orientation = axisAngle(1, 0, 0, 20 * d);
    if (m == 3) hs.rel.orientation = axisAngle(0, 0, 1, 20 * d);
    if (m == 4) hs.rel.position = {0.3f, 0, 0};
    if (m == 6) hs.rel.orientation = axisAngle(0, 1, 0, 10 * d);   // 10 deg left (FOV calibration)
    if (m == 7) hs.rel.orientation = axisAngle(1, 0, 0, -35 * d);  // 35 deg down
    if (m == 8) hs.rel.orientation = axisAngle(0, 1, 0, 180 * d);  // turned round
    if (m == 10) hs.rel.orientation = axisAngle(1, 0, 0, 55 * d);  // 55 deg up
    if (m == 11) hs.rel.orientation = axisAngle(0, 1, 0, 150 * d); // 150 deg left
    if (m == 5) hs.rel.orientation = axisAngle(0, 1, 0, 40 * d * std::sin(float(GetTickCount64() % 8000) / 8000.f * 2 * kPi));
    if (m == 14) {  // the diagonal sweep while the head also moves forward / back and sideways (dev)
        const float ts = float(GetTickCount64() % 1000000) / 1000.f;
        hs.rel.position = {0.15f * std::sin(ts * 2 * kPi * 0.41f), 0.f, -0.25f * std::sin(ts * 2 * kPi * 0.3f)};
    }
    if (m == 12 || m == 13 || m == 14) {  // diagonal sweep (test: frame cost while the head moves); 13 = three times faster
        const float sp = m == 13 ? 3.f : 1.f, ts = float(GetTickCount64() % 1000000) / 1000.f * sp;
        const XrQuaternionf qy = axisAngle(0, 1, 0, 40 * d * std::sin(ts * 2 * kPi * 0.25f));
        const XrQuaternionf qp = axisAngle(1, 0, 0, 25 * d * std::sin(ts * 2 * kPi * 0.37f));
        hs.rel.orientation = {qy.w * qp.x + qy.x * qp.w + qy.y * qp.z - qy.z * qp.y, qy.w * qp.y - qy.x * qp.z + qy.y * qp.w + qy.z * qp.x,
                              qy.w * qp.z + qy.x * qp.y - qy.y * qp.x + qy.z * qp.w, qy.w * qp.w - qy.x * qp.x - qy.y * qp.y - qy.z * qp.z};
    }
    hs.raw = hs.rel;  // no recentering when simulated: the raw pose is the same (the UI layer's world-fixed surroundings
                      // follow the raw pose; with an identity raw pose they turned with the simulated head)
    for (int e = 0; e < 2; ++e) {
        hs.eyeRel[e] = hs.rel;
        hs.eyeRaw[e] = hs.raw;
        const float ipd = 0.032f * (e ? 1.f : -1.f);  // eye offset along the head's right axis
        const XrQuaternionf& q = hs.rel.orientation;
        // rotate (ipd,0,0) by q
        const float x = 1 - 2 * (q.y * q.y + q.z * q.z), y = 2 * (q.x * q.y + q.w * q.z), z = 2 * (q.x * q.z - q.w * q.y);
        hs.eyeRel[e].position = {hs.rel.position.x + x * ipd, hs.rel.position.y + y * ipd, hs.rel.position.z + z * ipd};
        hs.eyeRaw[e].position = hs.eyeRel[e].position;
        hs.fov[e] = {-50 * d, 50 * d, 50 * d, -50 * d};
    }
    return true;
}

// --- immersive vs menu classification (current GameInfo class + pause state) ---
uint32_t g_offGamePlayers = 0, g_offPlayerActor = 0, g_offWorldInfo = 0, g_offGame = 0, g_offPauser = 0;
uintptr_t g_lastGameObj = 0, g_lastGameCls = 0, g_lastGameWi = 0;
uint32_t g_gameChanges = 0;  // game mode / level changes so far
std::atomic<bool> g_inFinalDoki{false};  // the final Doki Doki Mode (GG2DokiSp*) is up (game thread)
bool g_lastGameImmersive = false;
std::string g_lastGameClass;
char g_gameClassDiag[64] = "";  // a copy for other threads (freeze report)
bool g_lastSceneImmersive = false;
bool g_lastPaused = false;
std::atomic<bool> g_zoomInterrupt{false};  // paused / continue prompt or results / Mom is coming / game over
void EngineSmoothingTick(bool vrOn);  // below: the engine's frame smoothing off while VR runs

uint32_t g_offPlayerCamera = 0, g_offConstrainAspect = 0, g_maskConstrainAspect = 0;
uintptr_t g_lastPc = 0;

// GG2Camera.UpdateCamera re-enables bConstrainAspectRatio (16:9 letterbox) every tick. VR eye images
// are not 16:9, so clear it on the player's camera between the tick and the eye draws.
void ClearAspectConstraint() {
    if (!g_lastPc || !g_offPlayerCamera || !g_offConstrainAspect) return;
    uintptr_t cam = 0;
    if (!Read(g_lastPc + g_offPlayerCamera, cam) || !cam) return;
    uint32_t bits = 0;
    if (!Read(cam + g_offConstrainAspect, bits)) return;
    if (bits & g_maskConstrainAspect) *reinterpret_cast<uint32_t*>(cam + g_offConstrainAspect) = bits & ~g_maskConstrainAspect;
}

bool SceneIsImmersive() {
    uintptr_t engine = 0, players = 0, lp = 0, pc = 0, wi = 0, game = 0, pauser = 0;
    int32_t num = 0;
    if (!Read(Addr(rva::GEngine), engine) || !engine) return false;
    Read(engine + g_offGamePlayers, players);
    Read(engine + g_offGamePlayers + 8, num);
    if (!players || num < 1 || !Read(players, lp) || !lp || !Read(lp + g_offPlayerActor, pc) || !pc) return false;
    g_lastPc = pc;
    if (!g_offWorldInfo || !Read(pc + g_offWorldInfo, wi) || !wi) return false;
    Read(wi + g_offGame, game);
    Read(wi + g_offPauser, pauser);
    g_lastPaused = pauser != 0;
    // a new GameInfo can be allocated where the old one was (0.9.32: after a perfect Her Majesty Shinobu the stage
    // select kept the minigame mode - no "game mode" change), so its class and WorldInfo are compared too
    const uintptr_t gameCls = game ? ue3::ClassOf(game) : 0;
    if (game != g_lastGameObj || gameCls != g_lastGameCls || wi != g_lastGameWi) {
        g_lastGameObj = game, g_lastGameCls = gameCls, g_lastGameWi = wi;
        ++g_gameChanges;
        g_lastGameClass = game ? ue3::ObjName(ue3::ClassOf(game)) : "";
        g_lastGameImmersive = false;
        for (auto& k : config::Get().vrImmersiveGames)
            if (!k.empty() && g_lastGameClass.find(k) != std::string::npos) g_lastGameImmersive = true;
        LOGP("[vr] game mode %s -> %s", g_lastGameClass.c_str(), g_lastGameImmersive ? "IMMERSIVE" : "virtual screen");
        strncpy_s(g_gameClassDiag, g_lastGameClass.c_str(), _TRUNCATE);
        // the layered menus probe clip names that may not exist; each miss was a DevGFxUI line in Launch.log (108k
        // lines, ~1500/s in menus - 0.9.15 log): that log category is muted for the session
        static bool s_suppressed = false;
        if (!s_suppressed) s_suppressed = true, ue3::console::Enqueue("suppress DevGFxUI");
        ui::ResetContinuePrompt();
    }
    const bool imm = g_lastGameImmersive && !(pauser && config::Get().vrPauseOnScreen);
    if (imm != g_lastSceneImmersive) {
        g_lastSceneImmersive = imm;
        LOG("[vr] scene -> %s%s", imm ? "immersive" : "virtual screen", pauser ? " (paused)" : "");
    }
    return imm;
}

// Per-eye render size: the symmetric frustum covering the eye at the headset's recommended pixel density
// (pixels per tangent unit) times [vr] render_scale.
void UpdateVrRenderSize(const xr::HeadSample& hs) {
    uint32_t rw = 0, rh = 0;
    if (!xr::RecommendedEyeSize(rw, rh)) return;
    const XrFovf& f = hs.fov[0];
    const float ppt = float(rw) / (std::tan(-f.angleLeft) + std::tan(f.angleRight));
    const float m = config::Get().vrFovMarginDeg * kPi / 180.f;
    const float h = std::fmax(-f.angleLeft, f.angleRight) + m, v = std::fmax(f.angleUp, -f.angleDown) + m;
    const float k = config::Get().vrRenderScale;
    const uint32_t w = (uint32_t(2.f * std::tan(h) * ppt * k) + 1) & ~1u;
    const uint32_t hh = (uint32_t(2.f * std::tan(v) * ppt * k) + 1) & ~1u;
    SetVrRenderSize(w, hh);
}

bool g_loggedEyes = false;
void LogEyesOnce(const xr::HeadSample& hs) {
    if (g_loggedEyes) return;
    g_loggedEyes = true;
    const auto& a = hs.eyeRel[0].position;
    const auto& b = hs.eyeRel[1].position;
    const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
    const float ipd = std::sqrt(dx * dx + dy * dy + dz * dz);
    // eye-to-eye vector in head-local axes (should be ~ +X = right in OpenXR)
    const auto& o = hs.rel.orientation;
    const XrQuaternionf inv{-o.x, -o.y, -o.z, o.w};
    const Q4 qi{inv.x, inv.y, inv.z, inv.w};
    const V3 local = Rotate(qi, {dx, dy, dz});
    LOG("[vr] eye check: IPD %.1f mm -> %.2f uu at world_scale %.1f; left->right in head axes (%.3f, %.3f, %.3f) m; "
        "fov L(%.1f,%.1f,%.1f,%.1f) R(%.1f,%.1f,%.1f,%.1f) deg",
        ipd * 1000.f, ipd * config::Get().vrWorldScale, config::Get().vrWorldScale, local.x, local.y, local.z,
        hs.fov[0].angleLeft * 57.3f, hs.fov[0].angleRight * 57.3f, hs.fov[0].angleUp * 57.3f, hs.fov[0].angleDown * 57.3f,
        hs.fov[1].angleLeft * 57.3f, hs.fov[1].angleRight * 57.3f, hs.fov[1].angleUp * 57.3f, hs.fov[1].angleDown * 57.3f);
}

bool Sample(xr::HeadSample& hs) {
    if (g_fakeHead.load()) return FakeSample(hs);
    return xr::Running() && xr::SampleHead(hs);
}

// Live stereo diagnostics: 0 normal, 1 swapped eyes, 2 mono (both eyes rendered from the left eye pose)
std::atomic<int> g_stereoVariant{0};
std::atomic<float> g_ipdScale{1.f};

XrPosef ScaledEye(const XrPosef& eye, const XrPosef& head) {  // scale the eye's offset from the head centre
    const float k = g_ipdScale.load();
    XrPosef r = eye;
    r.position = {head.position.x + (eye.position.x - head.position.x) * k,
                  head.position.y + (eye.position.y - head.position.y) * k,
                  head.position.z + (eye.position.z - head.position.z) * k};
    return r;
}

// FViewport::Draw: in stereo mode the main game viewport is drawn (and presented) once per eye.
std::atomic<float> g_gameFovDeg{0};  // the game's own FOV (zoom) before the VR override
std::atomic<float> g_zoom{1.f};
std::atomic<uint64_t> g_drawCount{0};
// Dressing Room backdrop (UI layer): on/off and the world's up in the recentered frame (game thread -> render thread)
std::mutex g_voidMu;
DressingRoom g_room;  // shared with the UI layer (render thread), under g_voidMu
// The vanilla backdrop: one mesh (wall + floor slab) in front of the camera. It is hidden and the room replaces it;
// its bounds give the room's size (the wall's distance from the camera, its height, the floor's height).
float NearestGirlAhead(const V3& loc, const Rot& rot, uintptr_t* girlOut);
struct RoomSrc {
    uintptr_t actor = 0;
    float dolly = 0;          // uu the viewpoint moves forward (toward the girl), along the horizontal view
    V3 camStart{};            // the game camera when the room was set up
    V3 centreW{};             // room centre on the floor, world
    float radius = 0, wallH = 0;
    ULONGLONG nextTry = 0;
} g_roomSrc;
bool g_roomActive = false;  // game thread: the Dressing Room room is up (viewpoint moved)
// Dressing Room: its camera takes up the view the mod composed (head pose included) and reports it back on later frames
// (test: a 30 deg head turn turned the view ~60 deg and kept drifting; a 0.3 m step moved the camera 0.3 m). The camera
// never moves by itself there, so the pose it reports on the scene's first frame is pinned for the whole visit.
bool g_lrScene = false;
// Academy Store (GG2BaseGame with the shop screen): the game's camera looks at the booth from the side; the viewpoint
// is pinned straight in front of the window at the same distance, and the corridor around it is drawn (UI layer).
struct ShopSrc {
    bool found = false;
    ULONGLONG nextTry = 0;
    V3 camLoc{}, centreW{};
    Rot camRot{};
    float back = 0, ceil = 0, halfWidth = 0, base = 0;
} g_shopSrc;
bool g_shopActive = false;
// Aoi stands turned toward the game's original camera spot (her pose doesn't follow the camera): while the store is
// shown with the moved viewpoint, she is turned by the difference, and turned back afterwards
struct { uintptr_t girl = 0; Rot orig{}; int32_t delta = 0; Rot shopRot{}; bool shopRotSaved = false; } g_aoi;
void SetActorRotation(uintptr_t actor, const Rot& r) {
    static const uintptr_t fn = ue3::FindObject("Engine.Actor.SetRotation", "Function");
    static uint32_t offRot = 0;
    if (!fn) return;
    if (!offRot)
        for (auto& q : ue3::Properties(fn))
            if (q.name == "NewRotation") offRot = q.offset;
    uint8_t parms[64]{};
    memcpy(parms + offRot, &r, sizeof r);
    uintptr_t vt = 0, pe = 0;
    Read(actor, vt), Read(vt + game::rva::ProcessEvent_VSlot, pe);
    using PEFn = void(__fastcall*)(uintptr_t, uintptr_t, void*, void*);
    reinterpret_cast<PEFn>(pe)(actor, fn, parms, nullptr);
}
void UpdateAoi(bool shop, const V3& oldCam, const V3& newCam) {
    static uint32_t offRotation = 0, offLocation = 0;
    if (!offRotation) {
        ue3::PropInfo pi;
        if (uintptr_t c = ue3::FindObject("Engine.Actor", "Class"); c && ue3::FindProperty(c, "Rotation", pi)) offRotation = pi.offset;
        if (uintptr_t c = ue3::FindObject("Engine.Actor", "Class"); c && ue3::FindProperty(c, "Location", pi)) offLocation = pi.offset;
        if (!offRotation || !offLocation) return;
    }
    if (!shop) {
        if (g_aoi.girl && ue3::IsObject(g_aoi.girl)) {
            SetActorRotation(g_aoi.girl, g_aoi.orig);
            static uint32_t offR = 0;
            if (!offR)
                if (uintptr_t c = ue3::FindObject("GG2Game.GG2ShopGirl", "Class")) {
                    ue3::PropInfo pi;
                    if (ue3::FindProperty(c, "Rot", pi)) offR = pi.offset;
                }
            if (offR && g_aoi.shopRotSaved) *reinterpret_cast<Rot*>(g_aoi.girl + offR) = g_aoi.shopRot;
        }
        g_aoi.girl = 0, g_aoi.shopRotSaved = false;
        return;
    }
    if (!g_aoi.girl || !ue3::IsObject(g_aoi.girl)) {
        g_aoi.girl = 0;
        for (int32_t i = 0, n = ue3::NumObjects(); i < n && !g_aoi.girl; ++i) {
            const uintptr_t o = ue3::ObjectAt(i);
            if (!o || !ue3::IsA(o, "GG2Girl") || ue3::PathName(o).find("Default__") != std::string::npos) continue;
            V3 l;
            Read(o + offLocation, l);
            if (std::fabs(l.x - g_shopSrc.centreW.x) > 260.f || l.y > g_shopSrc.centreW.y + 40.f || l.y < g_shopSrc.centreW.y - 400.f) continue;
            g_aoi.girl = o;
            Read(o + offRotation, g_aoi.orig);
            const float a0 = std::atan2(oldCam.y - l.y, oldCam.x - l.x), a1 = std::atan2(newCam.y - l.y, newCam.x - l.x);
            g_aoi.delta = RadToRot(a1 - a0);
            LOG("[vr] academy store: %s turned %.1f deg toward the moved viewpoint", ue3::ObjName(o).c_str(), (a1 - a0) * 57.2958f);
        }
        if (!g_aoi.girl) return;
    }
    Rot cur{};
    Read(g_aoi.girl + offRotation, cur);
    const Rot want{g_aoi.orig.pitch, g_aoi.orig.yaw + g_aoi.delta, g_aoi.orig.roll};
    if (cur.yaw != want.yaw) SetActorRotation(g_aoi.girl, want);
    // her script keeps her own GG2ShopGirl.Rot (InitPosRot / Update): turned with her (setting the actor alone showed
    // no change in testing)
    static uint32_t offShopRot = 0;
    if (!offShopRot)
        if (uintptr_t c = ue3::FindObject("GG2Game.GG2ShopGirl", "Class")) {
            ue3::PropInfo pi;
            if (ue3::FindProperty(c, "Rot", pi)) offShopRot = pi.offset;
        }
    if (offShopRot) {
        Rot sr{};
        Read(g_aoi.girl + offShopRot, sr);
        if (!g_aoi.shopRotSaved) g_aoi.shopRot = sr, g_aoi.shopRotSaved = true;
        const Rot sw{g_aoi.shopRot.pitch, g_aoi.shopRot.yaw + g_aoi.delta, g_aoi.shopRot.roll};
        if (sr.yaw != sw.yaw) *reinterpret_cast<Rot*>(g_aoi.girl + offShopRot) = sw;
        static int s_logs = 0;
        if (s_logs++ < 3) LOG("[vr] academy store: Aoi actor yaw %d (want %d), her Rot yaw %d -> %d", cur.yaw, want.yaw, sr.yaw, sw.yaw);
    }
}
bool FindShopMesh() {
    static ue3::PropInfo pb, pm;
    static bool s_props = false;
    if (!s_props) {
        const uintptr_t cPrim = ue3::FindObject("Engine.PrimitiveComponent", "Class"), cSm = ue3::FindObject("Engine.StaticMeshComponent", "Class");
        s_props = cPrim && cSm && ue3::FindProperty(cPrim, "Bounds", pb) && ue3::FindProperty(cSm, "StaticMesh", pm);
        if (!s_props) return false;
    }
    // the booth (Koubai_Base) and its shutter (koubai_Shutter, in the window): the front wall is the shutter's
    // front plane - the booth's bounds reach ~45 uu further out (counter, roof), which shifted the surround sideways
    // when seen from the game's off-axis camera
    float base[6]{}, shut[6]{};
    bool haveBase = false, haveShut = false;
    for (int32_t i = 0, n = ue3::NumObjects(); i < n; ++i) {
        const uintptr_t o = ue3::ObjectAt(i);
        if (!o || !ue3::IsA(o, "StaticMeshComponent")) continue;
        uintptr_t mesh = 0;
        Read(o + pm.offset, mesh);
        if (!mesh || !ue3::IsObject(mesh)) continue;
        const std::string mn = ue3::ObjName(mesh);
        if (mn == "Koubai_Base" && !haveBase) Read(o + pb.offset, base), haveBase = true;
        if (mn == "koubai_Shutter" && !haveShut) Read(o + pb.offset, shut), haveShut = true;
    }
    if (!haveBase) return false;
    const float* b = base;
    // floor: 1.6 m below the viewpoint, like the title's. It used to be drawn 35 uu above the booth's bounds' bottom,
    // only 90 uu (~1.06 m) below the viewpoint, over the booth's own baseboard: the player felt very short
    // (0.9.13). The booth's mesh reaches down to its bounds' bottom (its teal baseboard; Aoi's feet are 13 uu higher);
    // the composite carries its wall on down to the floor (StoreWallDown).
    const float frontY = haveShut ? shut[1] + shut[4] : b[1] + b[4] - 43.f, bottom = b[2] - b[5], baseZ = bottom;
    g_shopSrc.camLoc = {b[0], frontY + 178.f, bottom + 125.f};
    const float floorZ = std::fmin(baseZ, g_shopSrc.camLoc.z - 1.6f * config::Get().vrWorldScale);
    g_shopSrc.centreW = {b[0], frontY, floorZ};
    g_shopSrc.base = baseZ - floorZ;
    g_shopSrc.camRot = {0, -16384, 0};
    g_shopSrc.back = 178.f + 200.f;
    g_shopSrc.halfWidth = b[3];  // the front wall spans the booth's width (window opening about +-110)
    g_shopSrc.ceil = b[2] + b[5] + 30.f - floorZ;
    g_shopSrc.found = true;
    LOG("[vr] academy store: booth front plane y %.0f (shutter %s), half-width %.0f, floor z %.0f (booth base %.0f uu above it)", frontY,
        haveShut ? "found" : "missing", b[3], floorZ, g_shopSrc.base);
    return true;
    return false;
}
struct { bool set = false; V3 loc{}; Rot rot{}; } g_lrPin;
V3 DressingDollyOffset(const Rot& rot) {
    if (!g_roomActive || g_roomSrc.dolly <= 0) return {};
    const Axes ax = AxesOf(rot);
    V3 f{ax.x.x, ax.x.y, 0};
    const float fl = std::sqrt(f.x * f.x + f.y * f.y);
    return fl > 1e-3f ? f * (g_roomSrc.dolly / fl) : V3{};
}
bool FindDressingRoomMesh(const V3& cam, const Rot& camRot) {
    static ue3::PropInfo pb, pm, pown;
    static bool s_props = false;
    if (!s_props) {
        const uintptr_t cPrim = ue3::FindObject("Engine.PrimitiveComponent", "Class"), cSm = ue3::FindObject("Engine.StaticMeshComponent", "Class"),
                        cAc = ue3::FindObject("Engine.ActorComponent", "Class");
        s_props = cPrim && cSm && cAc && ue3::FindProperty(cPrim, "Bounds", pb) && ue3::FindProperty(cSm, "StaticMesh", pm) &&
                  ue3::FindProperty(cAc, "Owner", pown);
        if (!s_props) return false;
    }
    for (int32_t i = 0, n = ue3::NumObjects(); i < n; ++i) {
        const uintptr_t o = ue3::ObjectAt(i);
        if (!o || !ue3::IsA(o, "StaticMeshComponent")) continue;
        uintptr_t mesh = 0, owner = 0;
        Read(o + pm.offset, mesh), Read(o + pown.offset, owner);
        if (!mesh || !owner || !ue3::IsObject(mesh) || !ue3::IsObject(owner) || ue3::ObjName(mesh) != "CRoom_Wallpaper") continue;
        float b[6]{};
        Read(o + pb.offset, b);  // FBoxSphereBounds: Origin, BoxExtent
        const V3 lo{b[0] - b[3], b[1] - b[4], b[2] - b[5]};
        // horizontal camera forward; the wall is the bounds' far side along it
        const Axes ax = AxesOf(camRot);
        V3 f{ax.x.x, ax.x.y, 0};
        const float fl = std::sqrt(f.x * f.x + f.y * f.y);
        if (fl < 1e-3f) return false;
        f = f * (1.f / fl);
        const float wallDist = (b[0] - cam.x) * f.x + (b[1] - cam.y) * f.y + std::fabs(b[3] * f.x) + std::fabs(b[4] * f.y);
        const float floorZ = lo.z + 5.f;  // the slab's top (its bounds reach 5 uu below the surface the props stand on)
        // closer: the vanilla camera stands ~14 m from the girl; move to dressing_distance metres from her. The room is
        // centred on the new viewpoint and keeps the vanilla wall where it was.
        const float girl = NearestGirlAhead(cam, camRot, nullptr), want = config::Get().vrDressingDistance * config::Get().vrWorldScale;
        const float dolly = girl > 0 && want > 0 ? std::fmax(0.f, std::fmin(girl - want, wallDist - 200.f)) : 0.f;
        g_roomSrc.actor = owner;
        g_roomSrc.dolly = dolly;
        g_roomSrc.camStart = cam;
        g_roomSrc.centreW = {cam.x + f.x * dolly, cam.y + f.y * dolly, floorZ};
        g_roomSrc.radius = wallDist - dolly;
        g_roomSrc.wallH = b[2] + b[5] - floorZ;
        static const uintptr_t fnHide = ue3::FindObject("Engine.Actor.SetHidden", "Function");
        if (fnHide) {
            uint8_t parms[16]{};
            *reinterpret_cast<uint32_t*>(parms) = 1;
            uintptr_t vt = 0, pe = 0;
            Read(owner, vt), Read(vt + game::rva::ProcessEvent_VSlot, pe);
            using PEFn = void(__fastcall*)(uintptr_t, uintptr_t, void*, void*);
            reinterpret_cast<PEFn>(pe)(owner, fnHide, parms, nullptr);
        }
        LOG("[vr] dressing room: backdrop %s hidden=%d; girl %.0f uu ahead, viewpoint %.0f uu forward; room radius %.0f uu, wall %.0f uu high, floor z %.0f",
            ue3::ObjName(owner).c_str(), fnHide != 0, girl, dolly, g_roomSrc.radius, g_roomSrc.wallH, floorZ);
        return true;
    }
    return false;
}
// Stereo draws the frame twice, and the HUD's script runs in both draws: menus that poll input from the HUD
// (pause menu, in-stage windows) saw each key press twice per frame and stepped two (or three) items per press.
// During the second eye's draw the native key/mouse edge queries (IsKeyTrigger/Repeat/Release, IsMouseTrigger/
// Release) answer "no"; held-state queries (IsKeyDigital) are untouched.
// the last stereo frame's eyes (game thread): each eye's horizontal FOV tangents and the real eye separation (m)
float g_eyeHalfTan = 0.f, g_eyeSepM = 0.f;
bool g_secondEyeDraw = false;  // game thread
bool g_firstEyeDraw = false;   // game thread: inside the left eye's draw
bool g_aeHudDraw = false;      // game thread: inside the minigame HUD's PostRender (runs in the right eye's draw only)
using ExecFn = void(__fastcall*)(void*, void*, void*);
struct EdgeQuery { const char* name; uint32_t rva; ExecFn orig; };
EdgeQuery g_edge[] = {{"IsKeyTrigger", 0x16D0DD0, nullptr}, {"IsKeyRepeat", 0x16D0F50, nullptr},
                      {"IsKeyRelease", 0x16D0E90, nullptr}, {"IsMouseTrigger", 0x16D1820, nullptr},
                      {"IsMouseRelease", 0x16D18E0, nullptr}};
std::atomic<int> g_edgeSuppressed{0};
template <int I>
void __fastcall hk_Edge(void* self, void* stack, void* result) {
    g_edge[I].orig(self, stack, result);
    // (not in the minigame HUD's draw: it runs in the right eye only, so its menus - pause - would never see a press)
    if (g_secondEyeDraw && !g_aeHudDraw && result && *static_cast<uint32_t*>(result)) {
        *static_cast<uint32_t*>(result) = 0;
        if (g_edgeSuppressed.fetch_add(1) < 10) LOG("[vr] %s in the second eye's draw ignored", g_edge[I].name);
    }
}
// Zoom scope render (zoom_scope_render): while the right-click zoom is on with controller aim, a third draw per
// frame renders the magnified view itself, from the hand along the aim ray with the eye's frustum narrowed by the
// zoom factor, so the scope circle shows real detail (the eye image stretched ~3x was blurry). It is drawn before
// the eyes, so the state their draws leave behind (cached view, HUD script results) is as before, and the HUD
// script (PostRender events) does not run in it. The UI layer keeps its image (FrameRecord eye 2).
std::atomic<bool> g_scopeDraw{false};
DWORD g_scopeThread = 0;
float g_scopeFovDeg = 0;
double GameTick();  // below: changes once per game tick
void ScaleAeIconOffsets();  // below: hidden minigame icons' wander, scaled to the game's FOV
bool SkipInScopeDraw(uintptr_t self, uintptr_t fn) {
    static std::unordered_map<uintptr_t, bool> s_isPostRender;  // UFunction -> named PostRender (game thread)
    auto it = s_isPostRender.find(fn);
    if (it == s_isPostRender.end()) it = s_isPostRender.emplace(fn, ue3::ObjName(fn) == "PostRender").first;
    if (!it->second) return false;
    // Minigames run their touch detection and hit checks in the HUD's draw (GG2AEHud_DrawHud_CheckHit_*), which in
    // stereo ran once per eye: each pass clears the hit flag the request actor reads once per tick, so one-frame
    // successes (poke, flick) were wiped by the second pass, and every minigame timer ran at double speed. Their HUD
    // draws once per game tick (its UI is Scaleform; the canvas pass draws nothing per eye). (0.9.21)
    // Runs in the right eye's draw only: the left eye's draw calls it twice per tick (and those passes don't position
    // the icons), the right eye's once.
    if (config::Get().vrAeMinigameFix && self && ue3::IsA(self, "GG2ActionEventBaseHud")) {
        if (g_firstEyeDraw) return true;
        ScaleAeIconOffsets();
        static bool s_post = ue3::pe::RegisterPost(fn, [](uintptr_t, void*) { g_aeHudDraw = false; });
        g_aeHudDraw = s_post;
        return false;
    }
    if (!g_scopeDraw.load(std::memory_order_relaxed) || GetCurrentThreadId() != g_scopeThread) return false;
    static bool s_said = false;
    if (!s_said) s_said = true, LOG("[vr] zoom scope render: %s skipped in its draw", ue3::PathName(fn).c_str());
    return true;
}
// head orientation turned (shortest arc) so it looks at the point `dist` metres along the aim ray (1e6: along the ray)
XrQuaternionf LookAtAimPoint(const XrPosef& head, const XrPosef& aim, float dist) {
    const Q4 hq{head.orientation.x, head.orientation.y, head.orientation.z, head.orientation.w};
    const Q4 aq{aim.orientation.x, aim.orientation.y, aim.orientation.z, aim.orientation.w};
    const V3 hf = Rotate(hq, {0, 0, -1}), af = Rotate(aq, {0, 0, -1});
    V3 t = V3{aim.position.x, aim.position.y, aim.position.z} + af * dist - V3{head.position.x, head.position.y, head.position.z};
    const float len = std::sqrt(Dot(t, t));
    if (len < 1e-4f) return head.orientation;
    t = t * (1.f / len);
    Q4 d{hf.y * t.z - hf.z * t.y, hf.z * t.x - hf.x * t.z, hf.x * t.y - hf.y * t.x, 1.f + Dot(hf, t)};
    const float n = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z + d.w * d.w);
    if (n < 1e-4f) return head.orientation;
    d = {d.x / n, d.y / n, d.z / n, d.w / n};
    const Q4 r = Mul(d, hq);
    return {r.x, r.y, r.z, r.w};
}
constexpr uint8_t kEdgeBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x42, 0x24};
float g_baseFovDeg = 0;
// Game thread, once per frame: zoom = tan(base/2) / tan(current/2) for the game's right-click zoom.
// The base is the FOV while the player is not zooming (m_nsm.isZoom clear); during the zoom the FOV ramps down to
// GG2PlayerController.CZoomFovValue-ish values. Cutscene and Fantasize FOV changes never set isZoom.
constexpr uint32_t kOffZoomFov = 0x7EC;  // GG2PlayerController.CZoomFovValue
void UpdateZoom() {
    float z = 1.f, zoomFov = 0.f;
    const float fov = g_gameFovDeg.load();
    if (config::Get().vrZoomInPanel && g_lastPc && ue3::IsObject(g_lastPc) && ue3::IsA(g_lastPc, "GG2PlayerController") &&
        fov > 1.f) {
        Read(g_lastPc + kOffZoomFov, zoomFov);
        uint32_t nsmFlags = 0;
        const bool playerZoom = ue3::IsA(g_lastPc, "GG2StandardPlayerController") && Read(g_lastPc + 0xB98, nsmFlags) &&
                                (nsmFlags & 0x8);
        if (!playerZoom || g_baseFovDeg <= 1.f) g_baseFovDeg = fov;
        else if (fov < g_baseFovDeg - 0.05f) z = std::tan(g_baseFovDeg * kPi / 360.f) / std::tan(fov * kPi / 360.f);
    }
    z = std::fmin(std::fmax(z, 1.f), 8.f);
    const float prev = g_zoom.exchange(z);
    static int s_logs = 0;
    if ((prev <= 1.001f) != (z <= 1.001f) && s_logs < 20) {
        ++s_logs;
        LOG("[vr] zoom %s (game FOV %.1f, base %.1f, zoom limit %.1f)", z > 1.001f ? "on" : "off", fov, g_baseFovDeg, zoomFov);
    }
    static int s_peak = 0;
    if (z > 2.5f && s_peak < 1) ++s_peak, LOG("[vr] zoom factor %.2f (FOV %.1f of %.1f)", z, fov, g_baseFovDeg);
}
// Stage 7: the game's 2D sight position (m_nsm.cursorScreenPosition, HUD-band pixels without the band offset)
// follows the controller: the point where the aim ray crosses the panel plane. Not clamped to the panel; the
// game's own sight movie is hidden while controller aim is active (the reticle replaces it).
uint32_t g_lastBandW = 0, g_lastBandH = 0;
float g_lastViewW = 0, g_lastViewH = 0;
// Cursor coordinate space while controller aim is active. 1 = eye view (eye-image pixel of the aim point minus the
// HUD band's top offset): the game's native screen-space tests (sight-circle reveal of mini demons / Mr. Happiness,
// targeting) compare the cursor with eye-view projections in that space (grid scan). 0 = panel.
std::atomic<int> g_cursorSpace{1};
std::atomic<float> g_aimEyeX{0}, g_aimEyeY{0};
}  // namespace
void SetCursorSpace(int s) { g_cursorSpace = s; LOG("[vr] cursor space -> %s", s ? "eye view" : "panel"); }
namespace {
std::atomic<bool> g_aimValid{false};
XrPosef g_aimHead{};  // head pose (rel) of the last stereo frame
std::mutex g_headRawMu;
XrVector3f g_headRawPos{};  // head position (raw LOCAL) of the last stereo frame, for the UI layer's reticle
// The aim ray in the recentered frame (metres, XR axes). aim_origin=head (default): from the head through the point
// reticle_distance along the controller ray - shot, game cursor, reticle and zoom scope then lie on one line from
// the eyes, so the game's screen-space tests (B/W/H zoom measurements, reveal) agree at every distance. With the
// ray from the hand, they matched only where the hand and eye lines cross (0.8.6: zoom on target close up,
// left of it at a distance; circle and cursor jumping when the hit depth changed).
void AimRayRel(const ctrl::Aim& aim, V3& org, V3& dir) {
    const auto& o = aim.rel.orientation;
    const V3 f = Rotate(Q4{o.x, o.y, o.z, o.w}, {0, 0, -1});
    const V3 hp{aim.rel.position.x, aim.rel.position.y, aim.rel.position.z};
    if (!config::Get().vrAimFromHead) {
        org = hp, dir = f;
        return;
    }
    const V3 h{g_aimHead.position.x, g_aimHead.position.y, g_aimHead.position.z};
    const V3 v = hp + f * config::Get().vrReticleDistance - h;
    const float len = std::sqrt(Dot(v, v));
    org = h, dir = len > 1e-4f ? v * (1.f / len) : f;
}
Rot g_aimGameRot{};   // game camera rotation before head composition
V3 g_aimGameLoc{};    // game camera location before head composition
uint32_t g_offHeadRet = 0;  // GG2Girl.GetHeadLocation return offset
// Dev: aim the simulated controller at the nearest live girl (end-to-end hit test of the controller aim).
std::atomic<bool> g_fakeAimGirl{false};
char g_fakeAimClass[64] = "GG2ZakoGirl";  // dev: the class aimed at (!fakectrl girl | !fakectrl at <class>)
}  // namespace
std::atomic<float> g_fakeGirlYawOff{0};
void SetFakeAimAtGirl(bool on, float yawOffset) {
    strcpy_s(g_fakeAimClass, "GG2ZakoGirl");
    g_fakeAimGirl = on, g_fakeGirlYawOff = yawOffset;
    ctrl::SetFake(on, 0, 0);
}
void SetFakeAimAtClass(const char* cls) {  // dev: the nearest live actor of that class (e.g. boss shells)
    strncpy_s(g_fakeAimClass, cls, _TRUNCATE);
    g_fakeAimGirl = true, g_fakeGirlYawOff = 0;
    ctrl::SetFake(true, 0, 0);
    LOG("[vr] dev: simulated controller aims at the nearest %s", g_fakeAimClass);
}
namespace {
bool FirstBossMarker3D(V3& out);  // dev: the first visible heroine-boss marker (below)
float g_aePinOfs[2]{};            // dev: offset of the pinned pointer from the minigame icon
int g_aePinMode = 0;
ULONGLONG g_aePinT0 = 0;          // when the mode was set              // dev (!aepin mode amp hz): 0 still, 1 up-down, 2 circle, 3 left-right around the icon
float g_aePinAmp = 0.f, g_aePinHz = 1.f;
void UpdateFakeAimAtGirl() {
    if (!g_fakeAimGirl) return;
    if (!strcmp(g_fakeAimClass, "aeicon") || !strcmp(g_fakeAimClass, "aereq") || !strcmp(g_fakeAimClass, "aecenter") ||
        !strcmp(g_fakeAimClass, "aeact") || !strcmp(g_fakeAimClass, "dkmarker")) {
        // dev: the last minigame icon on the panel (aecenter: the panel centre, for the screen-space gestures)
        float sx = 960.f, sy = 540.f, fh[3];
        const int kind = g_fakeAimClass[0] == 'd' ? 3 : g_fakeAimClass[2] == 'r' ? 1 : g_fakeAimClass[2] == 'a' ? 2 : 0;
        if (g_fakeAimClass[2] != 'c' && !ui::LastAeIcon(kind, sx, sy)) return;
        {  // the pointer pinned exactly there (whatever the simulated ray does), optionally moving for gestures
            const float ph = float(GetTickCount64() % 100000) * 0.001f * g_aePinHz * 2.f * kPi;
            g_aePinOfs[0] = g_aePinMode == 2 ? g_aePinAmp * std::cos(ph) : g_aePinMode == 3 ? g_aePinAmp * std::sin(ph) : 0.f;
            if (g_aePinMode == 5) {  // repeated flicks: every 0.7 s a 250 ms press swiping amp over 0.15 s
                const float t = float(GetTickCount64() - g_aePinT0) * 0.001f;
                static int s_cycle = -1;
                const int c = int(t / 0.7f);
                if (c != s_cycle) s_cycle = c, ctrl::FakeGrip(250);
                const float u = t - float(c) * 0.7f;
                g_aePinOfs[0] = u < 0.35f ? g_aePinAmp * std::fmax(0.f, std::fmin(1.f, (u - 0.03f) / 0.15f)) : 0.f;
            }
            if (g_aePinMode == 4)  // a swipe: still for 0.05 s, then amp to the right over 0.15 s
                g_aePinOfs[0] = g_aePinAmp * std::fmax(0.f, std::fmin(1.f, (float(GetTickCount64() - g_aePinT0) * 0.001f - 0.05f) / 0.15f));
            g_aePinOfs[1] = g_aePinMode == 1 || g_aePinMode == 2 ? g_aePinAmp * std::sin(ph) : 0.f;
            pointer::DevPin(sx + g_aePinOfs[0], sy + g_aePinOfs[1]);
        }
        ctrl::FakeHand(fh);
        const float Wp = ui::PanelWidth(), Hp = Wp * 9.f / 16.f, D = config::Get().vrUiDistance;
        const float dx = (sx / 1920.f - 0.5f) * Wp - fh[0], dy = (0.5f - sy / 1080.f) * Hp + ui::PanelYOffset() - fh[1], dz = -D - fh[2];
        ctrl::SetFakeQuiet(std::atan2(-dx, -dz) * 180.f / kPi, std::atan2(dy, std::sqrt(dx * dx + dz * dz)) * 180.f / kPi);
        return;
    }
    const bool markerMode = !strcmp(g_fakeAimClass, "bossmarker");
    V3 m{};
    if (markerMode && FirstBossMarker3D(m)) {  // dev: the first visible boss marker (needs !bossmarkers running), else the boss
        // the direction from the head (as a player lines the reticle up with the marker), not from the simulated hand: the
        // cursor is the head's view of the point reticle_distance along the hand ray, so a hand ray through a near marker
        // lands off it by the hand-to-head offset
        const V3 hand = g_aimGameLoc;
        const V3 d{m.x - hand.x, m.y - hand.y, m.z - hand.z};
        const Axes ax = AxesOf(g_aimGameRot);
        const float f = d.x * ax.x.x + d.y * ax.x.y + d.z * ax.x.z, r = d.x * ax.y.x + d.y * ax.y.y + d.z * ax.y.z,
                    u = d.x * ax.z.x + d.y * ax.z.y + d.z * ax.z.z;
        ctrl::SetFakeQuiet(std::atan2(-r, f) * 180.f / kPi, std::atan2(u, std::sqrt(f * f + r * r)) * 180.f / kPi);
        return;
    }
    const char* cls = markerMode ? "GG2BsHeroineGirlGround" : g_fakeAimClass;
    float fh[3];
    ctrl::FakeHand(fh);  // aim from the simulated hand
    const V3 hand = g_aimGameLoc + ToWorld(AxesOf(g_aimGameRot), XrToUe({fh[0], fh[1], fh[2]}) * config::Get().vrWorldScale);
    float best = 1e30f;
    V3 target{};
    for (int32_t i = 0, n = ue3::NumObjects(); i < n; ++i) {
        const uintptr_t o = ue3::ObjectAt(i);
        if (!o || !ue3::IsA(o, cls) || ue3::PathName(o).find("Default__") != std::string::npos) continue;
        V3 l;
        Read(o + 0x80, l);
        const V3 d{l.x - hand.x, l.y - hand.y, l.z - hand.z};
        const float dist2 = d.x * d.x + d.y * d.y + d.z * d.z;
        const Axes ax = AxesOf(g_aimGameRot);
        const float fwd = d.x * ax.x.x + d.y * ax.x.y + d.z * ax.x.z;
        // the girl nearest the view centre (pooled girls sit far below/behind), within ~60 m
        const float cosToCentre = fwd / std::sqrt(std::fmax(dist2, 1.f));
        const float score = 1.f - cosToCentre;
        if (fwd > 50.f && dist2 < 2500.f * 2500.f && score < best) best = score, target = l;
    }
    if (best > 1e29f) return;
    const bool girlMode = !strcmp(g_fakeAimClass, "GG2ZakoGirl");
    // aim between the pivot and the head (GG2Girl.GetHeadLocation) - the torso
    static const uintptr_t fnHead = ue3::FindObject("GG2Game.GG2Girl.GetHeadLocation", "Function");
    uintptr_t girl = 0;
    for (int32_t i = 0, n = ue3::NumObjects(); i < n && !girl; ++i) {
        const uintptr_t o = ue3::ObjectAt(i);
        if (!o || !ue3::IsA(o, "GG2ZakoGirl")) continue;
        V3 l;
        Read(o + 0x80, l);
        if (l.x == target.x && l.y == target.y && l.z == target.z) girl = o;
    }
    if (!girlMode) {
        // other actors: their own location
    } else if (fnHead && girl && g_offHeadRet) {
        uint8_t parms[64]{};
        uintptr_t vt = 0, pe = 0;
        Read(girl, vt);
        Read(vt + game::rva::ProcessEvent_VSlot, pe);
        using PEFn = void(__fastcall*)(uintptr_t, uintptr_t, void*, void*);
        reinterpret_cast<PEFn>(pe)(girl, fnHead, parms, nullptr);
        V3 head;
        memcpy(&head, parms + g_offHeadRet, sizeof head);
        if (head.z > target.z) target = {(target.x + head.x) / 2, (target.y + head.y) / 2, target.z + (head.z - target.z) * 0.6f};
    } else {
        target.z += 30.f;
    }
    const V3 d{target.x - hand.x, target.y - hand.y, target.z - hand.z};
    const Axes ax = AxesOf(g_aimGameRot);
    const float f = d.x * ax.x.x + d.y * ax.x.y + d.z * ax.x.z, r = d.x * ax.y.x + d.y * ax.y.y + d.z * ax.y.z,
                u = d.x * ax.z.x + d.y * ax.z.y + d.z * ax.z.z;
    // XR frame: x right, y up, -z forward; yaw positive = left
    const float yaw = std::atan2(-r, f) * 180.f / kPi, pitch = std::atan2(u, std::sqrt(f * f + r * r)) * 180.f / kPi;
    if (g_fakeAimGirl) ctrl::SetFakeQuiet(yaw + g_fakeGirlYawOff.load(), pitch);  // re-check: the console may have switched modes meanwhile
}

bool g_aimDiag = false;
// dev (!bossmarkers <s>): the heroine boss HUD's marker table (GG2BsHeroineBattleHud.m_bossMarkerPosInfoTbl: the "!"
// dolls' positions as the HUD hit test sees them) against the game cursor, 4x a second
ULONGLONG g_bossMarkersUntil = 0;
ue3::PropInfo g_bmTbl, g_bmVis, g_bm3;
bool g_bmOk = false;
bool FirstBossMarker3D(V3& out) {
    if (!g_bmOk) return false;
    for (int32_t i = 0, n = ue3::NumObjects(); i < n; ++i) {
        const uintptr_t o = ue3::ObjectAt(i);
        if (!o || !ue3::IsA(o, "GG2BsHeroineBattleHud") || ue3::PathName(o).find("Default__") != std::string::npos) continue;
        for (uint32_t k = 0; k < g_bmTbl.arrayDim && k < 16; ++k) {
            const uintptr_t e = o + g_bmTbl.offset + k * g_bmTbl.elementSize;
            uint32_t vis = 0;
            Read(e + g_bmVis.offset, vis);
            if (vis & g_bmVis.bitMask) return Read(e + g_bm3.offset, out);
        }
    }
    return false;
}
// dev (!aediag <s>): minigame (GG2ActionEvent) input spaces - the virtual mouse (GG2PlayerInput.m_mousePosition +0xC48),
// the touch position the minigame reads (+0xC80, GetMouseTouchPosition), the display size (+0xC50), the sight
// (m_CameraControlParameter_MarkerShot.m_TargetSightDispPos) and every marker's display position (m_DispPos[0])
ULONGLONG g_aeDiagUntil = 0;
void LogAeTick() {
    if (!g_aeDiagUntil || GetTickCount64() > g_aeDiagUntil) return;
    static ULONGLONG s_next = 0;
    if (GetTickCount64() < s_next) return;
    s_next = GetTickCount64() + 100;
    static ue3::PropInfo pCam, pSight, pDisp;
    static bool s_ok = [] {
        const uintptr_t c = ue3::FindObject("GG2Game.GG2ActionEventBasePlayerController", "Class");
        const uintptr_t st = ue3::FindObject("GG2Game.GG2ActionEventBasePlayerDefine.sGG2AEPlayer_CameraControlParameter_MarkerShot", "ScriptStruct");
        const uintptr_t m = ue3::FindObject("GG2Game.GG2ActionEventBaseMarkerActor", "Class");
        return c && st && m && ue3::FindProperty(c, "m_CameraControlParameter_MarkerShot", pCam) &&
               ue3::FindProperty(st, "m_TargetSightDispPos", pSight) && ue3::FindProperty(m, "m_DispPos", pDisp);
    }();
    if (!s_ok || !g_lastPc) { LOG("[vr.ae] properties not found / no controller"); g_aeDiagUntil = 0; return; }
    uintptr_t pi = 0;
    Read(g_lastPc + 0x554, pi);
    int32_t mp[2]{};
    float tp[2]{}, ds[2]{}, sight[2]{};
    if (pi) Read(pi + 0xC48, mp), Read(pi + 0xC80, tp), Read(pi + 0xC50, ds);
    if (ue3::IsA(g_lastPc, "GG2ActionEventBasePlayerController")) Read(g_lastPc + pCam.offset + pSight.offset, sight);
    std::string s;
    for (int32_t i = 0, n = ue3::NumObjects(); i < n && s.size() < 600; ++i) {
        const uintptr_t o = ue3::ObjectAt(i);
        if (!o || !ue3::IsA(o, "GG2ActionEventBaseMarkerActor") || ue3::IsA(o, "GG2ActionEventBaseMarkerInvisibleActor") ||
            ue3::PathName(o).find("Default__") != std::string::npos)
            continue;
        float d[2]{};
        Read(o + pDisp.offset, d);
        char b[96];
        snprintf(b, sizeof b, " %s (%.0f,%.0f);", ue3::ObjName(ue3::ClassOf(o)).c_str() + 13, d[0], d[1]);
        s += b;
    }
    uint8_t dev = 0xFF;
    static uint32_t s_offDev = 0;
    if (!s_offDev)
        if (uintptr_t c = ue3::FindObject("GG2Game.GG2PlayerInput", "Class")) {
            ue3::PropInfo q;
            if (ue3::FindProperty(c, "m_CurrentControlDeviceType", q)) s_offDev = q.offset;
        }
    if (pi && s_offDev) Read(pi + s_offDev, dev);
    static bool s_vt = false;
    if (pi && !s_vt) {
        s_vt = true;
        uintptr_t vt = 0;
        Read(pi, vt);
        const uintptr_t mod = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        for (uint32_t o : {0x318u, 0x320u, 0x328u, 0x330u, 0x338u, 0x3B8u, 0x3C8u, 0x3D0u}) {
            uintptr_t f = 0;
            Read(vt + o, f);
            LOG("[vr.ae] PlayerInput vtable +0x%X -> rva 0x%llX (device field +0x%X)", o, (unsigned long long)(f - mod), s_offDev);
        }
    }
    {  // the requests' detection state (SAEMarkerRequestData in m_aeMarkerRequestTbl)
        static ue3::PropInfo pTbl, fState, fType, fTimer, fTouch, fBegin;
        static uint32_t s_stride = 0;
        static int s_rqOk = -1;
        if (s_rqOk < 0) {
            const uintptr_t c = ue3::FindObject("GG2Game.GG2ActionEventBasePlayerInput", "Class");
            const uintptr_t st = ue3::FindObject("GG2Game.GG2ActionEventBasePlayerInput.SAEMarkerRequestData", "ScriptStruct");
            s_rqOk = c && st && ue3::FindProperty(c, "m_aeMarkerRequestTbl", pTbl) && ue3::FindProperty(st, "m_state", fState) &&
                   ue3::FindProperty(st, "m_RequestType", fType) && ue3::FindProperty(st, "m_timerFromStart", fTimer) &&
                   ue3::FindProperty(st, "m_touchNo", fTouch) && ue3::FindProperty(st, "m_beginTouchPos2D", fBegin);
            if (s_rqOk) {
                bool ptr = false;
                for (auto& q : ue3::Properties(st)) {
                    s_stride = std::max(s_stride, q.offset + q.elementSize * std::max(1u, q.arrayDim));
                    ptr |= q.type.find("Object") != std::string::npos;
                    LOG("[vr.ae] SAEMarkerRequestData +0x%X %s %s", q.offset, q.type.c_str(), q.name.c_str());
                }
                s_stride = (s_stride + (ptr ? 7 : 3)) & ~(ptr ? 7u : 3u);
                LOG("[vr.ae] SAEMarkerRequestData stride %u", s_stride);
            }
        }
        if (s_rqOk == 1 && pi) {
            uintptr_t data = 0;
            int32_t num = 0;
            if (pTbl.arrayDim > 1) {  // a fixed array: inline
                data = pi + pTbl.offset, num = int32_t(pTbl.arrayDim), s_stride = pTbl.elementSize;
            } else {
                game::Read(pi + pTbl.offset, data), game::Read(pi + pTbl.offset + 8, num);
            }
            static bool s_said = false;
            if (!s_said) s_said = true, LOG("[vr.ae] m_aeMarkerRequestTbl: %s, dim %u, element %u", pTbl.type.c_str(), pTbl.arrayDim, pTbl.elementSize);
            std::string rq;
            for (int32_t i = 0; data && i < num && i < 6; ++i) {
                const uintptr_t e = data + uintptr_t(i) * s_stride;
                uint8_t stt = 0, typ = 0, tno = 0;
                float tm = 0, bg[2]{};
                game::Read(e + fState.offset, stt), game::Read(e + fType.offset, typ), game::Read(e + fTouch.offset, tno);
                game::Read(e + fTimer.offset, tm), game::Read(e + fBegin.offset, bg);
                char b[96];
                snprintf(b, sizeof b, " r%d type %u state %u timer %.2f touch %u begin (%.0f,%.0f);", i, typ, stt, tm, tno, bg[0], bg[1]);
                rq += b;
            }
            static ue3::PropInfo hVit, hType;
            static int s_h = -1;
            if (s_h < 0) {
                const uintptr_t hc = ue3::FindObject("GG2Game.GG2ActionEventBaseHitParameterMarkerRequest", "Class");
                s_h = hc && ue3::FindProperty(hc, "m_Vitality", hVit) && ue3::FindProperty(hc, "m_MarkerRequestType", hType);
            }
            for (int32_t i = 0, n = ue3::NumObjects(); s_h == 1 && i < n; ++i) {
                const uintptr_t o = ue3::ObjectAt(i);
                if (!o || !ue3::IsA(o, "GG2ActionEventBaseHitParameterMarkerRequest") || ue3::PathName(o).find("Default__") != std::string::npos) continue;
                float vit = 0;
                uint8_t ty = 0;
                game::Read(o + hVit.offset, vit), game::Read(o + hType.offset, ty);
                char b[64];
                snprintf(b, sizeof b, " hit type %u vitality %.2f;", ty, vit);
                rq += b;
            }
            if (!rq.empty()) LOG("[vr.ae] requests:%s", rq.c_str());
        }
    }
    std::string tt;  // touch table: +0x948 + i*0x44: state (1 began, 2 held, 3 released), location +0x18
    for (int i = 0; pi && i < 3; ++i) {
        int32_t st = 0;
        float loc[2]{};
        Read(pi + 0x948 + i * 0x44, st), Read(pi + 0x960 + i * 0x44, loc);
        char b[64];
        snprintf(b, sizeof b, " t%d %d (%.0f,%.0f)", i, st, loc[0], loc[1]);
        tt += b;
    }
    LOG("[vr.ae] mouse (%d,%d) touch (%.0f,%.0f) display %.0fx%.0f sight (%.0f,%.0f) device %u%s;%s", mp[0], mp[1], tp[0], tp[1], ds[0],
        ds[1], sight[0], sight[1], dev, tt.c_str(), s.c_str());
}
void LogBossMarkersTick() {
    if (!g_bossMarkersUntil || GetTickCount64() > g_bossMarkersUntil) return;
    static ULONGLONG s_next = 0;
    if (GetTickCount64() < s_next) return;
    s_next = GetTickCount64() + 250;
    static ue3::PropInfo pTbl, pNum, pVis, p3, p2;
    static bool s_ok = [] {
        const uintptr_t c = ue3::FindObject("GG2Game.GG2BsHeroineBattleHud", "Class");
        const uintptr_t st = ue3::FindObject("GG2Game.GG2BsHeroineBattleHud.SGG2BsHeroineHudPosInfo", "ScriptStruct");
        return c && st && ue3::FindProperty(c, "m_bossMarkerPosInfoTbl", pTbl) && ue3::FindProperty(c, "m_numActiveBossMarkers", pNum) &&
               ue3::FindProperty(st, "m_isVisible", pVis) && ue3::FindProperty(st, "m_pos3D", p3) && ue3::FindProperty(st, "m_pos2D", p2);
    }();
    if (!s_ok) { LOG("[vr.boss] marker table properties not found"); g_bossMarkersUntil = 0; return; }
    g_bmTbl = pTbl, g_bmVis = pVis, g_bm3 = p3, g_bmOk = true;
    uintptr_t hud = 0;
    for (int32_t i = 0, n = ue3::NumObjects(); i < n && !hud; ++i) {
        const uintptr_t o = ue3::ObjectAt(i);
        if (o && ue3::IsA(o, "GG2BsHeroineBattleHud") && ue3::PathName(o).find("Default__") == std::string::npos) hud = o;
    }
    float cur[2]{};
    if (g_lastPc) Read(g_lastPc + 0xBA4, cur);
    if (g_lastViewW > 0 && g_lastViewH > 0) {  // as the hit test now reads it (hk_SightCursorY)
        uint32_t bx, by, bw, bh;
        ui::BandOf(uint32_t(g_lastViewW + 0.5f), uint32_t(g_lastViewH + 0.5f), bx, by, bw, bh);
        cur[1] += float(by);
    }
    if (!hud) { LOG("[vr.boss] no boss HUD; cursor (%.0f,%.0f)", cur[0], cur[1]); return; }
    int32_t num = 0;
    Read(hud + pNum.offset, num);
    std::string s;
    for (uint32_t k = 0; k < pTbl.arrayDim && k < 16; ++k) {
        const uintptr_t e = hud + pTbl.offset + k * pTbl.elementSize;
        uint32_t vis = 0;
        V3 w{};
        float p[2]{};
        Read(e + pVis.offset, vis), Read(e + p3.offset, w), Read(e + p2.offset, p);
        if (!(vis & pVis.bitMask)) continue;
        char b[160];
        // the hit test scales the base (1920x1080) position to the game screen (HUD band) and allows ~70 * band/960 px
        const float sx = g_lastBandW ? float(g_lastBandW) / 1920.f : 1.f, sy = g_lastBandH ? float(g_lastBandH) / 1080.f : 1.f;
        const float mx = p[0] * sx, my = p[1] * sy;
        snprintf(b, sizeof b, " [%u] 2D (%.0f,%.0f)=screen (%.0f,%.0f) 3D (%.0f,%.0f,%.0f) d %.0f of ~%.0f;", k, p[0], p[1], mx, my, w.x, w.y,
                 w.z, std::sqrt((mx - cur[0]) * (mx - cur[0]) + (my - cur[1]) * (my - cur[1])), 70.f * float(g_lastBandW) / 960.f);
        s += b;
    }
    int32_t shots = 0, hits = 0;
    uintptr_t tgt = 0;
    if (g_lastPc) Read(g_lastPc + 0xB88, shots), Read(g_lastPc + 0xB8C, hits), Read(g_lastPc + 0xBE8, tgt);  // m_sm / m_nsm target
    LOG("[vr.boss] active %d (table %u x %u B); cursor (%.0f,%.0f); shots %d hits %d target %s;%s", num, pTbl.arrayDim, pTbl.elementSize,
        cur[0], cur[1], shots, hits, tgt && ue3::IsObject(tgt) ? ue3::ObjName(tgt).c_str() : "-", s.c_str());
}
std::atomic<float> g_aimPanelX{0}, g_aimPanelY{0};
std::atomic<ULONGLONG> g_aimPanelT{0};
void UpdateControllerCursor() {
    {  // hit feedback: the score manager's hit counter (m_sm at PC+0xB7C, hitCount +0x10) went up
        static uintptr_t s_pc = 0;
        static int32_t s_hits = -1;
        int32_t hits = -1;
        if (g_lastPc && ue3::IsObject(g_lastPc) && ue3::IsA(g_lastPc, "GG2StandardPlayerController")) Read(g_lastPc + 0xB7C + 0x10, hits);
        if (g_lastPc != s_pc) s_pc = g_lastPc, s_hits = hits;
        if (hits > s_hits && s_hits >= 0 && hits - s_hits < 50) ctrl::HitPulse();
        s_hits = hits;
    }
    ctrl::Aim aim;
    if (!config::Get().vrControllers || !g_lastPc || !ue3::IsObject(g_lastPc) ||
        !ue3::IsA(g_lastPc, "GG2StandardPlayerController") || !ctrl::GetAim(aim) || !g_lastBandW)
        return;
    V3 rayOrg, d;
    AimRayRel(aim, rayOrg, d);
    const float D = config::Get().vrUiDistance, W = ui::PanelWidth(), H = W * float(g_lastBandH) / float(g_lastBandW);
    const XrVector3f hp{rayOrg.x, rayOrg.y, rayOrg.z};
    // The panel crossing only exists while the ray points toward the panel plane (within ~87 deg of the game camera's
    // forward). The eye-view cursor doesn't need it: aiming at girls beside / behind the game camera with the head
    // turned, the cursor used to freeze there - no weak-spot hits, no weak-spot pop-ups on them (0.9.111, 2F Stairway).
    const bool crossesPanel = d.z <= -0.05f;
    const bool eyeCursor = g_cursorSpace == 1 && g_lastViewW > 0 && g_cur.halfH > 0;
    if (!crossesPanel && !eyeCursor) return;  // panel cursor space: pointing away from the panel plane
    float px = 0, py = 0;
    const float z = g_zoom.load();
    if (crossesPanel) {
        const float t = (-D - hp.z) / d.z;
        const float x = hp.x + d.x * t, y = hp.y + d.y * t - ui::PanelYOffset();
        px = (x * z / W + 0.5f) * float(g_lastBandW), py = (0.5f - y * z / H) * float(g_lastBandH);
        // where the aim ray crosses the HUD panel, in its stage units (route choice arrows - ui_layer)
        g_aimPanelX = px * 1920.f / float(g_lastBandW), g_aimPanelY = py * 1080.f / float(g_lastBandH), g_aimPanelT = GetTickCount64();
    }
    if (eyeCursor) {
        // the eye-view pixel of the aim point, minus the HUD band's top offset. The point is where the shot ray hits
        // (the game's last trace), else reticle_distance along it - the same point the reticle and the zoom scope
        // are drawn at. A fixed 10 m put the cursor off the hit point by the hand-to-eye parallax for anything nearer
        // or farther, which the zoom magnifies.
        const float hitD = ctrl::HitDistance(), maxD = config::Get().vrReticleDistance;
        const float cd = hitD > 0.1f ? std::fmin(hitD, maxD) : maxD;
        const V3 pt{hp.x + d.x * cd - g_aimHead.position.x, hp.y + d.y * cd - g_aimHead.position.y,
                    hp.z + d.z * cd - g_aimHead.position.z};
        const auto& ho = g_aimHead.orientation;
        const V3 e = Rotate(Q4{-ho.x, -ho.y, -ho.z, ho.w}, pt);
        if (e.z < -0.01f) {
            uint32_t bx, by, bw, bh;
            ui::BandOf(uint32_t(g_lastViewW + 0.5f), uint32_t(g_lastViewH + 0.5f), bx, by, bw, bh);
            px = 0.5f * g_lastViewW * (1.f + (e.x / -e.z) / std::tan(g_cur.halfH));
            py = 0.5f * g_lastViewH * (1.f - (e.y / -e.z) / std::tan(g_cur.halfV)) - float(by);
            g_aimEyeX = px, g_aimEyeY = py + float(by);
        } else if (!crossesPanel) {
            return;  // the aim point is behind the head and off the panel: nothing to put the cursor on
        }
        {  // once per aim side: the cursor follows an aim the panel plane can't (diagnostic)
            static bool s_was = true;
            if (crossesPanel != s_was) {
                s_was = crossesPanel;
                static int s_logs = 0;
                if (s_logs++ < 20) LOG("[vr] sight cursor %s the panel plane (eye view %.0f,%.0f)", crossesPanel ? "back on" : "beyond", px, py);
            }
        }
    }
    px = std::fmax(-4.f * g_lastBandW, std::fmin(5.f * g_lastBandW, px));
    py = std::fmax(-4.f * g_lastBandH, std::fmin(5.f * g_lastBandH, py));
    if (g_aimDiag) {  // dev: what the game made of last frame's cursor, and its shot ray against the controller's
        static ULONGLONG s_t = 0;
        static float s_px = 0, s_py = 0;
        if (GetTickCount64() - s_t > 1000) {
            s_t = GetTickCount64();
            float cur[2]{}, base[2]{};
            V3 gl{}, gd{};
            Read(g_lastPc + 0xBA4, cur), Read(g_lastPc + 0xB9C, base);
            Read(g_lastPc + 0xBB8, gl), Read(g_lastPc + 0xBC4, gd);
            const V3 wd = ToWorld(AxesOf(g_aimGameRot), XrToUe(d));
            const float gp = std::atan2(gd.z, std::sqrt(gd.x * gd.x + gd.y * gd.y)) * 57.2958f,
                        hpch = std::atan2(wd.z, std::sqrt(wd.x * wd.x + wd.y * wd.y)) * 57.2958f;
            const float gy = std::atan2(gd.y, gd.x) * 57.2958f, hy = std::atan2(wd.y, wd.x) * 57.2958f;
            int32_t mp[2]{};
            uintptr_t pi = 0;
            uint8_t dev = 0xFF;
            static uint32_t s_offDev = 0;
            if (!s_offDev)
                if (uintptr_t c = ue3::FindObject("GG2Game.GG2PlayerInput", "Class")) {
                    ue3::PropInfo q;
                    if (ue3::FindProperty(c, "m_CurrentControlDeviceType", q)) s_offDev = q.offset;
                }
            if (game::Read(g_lastPc + 0x554, pi) && pi) {
                Read(pi + 0xC48, mp);
                if (s_offDev) Read(pi + s_offDev, dev);
            }
            LOG("[vr.aimdiag] wrote (%.0f,%.0f) game now (%.0f,%.0f) base (%.0f,%.0f); new (%.0f,%.0f) band %ux%u view %.0fx%.0f half %.1f/%.1f zoom %.2f; "
                "shot ray pitch %.2f yaw %.2f vs hand pitch %.2f yaw %.2f; mouse (%d,%d) device %u (+0x%X) paused %d",
                s_px, s_py, cur[0], cur[1], base[0], base[1], px, py, g_lastBandW, g_lastBandH, g_lastViewW, g_lastViewH,
                g_cur.halfH * 57.2958f, g_cur.halfV * 57.2958f, z, gp, gy, hpch, hy, mp[0], mp[1], dev, s_offDev, g_lastPaused ? 1 : 0);
        }
        s_px = px, s_py = py;
    }
    *reinterpret_cast<float*>(g_lastPc + 0xBA4) = px;  // m_nsm.cursorScreenPosition (live object, game thread)
    *reinterpret_cast<float*>(g_lastPc + 0xBA8) = py;
    *reinterpret_cast<float*>(g_lastPc + 0xB9C) = px;  // m_nsm.cursorScreenPositionBase (the game rebuilds the
    *reinterpret_cast<float*>(g_lastPc + 0xBA0) = py;  // position from it every tick)
    // reticle depth: the game's own trace along the shot ray (m_nsm.isHitObject / HitLocation)
    uint32_t flags = 0;
    V3 hit{};
    Read(g_lastPc + 0xB98, flags);
    Read(g_lastPc + 0xBD0, hit);
    const Axes game = AxesOf(g_aimGameRot);
    const float ws = config::Get().vrWorldScale;
    const V3 org = g_aimGameLoc + ToWorld(game, XrToUe({hp.x, hp.y, hp.z}) * ws);
    const V3 wd = ToWorld(game, XrToUe(d));
    const V3 v{hit.x - org.x, hit.y - org.y, hit.z - org.z};
    const float along = v.x * wd.x + v.y * wd.y + v.z * wd.z, len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    // accept only a hit that lies on the current ray (the trace can lag a frame behind)
    const bool onRay = (flags & 0x10) && along > 1.f && along > 0.995f * len;
    ctrl::SetHitDistance(onRay ? along / ws : 0.f);
    static int s_hitLogs = 0;
    if (onRay && s_hitLogs < 5) ++s_hitLogs, LOG("[vr] reticle on shot-ray hit at %.2f m", along / ws);
    // diag: the game's own shot ray (m_nsm ray origin +0xBB8, direction +0xBC4) against the controller ray, zoomed
    static ULONGLONG s_zlog = 0;
    static int s_zlogs = 0;
    if (z > 1.001f && s_zlogs < 40 && GetTickCount64() - s_zlog > 400) {
        s_zlog = GetTickCount64(), ++s_zlogs;
        V3 gl{}, gd{};
        Read(g_lastPc + 0xBB8, gl), Read(g_lastPc + 0xBC4, gd);
        const float c = std::fmin(1.f, std::fmax(-1.f, Dot(gd, wd) / std::sqrt(std::fmax(Dot(gd, gd) * Dot(wd, wd), 1e-12f))));
        LOG("[vr.zoomaim] zoom %.2f cursor (%.0f,%.0f): game ray org (%.0f,%.0f,%.0f) vs hand (%.0f,%.0f,%.0f), angle %.2f deg; trace hit %u along %.1f m",
            z, px, py, gl.x, gl.y, gl.z, org.x, org.y, org.z, std::acos(c) * 57.2958f, (flags >> 4) & 1, along / ws);
    }
}

void PatchAeScript();  // below: minigame requests beyond the flat screen area
ULONGLONG g_auraDumpAt = 0;  // diagnostic: pending stereo dump after a devil aura appeared

double PerfNowMs() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) * 1000.0 / double(f.QuadPart);
}
// Performance (0.9.53): the eyes see more downward than upward (e.g. U44 / D55 deg). The symmetric render frustum
// had to cover the larger half both ways; both eye renders are now tilted down by the midpoint (about 5.5 deg) and
// their vertical half-angle shrinks to match - ~24% fewer pixels for the same view and sharpness (the user's stages ran
// at half rate: GPU ~15.5 ms against the 13.9 ms of 72 Hz). The eye poses and FOVs are adjusted once, right after
// sampling; everything downstream (frustum, render size, the submitted views, UI projections) follows from them.
Q4 g_cantQ{};  // the tilt applied to the eye renders (identity = none)
void CantEyes(xr::HeadSample& hs) {
    g_cantQ = {};
    if (!config::Get().vrRenderCant) return;
    const float a = 0.5f * (std::fmax(hs.fov[0].angleUp, hs.fov[1].angleUp) + std::fmin(hs.fov[0].angleDown, hs.fov[1].angleDown));
    if (std::fabs(a) < 0.005f) return;
    const Q4 q{std::sin(a / 2), 0, 0, std::cos(a / 2)};  // about the eye's +X: negative = down
    for (int e = 0; e < 2; ++e) {
        for (XrPosef* p : {&hs.eyeRel[e], &hs.eyeRaw[e]}) {
            const Q4 o{p->orientation.x, p->orientation.y, p->orientation.z, p->orientation.w};
            const Q4 r = Mul(o, q);
            p->orientation = {r.x, r.y, r.z, r.w};
        }
        // a rotation about X keeps each ray's vertical angle a pure offset: exact
        hs.fov[e].angleUp -= a, hs.fov[e].angleDown -= a;
    }
    g_cantQ = q;
    static int s_logs = 0;
    if (s_logs++ < 1) LOG("[vr] eye renders tilted %.1f deg (vertical half-angle %.1f instead of %.1f)", a * 57.2958f,
                          std::fmax(hs.fov[0].angleUp, -hs.fov[0].angleDown) * 57.2958f,
                          (std::fmax(hs.fov[0].angleUp, -hs.fov[0].angleDown) + std::fabs(a)) * 57.2958f);
}
void __fastcall hk_VpDraw(void* viewport, uint32_t present) {
    const int mode = config::Get().vrMode;
    if (reinterpret_cast<uintptr_t>(_ReturnAddress()) != Addr(kVpDrawFromRedraw) || mode == 0) {
        o_VpDraw(viewport, present);
        return;
    }
    ++g_drawCount;
    if (config::Get().vrPerfLog) {  // [perf] game: draw-to-draw rate of the main viewport, any mode (flat testing too)
        static double s_last = 0, s_sum = 0, s_max = 0;
        static int s_n = 0;
        static ULONGLONG s_next = 0;
        const double now = PerfNowMs();
        if (s_last > 0 && now - s_last < 1000.0) s_sum += now - s_last, s_max = std::fmax(s_max, now - s_last), ++s_n;
        s_last = now;
        if (GetTickCount64() >= s_next) {
            if (s_next && s_n) LOG("[perf] game thread: %.1f frames/s (%.2f ms avg, %.1f max) in %s", 1000.0 * s_n / s_sum, s_sum / s_n, s_max,
                                   g_lastGameClass.c_str());
            s_next = GetTickCount64() + 5000, s_sum = s_max = 0, s_n = 0;
        }
    }
    xr::HeadSample hs;
    const bool immersive = SceneIsImmersive();
    bool ok = immersive && Sample(hs);
    if (ok && mode == 2) CantEyes(hs);
    else g_cantQ = {};
    UpdateZoom();
    if (ok) {  // minigames: the panel follows the gaze up / down (ui_layer)
        const auto& q = hs.rel.orientation;
        const V3 f = Rotate(Q4{q.x, q.y, q.z, q.w}, {0, 0, -1});
        ui::UpdatePanelFollow(std::asin(std::fmax(-1.f, std::fmin(1.f, f.y))) * 57.2958f);
    }
    UpdateFakeAimAtGirl();
    UpdateControllerCursor();
    LogBossMarkersTick();
    LogAeTick();
    ui::AeRevealTick();
    ui::DokiTick();
    KeyDiagTick();
    // menu pointer: virtual-screen scenes, the in-stage pause and the immersive menu scenes (menus on the UI panel)
    // the Continue / Give Up prompt after losing in a stage: a menu over the (unpaused) stage, like the pause menu
    // the end-of-stage score screen is a menu over the still-running stage too (0.9.63: reticle and trigger did
    // nothing there) - minigames' results already work through their touch pointer
    const bool stageResult = immersive && g_lastGameClass.find("GG2ActionEvent") == std::string::npos && ui::StageResultUp();
    {
        static bool s_wasResult = false;
        if (stageResult != s_wasResult) s_wasResult = stageResult, LOG("[vr] stage results %s", stageResult ? "up: menu pointer" : "gone");
    }
    const bool continuePrompt = immersive && (ui::ContinuePromptUp() || stageResult);
    {
        static bool s_was = false;
        if (continuePrompt != s_was) s_was = continuePrompt, LOG("[vr] continue prompt %s", continuePrompt ? "up: menu pointer" : "gone");
    }
    // Zoom interrupted: losing all HP, pausing, Mom is coming, game over drop the zoom (0.9.110: with the grip held
    // when the HP ran out, the zoom stayed on through the continue prompt until pressed again - the grip's release never
    // reached the game behind the prompt). Like the boss scripts in PlayerDead: m_nsm.isZoom (+0xB98 bit 0x8) cleared;
    // the controllers send no zoom until the grip is let go (ZoomInterrupted).
    {
        const bool interrupt = g_lastPaused || continuePrompt || ui::MamakitaUp() ||
                               g_lastGameClass.find("GG2GameOverGameInfo") != std::string::npos;
        g_zoomInterrupt = interrupt;
        if (interrupt && g_lastPc && ue3::IsObject(g_lastPc) && ue3::IsA(g_lastPc, "GG2StandardPlayerController")) {
            uint32_t f = 0;
            if (Read(g_lastPc + 0xB98, f) && (f & 0x8u)) {
                *reinterpret_cast<uint32_t*>(g_lastPc + 0xB98) = f & ~0x8u;
                static int s_logs = 0;
                if (s_logs++ < 20)
                    LOG("[vr] zoom dropped (%s)", g_lastPaused ? "paused" : continuePrompt ? "continue prompt / results" : "Mom is coming / game over");
            }
        }
    }
    bool menuScene = !immersive || g_lastPaused || continuePrompt;
    for (auto& k : config::Get().vrPointerGames)
        if (!k.empty() && g_lastGameClass.find(k) != std::string::npos) menuScene = true;
    // a menu is up: the pointer stays on it wherever the controller points (not in story cutscenes, whose trigger
    // keeps its own use; minigame play is excluded in the pointer itself)
    pointer::SetContext(menuScene, immersive, g_lastPc, !immersive || g_lastPaused || continuePrompt,
                        menuScene && g_lastGameClass.find("GG2DemoGame") == std::string::npos);
    // minigames: the pointer is also the touch position, in the game's eye-view projection space
    // (not while paused / at the continue prompt: those menus take the pointer in their own stage coordinates)
    pointer::SetTouchInEyeView(immersive && !g_lastPaused && !continuePrompt && g_lastGameClass.find("GG2ActionEvent") != std::string::npos);
    ui::SetTallPanel(immersive && config::Get().vrAeMinigameFix && g_lastGameClass.find("GG2ActionEvent") != std::string::npos);
    // hud_extend_rows (0.9.70 B): stage gameplay only - not paused, not at the continue prompt / results
    ui::SetStageRows(config::Get().vrHudExtendRows && immersive && !g_lastPaused && !continuePrompt &&
                     (g_lastGameClass.find("GG2ShootingGame") != std::string::npos ||
                      g_lastGameClass.find("GG2DokiSpHeroineGame") != std::string::npos));  // + FDDM (0.9.91: its zoom-in marks)
    {  // a hidden HUD comes back whenever the stage ends, is left or lost (0.9.39): any game mode or level change
       // (results -> stage select / title / next story stage / game over map), and the continue prompt
        static uint32_t s_changes = 0;
        static bool s_prompt = false;
        if (g_gameChanges != s_changes) s_changes = g_gameChanges, ui::ShowHud("game mode / level change");
        if (continuePrompt && !s_prompt) ui::ShowHud("continue prompt");
        s_prompt = continuePrompt;
    }
    ui::SetHudToggleContext(immersive && !g_lastPaused && !continuePrompt &&
                            (g_lastGameClass.find("GG2ShootingGame") != std::string::npos || g_lastGameClass.find("GG2Bs") != std::string::npos));
    if (immersive && config::Get().vrAeMinigameFix && g_lastGameClass.find("GG2ActionEvent") != std::string::npos) PatchAeScript();
    {
        // world up in the game camera's frame (XR axes: x right, y up, z back), from the camera's rotation
        const Axes ax = AxesOf(g_aimGameRot);
        const bool lr = immersive && config::Get().vrDressingBackdrop && g_lastGameClass.find("GG2LockerRoomGameInfo") != std::string::npos;
        g_lrScene = immersive && g_lastGameClass.find("GG2LockerRoomGameInfo") != std::string::npos;
        if (!g_lrScene) g_lrPin.set = false;
        static uint64_t s_composedAtEntry = 0;  // the camera must have been read in this scene (not a stale one)
        if (!lr) {
            g_roomSrc.actor = 0;
            s_composedAtEntry = g_composed;
        } else if ((!g_roomSrc.actor || !ue3::IsObject(g_roomSrc.actor)) && g_composed > s_composedAtEntry + 20 &&
                   GetTickCount64() >= g_roomSrc.nextTry) {
            g_roomSrc.actor = 0;
            g_roomSrc.nextTry = GetTickCount64() + 1000;
            FindDressingRoomMesh(g_aimGameLoc, g_aimGameRot);
        }
        std::lock_guard l(g_voidMu);
        g_room.on = lr && g_roomSrc.actor;
        g_roomActive = g_room.on;
        g_room.kind = 1, g_room.fwd[0] = g_room.fwd[1] = g_room.fwd[2] = 0.f;
        {  // Academy Store
            const bool shop = immersive && config::Get().vrMenuLayers && g_lastGameClass.find("GG2BaseGame") != std::string::npos &&
                              ui::ShopOpen();
            if (!shop) {
                g_shopSrc.found = false;
            } else if (!g_shopSrc.found && GetTickCount64() >= g_shopSrc.nextTry) {
                g_shopSrc.nextTry = GetTickCount64() + 1000;
                FindShopMesh();
            }
            g_shopActive = shop && g_shopSrc.found;
            // the game's own camera spot for the store (measured: 90 uu right of the booth's centre, 133 uu in front)
            const V3 gameCam{g_shopSrc.centreW.x + 90.f, g_shopSrc.centreW.y + 133.f, g_shopSrc.camLoc.z};
            (void)gameCam;  // Aoi can't be turned (0.9.3: no visible change): the game's own camera is kept
        }
        g_room.up[0] = ax.y.z, g_room.up[1] = ax.z.z, g_room.up[2] = -ax.x.z;
        // the room centre in the recentered frame (metres, XR axes): world offset from the viewpoint, in its axes
        const V3 v = g_roomSrc.centreW - g_aimGameLoc;  // g_aimGameLoc is the moved viewpoint while the room is up
        const float ws = config::Get().vrWorldScale;
        g_room.centre[0] = Dot(v, ax.y) / ws, g_room.centre[1] = Dot(v, ax.z) / ws, g_room.centre[2] = -Dot(v, ax.x) / ws;
        g_room.unitsPerMetre = ws, g_room.radius = g_roomSrc.radius, g_room.wallH = g_roomSrc.wallH;
        if (g_shopActive) {
            const V3 sv = g_shopSrc.centreW - g_aimGameLoc;
            g_room.on = true, g_room.kind = 2;
            g_room.centre[0] = Dot(sv, ax.y) / ws, g_room.centre[1] = Dot(sv, ax.z) / ws, g_room.centre[2] = -Dot(sv, ax.x) / ws;
            g_room.radius = g_shopSrc.back, g_room.wallH = g_shopSrc.ceil, g_room.halfWidth = g_shopSrc.halfWidth, g_room.base = g_shopSrc.base;
            // the booth faces +Y: the direction into it (world -Y) in the camera's frame - the game's camera looks at
            // it from ~22 deg off-axis
            const V3 into{0.f, -1.f, 0.f};
            g_room.fwd[0] = Dot(into, ax.y), g_room.fwd[1] = Dot(into, ax.z), g_room.fwd[2] = -Dot(into, ax.x);
        }
        const bool g_voidOn = g_room.on;
        // the game's depth of field blurs the black void into the props' edges (dark halos the room fill can't
        // replace) and would blur the room: off in the Dressing Room, the user's own setting back afterwards
        static bool s_dofOff = false;
        if (g_room.on != s_dofOff) {
            s_dofOff = g_room.on;
            settings_guard::MarkDirty();
            const std::string v = s_dofOff ? "False" : settings_guard::BaselineValue("DepthOfField", "True");
            ue3::console::Enqueue("scale set DepthOfField " + v);
            LOG("[vr] dressing room: depth of field -> %s", v.c_str());
        }
        static V3 s_logged{};
        static int s_camLogs = 0;
        if (g_voidOn && s_camLogs < 6 && std::fabs(g_aimGameLoc.x - s_logged.x) + std::fabs(g_aimGameLoc.y - s_logged.y) > 5.f &&
            (s_logged = g_aimGameLoc, ++s_camLogs))
            LOG("[vr] dressing room: camera (%.0f,%.0f,%.0f) rot (%d,%d,%d)", g_aimGameLoc.x, g_aimGameLoc.y, g_aimGameLoc.z,
                g_aimGameRot.pitch, g_aimGameRot.yaw, g_aimGameRot.roll);
    }
    // minigame touch = the grip; in the timed gesture phases (nothing to shoot) the trigger too, so the game's mouse
    // guide - hold the button and drag / circle - is literally the VR control (0.9.28)
    {
        // touch sources: the grip; in the gesture phases the triggers too. A new press while another source is still
        // held counts as a new tap (one frame up), so alternating LT / RT taps aren't merged (0.9.30)
        const bool g = ui::GesturePhase();
        const bool src[3] = {ctrl::GripHeld(), g && ctrl::TriggerHeld(), g && ctrl::OffTriggerHeld()};
        static bool s_prev[3]{}, s_last = false;
        bool any = false, edge = false;
        for (int i = 0; i < 3; ++i) any |= src[i], edge |= src[i] && !s_prev[i], s_prev[i] = src[i];
        const bool touch = any && !(edge && s_last);
        s_last = touch;
        pointer::Update(ctrl::TriggerHeld(), touch);
    }
    if (ok && !g_fakeHead.load()) UpdateVrRenderSize(hs);
    SetRenderMode(immersive && xr::Running());
    EngineSmoothingTick(ok && mode == 2 && (xr::Running() || g_fakeHead.load() != 0));
    xr::SetImmersiveScene(immersive);
    g_inFinalDoki = immersive && g_lastGameClass.find("GG2DokiSp") != std::string::npos;
    const bool posed = ok;  // a VR scene with a head pose
    if (ok && !RenderSizeSettled()) {  // VR resize still pending: show the flat screen until it lands
        ok = false;
        static uint64_t s_waitLogs = 0;
        if (s_waitLogs++ % 300 == 0) LOG("[vr] waiting for the VR render size before starting stereo");
    }
    {  // one line whenever stereo starts or stops, with the reason - a player's log shows at once why a scene was flat
        static int s_why = -1, s_lines = 0;
        static ULONGLONG s_since = 0;
        const bool live = xr::Running() || g_fakeHead.load() != 0;
        const int why = ok ? 0 : !immersive ? 1 : !live ? 2 : !posed ? 3 : 4;
        if (why != s_why) {
            static const char* const kWhy[] = {"on", "off: flat scene (virtual screen)", "off: the headset session is not running",
                                               "off: no head pose from the runtime", "off: waiting for the VR render size"};
            const ULONGLONG now = GetTickCount64();
            if (s_lines < 400) {  // a flapping state stops logging here (the [xr] frame lines still count stereo frames)
                ++s_lines;
                if (s_why > 0 && why == 0)
                    LOG("[vr] stereo on (was %s for %.1f s)", kWhy[s_why] + 5, double(now - s_since) / 1000.0);
                else
                    LOG("[vr] stereo %s in %s", kWhy[why], g_lastGameClass.c_str());
            }
            s_why = why, s_since = now;
        }
    }
    if (ok) ClearAspectConstraint();
    if (ok) {
        g_aimHead = hs.rel;
        {  // the cursor is projected into the eye view: with the eyes' tilt (CantEyes)
            const auto& o = hs.rel.orientation;
            const Q4 r = Mul(Q4{o.x, o.y, o.z, o.w}, g_cantQ);
            g_aimHead.orientation = {r.x, r.y, r.z, r.w};
        }
        std::lock_guard l(g_headRawMu);
        g_headRawPos = hs.raw.position;
    }
    g_aimValid = ok && mode == 2;
    if (ok && !g_fakeHead.load()) LogEyesOnce(hs);
    ++g_frameId;
    ui::UpdateMenuLayersGT(g_frameId, immersive && (g_lastGameClass.find("GG2TitleGame") != std::string::npos ||
                                                    g_lastGameClass.find("GG2BaseGame") != std::string::npos ||
                                                    g_lastGameClass.find("GG2ScoreAttackGameInfo") != std::string::npos));
    if (g_auraDumpAt && GetTickCount64() >= g_auraDumpAt && config::Get().vrDebugFakeHead + g_fakeHead.load() > 0) {
        g_auraDumpAt = 0;
        xr::RequestStereoDump();  // dev only (fake head)
    }
    ctrl::Aim scopeAim;
    if (ok && mode == 2 && config::Get().vrZoomScopeRender && config::Get().vrZoomScope && config::Get().vrZoomInPanel &&
        config::Get().vrControllers && ui::Enabled() && !g_lastPaused && g_zoom.load() > 1.001f && ctrl::GetAim(scopeAim)) {
        const float maxDist = config::Get().vrReticleDistance, hit = ctrl::HitDistance();
        const float dist = hit > 0.1f ? std::fmin(hit, maxDist) : maxDist;  // where the reticle is drawn
        SetFrustum(&hs.fov[0], 1);
        const float aspect = xr::BackbufferAspect();
        g_cur.halfH = std::atan(std::tan(g_cur.halfH) / g_zoom.load());
        g_cur.halfV = std::atan(std::tan(g_cur.halfH) / aspect);
        g_cur.fovDeg = g_scopeFovDeg = 2.f * g_cur.halfH * 180.f / kPi;
        g_cur.valid = true;
        // seen from the hand along the aim ray (like a scope on the gun): its centre is where the shot goes at every
        // distance. From the head, anything nearer than the reticle point sat off-centre by the hand-head parallax,
        // magnified by the zoom (0.8.4: shots landed left of the scope's centre).
        // head aim: from the head toward the aim ray's convergence point (the shot line)
        const bool fromHead = config::Get().vrAimFromHead;
        const float conv = fromHead ? config::Get().vrReticleDistance : 1e6f;
        g_cur.rel = {LookAtAimPoint(hs.rel, scopeAim.rel, conv), fromHead ? hs.rel.position : scopeAim.rel.position};
        g_cur.raw = {LookAtAimPoint(hs.raw, scopeAim.raw, conv), fromHead ? hs.raw.position : scopeAim.raw.position};
        xr::FrameRecord rec{2, 1, g_cur.raw, g_cur.halfH, g_cur.halfV, g_frameId, {hs.fov[0], hs.fov[1]}};
        xr::QueueFrameRecord(rec);
        g_scopeThread = GetCurrentThreadId();
        g_scopeDraw = true;
        o_VpDraw(viewport, present);
        g_scopeDraw = false;
        static int s_logs = 0;
        if (s_logs++ < 3) LOG("[vr] zoom scope render: FOV %.1f deg (zoom %.2f), reticle at %.1f m", g_cur.fovDeg, g_zoom.load(), dist);
    }
    if (ok && mode == 2) {
        const int variant = g_stereoVariant.load();
        double eyeMs[2]{};
        {
            const auto& a = hs.eyeRaw[0].position;
            const auto& b = hs.eyeRaw[1].position;
            g_eyeSepM = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y) + (b.z - a.z) * (b.z - a.z));
            // effect_rest_depth: the disparity (stage units) of something at the HUD panel's distance
            const float D = config::Get().vrUiDistance, ht = g_eyeHalfTan;
            ui::SetEffectRestShift(D > 0.1f && ht > 0.f ? g_eyeSepM / D / ht * 960.f : 0.f);
        }
        for (int eye = 0; eye < 2; ++eye) {
            const int src = variant == 1 ? 1 - eye : variant == 2 ? 0 : eye;  // which eye pose renders this image
            g_cur.valid = true;
            g_cur.rel = ScaledEye(hs.eyeRel[src], hs.rel);
            g_cur.raw = ScaledEye(hs.eyeRaw[src], hs.raw);
            SetFrustum(&hs.fov[eye], 1);
            g_eyeHalfTan = std::tan(g_cur.halfH);
            xr::FrameRecord rec{eye, 1, g_cur.raw, g_cur.halfH, g_cur.halfV, g_frameId, {hs.fov[0], hs.fov[1]}};
            xr::QueueFrameRecord(rec);  // tags the Present this draw ends with
            g_secondEyeDraw = eye == 1, g_firstEyeDraw = eye == 0;
            const double t0 = PerfNowMs();
            o_VpDraw(viewport, present);
            eyeMs[eye] = PerfNowMs() - t0;
            g_secondEyeDraw = g_firstEyeDraw = false;
        }
        if (config::Get().vrPerfLog) {  // telemetry: the whole frame (draw to draw) and its parts
            static double s_last = 0;
            const double now = PerfNowMs();
            if (s_last > 0 && now - s_last < 1000.0) xr::PerfGame(now - s_last, now - s_last - eyeMs[0] - eyeMs[1], eyeMs[0], eyeMs[1]);
            s_last = now;
        }
    } else if (ok) {  // mode 1: mono head-tracked
        g_cur.valid = true;
        g_cur.rel = hs.rel;
        g_cur.raw = hs.raw;
        SetFrustum(hs.fov, 2);
        xr::FrameRecord rec{-1, 1, g_cur.raw, g_cur.halfH, g_cur.halfV, g_frameId, {hs.fov[0], hs.fov[1]}};
        xr::QueueFrameRecord(rec);
        o_VpDraw(viewport, present);
    } else {
        g_cur.valid = false;
        xr::FrameRecord rec{-1, 0, {}, 0, 0, g_frameId};
        xr::QueueFrameRecord(rec);
        o_VpDraw(viewport, present);
    }
    g_cur.valid = ok;  // keep the last pose for other CalcSceneView callers (e.g. cursor deprojection)
    if (ok && g_frameId % 900 == 1) {
        uint32_t lag = 0;
        Read(Addr(0x304E68C), lag);
        LOG("[vr] frame %llu mode %d: FOV %.1f deg (half H %.1f V %.1f), composed %llu, oneFrameLag=%u", g_frameId,
            mode, g_cur.fovDeg, g_cur.halfH * 57.2958f, g_cur.halfV * 57.2958f, g_composed, lag);
    }
}

void* __fastcall hk_Csv(void* lp, void* family, void* loc, void* rot, void* viewport, void* drawer) {
    ++t_inCsv;
    void* view = o_Csv(lp, family, loc, rot, viewport, drawer);
    --t_inCsv;
    return view;
}

// Diagnostic: deprojection calls (cursor -> world ray)
using DeprojFn = void(__fastcall*)(void*, const float*, float*, float*);
DeprojFn o_Deproj = nullptr;
std::atomic<int> g_deprojLogs{0};
}  // namespace
void ResetDeprojLog() { g_deprojLogs = 0; }
namespace {
// GG2 script deprojects the sight cursor via Canvas.DeProject. The cursor lives in the pixels of the 16:9
// HUD band ("show all" layout: width vw, height vw*9/16 for a tall view, range measured 0..3640 x 0..2047 at
// 3640x3782) but without the band's centring offset, while the sight is drawn at cursor + offset (measured
// with pinned cursors: (480,270)->(472,1136), (3000,1900)->(3000,~2760)). Add the offset so shots go where
// the sight is drawn. At a 16:9 view the offset is zero.
constexpr uint32_t kCanvasExecDeProject = 0xA48E60, kCanvasExecDeProjectEnd = 0xA48FA6;
// Stage 5: with the UI on the floating panel, the sight is seen on the panel, so the shot goes from the head
// through the sight's point on the panel. Panel and head are in the recentered tracking frame ("rel"); the
// direction is taken into the world with the game camera's axes, exactly like the head pose composition.
bool PanelAim(float vw, float vh, const float* screen, float* dir) {
    if (!g_aimValid || !ui::Enabled() || config::Get().vrMode != 2) return false;
    uint32_t x0, y0, bw, bh;
    ui::BandOf(uint32_t(vw + 0.5f), uint32_t(vh + 0.5f), x0, y0, bw, bh);
    if (!bw || !bh) return false;
    const float W = ui::PanelWidth(), H = W * float(bh) / float(bw), D = config::Get().vrUiDistance;
    const float u = screen[0] / float(bw), v = screen[1] / float(bh);
    const float z = g_zoom.load();  // the panel shows the view magnified z times about its centre
    const auto& hp = g_aimHead.position;
    V3 d{(u - 0.5f) * W / z - hp.x, (0.5f - v) * H / z + ui::PanelYOffset() - hp.y, -D - hp.z};  // XR axes, rel frame
    const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    if (len < 1e-4f) return false;
    d = d * (1.f / len);
    const V3 w = ToWorld(AxesOf(g_aimGameRot), XrToUe(d));
    dir[0] = w.x, dir[1] = w.y, dir[2] = w.z;
    return true;
}

void __fastcall hk_Deproj(void* view, const float* screen, float* origin, float* dir) {
    const uint32_t caller = ToRva(reinterpret_cast<uintptr_t>(_ReturnAddress()));
    {  // diag: each distinct caller once (a zoomed shot may deproject through another path)
        static uint32_t s_callers[16]{};
        static int s_n = 0;
        bool seen = false;
        for (int i = 0; i < s_n; ++i) seen |= s_callers[i] == caller;
        if (!seen && s_n < 16) s_callers[s_n++] = caller, LOG("[vr] deproject caller rva 0x%X (zoom %.2f)", caller, g_zoom.load());
    }
    float vw = 0, vh = 0;
    Read(uintptr_t(view) + 0x64, vw);  // FSceneView SizeX/SizeY (floats)
    Read(uintptr_t(view) + 0x68, vh);
    if (vw > 0 && vh > 0) {
        uint32_t bx, by;
        ui::BandOf(uint32_t(vw + 0.5f), uint32_t(vh + 0.5f), bx, by, g_lastBandW, g_lastBandH);
        g_lastViewW = vw, g_lastViewH = vh;
    }
    if (caller >= kCanvasExecDeProject && caller < kCanvasExecDeProjectEnd && vw > 0 && vh > 0) {
        o_Deproj(view, screen, origin, dir);  // origin (and the fallback direction)
        ctrl::Aim aim;
        if (config::Get().vrControllers && config::Get().vrMode == 2 && ctrl::GetAim(aim)) {
            // Stage 7: the shot goes along the aim ray (AimRayRel), in the game camera's frame
            V3 ro, rd;
            AimRayRel(aim, ro, rd);
            const Axes game = AxesOf(g_aimGameRot);
            const V3 w = ToWorld(game, XrToUe(rd));
            const V3 org = g_aimGameLoc + ToWorld(game, XrToUe(ro) * config::Get().vrWorldScale);
            if (g_deprojLogs.fetch_add(1) < 4)
                LOG("[vr] controller aim -> origin (%.0f,%.0f,%.0f) dir (%.3f,%.3f,%.3f) (cursor %.0f,%.0f)", org.x, org.y,
                    org.z, w.x, w.y, w.z, screen[0], screen[1]);
            origin[0] = org.x, origin[1] = org.y, origin[2] = org.z;
            dir[0] = w.x, dir[1] = w.y, dir[2] = w.z;
            return;
        }
        float pd[3];
        if (PanelAim(vw, vh, screen, pd)) {
            if (g_deprojLogs.fetch_add(1) < 4)
                LOG("[vr] panel aim: cursor (%.1f,%.1f) view %.0fx%.0f -> dir (%.3f,%.3f,%.3f) (screen ray (%.3f,%.3f,%.3f))",
                    screen[0], screen[1], vw, vh, pd[0], pd[1], pd[2], dir[0], dir[1], dir[2]);
            memcpy(dir, pd, sizeof pd);
            return;
        }
    }
    const float gw = float(config::Get().vrScreenResW), gh = float(config::Get().vrScreenResH);
    if (caller >= kCanvasExecDeProject && caller < kCanvasExecDeProjectEnd && vw > 0 && vh > 0 &&
        (std::fabs(vw - gw) > 0.5f || std::fabs(vh - gh) > 0.5f) && config::Get().vrRemapCursor) {
        const float sc = std::fmin(vw / gw, vh / gh);
        const float mapped[2] = {screen[0] + 0.5f * (vw - gw * sc), screen[1] + 0.5f * (vh - gh * sc)};
        if (g_deprojLogs.fetch_add(1) < 4)
            LOG("[vr] cursor deprojection remapped (%.1f,%.1f) -> (%.1f,%.1f) for view %.0fx%.0f", screen[0], screen[1],
                mapped[0], mapped[1], vw, vh);
        o_Deproj(view, mapped, origin, dir);
        return;
    }
    o_Deproj(view, screen, origin, dir);
    static std::atomic<int> s_nativeLogs{0};
    if (!(caller >= kCanvasExecDeProject && caller < kCanvasExecDeProjectEnd) && s_nativeLogs.fetch_add(1) < 12)
        LOG("[vr.diag] native deproject (%.1f,%.1f) view %.0fx%.0f -> dir (%.3f,%.3f,%.3f) caller %X", screen[0], screen[1],
            vw, vh, dir[0], dir[1], dir[2], caller);
}
const uint8_t kDeprojBytes[] = {0x48, 0x8B, 0xC4, 0x55, 0x53, 0x56, 0x57, 0x48, 0x8D, 0x68, 0xB8, 0x48};

// Weak-spot pop-ups (and Doki Doki target-select name tags): GG2ShootingHud.ConvertDimension, run in each eye's draw, shows one only
// if its point is in front of the GAME camera - Dot(Vector(spc.Rotation), Normal(loc - spc.Location)) > 0. With the head
// turned to a girl beside / behind the game camera, hers never showed (0.9.111, 2F Stairway: Ruriko, Yukina).
// UObject::execDot_VectorVector is reimplemented for calls from ConvertDimension during an eye draw: the operand equal to
// the controller's facing is replaced by the drawing eye's forward; every other call runs the original.
constexpr uint32_t kGNatives = 0x3144990;  // GNatives[] (execDot steps its operands through it; 0x41 = the debug check)
const uint8_t kDotBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48};
using DotExecFn = void(__fastcall*)(void*, void*, void*);  // UObject::exec* (this, FFrame&, Result)
DotExecFn o_Dot = nullptr;
uintptr_t g_fnConvDim = 0;
void __fastcall hk_Dot(void* obj, void* stack, void* result) {
    uint8_t* fr = static_cast<uint8_t*>(stack);
    if (!g_fnConvDim || *reinterpret_cast<uintptr_t*>(fr + 0x14) != g_fnConvDim || !g_cur.valid || DrawingEye() < 0 ||
        !g_lastPc || !config::Get().vrHeadFacingTests)
        return o_Dot(obj, stack, result);
    // P_GET_VECTOR x2 + P_FINISH, exactly as the original (FFrame: Object +0x1C, Code +0x24)
    auto* natives = reinterpret_cast<DotExecFn*>(Addr(kGNatives));
    float v[2][4]{};
    for (int i = 0; i < 2; ++i) {
        uint8_t*& code = *reinterpret_cast<uint8_t**>(fr + 0x24);
        const uint8_t op = *code++;
        natives[op](*reinterpret_cast<void**>(fr + 0x1C), stack, v[i]);
    }
    {
        uint8_t*& code = *reinterpret_cast<uint8_t**>(fr + 0x24);
        ++code;
        if (*code == 0x41) {
            ++code;
            natives[0x41](*reinterpret_cast<void**>(fr + 0x1C), stack, nullptr);
        }
    }
    const V3 a{v[0][0], v[0][1], v[0][2]}, b{v[1][0], v[1][1], v[1][2]};
    float r = Dot(a, b);
    Rot pcRot{};
    if (Read(g_lastPc + 0x8C, pcRot)) {  // Actor.Rotation (after Location at +0x80)
        const V3 face = AxesOf(pcRot).x;
        auto isFacing = [&](const V3& u) {
            const float l2 = Dot(u, u);
            return l2 > 0.98f && l2 < 1.02f && Dot(u, face) > 0.999f;
        };
        const auto& o = g_cur.rel.orientation;
        const V3 eyeFwd = ToWorld(AxesOf(g_aimGameRot), XrToUe(Rotate(Q4{o.x, o.y, o.z, o.w}, {0, 0, -1})));
        const bool fa = isFacing(a), fb = !fa && isFacing(b);
        if (fa || fb) {
            const float r2 = fa ? Dot(eyeFwd, b) : Dot(a, eyeFwd);
            static int s_logs = 0;
            if ((r > 0.f) != (r2 > 0.f) && s_logs < 20)
                ++s_logs, LOG("[vr] HUD facing test from the eye: %.2f -> %.2f (eye %d)", r, r2, DrawingEye());
            r = r2;
        }
    }
    *static_cast<float*>(result) = r;
}

// Diagnostic (shout attacks): UCanvas::execProject(FFrame&, Result) -> logs the canvas size and the result.
using ExecFn3 = void(__fastcall*)(void*, void*, void*);
ExecFn3 o_CanvasProject = nullptr;
std::atomic<int> g_projLogs{0};
}  // namespace
void ResetProjectLog() { g_projLogs = 0; }
namespace {
// Shout attacks: GG2MojiShell.CheckInCamera tests Canvas.Project(Location) against a hard-coded 1920x1080
// screen (plus a margin). In VR the canvas is the eye view (e.g. 3640x3782), so most of the view counted as
// off-screen. For calls from that function only, rescale the projected point from the canvas size to 1920x1080.
uintptr_t g_fnCheckInCamera = 0;
std::atomic<int> g_nodeLogs{0};
void __fastcall hk_CanvasProject(void* canvas, void* stack, void* result) {
    o_CanvasProject(canvas, stack, result);
    {  // the eye-view size, also in scenes that never deproject the shooting cursor (minigames: the HUD projections
       // here are their only screen work) - HudProjectionToPanel needs it
        int32_t cw = 0, ch = 0;
        Read(uintptr_t(canvas) + 0x90, cw), Read(uintptr_t(canvas) + 0x94, ch);  // Canvas.SizeX/SizeY
        if (cw > 0 && ch > 0 && config::Get().vrMode == 2 && (float(cw) != g_lastViewW || float(ch) != g_lastViewH)) {
            uint32_t bx, by;
            ui::BandOf(uint32_t(cw), uint32_t(ch), bx, by, g_lastBandW, g_lastBandH);
            g_lastViewW = float(cw), g_lastViewH = float(ch);
        }
    }
    // FFrame.Node (the executing UFunction): its offset is found once by scanning the frame for a pointer to a
    // Function object (the FOutputDevice base layout is not certain in this build).
    static int s_nodeOff = -1, s_scans = 0;
    if (s_nodeOff < 0 && s_scans < 50) {
        ++s_scans;
        static const uintptr_t s_fnClass = ue3::FindObject("Core.Function", "Class");
        for (int off = 8; off < 0x80; off += 4) {  // UE3 packs structs on 4 bytes
            uintptr_t p = 0;
            Read(uintptr_t(stack) + off, p);
            if (ue3::IsObject(p) && ue3::ClassOf(p) == s_fnClass) {
                s_nodeOff = off;
                LOG("[vr] FFrame.Node found at +0x%X (%s)", off, ue3::PathName(p).c_str());
                break;
            }
        }
        if (s_nodeOff < 0 && s_scans == 50) LOG("[vr] FFrame.Node NOT found: shout visibility fix inactive");
    }
    uintptr_t node = 0;
    if (s_nodeOff >= 0) Read(uintptr_t(stack) + s_nodeOff, node);
    {  // diag: every distinct script function that projects through the canvas
        static std::mutex s_mu;
        static std::vector<uintptr_t> s_seen;
        std::lock_guard l(s_mu);
        if (node && s_seen.size() < 40 && std::find(s_seen.begin(), s_seen.end(), node) == s_seen.end()) {
            s_seen.push_back(node);
            LOG("[vr.diag] Canvas.Project caller #%zu: %s", s_seen.size(), ue3::IsObject(node) ? ue3::PathName(node).c_str() : "?");
        }
    }
    // (removed 1.0: re-expressing GG2Hud.conv3DTo2D projections in panel space, and squeezing GG2ShootingHud.IsInWindow -
    // both broke stages; HUD projections stay in eye-view space, matching the eye-view cursor)
    if (!g_fnCheckInCamera || node != g_fnCheckInCamera || !config::Get().vrFixShoutVisibility) return;
    int32_t sx = 0, sy = 0;
    Read(uintptr_t(canvas) + 0x90, sx), Read(uintptr_t(canvas) + 0x94, sy);  // Canvas.SizeX/SizeY
    if (sx <= 0 || sy <= 0 || (sx == 1920 && sy == 1080)) return;
    auto* r = static_cast<float*>(result);
    const float x = r[0], y = r[1];
    r[0] = x * 1920.f / float(sx), r[1] = y * 1080.f / float(sy);
    if (g_projLogs.fetch_add(1) < 6)
        LOG("[vr] shout visibility: projected (%.0f,%.0f) on %dx%d -> (%.0f,%.0f) in 1920x1080", x, y, sx, sy, r[0], r[1]);
}
const uint8_t kCanvasProjectBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48};

// GG2StandardPlayerController.GetSightCursorScreenPositionY (native: returns the cursor's y, PC+0xBA8). The boss attack
// markers ("!" dolls; IsHitBossMarkerCircle, all bosses) compare it with HUD projections (GG2Hud.conv3DTo2D), which are
// full eye-view (canvas) pixels; the cursor is HUD-band pixels (the band's top offset subtracted). Test: aimed straight
// at a marker, the two differed by exactly that offset (867 px at 3640x3782), so no doll could be shot (0.9.20).
// In VR the returned y gets the offset back. Only the boss targeting (and a voice-line check) read it.
ExecFn3 o_SightCursorY = nullptr;
void __fastcall hk_SightCursorY(void* pc, void* stack, void* result) {
    o_SightCursorY(pc, stack, result);
    if (config::Get().vrMode != 2 || !SceneIsImmersive() || g_lastViewW <= 0 || g_lastViewH <= 0) return;
    uint32_t bx, by, bw, bh;
    ui::BandOf(uint32_t(g_lastViewW + 0.5f), uint32_t(g_lastViewH + 0.5f), bx, by, bw, bh);
    if (!by) return;
    *static_cast<float*>(result) += float(by);
    static int s_logs = 0;
    if (s_logs++ < 3) LOG("[vr] sight cursor y for screen-space hit tests: +%u (band offset) -> %.0f", by, *static_cast<float*>(result));
}
const uint8_t kSightCursorYBytes[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0xFF};

// The engine's frame smoothing (GameEngine.bSmoothFrameRate, MaxSmoothedFrameRate 62 in the game's default config) caps
// the game below a 72+ Hz headset: frames alternated one and two display slots (0.9.113 log: ~60 fps, 1.14 slots),
// and its sleeps fight the headset's own pacing (xrWaitFrame). Turned off in memory while stereo VR runs, back after.
void EngineSmoothingTick(bool vrOn) {
    static ue3::PropInfo pi;
    static int s_ok = -1;
    static uintptr_t s_eng = 0;
    static ULONGLONG s_find = 0;
    static bool s_cleared = false;
    if (s_ok < 0) {
        const uintptr_t c = ue3::FindObject("Engine.Engine", "Class");
        s_ok = c && ue3::FindProperty(c, "bSmoothFrameRate", pi) && pi.bitMask ? 1 : 0;
        LOG("[vr] Engine.bSmoothFrameRate %s", s_ok ? "found" : "NOT FOUND (frame smoothing left alone)");
    }
    if (s_ok != 1) return;
    if (!s_eng || !ue3::IsObject(s_eng)) {
        s_eng = 0;
        if (GetTickCount64() < s_find) return;
        s_find = GetTickCount64() + 2000;
        for (int32_t i = 0, n = ue3::NumObjects(); i < n && !s_eng; ++i) {
            const uintptr_t o = ue3::ObjectAt(i);
            if (o && ue3::IsA(o, "GameEngine") && ue3::PathName(o).find("Default__") == std::string::npos) s_eng = o;
        }
        if (!s_eng) return;
    }
    uint32_t v = 0;
    if (!Read(s_eng + pi.offset, v)) return;
    const bool want = vrOn && !config::Get().vrFrameSmoothing;
    if (want && (v & pi.bitMask)) {
        *reinterpret_cast<uint32_t*>(s_eng + pi.offset) = v & ~pi.bitMask;
        s_cleared = true;
        LOG("[vr] engine frame smoothing off while VR runs (the headset paces the frames)");
    } else if (!want && s_cleared && !(v & pi.bitMask)) {
        *reinterpret_cast<uint32_t*>(s_eng + pi.offset) = v | pi.bitMask;
        s_cleared = false;
        LOG("[vr] engine frame smoothing back on");
    }
}

// Hidden minigame icons wander around their spot by a 2D offset in screen units (m_DispPosOffset, e.g. Shinobu's: a
// circle of 150 * 0.7 px) added to the spot's 3D projection. In VR those units span the headset's much wider view, so
// the circles came out ~1.6x larger around her body - and as fast, to cover them (0.9.22: hard to hit). Each
// fresh offset (the actors recompute it every tick) is scaled by tan(game FOV / 2) / tan(eye FOV / 2) before the HUD
// adds it (in its draw, just after this).
void ScaleAeIconOffsets() {
    static ue3::PropInfo pOff;
    static int s_ok = -1;
    if (s_ok < 0) {
        const uintptr_t c = ue3::FindObject("GG2Game.GG2ActionEventBaseMarkerInvisibleActor", "Class");
        s_ok = c && ue3::FindProperty(c, "m_DispPosOffset", pOff);
        if (!s_ok) LOG("[vr] minigame icon offsets: m_DispPosOffset not found");
    }
    const float fov = g_gameFovDeg.load();
    const float k = fov > 10.f && fov < 170.f && g_cur.halfH > 0.f ? std::tan(fov * kPi / 360.f) / std::tan(g_cur.halfH) : 1.f;
    {
        static int s_why = 0;
        if (s_why < 3 && (s_ok != 1 || !g_cur.valid || k >= 0.999f))
            ++s_why, LOG("[vr] minigame icon wander not scaled: props %d, view %d, game FOV %.1f, eye half %.1f", s_ok, g_cur.valid, fov,
                         g_cur.halfH * 57.2958f);
    }
    if (s_ok != 1 || !g_cur.valid || k >= 0.999f) return;
    struct Icon { uintptr_t obj; float written[2]; };
    static std::vector<Icon> s_icons;
    static ULONGLONG s_scan = 0;
    // the live hidden-icon actors: refreshed twice a second, and at once when the HUD places a new icon - a new one
    // wandered unscaled (~2x too far) until the next refresh, then snapped into place (0.9.110: icons "fly by out
    // of place before correcting themselves")
    if (GetTickCount64() >= s_scan || ui::TakeAeNewIcon()) {
        s_scan = GetTickCount64() + 500;
        std::vector<Icon> now;
        for (int32_t i = 0, n = ue3::NumObjects(); i < n; ++i) {
            const uintptr_t o = ue3::ObjectAt(i);
            if (!o || !ue3::IsA(o, "GG2ActionEventBaseMarkerInvisibleActor") || ue3::PathName(o).find("Default__") != std::string::npos) continue;
            Icon ic{o, {1e30f, 1e30f}};
            for (auto& old : s_icons)
                if (old.obj == o) ic = old;
            now.push_back(ic);
        }
        s_icons.swap(now);
    }
    for (auto& ic : s_icons) {
        float off[2];
        if (!game::Read(ic.obj + pOff.offset, off) || (off[0] == ic.written[0] && off[1] == ic.written[1])) continue;
        {  // check: the largest fresh (unscaled) offset over 5 s - shrinking would mean scaling twice
            static float s_max = 0;
            static ULONGLONG s_next = 0;
            static int s_n = 0;
            s_max = std::fmax(s_max, std::hypot(off[0], off[1]));
            if (GetTickCount64() > s_next) {
                if (s_next && s_n++ < 6) LOG("[vr] minigame icon wander: largest offset %.0f -> %.0f (x%.2f)", s_max, s_max * k, k);
                s_next = GetTickCount64() + 5000, s_max = 0;
            }
        }
        off[0] *= k, off[1] *= k;  // fresh this tick: scale once
        memcpy(reinterpret_cast<void*>(ic.obj + pOff.offset), off, sizeof off);
        ic.written[0] = off[0], ic.written[1] = off[1];
    }
    static int s_logs = 0;
    if (s_logs++ == 0) LOG("[vr] minigame icon wander scaled x%.2f (game FOV %.0f, eye %.0f)", k, fov, g_cur.halfH * 360.f / kPi);
}

// Patches FloatConst values (token 1E + float) in a script function's bytecode, in order: occurrence i of `from`
// becomes to[i] (fewer values: the rest keep the last). UStruct::Script is a TArray<BYTE> (4-byte aligned: +0x8C).
int PatchScriptFloats(const char* path, float from, const float* to, int nTo) {
    const uintptr_t fn = ue3::FindObject(path, "Function");
    if (!fn || nTo < 1) return -1;
    uint8_t pat[5] = {0x1E};
    memcpy(pat + 1, &from, 4);
    for (uint32_t o = 0x30; o < 0x180; o += 4) {
        uintptr_t data = 0;
        int32_t num[2]{};
        if (!game::Read(fn + o, data) || !game::Read(fn + o + 8, num) || !data || num[0] < 16 || num[0] > 4096 || num[1] < num[0])
            continue;
        static uint8_t code[4096];
        if (!game::SafeRead(reinterpret_cast<const void*>(data), code, size_t(num[0]))) continue;
        int k = 0;
        for (int i = 0; i + 5 <= num[0]; ++i)
            if (!memcmp(code + i, pat, 5)) {
                memcpy(reinterpret_cast<void*>(data + i + 1), &to[k < nTo ? k : nTo - 1], 4);
                ++k, i += 4;
            }
        if (k) return k;
    }
    return 0;
}
// Minigame bytecode fixes, once (the script package stays loaded):
//  - AEIsInScreen skips any request whose marker projects outside 0..1920 x 0..1080 (the flat screen). In VR the
//    projection is of the taller eye image (~2000 high): requests on her lower body were never detected (0.9.21).
//  - AEDetectInputMarkerShotPhase gives every request detector distance 65536 (a touch anywhere counts). In VR you
//    aim at the request instead (0.9.22): minigame_request_radius (outer) and 40% of it (full accuracy), in the
//    eye-view projection's units. Order there: poke (worst, best), rub (worst, best), flick (start), hold (worst, best).
void PatchAeScript() {
    static bool s_done = false;
    if (s_done) return;
    s_done = true;
    const float big = 100000.f;
    LOG("[vr] AEIsInScreen: y limit 1080 raised (%d constant(s))", PatchScriptFloats("GG2Game.GG2ActionEventBasePlayerInput.AEIsInScreen", 1080.f, &big, 1));
    const float r = config::Get().vrAeRequestRadius;
    if (r > 0.f) {
        const float b = r * 0.4f, seq[7] = {r, b, r, b, r, r, b};
        LOG("[vr] minigame requests: aim within %.0f units (%d constants)", r,
            PatchScriptFloats("GG2Game.GG2ActionEventBasePlayerInput.AEDetectInputMarkerShotPhase", 65536.f, seq, 7));
    }
}

// Minigame touches (GG2ActionEvent: requests - poke, rub, hold - and the gesture phases) are read from GG2PlayerInput's
// touch table (+0x948, 0x44 per touch: state 1 began / 2 held / 3 ended, location +0x18 in the minigames' 3D-projection
// stage units) through these virtuals. With our inputs the game never fills it (test: the mouse emulation stays idle), so
// in minigames touch 0 is the pointer: held trigger = touching, at the 3D projection of what is seen through the pointer.
// The device type reads as mouse there so the scripts take the table path (it only steers touch input in minigames).
// (0.9.21: the request icons - rub / poke / hold - couldn't be done)
using TouchBoolFn = uint32_t(__fastcall*)(uintptr_t, int32_t);
using TouchLocFn = float*(__fastcall*)(uintptr_t, float*, int32_t, int32_t);
using DevFn = uint32_t(__fastcall*)(uintptr_t);
TouchBoolFn o_TouchBegan = nullptr, o_TouchHeld = nullptr, o_TouchEnded = nullptr;
TouchLocFn o_TouchLoc = nullptr, o_TouchOldLoc = nullptr;
DevFn o_DevType = nullptr;
// The pointer as the minigames' touch 0 (and 1), latched once per game tick: the state from the trigger's presses /
// releases since the last tick (a tap shorter than a tick still begins on one tick and ends on the next), the
// position from the pointer. Two-finger gestures (the "skinship" phase: both hands dragged up / down together): the
// game's mouse emulation makes two touches from one mouse (GG2PlayerInput_MouseTouchEmulationParameter_Dual_*: fixed
// left / right X), whose height starts at a reset position on each press and then follows the mouse's relative motion,
// in flat-screen coordinates checked against fixed screen rectangles - the same here, from the pointer's vertical
// motion on the panel (stage units = flat-screen units). Flick requests: the motion since the press counts
// minigame_flick_gain times (the game wants 800 units in 0.5 s - a 40-50 degree swing in the headset's wider view).
struct TickTouch {
    double tick = -1;
    int state = -1;               // 1 began, 2 held, 3 ended, 0 none, -1 not in use
    uint32_t presses = 0, releases = 0;
    bool endPending = false;      // a whole tap fell between two ticks: it ends on the next
    float x = 0, y = 0, px = 0, py = 0;  // touch 0 now / last tick (single: eye-view projection; dual: screen)
    float pressX = 0, pressY = 0, pressPanelX = 0, pressPanelY = 0;
    bool stickHeld = false;  // the touch is driven by a thumbstick
    bool stickWas = false;
    uint32_t stickPresses = 0, stickReleases = 0;  // folded into the trigger / grip counters
    bool pressOnPanel = false;
    bool dual = false;
    float xl = 0, xr = 0, dualY = 0, prevDualY = 0, lastPanelY = 0;
    bool havePanel = false;
};
TickTouch g_tt;
double GameTick() {  // WorldInfo.RealTimeSeconds: changes once per game tick
    static ue3::PropInfo pWi, pRt;
    static int s_ok = -1;
    if (s_ok < 0) {
        const uintptr_t a = ue3::FindObject("Engine.Actor", "Class"), w = ue3::FindObject("Engine.WorldInfo", "Class");
        s_ok = a && w && ue3::FindProperty(a, "WorldInfo", pWi) && ue3::FindProperty(w, "RealTimeSeconds", pRt);
        if (!s_ok) LOG("[vr] WorldInfo.RealTimeSeconds not found: minigame touches latched by time");
    }
    uintptr_t wi = 0;
    float t = 0;
    if (s_ok == 1 && g_lastPc && game::Read(g_lastPc + pWi.offset, wi) && wi && game::Read(wi + pRt.offset, t)) return t;
    return double(GetTickCount64() / 8);
}
void LatchDual(uintptr_t pi) {
    static ue3::PropInfo pParam, pReset, pResetOn, pEnable, pL, pR;
    static int s_ok = -1;
    if (s_ok < 0) {
        const uintptr_t c = ue3::FindObject("GG2Game.GG2PlayerInput", "Class");
        const uintptr_t d = ue3::FindObject("GG2Game.GG2PlayerInput_MouseTouchEmulationParameter_Dual_Common_ParallelHorizontal", "Class");
        s_ok = c && d && ue3::FindProperty(c, "m_MouseTouchEmulationParameter", pParam) &&
               ue3::FindProperty(c, "m_ResetMouseTouchPosition", pReset) &&
               ue3::FindProperty(c, "m_bEnableAdjustMouseTouchPosition_ResetPosition", pResetOn) &&
               ue3::FindProperty(c, "m_bEnableMouseTouchEmulation", pEnable) && ue3::FindProperty(d, "m_TouchEmulationPositionX_Left", pL) &&
               ue3::FindProperty(d, "m_TouchEmulationPositionX_Right", pR);
        LOG("[vr] two-finger gesture emulation %s", s_ok ? "ready" : "unavailable (properties not found)");
    }
    const bool was = g_tt.dual;
    g_tt.dual = false;
    uintptr_t param = 0;
    uint32_t en = 0, resetOn = 0;
    if (s_ok != 1 || !pi || !game::Read(pi + pParam.offset, param) || !param ||
        !ue3::IsA(param, "GG2PlayerInput_MouseTouchEmulationParameter_Dual_Common_ParallelHorizontal") ||
        !game::Read(pi + pEnable.offset, en) || !(en & pEnable.bitMask)) {
        if (was) LOG("[vr] two-finger gesture emulation off");
        g_tt.havePanel = false;
        return;
    }
    game::Read(pi + pResetOn.offset, resetOn);
    float reset[2]{};
    game::Read(pi + pReset.offset, reset);
    game::Read(param + pL.offset, g_tt.xl), game::Read(param + pR.offset, g_tt.xr);
    float px, py;
    const bool onPanel = pointer::PanelStage(px, py);
    const float dy = onPanel && g_tt.havePanel ? py - g_tt.lastPanelY : 0.f;
    g_tt.havePanel = onPanel, g_tt.lastPanelY = py;
    g_tt.prevDualY = g_tt.dualY;
    if (g_tt.stickHeld) {  // a thumbstick: the height from its tilt (up phases: neutral = bottom, down: top)
        float ds[2], os[2];
        ctrl::Sticks(ds, os);
        const float sy = std::fabs(ds[1]) >= std::fabs(os[1]) ? ds[1] : os[1];
        const int type = AeSkinShipType();
        g_tt.dualY = type == 0 ? 540.f - std::fmax(0.f, sy) * 1080.f : type == 1 ? -540.f + std::fmax(0.f, -sy) * 1080.f : -sy * 540.f;
    } else if (!was || (g_tt.state == 1 && (resetOn & pResetOn.bitMask))) g_tt.dualY = g_tt.prevDualY = reset[1];
    else g_tt.dualY = std::fmax(-540.f, std::fmin(540.f, g_tt.dualY + dy));
    g_tt.dual = true;
    if (!was) LOG("[vr] two-finger gesture emulation on: X %.0f / %.0f, start height %.0f (reset %s)", g_tt.xl, g_tt.xr, reset[1],
                  (resetOn & pResetOn.bitMask) ? "on each press" : "off");
}
void LatchTouch() {
    const double tick = GameTick();
    if (tick == g_tt.tick) return;
    g_tt.tick = tick;
    float tx, ty;
    uintptr_t pi = 0;
    if (!config::Get().vrAeMinigameFix || !pointer::TouchStage(tx, ty) || !g_lastPc || !game::Read(g_lastPc + 0x554, pi) || !pi) {
        bool down;
        g_tt.state = -1, g_tt.dual = false, g_tt.endPending = false, g_tt.havePanel = false;
        pointer::TouchCounts(g_tt.presses, g_tt.releases, down);  // presses from outside minigames don't count
        g_tt.presses += g_tt.stickPresses, g_tt.releases += g_tt.stickReleases, g_tt.stickWas = false;
        return;
    }
    uint32_t p, r;
    bool down;
    pointer::TouchCounts(p, r, down);
    // gesture phases: the thumbsticks too, like the game's controller guides (0.9.29): a deflected stick is a
    // held touch, moved by the stick (circles / scratch: the dominant stick; two-hand drags: either stick)
    float ds[2], os[2];
    ctrl::Sticks(ds, os);
    const bool gesture = ui::GesturePhase();
    const float dmag = std::hypot(ds[0], ds[1]), omag = std::hypot(os[0], os[1]);
    // starts past 0.35 tilt; once going, it stays a touch while tilted past 0.08 or within 300 ms of the last strong
    // tilt - a left-right wiggle passes the centre without ending the touch (0.9.31: scratch by stick was hard)
    static ULONGLONG s_strong = 0;
    const float smag = std::fmax(dmag, omag);
    if (smag > 0.25f) s_strong = GetTickCount64();
    const bool stickOn = gesture && config::Get().vrAeControllerGuide;
    const bool stick = stickOn && (smag > 0.35f || (g_tt.stickWas && (smag > 0.08f || GetTickCount64() - s_strong < 300)));
    if (stick && !g_tt.stickWas && !down) ++g_tt.stickPresses;
    if (!stick && g_tt.stickWas && !down) ++g_tt.stickReleases;
    g_tt.stickWas = stick;
    p += g_tt.stickPresses, r += g_tt.stickReleases, down = down || stick;
    // reveal mode: a touch can only START while the pointer's circle covers the request; once touching it may leave
    // the circle (a flick does at once - 0.9.28: the swipe request couldn't be done)
    const bool touching = (g_tt.state == 1 || g_tt.state == 2) && down;
    if (!ui::GesturePhase() && !ui::AeRequestRevealed() && !touching) {
        g_tt.state = -1, g_tt.dual = false, g_tt.endPending = false;
        g_tt.presses = p, g_tt.releases = r;
        return;
    }
    const bool pressed = p != g_tt.presses, released = r != g_tt.releases;
    const int last = g_tt.state;
    if (g_tt.endPending) g_tt.state = 3, g_tt.endPending = false;
    else if (pressed) g_tt.state = 1, g_tt.endPending = released && !down;
    else if (released && (last == 1 || last == 2)) g_tt.state = 3;
    else g_tt.state = down ? 2 : 0;
    g_tt.presses = p, g_tt.releases = r;
    float panX = 0, panY = 0;
    const bool onPanel = pointer::PanelStage(panX, panY);
    g_tt.stickHeld = stick;
    if (g_tt.state == 1) g_tt.pressX = tx, g_tt.pressY = ty, g_tt.pressPanelX = panX, g_tt.pressPanelY = panY, g_tt.pressOnPanel = onPanel;
    LatchDual(pi);
    g_tt.px = g_tt.x, g_tt.py = g_tt.y;
    const float gain = config::Get().vrAeFlickGain;
    // while held, the motion since the press counts in panel units: the panel is the screen, so a rub / circle /
    // flick on it counts as the same stroke on the flat game's screen (the eye-view projection the game compares in
    // is wider - a panel unit is ~0.6 of its units - so rubs counted for little and circles came out small).
    // Flick requests: minigame_flick_gain times that (the game wants 800 units in 0.5 s).
    const int gtype = gesture ? AeSkinShipType() : -1;
    float gRadius = 0.f, gPos[2]{};
    const bool gParams = gesture && AeSkinShipParams(gRadius, gPos);
    if (gtype == 5 && gParams && !g_tt.dual) {  // taps: on the target (the game's controller mode needs no aiming)
        tx = gPos[0], ty = gPos[1];
    } else if (g_tt.stickHeld && !g_tt.dual && g_tt.state >= 1) {
        // stick: around where the touch began; circles at exactly the game's radius whatever the tilt (only the
        // direction counts), scratch with a long stroke (its progress is the distance moved) (0.9.30 / 0.9.31)
        const bool circle = gtype == 3 || gtype == 4;
        const float rr = circle ? (gRadius > 10.f ? gRadius : 200.f) : gtype == 6 ? 450.f : 250.f;
        float sx = ds[0], sy = ds[1];
        const float m = std::hypot(sx, sy);
        if (circle && m > 0.05f) sx /= m, sy /= m;
        tx = g_tt.pressX + sx * rr, ty = g_tt.pressY - sy * rr;
        // circles: the stick sets the direction of turn; while it clearly spins, the touch turns at a speed inside
        // the game's scoring window (too fast / too slow scored nothing - 0.9.32: stick felt sluggish)
        static float s_out = 0.f, s_in = 0.f;
        static double s_t = 0.0;
        float B = 0, best = 0, worst = 0;
        if (circle && AeCircleSpeed(B, best, worst) && B > 0.f) {
            const float a = std::atan2(-sy, sx);  // screen angle (y down)
            const double dt = tick - s_t;
            if (g_tt.state == 1 || dt <= 0.0 || dt > 0.2) {
                s_out = s_in = a;
            } else {
                float da = a - s_in;
                while (da > 3.14159265f) da -= 6.2831853f;
                while (da < -3.14159265f) da += 6.2831853f;
                // the game's angle unit: degrees when the border is large (a rad/s border of 20+ is ~3 turns a second)
                const float k = B > 20.f ? 57.2958f : 1.f;
                const float w = std::fabs(da) / float(dt) * k;  // the stick's speed, game units
                const float half = best > 0.f ? best * 0.8f : worst * 0.4f;
                float wo = w;
                if (w >= B * 0.3f) wo = std::fmax(B - half, std::fmin(B + half, w));
                s_out += (da < 0 ? -wo : wo) / k * float(dt);
                s_in = a;
            }
            tx = g_tt.pressX + std::cos(s_out) * rr, ty = g_tt.pressY + std::sin(s_out) * rr;
            static int s_logs = 0;
            if (g_tt.state == 1 && s_logs++ < 4) LOG("[vr] circle (stick): radius %.0f speed window %.1f +-%.1f (best +-%.1f)", rr, B, worst, best);
        }
        s_t = tick;
    } else if (!g_tt.dual && g_tt.state == 2 && onPanel && g_tt.pressOnPanel) {
        const float g = ui::FlickRequestUp() && !ui::GesturePhase() && gain > 0.f ? gain : 1.f;
        tx = g_tt.pressX + (panX - g_tt.pressPanelX) * g, ty = g_tt.pressY + (panY - g_tt.pressPanelY) * g;
    }
    // circles by hand: as before (the game tracks the circle's centre itself), but a circle bigger than the game's
    // size (m_TouchRadius_TouchPad) is shrunk uniformly toward it - the path stays a circle, so the game's centre
    // tracking still works, and a tight circle is unchanged (0.9.32: projecting onto the motion's own centre made it worse)
    {
        constexpr int kH = 256;  // ~1 s of game ticks
        static float s_hx[kH], s_hy[kH];
        static double s_ht[kH];
        static int s_hn = 0;
        static float s_rawX = 0, s_rawY = 0, s_outX = 0, s_outY = 0, s_k = 1.f;
        const bool circleHand = (gtype == 3 || gtype == 4) && !g_tt.stickHeld && !g_tt.dual && g_tt.state == 2 && gRadius > 10.f;
        if (!circleHand) {
            s_hn = 0, s_k = 1.f;
            s_rawX = s_outX = tx, s_rawY = s_outY = ty;
        } else {
            s_hx[s_hn % kH] = tx, s_hy[s_hn % kH] = ty, s_ht[s_hn % kH] = tick, ++s_hn;
            float cx = 0, cy = 0, r2 = 0;
            int k = 0;
            for (int i = 0; i < std::min(s_hn, kH); ++i)
                if (tick - s_ht[i] < 0.8) cx += s_hx[i], cy += s_hy[i], ++k;
            float want = 1.f;
            if (k >= 8) {
                cx /= k, cy /= k;
                for (int i = 0; i < std::min(s_hn, kH); ++i)
                    if (tick - s_ht[i] < 0.8) r2 += (s_hx[i] - cx) * (s_hx[i] - cx) + (s_hy[i] - cy) * (s_hy[i] - cy);
                const float r = std::sqrt(r2 / k);  // a circle's RMS distance from its centre = its radius
                if (r > gRadius) want = std::fmax(0.25f, gRadius / r);
            }
            s_k += (want - s_k) * 0.1f;  // smoothly: a sudden change would look like a jump to the game
            s_outX += (tx - s_rawX) * s_k, s_outY += (ty - s_rawY) * s_k;
            s_rawX = tx, s_rawY = ty;
            tx = s_outX, ty = s_outY;
        }
    }
    g_tt.x = tx, g_tt.y = ty;
    static int s_logs = 0;
    if (g_tt.state != last && g_tt.state > 0 && s_logs++ < 60)
        LOG("[vr] minigame touch: state %d at (%.0f,%.0f)%s", g_tt.state, tx, ty, g_tt.dual ? " (two fingers)" : "");
}
int PointerTouch(int32_t no, float* x = nullptr, float* y = nullptr, float* px = nullptr, float* py = nullptr) {
    LatchTouch();
    if (g_tt.state < 0 || no < 0 || no > (g_tt.dual ? 1 : 0)) return -1;
    if (g_tt.dual) {  // two fingers: screen coordinates, fixed X, the shared height
        const float fx = no == 0 ? g_tt.xl : g_tt.xr;
        if (x) *x = fx;
        if (y) *y = 540.f + g_tt.dualY;
        if (px) *px = fx;
        if (py) *py = 540.f + g_tt.prevDualY;
    } else {
        if (x) *x = g_tt.x;
        if (y) *y = g_tt.y;
        if (px) *px = g_tt.px;
        if (py) *py = g_tt.py;
    }
    return g_tt.state;
}
uint32_t __fastcall hk_TouchBegan(uintptr_t pi, int32_t no) {
    const int s = PointerTouch(no);
    return s < 0 ? o_TouchBegan(pi, no) : s == 1;
}
uint32_t __fastcall hk_TouchHeld(uintptr_t pi, int32_t no) {
    const int s = PointerTouch(no);
    return s < 0 ? o_TouchHeld(pi, no) : s == 1 || s == 2;
}
uint32_t __fastcall hk_TouchEnded(uintptr_t pi, int32_t no) {
    const int s = PointerTouch(no);
    return s < 0 ? o_TouchEnded(pi, no) : s == 3;
}
float* __fastcall hk_TouchLoc(uintptr_t pi, float* out, int32_t no, int32_t a) {
    float x, y;
    if (PointerTouch(no, &x, &y) < 0) return o_TouchLoc(pi, out, no, a);
    out[0] = x, out[1] = y;
    return out;
}
float* __fastcall hk_TouchOldLoc(uintptr_t pi, float* out, int32_t no, int32_t flag) {
    float px, py;
    if (PointerTouch(no, nullptr, nullptr, &px, &py) < 0) return o_TouchOldLoc(pi, out, no, flag);
    out[0] = px, out[1] = py;
    return out;
}
bool PointerTouchActive() {
    float x, y;
    return config::Get().vrAeMinigameFix && pointer::TouchStage(x, y);
}
// GetCurrentControlDeviceType (0 mouse, 1 controller): IsCurrentControlDeviceType_Mouse / _Controller go through it
uint32_t __fastcall hk_DevType(uintptr_t pi) { return PointerTouchActive() ? 0 : o_DevType(pi); }
const uint8_t kTouchBeganBytes[] = {0x33, 0xC0, 0x4C, 0x63, 0xC2, 0x4D, 0x6B, 0xC0, 0x44, 0x41, 0x83, 0xBC};
const uint8_t kTouchHeldBytes[] = {0x48, 0x63, 0xC2, 0x48, 0x6B, 0xC0, 0x44, 0x8B, 0x94, 0x08, 0x48, 0x09};
const uint8_t kTouchLocBytes[] = {0x49, 0x63, 0xC0, 0x48, 0x6B, 0xC0, 0x44, 0x48, 0x8B, 0x84, 0x08, 0x60};
const uint8_t kTouchOldLocBytes[] = {0x49, 0x63, 0xC0, 0x48, 0x6B, 0xC0, 0x44, 0x45, 0x85, 0xC9, 0x74, 0x0F};
const uint8_t kDevTypeBytes[] = {0x0F, 0xB6, 0x81, 0x9D, 0x05, 0x00, 0x00, 0xC3};

// Diagnostic: who calls UGameViewportClient::Draw.
using DrawFn = void(__fastcall*)(void*, void*, void*);
DrawFn o_VcDraw = nullptr;
int g_drawLogs = 0;
void __fastcall hk_VcDraw(void* self, void* viewport, void* canvas) {
    if (g_drawLogs < 1) {
        ++g_drawLogs;
        LOG("[vr.diag] UGameViewportClient::Draw(self=%p, viewport=%p) thread=%lu stack: %s", self, viewport,
            GetCurrentThreadId(), StackRvas(1, 16).c_str());
    }
    o_VcDraw(self, viewport, canvas);
}
const uint8_t kVcDrawBytes[] = {0x48, 0x8B, 0xC4, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56};

// Fantasize (Collection "WildFancy") zoom: the game narrows its FOV, which VR overrides. Instead the camera moves
// toward the girl so she appears as large as the game's zoom would make her: forward by dist * (1 - 1/zoom),
// where dist is the camera-girl distance measured while unzoomed; it stops ~80 cm short of her.
float NearestGirlAhead(const V3& loc, const Rot& rot, uintptr_t* girlOut = nullptr) {
    const Axes ax = AxesOf(rot);
    float best = 0;
    uintptr_t bestGirl = 0;
    std::string cls;
    for (int32_t i = 0, n = ue3::NumObjects(); i < n; ++i) {
        const uintptr_t o = ue3::ObjectAt(i);
        if (!o || !(ue3::IsA(o, "GG2Girl") || ue3::IsA(o, "GG2DokiSpGirl")) || ue3::PathName(o).find("Default__") != std::string::npos)
            continue;
        V3 l;
        Read(o + 0x80, l);
        const V3 d{l.x - loc.x, l.y - loc.y, l.z - loc.z};
        const float f = d.x * ax.x.x + d.y * ax.x.y + d.z * ax.x.z, dist = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        if (f > 20.f && f > 0.7f * dist && dist < 1500.f && (best == 0 || f < best))
            best = f, bestGirl = o, cls = ue3::ObjName(ue3::ClassOf(o));
    }
    if (girlOut) *girlOut = bestGirl;
    static int s_logs = 0;
    if (best > 0 && s_logs < 3) ++s_logs, LOG("[vr] Fantasize: girl (%s) %.0f uu ahead", cls.c_str(), best);
    return best;
}
// Final Doki Doki Mode: holding the shot zooms by narrowing the FOV (GG2DokiSpBasePlayerController.GetZoomedFov =
// base - m_zoomRateCurrent * m_zoomFovDelta), which VR overrides (0.9.50: it didn't zoom). Only that zoom part
// (not the camera animations' own FOV) becomes a move toward the selected girl, as for Fantasize.
float DokiSpDolly(const V3& loc, const Rot& rot) {
    static ue3::PropInfo pRate, pDelta;
    static int s_ok = -1;
    static uintptr_t s_girl = 0;
    if (!g_inFinalDoki.load() || !g_lastPc || !ue3::IsObject(g_lastPc) || !ue3::IsA(g_lastPc, "GG2DokiSpBasePlayerController")) {
        s_girl = 0;
        return 0;
    }
    if (s_ok < 0) {
        const uintptr_t c = ue3::FindObject("GG2Game.GG2DokiSpBasePlayerController", "Class");
        s_ok = c && ue3::FindProperty(c, "m_zoomRateCurrent", pRate) && ue3::FindProperty(c, "m_zoomFovDelta", pDelta);
        if (!s_ok) LOG("[vr] final Doki Doki zoom: properties not found");
    }
    float rate = 0, delta = 0;
    if (s_ok != 1 || !Read(g_lastPc + pRate.offset, rate) || !Read(g_lastPc + pDelta.offset, delta)) return 0;
    const float fov = g_gameFovDeg.load(), zoomDeg = rate * delta;
    if (fov <= 1.f || zoomDeg <= 0.01f) return 0;
    const float z = std::tan(std::fmin(170.f, fov + zoomDeg) * kPi / 360.f) / std::tan(fov * kPi / 360.f);
    if (!s_girl || !ue3::IsObject(s_girl)) NearestGirlAhead(loc, rot, &s_girl);
    if (!s_girl) return 0;
    V3 g;
    Read(s_girl + 0x80, g);
    const Axes ax = AxesOf(rot);
    const float dist = (g.x - loc.x) * ax.x.x + (g.y - loc.y) * ax.x.y + (g.z - loc.z) * ax.x.z;
    if (dist <= 40.f || z <= 1.001f) return 0;
    static int s_logs = 0;
    if (z > 1.3f && s_logs < 6)
        ++s_logs, LOG("[vr] final Doki Doki zoom: x%.2f (FOV %.1f + %.1f), girl %.0f uu ahead -> %.0f uu forward", z, fov, zoomDeg, dist,
                      dist * (1.f - 1.f / z));
    return std::fmax(0.f, std::fmin(dist * (1.f - 1.f / z), dist - 35.f));
}
float FantasizeDolly(const V3& loc, const Rot& rot) {
    // the zoom is measured against the widest FOV since Fantasize opened; the distance is the live distance from
    // the game's own camera to the girl, so the camera can never be moved into her (re-zooming used to creep closer)
    static float s_base = 0;
    static bool s_was = false;
    static uintptr_t s_girl = 0;
    if (!config::Get().vrFantasizeDolly || !ui::FantasizeActive()) {
        s_was = false, s_girl = 0;
        return 0;
    }
    const float fov = g_gameFovDeg.load();
    if (!s_was) s_was = true, s_base = fov;
    if (fov > s_base) s_base = fov;
    // the Collection camera's own unzoomed FOV (GG2CollectionPlayerController.InitFovNum): the zoom is
    // m_cameraZoom -> SetFOV relative to it
    float initFov = 0;
    if (g_lastPc && ue3::IsObject(g_lastPc) && ue3::IsA(g_lastPc, "GG2CollectionPlayerController") &&
        Read(g_lastPc + 0xB10, initFov) && initFov > 5.f && initFov < 170.f)
        s_base = initFov;
    if (!s_girl || !ue3::IsObject(s_girl) || !ue3::IsA(s_girl, "GG2Girl")) NearestGirlAhead(loc, rot, &s_girl);
    if (!s_girl) return 0;
    V3 g;
    Read(s_girl + 0x80, g);
    const Axes ax = AxesOf(rot);
    const float dist = (g.x - loc.x) * ax.x.x + (g.y - loc.y) * ax.x.y + (g.z - loc.z) * ax.x.z;  // along the view
    const float z = (fov > 1.f && s_base > 1.f) ? std::tan(s_base * kPi / 360.f) / std::tan(fov * kPi / 360.f) : 1.f;
    if (dist <= 40.f || z <= 1.001f) return 0;
    static int s_logs = 0;
    if (z > 1.5f && s_logs < 6) ++s_logs, LOG("[vr] Fantasize dolly: zoom %.2f, girl %.0f uu ahead -> %.0f uu forward", z, dist, dist * (1.f - 1.f / z));
    return std::fmax(0.f, std::fmin(dist * (1.f - 1.f / z), dist - 35.f));
}


uint32_t g_offActorLoc = 0, g_offCyl = 0, g_offCollH = 0;
// PlayerController.GetPlayerViewPoint(out vector out_Location, out rotator out_Rotation)
float g_lastCamZ = 0;
ULONGLONG g_lastCamLog = 0;
// ---- line checks (test: !tracetest) ----
bool TraceHit(uintptr_t actor, const V3& from, const V3& to, bool actors, V3& hitOut, uintptr_t* hitActor);
uintptr_t g_traceActor = 0;  // dev: the PlayerController the traces run on
// UWorld::SingleLineCheck, called the way AActor::execTrace calls it (Actor.Trace through ProcessEvent found nothing
// in testing, even straight through a blocking collision cylinder). FCheckResult (from execTrace): +0x08 Actor,
// +0x10 Location, +0x1C Normal, +0x28 Time (1 = no hit). Flags: execTrace's 0x2086 (world) / 0x20BF (with actors).
bool TraceHit(uintptr_t actor, const V3& from, const V3& to, bool actors, V3& hitOut, uintptr_t* hitActor) {
    uintptr_t world = 0;
    if (!Read(game::Addr(game::rva::GWorld), world) || !world) return false;
    alignas(16) uint8_t hit[0x100]{};
    const float one = 1.f;
    memcpy(hit + 0x28, &one, 4);
    const int32_t none = -1;
    memcpy(hit + 0x2C, &none, 4), memcpy(hit + 0x60, &none, 4);
    const V3 extent{0, 0, 0};
    using SlcFn = uint32_t(__fastcall*)(uintptr_t, void*, uintptr_t, const V3*, const V3*, uint32_t, const V3*, void*);
    const auto slc = reinterpret_cast<SlcFn>(game::Addr(game::rva::UWorld_SingleLineCheck));
    slc(world, hit, actor, &to, &from, actors ? 0x20BFu : 0x2086u, &extent, nullptr);
    uintptr_t who = 0;
    float t = 1.f;
    memcpy(&who, hit + 0x08, 8), memcpy(&t, hit + 0x28, 4);
    if (hitActor) *hitActor = who;
    if (!who && t >= 1.f) return false;
    memcpy(&hitOut, hit + 0x10, sizeof hitOut);
    return true;
}
// Story game over (GG2GameOverGameInfo): the girl stands 150 uu in front of a camera only 100 uu above her feet
// (framing her whole body: in VR the player felt very short - 0.9.9). The camera is raised to a stage's standing
// height (~130 uu, around her head: 122 uu) and moved a little closer, on the line between them.
bool g_goMoved = false;  // GameOverCamera moved the camera g_goFrom -> g_goTo (the girl's gaze follows it)
V3 g_goFrom{}, g_goTo{};
void GameOverCamera(V3& loc) {
    g_goMoved = false;
    static uintptr_t s_girl = 0;
    static ULONGLONG s_next = 0;
    if (g_lastGameClass.find("GG2GameOverGameInfo") == std::string::npos || !g_offActorLoc) {
        s_girl = 0;
        return;
    }
    const float wantH = config::Get().vrGameOverHeight, wantD = config::Get().vrGameOverDistance;
    if (wantH <= 0.f && wantD <= 0.f) return;
    if (s_girl && !ue3::IsObject(s_girl)) s_girl = 0;
    if (!s_girl && GetTickCount64() >= s_next) {
        s_next = GetTickCount64() + 500;
        for (int32_t i = 0, n = ue3::NumObjects(); i < n && !s_girl; ++i) {
            const uintptr_t o = ue3::ObjectAt(i);
            if (o && ue3::IsA(o, "GG2GameOverGirl") && ue3::PathName(o).find("Default__") == std::string::npos) s_girl = o;
        }
    }
    if (!s_girl) return;
    V3 g{};
    Read(s_girl + g_offActorLoc, g);
    uintptr_t cyl = 0;
    float collH = 0;
    if (g_offCyl && Read(s_girl + g_offCyl, cyl) && cyl) Read(cyl + g_offCollH, collH);
    const float feet = g.z - collH, hx = loc.x - g.x, hy = loc.y - g.y, len = std::sqrt(hx * hx + hy * hy);
    if (len < 40.f || len > 600.f || loc.z - feet < 20.f || loc.z - feet > 300.f) return;  // not the usual framing
    const V3 was = loc;
    if (wantH > 0.f) loc.z = feet + wantH;
    if (wantD > 0.f) loc.x = g.x + hx / len * wantD, loc.y = g.y + hy / len * wantD;
    g_goMoved = true, g_goFrom = was, g_goTo = loc;
    static uintptr_t s_logged = 0;
    if (s_logged != s_girl)
        s_logged = s_girl, LOG("[vr] game over: camera (%.0f,%.0f,%.0f) -> (%.0f,%.0f,%.0f): %.0f uu above her feet, %.0f uu from her",
                               was.x, was.y, was.z, loc.x, loc.y, loc.z, loc.z - feet, wantD > 0.f ? wantD : len);
}
V3 ShakeOffset(uintptr_t pc, Rot rot, bool force, V3 loc, int* yawCorr, int* pitchCorr);  // below
// dev: !shakediag <s> - the game's own camera shake fields next to the view point it reports (camera-shake comfort option)
std::atomic<ULONGLONG> g_shakeDiagUntil{0};
void LogShakeDiag(uintptr_t pc, const void* parms) {
    static ULONGLONG s_t = 0;
    const ULONGLONG now = GetTickCount64();
    if (now > g_shakeDiagUntil.load() || now - s_t < 100) return;
    s_t = now;
    const uintptr_t cls = ue3::ClassOf(pc);
    auto f = [&](const char* n, float* out, int cnt) {
        ue3::PropInfo pi;
        if (!ue3::FindProperty(cls, n, pi)) return false;
        for (int i = 0; i < cnt; ++i) Read(pc + pi.offset + 4 * i, out[i]);
        return true;
    };
    float cp = 0, cv[3]{}, mp = 0, mv[3]{}, mr = 0;
    const bool a = f("m_camShakePower", &cp, 1) && f("m_camShakeValue", cv, 3);
    const bool b = f("m_moveCamShakePower", &mp, 1) && f("m_moveCamShakeValue", mv, 3) && f("m_moveCamShakeRate", &mr, 1);
    const auto* p = static_cast<const uint8_t*>(parms);
    V3 loc;
    Rot rot;
    memcpy(&loc, p + g_offVpLoc, sizeof loc);
    memcpy(&rot, p + g_offVpRot, sizeof rot);
    int yc = 0, pcr = 0;
    const V3 off = ShakeOffset(pc, rot, true, loc, &yc, &pcr);
    LOG("[shake] %s view (%.1f,%.1f,%.1f) rot (%d,%d,%d) | cam %s power %.3f value (%.3f,%.3f,%.3f) | move %s power %.3f value (%.3f,%.3f,%.3f) rate %.3f"
        " | clean (%.1f,%.1f,%.1f) rot (%d,%d)",
        ue3::ObjName(pc).c_str(), loc.x, loc.y, loc.z, rot.pitch, rot.yaw, rot.roll, a ? "" : "(n/a)", cp, cv[0], cv[1], cv[2],
        b ? "" : "(n/a)", mp, mv[0], mv[1], mv[2], mr, loc.x - off.x, loc.y - off.y, loc.z - off.z, rot.pitch + pcr, rot.yaw + yc);
}
void ShakeDiagImpl(int seconds) {
    g_shakeDiagUntil = GetTickCount64() + ULONGLONG(seconds) * 1000;
    LOG("[shake] diagnostic for %d s", seconds);
}
// Camera-shake comfort options (0.9.83): the game moves the camera by its shake values, location offsets in the camera's
// own axes (x forward, y right, z up; dev: a hit's value (0,-1.5,-2.1) moved the view exactly 1.5 left, 2.1 down).
// Subtracted exactly - no smoothing, no lag - before the head pose is added.
// The camera also looks at its pose's eye target (GG2ShootingPlayerController.m_currentPoseParam.eyeTargetLocation),
// so a shake turns the view too (test: 0.13 deg for 2 uu; 0.9.83 "reduced, not exactly removed"): the turn
// look-at(shaken -> target) vs look-at(clean -> target) is returned in rotCorr (UE units) - any other turning is kept.
V3 ShakeOffset(uintptr_t pc, Rot rot, bool force, V3 loc, int* yawCorr, int* pitchCorr) {
    const auto& c = config::Get();
    if (yawCorr) *yawCorr = 0;
    if (pitchCorr) *pitchCorr = 0;
    const bool hit = force || c.vrRemoveHitShake, walk = force || c.vrRemoveWalkBob;
    if (!hit && !walk) return {};
    static uintptr_t s_cls = 0;
    static int s_offCam = -1, s_offMove = -1, s_offPose = -1;
    const uintptr_t cls = ue3::ClassOf(pc);
    if (cls != s_cls) {
        s_cls = cls, s_offCam = s_offMove = s_offPose = -1;
        ue3::PropInfo pi;
        if (ue3::FindProperty(cls, "m_camShakeValue", pi)) s_offCam = int(pi.offset);
        if (ue3::FindProperty(cls, "m_moveCamShakeValue", pi)) s_offMove = int(pi.offset);
        if (ue3::FindProperty(cls, "m_currentPoseParam", pi) && pi.type.find("poseParameters") != std::string::npos) s_offPose = int(pi.offset);
        LOG("[vr] camera shake fields on %s: hit %s, walk %s, eye target %s", ue3::ObjName(pc).c_str(), s_offCam >= 0 ? "found" : "none",
            s_offMove >= 0 ? "found" : "none", s_offPose >= 0 ? "found" : "none");
    }
    V3 v{};
    V3 t{};
    if (hit && s_offCam >= 0 && Read(pc + s_offCam, t)) v = v + t;
    if (walk && s_offMove >= 0 && Read(pc + s_offMove, t)) v = v + t;
    if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z) || std::fabs(v.x) + std::fabs(v.y) + std::fabs(v.z) > 200.f)
        return {};
    const V3 off = ToWorld(AxesOf(rot), v);
    V3 tgt{};
    if (yawCorr && pitchCorr && s_offPose >= 0 && (v.x != 0.f || v.y != 0.f || v.z != 0.f) && Read(pc + s_offPose + 0xC, tgt) &&
        std::isfinite(tgt.x) && std::isfinite(tgt.y) && std::isfinite(tgt.z)) {
        const V3 a = tgt - loc, b = tgt - (loc - off);
        const float la = std::sqrt(Dot(a, a)), lb = std::sqrt(Dot(b, b));
        if (la > 50.f && lb > 50.f) {
            auto yawOf = [](V3 d) { return std::atan2(d.y, d.x); };
            auto pitchOf = [](V3 d) { return std::atan2(d.z, std::sqrt(d.x * d.x + d.y * d.y)); };
            float dy = yawOf(b) - yawOf(a), dp = pitchOf(b) - pitchOf(a);
            if (dy > kPi) dy -= 2.f * kPi;
            if (dy < -kPi) dy += 2.f * kPi;
            const float k = 32768.f / kPi;  // radians -> UE rotation units
            if (std::fabs(dy) < 0.05f && std::fabs(dp) < 0.05f) *yawCorr = int(std::lround(dy * k)), *pitchCorr = int(std::lround(dp * k));
        }
    }
    return off;
}
void PostGetPlayerViewPoint(uintptr_t self, void* parms) {
    LogShakeDiag(self, parms);  // (any mode: the shake fields don't depend on VR)
    if (!t_inCsv || !g_cur.valid) return;
    auto* p = static_cast<uint8_t*>(parms);
    V3 loc;
    Rot rot;
    memcpy(&loc, p + g_offVpLoc, sizeof loc);
    memcpy(&rot, p + g_offVpRot, sizeof rot);
    // Feedback guard: the game camera can hand back the view composed by the previous call as its own pose (test: in
    // the zoom scope draw the second GetPlayerViewPoint returned the first call's result, so the aim turn was applied
    // twice - the scope looked 50 deg left for a 25 deg aim; the Dressing Room camera did the same with the head
    // pose). Input equal to our last output is replaced by the camera pose that output was composed from.
    static V3 s_outLoc{}, s_baseLoc{};
    static Rot s_outRot{}, s_baseRot{};
    static bool s_haveOut = false;
    if (s_haveOut && std::fabs(loc.x - s_outLoc.x) + std::fabs(loc.y - s_outLoc.y) + std::fabs(loc.z - s_outLoc.z) < 0.05f &&
        std::abs(rot.pitch - s_outRot.pitch) <= 1 && std::abs(rot.yaw - s_outRot.yaw) <= 1 && std::abs(rot.roll - s_outRot.roll) <= 1) {
        static int s_fbLogs = 0;
        if (s_fbLogs++ < 5 && (s_baseRot.yaw != rot.yaw || s_baseRot.pitch != rot.pitch))
            LOG("[vr] camera fed back our view (rot %d,%d,%d); using its pose (%d,%d,%d)", rot.pitch, rot.yaw, rot.roll,
                s_baseRot.pitch, s_baseRot.yaw, s_baseRot.roll);
        loc = s_baseLoc, rot = s_baseRot;
    }
    s_baseLoc = loc, s_baseRot = rot;
    if (g_lrScene) {  // Dressing Room: the pinned camera pose (+ the move toward the girl)
        if (!g_lrPin.set) {
            g_lrPin.set = true, g_lrPin.loc = loc, g_lrPin.rot = rot;
            LOG("[vr] dressing room: camera pinned at (%.0f,%.0f,%.0f) rot (%d,%d,%d)", loc.x, loc.y, loc.z, rot.pitch, rot.yaw, rot.roll);
        }
        rot = g_lrPin.rot;
        loc = g_lrPin.loc + DressingDollyOffset(rot);
    }
    {  // remove_hit_shake / remove_walk_bob: the offset and the turn it caused
        int yc = 0, pc = 0;
        const V3 off = ShakeOffset(self, rot, false, loc, &yc, &pc);
        loc = loc - off;
        rot.yaw += yc, rot.pitch += pc;
    }
    GameOverCamera(loc);
    g_lastCamZ = loc.z;
    g_traceActor = self;
    g_aimGameRot = rot;
    g_aimGameLoc = loc;
    const Axes game = AxesOf(rot);
    const auto& o = g_cur.rel.orientation;
    const Q4 q{o.x, o.y, o.z, o.w};
    const V3 fwd = ToWorld(game, XrToUe(Rotate(q, {0, 0, -1})));
    const V3 up = ToWorld(game, XrToUe(Rotate(q, {0, 1, 0})));
    const auto& pp = g_cur.rel.position;
    const V3 offLocal = XrToUe({pp.x, pp.y, pp.z}) * config::Get().vrWorldScale;
    const V3 newLoc = loc + ToWorld(game, offLocal) + game.x * (FantasizeDolly(loc, rot) + DokiSpDolly(loc, rot));
    const Rot newRot = RotFromAxes(fwd, up);
    memcpy(p + g_offVpLoc, &newLoc, sizeof newLoc);
    memcpy(p + g_offVpRot, &newRot, sizeof newRot);
    s_outLoc = newLoc, s_outRot = newRot, s_haveOut = true;
    {  // diag: the composed view of the zoom scope draw vs the eye draws
        static ULONGLONG s_t = 0;
        static int s_n = 0;
        if (g_zoom.load() > 1.5f && s_n < 24 && (g_scopeDraw || GetTickCount64() - s_t < 50)) {
            if (g_scopeDraw) s_t = GetTickCount64();
            ++s_n;
            const auto& o = g_cur.rel.orientation;
            LOG("[vr.scopediag] %s game rot (%d,%d,%d) -> view rot (%d,%d,%d) loc (%.0f,%.0f,%.0f) q(%.3f,%.3f,%.3f,%.3f)",
                g_scopeDraw ? "SCOPE" : "eye  ", rot.pitch, rot.yaw, rot.roll, newRot.pitch, newRot.yaw, newRot.roll, newLoc.x,
                newLoc.y, newLoc.z, o.x, o.y, o.z, o.w);
        }
    }
    ++g_composed;
}

// PlayerController.GetFOVAngle() -> float
void PostGetFOVAngle(uintptr_t, void* parms) {
    if (!t_inCsv || !g_cur.valid) return;
    float game = 0;
    memcpy(&game, static_cast<uint8_t*>(parms) + g_offFovRet, sizeof game);
    if (std::fabs(game - g_cur.fovDeg) < 0.01f) return;  // our own value cached by the camera
    if (g_scopeDraw || std::fabs(game - g_scopeFovDeg) < 0.01f) {  // the zoom scope render's FOV is ours too
        memcpy(static_cast<uint8_t*>(parms) + g_offFovRet, &g_cur.fovDeg, sizeof(float));
        return;
    }
    const float prev = g_gameFovDeg.exchange(game);
    static int s_fovLogs = 0;
    if (std::fabs(prev - game) > 0.5f && s_fovLogs < 40) {
        ++s_fovLogs;
        LOG("[vr.diag] game FOV %.2f -> %.2f deg", prev, game);
    }
    memcpy(static_cast<uint8_t*>(parms) + g_offFovRet, &g_cur.fovDeg, sizeof(float));
}

// Diagnostic: a possessed girl's devil aura was created (GG2Girl.CreateDevilAuraMesh event) -> dump the stereo
// pair a moment later (the aura renders with artifacts in VR).
void PostCreateDevilAuraMesh(uintptr_t self, void*) {
    LOG("[vr.diag] devil aura created on %s", ue3::ObjName(self).c_str());
    if (!g_auraDumpAt) g_auraDumpAt = GetTickCount64() + 1500;
}

// World-scale probe: GG2Girl.GetHeadLocation() vs the pawn's feet.
ULONGLONG g_lastScaleLog = 0;
int g_scaleLogs = 0;
// GG2Girl.GetFaceTargetLocation(out FaceTargetLocation, out EyesTargetLocation, ...): where her face and eyes aim (the
// native gaze code asks the script each tick). The script answers with the game's own camera spot, so on the story game
// over screen (camera moved by GameOverCamera) she looked below the player - 0.9.10. Targets at the game's camera
// spot move with it, plus gameover_gaze_offset in height (aimed exactly at the camera her head tipped back and she
// looked slightly above the player - 0.9.11).
uint32_t g_offFaceTgt = 0, g_offEyesTgt = 0;
void PostGetFaceTargetLocation(uintptr_t self, void* parms) {
    if (!g_goMoved) return;
    auto* p = static_cast<uint8_t*>(parms);
    for (const uint32_t off : {g_offFaceTgt, g_offEyesTgt}) {
        V3 t{};
        memcpy(&t, p + off, sizeof t);
        const V3 d = t - g_goFrom;
        if (Dot(d, d) > 40.f * 40.f) continue;  // aimed somewhere else
        t = t + (g_goTo - g_goFrom);
        t.z += config::Get().vrGameOverGazeOffset;
        memcpy(p + off, &t, sizeof t);
    }
    static uintptr_t s_logged = 0;
    if (s_logged != self) {
        s_logged = self;
        V3 f{};
        memcpy(&f, p + g_offFaceTgt, sizeof f);
        LOG("[vr] game over: %s looks at (%.0f,%.0f,%.0f) (the moved camera)", ue3::ObjName(self).c_str(), f.x, f.y, f.z);
    }
}
void PostGetHeadLocation(uintptr_t self, void* parms) {
    if (g_scaleLogs >= 30 || GetTickCount64() - g_lastScaleLog < 2000) return;
    g_lastScaleLog = GetTickCount64();
    ++g_scaleLogs;
    V3 head, loc;
    memcpy(&head, static_cast<uint8_t*>(parms) + g_offHeadRet, sizeof head);
    Read(self + g_offActorLoc, loc);
    uintptr_t cyl = 0;
    float collH = 0;
    if (Read(self + g_offCyl, cyl) && cyl) Read(cyl + g_offCollH, collH);
    const float feet = loc.z - collH, headAbove = head.z - feet, camAbove = g_lastCamZ - feet;
    LOG("[vr.scale] %s feet z=%.1f head %.1f uu above feet; player camera z=%.1f -> %.1f uu above her feet "
        "(%.1f uu/m if the player's eyes are 1.60 m high)",
        ue3::ObjName(self).c_str(), feet, headAbove, g_lastCamZ, camAbove, camAbove / 1.60f);
}
}  // namespace

void SetStereoVariant(int v) {
    g_stereoVariant = v;
    LOG("[vr] stereo variant = %d (%s)", v, v == 1 ? "SWAPPED eyes" : v == 2 ? "MONO (left eye pose)" : "normal");
}
void SetIpdScale(float k) {
    g_ipdScale = k;
    LOG("[vr] IPD scale = %.2f", k);
}

void SetFakeHead(int mode) {
    g_fakeHead = mode;
    LOG("[vr] debug fake head = %d", mode);
}

bool InstallHeadTracking() {
    g_fakeHead = config::Get().vrDebugFakeHead;
    const uintptr_t fnVp = ue3::FindObject("Engine.PlayerController.GetPlayerViewPoint", "Function");
    const uintptr_t fnFov = ue3::FindObject("Engine.PlayerController.GetFOVAngle", "Function");
    if (!fnVp || !fnFov) {
        LOG("[vr] GetPlayerViewPoint/GetFOVAngle not found; head tracking disabled");
        return false;
    }
    for (auto& q : ue3::Properties(fnVp)) {
        if (q.name == "out_Location") g_offVpLoc = q.offset;
        if (q.name == "out_Rotation") g_offVpRot = q.offset;
    }
    for (auto& q : ue3::Properties(fnFov))
        if (q.name == "ReturnValue") g_offFovRet = q.offset;
    LOG("[vr] GetPlayerViewPoint params: out_Location@0x%X out_Rotation@0x%X; GetFOVAngle ReturnValue@0x%X",
        g_offVpLoc, g_offVpRot, g_offFovRet);
    bool ok = hooks::Create("ULocalPlayer::CalcSceneView", kCalcSceneView, kCsvBytes, sizeof kCsvBytes,
                            (void*)&hk_Csv, (void**)&o_Csv);
    ok &= hooks::Create("FViewport::Draw", kViewportDraw, kVpDrawBytes, sizeof kVpDrawBytes, (void*)&hk_VpDraw,
                        (void**)&o_VpDraw);
    g_fnCheckInCamera = ue3::FindObject("GG2Game.GG2MojiShell.CheckInCamera", "Function");
    LOG("[vr] GG2MojiShell.CheckInCamera %s", g_fnCheckInCamera ? "found" : "NOT FOUND (shout visibility fix inactive)");
    hooks::Create("UGG2PlayerInput::IsTouchBegan", 0x16B2AD0, kTouchBeganBytes, sizeof kTouchBeganBytes, (void*)&hk_TouchBegan,
                  (void**)&o_TouchBegan);
    hooks::Create("UGG2PlayerInput::IsTouchHeld", 0x16B2AF0, kTouchHeldBytes, sizeof kTouchHeldBytes, (void*)&hk_TouchHeld,
                  (void**)&o_TouchHeld);
    hooks::Create("UGG2PlayerInput::IsTouchEnded", 0x16B2B20, kTouchBeganBytes, sizeof kTouchBeganBytes, (void*)&hk_TouchEnded,
                  (void**)&o_TouchEnded);
    hooks::Create("UGG2PlayerInput::TouchLocation", 0x16B2B40, kTouchLocBytes, sizeof kTouchLocBytes, (void*)&hk_TouchLoc,
                  (void**)&o_TouchLoc);
    hooks::Create("UGG2PlayerInput::TouchOldLocation", 0x16B2B60, kTouchOldLocBytes, sizeof kTouchOldLocBytes,
                  (void*)&hk_TouchOldLoc, (void**)&o_TouchOldLoc);
    hooks::Create("UGG2PlayerInput::GetCurrentControlDeviceType", 0x16B3220, kDevTypeBytes, sizeof kDevTypeBytes, (void*)&hk_DevType,
                  (void**)&o_DevType);
    hooks::Create("AGG2StandardPlayerController::execGetSightCursorScreenPositionY", 0x16B9F10, kSightCursorYBytes,
                  sizeof kSightCursorYBytes, (void*)&hk_SightCursorY, (void**)&o_SightCursorY);
    hooks::Create("UCanvas::execProject", 0xA48D20, kCanvasProjectBytes, sizeof kCanvasProjectBytes,
                  (void*)&hk_CanvasProject, (void**)&o_CanvasProject);
    g_fnConvDim = ue3::FindObject("GG2Game.GG2ShootingHud.ConvertDimension", "Function");
    LOG("[vr] GG2ShootingHud.ConvertDimension %s", g_fnConvDim ? "found (HUD facing tests from the eye)" : "NOT FOUND");
    if (g_fnConvDim)
        hooks::Create("UObject::execDot_VectorVector", 0x1A1890, kDotBytes, sizeof kDotBytes, (void*)&hk_Dot, (void**)&o_Dot);
    hooks::Create("FSceneView::Deproject", 0xAB67B0, kDeprojBytes, sizeof kDeprojBytes, (void*)&hk_Deproj,
                  (void**)&o_Deproj);
    {
        void* const det[] = {(void*)&hk_Edge<0>, (void*)&hk_Edge<1>, (void*)&hk_Edge<2>, (void*)&hk_Edge<3>, (void*)&hk_Edge<4>};
        for (int i = 0; i < 5; ++i)
            hooks::Create(g_edge[i].name, g_edge[i].rva, kEdgeBytes, sizeof kEdgeBytes, det[i], (void**)&g_edge[i].orig);
    }
    hooks::Create("UGameViewportClient::Draw(diag)", 0x7FA710, kVcDrawBytes, sizeof kVcDrawBytes, (void*)&hk_VcDraw,
                  (void**)&o_VcDraw);
    ok &= ue3::pe::RegisterPost(fnVp, &PostGetPlayerViewPoint);
    ue3::pe::SetSkip(&SkipInScopeDraw);
    ok &= ue3::pe::RegisterPost(fnFov, &PostGetFOVAngle);
    if (const uintptr_t fnAura = ue3::FindObject("GG2Game.GG2Girl.CreateDevilAuraMesh", "Function"))
        ue3::pe::RegisterPost(fnAura, &PostCreateDevilAuraMesh);
    const uintptr_t fnHead = ue3::FindObject("GG2Game.GG2Girl.GetHeadLocation", "Function");
    ue3::PropInfo pi;
    if (fnHead) {
        for (auto& q : ue3::Properties(fnHead))
            if (q.name == "ReturnValue") g_offHeadRet = q.offset;
        if (uintptr_t c = ue3::FindObject("Engine.Actor", "Class"); c && ue3::FindProperty(c, "Location", pi))
            g_offActorLoc = pi.offset;
        if (uintptr_t c = ue3::FindObject("Engine.Pawn", "Class"); c && ue3::FindProperty(c, "CylinderComponent", pi))
            g_offCyl = pi.offset;
        if (uintptr_t c = ue3::FindObject("Engine.CylinderComponent", "Class"); c && ue3::FindProperty(c, "CollisionHeight", pi))
            g_offCollH = pi.offset;
        if (g_offActorLoc && g_offCyl && g_offCollH) ue3::pe::RegisterPost(fnHead, &PostGetHeadLocation);
    }
    if (const uintptr_t fnFace = ue3::FindObject("GG2Game.GG2Girl.GetFaceTargetLocation", "Function")) {
        int found = 0;  // (FaceTargetLocation is at offset 0)
        for (auto& q : ue3::Properties(fnFace)) {
            if (q.name == "FaceTargetLocation") g_offFaceTgt = q.offset, ++found;
            else if (q.name == "EyesTargetLocation") g_offEyesTgt = q.offset, ++found;
        }
        if (found == 2) ue3::pe::RegisterPost(fnFace, &PostGetFaceTargetLocation);
    }
    // scene classification offsets
    if (uintptr_t c = ue3::FindObject("Engine.Engine", "Class"); c && ue3::FindProperty(c, "GamePlayers", pi)) g_offGamePlayers = pi.offset;
    if (uintptr_t c = ue3::FindObject("Engine.Player", "Class"); c && ue3::FindProperty(c, "Actor", pi)) g_offPlayerActor = pi.offset;
    if (uintptr_t c = ue3::FindObject("Engine.Actor", "Class"); c && ue3::FindProperty(c, "WorldInfo", pi)) g_offWorldInfo = pi.offset;
    if (uintptr_t c = ue3::FindObject("Engine.WorldInfo", "Class")) {
        if (ue3::FindProperty(c, "Game", pi)) g_offGame = pi.offset;
        if (ue3::FindProperty(c, "Pauser", pi)) g_offPauser = pi.offset;
    }
    if (uintptr_t c = ue3::FindObject("Engine.PlayerController", "Class"); c && ue3::FindProperty(c, "PlayerCamera", pi))
        g_offPlayerCamera = pi.offset;
    if (uintptr_t c = ue3::FindObject("Engine.Camera", "Class"); c && ue3::FindProperty(c, "bConstrainAspectRatio", pi))
        g_offConstrainAspect = pi.offset, g_maskConstrainAspect = pi.bitMask;
    LOG("[vr] PlayerController.PlayerCamera@0x%X Camera.bConstrainAspectRatio@0x%X mask 0x%X", g_offPlayerCamera,
        g_offConstrainAspect, g_maskConstrainAspect);
    LOG("[vr] scene offsets: GamePlayers@0x%X Player.Actor@0x%X Actor.WorldInfo@0x%X WorldInfo.Game@0x%X WorldInfo.Pauser@0x%X",
        g_offGamePlayers, g_offPlayerActor, g_offWorldInfo, g_offGame, g_offPauser);
    if (!InstallWindowHook()) LOG("[res] game window not found yet; will retry on first resolution change");
    ok &= ui::Install();
    ok &= hooks::EnableAll();
    LOG("[vr] head tracking %s (mode=%d: %s, world_scale=%.1f uu/m)", ok ? "installed" : "FAILED", config::Get().vrMode,
        config::Get().vrMode == 2 ? "stereo" : config::Get().vrMode == 1 ? "mono" : "screen", config::Get().vrWorldScale);
    return ok;
}
float ZoomFactor() { return g_zoom.load(); }
// A HUD element placed at a 3D projection (stage units = eye-image pixels * 1920/band width, no band offset) ->
// the stage position on the panel where that view direction crosses it.
// The inverse: a point on the panel (stage coords) -> the eye-view projection (stage coords, no band offset) of the
// direction from the eye through it - what the game's own 3D->2D projections (conv3DTo2D) give for the thing seen
// there. Minigame touches are compared with those (0.9.21: minigames).
bool PanelToHudProjection(float& sx, float& sy) {
    if (!g_cur.valid || !ui::Enabled() || config::Get().vrMode != 2 || g_lastViewW <= 0 || g_lastViewH <= 0) return false;
    uint32_t bx, by, bw, bh;
    ui::BandOf(uint32_t(g_lastViewW + 0.5f), uint32_t(g_lastViewH + 0.5f), bx, by, bw, bh);
    if (!bw || !bh) return false;
    const float k = float(bw) / 1920.f;
    const float D = config::Get().vrUiDistance, Wp = ui::PanelWidth(), Hp = Wp * float(bh) / float(bw);
    const float x = (sx / 1920.f - 0.5f) * Wp, y = (0.5f - sy / 1080.f) * Hp + ui::PanelYOffset();
    const auto& ep = g_cur.rel.position;
    const auto& o = g_cur.rel.orientation;
    const V3 d = Rotate(Q4{-o.x, -o.y, -o.z, o.w}, {x - ep.x, y - ep.y, -D - ep.z});
    if (d.z > -1e-3f) return false;
    const float tx = d.x / -d.z, ty = d.y / -d.z;
    const float ex = (tx / std::tan(g_cur.halfH) + 1.f) * 0.5f * g_lastViewW, ey = (1.f - ty / std::tan(g_cur.halfV)) * 0.5f * g_lastViewH;
    sx = ex / k, sy = ey / k;
    return true;
}
// The HUD band's top offset in the eye image, in stage units (eye-image pixels * 1920 / band width): 0 on a flat screen
float BandTopStage() {
    if (g_lastViewW <= 0 || g_lastViewH <= 0) return 0.f;
    uint32_t bx, by, bw, bh;
    ui::BandOf(uint32_t(g_lastViewW + 0.5f), uint32_t(g_lastViewH + 0.5f), bx, by, bw, bh);
    return bw ? float(by) * 1920.f / float(bw) : 0.f;
}
bool HudProjectionToPanel(float& sx, float& sy) {
    if (!g_cur.valid || !ui::Enabled() || config::Get().vrMode != 2 || g_lastViewW <= 0 || g_lastViewH <= 0) return false;
    uint32_t bx, by, bw, bh;
    ui::BandOf(uint32_t(g_lastViewW + 0.5f), uint32_t(g_lastViewH + 0.5f), bx, by, bw, bh);
    if (!bw || !bh) return false;
    const float k = float(bw) / 1920.f, ex = sx * k, ey = sy * k;
    const float tx = (2.f * ex / g_lastViewW - 1.f) * std::tan(g_cur.halfH);
    const float ty = (1.f - 2.f * ey / g_lastViewH) * std::tan(g_cur.halfV);
    const auto& o = g_cur.rel.orientation;
    const V3 d = Rotate(Q4{o.x, o.y, o.z, o.w}, {tx, ty, -1.f});
    if (d.z > -1e-3f) return false;
    const auto& ep = g_cur.rel.position;
    const float D = config::Get().vrUiDistance, Wp = ui::PanelWidth(), Hp = Wp * float(bh) / float(bw);
    const float t = (-D - ep.z) / d.z, x = ep.x + d.x * t, y = ep.y + d.y * t - ui::PanelYOffset();
    sx = (x / Wp + 0.5f) * float(bw) / k, sy = (0.5f - y / Hp) * float(bh) / k;
    return true;
}
unsigned long long DrawCount() { return g_drawCount.load(); }
void AimRayRaw(const XrPosef& aimRaw, XrVector3f& org, XrVector3f& dir) {
    const auto& o = aimRaw.orientation;
    const V3 f = Rotate(Q4{o.x, o.y, o.z, o.w}, {0, 0, -1});
    org = aimRaw.position, dir = {f.x, f.y, f.z};
    if (!config::Get().vrAimFromHead) return;
    XrVector3f h;
    {
        std::lock_guard l(g_headRawMu);
        h = g_headRawPos;
    }
    const V3 v = V3{aimRaw.position.x, aimRaw.position.y, aimRaw.position.z} + f * config::Get().vrReticleDistance - V3{h.x, h.y, h.z};
    const float len = std::sqrt(Dot(v, v));
    if (len < 1e-4f) return;
    org = h, dir = {v.x / len, v.y / len, v.z / len};
}
void TraceTest() {  // dev: !tracetest - forward along the view with actors, and straight down
    const uintptr_t pc = g_traceActor;
    if (!pc) { LOG("[vr] tracetest: no controller yet"); return; }
    const Axes a = AxesOf(g_aimGameRot);
    const V3 o = g_aimGameLoc;
    {  // through a girl's collision cylinder (a placed one: its component has an owner)
        static ue3::PropInfo pb, pm, po;
        const uintptr_t cPrim = ue3::FindObject("Engine.PrimitiveComponent", "Class"), cSm = ue3::FindObject("Engine.StaticMeshComponent", "Class"),
                        cAc = ue3::FindObject("Engine.ActorComponent", "Class");
        if (cPrim && cSm && cAc && ue3::FindProperty(cPrim, "Bounds", pb) && ue3::FindProperty(cSm, "StaticMesh", pm) &&
            ue3::FindProperty(cAc, "Owner", po)) {
            int tried = 0;
            for (int32_t i = ue3::NumObjects() - 1; i >= 0 && tried < 4; --i) {
                const uintptr_t c = ue3::ObjectAt(i);
                if (!c || !ue3::IsA(c, "StaticMeshComponent")) continue;
                uintptr_t mesh = 0, owner = 0;
                Read(c + pm.offset, mesh), Read(c + po.offset, owner);
                if (!mesh || !owner || ue3::PathName(c).find("Archetype") != std::string::npos) continue;
                const std::string mn = ue3::ObjName(mesh);
                if (mn != "Cylinder" && mn.find("Nakaniwa") == std::string::npos && mn.find("Koubai_Base") == std::string::npos) continue;
                static ue3::PropInfo pca;
                static bool s_pca = ue3::FindProperty(ue3::FindObject("Engine.Actor", "Class"), "bCollideActors", pca);
                uint32_t ca = 0;
                if (s_pca) Read(owner + pca.offset, ca);
                LOG("[vr] tracetest: %s mesh %s owner bCollideActors %d", ue3::PathName(c).c_str(), mn.c_str(), s_pca && (ca & pca.bitMask) ? 1 : 0);
                float b[7]{};
                Read(c + pb.offset, b);
                const V3 ctr{b[0], b[1], b[2]};
                for (int act = 0; act < 2; ++act) {
                    V3 h{};
                    uintptr_t who = 0;
                    const bool hit = TraceHit(pc, {ctr.x - 300, ctr.y, ctr.z}, {ctr.x + 300, ctr.y, ctr.z}, act, h, &who);
                    LOG("[vr] tracetest through %s (owner %s) at (%.0f,%.0f,%.0f) actors=%d: %s at (%.0f,%.0f,%.0f) %s", ue3::PathName(c).c_str(),
                        ue3::ObjName(owner).c_str(), ctr.x, ctr.y, ctr.z, act, hit ? "HIT" : "no hit", h.x, h.y, h.z,
                        who && ue3::IsObject(who) ? ue3::ObjName(who).c_str() : "-");
                }
                ++tried;
            }
        }
    }
    for (int k = 0; k < 4; ++k) {
        const bool act = k & 1;
        const V3 to = k < 2 ? o + a.x * 3000.f : V3{o.x, o.y, o.z - 3000.f};
        V3 h{};
        uintptr_t who = 0;
        const bool hit = TraceHit(pc, o, to, act, h, &who);
        LOG("[vr] tracetest %s actors=%d from (%.0f,%.0f,%.0f): %s at (%.0f,%.0f,%.0f) actor %s", k < 2 ? "forward" : "down", act ? 1 : 0, o.x,
            o.y, o.z, hit ? "HIT" : "no hit", h.x, h.y, h.z, who && ue3::IsObject(who) ? ue3::ObjName(who).c_str() : "-");
    }
}
void LogCamera() {
    LOG("[vr] camera (before head pose) at (%.0f,%.0f,%.0f) rot (%d,%d,%d); game class %s", g_aimGameLoc.x, g_aimGameLoc.y,
        g_aimGameLoc.z, g_aimGameRot.pitch, g_aimGameRot.yaw, g_aimGameRot.roll, g_lastGameClass.c_str());
    V3 fl{};
    if (g_traceActor && TraceHit(g_traceActor, g_aimGameLoc, {g_aimGameLoc.x, g_aimGameLoc.y, g_aimGameLoc.z - 1000.f}, false, fl, nullptr))
        LOG("[vr] camera: floor z %.1f (%.1f uu below)", fl.z, g_aimGameLoc.z - fl.z);
    static uint32_t offRot = 0;
    if (!offRot) {
        ue3::PropInfo pi;
        if (uintptr_t c = ue3::FindObject("Engine.Actor", "Class"); c && ue3::FindProperty(c, "Rotation", pi)) offRot = pi.offset;
    }
    for (int32_t i = 0, n = ue3::NumObjects(); i < n; ++i) {
        const uintptr_t o = ue3::ObjectAt(i);
        if (!o || !ue3::IsA(o, "GG2Girl") || ue3::PathName(o).find("Default__") != std::string::npos || !g_offActorLoc) continue;
        V3 l{};
        Rot r{};
        Read(o + g_offActorLoc, l), Read(o + offRot, r);
        uintptr_t cyl = 0;
        float collH = 0;
        if (Read(o + g_offCyl, cyl) && cyl) Read(cyl + g_offCollH, collH);
        const V3 v = l - g_aimGameLoc;
        if (Dot(v, v) > 2000.f * 2000.f) continue;
        V3 gf{};
        const bool hf = g_traceActor && TraceHit(g_traceActor, {l.x, l.y, l.z + 10.f}, {l.x, l.y, l.z - 1000.f}, false, gf, nullptr);
        LOG("[vr] girl %s (%s) at (%.0f,%.0f,%.0f) rot (%d,%d,%d) collision height %.0f; %.0f uu from the camera; floor under her %s%.1f",
            ue3::ObjName(o).c_str(), ue3::ObjName(ue3::ClassOf(o)).c_str(), l.x, l.y, l.z, r.pitch, r.yaw, r.roll, collH,
            std::sqrt(v.x * v.x + v.y * v.y), hf ? "" : "none ", gf.z);
        static ue3::PropInfo pEye, pBody, pTgt, pFx, pFy, pFollow;
        static const bool s_gaze = [] {
            const uintptr_t c = ue3::FindObject("GG2Game.GG2Girl", "Class");
            return c && ue3::FindProperty(c, "m_eyeTargetLocation", pEye) && ue3::FindProperty(c, "m_bodyTargetLocation", pBody) &&
                   ue3::FindProperty(c, "m_targetActor", pTgt) && ue3::FindProperty(c, "FaceDegreeX", pFx) &&
                   ue3::FindProperty(c, "FaceDegreeY", pFy) && ue3::FindProperty(c, "m_bAbsolutelyFaceFollow", pFollow);
        }();
        if (s_gaze) {
            V3 eye{}, body{}, tl{};
            uintptr_t tgt = 0;
            float fx = 0, fy = 0;
            uint32_t fol = 0;
            Read(o + pEye.offset, eye), Read(o + pBody.offset, body), Read(o + pTgt.offset, tgt), Read(o + pFx.offset, fx),
                Read(o + pFy.offset, fy), Read(o + pFollow.offset, fol);
            if (tgt && ue3::IsObject(tgt)) Read(tgt + g_offActorLoc, tl);
            LOG("[vr] girl %s gaze: eye target (%.0f,%.0f,%.0f) body target (%.0f,%.0f,%.0f) target actor %s (%.0f,%.0f,%.0f) face deg x %.1f y %.1f follow %d",
                ue3::ObjName(o).c_str(), eye.x, eye.y, eye.z, body.x, body.y, body.z,
                tgt && ue3::IsObject(tgt) ? ue3::PathName(tgt).c_str() : "-", tl.x, tl.y, tl.z, fx, fy, (fol & pFollow.bitMask) ? 1 : 0);
        }
    }
    if (g_traceActor && g_offActorLoc) {
        V3 pl{};
        Read(g_traceActor + g_offActorLoc, pl);
        uintptr_t cam = 0;
        V3 cl{};
        if (g_offPlayerCamera && Read(g_traceActor + g_offPlayerCamera, cam) && cam) Read(cam + g_offActorLoc, cl);
        LOG("[vr] controller %s at (%.0f,%.0f,%.0f); its camera actor at (%.0f,%.0f,%.0f)", ue3::ObjName(g_traceActor).c_str(), pl.x, pl.y,
            pl.z, cl.x, cl.y, cl.z);
    }
}
const char* GameClassDiag() { return g_gameClassDiag; }
float ZoomFactorDiag() { return g_zoom.load(); }
bool PausedDiag() { return g_lastPaused; }
bool ZoomInterrupted() { return g_zoomInterrupt.load(); }
void LogAeDiag(int seconds) {
    g_aeDiagUntil = seconds > 0 ? GetTickCount64() + ULONGLONG(seconds) * 1000 : 0;
    LOG("[vr.ae] diag %s", seconds > 0 ? "on" : "off");
}
bool InFinalDoki() { return g_inFinalDoki.load(); }
int DrawingEye() { return g_firstEyeDraw ? 0 : g_secondEyeDraw ? 1 : -1; }
// weak_spot_size (0.9.104): the game draws its pop-ups for its own FOV across the screen; drawn per eye the same pixels span
// the much wider eye FOV - this factor puts them back to the size they have on a monitor, for any headset
// (e.g. game 70 deg, eye 112 deg -> 0.47, the old fixed weak_spot_scale)
float FlatProportion() {
    const float game = g_gameFovDeg.load(), half = g_cur.halfH;
    if (game < 20.f || game > 150.f || half < 0.3f || half > 1.5f) return 0.47f;
    return std::tan(game * kPi / 360.f) / std::tan(half);
}
uintptr_t CurrentPc() { return g_lastPc; }
void ShakeDiag(int seconds) { ShakeDiagImpl(seconds); }
// stage x (0..1920 across the eye image) -> view tangent (the eye images are rendered symmetric, SetFrustum); the
// tangent difference between the eyes against their real separation gives the distance (along the view)
float StereoDistanceM(double xLeft, double xRight) {
    if (g_eyeSepM < 0.01f || g_eyeHalfTan <= 0.f) return -1.f;
    const double d = (xLeft - xRight) / 960.0 * double(g_eyeHalfTan);
    if (d <= 1e-4) return 1000.f;  // no measurable disparity: far
    return float(double(g_eyeSepM) / d);
}
bool AimPanelStage(float& x, float& y) {
    if (GetTickCount64() - g_aimPanelT.load() > 300) return false;
    x = g_aimPanelX.load(), y = g_aimPanelY.load();
    return true;
}
void DevEngineFps(int smooth, float maxFps) {
    uintptr_t eng = 0;
    if (!Read(Addr(rva::GEngine), eng) || !eng) { LOG("[perf] dev: no GEngine"); return; }
    ue3::PropInfo pS, pMax, pMin;
    const uintptr_t cls = ue3::ClassOf(eng);
    if (!ue3::FindProperty(cls, "bSmoothFrameRate", pS) || !ue3::FindProperty(cls, "MaxSmoothedFrameRate", pMax) ||
        !ue3::FindProperty(cls, "MinSmoothedFrameRate", pMin)) { LOG("[perf] dev: limiter properties not found"); return; }
    uint32_t bits = 0;
    float mx = 0, mn = 0;
    Read(eng + pS.offset, bits), Read(eng + pMax.offset, mx), Read(eng + pMin.offset, mn);
    LOG("[perf] dev: %s bSmoothFrameRate %d, MaxSmoothedFrameRate %.1f, MinSmoothedFrameRate %.1f", ue3::ObjName(cls).c_str(),
        (bits & pS.bitMask) ? 1 : 0, mx, mn);
    if (smooth < 0) return;
    *reinterpret_cast<uint32_t*>(eng + pS.offset) = smooth ? (bits | pS.bitMask) : (bits & ~pS.bitMask);
    if (maxFps > 0) *reinterpret_cast<float*>(eng + pMax.offset) = maxFps;
    LOG("[perf] dev: set bSmoothFrameRate %d, MaxSmoothedFrameRate %.1f", smooth ? 1 : 0, maxFps > 0 ? maxFps : mx);
}
bool BossMarkerDiag() { return g_bossMarkersUntil && GetTickCount64() < g_bossMarkersUntil; }
void LogBossMarkers(int seconds) {
    g_bossMarkersUntil = seconds > 0 ? GetTickCount64() + ULONGLONG(seconds) * 1000 : 0;
    LOG("[vr.boss] marker log %s", seconds > 0 ? "on" : "off");
}
ULONGLONG g_keyDiagUntil = 0;  // dev: !keydiag <s> - log the custom key bits (GG2PlayerInput.m_keyBitSteamCustomDigital)
void DevKeyDiag(int seconds) { g_keyDiagUntil = GetTickCount64() + uint64_t(seconds) * 1000; }
void KeyDiagTick() {
    if (!g_keyDiagUntil || GetTickCount64() > g_keyDiagUntil || !g_lastPc) return;
    static ue3::PropInfo q;
    static bool s_ok = [] {
        const uintptr_t c = ue3::FindObject("GG2Game.GG2PlayerInput", "Class");
        return c && ue3::FindProperty(c, "m_keyBitSteamCustomDigital", q);
    }();
    uintptr_t pi = 0;
    int32_t bits = 0;
    if (!s_ok || !game::Read(g_lastPc + 0x554, pi) || !pi || !game::Read(pi + q.offset, bits)) return;
    static int32_t s_last = -1;
    if (bits != s_last) s_last = bits, LOG("[vr] dev: custom key bits 0x%X", bits);
}
void DevDkdkStart() {  // dev: the Doki Doki target select, as the Doki key does once its checks pass (rva 0x16ACFC0)
    if (!g_lastPc || !ue3::IsA(g_lastPc, "GG2ShootingPlayerController")) { LOG("[vr] dev: not in a stage"); return; }
    uint32_t f11c4 = 0;
    float t11c8 = 0;
    Read(g_lastPc + 0x11C4, f11c4), Read(g_lastPc + 0x11C8, t11c8);
    LOG("[vr] dev: Doki Doki start (+0x11C4 0x%X, +0x11C8 %.2f)", f11c4, t11c8);
    reinterpret_cast<void(__fastcall*)(uintptr_t)>(Addr(0x16ACFC0))(g_lastPc);
}
void DevSetHeartGauge(int v) {  // dev: AGG2PlayerController::SetHeartGauge (rva 0x16C4980) - Doki Doki needs >= 100
    if (!g_lastPc || !ue3::IsA(g_lastPc, "GG2PlayerController")) return;
    if (v >= 0) reinterpret_cast<void(__fastcall*)(uintptr_t, int32_t)>(Addr(0x16C4980))(g_lastPc, v);
    // DoCanPlayDkdkMode (native): byte +0xF64 != 1, float +0x11D8 <= 0, byte +0xF66 == 0, vtable+0xA80() >= 100
    uintptr_t vt = 0;
    Read(g_lastPc, vt);
    uintptr_t fn = 0;
    Read(vt + 0xA80, fn);
    const int32_t g = fn ? reinterpret_cast<int32_t(__fastcall*)(uintptr_t)>(fn)(g_lastPc) : -1;
    uint8_t f64 = 0, f66 = 0;
    float t = 0;
    Read(g_lastPc + 0xF64, f64), Read(g_lastPc + 0xF66, f66), Read(g_lastPc + 0x11D8, t);
    LOG("[vr] dev: heart gauge set %d -> reads %d; +0xF64 %u, +0xF66 %u, +0x11D8 %.2f (%s)", v, g, f64, f66, t,
        ue3::ObjName(ue3::ClassOf(g_lastPc)).c_str());
    // the start routine (rva 0x16EDEB0) also needs: float +0x1164 <= 0, !(+0x1160 & 1), !(+0xF10 & 0x20), vtable+0xA78 != 0
    uintptr_t a78 = 0;
    Read(vt + 0xA78, a78);
    float t1164 = 0;
    uint32_t f1160 = 0, f10 = 0;
    Read(g_lastPc + 0x1164, t1164), Read(g_lastPc + 0x1160, f1160), Read(g_lastPc + 0xF10, f10);
    LOG("[vr] dev: dkdk start: +0x1164 %.2f, +0x1160 0x%X, +0xF10 0x%X, vtable+0xA78 -> rva 0x%llX", t1164, f1160, f10,
        (unsigned long long)(a78 - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))));
}
int AeSkinShipType() {
    static ue3::PropInfo pTbl, pType;
    static int s_ok = -1;
    if (s_ok < 0) {
        const uintptr_t c = ue3::FindObject("GG2Game.GG2ActionEventBasePlayerInput", "Class");
        const uintptr_t st = ue3::FindObject("GG2Game.GG2ActionEventBasePlayerInput.SAESkinShipMarkerData", "ScriptStruct");
        s_ok = c && st && ue3::FindProperty(c, "m_aeMarkerSkinShipTbl", pTbl) && ue3::FindProperty(st, "m_skinShipType", pType);
        if (!s_ok) LOG("[vr] minigame gesture type: properties not found");
    }
    uintptr_t pi = 0;
    if (s_ok != 1 || !g_lastPc || !game::Read(g_lastPc + 0x554, pi) || !pi || !ue3::IsA(pi, "GG2ActionEventBasePlayerInput")) return -1;
    uintptr_t base = pi + pTbl.offset;
    if (pTbl.arrayDim <= 1 && pTbl.type.find("Array") != std::string::npos) game::Read(pi + pTbl.offset, base);  // a dynamic array
    uint8_t t = 0xFF;
    return base && game::Read(base + pType.offset, t) && t < 16 ? int(t) : -1;
}
// The current gesture's parameters: circle radius (m_TouchRadius_TouchPad, what the circle test uses) and the tap
// target (m_ScreenPos, compared with the touches as is)
bool AeSkinShipParams(float& radius, float pos[2]) {
    static ue3::PropInfo pTbl, pParam, pRad, pPos;
    static int s_ok = -1;
    if (s_ok < 0) {
        const uintptr_t c = ue3::FindObject("GG2Game.GG2ActionEventBasePlayerInput", "Class");
        const uintptr_t st = ue3::FindObject("GG2Game.GG2ActionEventBasePlayerInput.SAESkinShipMarkerData", "ScriptStruct");
        const uintptr_t rc = ue3::FindObject("GG2Game.GG2ActionEventBaseSkinShipParameterRotation", "Class");
        const uintptr_t bc = ue3::FindObject("GG2Game.GG2ActionEventBaseSkinShipParameter", "Class");
        s_ok = c && st && rc && bc && ue3::FindProperty(c, "m_aeMarkerSkinShipTbl", pTbl) && ue3::FindProperty(st, "m_skinShipParameter", pParam) &&
               ue3::FindProperty(rc, "m_TouchRadius_TouchPad", pRad) && ue3::FindProperty(bc, "m_ScreenPos", pPos);
    }
    uintptr_t pi = 0, param = 0;
    if (s_ok != 1 || !g_lastPc || !game::Read(g_lastPc + 0x554, pi) || !pi || !ue3::IsA(pi, "GG2ActionEventBasePlayerInput")) return false;
    uintptr_t base = pi + pTbl.offset;
    if (pTbl.arrayDim <= 1 && pTbl.type.find("Array") != std::string::npos) game::Read(pi + pTbl.offset, base);
    if (!base || !game::Read(base + pParam.offset, param) || !param || !ue3::IsObject(param)) return false;
    radius = 0.f;
    if (ue3::IsA(param, "GG2ActionEventBaseSkinShipParameterRotation")) game::Read(param + pRad.offset, radius);
    game::Read(param + pPos.offset, pos[0]), game::Read(param + pPos.offset + 4, pos[1]);
    return true;
}
// The circle phase's angular speed window (AEDetectMarkerSkinShipCircleScrape: |omega - border| < worst scores,
// within best fully; the _TouchPad values are what the getters return)
bool AeCircleSpeed(float& border, float& best, float& worst) {
    static ue3::PropInfo pTbl, pParam, pB, pBest, pWorst;
    static int s_ok = -1;
    if (s_ok < 0) {
        const uintptr_t c = ue3::FindObject("GG2Game.GG2ActionEventBasePlayerInput", "Class");
        const uintptr_t st = ue3::FindObject("GG2Game.GG2ActionEventBasePlayerInput.SAESkinShipMarkerData", "ScriptStruct");
        const uintptr_t rc = ue3::FindObject("GG2Game.GG2ActionEventBaseSkinShipParameterRotation", "Class");
        s_ok = c && st && rc && ue3::FindProperty(c, "m_aeMarkerSkinShipTbl", pTbl) && ue3::FindProperty(st, "m_skinShipParameter", pParam) &&
               ue3::FindProperty(rc, "m_RotationAngleSpeedBorderHighAccuracy_TouchPad", pB) &&
               ue3::FindProperty(rc, "m_RotationAngleSpeedRangeBorderHighAccuracy_TouchPad", pBest) &&
               ue3::FindProperty(rc, "m_RotationAngleSpeedRange_TouchPad", pWorst);
    }
    uintptr_t pi = 0, param = 0;
    if (s_ok != 1 || !g_lastPc || !game::Read(g_lastPc + 0x554, pi) || !pi || !ue3::IsA(pi, "GG2ActionEventBasePlayerInput")) return false;
    uintptr_t base = pi + pTbl.offset;
    if (pTbl.arrayDim <= 1 && pTbl.type.find("Array") != std::string::npos) game::Read(pi + pTbl.offset, base);
    if (!base || !game::Read(base + pParam.offset, param) || !param || !ue3::IsObject(param) ||
        !ue3::IsA(param, "GG2ActionEventBaseSkinShipParameterRotation"))
        return false;
    return game::Read(param + pB.offset, border) && game::Read(param + pBest.offset, best) && game::Read(param + pWorst.offset, worst);
}
void DevSetPcProperty(const char* name, int v) {
    ue3::PropInfo q;
    if (!g_lastPc || !ue3::FindProperty(ue3::ClassOf(g_lastPc), name, q)) { LOG("[vr] dev: %s not found on the controller", name); return; }
    if (q.type.find("Byte") != std::string::npos) *reinterpret_cast<uint8_t*>(g_lastPc + q.offset) = uint8_t(v);
    else if (q.type.find("Int") != std::string::npos) *reinterpret_cast<int32_t*>(g_lastPc + q.offset) = v;
    else { LOG("[vr] dev: %s is %s (only Byte / Int)", name, q.type.c_str()); return; }
    LOG("[vr] dev: %s.%s = %d", ue3::ObjName(ue3::ClassOf(g_lastPc)).c_str(), name, v);
}
void SetAePin(int mode, float amp, float hz) {
    g_aePinMode = mode, g_aePinAmp = amp, g_aePinHz = hz, g_aePinT0 = GetTickCount64();
    LOG("[vr] dev: minigame pointer pin mode %d amplitude %.0f at %.2f Hz", mode, amp, hz);
}
void SetAimDiag(bool on) { g_aimDiag = on; LOG("[vr] aim diag %s", on ? "on" : "off"); }
bool GetDressingRoom(DressingRoom& out) {
    std::lock_guard l(g_voidMu);
    out = g_room;
    return out.on;
}
}  // namespace ggdpvr::vr
