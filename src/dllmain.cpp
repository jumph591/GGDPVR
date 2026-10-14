// GGDPVR core DLL (loaded by the dinput8.dll proxy from Binaries\Win64\GGDPVR\GGDPVR.dll).
// DllMain only records the main (= UE3 game) thread id and starts the init thread.
#include <windows.h>
#include <intrin.h>
#include <cwctype>
#include <string>

#include "core/config.h"
#include "core/game.h"
#include "core/hooks.h"
#include "core/log.h"
#include "core/settings_guard.h"
#include "diag/rr_probe.h"
#include "fix/fix_deferred_cleanup.h"
#include "ue3/objects.h"
#include "ue3/console.h"
#include "ue3/process_event.h"
#include "vr/menu_pointer.h"
#include "vr/vpad.h"
#include "vr/head_tracking.h"
#include "vr/headset_audio.h"
#include "xr/present_hook.h"
#include "xr/xr_screen.h"

#define GGDPVR_VERSION "1.0.1"

namespace {
DWORD g_mainThread = 0;

// Process initialisation (all static DLLs loaded and their DllMains run - ReShade, Steam overlay, ...) happens
// with the main thread holding the loader lock. Hooking (MinHook suspends threads) from this thread during that
// phase deadlocks against other modules' own initialisation (seen as 35 s stalls, then hangs).
// Wait until the loader lock has no owner before doing anything.
void WaitForProcessInit() {
    const auto* peb = reinterpret_cast<const uint8_t*>(__readgsqword(0x60));
    const auto* lock = *reinterpret_cast<RTL_CRITICAL_SECTION* const*>(peb + 0x110);  // PEB.LoaderLock
    const ULONGLONG t0 = GetTickCount64();
    int freeStreak = 0;
    while (GetTickCount64() - t0 < 10000 && freeStreak < 20) {
        freeStreak = lock && lock->OwningThread ? 0 : freeStreak + 1;
        Sleep(1);
    }
}

DWORD WINAPI InitThread(LPVOID) {
    WaitForProcessInit();
    using namespace ggdpvr;
    log::Init();
    LOGP("GGDPVR %s starting (game thread %lu)", GGDPVR_VERSION, g_mainThread);
    config::Load(game::ModDir());
    const auto& cfg = config::Get();
    log::Configure(cfg.logging);  // log=0 (and no developer=1): nothing is written
    {  // the settings the player set ([vr] / [debug] keys present in ggdpvr.ini)
        const std::wstring ini = game::ModDir() + L"\\ggdpvr.ini";
        std::string set;
        for (const wchar_t* sec : {L"vr", L"advanced", L"controls", L"debug"}) {
            std::wstring buf(32768, L'\0');
            const DWORD n = GetPrivateProfileSectionW(sec, buf.data(), DWORD(buf.size()), ini.c_str());
            for (const wchar_t* p = buf.c_str(); p < buf.c_str() + n && *p; p += wcslen(p) + 1) {
                char k[512]{};
                WideCharToMultiByte(CP_UTF8, 0, p, -1, k, sizeof k, nullptr, nullptr);
                // a typed-in audio device name could identify the player's hardware: only whether one is set
                if (!_strnicmp(k, "headset_audio_device=", 21) && k[21]) strcpy_s(k, "headset_audio_device=(set)");
                set += (set.empty() ? "" : ", ") + std::string(k);
            }
        }
        if (set.size() > 3800) set = set.substr(0, 3800) + " ...";
        LOGP("mode: %s | settings: %s", cfg.developer ? "developer" : "log", set.empty() ? "(defaults)" : set.c_str());
        if (!cfg.vrControlsInvalid.empty())
            LOGP("[controls] not understood (ignored): %s", cfg.vrControlsInvalid.c_str());
    }
    LOG("config: rr_probe=%d rr_trace_lifecycle=%d fix.deferred_cleanup_flush=%d ue3.bootstrap=%d processevent_hook=%d",
        cfg.rrProbe, cfg.rrTraceLifecycle, cfg.fixDeferredCleanupFlush, cfg.ue3Bootstrap, cfg.processEventHook);
    {  // platform: the Windows version, or Wine / Proton and the host system (Linux players' reports)
        const HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
        using WineVersionFn = const char*(__cdecl*)();
        using WineHostFn = void(__cdecl*)(const char**, const char**);
        OSVERSIONINFOW vi{sizeof vi};
        if (const auto rv = nt ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(nt, "RtlGetVersion")) : nullptr) rv(&vi);
        const auto wv = nt ? reinterpret_cast<WineVersionFn>(GetProcAddress(nt, "wine_get_version")) : nullptr;
        const auto wh = nt ? reinterpret_cast<WineHostFn>(GetProcAddress(nt, "wine_get_host_version")) : nullptr;
        if (wv) {
            const char *sys = nullptr, *rel = nullptr;
            if (wh) wh(&sys, &rel);
            // Proton sets STEAM_COMPAT_DATA_PATH (a path under the home folder: only whether it is set is logged)
            const bool proton = GetEnvironmentVariableW(L"STEAM_COMPAT_DATA_PATH", nullptr, 0) > 0;
            LOGP("platform: Wine %s%s on %s %s (reports Windows %lu.%lu build %lu)", wv(), proton ? " (Proton)" : "",
                 sys ? sys : "?", rel ? rel : "?", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
        } else {
            LOGP("platform: Windows %lu.%lu build %lu", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber);
        }
    }
    if (!game::Init() || !hooks::Init()) return 0;
    const DWORD loaderThread = g_mainThread;
    g_mainThread = game::MainThreadId();
    LOG("game thread = %lu (earliest thread; DllMain ran on %lu)", g_mainThread, loaderThread);

    settings_guard::InstallExitHooks();
    settings_guard::SetExitCallback(&xr::ShutdownForExit);
    vr::vpad::Install();  // xinput1_3 is a static import: loaded by now
    vr::pointer::Install();
    vr::audio::Install();  // before the engine creates XAudio2
    diag::rr::SetGameThread(g_mainThread);
    diag::rr::Install();
    fix::cleanup::Install();
    // virtual gamepad first: the game's first menus run long before the UE3 bootstrap completes
    vr::vpad::Configure(cfg.vrEnabled && cfg.vrControllers);
    hooks::EnableAll();
    if (cfg.vrEnabled) xr::InstallPresentHook(&xr::OnPresent);

    if (!cfg.ue3Bootstrap) return 0;
    // Wait for the engine to exist, then bootstrap reflection.
    bool objs = false, refl = false;
    for (int i = 0; i < 600 && !(objs && refl); ++i) {
        uintptr_t engine = 0;
        game::Read(game::Addr(game::rva::GEngine), engine);
        if (engine) {
            if (!objs) objs = ue3::BootstrapObjects();
            if (objs && !refl) refl = ue3::BootstrapReflection();
        }
        if (!vr::vpad::Installed()) vr::vpad::Install();  // xinput1_3 was not loaded yet at DllMain
        if (!(objs && refl)) Sleep(500);
    }
    if (!objs || !refl) {
        LOG("UE3 bootstrap FAILED (objects=%d reflection=%d)", objs, refl);
        return 0;
    }
    LOGP("UE3 bootstrap OK: %d objects, %d names", ue3::NumObjects(), ue3::NumNames());
    if (cfg.ue3DumpReflection && cfg.developer) ue3::DumpReflection(log::Directory() + L"\\reflection_dump.txt");
    if (cfg.processEventHook) {
        ue3::pe::Install();
        ue3::console::Init(g_mainThread);
        // Renderer-specific defaults
        std::wstring cmdline = GetCommandLineW();
        for (auto& ch : cmdline) ch = towlower(ch);
        // (0.9.105: the loader adds -d3d11 to the command line the game reads, and says so in GGDPVR_D3D11 - this
        // reads the real command line)
        wchar_t forced[4]{};
        const bool added = GetEnvironmentVariableW(L"GGDPVR_D3D11", forced, 4) && forced[0] == L'1';
        const bool d3d11 = added || cmdline.find(L"-d3d11") != std::wstring::npos || cmdline.find(L"-dx11") != std::wstring::npos;
        LOGP("renderer: %s (%s)", d3d11 ? "D3D11" : "D3D9", added ? "added by the mod" : "from the command line");
        wchar_t dxgi11[4]{};
        LOGP("DXGI factory: %s", GetEnvironmentVariableW(L"GGDPVR_DXGI11", dxgi11, 4) && dxgi11[0] == L'1'
                                     ? "1.1 (given by the mod - capture tools can share the image)" : "the game's own (1.0)");
        if (d3d11 && cfg.d3d11DisableMotionBlur) ue3::console::Enqueue("scale set MotionBlur False");
        if (d3d11 && cfg.d3d11MsaaSamples > 0)
            ue3::console::Enqueue("scale set MaxMultiSamples " + std::to_string(cfg.d3d11MsaaSamples));
        // [vr] dynamic_shadows=0: the moving shadows off in VR (~2 ms of GPU per stereo frame in stages - headset test:
        // a locked 72 fps without them); the user's own setting comes back on exit (settings_guard)
        if (d3d11 && cfg.vrEnabled && !cfg.vrDynamicShadows) ue3::console::Enqueue("scale set DynamicShadows False");
        // VSync off in VR (0.9.97): with the desktop mirror on it would tie the headset's frames to the monitor; the
        // player's own setting comes back on exit (settings_guard)
        if (d3d11 && cfg.vrEnabled) ue3::console::Enqueue("scale set UseVsync False");
        if (cfg.vrEnabled && cfg.vrMode >= 1) {
            // head pose rendered by the game thread must match the frame presented by the render thread
            ue3::console::Enqueue("scale set OneFrameThreadLag False");
            vr::InstallHeadTracking();
        }
    }
    return 0;
}
}  // namespace

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(mod);
        g_mainThread = GetCurrentThreadId();
        // Before the engine reads its config: undo overrides a crashed/killed previous run left behind.
        ggdpvr::settings_guard::OnProcessAttach();
        if (HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr)) CloseHandle(t);
    } else if (reason == DLL_PROCESS_DETACH) {
        // Clean exit (after the engine saved its config): put the user's own settings back.
        ggdpvr::settings_guard::OnProcessDetach();
    }
    return TRUE;
}
