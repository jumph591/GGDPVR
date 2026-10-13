#include "controllers.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

#include "../core/config.h"
#include "../core/log.h"
#include "../xr/xr_input.h"
#include "../xr/xr_screen.h"
#include "menu_pointer.h"
#include "vpad.h"
#include "ui_layer.h"
#include "head_tracking.h"

namespace ggdpvr::vr::ctrl {
namespace {
std::mutex g_mu;
bool g_haveAim = false;
bool g_aimAsleep = false;  // the aim controller's pose went inactive; its last aim is kept
Aim g_aim;
ULONGLONG g_aimTime = 0;
bool g_fake = false;
std::atomic<float> g_hitDist{0.f};
std::atomic<bool> g_trigger{false};
std::atomic<bool> g_grip{false};  // dominant grip, latched
std::atomic<float> g_stick[4]{};  // dominant x, y, other x, y
std::atomic<bool> g_offTrigger{false};
float g_fakeYaw = 0, g_fakePitch = 0;
float g_fakeHand[3] = {0, 0, 0};

// analog -> digital with hysteresis
struct Latch {
    bool on = false;
    bool Update(float v, float onAt, float offAt) { return on = on ? v > offAt : v > onAt; }
};
Latch g_inTrigger[2], g_inGrip[2];  // per hand (0 left, 1 right)
// Stick -> D-pad for menus: one direction at a time (dominant axis; a diagonal flick pressed two directions),
// and a direction only re-arms after the stick has come back near the centre (a flick's spring-back bounced over
// the threshold again = a second press). Both made menus move two items per flick.
struct DpadState { int dir = -1; bool armed = true; } g_dpOff, g_dpDom;
uint16_t StickToDpad(DpadState& g_dp, float x, float y) {
    const float mag = std::fmax(std::fabs(x), std::fabs(y));
    if (mag < 0.25f) g_dp.armed = true;
    if (g_dp.dir >= 0) {  // held: release when the stick drops back
        if (mag < 0.45f) g_dp.dir = -1;
    } else if (g_dp.armed && mag > 0.75f) {
        g_dp.dir = std::fabs(y) >= std::fabs(x) ? (y > 0 ? 0 : 1) : (x < 0 ? 2 : 3);
        g_dp.armed = false;
    }
    static const uint16_t kBits[4] = {vpad::kUp, vpad::kDown, vpad::kLeft, vpad::kRight};
    return g_dp.dir >= 0 ? kBits[g_dp.dir] : 0;
}
bool g_prevRecenter = false;
bool g_prevHudBtn = false;  // the HUD toggle button (edge)

// Speed-adaptive low-pass filter for the aim orientation (One Euro style): slow, small movements (hand tremor)
// are smoothed strongly, fast movements pass almost unfiltered, so aiming stays responsive.
XrQuaternionf g_smRel{0, 0, 0, 1}, g_smRaw{0, 0, 0, 1};
bool g_smInit = false;
ULONGLONG g_smLast = 0;
XrQuaternionf Nlerp(XrQuaternionf a, XrQuaternionf b, float t) {
    const float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0) b = {-b.x, -b.y, -b.z, -b.w};
    XrQuaternionf r{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
    const float n = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
    return {r.x / n, r.y / n, r.z / n, r.w / n};
}
float AngleDeg(XrQuaternionf a, XrQuaternionf b) {
    const float d = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    return 2.f * std::acos(std::fmin(1.f, d)) * 57.2958f;
}
void SmoothAim(XrPosef& rel, XrPosef& raw) {
    const float strength = config::Get().vrAimSmoothing;
    const ULONGLONG now = GetTickCount64();
    if (strength <= 0.f || !g_smInit || now - g_smLast > 250) {
        g_smRel = rel.orientation, g_smRaw = raw.orientation, g_smInit = true, g_smLast = now;
        return;
    }
    const float dt = std::fmax(0.004f, float(now - g_smLast) / 1000.f);
    g_smLast = now;
    const float speed = AngleDeg(g_smRel, rel.orientation) / dt;            // deg/s
    const float cutoff = (1.2f + 0.08f * speed) / strength;                 // Hz: ~1.2 at rest, wide open when moving
    const float tau = 1.f / (2.f * 3.14159265f * cutoff);
    const float a = 1.f / (1.f + tau / dt);
    g_smRel = Nlerp(g_smRel, rel.orientation, a), g_smRaw = Nlerp(g_smRaw, raw.orientation, a);
    rel.orientation = g_smRel, raw.orientation = g_smRaw;
}

XrQuaternionf FromYawPitch(float yawDeg, float pitchDeg) {
    const float y = yawDeg * 3.14159265f / 360.f, p = pitchDeg * 3.14159265f / 360.f;
    const XrQuaternionf qy{0, std::sin(y), 0, std::cos(y)}, qp{std::sin(p), 0, 0, std::cos(p)};
    return {qy.w * qp.x + qy.x * qp.w + qy.y * qp.z - qy.z * qp.y, qy.w * qp.y - qy.x * qp.z + qy.y * qp.w + qy.z * qp.x,
            qy.w * qp.z + qy.x * qp.y - qy.y * qp.x + qy.z * qp.w, qy.w * qp.w - qy.x * qp.x - qy.y * qp.y - qy.z * qp.z};
}
}  // namespace

void UpdateFromXr() {
    const auto& cfg = config::Get();
    xr::input::Snapshot s;
    if (!cfg.vrControllers || !xr::input::Get(s)) {
        vpad::SetFromControllers({});
        g_trigger = false;
        std::lock_guard l(g_mu);
        if (!g_fake) g_haveAim = false;
        return;
    }
    const int dom = cfg.vrDominantHand, off = 1 - dom;
    const xr::input::Hand &D = s.hand[dom], &O = s.hand[off];
    // [controls] (0.9.106): each game button is pressed by the controller inputs listed for it. Every input's state
    // first - triggers and grips are analog: pressed past a threshold, released a little below it (no flicker).
    using namespace config;
    bool pressed[2][6]{};
    float analog[2][6]{};
    for (int h = 0; h < 2; ++h) {
        const xr::input::Hand& H = s.hand[h];
        analog[h][kTrigger] = H.trigger, pressed[h][kTrigger] = g_inTrigger[h].Update(H.trigger, 0.55f, 0.45f);
        analog[h][kGrip] = H.grip, pressed[h][kGrip] = g_inGrip[h].Update(H.grip, 0.6f, 0.5f);
        pressed[h][kStickClick] = H.stickClick, pressed[h][kButtonLower] = H.lower;
        pressed[h][kButtonUpper] = H.upper, pressed[h][kMenu] = H.menu;
        for (int b = kStickClick; b <= kMenu; ++b) analog[h][b] = pressed[h][b] ? 1.f : 0.f;
    }
    auto handOf = [&](unsigned char in) {
        const int h = in / 8;
        return h == kMain ? dom : h == kOff ? off : h == kLeft ? 0 : 1;
    };
    bool consumed[2][6]{};
    // the HUD toggle (stages / boss battles in play, where A, B and the stick click send the game nothing - checked
    // with the key bits): there its inputs only toggle the HUD; elsewhere they do what they're mapped to
    {
        bool hudHeld = false;
        int hudHand = 1;
        for (unsigned char in : cfg.vrControls[kHudToggle])
            if (pressed[handOf(in)][in % 8]) hudHeld = true, hudHand = handOf(in);
        const bool ctx = !cfg.vrControls[kHudToggle].empty() && ui::HudToggleContext() && !pointer::Active();
        if (ctx && hudHeld && !g_prevHudBtn) {
            ui::ToggleHud();
            if (cfg.vrHaptics) xr::input::Rumble(hudHand, 0.25f, 0.03f);
        }
        g_prevHudBtn = hudHeld;
        if (ctx)
            for (unsigned char in : cfg.vrControls[kHudToggle]) consumed[handOf(in)][in % 8] = true;
    }
    auto held = [&](int action, int* hand = nullptr) {
        for (unsigned char in : cfg.vrControls[action]) {
            const int h = handOf(in), b = in % 8;
            if (pressed[h][b] && !consumed[h][b]) {
                if (hand) *hand = h;
                return true;
            }
        }
        return false;
    };
    auto value = [&](int action) {  // analog: the furthest-pressed input (a button = fully pressed)
        float v = 0.f;
        for (unsigned char in : cfg.vrControls[action])
            if (!consumed[handOf(in)][in % 8]) v = std::fmax(v, analog[handOf(in)][in % 8]);
        return v;
    };
    vpad::State p;
    const bool wasTrigger = g_trigger.load();
    int shootHand = dom;
    g_trigger = held(kShoot, &shootHand);
    if (g_trigger && !wasTrigger && cfg.vrHaptics) xr::input::Rumble(shootHand, pointer::Active() ? 0.2f : 0.35f, 0.025f);
    if (g_trigger && !pointer::Active()) p.buttons |= vpad::kX;  // in menus the shoot button clicks instead (menu_pointer)
    g_grip = held(kZoom);
    // an interruption (pause, continue prompt, Mom is coming...) drops the zoom: the zoom input then sends nothing until
    // it is let go and pressed again (0.9.110)
    static bool s_zoomBlocked = false;
    if (vr::ZoomInterrupted()) s_zoomBlocked = true;
    else if (!g_grip) s_zoomBlocked = false;
    // minigames: the zoom button is the touch (requests, gestures - the game's own touch also answers its zoom key), not RB
    if (g_grip && !s_zoomBlocked && !pointer::TouchModeActive()) p.buttons |= vpad::kRB;
    if (held(kSightSpeed)) p.buttons |= vpad::kLB;
    p.rt = value(kSpecialRequests);
    g_offTrigger = held(kSpecialRequests);
    g_stick[0] = D.stick.x, g_stick[1] = D.stick.y, g_stick[2] = O.stick.x, g_stick[3] = O.stick.y;
    if (held(kConfirm)) p.buttons |= vpad::kA;
    if (held(kCancel)) p.buttons |= vpad::kB;
    if (held(kDokiDokiMode)) p.buttons |= vpad::kY;
    if (held(kMomIsComing)) p.buttons |= vpad::kBack;
    if (held(kPause)) p.buttons |= vpad::kStart;
    // The off-hand stick drives the D-pad (route choice, menus). Sending it as the analog stick as well made menus
    // move two steps per flick; the analog stick is only needed by Fantasize's camera.
    if (ui::FantasizeActive()) p.lx = O.stick.x, p.ly = O.stick.y;
    // The pause menu / flat menus also step with the dominant stick: as a raw right stick the game's own stick
    // handling moved two items per flick (seen in a log), so there it becomes a debounced D-pad too.
    if (pointer::SticksAsDpad()) {
        p.buttons |= StickToDpad(g_dpDom, D.stick.x, D.stick.y);
    } else {
        StickToDpad(g_dpDom, 0, 0);
        p.rx = D.stick.x, p.ry = D.stick.y;
    }
    // menus on the pointer (Score Attack, save slots, ...): the aim-hand stick scrolls lists like a mouse wheel
    pointer::Scroll(D.stick.y, pointer::SticksAsDpad());
    if (held(kR3)) p.buttons |= vpad::kR3;
    p.buttons |= StickToDpad(g_dpOff, O.stick.x, O.stick.y);
    vpad::SetFromControllers(p);
    const bool recenter = held(kRecenter);
    if (recenter && !g_prevRecenter) {
        xr::RequestRecenter();
        LOG("[ctrl] recenter (controller button)");
    }
    g_prevRecenter = recenter;
    std::lock_guard l(g_mu);
    if (g_fake) return;
    // An idle controller goes to sleep (or the runtime switches to hand tracking) and its pose turns inactive: the last
    // aim is kept, so controller aiming stays on and the reticle waits where it was - it used to drop to the game's own
    // 2D sight and back (0.9.59: "when you idle the default reticle comes back and our reticle disappears")
    if (D.active) {
        XrPosef rel = D.aimRel, raw = D.aimRaw;
        SmoothAim(rel, raw);
        g_aim = {raw, rel}, g_haveAim = true;
        if (g_aimAsleep) g_aimAsleep = false, LOG("[ctrl] aim controller active again");
    } else if (g_haveAim && !g_aimAsleep) {
        g_aimAsleep = true;
        LOG("[ctrl] aim controller inactive (idle / asleep): keeping its last aim");
    }
    g_aimTime = GetTickCount64();  // the controllers are being read (stale only when the XR frames stop)
}

bool GetAim(Aim& out) {
    std::lock_guard l(g_mu);
    if (g_fake) {
        // simulated right hand: 20 cm right, 25 cm down, 20 cm forward of the head, pointing yaw/pitch
        out.rel = {FromYawPitch(g_fakeYaw, g_fakePitch), {g_fakeHand[0], g_fakeHand[1], g_fakeHand[2]}};
        out.raw = out.rel;
        return true;
    }
    if (!g_haveAim || GetTickCount64() - g_aimTime > 500) return false;
    out = g_aim;
    return true;
}
void SetHitDistance(float m) { g_hitDist = m; }
void HitPulse() {
    static int s_logs = 0;
    if (s_logs++ < 5) LOG("[ctrl] hit -> vibration");
    if (config::Get().vrHaptics) xr::input::Rumble(config::Get().vrDominantHand, 0.8f, 0.06f);
}
float HitDistance() { return g_hitDist.load(); }
std::atomic<unsigned long long> g_fakeTriggerUntil{0};  // dev: !faketrigger
bool TriggerHeld() { return g_trigger.load() || GetTickCount64() < g_fakeTriggerUntil.load(); }
void FakeTrigger(unsigned ms) { g_fakeTriggerUntil = GetTickCount64() + ms; LOG("[ctrl] simulated trigger for %u ms", ms); }
std::atomic<unsigned long long> g_fakeGripUntil{0};  // dev: !fakegrip
void Sticks(float dom[2], float off[2]) {
    dom[0] = g_stick[0].load(), dom[1] = g_stick[1].load(), off[0] = g_stick[2].load(), off[1] = g_stick[3].load();
}
bool OffTriggerHeld() { return g_offTrigger.load(); }
bool GripHeld() { return g_grip.load() || GetTickCount64() < g_fakeGripUntil.load(); }
void FakeGrip(unsigned ms) { g_fakeGripUntil = GetTickCount64() + ms; LOG("[ctrl] simulated grip for %u ms", ms); }
bool AimActive() {
    Aim a;
    return config::Get().vrControllers && GetAim(a);
}
void SetFakeHand(float x, float y, float z) {
    std::lock_guard l(g_mu);
    g_fakeHand[0] = x, g_fakeHand[1] = y, g_fakeHand[2] = z;
    LOG("[ctrl] simulated hand at (%.2f, %.2f, %.2f) m", x, y, z);
}
void FakeHand(float out[3]) {
    std::lock_guard l(g_mu);
    memcpy(out, g_fakeHand, sizeof g_fakeHand);
}
void SetFakeQuiet(float yawDeg, float pitchDeg) {
    std::lock_guard l(g_mu);
    g_fake = true, g_fakeYaw = yawDeg, g_fakePitch = pitchDeg;
}
void SetFake(bool on, float yawDeg, float pitchDeg) {
    std::lock_guard l(g_mu);
    g_fake = on, g_fakeYaw = yawDeg, g_fakePitch = pitchDeg;
    LOG("[ctrl] simulated controller %s (yaw %.1f pitch %.1f)", on ? "ON" : "off", yawDeg, pitchDeg);
}
}  // namespace ggdpvr::vr::ctrl
