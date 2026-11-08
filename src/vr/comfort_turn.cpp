#include "comfort_turn.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include "../core/config.h"
#include "../core/game.h"
#include "../core/log.h"
#include "../ue3/objects.h"
#include "controllers.h"
#include "indicators.h"
#include "menu_pointer.h"

namespace ggdpvr::vr::comfort {
namespace {
using game::Read;

constexpr float kDeg = 180.f / kPi;
constexpr float kUuPerM = 85.f;  // the game's scale (a girl's head bone ~124-135 uu above her feet)
float Wrap(float d) {
    while (d > 180.f) d -= 360.f;
    while (d < -180.f) d += 360.f;
    return d;
}
float DegOf(int32_t u) { return Wrap(float(u & 0xFFFF) * 360.f / 65536.f); }  // a rotator component, -180..180
int32_t RotOf(float deg) { return int32_t(std::lround(Wrap(deg) * 65536.f / 360.f)); }
// horizontal direction from -> to (degrees, UE yaw); none when the target is (almost) straight above / below
bool YawTo(const V3& from, const V3& to, float& yaw) {
    const float dx = to.x - from.x, dy = to.y - from.y;
    if (dx * dx + dy * dy < 20.f * 20.f) return false;
    yaw = std::atan2(dy, dx) * kDeg;
    return true;
}
double NowS() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Field offsets (runtime reflection; resolved per controller class)
struct Offs {
    uintptr_t cls = 0;
    bool ok = false;
    uint32_t loc = 0;                                           // Actor.Location
    uint32_t holding = 0, holdingMask = 0;                      // the controller's m_isHolding (optional)
    uint32_t holdSelf = 0, holdTgt = 0;                         // ...m_holdingEyeSelf / TargetLocation (optional)
    uint32_t rm = 0, poseParam = 0, flags = 0, rushMask = 0;    // the controller
    uint32_t hack = 0, scene = 0, scenes = 0, sceneStride = 0, sceneTgt = 0, sceneEnable = 0, sceneEnableMask = 0;
    uint32_t rmPoint = 0, rmPose = 0, rmArray = 0;              // GG2ShootingPlayerRootManager
    uint32_t pdPoses = 0, pdStride = 0, pdAct = 0;              // PointData (actPattern: optional)
    uint32_t ppPos = 0, ppTgt = 0, ppLock = 0, ppLockMask = 0, ppWait = 0, ppDelay = 0, ppMove = 0, ppStride = 0;  // PoseParameter
    uint32_t ppSprm = 0;                                        // ...sprmcsp (the move style: AUTO / JUMP / ...; optional)
} g_o;
uint32_t StrideOf(uintptr_t s) {
    uint32_t m = 0;
    for (const auto& p : ue3::Properties(s)) m = std::max(m, p.offset + p.elementSize * std::max(1u, p.arrayDim));
    return (m + 3) & ~3u;  // UE3 structs are packed on 4 bytes
}
bool Resolve(uintptr_t pc) {
    const uintptr_t cls = ue3::ClassOf(pc);
    if (cls == g_o.cls) return g_o.ok;
    g_o = {};
    g_o.cls = cls;
    ue3::PropInfo p;
    auto f = [&](uintptr_t c, const char* n, uint32_t& off, uint32_t* mask = nullptr) {
        if (!c || !ue3::FindProperty(c, n, p)) {
            LOG("[comfort] field %s not found: comfort turning off", n);
            return false;
        }
        off = p.offset;
        if (mask) *mask = p.bitMask;
        return true;
    };
    const uintptr_t actor = ue3::FindObject("Engine.Actor", "Class");
    const uintptr_t rmc = ue3::FindObject("GG2Game.GG2ShootingPlayerRootManager", "Class");
    const uintptr_t pd = ue3::FindObject("GG2Game.GG2ShootingPlayerRootManager.PointData", "ScriptStruct");
    const uintptr_t pp = ue3::FindObject("GG2Game.GG2ShootingPlayerRootManager.PoseParameter", "ScriptStruct");
    const uintptr_t sc = ue3::FindObject("GG2Game.GG2ShootingPlayerController.SShootingZakoRushSceneParameter", "ScriptStruct");
    bool ok = f(actor, "Location", g_o.loc) && f(cls, "m_playerRootManager", g_o.rm) && f(cls, "m_currentPoseParam", g_o.poseParam) &&
              f(cls, "m_isPlayingZakoRush", g_o.flags, &g_o.rushMask) && f(cls, "m_hackRateToEyesightFromHoldingGirl", g_o.hack) &&
              f(cls, "m_zakoRushScene", g_o.scene) && f(cls, "m_zakoRushSceneParameters", g_o.scenes);
    if (ok) g_o.sceneStride = p.elementSize;
    ok = ok && f(sc, "eyeTargetLocation", g_o.sceneTgt) && f(sc, "isEnable", g_o.sceneEnable, &g_o.sceneEnableMask) &&
         f(rmc, "m_currentPoint", g_o.rmPoint) && f(rmc, "m_currentPosePoint", g_o.rmPose) && f(rmc, "m_pointDataArray", g_o.rmArray) &&
         f(pd, "poseParameters", g_o.pdPoses) && f(pp, "positionActor", g_o.ppPos) && f(pp, "eyeTargetActor", g_o.ppTgt) &&
         f(pp, "isLockEyeDirection", g_o.ppLock, &g_o.ppLockMask) && f(pp, "beginWait", g_o.ppWait) && f(pp, "endDelay", g_o.ppDelay) && f(pp, "MoveTime", g_o.ppMove);
    if (ok) g_o.pdStride = StrideOf(pd), g_o.ppStride = StrideOf(pp);
    if (ue3::FindProperty(pp, "sprmcsp", p)) g_o.ppSprm = p.offset;
    else LOG("[comfort] PoseParameter.sprmcsp not found: jump steps only by their speed");
    if (ue3::FindProperty(pd, "actPattern", p)) g_o.pdAct = p.offset;
    else LOG("[comfort] PointData.actPattern not found: route choices not detected");
    if (ue3::FindProperty(cls, "m_isHolding", p)) g_o.holding = p.offset, g_o.holdingMask = p.bitMask ? p.bitMask : 0xFF;
    else LOG("[comfort] m_isHolding not found: a hold is seen from its camera blend only");
    if (ue3::FindProperty(cls, "m_holdingEyeSelfLocation", p)) g_o.holdSelf = p.offset;
    if (ue3::FindProperty(cls, "m_holdingEyeTargetLocation", p)) g_o.holdTgt = p.offset;
    if (!g_o.holdSelf || !g_o.holdTgt) LOG("[comfort] hold eye fields not found: grab_snap off (the game's own turn)");
    g_o.ok = ok && g_o.pdStride && g_o.ppStride && g_o.sceneStride && g_o.rushMask && g_o.ppLockMask;
    LOG("[comfort] %s fields %s (point data %u B, pose %u B, rush scene %u B)", ue3::ObjName(cls).c_str(),
        g_o.ok ? "found" : "MISSING", g_o.pdStride, g_o.ppStride, g_o.sceneStride);
    return g_o.ok;
}

// The controller's script state (UObject.StateFrame -> its UState): found once by scanning the state frame.
int g_stateOff = -1;
std::string StateName(uintptr_t obj) {
    uintptr_t sf = 0;
    if (!Read(obj + 0x20, sf) || !sf) return "";  // UObject.StateFrame (after HashOuterNext; Index is at 0x38)
    static const uintptr_t s_state = ue3::FindObject("Core.State", "Class");
    if (g_stateOff < 0) {
        static int s_tries = 0;
        if (!s_state || s_tries > 50) return "";
        ++s_tries;
        for (int off = 0x40; off < 0xA0 && g_stateOff < 0; off += 4) {  // after the FFrame part (its Node is at +0x14)
            uintptr_t p = 0;
            if (Read(sf + off, p) && ue3::IsObject(p) && ue3::ClassOf(p) == s_state) {
                g_stateOff = off;
                LOG("[comfort] controller state at StateFrame+0x%X (%s)", off, ue3::ObjName(p).c_str());
            }
        }
        if (g_stateOff < 0) {
            if (s_tries == 50) LOG("[comfort] controller state NOT found: Doki-Doki Mode not detected");
            return "";
        }
    }
    uintptr_t st = 0;
    if (!Read(sf + g_stateOff, st) || !ue3::IsObject(st)) return "";
    return ue3::ObjName(st);
}

struct TArr {
    uintptr_t data = 0;
    int32_t num = 0, max = 0;
};

// The value of an enum entry (e.g. "GG2SPRMAP_BUNKI"): its index in the name list of the Core.Enum object holding it.
// Found by scanning the enums once (the list is the first TArray of FNames after the UField part).
int EnumValue(const std::string& want) {
    const uintptr_t enumCls = ue3::FindObject("Core.Enum", "Class");
    const std::string prefix = want.substr(0, want.find('_') + 1);
    if (!enumCls || prefix.empty()) return -1;
    const int32_t names = ue3::NumNames();
    for (int32_t i = 0, n = ue3::NumObjects(); i < n; ++i) {
        const uintptr_t o = ue3::ObjectAt(i);
        if (!o || ue3::ClassOf(o) != enumCls) continue;
        for (uint32_t off = 0x30; off < 0xA0; off += 8) {
            TArr a;
            int32_t nm[2]{};
            if (!Read(o + off, a) || !a.data || a.num <= 0 || a.num > 256 || a.max < a.num || !Read(a.data, nm) || nm[0] < 0 ||
                nm[0] >= names || nm[1] != 0 || ue3::Name(nm[0]).rfind(prefix, 0) != 0)
                continue;
            for (int32_t k = 0; k < a.num; ++k)
                if (Read(a.data + uint64_t(k) * 8, nm) && nm[0] >= 0 && nm[0] < names && nm[1] == 0 && ue3::Name(nm[0]) == want) return k;
            return -1;  // (the right enum, without that entry)
        }
    }
    return -1;
}
// The route is at a route choice (a BUNKI point: the player picks the way on the HUD).
int g_bunki = -2;  // GG2SPRMAP_BUNKI's value (-1: not found), looked up once
bool IsChoice(uintptr_t rm, int32_t pt) {
    if (!g_o.pdAct) return false;
    if (g_bunki == -2) {
        g_bunki = EnumValue("GG2SPRMAP_BUNKI");
        LOG("[comfort] route choice = actPattern %d%s", g_bunki, g_bunki < 0 ? " (NOT found: route choices not detected)" : "");
    }
    TArr pts;
    uint8_t act = 0;
    return g_bunki >= 0 && Read(rm + g_o.rmArray, pts) && pts.data && pt >= 0 && pt < pts.num && pts.num <= 4096 &&
           Read(pts.data + uint64_t(pt) * g_o.pdStride + g_o.pdAct, act) && act == g_bunki;
}

// Where the camera next comes to rest, from route pose `ps` of point `pt` on: each pose faces from its position toward
// its eye target. A pose without a position stays where the camera is; one without an eye target keeps the target it
// had (or, with isLockEyeDirection, the direction it had). Poses that follow each other with no pause (endDelay + the
// next beginWait < 0.3 s) are one sweep - the "look behind" 88 + 89 deg, the bridge's -67 + -80 deg walk - up to 3 s of
// MoveTime. `rest`: how long the camera then stays (-1 = unknown: the point ends, or it sweeps on past 3 s); `next`:
// the pose after the sweep.
struct Look {
    V3 pos, tgt;
};
struct Sweep {
    bool have = false;  // an end direction (else the camera keeps the one it has)
    float yaw = 0.f, rest = -1.f;
    int32_t next = -1;
};
bool Ahead(uintptr_t rm, int32_t pt, int32_t ps, Look& lk, Sweep& sw) {
    sw = {};
    TArr pts;
    if (!Read(rm + g_o.rmArray, pts) || !pts.data || pt < 0 || pt >= pts.num || pts.num > 4096) return false;
    TArr poses;
    if (!Read(pts.data + uint64_t(pt) * g_o.pdStride + g_o.pdPoses, poses) || !poses.data || ps < 0 || ps >= poses.num ||
        poses.num > 256)
        return false;
    float prevDelay = 0.f, time = 0.f;
    int32_t q = ps;
    for (; q < poses.num && q < ps + 8; ++q) {
        const uintptr_t e = poses.data + uint64_t(q) * g_o.ppStride;
        uintptr_t pa = 0, ta = 0;
        uint32_t lockBits = 0;
        float wait = 0.f, delay = 0.f, move = 0.f;
        Read(e + g_o.ppPos, pa), Read(e + g_o.ppTgt, ta), Read(e + g_o.ppLock, lockBits), Read(e + g_o.ppWait, wait),
            Read(e + g_o.ppDelay, delay), Read(e + g_o.ppMove, move);
        if (q > ps && (prevDelay + wait >= 0.3f || time + move > 3.f)) {
            if (prevDelay + wait >= 0.3f) sw.rest = prevDelay + wait;  // (cut by the 3 s cap: still sweeping, no rest)
            break;
        }
        V3 np = lk.pos;
        if (pa && (!ue3::IsObject(pa) || !Read(pa + g_o.loc, np))) return false;
        if (ta && (!ue3::IsObject(ta) || !Read(ta + g_o.loc, lk.tgt))) return false;
        const bool locked = (lockBits & g_o.ppLockMask) && !ta;
        float y = 0.f;
        if (!locked && YawTo(np, lk.tgt, y)) sw.yaw = y, sw.have = true;
        lk.pos = np, prevDelay = delay, time += std::max(0.f, move);
    }
    sw.next = q < poses.num ? q : -1;
    return true;
}

// Route pose `ps` of point `pt`: its node's height (positionActor z), MoveTime and beginWait. False: no node (it stays).
bool PoseNode(uintptr_t rm, int32_t pt, int32_t ps, float& z, float& move, float& wait, uint8_t& style) {
    TArr pts, poses;
    if (!Read(rm + g_o.rmArray, pts) || !pts.data || pt < 0 || pt >= pts.num || pts.num > 4096) return false;
    if (!Read(pts.data + uint64_t(pt) * g_o.pdStride + g_o.pdPoses, poses) || !poses.data || ps < 0 || ps >= poses.num ||
        poses.num > 256)
        return false;
    const uintptr_t e = poses.data + uint64_t(ps) * g_o.ppStride;
    uintptr_t pa = 0;
    V3 at{};
    if (!Read(e + g_o.ppPos, pa) || !pa || !ue3::IsObject(pa) || !Read(pa + g_o.loc, at) || !Read(e + g_o.ppMove, move) ||
        !Read(e + g_o.ppWait, wait))
        return false;
    z = at.z, style = 0xFF;
    if (g_o.ppSprm) Read(e + g_o.ppSprm, style);
    return std::isfinite(z) && std::isfinite(move) && std::isfinite(wait);
}

// The walk route pose `ps` of point `pt` starts from `from`: its horizontal direction (UE yaw), length (uu), MoveTime (s).
bool Walk(uintptr_t rm, int32_t pt, int32_t ps, const V3& from, float& yaw, float& dist, float& time) {
    TArr pts, poses;
    if (!Read(rm + g_o.rmArray, pts) || !pts.data || pt < 0 || pt >= pts.num || pts.num > 4096) return false;
    if (!Read(pts.data + uint64_t(pt) * g_o.pdStride + g_o.pdPoses, poses) || !poses.data || ps < 0 || ps >= poses.num ||
        poses.num > 256)
        return false;
    const uintptr_t e = poses.data + uint64_t(ps) * g_o.ppStride;
    uintptr_t pa = 0;
    V3 to{};
    if (!Read(e + g_o.ppPos, pa) || !pa || !ue3::IsObject(pa) || !Read(pa + g_o.loc, to) || !Read(e + g_o.ppMove, time)) return false;
    dist = std::sqrt((to.x - from.x) * (to.x - from.x) + (to.y - from.y) * (to.y - from.y));
    return YawTo(from, to, yaw);
}

// --- state (game thread) ---
uintptr_t g_pc = 0;
bool g_init = false;      // the comfort yaw was set for this controller
bool g_applied = false;   // the view uses the comfort frame (else the game's rotation)
float g_c = 0.f;          // comfort yaw (UE degrees)
float g_hack = 0.f;       // the hold's eyesight blend (0..1)
uintptr_t g_rm = 0;
int32_t g_pt = -2, g_ps = -2, g_scene = -2;
bool g_rush = false;
float g_lastGy = 0.f, g_settled = 0.f;
float g_refGy = 0.f;      // the game camera at the last snap, or when it last met the view (the fallback's reference)
bool g_waitCam = false;   // a look-ahead snap is waiting for the camera to turn there (no re-centring meanwhile)
bool g_userAway = false;  // a stick turn moved the view off the camera on purpose (no re-centring until it meets it)
float g_lastHead = 0.f;
bool g_haveHead = false;
V3 g_walkFrom{};          // the rail's walking direction (horizontal camera travel): re-centring with auto_snap off / travel
bool g_walkInit = false;
float g_walkYaw = 0.f;
double g_walkT = -1e9;
bool g_stickArmed = false;
double g_lastT = 0;
double g_freeT = 0;
double g_quietT = 0;
bool g_menu = false;      // a menu / a route choice was up last frame
std::atomic<bool> g_heldA{false};
std::atomic<bool> g_marksA{false};  // the off-screen indicators may show (comfort turning in charge, no menu)      // a skipped glance plays out until then: no fallback snap toward it       // a hold (grab / shove) ended: stick snaps wait until then (the escape's wiggling stops)
int g_logs = 0;
std::atomic<bool> g_consume{false};
std::atomic<float> g_devFadeMs{-1.f};  // dev: !snapfade - overrides snap_fade_ms
float FadeMs() { return g_devFadeMs.load() >= 0.f ? g_devFadeMs.load() : config::Get().vrSnapFadeMs; }

// grab_snap: a hold's view (where the game's camera ends up: from the player's eye toward the girl's), after a blink
bool g_holdView = false;
bool g_holdSnap = false;  // this hold is shown by blinks (grab_snap=1 and the eye fields found)
Rot g_holdRot{};
enum Mode { kYaw, kToGame, kToComfort, kToHold, kFromHold, kTeleport, kNone };
struct Blink {
    enum { kIdle, kOut, kHold, kIn } ph = kIdle;
    float amt = 0.f;
    int holdFrames = 0;
    Mode mode = kYaw;
    float target = 0.f;
} g_bl;

// per-frame blink amounts for the composite pass (render thread)
struct Slot {
    std::atomic<uint64_t> frame{0};
    std::atomic<float> amt{0.f};
    std::atomic<float> vig{0.f};  // the motion vignette's strength (0..1)
} g_ring[64];
std::atomic<ULONGLONG> g_lastBlinkT{0};

float TargetYaw() {
    return g_bl.ph != Blink::kIdle && g_bl.ph != Blink::kIn && (g_bl.mode == kYaw || g_bl.mode == kTeleport) ? g_bl.target : g_c;
}
V3 g_anchor{}, g_tpLoc{};  // boss teleports: the view's held place, and where the blink under way puts it
bool g_anchorOn = false;
bool g_darkHold = false;   // a boss move to an unknown place: the blink stays dark until the camera has settled
double g_darkT = 0;
void Apply(float gy) {
    switch (g_bl.mode) {
        case kYaw: g_c = Wrap(g_bl.target); break;
        case kToGame: g_applied = false; break;
        case kToComfort: g_c = gy, g_applied = true, g_anchor = g_tpLoc, g_anchorOn = true; break;
        case kToHold: g_holdView = true; break;
        case kFromHold: g_holdView = false; break;
        case kTeleport: g_c = Wrap(g_bl.target), g_anchor = g_tpLoc, g_anchorOn = true; break;
        case kNone: break;
    }
}
void Request(Mode m, float target, float gy, const char* why) {
    const float from = g_c;
    if (g_logs < 400) {
        ++g_logs;
        if (m == kYaw)
            LOG("[comfort] snap (%s): %.0f -> %.0f deg (game camera %.0f)", why, from, Wrap(target), gy);
        else if (m == kTeleport)
            LOG("[comfort] teleport (%s): to (%.0f,%.0f,%.0f) facing %.0f (from %.0f)", why, g_tpLoc.x, g_tpLoc.y, g_tpLoc.z, Wrap(target), from);
        else if (m == kNone)
            LOG("[comfort] dark (%s)", why);
        else
            LOG("[comfort] %s (%s)",
                m == kToGame     ? "to the game camera"
                : m == kToHold   ? "to the hold's view"
                : m == kFromHold ? "back from the hold"
                                 : "comfort turning back on",
                why);
    }
    g_bl.mode = m, g_bl.target = target;
    g_refGy = gy;
    if (FadeMs() <= 0.f) {  // no fade: turn at once
        Apply(gy);
        g_bl.ph = Blink::kIdle, g_bl.amt = 0.f;
        return;
    }
    if (g_bl.ph == Blink::kHold) Apply(gy), g_bl.holdFrames = 0;  // already black
    else if (g_bl.ph != Blink::kOut) g_bl.ph = Blink::kOut;      // idle, or fading back in: fade out (again)
}
void StepBlink(float dt, float gy) {
    const float fade = std::max(0.02f, FadeMs() / 1000.f);
    switch (g_bl.ph) {
        case Blink::kIdle: break;
        case Blink::kOut:
            g_bl.amt += dt / fade;
            if (g_bl.amt >= 1.f) g_bl.amt = 1.f, Apply(gy), g_bl.ph = Blink::kHold, g_bl.holdFrames = 0;
            break;
        case Blink::kHold:  // this frame and the next one fully black: the switch never shows
            if (g_darkHold) break;  // (a boss move played out in the dark)
            if (++g_bl.holdFrames >= 2) g_bl.ph = Blink::kIn;
            break;
        case Blink::kIn:
            g_bl.amt -= dt / fade;
            if (g_bl.amt <= 0.f) g_bl.amt = 0.f, g_bl.ph = Blink::kIdle;
            break;
    }
}
void Store(uint64_t frameId, float amt, float vig) {
    Slot& s = g_ring[frameId % 64];
    s.amt = amt, s.vig = vig;
    s.frame = frameId;
    if (amt > 0.f || vig > 0.f) g_lastBlinkT = GetTickCount64();
}

// jump_blink: a big jump / fast drop step is under way (dark until g_fallUntil)
float g_jumpAmt = 0.f;
int g_jumpStyle = -2;  // EGG2SPRMCSP_JUMP's value (-1: not found), looked up once
// flatten_jumps: a small jump step - the view's straight line (game thread)
struct Hop {
    bool on = false;
    double t0 = 0, wait = 0, move = 0;
    float z0 = 0.f, dz = 0.f;
} g_hop;
// ...and a plain route step that changes the height fast and far (the 3-2 Gym Square balcony: -4.9 m in 0.4 s)
uintptr_t g_fRm = 0;
int32_t g_fPt = -2, g_fPs = -2;
float g_fLastZ = 0.f;
bool g_fHaveZ = false;
double g_fallUntil = 0;
// vignette: the motion that drives it (stage play), smoothed
float g_vig = 0.f, g_vigRaw = 0.f, g_vigTarget = 0.f;
float g_vigDist = 0.f, g_vigTurn = 0.f, g_vigTime = 0.f;  // the camera's travel / turning in the current measuring window
V3 g_vigLoc{};
float g_vigYaw = 0.f, g_vigPitch = 0.f;
bool g_vigHave = false;
std::atomic<bool> g_stageDoki{false};  // the stage's Doki-Doki Mode is up (controller state *Dkdk*)
// The motion vignette: from the camera's movement (0.5 -> 2.5 m/s) and, with `turns`, the view's own turning (20 -> 90
// deg/s; a snap / cut is no motion), smoothed. k: its strength (0 = off); off while !on. eager: full from slow camera work
// (0.1 -> 0.8 m/s, 4 -> 30 deg/s). the speed is measured over 0.1 s windows (frame-to-frame it jitters with uneven
// frame times, and the aperture showed it: "flicker"), then smoothed twice, so the edge glides; it fades out when off.
float MotionVignette(float k, bool on, bool turns, const Rot& view, const V3& loc, float dt, bool eager = false) {
    if (k <= 0.f) {
        g_vig = g_vigRaw = g_vigTarget = 0.f, g_vigHave = false, g_vigDist = g_vigTurn = g_vigTime = 0.f;
        return 0.f;
    }
    if (!on) {
        g_vigTarget = 0.f, g_vigHave = false, g_vigDist = g_vigTurn = g_vigTime = 0.f;
    } else {
        if (g_vigHave && dt > 0.f) {
            const V3 d = loc - g_vigLoc;
            const float step = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
            const float dy = Wrap(DegOf(view.yaw) - g_vigYaw), dp = Wrap(DegOf(view.pitch) - g_vigPitch);
            const float turn = std::sqrt(dy * dy + dp * dp);
            g_vigTime += dt;
            if (step < 400.f && step / dt < 40.f * kUuPerM) g_vigDist += step;  // (a jump / teleport = a cut)
            if (turns && turn < 35.f && turn / dt < 400.f) g_vigTurn += turn;   // (a snap = a cut)
        }
        if (g_vigTime >= 0.1f) {
            const float sp = g_vigDist / kUuPerM / g_vigTime, ts = g_vigTurn / g_vigTime;
            const float m = std::max(eager ? (sp - 0.1f) / 0.7f : (sp - 0.5f) / 2.f, eager ? (ts - 4.f) / 26.f : (ts - 20.f) / 70.f);
            g_vigTarget = std::min(1.f, std::max(0.f, m));
            g_vigDist = g_vigTurn = g_vigTime = 0.f;
        }
        g_vigLoc = loc, g_vigYaw = DegOf(view.yaw), g_vigPitch = DegOf(view.pitch), g_vigHave = true;
    }
    g_vigRaw += (g_vigTarget - g_vigRaw) * std::min(1.f, dt / 0.15f);
    g_vig += (g_vigRaw - g_vig) * std::min(1.f, dt / (g_vigRaw > g_vig ? 0.2f : 0.4f));
    if (g_vig < 0.002f && g_vigTarget == 0.f) g_vig = g_vigRaw = 0.f;
    return g_vig * std::min(1.f, k);
}

// --- boss battles (GG2Bs*) and minigames (GG2ActionEvent*) ---
// A hold (a girl grabbing / shoving you) on the boss controller: Angels' m_isHolded, the heroine battles' m_isHolding
struct BossOffs {
    uintptr_t cls = 0;
    uint32_t loc = 0;  // Actor.Location
    uint32_t hold = 0, holdMask = 0;
    // the heroine battles' sight target move (the magazine): its destination, start delay, moving flag
    uint32_t dest = 0, delay = 0, moving = 0, movingMask = 0;
    uint32_t placeDest = 0, placeMoving = 0, placeMovingMask = 0;  // ...its sight place move (the camera's place)
} g_bo;
void ResolveBoss(uintptr_t pc) {
    const uintptr_t cls = ue3::ClassOf(pc);
    if (cls == g_bo.cls) return;
    g_bo = {};
    g_bo.cls = cls;
    ue3::PropInfo p;
    const uintptr_t actor = ue3::FindObject("Engine.Actor", "Class");
    if (actor && ue3::FindProperty(actor, "Location", p)) g_bo.loc = p.offset;
    if (ue3::FindProperty(cls, "m_isHolded", p) || ue3::FindProperty(cls, "m_isHolding", p))
        g_bo.hold = p.offset, g_bo.holdMask = p.bitMask ? p.bitMask : 0xFF;
    if (ue3::FindProperty(cls, "m_SightTargetDestination", p)) g_bo.dest = p.offset;
    if (ue3::FindProperty(cls, "CamTargetMoveStertDelay", p)) g_bo.delay = p.offset;
    if (ue3::FindProperty(cls, "m_isCameraTargetMoving", p)) g_bo.moving = p.offset, g_bo.movingMask = p.bitMask ? p.bitMask : 0xFF;
    if (ue3::FindProperty(cls, "m_SightPlaceDestination", p)) g_bo.placeDest = p.offset;
    if (ue3::FindProperty(cls, "m_isCameraPlaceMoving", p)) g_bo.placeMoving = p.offset, g_bo.placeMovingMask = p.bitMask ? p.bitMask : 0xFF;
    LOG("[comfort] boss controller %s: hold %s, sight target move %s", ue3::ObjName(cls).c_str(), g_bo.hold ? "found" : "none",
        g_bo.dest && g_bo.delay && g_bo.moving ? "found" : "none");
}
// The minigame is in play (its game info's state *Main*: marker shots, skinship) - no vignette then
// Sisters' Odd Ritual (GG2ActionEvent05): the pose cutscene between rounds ends in a camera swoop back and forth (scene 07;
// the fades chopped it up). The view's place stays where the scene began; after it, one glide to the game camera.
std::atomic<bool> g_aeSwoop{false};
bool AeInPlay() {
    static uintptr_t s_gi = 0;
    static double s_find = 0;
    const double now = NowS();
    if ((!s_gi || !ue3::IsObject(s_gi)) && now >= s_find) {
        s_find = now + 1.0, s_gi = 0;
        for (int32_t i = 0, n = ue3::NumObjects(); i < n && !s_gi; ++i) {
            const uintptr_t o = ue3::ObjectAt(i);
            if (o && ue3::IsA(o, "GG2ActionEventBaseGameInfo") && ue3::PathName(o).find("Default__") == std::string::npos) s_gi = o;
        }
    }
    if (!s_gi || !ue3::IsObject(s_gi)) return false;
    const std::string st = StateName(s_gi);
    static std::string s_last;
    if (st != s_last && g_logs < 400) ++g_logs, LOG("[comfort] minigame state %s", st.c_str()), s_last = st;
    g_aeSwoop = config::Get().vrMinigameFade && st == "StateGG2AE_GamePhaseShiftDemoScene07" && ue3::IsA(s_gi, "GG2ActionEvent05GameInfo");
    return st.find("Main") != std::string::npos && st.find("Result") == std::string::npos;
}
// The park battles' magazine (GG2BsHeroineBattleHoldItem): the heroine's ST_HoldItemDiscovery turns the camera to it
// (SetSightTargetDestination = the item, then CheckCameraMoveEnd waits). magazine_look: the move's start delay is held
// until the player has looked at the item. Returns true while the sequence's camera turns are to stay hidden.
uintptr_t g_magGirl = 0;
std::vector<uintptr_t> g_magItems;
double g_magScanT = 0, g_magStart = 0;
bool g_magGating = false, g_magLooked = false;
bool g_magSeq = false;  // from the discovery to the pounce: the camera's turns (to the item, back, onto it) stay hidden
bool MagazineTick(uintptr_t pc, double now, bool snap, const V3& camLoc, float gy) {
    if (!ue3::IsA(pc, "GG2BsHeroineBattlePlayerController")) return false;
    if (now - g_magScanT > 0.5) {  // the heroine and the items (spawned per attack)
        g_magScanT = now;
        static std::unordered_map<uintptr_t, int> s_kind;  // per class: 1 = the ground heroine, 2 = a hold item, 0 = other
        if (!g_magGirl || !ue3::IsObject(g_magGirl)) g_magGirl = 0;
        g_magItems.clear();
        for (int32_t i = 0, n = ue3::NumObjects(); i < n; ++i) {
            const uintptr_t o = ue3::ObjectAt(i);
            if (!o) continue;
            const uintptr_t cls = ue3::ClassOf(o);
            auto it = s_kind.find(cls);
            if (it == s_kind.end())
                it = s_kind.emplace(cls, ue3::IsA(o, "GG2BsHeroineGirlGround") ? 1 : ue3::IsA(o, "GG2BsHeroineBattleHoldItem") ? 2 : 0).first;
            if (!it->second || ue3::PathName(o).find(".TheWorld.") == std::string::npos) continue;  // (live, not archetypes)
            if (it->second == 1 && !g_magGirl) g_magGirl = o;
            if (it->second == 2) g_magItems.push_back(o);
        }
    }
    const std::string st = g_magGirl && ue3::IsObject(g_magGirl) ? StateName(g_magGirl) : std::string();
    // (Discovery -> StayHere, the camera resting on the item -> WarpNear -> Pounce, the turn to her)
    if (st == "ST_HoldItemDiscovery") g_magSeq = true;
    else if (st != "ST_StayHere" && st != "ST_HoldWarpNear") g_magSeq = false;
    const bool quiet = g_magSeq;
    static std::string s_lastSt;
    if (st != s_lastSt && st.find("Hold") != std::string::npos && g_logs < 400)  // (the hold attack's steps)
        ++g_logs, LOG("[comfort] heroine %s: %s (%zu item(s))", g_magGirl ? ue3::ObjName(g_magGirl).c_str() : "?", st.c_str(), g_magItems.size());
    s_lastSt = st;
    if (st != "ST_HoldItemDiscovery") {
        if (g_magGating && g_logs < 400) ++g_logs, LOG("[comfort] magazine: the heroine moved on (%s)", st.c_str());
        g_magGating = g_magLooked = false;
        ind::SetAttention(false, {});
        return quiet;
    }
    if (!snap || !config::Get().vrMagazineLook || !g_bo.loc || !g_bo.dest || !g_bo.delay || !g_bo.moving || g_magLooked) {
        ind::SetAttention(false, {});
        return quiet;
    }
    V3 dest{};
    uint32_t mv = 0;
    Read(pc + g_bo.dest, dest), Read(pc + g_bo.moving, mv);
    const V3* item = nullptr;
    V3 il{};
    for (const uintptr_t o : g_magItems) {
        if (!ue3::IsObject(o) || !Read(o + g_bo.loc, il)) continue;
        const V3 d = il - dest;
        if (d.x * d.x + d.y * d.y + d.z * d.z < 60.f * 60.f) item = &il;
        if (item) break;
    }
    if (!(mv & g_bo.movingMask) || !item) {  // (not the move toward the item: before it, or already past)
        static double s_diagT = 0;
        if (config::Get().developer && now - s_diagT > 0.5 && g_logs < 400) {  // dev: why the gate isn't on
            s_diagT = now, ++g_logs;
            LOG("[comfort] magazine diag: moving %d, dest (%.0f,%.0f,%.0f), %zu item(s), first at (%.0f,%.0f,%.0f)", (mv & g_bo.movingMask) ? 1 : 0,
                dest.x, dest.y, dest.z, g_magItems.size(), il.x, il.y, il.z);
        }
        ind::SetAttention(false, {});
        return quiet;
    }
    if (!g_magGating) {
        g_magGating = true, g_magStart = now;
        if (g_logs < 400) ++g_logs, LOG("[comfort] magazine at (%.0f,%.0f,%.0f): its attack waits for a look", il.x, il.y, il.z);
    }
    ind::SetAttention(true, {il.x, il.y, il.z + 5.f});
    const bool looked = ind::AttentionLooked(), timeout = now - g_magStart > 30.0;
    if (looked || timeout) {
        g_magLooked = true;
        *reinterpret_cast<float*>(pc + g_bo.delay) = 0.f;  // (pc: a validated live object, game thread)
        ind::SetAttention(false, {});
        if (g_logs < 400) ++g_logs, LOG("[comfort] magazine %s after %.1f s: the attack goes on", looked ? "looked at" : "NOT looked at (timeout)", now - g_magStart);
        float y = 0.f;  // timeout: a blink faces it, as the game expects you to be looking there (then the pounce's own snap)
        if (!looked && g_applied && YawTo(camLoc, il, y) && std::fabs(Wrap(y - TargetYaw())) >= 5.f) Request(kYaw, y, gy, "magazine: face it");
    } else {
        *reinterpret_cast<float*>(pc + g_bo.delay) = 0.5f;  // the move stays pending: the heroine's script waits
    }
    return quiet;
}
// --- boss teleports, Angels spots, heroine specials, inside-scene sky, minigame level / blink ---
// The view's place in boss battles (teleports): it stays at g_anchor while the game camera travels; a blink moves it.
V3 g_rawLoc{}, g_lastRaw{};
bool g_haveRaw = false;
std::atomic<bool> g_bossView{false};  // OtherTick runs a boss battle with turning=snap (ViewLocation applies)
float g_locSettled = 0.f;
int g_angelsSub = -1;                  // the Angels controller's m_playerMovingSubStateId last frame
// minigames: roll levelled (ViewFrame), and a blink while the camera whips between scenes
std::atomic<bool> g_aeLevel{false};
float g_aeAmt = 0.f, g_aeCalm = 0.f, g_aeDark = 0.f;
float g_aeWinT = 0.f, g_aeWinTurn = 0.f, g_aeWinDist = 0.f, g_aeTurnSp = 0.f, g_aeMoveSp = 0.f;  // the view's speed, over 50 ms windows
V3 g_aeLastLoc{};
Rot g_aeLast{};
bool g_aeHave = false;
// Angels: the controller's sub-state (0 play, 1-5 summon, 6-12 kick special, 13-14 holds, 15-16 victory demo)
struct AngelsOffs {
    uintptr_t cls = 0;
    uint32_t sub = 0;
} g_ao;
void ResolveAngels(uintptr_t pc) {
    const uintptr_t cls = ue3::ClassOf(pc);
    if (cls == g_ao.cls) return;
    g_ao = {};
    g_ao.cls = cls;
    ue3::PropInfo p;
    if (ue3::FindProperty(cls, "m_playerMovingSubStateId", p)) g_ao.sub = p.offset;
    LOG("[comfort] Angels battle: sub-state %s", g_ao.sub ? "found" : "MISSING");
}
// boss moves : placed at a move's start - the places the camera settled at, the heroine's special attack, ...
double g_specialT = 0;    // the heroine was last in an ST_Special* state
int g_moveFrames = 0;     // frames the game camera has been travelling
bool g_moveDone = false;  // this move was handled (placed at its start / dark)
bool g_expectOn = false;  // placed ahead of the camera: no settle teleports until it has got there (its start waits)
V3 g_expect{};
double g_expectT = 0;
bool g_darkMoved = false;  // the camera has moved / turned since the dark began
int32_t g_darkSub = -1;    // Angels: the sub-state the dark began in (its opening move ends with it)
bool g_heroMove = false;  // the heroine controller's place move was on last frame
V3 g_lastSettle{};
bool g_haveSettle = false;
struct Spot {
    V3 loc;
    float yaw;
};
std::vector<Spot> g_spots;
void AddSpot(const V3& at, float yaw) {
    for (Spot& s : g_spots) {
        const V3 d = s.loc - at;
        if (d.x * d.x + d.y * d.y + d.z * d.z < 100.f * 100.f) {
            s.yaw = yaw;
            return;
        }
    }
    if (g_spots.size() >= 16) return;
    g_spots.push_back({at, yaw});
    if (g_logs < 400) ++g_logs, LOG("[comfort] boss camera place %zu: (%.0f,%.0f,%.0f) facing %.0f", g_spots.size(), at.x, at.y, at.z, yaw);
}
// the known place a move heads for: the one (150+ uu away) best in line with its first steps, clearly
bool NearestSpot(const V3& dir, V3& loc, float& yaw) {
    const float dl = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (dl < 10.f) return false;
    float best = -2.f, second = -2.f;
    int bi = -1;
    for (int i = 0; i < int(g_spots.size()); ++i) {
        const V3 v = g_spots[i].loc - g_lastSettle;
        const float vl = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        if (vl < 150.f) continue;
        const float c = (dir.x * v.x + dir.y * v.y + dir.z * v.z) / (dl * vl);
        if (c > best) second = best, best = c, bi = i;
        else if (c > second) second = c;
    }
    if (bi < 0 || best < 0.6f || best - second < 0.3f) return false;
    loc = g_spots[bi].loc, yaw = g_spots[bi].yaw;
    return true;
}
void StartDark(double now, const char* why) {
    g_darkHold = true, g_darkT = now, g_darkMoved = false, g_darkSub = -1;
    Request(kNone, g_c, g_lastGy, why);
}
void EndDark(float gy, const V3& raw, const char* why) {  // the move is over: placed where the camera is, then back
    g_darkHold = false;
    if (g_applied) {
        g_bl.mode = kTeleport, g_bl.target = gy, g_tpLoc = raw;
        if (g_bl.ph == Blink::kHold) Apply(gy);
    }
    if (g_logs < 400) ++g_logs, LOG("[comfort] dark move over (%s): at (%.0f,%.0f,%.0f) facing %.0f", why, raw.x, raw.y, raw.z, gy);
}
// Plant / Balloon: the tentacle (inside) scene - its sky mesh actors are hidden there (Actor.SetHidden)
std::vector<uintptr_t> g_skyActors;
bool g_skyHidden = false;
void SetSkyHidden(bool hide) {
    using PEFn = void(__fastcall*)(uintptr_t, uintptr_t, void*, void*);
    static const uintptr_t fn = ue3::FindObject("Engine.Actor.SetHidden", "Function");
    if (hide) {  // (found afresh each time: the inside scene comes a few times a battle)
        g_skyActors.clear();
        static uint32_t s_mesh = 0, s_owner = 0;
        if (!s_mesh) {
            ue3::PropInfo p;
            const uintptr_t smc = ue3::FindObject("Engine.StaticMeshComponent", "Class"), ac = ue3::FindObject("Engine.ActorComponent", "Class");
            if (smc && ue3::FindProperty(smc, "StaticMesh", p)) s_mesh = p.offset;
            if (ac && ue3::FindProperty(ac, "Owner", p)) s_owner = p.offset;
        }
        for (int32_t i = 0, n = ue3::NumObjects(); i < n && s_mesh && s_owner; ++i) {
            const uintptr_t o = ue3::ObjectAt(i);
            if (!o || !ue3::IsA(o, "StaticMeshComponent")) continue;
            uintptr_t mesh = 0, owner = 0;
            if (!Read(o + s_mesh, mesh) || !mesh || !ue3::IsObject(mesh) || ue3::ObjName(mesh).rfind("Sky_", 0) != 0) continue;
            if (Read(o + s_owner, owner) && owner && ue3::IsObject(owner) && ue3::PathName(owner).find(".TheWorld.") != std::string::npos)
                g_skyActors.push_back(owner);
        }
        for (int32_t i = 0, n = ue3::NumObjects(); i < n; ++i) {  // the sun's lens flares (Balloon's stayed)
            const uintptr_t o = ue3::ObjectAt(i);
            if (o && ue3::IsA(o, "LensFlareSource") && ue3::PathName(o).find(".TheWorld.") != std::string::npos) g_skyActors.push_back(o);
        }
    }
    int n = 0;
    for (const uintptr_t a : g_skyActors) {
        if (!fn || !ue3::IsObject(a)) continue;
        uint32_t parms[4]{hide ? 1u : 0u};
        uintptr_t vt = 0, pe = 0;
        Read(a, vt), Read(vt + game::rva::ProcessEvent_VSlot, pe);
        if (pe) reinterpret_cast<PEFn>(pe)(a, fn, parms, nullptr), ++n;
    }
    g_skyHidden = hide;
    if (g_logs < 400) ++g_logs, LOG("[comfort] inside scene %s: %d sky / lens flare actor(s) %s", hide ? "on" : "off", n, hide ? "hidden" : "shown");
    if (!hide) g_skyActors.clear();
}
uintptr_t BalloonGame() {  // the live GG2BsBalloonGame
    static uintptr_t s_game = 0;
    if (!s_game || !ue3::IsObject(s_game)) {
        s_game = 0;
        for (int32_t i = 0, n = ue3::NumObjects(); i < n && !s_game; ++i) {
            const uintptr_t o = ue3::ObjectAt(i);
            if (o && ue3::IsA(o, "GG2BsBalloonGame") && ue3::PathName(o).find("Default__") == std::string::npos) s_game = o;
        }
    }
    return s_game;
}
bool InsideScene(uintptr_t pc) {
    static uintptr_t s_cls = 0;
    static uint32_t s_pcScene = 0, s_gameScene = 0;
    if (ue3::ClassOf(pc) != s_cls) {
        s_cls = ue3::ClassOf(pc), s_pcScene = s_gameScene = 0;
        ue3::PropInfo p;
        if (ue3::FindProperty(s_cls, "m_sceneId", p)) s_pcScene = p.offset;
        const uintptr_t bg = ue3::FindObject("GG2Game.GG2BsBalloonGame", "Class");
        if (bg && ue3::FindProperty(bg, "m_sceneId", p)) s_gameScene = p.offset;
    }
    uint8_t id = 0;
    if (s_pcScene) return Read(pc + s_pcScene, id) && id == 1;
    if (!s_gameScene || !ue3::IsA(pc, "GG2BsBalloonPlayerController")) return false;
    const uintptr_t game = BalloonGame();
    return game && Read(game + s_gameScene, id) && id == 1;
}

// Boss battles: the view's place and direction change only by blinks (teleports) - to where the game camera settles,
// at once with a cut; the Angels battles' play by the stick between the roof's spots; holds and the heroine's specials
// in the game's own view. Minigames: the roll levelled, a blink for the camera's whips between scenes, the vignette.
void OtherTick(uint64_t frameId, uintptr_t pc, bool boss, bool paused, const Rot& gameRot, const V3& gameLoc, bool headOk,
               float headYaw, float dt, double now) {
    const auto& cfg = config::Get();
    const float gy = DegOf(gameRot.yaw);
    const bool live = pc && ue3::IsObject(pc);
    const bool snap = boss && cfg.vrTurning == 1 && live;
    const V3 raw = boss && g_haveRaw ? g_rawLoc : gameLoc;  // the game camera's own place (not the held view's)
    // the heroines' rockets exploding on them: a violent shake (camera shake preset 3, taken out where it's made - it
    // also turned the game's view, which the shake removal doesn't undo here)
    static uintptr_t s_shakeOff = 0;
    if (boss && live && pc != s_shakeOff && cfg.vrRemoveHitShake && ue3::IsA(pc, "GG2BsHeroineBattlePlayerController")) {
        s_shakeOff = pc;
        for (const char* name : {"c_cameraShakeParameters", "CCameraShakeParametersPreset"}) {
            ue3::PropInfo p;
            if (!ue3::FindProperty(ue3::ClassOf(pc), name, p)) continue;
            auto* q = reinterpret_cast<float*>(pc + p.offset + 3 * 0x18);  // (SGG2PlayerCameraShakeParameters: 0x18 bytes)
            LOG("[comfort] heroine battle: %s[3] (power %.2f, values %.1f %.1f) off", name, q[0], q[4], q[5]);
            q[0] = q[4] = q[5] = 0.f;
        }
    }
    if (cfg.developer && live) {  // dev: the game camera and the controller's state, 5 times a second (camera research)
        static double s_next = 0;
        static V3 s_loc{};
        if (now >= s_next) {
            const V3 d = raw - s_loc;
            LOG("[camtrace] %s %s yaw %.0f pitch %.0f roll %.0f at (%.0f,%.0f,%.0f) moved %.0f uu, view %.0f%s, vig %.2f%s", ue3::ObjName(ue3::ClassOf(pc)).c_str(),
                StateName(pc).c_str(), gy, DegOf(gameRot.pitch), DegOf(gameRot.roll), raw.x, raw.y, raw.z,
                std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z), g_c, g_applied ? "" : " (game)", g_vig, paused ? " paused" : "");
            s_next = now + 0.2, s_loc = raw;
            // the scripted move's destination: monster pawns' eye moves, the heroine controller's sight place / target
            static uintptr_t s_cls = 0, s_pcCls = 0;
            static uint32_t s_pawn = 0, s_pd = 0, s_pt = 0, s_tm = 0, s_tf = 0, s_td = 0, s_wp = 0, s_hp = 0, s_ht = 0, s_hpm = 0, s_hpmMask = 0;
            uintptr_t pawn = 0;
            ue3::PropInfo q;
            if (ue3::ClassOf(pc) != s_pcCls) {
                s_pcCls = ue3::ClassOf(pc), s_pawn = s_hp = s_ht = s_hpm = 0;
                if (ue3::FindProperty(s_pcCls, "Pawn", q)) s_pawn = q.offset;
                if (ue3::FindProperty(s_pcCls, "m_SightPlaceDestination", q)) s_hp = q.offset;
                if (ue3::FindProperty(s_pcCls, "m_SightTargetDestination", q)) s_ht = q.offset;
                if (ue3::FindProperty(s_pcCls, "m_isCameraPlaceMoving", q)) s_hpm = q.offset, s_hpmMask = q.bitMask;
            }
            if (s_pawn && Read(pc + s_pawn, pawn) && pawn && ue3::IsObject(pawn) && ue3::ClassOf(pawn) != s_cls) {
                s_cls = ue3::ClassOf(pawn), s_pd = s_pt = s_tm = s_tf = s_td = s_wp = 0;
                if (ue3::FindProperty(s_cls, "m_eyePosMovingDestination", q)) s_pd = q.offset;
                if (ue3::FindProperty(s_cls, "m_eyeTargetMovingDestination", q)) s_td = q.offset;
                if (ue3::FindProperty(s_cls, "m_eyePosMovingTimer", q)) s_tm = q.offset;
                if (ue3::FindProperty(s_cls, "m_eyePosMovingTimeToFinish", q)) s_tf = q.offset;
                if (ue3::FindProperty(s_cls, "m_eyeWorldPos", q)) s_wp = q.offset;
                if (ue3::FindProperty(s_cls, "m_basePos", q)) s_pt = q.offset;
            }
            if (pawn && s_pd) {
                V3 pd{}, td{}, wp{}, bp{};
                float tm = 0.f, tf = 0.f;
                Read(pawn + s_pd, pd), Read(pawn + s_td, td), Read(pawn + s_wp, wp), Read(pawn + s_pt, bp), Read(pawn + s_tm, tm), Read(pawn + s_tf, tf);
                LOG("[camtrace]   eye move to (%.0f,%.0f,%.0f) looking at (%.0f,%.0f,%.0f), %.2f / %.2f s; eye (%.0f,%.0f,%.0f) base (%.0f,%.0f,%.0f)",
                    pd.x, pd.y, pd.z, td.x, td.y, td.z, tm, tf, wp.x, wp.y, wp.z, bp.x, bp.y, bp.z);
            }
            if (s_hp) {
                V3 hp{}, ht{};
                uint32_t mv = 0;
                Read(pc + s_hp, hp), Read(pc + s_ht, ht), Read(pc + s_hpm, mv);
                LOG("[camtrace]   sight place to (%.0f,%.0f,%.0f)%s, target to (%.0f,%.0f,%.0f)", hp.x, hp.y, hp.z,
                    (mv & s_hpmMask) ? " (moving)" : "", ht.x, ht.y, ht.z);
            }
        }
    }
    if (pc != g_pc || !snap) {
        if (pc != g_pc || g_init) {
            g_pc = pc, g_init = false, g_applied = false, g_bl = {}, g_hack = 0.f, g_settled = 0.f, g_holdView = g_holdSnap = false;
            g_pt = g_ps = g_scene = -2, g_rm = 0, g_rush = false, g_hop = {}, g_fallUntil = 0, g_jumpAmt = 0.f;
            g_magGating = g_magLooked = g_magSeq = false, g_magGirl = 0, g_magScanT = 0;
            g_anchorOn = false, g_locSettled = 0.f, g_angelsSub = -1, g_stickArmed = false;
            g_spots.clear(), g_haveSettle = false, g_darkHold = false, g_specialT = 0, g_heroMove = false, g_moveFrames = 0;
            g_expectOn = false, g_moveDone = false;
            ind::SetAttention(false, {});
        }
    }
    g_bossView = snap;
    g_aeLevel = !boss && cfg.vrMinigameLevel != 0;
    g_stageDoki = false, g_consume = false, g_heldA = false;
    bool held = false, quiet = false;
    const bool menu = paused || pointer::MenuScene();
    // the tentacle (inside) scenes: no sky behind you (any turning mode)
    if (boss && live) {
        const bool inside = InsideScene(pc);  // (from the fade, not after the ChangeScene state)
        if (inside != g_skyHidden) SetSkyHidden(inside);
    } else if (g_skyHidden) {
        g_skyHidden = false, g_skyActors.clear();
    }
    if (snap) {
        ResolveBoss(pc);
        const bool angels = ue3::IsA(pc, "GG2BsAngelsPlayerController");
        int32_t sub = -1;
        if (angels) {
            ResolveAngels(pc);
            if (g_ao.sub) Read(pc + g_ao.sub, sub);
        }
        uint32_t hb = 0;
        held = (g_bo.hold && Read(pc + g_bo.hold, hb) && (hb & g_bo.holdMask)) || sub == 13 || sub == 14;
        quiet = MagazineTick(pc, now, true, raw, gy);
        // the heroine's special attacks (the rockets, ...): her own camera work, in the game's view - from her first
        // ST_Special* state until 2 s of others (her states flip in and out of it, a blink each time)
        const std::string hst = g_magGirl && ue3::IsObject(g_magGirl) ? StateName(g_magGirl) : std::string();
        if (hst.rfind("ST_Special", 0) == 0) g_specialT = now;
        const bool special = g_specialT > 0 && now - g_specialT < 2.0;
        // Angels battles: the game's own camera, moves and turns, with the vignette (the stick teleports
        // disoriented, and the boss aims its shots at the game camera's place)
        const bool want = !held && !special && !angels;
        const float step = g_haveRaw ? std::sqrt((raw.x - g_lastRaw.x) * (raw.x - g_lastRaw.x) + (raw.y - g_lastRaw.y) * (raw.y - g_lastRaw.y) +
                                                 (raw.z - g_lastRaw.z) * (raw.z - g_lastRaw.z))
                                     : 0.f;
        const float speed = dt > 0.f ? std::fabs(Wrap(gy - g_lastGy)) / dt : 0.f;
        g_settled = speed < 25.f ? g_settled + dt : 0.f;
        g_locSettled = dt > 0.f && step < 400.f && step / dt < 15.f ? g_locSettled + dt : 0.f;
        const bool settled = g_settled >= 0.25f && g_locSettled >= 0.25f;
        if (!g_init) {
            LOG("[comfort] boss battle: %s", angels ? "the game's own camera (Angels), dark at the specials' start" : "teleports on");
            g_init = true, g_applied = want, g_c = gy, g_lastGy = gy, g_settled = 0.f, g_refGy = gy, g_anchor = raw, g_anchorOn = true;
            g_lastSettle = raw, g_haveSettle = false;
        } else if (want != g_applied && !g_darkHold && !(g_bl.ph != Blink::kIdle && g_bl.mode != kYaw && g_bl.mode != kTeleport)) {
            g_tpLoc = raw;
            Request(want ? kToComfort : kToGame, gy, gy, want ? "back from the game's view" : held ? "a hold: the game's view" : "a special attack: the game's view");
        } else if (g_applied && step > 400.f) {  // a cut (a scene change behind a fade): placed and turned with it at once
            g_anchor = raw, g_c = gy, g_refGy = gy, g_locSettled = 0.f;
            if (g_bl.ph != Blink::kIdle && (g_bl.mode == kYaw || g_bl.mode == kTeleport)) g_bl.target = gy, g_tpLoc = raw;
            if (g_logs < 400) ++g_logs, LOG("[comfort] camera cut %.0f uu: view placed with it, facing %.0f", step, gy);
        }
        // Angels summons / specials: dark from their start until the camera has got there
        if (angels && sub != g_angelsSub && g_angelsSub == 0 && sub >= 1 && sub <= 12 && !g_darkHold)
            StartDark(now, sub <= 5 ? "Angels summon" : "Angels special"), g_darkSub = sub;
        // a move starts (placed at once, not after it: the boss's entrance faced away until then): the
        // heroine's sight place destination facing its target; else a place it settled at before, in the move's
        // direction; else dark until it settles
        uint32_t pmv = 0;
        const bool heroMove = g_bo.placeDest && g_bo.placeMoving && Read(pc + g_bo.placeMoving, pmv) && (pmv & g_bo.placeMovingMask);
        g_moveFrames = dt > 0.f && step < 400.f && step / dt > 150.f ? g_moveFrames + 1 : 0;
        // (its direction read once it has gone 40 uu from where it rested: its first frames ease in)
        const V3 gone = raw - g_lastSettle;
        const bool headed = g_moveFrames >= 2 && !g_moveDone && gone.x * gone.x + gone.y * gone.y + gone.z * gone.z >= 40.f * 40.f;
        if (g_applied && want && !menu && !g_darkHold && g_bl.ph == Blink::kIdle && ((heroMove && !g_heroMove) || (!g_bo.placeDest && headed))) {
            g_moveDone = true;
            V3 dest{}, tgt{};
            float y = gy;
            if (heroMove && Read(pc + g_bo.placeDest, dest) && Read(pc + g_bo.dest, tgt)) {
                YawTo(dest, tgt, y);
                g_tpLoc = dest, Request(kTeleport, y, gy, "a move starts: its destination");
                g_expectOn = true, g_expect = dest, g_expectT = now;
            } else if (g_haveSettle && NearestSpot(raw - g_lastSettle, dest, y)) {
                g_tpLoc = dest, Request(kTeleport, y, gy, "a move starts: a known place");
                g_expectOn = true, g_expect = dest, g_expectT = now;
            } else {
                StartDark(now, "a move to a new place");
            }
        }
        g_heroMove = heroMove;
        if (g_darkHold && ((dt > 0.f && step < 400.f && step / dt > 150.f) || speed > 60.f)) g_darkMoved = true;
        if (g_expectOn) {  // arrived (or given up on): settle teleports again
            const V3 de = raw - g_expect;
            if (de.x * de.x + de.y * de.y + de.z * de.z < 60.f * 60.f || now - g_expectT > 4.0 || (!heroMove && g_bo.placeDest && settled && now - g_expectT > 1.0))
                g_expectOn = false;
        }
        if (settled) {  // a resting place: remembered for later moves (and the end of a dark move)
            g_lastSettle = raw, g_haveSettle = true, g_moveDone = false;
            AddSpot(raw, gy);
            if (g_darkHold && (g_darkMoved || now - g_darkT > 1.0)) EndDark(gy, raw, g_darkMoved ? "the camera settled" : "it didn't move");
        }
        if (g_darkHold && now - g_darkT > 2.5) EndDark(gy, raw, "2.5 s");
        if (g_darkHold && angels && g_darkSub >= 0 && sub != g_darkSub) EndDark(gy, raw, "the next step of the special");
        if (g_applied && want) {
            const float thr = std::max(5.f, cfg.vrBossSnapThreshold);
            const float off = Wrap(gy - TargetYaw());
            const V3 da = raw - g_anchor;
            const float away = std::sqrt(da.x * da.x + da.y * da.y + da.z * da.z);
            if (std::fabs(off) < thr / 3.f) g_refGy = gy;
            if (menu && !g_menu && std::fabs(off) >= 10.f) g_tpLoc = g_anchor, Request(kTeleport, gy, gy, "menu: face the game camera");
            if (!menu && !quiet && !g_darkHold && !g_expectOn && g_bl.ph == Blink::kIdle && settled &&
                (away > 60.f || (std::fabs(off) >= thr && std::fabs(Wrap(gy - g_refGy)) >= thr))) {
                char why[64];
                sprintf_s(why, "boss camera settled %.1f m away", away / kUuPerM);
                g_tpLoc = raw;
                Request(kTeleport, gy, gy, why);
            }
            // small offsets close as the head turns (as in stages, toward the game camera)
            float dh = 0.f;
            if (headOk && g_haveHead) dh = std::fabs(Wrap(headYaw - g_lastHead));
            if (dh > 10.f || (dt > 0.f && dh / dt < 8.f)) dh = 0.f;
            if (cfg.vrRecenterGain > 0.f && dh > 0.f && !quiet && g_bl.ph == Blink::kIdle && std::fabs(off) < thr && speed < 45.f) {
                const float st = std::min(std::fabs(off), std::min(0.5f, cfg.vrRecenterGain) * dh);
                g_c = Wrap(g_c + (off > 0.f ? st : -st));
            }
        }
        g_angelsSub = sub;
        g_lastGy = gy;
        g_lastHead = headYaw, g_haveHead = headOk;
    } else if (g_darkHold) {
        EndDark(gy, raw, "out of the boss view");
    }
    if (boss) StepBlink(dt, gy);  // (also the victory's blink with turning=game)
    g_lastRaw = raw;
    g_menu = menu;
    g_marksA = snap && g_applied && !held && !menu;
    // minigames: a fade while the camera moves violently between scenes (the knock-down) - dark over 120 deg/s or 5 m/s of
    // what is seen (the level view: the pitch and roll taken out don't count), back once under 20 deg/s and 1.5 m/s for 0.1 s, at most 1 s
    if (!boss && live && cfg.vrMinigameFade) {
        const Rot view = ViewFrame(gameRot);
        if (g_aeHave && dt > 0.f) {
            const float dy = Wrap(DegOf(view.yaw) - DegOf(g_aeLast.yaw)), dp = Wrap(DegOf(view.pitch) - DegOf(g_aeLast.pitch));
            const float turn = std::sqrt(dy * dy + dp * dp);
            const V3 d = gameLoc - g_aeLastLoc;
            const float step = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
            g_aeWinT += dt;
            if (turn < 35.f && turn / dt < 800.f) g_aeWinTurn += turn;                // (a cut is no motion)
            if (step < 400.f && step / dt < 40.f * kUuPerM) g_aeWinDist += step;
        }
        g_aeLast = view, g_aeLastLoc = gameLoc, g_aeHave = true;
        if (g_aeWinT >= 0.05f) {
            g_aeTurnSp = g_aeWinTurn / g_aeWinT, g_aeMoveSp = g_aeWinDist / kUuPerM / g_aeWinT;
            g_aeWinT = g_aeWinTurn = g_aeWinDist = 0.f;
        }
        g_aeCalm = g_aeTurnSp < 20.f && g_aeMoveSp < 1.5f ? g_aeCalm + dt : 0.f;
        const bool play = AeInPlay();
        bool dark = !play && !paused && (g_aeTurnSp > 120.f || g_aeMoveSp > 5.f || (g_aeAmt > 0.f && g_aeCalm < 0.1f));
        g_aeDark = dark ? g_aeDark + dt : 0.f;
        if (g_aeDark > 1.f) dark = false;
        if (dark && g_aeAmt == 0.f && g_logs < 400) ++g_logs, LOG("[comfort] minigame camera lurches (%.0f deg/s, %.1f m/s): dark until it settles", g_aeTurnSp, g_aeMoveSp);
        const float fade = std::max(0.02f, FadeMs() / 1000.f);
        g_aeAmt = dark ? std::min(1.f, g_aeAmt + dt / fade) : std::max(0.f, g_aeAmt - dt / fade);
    } else {
        g_aeAmt = 0.f, g_aeHave = false, g_aeWinT = g_aeWinTurn = g_aeWinDist = g_aeTurnSp = g_aeMoveSp = 0.f;
    }
    // the vignette: bosses - the view's own movement (none with teleports; the game's in its own view); minigames -
    // turns and moves, outside play
    float vigOut = 0.f;
    if (boss) {
        const V3 vl = snap && g_applied && g_anchorOn ? g_anchor : raw;
        // (the heroines' special attacks: the game's camera work, slow but constant - "moving around a lot")
        const bool inSpecial = snap && g_specialT > 0 && now - g_specialT < 2.0;
        vigOut = MotionVignette(cfg.vrVignette, live && !paused && !held, true, ViewFrame(gameRot), vl, dt, inSpecial);
        static bool s_wasSpecial = false;
        static float s_peak = 0.f;
        if (inSpecial) s_peak = std::max(s_peak, vigOut);
        else if (s_wasSpecial) LOG("[comfort] special attack over: its vignette peaked at %.2f", s_peak), s_peak = 0.f;
        s_wasSpecial = inSpecial;
    } else {
        vigOut = MotionVignette(cfg.vrVignette, live && !paused && !AeInPlay(), true, ViewFrame(gameRot), gameLoc, dt, true);
    }
    Store(frameId, std::max(g_bl.amt, g_aeAmt), vigOut);
}
}  // namespace

