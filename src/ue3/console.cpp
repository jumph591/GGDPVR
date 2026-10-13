#include "console.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <fstream>
#include <mutex>
#include <vector>

#include "../core/config.h"
#include "../core/game.h"
#include "../core/log.h"
#include "objects.h"
#include "../diag/renderdoc_capture.h"
#include "../xr/xr_screen.h"
#include "../core/settings_guard.h"
#include "../vr/head_tracking.h"
#include "../vr/render_res.h"
#include "../vr/ui_layer.h"
#include "../vr/vpad.h"
#include "../vr/controllers.h"
#include "../vr/gfx_vars.h"
#include "../vr/menu_pointer.h"
#include "../vr/headset_audio.h"

namespace ggdpvr::ue3::console {
namespace {
using PEFn = void(__fastcall*)(uintptr_t self, uintptr_t fn, void* parms, void* result);

uint32_t g_gameThread = 0;
uintptr_t g_fnConsoleCommand = 0;
uint32_t g_offCommand = 0, g_offWriteToLog = 0, g_bitWriteToLog = 1, g_offReturn = 0, g_parmsSize = 0;
uint32_t g_offGamePlayers = 0, g_offPlayerActor = 0;
std::mutex g_mu;
std::deque<std::string> g_queue;
thread_local bool t_inPump = false;
struct Hotkey { int vk; std::string cmd; bool down; };
std::vector<Hotkey> g_hotkeys;
struct FlagReq { bool world; bool on; };
std::vector<FlagReq> g_flagReq;  // debug UI-layer experiments, applied on the game thread
std::vector<std::string> g_stateReq;  // debug: put the nearest girl into a state (game thread)

uintptr_t LocalPlayerController();
// Debug: Object.GotoState(NewState) on the live GG2ZakoGirl nearest the local player.
void GirlGotoState(const std::string& state) {
    int32_t nameIdx = -1;
    for (int32_t i = 0, n = NumNames(); i < n; ++i)
        if (Name(i) == state) { nameIdx = i; break; }
    static const uintptr_t fn = FindObject("Core.Object.GotoState", "Function");
    const uintptr_t pc = LocalPlayerController();
    if (nameIdx < 0 || !fn || !pc) {
        LOG("[console] zakostate: name %d fn %p pc %p -> cannot run", nameIdx, (void*)fn, (void*)pc);
        return;
    }
    float pl[3]{};
    game::Read(pc + 0x80, pl);
    uintptr_t best = 0;
    float bestD = 1e30f;
    for (int32_t i = 0, cnt = NumObjects(); i < cnt; ++i) {
        const uintptr_t o = ObjectAt(i);
        if (!o || !IsA(o, "GG2ZakoGirl") || PathName(o).find("Default__") != std::string::npos) continue;
        float l[3]{};
        game::Read(o + 0x80, l);
        const float d = (l[0] - pl[0]) * (l[0] - pl[0]) + (l[1] - pl[1]) * (l[1] - pl[1]) + (l[2] - pl[2]) * (l[2] - pl[2]);
        if (d < bestD) bestD = d, best = o;
    }
    if (!best) {
        LOG("[console] zakostate: no live GG2ZakoGirl");
        return;
    }
    uint8_t parms[0x40]{};
    memcpy(parms, &nameIdx, 4);  // NewState (FName index, number 0); Label = None
    uintptr_t vt = 0, pe = 0;
    game::Read(best, vt);
    game::Read(vt + game::rva::ProcessEvent_VSlot, pe);
    reinterpret_cast<PEFn>(pe)(best, fn, parms, nullptr);
    LOG("[console] zakostate: %s -> GotoState(%s) (distance %.0f uu)", ObjName(best).c_str(), state.c_str(), std::sqrt(bestD));
}
std::atomic<bool> g_cursorPin{false};
std::atomic<float> g_cursorX{0}, g_cursorY{0};


struct FString { wchar_t* data; int32_t num, max; };

uintptr_t LocalPlayerController() {
    uintptr_t engine = 0;
    game::Read(game::Addr(game::rva::GEngine), engine);
    if (!engine) return 0;
    struct { uintptr_t data; int32_t num, max; } players{};
    if (!game::Read(engine + g_offGamePlayers, players) || players.num < 1) return 0;
    uintptr_t lp = 0, pc = 0;
    game::Read(players.data, lp);
    if (lp) game::Read(lp + g_offPlayerActor, pc);
    return IsObject(pc) ? pc : 0;
}

void Execute(const std::string& cmd) {
    // 0.9.110: the engine's SET / SETNOPEC also SAVE the class's config into the player's GG2Engine.ini - a test `set`
    // on the audio classes once zeroed [Engine.AudioDevice] there and New Game crashed even without the mod
    {
        std::string low = cmd.substr(0, 9);
        for (auto& c : low) c = char(tolower(static_cast<unsigned char>(c)));
        if (low.rfind("set ", 0) == 0 || low.rfind("setnopec ", 0) == 0) {
            LOG("[console] refused (writes the game's config files): %s", cmd.c_str());
            return;
        }
    }
    uintptr_t pc = LocalPlayerController();
    if (!pc) {
        LOG("[console] no local PlayerController; dropped: %s", cmd.c_str());
        return;
    }
    std::wstring w(cmd.begin(), cmd.end());
    std::vector<uint8_t> parms(g_parmsSize + 64, 0);
    FString in{w.data(), int32_t(w.size() + 1), int32_t(w.size() + 1)};
    memcpy(parms.data() + g_offCommand, &in, sizeof in);
    uint32_t bits = g_bitWriteToLog;
    memcpy(parms.data() + g_offWriteToLog, &bits, 4);
    uintptr_t vt = 0, pe = 0;
    game::Read(pc, vt);
    game::Read(vt + game::rva::ProcessEvent_VSlot, pe);
    reinterpret_cast<PEFn>(pe)(pc, g_fnConsoleCommand, parms.data(), nullptr);
    FString out{};
    memcpy(&out, parms.data() + g_offReturn, sizeof out);
    std::string result;
    for (int i = 0; out.data && i < out.num - 1 && i < 2000; ++i) result.push_back(char(out.data[i]));
    LOG("[console] %s -> %s", cmd.c_str(), result.empty() ? "(no output)" : result.c_str());
    // The engine allocated the return string with its own allocator; leaking a few bytes per command is accepted.
}

DWORD WINAPI Watcher(LPVOID) {
    const std::wstring file = log::Directory() + L"\\exec\\live.txt";
    CreateDirectoryW((log::Directory() + L"\\exec").c_str(), nullptr);
    // commands left over from a previous run (e.g. a test 'exit' never consumed) must not run in this one
    if (DeleteFileW(file.c_str())) LOG("[console] discarded stale live.txt from a previous run");
    for (;;) {
        Sleep(200);
        if (GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES) {
            std::ifstream f(file);
            std::string line;
            std::vector<std::string> cmds;
            while (std::getline(f, line)) {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                if (!line.empty() && line[0] != '#') cmds.push_back(line);
            }
            f.close();
            DeleteFileW(file.c_str());
            for (auto& c : cmds) Enqueue(c);
        }
        HWND fg = GetForegroundWindow();
        DWORD pid = 0;
        GetWindowThreadProcessId(fg, &pid);
        const bool focused = pid == GetCurrentProcessId();
        for (auto& h : g_hotkeys) {
            const SHORT ks = GetAsyncKeyState(h.vk);  // bit 15: down now; bit 0: pressed since last poll
            const bool down = focused && (ks & 0x8001);
            if (down && !h.down) Enqueue(h.cmd);
            h.down = down;
        }
    }
}
}  // namespace

void Enqueue(const std::string& cmd) {
    if (!cmd.empty() && cmd[0] == '!') {  // mod-internal commands
        if (cmd.rfind("!rdcapture", 0) == 0) {
            unsigned n = 1;
            sscanf_s(cmd.c_str(), "!rdcapture %u", &n);
            diag::renderdoc::TriggerCapture(n);
        } else if (cmd.rfind("!fakehead", 0) == 0) {
            int n = 0;
            sscanf_s(cmd.c_str(), "!fakehead %d", &n);
            vr::SetFakeHead(n);
        } else if (cmd.rfind("!tagtest", 0) == 0) {  // dev: !tagtest <x> <y> | !tagtest off - name tag slot 0 pinned (eye view, stage units)
            float x = 960, y = 400;
            const bool on = sscanf_s(cmd.c_str() + 8, "%f %f", &x, &y) == 2;
            vr::ui::SetTagTest(on, x, y);
        } else if (cmd.rfind("!stereo", 0) == 0) {
            int v = 0;
            sscanf_s(cmd.c_str(), "!stereo %d", &v);
            vr::SetStereoVariant(v);
        } else if (cmd == "!collision") {  // diag: static mesh components' collision flags
            const uintptr_t cPrim = FindObject("Engine.PrimitiveComponent", "Class"), cSm = FindObject("Engine.StaticMeshComponent", "Class");
            PropInfo pc, pz, pb, pm;
            if (cPrim && cSm && FindProperty(cPrim, "CollideActors", pc) && FindProperty(cPrim, "BlockZeroExtent", pz) &&
                FindProperty(cPrim, "BlockActors", pb) && FindProperty(cSm, "StaticMesh", pm)) {
                int total = 0, coll = 0, zero = 0, shown = 0;
                for (int32_t i = 0, cnt = NumObjects(); i < cnt; ++i) {
                    const uintptr_t o = ObjectAt(i);
                    if (!o || !IsA(o, "StaticMeshComponent") || PathName(o).find("Default__") != std::string::npos) continue;
                    uint32_t wc = 0, wz = 0, wb = 0;
                    uintptr_t mesh = 0;
                    game::Read(o + pc.offset, wc), game::Read(o + pz.offset, wz), game::Read(o + pb.offset, wb), game::Read(o + pm.offset, mesh);
                    const bool c = wc & pc.bitMask, z = wz & pz.bitMask, bl = wb & pb.bitMask;
                    ++total, coll += c, zero += c && z;
                    const std::string pn = PathName(o);
                    if (shown < 25 && mesh && pn.find("GG2CollisionMesh") == std::string::npos && pn.find("Sns.") == std::string::npos &&
                        ObjName(mesh) != "Cylinder") {
                        ++shown;
                        LOG("[diag] collision %s (%s): CollideActors %d BlockZeroExtent %d BlockActors %d", PathName(o).c_str(),
                            ObjName(mesh).c_str(), c, z, bl);
                    }
                }
                LOG("[diag] collision: %d static mesh components, %d CollideActors, %d also BlockZeroExtent (line traces hit those)", total, coll, zero);
            }
        } else if (cmd == "!aimdiag" || cmd == "!aimdiag 0") {
            vr::SetAimDiag(cmd == "!aimdiag");
        } else if (cmd == "!tracetest") {
            vr::TraceTest();
        } else if (cmd.rfind("!shopvoid", 0) == 0) {  // !shopvoid [0|1]: black / white void in the Academy Store (no arg = toggle)
            const char* a = cmd.c_str() + 9;
            while (*a == ' ') ++a;
            config::SetShopVoidWhite(*a ? (atoi(a) ? 1 : 0) : !config::Get().vrShopVoidWhite);
            LOG("[console] shop void %s", config::Get().vrShopVoidWhite ? "white" : "black");
        } else if (cmd.rfind("!worldscale", 0) == 0) {  // !worldscale <uu per metre> | +n | -n
            const char* a = cmd.c_str() + 11;
            while (*a == ' ') ++a;
            const float cur = config::Get().vrWorldScale, v = float(atof(a));
            float next = (*a == '+' || *a == '-') ? cur + v : v;
            next = std::fmin(200.f, std::fmax(20.f, next));
            if (*a) config::SetWorldScale(next);
            LOG("[console] world scale %.1f -> %.1f uu/m", cur, config::Get().vrWorldScale);
        } else if (cmd.rfind("!ipdscale", 0) == 0) {
            float k = 1.f;
            sscanf_s(cmd.c_str(), "!ipdscale %f", &k);
            vr::SetIpdScale(k);
        } else if (cmd.rfind("!vrres", 0) == 0) {
            unsigned w = 0, h = 0;
            sscanf_s(cmd.c_str(), "!vrres %ux%u", &w, &h);
            vr::ForceResolutionTest(w, h);
        } else if (cmd.rfind("!cursor", 0) == 0) {
            float x = 0, y = 0;
            if (sscanf_s(cmd.c_str(), "!cursor %f %f", &x, &y) == 2) {
                g_cursorX = x, g_cursorY = y, g_cursorPin = true;
                vr::ResetDeprojLog();
                LOG("[console] sight cursor pinned at %.1f,%.1f", x, y);
            } else {
                g_cursorPin = false;
                LOG("[console] sight cursor released");
            }
        } else if (cmd.rfind("!noworld", 0) == 0 || cmd.rfind("!nohud", 0) == 0) {
            int on = 1;
            const bool world = cmd.rfind("!noworld", 0) == 0;
            sscanf_s(cmd.c_str() + (world ? 8 : 6), "%d", &on);
            std::lock_guard l(g_mu);
            g_flagReq.push_back({world, on != 0});
        } else if (cmd.rfind("!uilayer", 0) == 0) {
            int on = 1;
            sscanf_s(cmd.c_str(), "!uilayer %d", &on);
            vr::ui::SetEnabled(on != 0);
        } else if (cmd.rfind("!opt ", 0) == 0 || cmd.rfind("!steamcfg ", 0) == 0) {  // dev: the game's option / launcher data
            std::lock_guard l(g_mu);
            g_stateReq.push_back("__" + cmd.substr(1));
        } else if (cmd == "!shotstats") {  // dev: score manager counters + the shot manager's ray/target
            std::lock_guard l(g_mu);
            g_stateReq.push_back("__shotstats");
        } else if (cmd.rfind("!stencilscale ", 0) == 0) {
            std::lock_guard l(g_mu);
            g_stateReq.push_back("__stencilscale " + cmd.substr(14));
        } else if (cmd.rfind("!clickpath ", 0) == 0) {
            vr::pointer::SetClickPath(atoi(cmd.c_str() + 11));
        } else if (cmd.rfind("!ptrmap ", 0) == 0) {
            vr::pointer::SetMapping(atoi(cmd.c_str() + 8));
        } else if (cmd.rfind("!rclick", 0) == 0) {  // dev: !rclick [frames] - right button at the pointer
            vr::pointer::DevRightClick(cmd.size() > 8 ? atoi(cmd.c_str() + 8) : 60);
        } else if (cmd == "!click") {  // dev: one menu-pointer click at the simulated controller's aim
            vr::pointer::DevClick();
        } else if (cmd.rfind("!cursorspace ", 0) == 0) {
            vr::SetCursorSpace(atoi(cmd.c_str() + 13));
        } else if (cmd.rfind("!fakehand ", 0) == 0) {  // dev: !fakehand x y z (metres, XR axes, recentered frame)
            float x = 0, y = 0, z = 0;
            sscanf_s(cmd.c_str() + 10, "%f %f %f", &x, &y, &z);
            vr::ctrl::SetFakeHand(x, y, z);
        } else if (cmd.rfind("!engfps", 0) == 0) {  // dev: !engfps [smooth 0|1] [max fps] - the engine's frame limiter
            int sm = -1;
            float mx = 0;
            sscanf_s(cmd.c_str(), "!engfps %d %f", &sm, &mx);
            vr::DevEngineFps(sm, mx);
        } else if (cmd.rfind("!wheel", 0) == 0) {  // dev: !wheel <notches> - mouse wheel at the menu pointer (+ up)
            vr::pointer::DevWheel(cmd.size() > 7 ? atoi(cmd.c_str() + 7) : 1);
        } else if (cmd.rfind("!keydiag", 0) == 0) {  // dev: !keydiag <seconds>
            vr::DevKeyDiag(cmd.size() > 9 ? atoi(cmd.c_str() + 9) : 30);
        } else if (cmd == "!dkdkstart") {
            vr::DevDkdkStart();
        } else if (cmd.rfind("!heart", 0) == 0) {  // dev: !heart <value> - the heart (Doki Doki) gauge
            vr::DevSetHeartGauge(cmd.size() > 7 ? atoi(cmd.c_str() + 7) : 100);
        } else if (cmd.rfind("!pcset ", 0) == 0) {  // dev: !pcset <property> <int> - an int/byte property of the player controller
            char name[96]{};
            int v = 0;
            if (sscanf_s(cmd.c_str() + 7, "%95s %d", name, unsigned(sizeof name), &v) == 2) vr::DevSetPcProperty(name, v);
        } else if (cmd.rfind("!fakegrip", 0) == 0) {  // dev: !fakegrip <ms> - the dominant grip (minigame touch)
            unsigned ms = 150;
            sscanf_s(cmd.c_str(), "!fakegrip %u", &ms);
            vr::ctrl::FakeGrip(ms);
        } else if (cmd.rfind("!faketrigger", 0) == 0) {
            unsigned ms = 150;
            sscanf_s(cmd.c_str(), "!faketrigger %u", &ms);
            vr::ctrl::FakeTrigger(ms);
        } else if (cmd.rfind("!aepin", 0) == 0) {  // dev: !aepin <mode 0 still|1 up-down|2 circle|3 left-right> [amp] [hz]
            int mode = 0;
            float amp = 0.f, hz = 1.f;
            sscanf_s(cmd.c_str() + 6, "%d %f %f", &mode, &amp, &hz);
            vr::SetAePin(mode, amp, hz);
        } else if (cmd.rfind("!aediag", 0) == 0) {  // dev: !aediag <seconds> - minigame input spaces and markers
            vr::LogAeDiag(cmd.size() > 8 ? atoi(cmd.c_str() + 8) : 30);
        } else if (cmd.rfind("!bossmarkers", 0) == 0) {  // dev: !bossmarkers <seconds> - boss HUD marker table vs cursor
            vr::LogBossMarkers(cmd.size() > 13 ? atoi(cmd.c_str() + 13) : 30);
        } else if (cmd.rfind("!fakectrl at ", 0) == 0) {  // dev: !fakectrl at <class> - the nearest live actor of it
            vr::SetFakeAimAtClass(cmd.c_str() + 13);
        } else if (cmd.rfind("!fakectrl girl", 0) == 0) {  // optional yaw offset in degrees
            vr::SetFakeAimAtGirl(true, cmd.size() > 15 ? float(atof(cmd.c_str() + 15)) : 0.f);
            vr::ResetDeprojLog();
        } else if (cmd.rfind("!fakectrl", 0) == 0) {  // dev: !fakectrl <yawDeg> <pitchDeg> | !fakectrl off
            float yaw = 0, pitch = 0;
            const bool on = sscanf_s(cmd.c_str() + 9, "%f %f", &yaw, &pitch) == 2;
            vr::SetFakeAimAtGirl(false);
            vr::ctrl::SetFake(on, yaw, pitch);
            vr::ResetDeprojLog();
        } else if (cmd.rfind("!pad ", 0) == 0) {  // dev: !pad <BUTTON|LT|RT|LX|LY|RX|RY> [value] <ms>
            vr::ui::TraceAs(1500);
            char what[16]{};
            float a = 0, b = 0;
            const int n = sscanf_s(cmd.c_str() + 5, "%15s %f %f", what, unsigned(sizeof what), &a, &b);
            vr::vpad::State st;
            uint32_t ms = 0;
            uint16_t bits = 0;  // buttons, combinable with '+' (e.g. RB+X)
            {
                std::string w = what;
                for (size_t p0 = 0; p0 <= w.size();) {
                    const size_t p1 = w.find('+', p0);
                    const std::string part = w.substr(p0, p1 == std::string::npos ? std::string::npos : p1 - p0);
                    const uint16_t bit = vr::vpad::ButtonByName(part.c_str());
                    if (!bit) { bits = 0; break; }
                    bits |= bit;
                    if (p1 == std::string::npos) break;
                    p0 = p1 + 1;
                }
            }
            if (bits) {
                st.buttons = bits, ms = n >= 2 ? uint32_t(a) : 200;
            } else {
                const std::string w = what;
                float* f = w == "LT" ? &st.lt : w == "RT" ? &st.rt : w == "LX" ? &st.lx : w == "LY" ? &st.ly
                         : w == "RX" ? &st.rx : w == "RY" ? &st.ry : nullptr;
                if (f && n >= 3) *f = a, ms = uint32_t(b);
            }
            if (ms) {
                vr::vpad::Pulse(st, ms);
                LOG("[console] pad pulse %s for %u ms", what, ms);
            } else {
                LOG("[console] usage: !pad <A|B|X|Y|LB|RB|START|BACK|L3|R3|UP|DOWN|LEFT|RIGHT> [ms] | !pad <LT|RT|LX|LY|RX|RY> <value> <ms>");
            }
        } else if (cmd.rfind("!zakostate ", 0) == 0) {
            std::lock_guard l(g_mu);
            g_stateReq.push_back(cmd.substr(11));
        } else if (cmd == "!shells") {  // debug: live shout projectiles (GG2MojiShell*)
            vr::ResetProjectLog();
            int n = 0;
            for (int32_t i = 0, cnt = NumObjects(); i < cnt && n < 30; ++i) {
                const uintptr_t o = ObjectAt(i);
                if (!o) continue;
                const std::string cls = ObjName(ClassOf(o));
                if (cls.rfind("GG2MojiShell", 0) != 0 || cls == "GG2MojiShellManager" || cls == "GG2MojiShellMesh") continue;
                if (PathName(o).find("Default__") != std::string::npos) continue;
                uint32_t bits = 0;
                float loc[3]{};
                game::Read(o + 0x2A8, bits);
                game::Read(o + 0x80, loc);  // Actor.Location
                ++n;
                LOG("[diag] %s %s life=%u playing=%u inCamera=%u loc=(%.0f,%.0f,%.0f)", cls.c_str(), ObjName(o).c_str(),
                    (bits >> 1) & 1, (bits >> 2) & 1, (bits >> 8) & 1, loc[0], loc[1], loc[2]);
            }
            LOG("[diag] %d shout projectiles", n);
        } else if (cmd == "!stencilinfo") {  // diag: GFx movies that render to a texture (the sight mask)
            for (int32_t i = 0, cnt = NumObjects(); i < cnt; ++i) {
                const uintptr_t o = ObjectAt(i);
                if (!o || !IsA(o, "GFxMoviePlayer") || PathName(o).find("Default__") != std::string::npos) continue;
                uint8_t mode = 0;
                uint32_t bits = 0;
                uintptr_t rt = 0;
                game::Read(o + 0x116, mode), game::Read(o + 0x118, bits), game::Read(o + 0x11C, rt);
                if (!rt && ObjName(ClassOf(o)).find("Stencil") == std::string::npos) continue;
                int32_t sx = 0, sy = 0;
                if (rt && IsObject(rt)) game::Read(rt + 0x134, sx), game::Read(rt + 0x138, sy);
                LOG("[diag] movie %s open=%u renderTextureMode=%u forceFullViewport=%u target=%s %dx%d", ObjName(o).c_str(),
                    bits & 1, mode, (bits >> 12) & 1, rt && IsObject(rt) ? PathName(rt).c_str() : "-", sx, sy);
            }
        } else if (cmd == "!captures") {  // debug: live scene capture components and render targets
            int n = 0;
            for (int32_t i = 0, cnt = NumObjects(); i < cnt && n < 60; ++i) {
                const uintptr_t o = ObjectAt(i);
                if (!o) continue;
                const std::string cls = ObjName(ClassOf(o));
                if (cls.find("SceneCapture") == std::string::npos) continue;
                if (PathName(o).find("Default__") != std::string::npos) continue;
                ++n;
                uint32_t bits = 0;
                float fov = 0;
                uintptr_t rt = 0;
                int32_t sx = 0, sy = 0;
                game::Read(o + 0x88, bits);
                if (cls == "SceneCapture2DComponent") {
                    game::Read(o + 0xD8, fov);
                    game::Read(o + 0xD0, rt);
                    if (rt && IsObject(rt)) game::Read(rt + 0x134, sx), game::Read(rt + 0x138, sy);
                }
                LOG("[diag] %s %s enabled=%u fov=%.1f target=%s %dx%d", cls.c_str(), PathName(o).c_str(), bits & 1, fov,
                    rt && IsObject(rt) ? PathName(rt).c_str() : "-", sx, sy);
            }
            LOG("[diag] %d scene capture objects", n);
        } else if (cmd == "!camera") {
            vr::LogCamera();
        } else if (cmd.rfind("!menulayers ", 0) == 0) {
            vr::ui::SetMenuLayers(atoi(cmd.c_str() + 12) != 0);
        } else if (cmd == "!roomdebug") {
            vr::ui::ToggleRoomDebug();
        } else if (cmd == "!dumplayers") {
            vr::ui::DumpLayers();
        } else if (cmd.rfind("!layerlag ", 0) == 0) {
            vr::ui::SetLayerLag(atoi(cmd.c_str() + 10));
        } else if (cmd.rfind("!gfx", 0) == 0) {  // dev: !gfxvar <class> <path> | !gfxset <class> <path> 0|1 | !gfxprobe <class>
            std::lock_guard l(g_mu);
            g_stateReq.push_back("__" + cmd.substr(1));
        } else if (cmd.rfind("!dumpswf", 0) == 0) {  // diag: !dumpswf [path substring] - Scaleform movie data (SwfMovie.RawData)
            std::string want = cmd.size() > 9 ? cmd.substr(9) : "";
            std::transform(want.begin(), want.end(), want.begin(), ::tolower);
            PropInfo pr;
            const uintptr_t cRaw = FindObject("GFxUI.GFxRawData", "Class");
            if (cRaw && FindProperty(cRaw, "RawData", pr)) {
                const std::wstring dir = log::Directory() + L"\\swf";
                CreateDirectoryW(dir.c_str(), nullptr);
                int n = 0;
                for (int32_t i = 0, cnt = NumObjects(); i < cnt; ++i) {
                    const uintptr_t o = ObjectAt(i);
                    if (!o || !IsA(o, "SwfMovie")) continue;
                    std::string path = PathName(o), low = path;
                    std::transform(low.begin(), low.end(), low.begin(), ::tolower);
                    if (low.find("default__") != std::string::npos || (!want.empty() && low.find(want) == std::string::npos)) continue;
                    uintptr_t data = 0;
                    int32_t num = 0;
                    game::Read(o + pr.offset, data), game::Read(o + pr.offset + 8, num);
                    LOG("[diag] swf %s: %d bytes", path.c_str(), num);
                    if (want.empty() || !data || num <= 0 || num > (64 << 20)) continue;
                    std::vector<char> buf(static_cast<size_t>(num));
                    if (!game::SafeRead(reinterpret_cast<const void*>(data), buf.data(), buf.size())) continue;
                    std::wstring fn = dir + L"\\" + std::wstring(path.begin(), path.end()) + L".gfx";
                    std::ofstream(fn, std::ios::binary).write(buf.data(), std::streamsize(buf.size()));
                    ++n;
                }
                LOG("[diag] !dumpswf: %d written", n);
            }
        } else if (cmd.rfind("!meshes ", 0) == 0) {  // diag: loaded StaticMesh objects whose path contains the text
            const std::string want = cmd.substr(8);
            int n = 0;
            for (int32_t i = 0, cnt = NumObjects(); i < cnt && n < 200; ++i) {
                const uintptr_t o = ObjectAt(i);
                if (!o || ObjName(ClassOf(o)) != "StaticMesh") continue;
                const std::string path = PathName(o);
                if (path.find(want) == std::string::npos) continue;
                ++n;
                LOG("[diag] mesh %s", path.c_str());
            }
            LOG("[diag] !meshes %s: %d", want.c_str(), n);
        } else if (cmd == "!streaming") {  // diag: streaming levels (package, flags, loaded level)
            PropInfo pn, pl;
            const uintptr_t cLs = FindObject("Engine.LevelStreaming", "Class");
            if (cLs && FindProperty(cLs, "PackageName", pn) && FindProperty(cLs, "LoadedLevel", pl)) {
                PropInfo pb;
                const bool hasB = FindProperty(cLs, "bShouldBeLoaded", pb);
                for (int32_t i = 0, cnt = NumObjects(); i < cnt; ++i) {
                    const uintptr_t o = ObjectAt(i);
                    if (!o || !IsA(o, "LevelStreaming") || PathName(o).find("Default__") != std::string::npos) continue;
                    int32_t nm[2]{};
                    uintptr_t lvl = 0;
                    uint32_t bits = 0;
                    game::Read(o + pn.offset, nm), game::Read(o + pl.offset, lvl);
                    if (hasB) game::Read(o + pb.offset, bits);
                    LOG("[diag] streaming %s (%s): package %s loaded=%s flags 0x%X", PathName(o).c_str(), ObjName(ClassOf(o)).c_str(),
                        Name(nm[0], nm[1]).c_str(), lvl && IsObject(lvl) ? "yes" : "no", bits);
                }
            }
        } else if (cmd == "!levelmeshes") {  // diag: static meshes in the loaded level (mesh, materials, bounds, hidden)
            PropInfo pb, pm, pmat, pown, phid;
            const uintptr_t cPrim = FindObject("Engine.PrimitiveComponent", "Class"), cSm = FindObject("Engine.StaticMeshComponent", "Class"),
                            cMesh = FindObject("Engine.MeshComponent", "Class"), cAc = FindObject("Engine.ActorComponent", "Class");
            if (cPrim && cSm && cMesh && cAc && FindProperty(cPrim, "Bounds", pb) && FindProperty(cSm, "StaticMesh", pm) &&
                FindProperty(cMesh, "Materials", pmat) && FindProperty(cAc, "Owner", pown) && FindProperty(cPrim, "HiddenGame", phid)) {
                int n = 0;
                for (int32_t i = 0, cnt = NumObjects(); i < cnt && n < 120; ++i) {
                    const uintptr_t o = ObjectAt(i);
                    if (!o || !IsA(o, "StaticMeshComponent")) continue;
                    const std::string path = PathName(o);
                    if (path.find("Default__") != std::string::npos || path.find("TheWorld") == std::string::npos) continue;
                    uintptr_t mesh = 0, owner = 0, mats = 0;
                    int32_t nm = 0;
                    uint32_t hid = 0;
                    float b[7]{};
                    game::Read(o + pm.offset, mesh), game::Read(o + pown.offset, owner), game::Read(o + pb.offset, b);
                    game::Read(o + pmat.offset, mats), game::Read(o + pmat.offset + 8, nm), game::Read(o + phid.offset, hid);
                    std::string ms;
                    for (int32_t k = 0; k < nm && k < 4 && mats; ++k) {
                        uintptr_t m = 0;
                        game::Read(mats + 8 * k, m);
                        ms += (k ? "," : "") + (m && IsObject(m) ? PathName(m) : std::string("-"));
                    }
                    ++n;
                    LOG("[diag] smc %s owner=%s mesh=%s mats=[%s] origin(%.0f,%.0f,%.0f) extent(%.0f,%.0f,%.0f) hidden=%u", ObjName(o).c_str(),
                        owner && IsObject(owner) ? FullName(owner).c_str() : "-", mesh && IsObject(mesh) ? PathName(mesh).c_str() : "-",
                        ms.c_str(), b[0], b[1], b[2], b[3], b[4], b[5], (hid & phid.bitMask) ? 1u : 0u);
                }
                LOG("[diag] %d level static meshes", n);
            } else {
                LOG("[diag] !levelmeshes: reflection lookups failed");
            }
        } else if (cmd == "!volscan1" || cmd == "!volscan2") {
            // dev (0.9.109): the unfocused mute - floats in GG2Game.exe's writable sections that are 1.0 now (1) and
            // 0.0 after the focus moved away (2)
            static std::vector<uintptr_t> s_ones;
            auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
            auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
            if (cmd == "!volscan1") {
                s_ones.clear();
                auto* sec = IMAGE_FIRST_SECTION(nt);
                for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
                    if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
                    const uintptr_t a0 = uintptr_t(base) + sec->VirtualAddress, n = sec->Misc.VirtualSize / 4;
                    for (uintptr_t k = 0; k < n; ++k) {
                        float f = 0.f;
                        if (game::Read(a0 + k * 4, f) && f == 1.f) s_ones.push_back(a0 + k * 4);
                    }
                }
                LOG("[volscan] %zu floats at 1.0", s_ones.size());
            } else {
                int hits = 0;
                for (uintptr_t a1 : s_ones) {
                    float f = 1.f;
                    if (game::Read(a1, f) && f == 0.f && hits++ < 20) LOG("[volscan] rva 0x%llX: 1.0 -> 0.0", (unsigned long long)(a1 - uintptr_t(base)));
                }
                LOG("[volscan] %d of %zu now 0.0", hits, s_ones.size());
            }
        } else if (cmd == "!sharetest") {
            xr::RequestShareTest();
        } else if (cmd == "!dumpstereo") {
            xr::RequestStereoDump();
        } else if (cmd.rfind("!shakediag", 0) == 0) {  // dev: !shakediag <seconds>
            vr::ShakeDiag(cmd.size() > 11 ? atoi(cmd.c_str() + 11) : 20);
        } else if (cmd == "!xrrecenter") {
            xr::RequestRecenter();
            LOG("[console] VR recenter requested");
        } else if (cmd == "!rdlast") {
            LOG("[renderdoc] last capture: %s", diag::renderdoc::LastCapturePath().c_str());
        } else {
            LOG("[console] unknown internal command %s", cmd.c_str());
        }
        return;
    }
    // the engine persists these to the user's live GG2SystemSettings.ini: arm the settings guard first
    if (_strnicmp(cmd.c_str(), "scale set", 9) == 0 || _strnicmp(cmd.c_str(), "setres", 6) == 0)
        settings_guard::MarkDirty();
    std::lock_guard l(g_mu);
    g_queue.push_back(cmd);
}

