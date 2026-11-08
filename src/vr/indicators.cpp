#include "indicators.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../core/config.h"
#include "../core/game.h"
#include "../core/log.h"
#include "../ue3/objects.h"
#include "comfort_turn.h"

namespace ggdpvr::vr::ind {
namespace {
using game::Read;

constexpr float kDeg = kPi / 180.f;
XrVector3f QRotInv(const XrQuaternionf& q, XrVector3f v) {  // rotate by the conjugate of q
    const XrQuaternionf c{-q.x, -q.y, -q.z, q.w};
    const float tx = 2.f * (c.y * v.z - c.z * v.y), ty = 2.f * (c.z * v.x - c.x * v.z), tz = 2.f * (c.x * v.y - c.y * v.x);
    return {v.x + c.w * tx + (c.y * tz - c.z * ty), v.y + c.w * ty + (c.z * tx - c.x * tz), v.z + c.w * tz + (c.x * ty - c.y * tx)};
}
XrVector3f QRot(const XrQuaternionf& q, XrVector3f v) { return QRotInv({-q.x, -q.y, -q.z, q.w}, v); }
XrVector3f Sub(XrVector3f a, XrVector3f b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
XrVector3f Add(XrVector3f a, XrVector3f b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }

// --- the girls (game thread) ---
struct Girl {
    uintptr_t obj;
    bool shown;  // her chevron is up (hysteresis: it shows once she is out of view, hides once she is well inside it)
};
std::vector<Girl> g_girls;
double g_scanT = 0;
uint32_t g_offLoc = 0, g_offHidden = 0, g_hiddenMask = 0, g_offVital = 0;
bool g_offsOk = false;
// a girl who can't attack or be shot (yet / any more): spawning, defeated, running off, leaving
bool Gone(const std::string& st) {
    return st.empty() || st == "ST_Begin" || st.rfind("ST_Shouten", 0) == 0 || st.rfind("ST_Escape", 0) == 0 || st == "ST_ActionExit";
}
void Scan() {  // the zako girls of the level (twice a second: they are placed actors, reused)
    static std::unordered_map<uintptr_t, bool> s_isGirl;  // per class: a GG2ZakoGirl (or a subclass)? (cheaper than IsA per object)
    std::vector<Girl> next;
    for (int32_t i = 0, n = ue3::NumObjects(); i < n; ++i) {
        const uintptr_t o = ue3::ObjectAt(i);
        if (!o) continue;
        const uintptr_t cls = ue3::ClassOf(o);
        auto it = s_isGirl.find(cls);
        if (it == s_isGirl.end()) it = s_isGirl.emplace(cls, ue3::IsA(o, "GG2ZakoGirl")).first;
        if (!it->second) continue;
        if (ue3::PathName(o).find("Default__") != std::string::npos || ue3::ObjName(o) == "GirlBase") continue;  // (archetypes)
        bool shown = false;
        for (const Girl& g : g_girls)
            if (g.obj == o) shown = g.shown;
        next.push_back({o, shown});
    }
    g_girls.swap(next);
}

// --- this frame's girls and head (game thread), placed on the frame's first SetFrame ---
struct Pending {
    uint64_t frameId = 0;
    bool active = false, placed = false;
    xr::HeadSample hs{};
    std::vector<std::pair<size_t, V3>> girls;  // her index in g_girls, her torso (world)
};
Pending g_pend;
bool g_markerShown = false;
// the attention target (SetAttention) and whether the head looked at it
bool g_attOn = false, g_attShown = false;
ULONGLONG g_attSince = 0;  // it has been in view since (0: it isn't)
V3 g_att{};
std::atomic<bool> g_attLooked{false};

// --- the marks per frame (game thread writes, render thread reads) ---
struct Slot {
    uint64_t frameId = 0;
    int n = 0;
    Mark marks[kMaxMarks]{};
    bool marker = false;
    Mark mk{};
};
std::mutex g_slotMu;
Slot g_slots[8];
std::atomic<ULONGLONG> g_lastAnyT{0};

V3 g_devCam{};  // dev dump: the last camera location
}  // namespace

void Tick(uint64_t frameId, bool active, const xr::HeadSample& hs, bool headOk) {
    const auto& cfg = config::Get();
    g_pend.frameId = frameId, g_pend.active = active && headOk && (cfg.vrIndicators || cfg.vrGameMarker || g_attOn);
    g_pend.placed = false, g_pend.hs = hs;
    g_pend.girls.clear();
    if (!g_pend.active || !cfg.vrIndicators) return;
    if (!g_offsOk) {
        static bool s_tried = false;
        if (s_tried) return;
        s_tried = true;
        const uintptr_t actor = ue3::FindObject("Engine.Actor", "Class"), zako = ue3::FindObject("GG2Game.GG2ZakoGirl", "Class");
        ue3::PropInfo p;
        if (actor && ue3::FindProperty(actor, "Location", p)) g_offLoc = p.offset;
        if (actor && ue3::FindProperty(actor, "bHidden", p)) g_offHidden = p.offset, g_hiddenMask = p.bitMask;
        if (zako && ue3::FindProperty(zako, "m_vital", p)) g_offVital = p.offset;
        g_offsOk = g_offLoc && g_offHidden && g_hiddenMask;
        LOG("[ind] girl fields %s (vital %s)", g_offsOk ? "found" : "MISSING: no indicators", g_offVital ? "found" : "not found");
        if (!g_offsOk) return;
    }
    const double now = GetTickCount64() / 1000.0;
    if (now - g_scanT > 0.5) g_scanT = now, Scan();
    for (size_t i = 0; i < g_girls.size(); ++i) {
        const uintptr_t o = g_girls[i].obj;
        if (!ue3::IsObject(o)) continue;
        uint32_t hb = 0;
        if (Read(o + g_offHidden, hb) && (hb & g_hiddenMask)) continue;
        int32_t vital = 1;
        if (g_offVital && Read(o + g_offVital, vital) && vital <= 0) continue;
        if (Gone(comfort::StateOf(o))) continue;
        V3 l{};
        if (!Read(o + g_offLoc, l)) continue;
        g_pend.girls.push_back({i, {l.x, l.y, l.z + 80.f}});  // her torso (her head bone is ~124-135 uu above her feet)
    }
}

void SetFrame(uint64_t frameId, const V3& loc, const Rot& frame, const Rot& gameCam) {
    g_devCam = loc;
    if (g_pend.placed || g_pend.frameId != frameId) {
        static int s_miss = 0;
        if (!g_pend.placed && config::Get().developer && s_miss < 5) ++s_miss, LOG("[ind] frame mismatch: view %llu, girls %llu", frameId, g_pend.frameId);
        return;
    }
    g_pend.placed = true;
    const auto& cfg = config::Get();
    Slot s;
    s.frameId = frameId;
    if (g_pend.active) {
        // the world -> the recentred tracking space (XR axes, metres), as PostGetPlayerViewPoint composes the view: the
        // eye = loc + the frame's axes applied to the head pose x world_scale
        const Axes ax = AxesOf(frame);
        const float ws = cfg.vrWorldScale > 1.f ? cfg.vrWorldScale : 85.f;
        auto toRel = [&](V3 w) -> XrVector3f { return {Dot(w, ax.y) / ws, Dot(w, ax.z) / ws, -Dot(w, ax.x) / ws}; };
        const xr::HeadSample& hs = g_pend.hs;
        const XrPosef head = hs.rel;
        // in view of an eye: inside its FOV shrunk by `margin` degrees
        auto inEye = [&](XrVector3f p, int e, float margin) {
            const XrVector3f v = QRotInv(hs.eyeRel[e].orientation, Sub(p, hs.eyeRel[e].position));
            if (v.z >= -1e-4f) return false;
            const float h = std::atan2(v.x, -v.z), u = std::atan2(v.y, -v.z), m = margin * kDeg;
            const XrFovf& f = hs.fov[e];
            return h > f.angleLeft + m && h < f.angleRight - m && u > f.angleDown + m && u < f.angleUp - m;
        };
        auto inView = [&](XrVector3f p, float margin) { return inEye(p, 0, margin) || inEye(p, 1, margin); };
        const float ring = std::min(45.f, std::max(5.f, cfg.vrIndicatorRing)) * kDeg, dist = std::max(0.3f, cfg.vrUiDistance);
        // the bearing of a point around the head's forward (radians; straight behind: down)
        auto bearing = [&](XrVector3f p) {
            const XrVector3f h = QRotInv(head.orientation, Sub(p, head.position));
            return h.x * h.x + h.y * h.y < 1e-8f ? -kPi / 2 : std::atan2(h.y, h.x);
        };
        // its mark: on the ring (+ `out` radians further out), pointing outward along that bearing
        auto place = [&](float a, float out, Mark& m) {
            auto at = [&](float r) {
                return Add(head.position, QRot(head.orientation, {std::sin(r) * std::cos(a) * dist, std::sin(r) * std::sin(a) * dist,
                                                                  -std::cos(r) * dist}));
            };
            const XrVector3f c = at(ring + out), t = at(ring + out + 2.f * kDeg);
            m.at[0] = c.x, m.at[1] = c.y, m.at[2] = c.z, m.tip[0] = t.x, m.tip[1] = t.y, m.tip[2] = t.z;
        };
        const float size = std::max(0.3f, cfg.vrIndicatorSize) * kDeg;
        // out of view once outside the FOV shrunk by 2 deg; back in view once 7 deg inside it
        auto hysteresis = [&](bool shown, XrVector3f p) { return shown ? !inView(p, 7.f) : !inView(p, 2.f); };
        if (cfg.vrIndicators) {
            struct Cand {
                float d2;
                XrVector3f p;
            };
            std::vector<Cand> cands;
            for (auto& [i, w] : g_pend.girls) {
                if (i >= g_girls.size()) continue;
                const XrVector3f p = toRel(w - loc);  // (loc = the recentred origin; the head's offset is in hs)
                g_girls[i].shown = hysteresis(g_girls[i].shown, p);
                if (g_girls[i].shown) cands.push_back({p.x * p.x + p.y * p.y + p.z * p.z, p});
            }
            std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.d2 < b.d2; });  // nearest first
            // girls in about the same direction stack outward (a double chevron = two girls there), up to 3
            float placedA[kMaxMarks];
            int stack[kMaxMarks]{};
            int groups = 0;
            for (const Cand& c : cands) {
                if (s.n >= kMaxMarks) break;
                const float a = bearing(c.p);
                int g = -1;
                for (int i = 0; i < groups; ++i) {
                    float d = std::fabs(a - placedA[i]);
                    if (d > kPi) d = 2 * kPi - d;
                    if (d * std::sin(ring) < 1.2f * size) g = i;  // (closer than ~1 mark along the ring)
                }
                if (g < 0) placedA[groups] = a, g = groups++;
                if (stack[g] >= 3) continue;
                place(placedA[g], float(stack[g]++) * 0.75f * size, s.marks[s.n++]);
            }
        }
        if (g_attOn && s.n < kMaxMarks) {  // the attention target: looked at = well inside the view (as its chevron hides) for 0.15 s
            const XrVector3f p = toRel(g_att - loc);
            // (cones were too narrow at 20 and 35 deg; 70 deg sideways fired before you'd seen it, the chevron never showed)
            if (!inView(p, 7.f)) g_attSince = 0;
            else if (!g_attSince) g_attSince = GetTickCount64();
            g_attLooked = g_attSince && GetTickCount64() - g_attSince >= 150;
            g_attShown = hysteresis(g_attShown, p);
            if (g_attShown) place(bearing(p), 0.f, s.marks[s.n++]);
        } else {
            g_attLooked = false;
        }
        if (cfg.vrGameMarker) {  // where the game camera looks: 10 m along its forward
            const XrVector3f p = Add(head.position, toRel(AxesOf(gameCam).x * (10.f * ws)));
            g_markerShown = hysteresis(g_markerShown, p);
            if (g_markerShown) s.marker = true, place(bearing(p), 0.f, s.mk);
        }
    }
    if (s.n || s.marker) g_lastAnyT = GetTickCount64();
    if (cfg.developer) {  // dev: once a second
        static ULONGLONG s_next = 0;
        if (GetTickCount64() >= s_next) {
            s_next = GetTickCount64() + 1000;
            const XrFovf& f = g_pend.hs.fov[0];
            LOG("[ind] frame %llu: active %d, %zu live girls, %d marks, marker %d (eye 0 FOV %.0f/%.0f/%.0f/%.0f deg)", frameId,
                g_pend.active ? 1 : 0, g_pend.girls.size(), s.n, s.marker ? 1 : 0, f.angleLeft / kDeg, f.angleRight / kDeg,
                f.angleUp / kDeg, f.angleDown / kDeg);
        }
    }
    std::lock_guard l(g_slotMu);
    g_slots[frameId % 8] = s;
}

