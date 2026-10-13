#include "xr_input.h"

#include <cstring>
#include <initializer_list>
#include <mutex>
#include <string>
#include <vector>

#include "../core/log.h"
#include "xr_screen.h"

namespace ggdpvr::xr::input {
namespace {
XrInstance g_inst = XR_NULL_HANDLE;
XrActionSet g_set = XR_NULL_HANDLE;
XrAction g_aim = XR_NULL_HANDLE, g_trigger = XR_NULL_HANDLE, g_grip = XR_NULL_HANDLE, g_stick = XR_NULL_HANDLE,
         g_stickClick = XR_NULL_HANDLE, g_lower = XR_NULL_HANDLE, g_upper = XR_NULL_HANDLE, g_menu = XR_NULL_HANDLE,
         g_haptic = XR_NULL_HANDLE, g_pad = XR_NULL_HANDLE, g_padClick = XR_NULL_HANDLE;
// Controllers with a round trackpad instead of face buttons (Windows Mixed Reality, HTC Vive wands): a trackpad press
// counts as a face button by where it's pressed - top = upper button, bottom = lower button; on Vive wands, whose
// trackpad is also the thumbstick, the centre = thumbstick click. The zone is kept until the press ends.
bool g_padIsStick[2]{};  // the runtime picked the Vive wand profile for this hand
int g_padZone[2] = {-1, -1};
struct Buzz { bool pending = false; float amp = 0, sec = 0; };
Buzz g_buzz[2];  // guarded by g_mu
XrPath g_hand[2]{};
XrSpace g_aimSpace[2]{};
std::mutex g_mu;
Snapshot g_snap;
uint64_t g_seq = 0;
int g_syncFailLogs = 0;
bool g_hpExt = false;  // the runtime enabled XR_EXT_hp_mixed_reality_controller

XrPath P(const char* s) {
    XrPath p = XR_NULL_PATH;
    xrStringToPath(g_inst, s, &p);
    return p;
}
bool MakeAction(const char* name, const char* loc, XrActionType type, bool perHand, XrAction& out) {
    XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
    strcpy_s(ci.actionName, name);
    strcpy_s(ci.localizedActionName, loc);
    ci.actionType = type;
    if (perHand) ci.countSubactionPaths = 2, ci.subactionPaths = g_hand;
    const XrResult r = xrCreateAction(g_set, &ci, &out);
    if (XR_FAILED(r)) LOG("[xr.input] xrCreateAction(%s) failed: %d", name, int(r));
    return XR_SUCCEEDED(r);
}
XrPosef Rel(const XrPosef& p, const XrPosef& ref) {
    auto qmul = [](XrQuaternionf a, XrQuaternionf b) {
        return XrQuaternionf{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                             a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
    };
    const XrQuaternionf inv{-ref.orientation.x, -ref.orientation.y, -ref.orientation.z, ref.orientation.w};
    const XrVector3f d{p.position.x - ref.position.x, p.position.y - ref.position.y, p.position.z - ref.position.z};
    // rotate d by inv: q * (d,0) * q^-1
    const XrQuaternionf v{d.x, d.y, d.z, 0};
    const XrQuaternionf r = qmul(qmul(inv, v), XrQuaternionf{-inv.x, -inv.y, -inv.z, inv.w});
    return {qmul(inv, p.orientation), {r.x, r.y, r.z}};
}
float GetFloat(XrSession s, XrAction a, XrPath sub) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a, gi.subactionPath = sub;
    XrActionStateFloat st{XR_TYPE_ACTION_STATE_FLOAT};
    return XR_SUCCEEDED(xrGetActionStateFloat(s, &gi, &st)) && st.isActive ? st.currentState : 0.f;
}
bool GetBool(XrSession s, XrAction a, XrPath sub) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a, gi.subactionPath = sub;
    XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
    return XR_SUCCEEDED(xrGetActionStateBoolean(s, &gi, &st)) && st.isActive && st.currentState;
}
XrVector2f GetVec2(XrSession s, XrAction a, XrPath sub) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a, gi.subactionPath = sub;
    XrActionStateVector2f st{XR_TYPE_ACTION_STATE_VECTOR2F};
    return XR_SUCCEEDED(xrGetActionStateVector2f(s, &gi, &st)) && st.isActive ? st.currentState : XrVector2f{};
}
// one controller type's bindings: hands 'L' / 'R' / 'B' (both) + the input path under /user/hand/<hand>/
struct Bind {
    XrAction* action;
    char hands;
    const char* path;
};
bool Suggest(const char* profile, std::initializer_list<Bind> binds) {
    std::vector<XrActionSuggestedBinding> b;
    for (const Bind& x : binds) {
        for (int h = 0; h < 2; ++h) {
            if (x.hands != 'B' && x.hands != (h ? 'R' : 'L')) continue;
            b.push_back({*x.action, P((std::string(h ? "/user/hand/right/" : "/user/hand/left/") + x.path).c_str())});
        }
    }
    XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    sb.interactionProfile = P(profile);
    sb.countSuggestedBindings = uint32_t(b.size());
    sb.suggestedBindings = b.data();
    const XrResult r = xrSuggestInteractionProfileBindings(g_inst, &sb);
    if (XR_SUCCEEDED(r)) LOG("[xr.input] bindings for %s: ok", profile);
    else LOG("[xr.input] bindings for %s: rejected (%d) - skipped", profile, int(r));
    return XR_SUCCEEDED(r);
}
}  // namespace