void Pump() {
    if (!g_fnConsoleCommand || t_inPump || GetCurrentThreadId() != g_gameThread) return;
    vr::audio::KeepPlayingInBackground();
    {
        std::vector<FlagReq> reqs;
        {
            std::lock_guard l(g_mu);
            reqs.swap(g_flagReq);
        }
        std::vector<std::string> states;
        {
            std::lock_guard l(g_mu);
            states.swap(g_stateReq);
        }
        for (const auto& st : states) {
            if (st.rfind("__stencilscale ", 0) == 0) {  // dev: GG2HudShootingAggregate.SetStencilSightScale(x)
                const float sc = float(atof(st.c_str() + 15));
                static const uintptr_t fn = FindObject("GG2Game.GG2HudShootingAggregate.SetStencilSightScale", "Function");
                int n = 0;
                for (int32_t i = 0, cnt = NumObjects(); i < cnt && fn; ++i) {
                    const uintptr_t o = ObjectAt(i);
                    if (!o || !IsA(o, "GG2HudShootingAggregate") || PathName(o).find("Default__") != std::string::npos) continue;
                    uint8_t parms[32]{};
                    memcpy(parms, &sc, sizeof sc);
                    uintptr_t vt = 0, pe = 0;
                    game::Read(o, vt), game::Read(vt + game::rva::ProcessEvent_VSlot, pe);
                    reinterpret_cast<PEFn>(pe)(o, fn, parms, nullptr);
                    ++n;
                }
                LOG("[console] SetStencilSightScale(%.2f) on %d HUD aggregate(s) (fn %p)", sc, n, (void*)fn);
                continue;
            }
            if (st.rfind("__gfx", 0) == 0) {
                char op[16]{}, cls[64]{}, path[200]{};
                int val = 0;
                const int n = sscanf_s(st.c_str() + 2, "%15s %63s %199s %d", op, unsigned(sizeof op), cls, unsigned(sizeof cls), path,
                                       unsigned(sizeof path), &val);
                const uintptr_t pl = n >= 2 ? vr::gfx::FindPlayer(cls) : 0;
                if (!pl) {
                    LOG("[gfx] no movie player like '%s'", cls);
                } else if (!strcmp(op, "gfxvar") && n >= 3) {
                    std::string v;
                    vr::gfx::GetString(pl, path, v);
                    LOG("[gfx] %s %s = '%s'", cls, path, v.c_str());
                } else if (!strcmp(op, "gfxset") && n >= 4) {
                    vr::gfx::SetBool(pl, path, val != 0);
                    LOG("[gfx] %s %s := %d", cls, path, val);
                } else if (!strcmp(op, "gfxnames")) {  // !gfxnames <class> <file>: which names in the file are root clips
                    std::ifstream in(path);
                    std::string nm;
                    int found = 0;
                    while (in >> nm) {
                        std::string v;
                        vr::gfx::GetString(pl, "_root." + nm + "._name", v);
                        if (v == nm) ++found, LOG("[gfx] %s root clip: %s", cls, nm.c_str());
                    }
                    LOG("[gfx] gfxnames done: %d", found);
                } else if (!strcmp(op, "gfxprobe")) {
                    for (int i = 0; i < 1000; ++i) {
                        std::string nm;
                        vr::gfx::GetString(pl, "_root.instance" + std::to_string(i) + "._name", nm);
                        if (!nm.empty()) LOG("[gfx] %s _root.instance%d exists (_name '%s')", cls, i, nm.c_str());
                    }
                    LOG("[gfx] probe done");
                }
                continue;
            }
            if (st.rfind("__opt ", 0) == 0 || st.rfind("__steamcfg ", 0) == 0) {
                // the system save package's Get/SetOptionSaveData or Get/SetSteamConfigSaveData (in memory)
                const bool steam = st.rfind("__steamcfg ", 0) == 0;
                char op[8]{};
                int off = -1, val = 0;
                sscanf_s(st.c_str() + (steam ? 11 : 6), "%7s %d %d", op, unsigned(sizeof op), &off, &val);
                const char* nGet = steam ? "GetSteamConfigSaveData" : "GetOptionSaveData";
                const char* nSet = steam ? "SetSteamConfigSaveData" : "SetOptionSaveData";
                uintptr_t obj = 0;
                for (int32_t i = 0, cnt = NumObjects(); i < cnt && !obj; ++i) {
                    const uintptr_t o = ObjectAt(i);
                    if (o && IsA(o, "GG2SaveDataSystemPackage") && PathName(o).find("Default__") == std::string::npos) obj = o;
                }
                auto findFn = [&](const char* name) {
                    for (const char* c : {"GG2Game.GG2SaveDataSystemPackage.", "GG2Game.GG2SaveDataBase.", "GG2Game.GG2SaveDataSystem."}) {
                        const uintptr_t f = FindObject((std::string(c) + name).c_str(), "Function");
                        if (f) return f;
                    }
                    return uintptr_t(0);
                };
                const uintptr_t fGet = findFn(nGet), fSet = findFn(nSet);
                if (!obj || !fGet || !fSet) {
                    LOG("[opt] system save %p, %s %p, %s %p: not found", (void*)obj, nGet, (void*)fGet, nSet, (void*)fSet);
                    continue;
                }
                uint32_t poff = 0;
                for (auto& q : Properties(fGet)) poff = q.offset;  // the single (out) struct parameter
                uintptr_t vt = 0, pe = 0;
                game::Read(obj, vt), game::Read(vt + game::rva::ProcessEvent_VSlot, pe);
                std::vector<uint8_t> parms(0x800, 0);
                reinterpret_cast<PEFn>(pe)(obj, fGet, parms.data(), nullptr);
                const uint8_t* d = parms.data() + poff;
                auto show = [&](const char* when) {
                    if (steam)
                        LOG("[opt] steam config %s: resolution %u displayMode %u vsync %u analogSens %u mouseSens %u", when, d[1], d[2], d[3], d[4], d[5]);
                    else
                        LOG("[opt] options %s: shot %u dkdkShot %u zoom %u slowSight %u | sightReverse %u sightSpeed %u", when, d[0], d[1], d[2], d[3], d[8], d[9]);
                };
                show("now");
                if (!strcmp(op, "set") && off >= 0 && off < (steam ? 6 : 16)) {
                    std::vector<uint8_t> sp(0x800, 0);
                    uint32_t soff = 0;
                    for (auto& q : Properties(fSet)) soff = q.offset;
                    memcpy(sp.data() + soff, d, steam ? 0x628 : 0x18);
                    sp[soff + off] = uint8_t(val);
                    reinterpret_cast<PEFn>(pe)(obj, fSet, sp.data(), nullptr);
                    std::fill(parms.begin(), parms.end(), 0);
                    reinterpret_cast<PEFn>(pe)(obj, fGet, parms.data(), nullptr);
                    show("after set");
                }
                continue;
            }
            if (st == "__shotstats") {
                const uintptr_t pc = LocalPlayerController();
                if (!pc) continue;
                int32_t shots = 0, hits = 0;
                uint32_t flags = 0;
                float cur[2]{}, loc[3]{}, dir[3]{}, hit[3]{};
                uintptr_t tgt = 0;
                game::Read(pc + 0xB88, shots), game::Read(pc + 0xB8C, hits);       // m_sm.ShotCount / hitCount
                game::Read(pc + 0xB98, flags);                                       // m_nsm flags
                game::Read(pc + 0xBA4, cur), game::Read(pc + 0xBB8, loc), game::Read(pc + 0xBC4, dir);
                game::Read(pc + 0xBD0, hit), game::Read(pc + 0xBE8, tgt);
                float zr = 0;
                game::Read(pc + 0xB98 + 0x68, zr);
                LOG("[diag] zoom %u rate %.2f charging %u", (flags >> 3) & 1, zr, (flags >> 5) & 1);
                LOG("[diag] shots %d hits %d | cursor (%.0f,%.0f) loc (%.0f,%.0f,%.0f) dir (%.3f,%.3f,%.3f) hitObj %u hit (%.0f,%.0f,%.0f) target %s",
                    shots, hits, cur[0], cur[1], loc[0], loc[1], loc[2], dir[0], dir[1], dir[2], (flags >> 4) & 1,
                    hit[0], hit[1], hit[2], tgt && IsObject(tgt) ? ObjName(tgt).c_str() : "-");
                continue;
            }
            GirlGotoState(st);
        }
        for (const auto& r : reqs) {
            uintptr_t engine = 0, gvc = 0, pc = LocalPlayerController(), hud = 0;
            game::Read(game::Addr(game::rva::GEngine), engine);
            if (r.world && engine && game::Read(engine + 0x6DC, gvc) && IsObject(gvc)) {
                auto* bits = reinterpret_cast<uint32_t*>(gvc + 0x108);  // GameViewportClient.bDisableWorldRendering
                *bits = r.on ? (*bits | 0x20) : (*bits & ~0x20u);
                LOG("[console] GameViewportClient.bDisableWorldRendering = %d", r.on);
            } else if (!r.world && pc && game::Read(pc + 0x4A0, hud) && IsObject(hud)) {
                auto* bits = reinterpret_cast<uint32_t*>(hud + 0x24C);  // HUD.bShowHUD
                *bits = r.on ? (*bits & ~0x2u) : (*bits | 0x2);
                LOG("[console] HUD.bShowHUD = %d (%s)", !r.on, ObjName(hud).c_str());
            } else {
                LOG("[console] flag request failed (no viewport client / HUD)");
            }
        }
    }
    if (g_cursorPin) {  // debug: hold the sight cursor (m_nsm.cursorScreenPosition, game-screen pixels)
        uintptr_t pc = LocalPlayerController();
        if (pc && ObjName(pc).find("ShootingPlayerController") != std::string::npos) {
            const float x = g_cursorX, y = g_cursorY;
            *reinterpret_cast<float*>(pc + 0xBA4) = x;  // pc is a validated live object on the game thread
            *reinterpret_cast<float*>(pc + 0xBA8) = y;
        }
    }
    std::deque<std::string> todo;
    {
        std::lock_guard l(g_mu);
        if (g_queue.empty()) return;
    }
    if (!LocalPlayerController()) return;  // keep commands queued until a local PlayerController exists
    {
        std::lock_guard l(g_mu);
        todo.swap(g_queue);
    }
    t_inPump = true;
    for (auto& c : todo) Execute(c);
    t_inPump = false;
}