int MarksForFrame(uint64_t frameId, Mark* out, int max, bool& haveMarker, Mark& marker) {
    std::lock_guard l(g_slotMu);
    const Slot& s = g_slots[frameId % 8];
    haveMarker = false;
    if (s.frameId != frameId) return 0;
    const int n = std::min(max, s.n);
    for (int i = 0; i < n; ++i) out[i] = s.marks[i];
    if (s.marker) haveMarker = true, marker = s.mk;
    return n;
}
bool AnyRecent() { return GetTickCount64() - g_lastAnyT.load() < 500; }
void SetAttention(bool on, const V3& world) {
    if (on && !g_attOn) g_attShown = false, g_attSince = 0;
    g_attOn = on, g_att = world;
    if (!on) g_attLooked = false;
}
bool AttentionLooked() { return g_attLooked.load(); }

void DevDefeatLive(const std::string& how) {
    // default: the girl's own Doki-Doki bomb reaction (GG2ZakoGirl.RequestReactionDkdkBomb), as the real bomb does;
    // any other word: GotoState(that state)
    const bool bomb = how.empty() || how.rfind("bomb", 0) == 0;  // "bomb" / "bomb<reaction level>"
    const int32_t level = bomb && how.size() > 4 ? atoi(how.c_str() + 4) : 3;
    int32_t nameIdx = -1;
    if (!bomb)
        for (int32_t i = 0, n = ue3::NumNames(); i < n; ++i)
            if (ue3::Name(i) == how) { nameIdx = i; break; }
    const uintptr_t fn = bomb ? ue3::FindObject("GG2Game.GG2ZakoGirl.RequestReactionDkdkBomb", "Function")
                              : ue3::FindObject("Core.Object.GotoState", "Function");
    const uintptr_t actor = ue3::FindObject("Engine.Actor", "Class");
    ue3::PropInfo pHidden;
    if ((!bomb && nameIdx < 0) || !fn || !actor || !ue3::FindProperty(actor, "bHidden", pHidden)) {
        LOG("[ind] bombgirls: %s / bHidden not found", bomb ? "RequestReactionDkdkBomb" : how.c_str());
        return;
    }
    static bool s_params = false;
    if (bomb && !s_params) {
        s_params = true;
        std::string ps;
        for (const auto& p : ue3::Properties(fn)) ps += " " + p.type + " " + p.name + "@" + std::to_string(p.offset);
        LOG("[ind] RequestReactionDkdkBomb params:%s", ps.empty() ? " (none)" : ps.c_str());
    }
    int n = 0;
    for (int32_t i = 0, cnt = ue3::NumObjects(); i < cnt; ++i) {
        const uintptr_t o = ue3::ObjectAt(i);
        if (!o || !ue3::IsA(o, "GG2ZakoGirl") || ue3::PathName(o).find("Default__") != std::string::npos) continue;
        uint32_t hb = 0;
        Read(o + pHidden.offset, hb);
        if ((hb & pHidden.bitMask) || Gone(comfort::StateOf(o))) continue;
        uint8_t parms[0x80]{};
        if (!bomb) memcpy(parms, &nameIdx, 4);  // NewState (FName index, number 0); Label = None
        else memcpy(parms, &level, 4);          // reactionLevel
        uintptr_t vt = 0, pe = 0;
        Read(o, vt), Read(vt + game::rva::ProcessEvent_VSlot, pe);
        using PEFn = void(__fastcall*)(uintptr_t, uintptr_t, void*, void*);
        reinterpret_cast<PEFn>(pe)(o, fn, parms, nullptr);
        ++n;
    }
    LOG("[ind] bombgirls: %d live girl(s) -> %s", n, bomb ? ("RequestReactionDkdkBomb(" + std::to_string(level) + ")").c_str() : how.c_str());
}

