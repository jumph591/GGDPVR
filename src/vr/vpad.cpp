#include "vpad.h"

#include <windows.h>
#include <Xinput.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

#include "../core/config.h"
#include "../core/hooks.h"
#include "../core/log.h"
#include "../xr/xr_input.h"

namespace ggdpvr::vr::vpad {
namespace {
using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using GetCapsFn = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);
GetStateFn o_GetState = nullptr;
GetCapsFn o_GetCaps = nullptr;

std::mutex g_mu;
State g_ctrl, g_pulse;
ULONGLONG g_pulseUntil = 0;
DWORD g_packet = 1;
XINPUT_GAMEPAD g_lastOut{};
std::atomic<int> g_logs{0};

SHORT ToStick(float v) {
    v = std::fmax(-1.f, std::fmin(1.f, v));
    return SHORT(v * (v < 0 ? 32768.f : 32767.f));
}
BYTE ToTrigger(float v) { return BYTE(std::fmax(0.f, std::fmin(1.f, v)) * 255.f + 0.5f); }
SHORT Bigger(SHORT a, SHORT b) { return std::abs(int(b)) > std::abs(int(a)) ? b : a; }

// virtual pad for this poll; false when it is completely idle
bool Virtual(XINPUT_GAMEPAD& g) {
    std::lock_guard l(g_mu);
    State s = g_ctrl;
    if (GetTickCount64() < g_pulseUntil) {
        s.buttons |= g_pulse.buttons;
        s.lt = std::fmax(s.lt, g_pulse.lt), s.rt = std::fmax(s.rt, g_pulse.rt);
        if (std::fabs(g_pulse.lx) > std::fabs(s.lx)) s.lx = g_pulse.lx;
        if (std::fabs(g_pulse.ly) > std::fabs(s.ly)) s.ly = g_pulse.ly;
        if (std::fabs(g_pulse.rx) > std::fabs(s.rx)) s.rx = g_pulse.rx;
        if (std::fabs(g_pulse.ry) > std::fabs(s.ry)) s.ry = g_pulse.ry;
    }
    g = {};
    g.wButtons = s.buttons;
    g.bLeftTrigger = ToTrigger(s.lt), g.bRightTrigger = ToTrigger(s.rt);
    g.sThumbLX = ToStick(s.lx), g.sThumbLY = ToStick(s.ly), g.sThumbRX = ToStick(s.rx), g.sThumbRY = ToStick(s.ry);
    return g.wButtons || g.bLeftTrigger || g.bRightTrigger || g.sThumbLX || g.sThumbLY || g.sThumbRX || g.sThumbRY;
}
std::atomic<bool> g_everActive{false};
std::atomic<unsigned long long> g_lastDpadPress{0};
std::atomic<bool> g_installed{false};

DWORD WINAPI hk_GetState(DWORD user, XINPUT_STATE* st) {
    const DWORD r = o_GetState(user, st);
    if (user != 0 || !st) return r;
    XINPUT_GAMEPAD v;
    const bool active = Virtual(v);
    if (active) g_everActive = true;
    if (r != ERROR_SUCCESS) {
        // no physical pad: present the virtual one once VR controllers have been used
        if (!g_everActive) return r;
        memset(st, 0, sizeof *st);
    }
    XINPUT_GAMEPAD& g = st->Gamepad;
    g.wButtons |= v.wButtons;
    g.bLeftTrigger = std::max(g.bLeftTrigger, v.bLeftTrigger);
    g.bRightTrigger = std::max(g.bRightTrigger, v.bRightTrigger);
    g.sThumbLX = Bigger(g.sThumbLX, v.sThumbLX), g.sThumbLY = Bigger(g.sThumbLY, v.sThumbLY);
    g.sThumbRX = Bigger(g.sThumbRX, v.sThumbRX), g.sThumbRY = Bigger(g.sThumbRY, v.sThumbRY);
    {
        std::lock_guard l(g_mu);
        if ((g.wButtons & ~g_lastOut.wButtons) & 0x000F) g_lastDpadPress = GetTickCount64();  // D-pad press edge
        if (memcmp(&g, &g_lastOut, sizeof g) != 0) {
            g_lastOut = g;
            ++g_packet;
            if (active && g_logs.fetch_add(1) < 30)
                LOG("[vpad] state buttons %04X LT %u RT %u L(%d,%d) R(%d,%d)%s", g.wButtons, g.bLeftTrigger,
                    g.bRightTrigger, g.sThumbLX, g.sThumbLY, g.sThumbRX, g.sThumbRY, r == ERROR_SUCCESS ? " (+physical)" : "");
        }
        st->dwPacketNumber = r == ERROR_SUCCESS ? st->dwPacketNumber + g_packet : g_packet;
    }
    return ERROR_SUCCESS;
}

// The game's rumble (force feedback on pad 0) -> Touch controller vibration (both hands, the stronger motor).
using SetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
SetStateFn o_SetState = nullptr;
WORD g_lastRumble = 0;
DWORD WINAPI hk_SetState(DWORD user, XINPUT_VIBRATION* v) {
    const DWORD r = o_SetState(user, v);
    if (user == 0 && v && g_everActive && config::Get().vrHaptics) {
        const WORD m = std::max(v->wLeftMotorSpeed, v->wRightMotorSpeed);
        if (m != g_lastRumble) {
            g_lastRumble = m;
            xr::input::Rumble(2, m ? 0.25f + 0.75f * float(m) / 65535.f : 0.f, 3.f);
            static int s_logs = 0;
            if (s_logs++ < 10) LOG("[vpad] game rumble %u -> controllers", unsigned(m));
        }
        return ERROR_SUCCESS;  // the virtual pad "accepts" the rumble even without a physical one
    }
    return r;
}

DWORD WINAPI hk_GetCaps(DWORD user, DWORD flags, XINPUT_CAPABILITIES* caps) {
    const DWORD r = o_GetCaps(user, flags, caps);
    if (r == ERROR_SUCCESS || user != 0 || !caps || !g_everActive) return r;
    memset(caps, 0, sizeof *caps);
    caps->Type = XINPUT_DEVTYPE_GAMEPAD;
    caps->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
    caps->Gamepad.wButtons = 0xF3FF;
    caps->Gamepad.bLeftTrigger = caps->Gamepad.bRightTrigger = 255;
    caps->Gamepad.sThumbLX = caps->Gamepad.sThumbLY = caps->Gamepad.sThumbRX = caps->Gamepad.sThumbRY = SHORT(0xFFC0);
    return ERROR_SUCCESS;
}
}  // namespace