bool Init(uint32_t gameThreadId) {
    g_gameThread = gameThreadId;
    uintptr_t pcClass = FindObject("Engine.PlayerController", "Class");
    uintptr_t engClass = FindObject("Engine.Engine", "Class");
    uintptr_t playerClass = FindObject("Engine.Player", "Class");
    g_fnConsoleCommand = FindObject("Engine.PlayerController.ConsoleCommand", "Function");
    PropInfo p;
    if (!pcClass || !engClass || !playerClass || !g_fnConsoleCommand) {
        LOG("[console] missing classes/function; console bridge disabled");
        return false;
    }
    if (FindProperty(engClass, "GamePlayers", p)) g_offGamePlayers = p.offset;
    if (FindProperty(playerClass, "Actor", p)) g_offPlayerActor = p.offset;
    for (auto& q : Properties(g_fnConsoleCommand)) {
        if (q.name == "Command") g_offCommand = q.offset;
        if (q.name == "bWriteToLog") g_offWriteToLog = q.offset, g_bitWriteToLog = q.bitMask ? q.bitMask : 1;
        if (q.name == "ReturnValue") g_offReturn = q.offset;
        g_parmsSize = std::max(g_parmsSize, q.offset + q.elementSize);
    }
    LOG("[console] ConsoleCommand params: Command@0x%X bWriteToLog@0x%X(bit 0x%X) ReturnValue@0x%X size=0x%X; "
        "Engine.GamePlayers@0x%X Player.Actor@0x%X",
        g_offCommand, g_offWriteToLog, g_bitWriteToLog, g_offReturn, g_parmsSize, g_offGamePlayers, g_offPlayerActor);
    // developer mode only (0.9.101; [debug] developer=1): the F-key hotkeys and the developer console (exec\\live.txt) - and
    // through them every dump / capture command. Not part of the players' debug mode.
    if (!config::Get().developer) return g_offGamePlayers && g_offPlayerActor && g_parmsSize;
    // Hotkeys: [hotkeys] F1..F12=<command>
    const std::wstring ini = game::ModDir() + L"\\ggdpvr.ini";
    for (int i = 1; i <= 12; ++i) {
        wchar_t key[8], buf[512]{};
        swprintf(key, 8, L"F%d", i);
        GetPrivateProfileStringW(L"hotkeys", key, L"", buf, 512, ini.c_str());
        if (buf[0]) {
            char cmd[512]{};
            WideCharToMultiByte(CP_UTF8, 0, buf, -1, cmd, sizeof cmd, nullptr, nullptr);
            g_hotkeys.push_back({VK_F1 + i - 1, cmd, false});
            LOG("[console] hotkey F%d = %s", i, cmd);
        }
    }
    if (HANDLE t = CreateThread(nullptr, 0, Watcher, nullptr, 0, nullptr)) CloseHandle(t);
    return g_offGamePlayers && g_offPlayerActor && g_parmsSize;
}
}  // namespace ggdpvr::ue3::console
