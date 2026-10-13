#include "render_res.h"

#include <windows.h>
#include <windowsx.h>
#include <atomic>
#include <cstring>
#include <string>

#include "../core/config.h"
#include "../core/game.h"
#include "../core/log.h"
#include "../ue3/console.h"
#include "../xr/xr_shared.h"

namespace ggdpvr::vr {
namespace {
WNDPROC g_origProc = nullptr;
HWND g_hwnd = nullptr;
std::atomic<uint32_t> g_vrW{0}, g_vrH{0};
std::string g_lastSetres;

// FWindowsViewport::Resize (0x161B850) clamps windowed sizes to cached desktop rects:
// work area {L,T,R,B} at 0x329ABE0 and the virtual-screen rect {L,T} 0x329ABF8 / {R,B} 0x329AC00.
// Nothing else reads them after startup, so they are enlarged while a VR resolution is applied.
constexpr uint32_t kWorkArea = 0x329ABE0, kVirtTL = 0x329ABF8, kVirtBR = 0x329AC00;
int32_t g_savedWork[4]{}, g_savedVirt[4]{};
bool g_desktopLifted = false;
void LiftDesktopClamp(bool lift) {
    auto* work = reinterpret_cast<int32_t*>(game::Addr(kWorkArea));
    auto* vtl = reinterpret_cast<int32_t*>(game::Addr(kVirtTL));
    auto* vbr = reinterpret_cast<int32_t*>(game::Addr(kVirtBR));
    if (lift && !g_desktopLifted) {
        memcpy(g_savedWork, work, 16);
        memcpy(g_savedVirt, vtl, 8), memcpy(g_savedVirt + 2, vbr, 8);
        const int32_t big[4] = {g_savedWork[0], g_savedWork[1], 16384, 16384};
        memcpy(work, big, 16);
        vbr[0] = 16384, vbr[1] = 16384;
        g_desktopLifted = true;
        LOG("[res] desktop clamp lifted (work area was %d,%d-%d,%d)", g_savedWork[0], g_savedWork[1], g_savedWork[2],
            g_savedWork[3]);
    } else if (!lift && g_desktopLifted) {
        memcpy(work, g_savedWork, 16);
        memcpy(vtl, g_savedVirt, 8), memcpy(vbr, g_savedVirt + 2, 8);
        g_desktopLifted = false;
        LOG("[res] desktop clamp restored");
    }
}

std::atomic<int> g_wndLogs{0};
void LogWindowState(const char* why) {
    if (!g_hwnd) return;
    RECT wr{}, cr{};
    GetWindowRect(g_hwnd, &wr);
    GetClientRect(g_hwnd, &cr);
    LOG("[res] window (%s): rect %ld,%ld %ldx%ld client %ldx%ld style %08lX zoomed %d iconic %d foreground %d dpi %u",
        why, wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top, cr.right, cr.bottom,
        DWORD(GetWindowLongPtrW(g_hwnd, GWL_STYLE)), IsZoomed(g_hwnd), IsIconic(g_hwnd),
        GetForegroundWindow() == g_hwnd, GetDpiForWindow(g_hwnd));
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_WINDOWPOSCHANGING && g_wndLogs < 60) {
        const auto* wp2 = reinterpret_cast<const WINDOWPOS*>(lp);
        if (!(wp2->flags & SWP_NOSIZE)) {
            ++g_wndLogs;
            LOG("[res] WM_WINDOWPOSCHANGING %dx%d flags %X", wp2->cx, wp2->cy, wp2->flags);
        }
    }
    if (msg >= WM_LBUTTONDOWN && msg <= WM_LBUTTONUP) {  // diag: menu pointer clicks
        static int s_btnLogs = 0;
        if (s_btnLogs++ < 12) LOG("[res] %s (%d,%d) foreground %d", msg == WM_LBUTTONDOWN ? "WM_LBUTTONDOWN" : "WM_LBUTTONUP", GET_X_LPARAM(lp), GET_Y_LPARAM(lp), GetForegroundWindow() == h);
    }
    LRESULT r = CallWindowProcW(g_origProc, h, msg, wp, lp);
    if (msg == WM_SIZE && g_wndLogs < 60) {
        ++g_wndLogs;
        LOG("[res] WM_SIZE type %u -> %ux%u", unsigned(wp), LOWORD(lp), HIWORD(lp));
    }
    if (msg == WM_GETMINMAXINFO) {  // allow client areas larger than the desktop
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMaxTrackSize = {16384, 16384};
        mmi->ptMaxSize = {16384, 16384};
    }
    return r;
}

BOOL CALLBACK FindGameWindow(HWND h, LPARAM out) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    wchar_t cls[128]{};
    GetClassNameW(h, cls, 128);
    if (pid == GetCurrentProcessId() && !wcscmp(cls, L"LaunchUnrealUWindowsClient")) {
        *reinterpret_cast<HWND*>(out) = h;
        return FALSE;
    }
    return TRUE;
}
}  // namespace