bool Install() {
    if (g_installed) return true;
    // xinput1_3 is a static import of GG2Game.exe, so it is already loaded: no LoadLibrary (loader lock) here,
    // this runs during early startup.
    HMODULE xi = GetModuleHandleW(L"xinput1_3.dll");
    if (!xi) {
        LOG("[vpad] xinput1_3.dll not loaded yet");
        return false;
    }
    void* gs = reinterpret_cast<void*>(GetProcAddress(xi, "XInputGetState"));
    void* gc = reinterpret_cast<void*>(GetProcAddress(xi, "XInputGetCapabilities"));
    bool ok = gs && hooks::CreateAt("XInputGetState", gs, (void*)&hk_GetState, (void**)&o_GetState);
    if (gc) hooks::CreateAt("XInputGetCapabilities", gc, (void*)&hk_GetCaps, (void**)&o_GetCaps);
    if (void* ss = reinterpret_cast<void*>(GetProcAddress(xi, "XInputSetState")))
        hooks::CreateAt("XInputSetState", ss, (void*)&hk_SetState, (void**)&o_SetState);
    if (ok) g_installed = true;
    LOG("[vpad] virtual gamepad %s", ok ? "installed (xinput1_3)" : "FAILED");
    return ok;
}

bool Installed() { return g_installed; }
unsigned long long LastDpadPressMs() { return g_lastDpadPress.load(); }
void Configure(bool controllersEnabled) {
    // UE3 polls disconnected pads only every few seconds: report the virtual pad as connected from the start
    // when VR controllers are enabled, so the first press is not lost.
    if (controllersEnabled) g_everActive = true;
    LOG("[vpad] %s", controllersEnabled ? "VR controllers enabled: virtual pad reported as connected" : "VR controllers disabled");
}

void SetFromControllers(const State& s) {
    std::lock_guard l(g_mu);
    g_ctrl = s;
}
void Pulse(const State& s, uint32_t ms) {
    std::lock_guard l(g_mu);
    g_pulse = s;
    g_pulseUntil = GetTickCount64() + ms;
    g_everActive = true;
}
uint16_t ButtonByName(const char* n) {
    struct { const char* name; uint16_t bit; } static const kMap[] = {
        {"A", kA}, {"B", kB}, {"X", kX}, {"Y", kY}, {"LB", kLB}, {"RB", kRB}, {"START", kStart}, {"BACK", kBack},
        {"L3", kL3}, {"R3", kR3}, {"UP", kUp}, {"DOWN", kDown}, {"LEFT", kLeft}, {"RIGHT", kRight}};
    for (const auto& m : kMap)
        if (!_stricmp(n, m.name)) return m.bit;
    return 0;
}
}  // namespace ggdpvr::vr::vpad