void DevDumpGirls() {
    const V3 cam = g_devCam;
    const uintptr_t actor = ue3::FindObject("Engine.Actor", "Class");
    ue3::PropInfo pLoc, pHidden;
    const bool okLoc = actor && ue3::FindProperty(actor, "Location", pLoc);
    const bool okHid = actor && ue3::FindProperty(actor, "bHidden", pHidden);
    int n = 0, live = 0;
    for (int32_t i = 0, cnt = ue3::NumObjects(); i < cnt; ++i) {
        const uintptr_t o = ue3::ObjectAt(i);
        if (!o || !ue3::IsA(o, "GG2ZakoGirl") || ue3::PathName(o).find("Default__") != std::string::npos) continue;
        ++n;
        V3 l{};
        if (okLoc) Read(o + pLoc.offset, l);
        uint32_t hb = 0;
        if (okHid) Read(o + pHidden.offset, hb);
        const std::string st = comfort::StateOf(o);
        const bool hidden = (hb & pHidden.bitMask) != 0;
        if (hidden && st == "ST_Begin") continue;  // (not spawned yet: most of them)
        if (!hidden && !Gone(st)) ++live;
        const V3 d = l - cam;
        LOG("[ind] girl %s: state %s, hidden %d, at (%.0f,%.0f,%.0f), %.1f m from the camera", ue3::ObjName(o).c_str(), st.c_str(),
            hidden ? 1 : 0, l.x, l.y, l.z, std::sqrt(Dot(d, d)) / 85.f);
    }
    LOG("[ind] %d zako girls, %d live", n, live);
}
}  // namespace ggdpvr::vr::ind
