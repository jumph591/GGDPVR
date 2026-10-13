#include "menu_pointer.h"
#include "head_tracking.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>

#include "../core/config.h"
#include "../core/game.h"
#include "../core/hooks.h"
#include "../core/log.h"
#include "../ue3/objects.h"
#include "controllers.h"
#include "render_res.h"
#include "ui_layer.h"

namespace ggdpvr::vr::pointer {
namespace {
std::atomic<bool> g_menus{false}, g_onPanel{false}, g_valid{false}, g_installed{false}, g_stickMenus{false};
// a menu is up: the pointer stays on it wherever the controller points (0.9.110: the results screen only took the
// trigger while pointed at) - off the surface at its nearest edge, pointing away at its last spot
std::atomic<bool> g_sticky{false};
bool g_haveLast = false;
int g_lastX = 0, g_lastY = 0;
float g_lastU = 0.5f, g_lastV = 0.5f;
std::atomic<int> g_x{0}, g_y{0};      // client pixels
std::atomic<int> g_devClicks{0};
// Mouse space: the menus' 1920x1080 stage (the HUD band of the render: u,v * 1920x1080). 0 = client-linear (dev).
std::atomic<int> g_map{1};
std::atomic<float> g_hitDist{0};
float g_u = 0, g_v = 0;
std::atomic<bool> g_touchEyeView{false};  // minigames: the virtual mouse in the eye-view projection space
std::atomic<float> g_touchX{0}, g_touchY{0};  // ...the last such position (stage units)
std::atomic<bool> g_touchValid{false};
// the trigger's edges by pointer frame (Update runs in the draw, after the game's tick has read input): an edge recorded
// in frame N is seen by exactly the next tick
// the trigger's presses / releases, counted (the game samples them once per tick; the draw runs once per eye)
std::atomic<uint32_t> g_presses{0}, g_releases{0};
std::atomic<bool> g_trigDown{false};
uintptr_t g_pc = 0;  // game thread
bool g_prevTrigger = false;
int g_lastPostX = -1, g_lastPostY = -1;

// UWindowsClient's DirectInput mouse (buffered, relative axes): FWindowsViewport::ProcessInput turns its
// GetDeviceData records into MouseX/Y and button events; Scaleform then reads the position through
// GetCursorPos/ScreenToClient. Window messages are ignored for the mouse, so synthetic records are appended.
constexpr uint32_t kDiMouseGlobal = 0x329ABF0;  // IDirectInputDevice8* (set at client init)
struct DiObjData { DWORD ofs, data, timeStamp, sequence; UINT_PTR appData; };  // DIDEVICEOBJECTDATA (DI 8)
constexpr DWORD kOfsButton0 = 12;
using GetDeviceDataFn = HRESULT(WINAPI*)(void*, DWORD, DiObjData*, DWORD*, DWORD);
GetDeviceDataFn o_GetDeviceData = nullptr;
std::mutex g_qMu;
std::deque<std::pair<DWORD, DWORD>> g_queue;  // (offset, data)
bool g_diHooked = false;
bool g_deliverOk = false;  // one record per game frame: the engine drains the buffer in a loop, and a press and
                           // release inside one frame are never seen by the game's per-frame button sampling
void Queue(DWORD ofs, DWORD data);
std::atomic<int> g_clickPath{1};  // 0 DirectInput records, 1 window messages (test: !clickpath)
std::atomic<ULONGLONG> g_lastButton{0};
void Button(bool down) {
    g_lastButton = GetTickCount64();
    if (g_clickPath.load() == 0) return Queue(kOfsButton0, down ? 0x80 : 0);
    if (HWND h = static_cast<HWND>(GameWindow())) {
        const LPARAM at = MAKELPARAM(g_x.load(), g_y.load());
        PostMessageW(h, WM_MOUSEMOVE, down ? 0 : MK_LBUTTON, at);  // the press lands where the pointer is
        PostMessageW(h, down ? WM_LBUTTONDOWN : WM_LBUTTONUP, down ? MK_LBUTTON : 0, at);
    }
}
std::atomic<int> g_rFrames{0};  // dev: right button held for this many more frames
void RightButton(bool down) {
    if (HWND h = static_cast<HWND>(GameWindow())) {
        const LPARAM at = MAKELPARAM(g_x.load(), g_y.load());
        PostMessageW(h, WM_MOUSEMOVE, down ? 0 : MK_RBUTTON, at);
        PostMessageW(h, down ? WM_RBUTTONDOWN : WM_RBUTTONUP, down ? MK_RBUTTON : 0, at);
    }
}
void Queue(DWORD ofs, DWORD data) {
    std::lock_guard l(g_qMu);
    if (g_queue.size() < 64) g_queue.emplace_back(ofs, data);
}
HRESULT WINAPI hk_GetDeviceData(void* dev, DWORD cb, DiObjData* rg, DWORD* inOut, DWORD flags) {
    uintptr_t mouse = 0;
    game::Read(game::Addr(kDiMouseGlobal), mouse);
    const DWORD cap = inOut ? *inOut : 0;  // records the caller has room for
    HRESULT hr = o_GetDeviceData(dev, cb, rg, inOut, flags);
    if (reinterpret_cast<uintptr_t>(dev) != mouse || !rg || !inOut || cb != sizeof(DiObjData) || (flags & 1 /*PEEK*/))
        return hr;
    std::lock_guard l(g_qMu);
    if (g_queue.empty() || !g_deliverOk) return hr;
    // the device is not acquired while the window is unfocused: still deliver the controller's records
    DWORD n = SUCCEEDED(hr) ? *inOut : 0;
    static int s_logs = 0;
    if (s_logs < 8) ++s_logs, LOG("[ptr] DI mouse: game read %lu record(s) (hr 0x%08lX), adding %zu", n, (unsigned long)hr, g_queue.size());
    if (n < cap) {
        rg[n] = {g_queue.front().first, g_queue.front().second, GetTickCount(), 0, 0};
        g_queue.pop_front();
        ++n;
        g_deliverOk = false;
    }
    *inOut = n;
    return 0;  // DI_OK
}
void HookDiMouse() {
    if (g_diHooked) return;
    uintptr_t dev = 0, vt = 0;
    if (!game::Read(game::Addr(kDiMouseGlobal), dev) || !dev || !game::Read(dev, vt) || !vt) return;
    void* fn = nullptr;
    game::Read(vt + 10 * 8, fn);  // IDirectInputDevice8W::GetDeviceData
    g_diHooked = true;
    if (fn && hooks::CreateAt("IDirectInputDevice8::GetDeviceData(mouse)", fn, (void*)&hk_GetDeviceData, (void**)&o_GetDeviceData))
        hooks::EnableAll();
}

using GetCursorPosFn = BOOL(WINAPI*)(LPPOINT);
GetCursorPosFn o_GetCursorPos = nullptr;
BOOL WINAPI hk_GetCursorPos(LPPOINT p) {
    const BOOL r = o_GetCursorPos(p);
    if (p && Active()) {
        POINT c{g_x.load(), g_y.load()};
        if (HWND h = static_cast<HWND>(GameWindow()); h && ClientToScreen(h, &c)) *p = c;
    }
    return r;
}

// controller ray -> client pixel on the virtual screen or the UI panel (recentered frame, both face +Z at z = -dist)
std::atomic<float> g_pinX{0}, g_pinY{0};  // dev: the pointer pinned to a panel point (stage coords) while refreshed
std::atomic<ULONGLONG> g_pinT{0};
bool Hit(int& outX, int& outY, bool clampOff = false) {
    HWND h = static_cast<HWND>(GameWindow());
    ctrl::Aim aim;
    RECT rc{};
    if (g_onPanel.load() && GetTickCount64() - g_pinT.load() < 300 && h && GetClientRect(h, &rc) && rc.right > 0) {
        uint32_t bx, by, bw, bh;
        ui::BandOf(uint32_t(rc.right), uint32_t(rc.bottom), bx, by, bw, bh);
        const float e = ui::PanelExtension();
        g_u = std::fmax(0.f, std::fmin(1.f, g_pinX.load() / 1920.f)), g_v = std::fmax(-e, std::fmin(1.f + e, g_pinY.load() / 1080.f));
        outX = int(float(bx) + g_u * float(bw)), outY = int(float(by) + std::fmax(0.f, std::fmin(1.f, g_v)) * float(bh));
        return true;
    }
    if (!h || !ctrl::GetAim(aim) || !GetClientRect(h, &rc) || rc.right <= 0 || rc.bottom <= 0) return false;
    const auto& q = aim.rel.orientation;
    // forward (-Z) rotated by the aim orientation
    const float dx = -(2.f * (q.x * q.z + q.w * q.y)), dy = -(2.f * (q.y * q.z - q.w * q.x)),
                dz = -(1.f - 2.f * (q.x * q.x + q.y * q.y));
    if (dz > -0.05f) return false;
    const auto& cfg = config::Get();
    const bool panel = g_onPanel.load();
    const float dist = panel ? cfg.vrUiDistance : cfg.vrScreenDistance;
    const float t = (-dist - aim.rel.position.z) / dz;
    const float x = aim.rel.position.x + dx * t, y = aim.rel.position.y + dy * t - (panel ? ui::PanelYOffset() : 0.f);
    const uint32_t cw = uint32_t(rc.right), ch = uint32_t(rc.bottom);
    uint32_t bx = 0, by = 0, bw = cw, bh = ch;
    if (panel) ui::BandOf(cw, ch, bx, by, bw, bh);  // the panel shows the HUD band of the UI image
    const float w = panel ? ui::PanelWidth() : cfg.vrScreenWidth, hgt = w * float(bh) / float(bw);
    const float u = x / w + 0.5f, v = 0.5f - y / hgt;
    const float e = panel ? ui::PanelExtension() : 0.f;  // tall panel (minigames): its rows above / below the band
    if (!clampOff && (u < -0.05f || u > 1.05f || v < -0.05f - e || v > 1.05f + e)) return false;  // off the surface
    g_u = std::fmax(0.f, std::fmin(1.f, u)), g_v = std::fmax(-e, std::fmin(1.f + e, v));
    g_hitDist = t * std::sqrt(dx * dx + dy * dy + dz * dz);
    outX = int(float(bx) + std::fmax(0.f, std::fmin(1.f, u)) * float(bw));
    outY = int(float(by) + std::fmax(0.f, std::fmin(1.f, v)) * float(bh));
    return true;
}
}  // namespace

void Install() {
    if (g_installed.exchange(true)) return;
    HMODULE u = GetModuleHandleW(L"user32.dll");
    void* f = u ? reinterpret_cast<void*>(GetProcAddress(u, "GetCursorPos")) : nullptr;
    if (!f || !hooks::CreateAt("GetCursorPos", f, (void*)&hk_GetCursorPos, (void**)&o_GetCursorPos))
        LOG("[ptr] GetCursorPos hook FAILED");
}
void SetContext(bool menus, bool onPanel, uintptr_t pc, bool stickMenus, bool sticky) {
    g_menus = menus && config::Get().vrControllers, g_onPanel = onPanel, g_pc = pc, g_stickMenus = stickMenus, g_sticky = sticky;
}
bool Active() { return g_menus.load() && g_valid.load(); }
bool MenuScene() { return g_menus.load(); }
bool SticksAsDpad() { return g_stickMenus.load(); }

void Update(bool trigger, bool touch) {
    HookDiMouse();
    {
        std::lock_guard l(g_qMu);
        g_deliverOk = true;
    }
    int x = 0, y = 0;
    const bool sticky = g_menus.load() && g_sticky.load() && !g_touchEyeView.load();  // not in minigame play
    bool ok = g_menus.load() && Hit(x, y, sticky);
    if (ok) {
        g_haveLast = true, g_lastX = x, g_lastY = y, g_lastU = g_u, g_lastV = g_v;
    } else if (sticky) {  // pointing away from the menu: the pointer stays where it last was (the centre at first)
        if (!g_haveLast) {
            RECT rc{};
            GetClientRect(static_cast<HWND>(GameWindow()), &rc);
            g_lastX = int(rc.right / 2), g_lastY = int(rc.bottom / 2), g_lastU = g_lastV = 0.5f, g_haveLast = true;
        }
        x = g_lastX, y = g_lastY, g_u = g_lastU, g_v = g_lastV, ok = true;
    }
    if (!g_menus.load()) g_haveLast = false;  // a new menu starts from where the controller points (or the centre)
    if (ok != g_valid.load()) LOG("[ptr] menu pointer %s", ok ? "on" : "off");
    g_valid = ok;
    if (!ok) {
        if (g_prevTrigger) Button(false);
        if (g_trigDown.load()) ++g_releases;
        g_prevTrigger = false, g_trigDown = false;
        return;
    }
    g_x = x, g_y = y;
    // GG2's menus use their own virtual mouse: GG2PlayerInput.m_mousePosition (+0xC48, IntPoint), integrated from
    // relative mouse deltas in m_DisplayScreenSize (+0xC50, Vector2D) space. Set it to the pointer every frame;
    // clicks are window messages (the engine's mouse buttons come from WM_LBUTTON*, not DirectInput).
    uintptr_t pi = 0;
    if (g_pc && game::Read(g_pc + 0x554, pi) && pi && ue3::IsA(pi, "GG2PlayerInput")) {
        float ds[2]{};
        game::Read(pi + 0xC50, ds);
        RECT rc{};
        GetClientRect(static_cast<HWND>(GameWindow()), &rc);
        const float sx = (ds[0] > 1.f && rc.right > 0) ? ds[0] / float(rc.right) : 1.f;
        const float sy = (ds[1] > 1.f && rc.bottom > 0) ? ds[1] / float(rc.bottom) : 1.f;
        // The menus read m_mousePosition in their 1920x1080 stage coordinates (test, pause menu on the panel: the
        // cursor was drawn at 1.9x = 3640/1920 the position when it was given in display pixels)
        const bool stage = g_map.load() == 1;
        int32_t mp[2] = {stage ? int32_t(g_u * 1920.f) : int32_t(float(x) * sx), stage ? int32_t(g_v * 1080.f) : int32_t(float(y) * sy)};
        // minigames compare touches with their markers' 3D projections (stage units, no band offset): the virtual
        // mouse is the projection of what is seen through the pointer, not the panel point (0.9.21)
        if (g_touchEyeView.load() && g_onPanel.load()) {
            float ex = g_u * 1920.f, ey = g_v * 1080.f;
            g_touchValid = vr::PanelToHudProjection(ex, ey);
            if (g_touchValid) mp[0] = int32_t(ex), mp[1] = int32_t(ey), g_touchX = ex, g_touchY = ey;
        }
        memcpy(reinterpret_cast<void*>(pi + 0xC48), mp, sizeof mp);
        if (std::abs(x - g_lastPostX) + std::abs(y - g_lastPostY) >= 1) {
            g_lastPostX = x, g_lastPostY = y;
            static int s_logs = 0;
            if (s_logs++ < 40) LOG("[ptr] mouse (%d,%d) client %ldx%ld display %.0fx%.0f", mp[0], mp[1], rc.right, rc.bottom, ds[0], ds[1]);
        }
        {  // every 10 s while active: the mapping (0.9.28: the Score Attack aim was off once after a minigame)
            static ULONGLONG s_next = 0;
            if (GetTickCount64() >= s_next) {
                s_next = GetTickCount64() + 10000;
                LOG("[ptr] state: uv (%.3f,%.3f) window (%d,%d) mouse (%d,%d) display %.0fx%.0f map %d panel %d touch %d", g_u, g_v, x, y,
                    mp[0], mp[1], ds[0], ds[1], g_map.load(), g_onPanel.load() ? 1 : 0, g_touchEyeView.load() ? 1 : 0);
            }
        }
    }
    if (g_devClicks.load() > 0) --g_devClicks, trigger = true;  // dev click: held for a few frames
    {  // dev: right button
        static bool s_r = false;
        const bool r = g_rFrames.load() > 0;
        if (r) --g_rFrames;
        if (r != s_r) s_r = r, RightButton(r), LOG("[ptr] right button %s at (%d,%d)", r ? "down" : "up", x, y);
    }
    {
        // minigames: the touch (requests, gestures) is the grip; the trigger stays the shot (0.9.22: shots by
        // accident while doing a request)
        if (touch && !g_trigDown.load()) ++g_presses;
        if (!touch && g_trigDown.load()) ++g_releases;
        g_trigDown = touch;
    }
    if (trigger && !g_prevTrigger) {
        Button(true);
        static int s_logs = 0;
        if (s_logs++ < 20) LOG("[ptr] click at (%d,%d) on the %s", x, y, g_onPanel.load() ? "panel" : "virtual screen");
    } else if (!trigger && g_prevTrigger) {
        Button(false);
    }
    g_prevTrigger = trigger;
}
void DevClick() { g_devClicks = 8; }
void DevPin(float x, float y) { g_pinX = x, g_pinY = y, g_pinT = GetTickCount64(); }
void DevRightClick(int frames) { g_rFrames = frames; }
namespace {
void Wheel(int notches) {  // a DirectInput wheel record (DIMOFS_Z; the engine ignores WM_MOUSEWHEEL), + = up
    HookDiMouse();
    if (notches) Queue(8, DWORD(notches * WHEEL_DELTA));
}
}  // namespace
void DevWheel(int notches) {
    Wheel(notches);
    LOG("[ptr] dev: wheel %+d at (%d,%d)", notches, g_x.load(), g_y.load());
}
// Scroll lists in the menus (Score Attack stage list, save slots...) took dragging their bar with the pointer: the
// aim-hand stick now scrolls them like a mouse wheel - one notch on tilting, repeating faster the further it is
// tilted (0.9.41)
void Scroll(float sy, bool dpadMode) {
    static ULONGLONG s_next = 0;
    static int s_dir = 0;
    const float a = std::fabs(sy);
    const int dir = a < 0.35f ? 0 : sy > 0 ? 1 : -1;
    const ULONGLONG now = GetTickCount64();
    if (dir && (dpadMode || !Active())) {  // why a tilt didn't scroll (0.9.110: the right stick scrolled nothing)
        static ULONGLONG s_why = 0;
        static int s_n = 0;
        if (now >= s_why && s_n < 15)
            ++s_n, s_why = now + 2000, LOG("[ptr] stick scroll ignored: %s (menus %d, on menu %d)", dpadMode ? "stick is the D-pad here" : "pointer not active",
                                            g_menus.load() ? 1 : 0, g_valid.load() ? 1 : 0);
    }
    if (!dir || dpadMode || !Active()) {
        s_dir = 0;
        return;
    }
    if (dir != s_dir || now >= s_next) {
        const bool first = dir != s_dir;
        s_dir = dir;
        Wheel(dir);
        // first notch at once, then after 400 ms; repeats every 350 ms (light tilt) .. 130 ms (full tilt) - a notch
        // moves a Score Attack list ~3 rows
        const float t = std::fmin(1.f, (a - 0.35f) / 0.55f);
        s_next = now + (first ? 400 : ULONGLONG(350.f - 220.f * t));
        static int s_logs = 0;
        if (s_logs++ < 6) LOG("[ptr] stick scroll %+d at (%d,%d)", dir, g_x.load(), g_y.load());
    }
}
void SetTouchInEyeView(bool on) { g_touchEyeView = on; }
bool TouchModeActive() { return g_touchEyeView.load() && Active(); }
void TouchCounts(uint32_t& presses, uint32_t& releases, bool& down) {
    presses = g_presses.load(), releases = g_releases.load(), down = g_trigDown.load();
}
bool TouchStage(float& x, float& y) {
    if (!g_touchValid.load() || !g_touchEyeView.load() || !Active()) return false;
    x = g_touchX.load(), y = g_touchY.load();
    return true;
}
bool PanelStage(float& x, float& y) {
    if (!Active() || !g_onPanel.load()) return false;
    x = g_u * 1920.f, y = g_v * 1080.f;
    return true;
}
ULONGLONG LastButtonTick() { return g_lastButton.load(); }
void SetMapping(int m) { g_map = m; LOG("[ptr] mapping -> %s", m ? "HUD band" : "client"); }
float HitDistance() { return Active() ? g_hitDist.load() : 0.f; }
void SetClickPath(int p) { g_clickPath = p; LOG("[ptr] click path -> %s", p ? "window messages" : "DirectInput"); }
}  // namespace ggdpvr::vr::pointer