void Tick(uint64_t frameId, uintptr_t pc, bool stagePlay, bool bossPlay, bool aePlay, bool paused, const Rot& gameRot,
          const V3& gameLoc, bool headOk, float headYaw) {
    const auto& cfg = config::Get();
    const double now = NowS();
    const float dt = g_lastT > 0 ? float(std::min(0.1, std::max(0.0, now - g_lastT))) : 0.f;
    g_lastT = now;
    if (bossPlay || aePlay) return OtherTick(frameId, pc, bossPlay, paused, gameRot, gameLoc, headOk, headYaw, dt, now);
    g_bossView = false, g_aeLevel = false;
    const float gy = DegOf(gameRot.yaw);
    const bool resolved = stagePlay && pc && ue3::IsObject(pc) && ue3::IsA(pc, "GG2ShootingPlayerController") && Resolve(pc);
    const bool active = cfg.vrTurning == 1 && resolved;
    if (pc != g_pc || !active) {  // another stage / controller, or out of stage play: start again from the game camera
        if (pc != g_pc || g_init) {
            g_pc = pc, g_init = false, g_applied = false, g_rm = 0, g_pt = g_ps = g_scene = -2, g_rush = false;
            g_bl = {}, g_hack = 0.f, g_settled = 0.f, g_holdView = g_holdSnap = false;
            g_fRm = 0, g_fPt = g_fPs = -2, g_fHaveZ = false, g_fallUntil = 0, g_hop = {};
        }
    }
    // jump_blink, plain steps: a new route pose whose node is jump_blink_height or more above / below the last one, reached
    // at 3 m/s or more - dark from its start through its wait and move (node to node: no camera height offset involved)
    if (resolved && (cfg.vrJumpBlink || cfg.vrFlattenJumps)) {
        uintptr_t frm = 0;
        int32_t fpt = -1, fps = -1;
        if (Read(pc + g_o.rm, frm) && frm && ue3::IsObject(frm)) Read(frm + g_o.rmPoint, fpt), Read(frm + g_o.rmPose, fps);
        if (frm != g_fRm || fpt != g_fPt || fps != g_fPs) {
            g_fRm = frm, g_fPt = fpt, g_fPs = fps;
            float z = 0.f, move = 0.f, wait = 0.f;
            uint8_t style = 0xFF;
            g_hop.on = false;
            if (frm && PoseNode(frm, fpt, fps, z, move, wait, style)) {
                if (g_jumpStyle == -2) {
                    g_jumpStyle = EnumValue("EGG2SPRMCSP_JUMP");
                    LOG("[comfort] jump steps: sprmcsp %d%s", g_jumpStyle, g_jumpStyle < 0 ? " (NOT found: only fast drops)" : "");
                }
                const bool jump = g_jumpStyle >= 0 && style == g_jumpStyle;
                const float dz = z - g_fLastZ;
                const bool big = g_fHaveZ && std::fabs(dz) >= std::max(0.1f, cfg.vrJumpBlinkHeight) * kUuPerM;
                if (big && (jump || std::fabs(dz) / std::max(0.05f, move) >= 3.f * kUuPerM)) {
                    g_fallUntil = now + std::max(0.f, wait) + std::max(0.f, move) + 0.05;
                    if (g_logs < 400)
                        ++g_logs, LOG("[comfort] route P%d.%d: %s %+.1f m in %.1f s - dark through it", fpt, fps, jump ? "jump" : "drop",
                                      dz / kUuPerM, move);
                } else if (jump && g_fHaveZ && cfg.vrFlattenJumps && move > 0.05f) {
                    g_hop = {true, now, std::max(0.f, wait), move, gameLoc.z, dz};
                    if (g_logs < 400)
                        ++g_logs, LOG("[comfort] route P%d.%d: jump %+.1f m in %.1f s - the view goes straight", fpt, fps, dz / kUuPerM, move);
                }
                g_fLastZ = z, g_fHaveZ = true;
            }
        }
    }
    // jump_blink (any turning mode, stage play): dark from take-off to landing (a faster fade: the jump is short)
    {
        const bool air = cfg.vrJumpBlink && stagePlay && now < g_fallUntil;
        const float fade = std::max(0.02f, FadeMs() / 2000.f);
        if (air && g_jumpAmt == 0.f && g_logs < 400) ++g_logs, LOG("[comfort] jump: dark until the landing");
        g_jumpAmt = air ? std::min(1.f, g_jumpAmt + dt / fade) : std::max(0.f, g_jumpAmt - dt / fade);
    }
    // the motion vignette (any turning mode, stage play): the rail's movement or the view's own turning, smoothed.
    // Not in Doki-Doki Mode: its cameras circle slowly
    const bool doki = stagePlay && pc && ue3::IsObject(pc) && StateName(pc).find("Dkdk") != std::string::npos;
    g_stageDoki = doki;
    const float vigOut = MotionVignette(cfg.vrVignette, stagePlay && !doki, true, ViewFrame(gameRot), gameLoc, dt);
    if (!active) {
        g_consume = false, g_heldA = false, g_menu = false, g_marksA = false;
        Store(frameId, g_jumpAmt, vigOut);
        return;
    }
    const std::string st = StateName(pc);
    const bool want = st.find("Dkdk") == std::string::npos;  // Doki-Doki Mode: its own cameras (the game's rotation)
    // the stage's start (PlayerStarting: the camera is placed during it - it read 0 deg on the first frames): face the
    // game camera from frame to frame, no blink (the loading screen just ended)
    const bool starting = st.empty() || st == "PlayerStarting";
    if (!g_init || starting) {
        if (!g_init) LOG("[comfort] snap turning on (state %s)", st.empty() ? "?" : st.c_str());
        g_init = true, g_applied = want, g_c = gy, g_lastGy = gy, g_stickArmed = false, g_settled = 0.f;
        g_refGy = gy, g_waitCam = false, g_userAway = false, g_walkInit = false, g_walkT = -1e9;
    } else if (want != g_applied && !(g_bl.ph != Blink::kIdle && g_bl.mode != kYaw)) {
        Request(want ? kToComfort : kToGame, gy, gy, st.c_str());
    } else if (g_applied) {
        // a camera cut (the game jumps its camera in one frame, e.g. when the stage begins): the view jumps with it, as on
        // a monitor - no smooth turn to hide. Turning at up to ~400 deg/s moves far less than this per frame.
        const float jump = Wrap(gy - g_lastGy);
        if (std::fabs(jump) > 35.f && g_hack < 0.01f) {  // (not the hold's own turn)
            g_c = Wrap(g_c + jump);
            if (g_bl.ph != Blink::kIdle && g_bl.mode == kYaw) g_bl.target = Wrap(g_bl.target + jump);
            if (g_logs < 400) ++g_logs, LOG("[comfort] camera cut %.0f deg: view %.0f (game camera %.0f)", jump, g_c, gy);
        }
    }
    float hack = 0.f;
    Read(pc + g_o.hack, hack);
    g_hack = std::isfinite(hack) ? std::min(1.f, std::max(0.f, hack)) : 0.f;
    // a hold: the dominant stick goes back to the game (escaping needs both sticks), no stick snaps until 0.5 s after it
    bool held = g_hack > 0.01f;
    if (g_o.holdingMask) {
        uint32_t hb = 0;
        if (Read(pc + g_o.holding, hb) && (hb & g_o.holdingMask)) held = true;
    }
    if (held) {
        if (now >= g_freeT && g_logs < 400) ++g_logs, LOG("[comfort] hold: the sticks go to the game (escape)");
        g_freeT = now + 0.5, g_stickArmed = false;
    }
    // grab_snap: the hold's view after a blink (and back after one), instead of the game's 0.3 s turn
    if (g_holdSnap || (held && g_applied && want && cfg.vrGrabSnap && g_o.holdSelf && g_o.holdTgt)) {
        V3 eye{}, her{};
        float y = 0.f;
        if (Read(pc + g_o.holdSelf, eye) && Read(pc + g_o.holdTgt, her) && YawTo(eye, her, y)) {
            const V3 d = her - eye;
            g_holdRot = {RotOf(std::atan2(d.z, std::sqrt(d.x * d.x + d.y * d.y)) * kDeg), RotOf(y), 0};
        } else if (!g_holdView) {
            g_holdRot = gameRot;  // (no usable eye positions yet: the game's own view)
        }
        if (held && !g_holdSnap) g_holdSnap = true, Request(kToHold, 0.f, gy, "grab_snap");
        else if (!held && g_holdSnap) g_holdSnap = false, Request(kFromHold, 0.f, gy, "the hold ended");
    }
    g_heldA = held && g_applied;
    const float thr = std::max(5.f, cfg.vrSnapThreshold);
    // where the route is: its point and pose, Zako Rush, a route choice
    uintptr_t rm = 0;
    int32_t pt = -1, ps = -1;
    if (Read(pc + g_o.rm, rm) && rm && ue3::IsObject(rm)) Read(rm + g_o.rmPoint, pt), Read(rm + g_o.rmPose, ps);
    uint32_t flags = 0;
    Read(pc + g_o.flags, flags);
    const bool rush = (flags & g_o.rushMask) != 0;
    const bool choice = rm && IsChoice(rm, pt);
    // a menu (pause, continue prompt, results) or a route choice: no snap turning, and the stick goes to the game. When
    // one opens: face the game camera once (the HUD panel is fixed in the room: this lines the scene up behind it, e.g.
    // the route-choice arrows over the two ways). Zako Rush: the stick picks the rush's directions (their own snaps).
    const bool menu = paused || pointer::MenuScene() || choice;
    if (menu && !g_menu && g_applied && want && !starting && std::fabs(Wrap(gy - TargetYaw())) >= 10.f) {
        Request(kYaw, gy, gy, choice ? "route choice: face the game camera" : "menu: face the game camera");
        g_userAway = g_waitCam = false;
    }
    if (menu != g_menu && g_logs < 400) ++g_logs, LOG("[comfort] %s: stick snaps %s", choice ? "route choice" : "menu", menu ? "off" : "back on");
    g_menu = menu;
    if (menu || rush) g_freeT = std::max(g_freeT, now + 0.5), g_stickArmed = false;
    if (g_applied && want && !menu && !starting) {
        // the route: a new pose -> its end direction
        if (rm != g_rm || pt != g_pt || ps != g_ps) {
            const bool first = g_pt == -2;
            g_rm = rm, g_pt = pt, g_ps = ps;
            g_waitCam = false;
            Look lk{gameLoc, {}};
            Read(pc + g_o.poseParam + 0xC, lk.tgt);  // poseParameters.eyeTargetLocation (after Location)
            Sweep sw;
            const bool have = !rush && rm && Ahead(rm, pt, ps, lk, sw) && sw.have;
            // a glance: it rests there shorter than snap_glance, then the camera turns back to (near) the view - no snap
            Sweep back;
            const bool glance = have && sw.rest >= 0.f && sw.rest < cfg.vrSnapGlance && sw.next >= 0 &&
                                Ahead(rm, pt, sw.next, lk, back) && back.have && std::fabs(Wrap(back.yaw - TargetYaw())) < thr;
            if (cfg.developer)  // dev: each step's predicted rest direction (the game camera when it stops turning = the real one)
                LOG("[comfort] route P%d.%d: rest %s%.0f for %.1f s (next pose %d)%s, game camera now %.0f, view %.0f", pt, ps,
                    have ? "" : "(none) ", have ? sw.yaw : 0.f, sw.rest, sw.next, glance ? " - a glance" : "", gy, g_c);
            float wy = 0.f, wd = 0.f, wt = 0.f;
            const bool walk = cfg.vrAutoSnap == 1 && !rush && rm && Walk(rm, pt, ps, gameLoc, wy, wd, wt) &&
                              wd >= std::max(0.25f, cfg.vrTravelDistance) * kUuPerM && wt >= 0.4f * std::max(0.25f, cfg.vrTravelDistance);
            if (walk && !first && std::fabs(Wrap(wy - TargetYaw())) >= std::max(5.f, cfg.vrTravelThreshold)) {
                char why[48];
                sprintf_s(why, "walk P%d.%d, %.1f m in %.1f s", pt, ps, wd / kUuPerM, wt);
                Request(kYaw, wy, gy, why);
                g_userAway = false;
            }
            if (cfg.vrAutoSnap == 2 && !first && have && std::fabs(Wrap(sw.yaw - TargetYaw())) >= thr) {
                if (glance) {
                    if (g_logs < 400) ++g_logs, LOG("[comfort] glance skipped (route P%d.%d): %.0f deg for %.1f s, then back", pt, ps, sw.yaw, sw.rest);
                    g_quietT = now + sw.rest + 2.0;  // (the turn there, the rest, the turn back)
                } else {
                    char why[48];
                    sprintf_s(why, "route P%d.%d", pt, ps);
                    Request(kYaw, sw.yaw, gy, why);
                    g_userAway = false, g_waitCam = std::fabs(Wrap(gy - sw.yaw)) >= 5.f;
                }
            }
        }
        // Zako Rush: each direction change (and its start) -> the new scene's direction
        int32_t scene = -1;
        Read(pc + g_o.scene, scene);
        if (rush && (scene != g_scene || !g_rush) && scene >= 0 && scene < 9) {
            const uintptr_t s = pc + g_o.scenes + uint64_t(scene) * g_o.sceneStride;
            uint32_t en = 0;
            V3 t{};
            float y = 0.f;
            if (Read(s + g_o.sceneEnable, en) && (en & g_o.sceneEnableMask) && Read(s + g_o.sceneTgt, t) && YawTo(gameLoc, t, y) &&
                std::fabs(Wrap(y - TargetYaw())) >= 10.f) {
                char why[32];
                sprintf_s(why, "rush scene %d", scene);
                Request(kYaw, y, gy, why);
                g_userAway = false;
            }
        }
        g_rush = rush, g_scene = scene;
        // anything else that turned the game camera: once it has turned on its own (since the last snap, or since it last
        // met the view) and settled well away from the view - never undoing a look-ahead snap that waits for the camera,
        // or a stick turn
        const float speed = dt > 0.f ? std::fabs(Wrap(gy - g_lastGy)) / dt : 0.f;
        g_settled = speed < 25.f ? g_settled + dt : 0.f;
        const float off = Wrap(gy - TargetYaw());  // (a snap being faded to counts as done)
        if (std::fabs(off) < thr / 3.f) g_refGy = gy, g_waitCam = false;
        if (std::fabs(off) < 5.f) g_userAway = false;
        if (g_waitCam && g_settled >= 4.f) g_waitCam = false;  // (the camera stopped short of the predicted direction)
        if (cfg.vrAutoSnap == 2 && now >= g_quietT && g_bl.ph == Blink::kIdle && g_hack < 0.01f && g_settled >= 0.25f && std::fabs(off) >= 1.5f * thr &&
            std::fabs(Wrap(gy - g_refGy)) >= thr) {
            Request(kYaw, gy, gy, "game camera settled away");
            g_waitCam = g_userAway = false;
        }
        // the rail's walking direction: from the camera's horizontal travel (a jump of 9+ m in one step = a cut)
        {
            const float dx = gameLoc.x - g_walkFrom.x, dy = gameLoc.y - g_walkFrom.y, d2 = dx * dx + dy * dy;
            if (!g_walkInit || d2 > 400.f * 400.f) g_walkFrom = gameLoc, g_walkInit = true, g_walkT = -1e9;
            else if (d2 >= 30.f * 30.f) g_walkYaw = std::atan2(dy, dx) * kDeg, g_walkFrom = gameLoc, g_walkT = now;
        }
        const bool walking = now - g_walkT < 1.0;
        // re-centring: a small offset (under the threshold) closes while the player turns their own head - by up to
        // recenter_gain of the head's turn, which goes unnoticed. Toward the game camera (auto_snap=route), or toward the
        // walking direction while the rail moves (off / travel). Never with the head still, while the camera turns fast,
        // or during a blink, a hold or a Zako Rush.
        float dh = 0.f;
        if (headOk && g_haveHead) dh = std::fabs(Wrap(headYaw - g_lastHead));
        if (dh > 10.f || (dt > 0.f && dh / dt < 8.f)) dh = 0.f;  // a jump (recentre) / tracking noise
        const bool toCam = cfg.vrAutoSnap == 2;
        const float roff = toCam ? off : Wrap(g_walkYaw - TargetYaw());
        if (cfg.vrRecenterGain > 0.f && dh > 0.f && !rush && g_bl.ph == Blink::kIdle && g_hack < 0.01f && std::fabs(roff) < thr &&
            (toCam ? !g_waitCam && !g_userAway && speed < 45.f : walking)) {
            const float step = std::min(std::fabs(roff), std::min(0.5f, cfg.vrRecenterGain) * dh);
            g_c = Wrap(g_c + (roff > 0.f ? step : -step));
        }
        // the off-hand stick (the one that turns in Zako Rush): left / right = turn by snap_angle, pulled back = face
        // where the game camera looks
        float d[2]{}, o[2]{};
        ctrl::Sticks(d, o);
        if (std::fabs(o[0]) < 0.3f && o[1] > -0.3f && now >= g_freeT) g_stickArmed = true;
        if (g_stickArmed && cfg.vrControllers && now >= g_freeT) {
            const float a = std::max(1.f, cfg.vrSnapAngle);
            if (o[0] > 0.7f) Request(kYaw, TargetYaw() + a, gy, "stick right"), g_stickArmed = false, g_userAway = true;
            else if (o[0] < -0.7f) Request(kYaw, TargetYaw() - a, gy, "stick left"), g_stickArmed = false, g_userAway = true;
            else if (o[1] < -0.7f) {  // (already facing it: nothing to do)
                if (std::fabs(Wrap(gy - TargetYaw())) >= 5.f) Request(kYaw, gy, gy, "stick back: face the action");
                g_stickArmed = false, g_userAway = g_waitCam = false;
            }
        }
    }
    g_lastGy = gy;
    g_lastHead = headYaw, g_haveHead = headOk;
    if (cfg.developer) {  // dev: the state once a second
        static double s_next = 0;
        if (now >= s_next) {
            s_next = now + 1.0;
            LOG("[comfort] view %.0f (%s) game %.0f (ref %.0f%s%s), walk %.0f%s, route P%d.%d, rush %d scene %d, hold %.2f, blink %.2f, "
                "state %s",
                g_c, g_applied ? "comfort" : "game", gy, g_refGy, g_waitCam ? ", waiting for it" : "", g_userAway ? ", stick away" : "",
                g_walkYaw, now - g_walkT < 1.0 ? " (walking)" : "", g_pt, g_ps, g_rush ? 1 : 0, g_scene, g_hack, g_bl.amt, st.c_str());
        }
    }
    StepBlink(dt, gy);
    g_consume = g_applied && want && !menu && !held && !rush;
    g_marksA = g_applied && want && !menu && !starting;
    Store(frameId, std::max(g_bl.amt, g_jumpAmt), vigOut);
}