// Called before every resolution change: the game can recreate its window or install its own window procedure
// after we subclassed it (seen once the mod initialised early, 0.7.3+), which silently dropped the size-clamp
// lift and let Windows clamp the VR render size to the desktop height.
bool InstallWindowHook() {
    if (g_hwnd && IsWindow(g_hwnd) &&
        GetWindowLongPtrW(g_hwnd, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(&WndProc))
        return true;
    HWND h = nullptr;
    EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&h));
    if (!h) return false;
    const bool again = g_hwnd != nullptr;
    g_origProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&WndProc)));
    g_hwnd = h;
    LOG("[res] game window %p subclassed%s (window size clamp lifted)", h, again ? " AGAIN (hook had been replaced)" : "");
    return true;
}

void SetVrRenderSize(uint32_t w, uint32_t h) {
    const bool changedW = g_vrW.exchange(w) != w, changedH = g_vrH.exchange(h) != h;
    if (changedW || changedH) LOG("[res] VR render size %ux%u", w, h);
}

std::atomic<uint64_t> g_wantSize{0};  // backbuffer size the last setres asked for (0: none/fallback)

bool g_forced = false;  // debug override via !vrres
void SetRenderMode(bool immersive) {
    const auto& cfg = config::Get();
    if (!cfg.vrResEnabled || g_forced) return;
    std::string cmd;
    if (immersive && g_vrW && g_vrH)
        cmd = "setres " + std::to_string(g_vrW.load()) + "x" + std::to_string(g_vrH.load()) + "w";
    else if (!immersive)
        cmd = "setres " + std::to_string(cfg.vrScreenResW) + "x" + std::to_string(cfg.vrScreenResH) + "w";
    if (cmd.empty()) return;
    static ULONGLONG s_issuedAt = 0;
    static int s_retries = 0;
    static bool s_fallback = false;  // VR size would not apply in this immersive scene: render it at the screen size
    if (!immersive) s_fallback = false;
    if (s_fallback) {
        g_wantSize = 0;
        return;
    }
    if (cmd == g_lastSetres) {
        // A setres issued during a level load (loading movie) can update the UE3 viewport size without
        // resizing the D3D backbuffer. Verify against the presented backbuffer and re-apply if it never took.
        const uint32_t tw = immersive ? g_vrW.load() : cfg.vrScreenResW, th = immersive ? g_vrH.load() : cfg.vrScreenResH;
        const uint64_t bb = xr::BackbufferSize();
        g_wantSize = (uint64_t(tw) << 32) | th;
        if (!bb || bb == g_wantSize) { s_retries = 0; return; }
        if (GetTickCount64() - s_issuedAt < 1500) return;
        LogWindowState("size mismatch");
        if (immersive && s_retries >= 2) {
            // Fallback: keep the viewport and backbuffer consistent at the screen size (correct, lower resolution)
            s_fallback = true;
            g_lastSetres.clear();
            ue3::console::Enqueue("setres " + std::to_string(cfg.vrScreenResW) + "x" + std::to_string(cfg.vrScreenResH) + "w");
            LiftDesktopClamp(false);
            LOG("[res] VR resolution did not apply; FALLBACK to %ux%u for this scene", cfg.vrScreenResW, cfg.vrScreenResH);
            return;
        }
        if (s_retries >= 10) return;
        ++s_retries;
        LOG("[res] backbuffer is %ux%u, expected %ux%u: re-applying (retry %d)", uint32_t(bb >> 32), uint32_t(bb),
            tw, th, s_retries);
        InstallWindowHook();
        if (IsZoomed(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
        // the viewport already believes it has the target size, so bounce through another size first
        ue3::console::Enqueue(immersive ? "setres " + std::to_string(cfg.vrScreenResW) + "x" +
                                              std::to_string(cfg.vrScreenResH) + "w"
                                        : "setres " + std::to_string(cfg.vrScreenResW - 2) + "x" +
                                              std::to_string(cfg.vrScreenResH - 2) + "w");
        ue3::console::Enqueue(cmd);
        s_issuedAt = GetTickCount64();
        return;
    }
    g_lastSetres = cmd;
    g_wantSize = immersive ? (uint64_t(g_vrW.load()) << 32) | g_vrH.load()
                           : (uint64_t(cfg.vrScreenResW) << 32) | cfg.vrScreenResH;
    s_issuedAt = GetTickCount64();
    s_retries = 0;
    InstallWindowHook();
    g_wndLogs = 0;
    LogWindowState(immersive ? "before VR setres" : "before screen setres");
    LiftDesktopClamp(immersive);
    ue3::console::Enqueue(cmd);
    LOG("[res] %s (%s)", cmd.c_str(), immersive ? "immersive" : "virtual screen");
}
void* GameWindow() { return g_hwnd; }
bool RenderSizeSettled() {
    if (!config::Get().vrResEnabled || g_forced) return true;
    const uint64_t want = g_wantSize, bb = xr::BackbufferSize();
    return !want || !bb || want == bb;
}

void ForceResolutionTest(uint32_t w, uint32_t h) {
    InstallWindowHook();
    LiftDesktopClamp(w > 0);
    const auto& cfg = config::Get();
    const std::string cmd = w ? "setres " + std::to_string(w) + "x" + std::to_string(h) + "w"
                              : "setres " + std::to_string(cfg.vrScreenResW) + "x" + std::to_string(cfg.vrScreenResH) + "w";
    g_lastSetres.clear();
    g_forced = w > 0;  // !vrres 0x0 returns to automatic management
    ue3::console::Enqueue(cmd);
}
}  // namespace ggdpvr::vr