bool Create(XrInstance instance, XrSession session) {
    Destroy();
    g_inst = instance;
    g_hpExt = xr::HasHpControllerExt();
    const bool frameExt = xr::HasFrameControllerExt();
    g_hand[0] = P("/user/hand/left");
    g_hand[1] = P("/user/hand/right");
    XrActionSetCreateInfo si{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(si.actionSetName, "ggdpvr_gameplay");
    strcpy_s(si.localizedActionSetName, "GGDPVR gameplay");
    if (XR_FAILED(xrCreateActionSet(instance, &si, &g_set))) {
        LOG("[xr.input] action set creation failed");
        return false;
    }
    bool ok = MakeAction("aim", "Aim", XR_ACTION_TYPE_POSE_INPUT, true, g_aim) &&
              MakeAction("trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT, true, g_trigger) &&
              MakeAction("grip", "Grip", XR_ACTION_TYPE_FLOAT_INPUT, true, g_grip) &&
              MakeAction("stick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT, true, g_stick) &&
              MakeAction("stick_click", "Thumbstick click", XR_ACTION_TYPE_BOOLEAN_INPUT, true, g_stickClick) &&
              MakeAction("button_lower", "Lower face button (A / X)", XR_ACTION_TYPE_BOOLEAN_INPUT, true, g_lower) &&
              MakeAction("button_upper", "Upper face button (B / Y)", XR_ACTION_TYPE_BOOLEAN_INPUT, true, g_upper) &&
              MakeAction("menu", "Menu button", XR_ACTION_TYPE_BOOLEAN_INPUT, true, g_menu) &&
              MakeAction("trackpad", "Trackpad position", XR_ACTION_TYPE_VECTOR2F_INPUT, true, g_pad) &&
              MakeAction("trackpad_click", "Trackpad press", XR_ACTION_TYPE_BOOLEAN_INPUT, true, g_padClick) &&
              MakeAction("haptic", "Vibration", XR_ACTION_TYPE_VIBRATION_OUTPUT, true, g_haptic);
    if (!ok) return false;
    // every controller type the mod knows: the runtime uses the one matching the controllers in your hands (or
    // translates the nearest one); a type this runtime doesn't know is rejected and skipped
    int accepted = 0;
    accepted += Suggest("/interaction_profiles/oculus/touch_controller",
                        {{&g_aim, 'B', "input/aim/pose"}, {&g_trigger, 'B', "input/trigger/value"},
                         {&g_grip, 'B', "input/squeeze/value"}, {&g_stick, 'B', "input/thumbstick"},
                         {&g_stickClick, 'B', "input/thumbstick/click"}, {&g_lower, 'L', "input/x/click"},
                         {&g_upper, 'L', "input/y/click"}, {&g_lower, 'R', "input/a/click"},
                         {&g_upper, 'R', "input/b/click"}, {&g_menu, 'L', "input/menu/click"},
                         {&g_haptic, 'B', "output/haptic"}});
    accepted += Suggest("/interaction_profiles/valve/index_controller",
                        {{&g_aim, 'B', "input/aim/pose"}, {&g_trigger, 'B', "input/trigger/value"},
                         {&g_grip, 'B', "input/squeeze/value"}, {&g_stick, 'B', "input/thumbstick"},
                         {&g_stickClick, 'B', "input/thumbstick/click"}, {&g_lower, 'B', "input/a/click"},
                         {&g_upper, 'B', "input/b/click"}, {&g_menu, 'B', "input/trackpad/force"},
                         {&g_haptic, 'B', "output/haptic"}});
    accepted += Suggest("/interaction_profiles/microsoft/motion_controller",
                        {{&g_aim, 'B', "input/aim/pose"}, {&g_trigger, 'B', "input/trigger/value"},
                         {&g_grip, 'B', "input/squeeze/click"}, {&g_stick, 'B', "input/thumbstick"},
                         {&g_stickClick, 'B', "input/thumbstick/click"}, {&g_pad, 'B', "input/trackpad"},
                         {&g_padClick, 'B', "input/trackpad/click"}, {&g_menu, 'B', "input/menu/click"},
                         {&g_haptic, 'B', "output/haptic"}});
    if (g_hpExt)  // HP Reverb G2 controllers: only with XR_EXT_hp_mixed_reality_controller
        accepted += Suggest("/interaction_profiles/hp/mixed_reality_controller",
                            {{&g_aim, 'B', "input/aim/pose"}, {&g_trigger, 'B', "input/trigger/value"},
                             {&g_grip, 'B', "input/squeeze/value"}, {&g_stick, 'B', "input/thumbstick"},
                             {&g_stickClick, 'B', "input/thumbstick/click"}, {&g_lower, 'L', "input/x/click"},
                             {&g_upper, 'L', "input/y/click"}, {&g_lower, 'R', "input/a/click"},
                             {&g_upper, 'R', "input/b/click"}, {&g_menu, 'B', "input/menu/click"},
                             {&g_haptic, 'B', "output/haptic"}});
    if (frameExt)  // Steam Frame controllers (Valve's own Touch emulation uses the same layout); right: A B X Y + Menu,
        // left: D-pad + View; each has its own System (Steam) button, which stays SteamVR's
        accepted += Suggest("/interaction_profiles/valve/frame_controller_valve",
                            {{&g_aim, 'B', "input/aim/pose"}, {&g_trigger, 'B', "input/trigger/value"},
                             {&g_grip, 'B', "input/squeeze/value"}, {&g_stick, 'B', "input/thumbstick"},
                             {&g_stickClick, 'B', "input/thumbstick/click"}, {&g_lower, 'L', "input/dpad_down/click"},
                             {&g_upper, 'L', "input/dpad_up/click"}, {&g_upper, 'L', "input/dpad_left/click"},
                             {&g_upper, 'L', "input/dpad_right/click"}, {&g_lower, 'R', "input/a/click"},
                             {&g_upper, 'R', "input/b/click"}, {&g_upper, 'R', "input/x/click"},
                             {&g_upper, 'R', "input/y/click"}, {&g_menu, 'L', "input/view/click"},
                             {&g_menu, 'R', "input/menu/click"}, {&g_haptic, 'B', "output/haptic"}});
    accepted += Suggest("/interaction_profiles/htc/vive_controller",
                        {{&g_aim, 'B', "input/aim/pose"}, {&g_trigger, 'B', "input/trigger/value"},
                         {&g_grip, 'B', "input/squeeze/click"}, {&g_stick, 'B', "input/trackpad"},
                         {&g_pad, 'B', "input/trackpad"}, {&g_padClick, 'B', "input/trackpad/click"},
                         {&g_menu, 'B', "input/menu/click"}, {&g_haptic, 'B', "output/haptic"}});
    accepted += Suggest("/interaction_profiles/khr/simple_controller",
                        {{&g_aim, 'B', "input/aim/pose"}, {&g_trigger, 'B', "input/select/click"},
                         {&g_menu, 'B', "input/menu/click"}, {&g_haptic, 'B', "output/haptic"}});
    if (!accepted) {
        LOG("[xr.input] no controller bindings accepted");
        return false;
    }
    for (int h = 0; h < 2; ++h) {
        XrActionSpaceCreateInfo ai{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        ai.action = g_aim, ai.subactionPath = g_hand[h], ai.poseInActionSpace.orientation.w = 1;
        if (XR_FAILED(xrCreateActionSpace(session, &ai, &g_aimSpace[h]))) LOG("[xr.input] aim space %d failed", h);
    }
    XrSessionActionSetsAttachInfo at{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    at.countActionSets = 1, at.actionSets = &g_set;
    const XrResult r = xrAttachSessionActionSets(session, &at);
    LOG("[xr.input] controller actions %s (%d controller types)", XR_SUCCEEDED(r) ? "attached" : "ATTACH FAILED", accepted);
    return XR_SUCCEEDED(r);
}

void Destroy() {
    for (auto& sp : g_aimSpace)
        if (sp) xrDestroySpace(sp), sp = XR_NULL_HANDLE;
    if (g_set) xrDestroyActionSet(g_set);  // destroys its actions
    g_set = XR_NULL_HANDLE;
    g_aim = g_trigger = g_grip = g_stick = g_stickClick = g_lower = g_upper = g_menu = g_haptic = XR_NULL_HANDLE;
    g_pad = g_padClick = XR_NULL_HANDLE;
    g_padIsStick[0] = g_padIsStick[1] = false;
    g_padZone[0] = g_padZone[1] = -1;
    std::lock_guard l(g_mu);
    g_snap = {};
}

void Sync(XrSession session, XrSpace local, XrTime time, const XrPosef& ref) {
    if (g_haptic) {  // pending vibration requests
        Buzz bz[2];
        {
            std::lock_guard l(g_mu);
            bz[0] = g_buzz[0], bz[1] = g_buzz[1];
            g_buzz[0].pending = g_buzz[1].pending = false;
        }
        for (int h = 0; h < 2; ++h) {
            if (!bz[h].pending) continue;
            XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
            hi.action = g_haptic, hi.subactionPath = g_hand[h];
            if (bz[h].amp <= 0.f) {
                xrStopHapticFeedback(session, &hi);
                continue;
            }
            XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION};
            v.amplitude = bz[h].amp > 1.f ? 1.f : bz[h].amp;
            v.duration = XrDuration(bz[h].sec * 1e9);
            v.frequency = XR_FREQUENCY_UNSPECIFIED;
            xrApplyHapticFeedback(session, &hi, reinterpret_cast<XrHapticBaseHeader*>(&v));
        }
    }
    if (!g_set) return;
    XrActiveActionSet aas{g_set, XR_NULL_PATH};
    XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets = 1, si.activeActionSets = &aas;
    const XrResult r = xrSyncActions(session, &si);
    Snapshot s;
    if (r == XR_SUCCESS) {
        s.valid = true;
        for (int h = 0; h < 2; ++h) {
            Hand& hd = s.hand[h];
            hd.trigger = GetFloat(session, g_trigger, g_hand[h]);
            hd.grip = GetFloat(session, g_grip, g_hand[h]);
            hd.stick = GetVec2(session, g_stick, g_hand[h]);
            hd.stickClick = GetBool(session, g_stickClick, g_hand[h]);
            hd.lower = GetBool(session, g_lower, g_hand[h]);
            hd.upper = GetBool(session, g_upper, g_hand[h]);
            hd.menu = GetBool(session, g_menu, g_hand[h]);
            if (GetBool(session, g_padClick, g_hand[h])) {
                if (g_padZone[h] < 0) {
                    const float y = GetVec2(session, g_pad, g_hand[h]).y;
                    g_padZone[h] = y > 0.3f ? 2 : (y < -0.3f || !g_padIsStick[h]) ? 1 : 0;
                }
                if (g_padZone[h] == 2) hd.upper = true;
                if (g_padZone[h] == 1) hd.lower = true;
                if (g_padZone[h] == 0) hd.stickClick = true;
            } else {
                g_padZone[h] = -1;
            }
            XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
            if (g_aimSpace[h] && XR_SUCCEEDED(xrLocateSpace(g_aimSpace[h], local, time, &loc)) &&
                (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) &&
                (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
                hd.active = true;
                hd.aimRaw = loc.pose;
                hd.aimRel = Rel(loc.pose, ref);
            }
        }
    } else if (r != XR_SESSION_NOT_FOCUSED && g_syncFailLogs < 5) {
        ++g_syncFailLogs;
        LOG("[xr.input] xrSyncActions -> %d", int(r));
    }
    static bool s_loggedFirst = false;
    if (s.valid && !s_loggedFirst && (s.hand[0].active || s.hand[1].active)) {
        s_loggedFirst = true;
        LOG("[xr.input] controllers tracked (left %d, right %d)", s.hand[0].active, s.hand[1].active);
    }
    std::lock_guard l(g_mu);
    s.seq = ++g_seq;
    g_snap = s;
}

bool Get(Snapshot& out) {
    std::lock_guard l(g_mu);
    out = g_snap;
    return out.valid;
}
void Rumble(int hand, float amplitude, float seconds) {
    std::lock_guard l(g_mu);
    for (int h = 0; h < 2; ++h)
        if (hand == 2 || hand == h) g_buzz[h] = {true, amplitude, seconds};
}
void ProfilesChanged(XrSession session) {
    if (!g_inst) return;
    for (int h = 0; h < 2; ++h) {
        XrInteractionProfileState st{XR_TYPE_INTERACTION_PROFILE_STATE};
        if (XR_FAILED(xrGetCurrentInteractionProfile(session, g_hand[h], &st))) continue;
        char buf[XR_MAX_PATH_LENGTH]{};
        uint32_t n = 0;
        if (st.interactionProfile != XR_NULL_PATH) xrPathToString(g_inst, st.interactionProfile, sizeof buf, &n, buf);
        g_padIsStick[h] = !strcmp(buf, "/interaction_profiles/htc/vive_controller");
        LOG("[xr.input] %s controller type: %s", h ? "right" : "left", buf[0] ? buf : "(none)");
    }
}
}  // namespace ggdpvr::xr::input