Rot ViewFrame(const Rot& g) {
    if (g_aeLevel.load() && !g_applied) {
        // minigames: level, but the scene stays within minigame_look_limit of eye level: only the camera's pitch beyond that
        // is kept (Shinobu in the Window's high camera was "too much of a stretch", and the panel's icons, on a flat
        // plane moved down with the gaze, were far away and seen at a slant)
        const float lim = config::Get().vrMinigameLookLimit, p = Wrap(DegOf(g.pitch));
        return {RotOf(p - std::min(lim, std::max(-lim, p))), g.yaw, 0};
    }
    if (!g_applied) return g;
    if (g_holdView) return g_holdRot;  // grab_snap: the hold's view (after the blink)
    Rot f{0, RotOf(g_c), 0};  // level: no pitch, no roll
    if (g_hack > 0.001f && !g_holdSnap) {  // a hold: toward the game's own view of her, by the game's own blend
        f.yaw = RotOf(g_c + g_hack * Wrap(DegOf(g.yaw) - g_c));
        f.pitch = RotOf(g_hack * DegOf(g.pitch)), f.roll = RotOf(g_hack * DegOf(g.roll));
    }
    return f;
}

float VignetteForFrame(uint64_t frameId) {
    static float s_last = 0.f;  // (a frame without its value keeps the last one, not a flash of none)
    const Slot& s = g_ring[frameId % 64];
    if (s.frame.load() == frameId) s_last = s.vig.load();
    return BlinkRecent() ? s_last : 0.f;
}
float BlinkForFrame(uint64_t frameId) {
    const Slot& s = g_ring[frameId % 64];
    return s.frame.load() == frameId ? s.amt.load() : 0.f;
}
bool BlinkRecent() { return GetTickCount64() - g_lastBlinkT.load() < 500; }
bool ConsumesStick() { return g_consume.load(); }
bool Held() { return g_heldA.load(); }
bool HopViewZ(float& z) {
    if (!g_hop.on) return false;
    const double t = NowS() - g_hop.t0 - g_hop.wait;
    if (t >= g_hop.move + 0.1) {
        g_hop.on = false;
        return false;
    }
    const float k = float(std::min(1.0, std::max(0.0, t / g_hop.move)));
    z = g_hop.z0 + g_hop.dz * k;
    return true;
}
bool MarksActive() { return g_marksA.load(); }
bool InStageDoki() { return g_stageDoki.load(); }
bool BossEnding(uintptr_t pc) {
    const std::string st = StateName(pc);
    if (st == "PlayerResult" || st == "PlayerOutsideEndDemo") return true;  // (Plant's ending)
    if (!ue3::IsA(pc, "GG2BsBalloonPlayerController")) return false;
    static uint32_t s_state = 0;
    if (!s_state) {
        ue3::PropInfo p;
        const uintptr_t bg = ue3::FindObject("GG2Game.GG2BsBalloonGame", "Class");
        if (bg && ue3::FindProperty(bg, "m_stateId", p)) s_state = p.offset;
    }
    uint8_t id = 0;  // (GG2BsBalloonGame.IsEndDemo: m_stateId 9)
    const uintptr_t game = s_state ? BalloonGame() : 0;
    return game && Read(game + s_state, id) && id == 9;
}
void ViewLocation(V3& loc) {
    g_rawLoc = loc, g_haveRaw = true;
    if (g_bossView.load() && g_applied && g_anchorOn) loc = g_anchor;
    static bool s_on = false;
    static V3 s_at{};
    static double s_glideT = 0;
    if (g_aeSwoop.load()) {
        if (!s_on) s_on = true, s_at = loc, s_glideT = 0, LOG("[comfort] minigame: the camera swoop is held at (%.0f,%.0f,%.0f)", loc.x, loc.y, loc.z);
        loc = s_at;
    } else if (s_on) {
        if (s_glideT == 0) s_glideT = NowS(), LOG("[comfort] minigame: glide to (%.0f,%.0f,%.0f)", loc.x, loc.y, loc.z);
        const float t = float(NowS() - s_glideT) / 0.8f;
        const V3 d = loc - s_at;
        if (t >= 1.f || d.x * d.x + d.y * d.y + d.z * d.z > 2000.f * 2000.f) {
            s_on = false;
        } else {
            const float k = t * t * (3.f - 2.f * t);
            loc = {s_at.x + d.x * k, s_at.y + d.y * k, s_at.z + d.z * k};
        }
    }
}
std::string StateOf(uintptr_t obj) { return StateName(obj); }
void DevSetFadeMs(float ms) {
    g_devFadeMs = ms;
    LOG("[comfort] fade %s", ms >= 0.f ? (std::to_string(int(ms)) + " ms (dev)").c_str() : "from the ini");
}
}  // namespace ggdpvr::vr::comfort
