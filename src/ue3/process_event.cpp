#include "process_event.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

#include "../core/config.h"
#include "../core/game.h"
#include "../core/hooks.h"
#include "../core/log.h"
#include "console.h"
#include "objects.h"

namespace ggdpvr::ue3::pe {
namespace {
using PEFn = void(__fastcall*)(uintptr_t self, uintptr_t fn, void* parms, void* result);
constexpr int kMaxImpl = 8;
PEFn g_orig[kMaxImpl]{};

struct FnInfo {
    std::string name;
    bool match = false;
    uint64_t total = 0, window = 0;
};
std::mutex g_mu;
std::unordered_map<uintptr_t, FnInfo> g_fns;
ULONGLONG g_lastSummary = 0, g_lastCursor = 0;
uintptr_t g_stdPcClass = 0;     // GG2Game.GG2StandardPlayerController
ue3::PropInfo g_cursorProp{};   // property covering +0xBA4 (if reflected)

void Summary() {
    std::vector<std::pair<uint64_t, std::string>> v;
    for (auto& [fn, info] : g_fns)
        if (info.window) v.emplace_back(info.window, info.name), info.window = 0;
    std::sort(v.rbegin(), v.rend());
    LOG("ProcessEvent summary (last %.0fs): %zu distinct functions; top:", config::Get().peSummarySeconds, v.size());
    for (size_t i = 0; i < v.size() && i < 30; ++i) LOG("  %8llu  %s", v[i].first, v[i].second.c_str());
}

void CursorProbe(uintptr_t self) {
    float x = 0, y = 0;
    game::Read(self + 0xBA4, x);
    game::Read(self + 0xBA8, y);
    LOG("cursor: %s +0xBA4=%.3f +0xBA8=%.3f (reflected property at 0xBA4: %s %s)", ue3::ObjName(self).c_str(), x, y,
        g_cursorProp.type.c_str(), g_cursorProp.name.c_str());
}

void OnEvent(uintptr_t self, uintptr_t fn) {
    console::Pump();
    std::lock_guard l(g_mu);
    auto it = g_fns.find(fn);
    if (it == g_fns.end()) {
        FnInfo info;
        info.name = ue3::PathName(fn);
        for (auto& f : config::Get().peLogFilters)
            if (info.name.find(f) != std::string::npos) info.match = true;
        it = g_fns.emplace(fn, std::move(info)).first;
    }
    FnInfo& info = it->second;
    ++info.total, ++info.window;
    if (config::Get().logging && info.match && info.total <= uint64_t(config::Get().peLogFirstN))
        LOG("PE #%llu %s  on %s", info.total, info.name.c_str(), ue3::FullName(self).c_str());
    ULONGLONG now = GetTickCount64();
    if (config::Get().logging && now - g_lastSummary > ULONGLONG(config::Get().peSummarySeconds * 1000)) {
        g_lastSummary = now;
        Summary();
    }
    static std::unordered_map<uintptr_t, bool> isStdPc;  // class -> derives from GG2StandardPlayerController
    if (config::Get().logging && config::Get().cursorProbe && g_stdPcClass && now - g_lastCursor > 1000) {
        uintptr_t cls = ue3::ClassOf(self);
        auto c = isStdPc.find(cls);
        if (c == isStdPc.end()) c = isStdPc.emplace(cls, ue3::IsA(self, "GG2StandardPlayerController")).first;
        if (!c->second) return;
        g_lastCursor = now;
        CursorProbe(self);
    }
}

struct PostEntry { std::atomic<uintptr_t> fn{0}; PostFn cb = nullptr; };
PostEntry g_post[8];
std::atomic<int> g_postCount{0};
std::atomic<SkipFn> g_skip{nullptr};

template <int I>
void __fastcall Detour(uintptr_t self, uintptr_t fn, void* parms, void* result) {
    OnEvent(self, fn);
    if (const SkipFn skip = g_skip.load(std::memory_order_relaxed); skip && skip(self, fn)) return;
    g_orig[I](self, fn, parms, result);
    for (int i = 0, n = g_postCount.load(std::memory_order_acquire); i < n; ++i)
        if (g_post[i].fn.load(std::memory_order_relaxed) == fn) g_post[i].cb(self, parms);
}
void* const kDetours[kMaxImpl] = {(void*)&Detour<0>, (void*)&Detour<1>, (void*)&Detour<2>, (void*)&Detour<3>,
                                  (void*)&Detour<4>, (void*)&Detour<5>, (void*)&Detour<6>, (void*)&Detour<7>};
}  // namespace

void SetSkip(SkipFn cb) { g_skip = cb; }
bool RegisterPost(uintptr_t fn, PostFn cb) {
    const int i = g_postCount.load();
    if (!fn || i >= 8) return false;
    g_post[i].cb = cb;
    g_post[i].fn = fn;
    g_postCount.store(i + 1, std::memory_order_release);
    LOG("ProcessEvent post-hook #%d on %s", i, ue3::PathName(fn).c_str());
    return true;
}

bool Install() {
    // Distinct ProcessEvent implementations (UObject::ProcessEvent, AActor::ProcessEvent, ...).
    std::set<uintptr_t> impls;
    std::unordered_map<uintptr_t, std::string> example;
    for (int32_t i = 0, n = ue3::NumObjects(); i < n; ++i) {
        uintptr_t o = ue3::ObjectAt(i), vt = 0, f = 0;
        if (!o || !game::Read(o, vt) || !game::InRData(vt) || !game::Read(vt + game::rva::ProcessEvent_VSlot, f))
            continue;
        if (!game::InText(f)) continue;
        if (impls.insert(f).second) example[f] = ue3::FullName(o);
    }
    LOG("ProcessEvent: %zu distinct implementations at vtable slot 0x%X", impls.size(), game::rva::ProcessEvent_VSlot);
    int idx = 0;
    for (uintptr_t f : impls) {
        LOG("  impl rva 0x%X (e.g. %s)", game::ToRva(f), example[f].c_str());
        if (idx >= kMaxImpl) {
            LOG("  too many implementations; not hooking the rest");
            break;
        }
        char nm[64];
        snprintf(nm, sizeof nm, "ProcessEvent#%d", idx);
        if (hooks::CreateAt(nm, (void*)f, kDetours[idx], (void**)&g_orig[idx])) ++idx;
    }
    g_stdPcClass = ue3::FindObject("GG2Game.GG2StandardPlayerController", "Class");
    if (g_stdPcClass && ue3::PropertyAt(g_stdPcClass, 0xBA4, g_cursorProp))
        LOG("sight cursor +0xBA4 is reflected as %s %s::%s (offset 0x%X size 0x%X)", g_cursorProp.type.c_str(),
            g_cursorProp.owner.c_str(), g_cursorProp.name.c_str(), g_cursorProp.offset, g_cursorProp.elementSize);
    else
        LOG("sight cursor +0xBA4: no reflected property covers it (native-only field?)");
    return idx > 0 && hooks::EnableAll();
}
}  // namespace ggdpvr::ue3::pe
