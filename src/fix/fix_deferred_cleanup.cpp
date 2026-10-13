#include "fix_deferred_cleanup.h"

#include <windows.h>
#include <intrin.h>
#include <atomic>

#include "../core/config.h"
#include "../core/game.h"
#include "../core/hooks.h"
#include "../core/log.h"

namespace ggdpvr::fix::cleanup {
namespace {
using namespace ggdpvr::game;

constexpr uint32_t kPendingCleanupDtor = 0x41B220;  // FPendingCleanupObjects::~FPendingCleanupObjects
constexpr uint32_t kFlushRenderingCommands = 0x41EDA0;
constexpr uint32_t kEngineLoopReturn = 0x16A1F17;   // return address of the call in FEngineLoop::Tick
constexpr uint32_t kFrameSyncEnabled = 0x30571C0;   // cleared by LoadMap movie start, set by StopMovie
constexpr uint32_t kGIsThreadedRendering = 0x325B9C4;

using FnThis = void(__fastcall*)(void*);
FnThis o_Dtor = nullptr;
FnThis FlushRenderingCommands = nullptr;
bool g_enabled = false;
std::atomic<uint64_t> g_unsafeWindows{0}, g_flushes{0};

void __fastcall hk_Dtor(void* list) {
    if (reinterpret_cast<uintptr_t>(_ReturnAddress()) == Addr(kEngineLoopReturn)) {
        struct { uintptr_t data; int32_t num, max; } arr{};
        uint32_t syncEnabled = 1, threaded = 0;
        Read(uintptr_t(list), arr);
        Read(Addr(kFrameSyncEnabled), syncEnabled);
        Read(Addr(kGIsThreadedRendering), threaded);
        if (arr.num > 0 && !syncEnabled && threaded) {
            const uint64_t n = ++g_unsafeWindows;
            if (g_enabled) {
                LARGE_INTEGER f, a, b;
                QueryPerformanceFrequency(&f);
                QueryPerformanceCounter(&a);
                FlushRenderingCommands(nullptr);
                QueryPerformanceCounter(&b);
                ++g_flushes;
                LOG("[fix.cleanup] frame sync skipped (loading movie) with %d deferred-cleanup objects pending: "
                    "flushed rendering commands first (%.2f ms) [window #%llu]",
                    arr.num, double(b.QuadPart - a.QuadPart) * 1000.0 / double(f.QuadPart), n);
            } else {
                LOG("[fix.cleanup] UNSAFE: engine deletes %d deferred-cleanup objects without frame sync "
                    "(loading movie) - fix disabled [window #%llu]",
                    arr.num, n);
            }
        }
    }
    o_Dtor(list);
}

const uint8_t kDtorBytes[] = {0x48, 0x89, 0x4C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x30};
const uint8_t kFlushBytes[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x1D};
const uint8_t kCallSiteBytes[] = {0xE8};  // call rel32 at kEngineLoopReturn - 5
}  // namespace

bool Install() {
    g_enabled = config::Get().fixDeferredCleanupFlush;
    // Verify the three code facts the fix depends on.
    if (!VerifyBytes(kFlushRenderingCommands, kFlushBytes, sizeof kFlushBytes) ||
        !VerifyBytes(kEngineLoopReturn - 5, kCallSiteBytes, sizeof kCallSiteBytes)) {
        LOG("[fix.cleanup] code verification failed; not installed");
        return false;
    }
    int32_t rel = 0;
    Read(Addr(kEngineLoopReturn - 4), rel);
    if (Addr(kEngineLoopReturn) + rel != Addr(kPendingCleanupDtor)) {
        LOG("[fix.cleanup] engine loop call site does not target FPendingCleanupObjects dtor; not installed");
        return false;
    }
    FlushRenderingCommands = reinterpret_cast<FnThis>(Addr(kFlushRenderingCommands));
    bool ok = hooks::Create("FPendingCleanupObjects::~dtor", kPendingCleanupDtor, kDtorBytes, sizeof kDtorBytes,
                            (void*)&hk_Dtor, (void**)&o_Dtor);
    LOG("[fix.cleanup] installed (%s)", g_enabled ? "ENABLED: flush before unsafe deletion" : "disabled: log only");
    return ok;
}
}  // namespace ggdpvr::fix::cleanup
