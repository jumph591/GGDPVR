#include "vf_trace.h"

#include <windows.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../core/config.h"
#include "../core/game.h"
#include "../core/hooks.h"
#include "../core/log.h"

namespace ggdpvr::diag::vf {
namespace {
using namespace ggdpvr::game;

struct Event {
    double t;
    const char* kind;
    DWORD tid;
    std::string stack;
};
std::mutex g_mu;
std::unordered_map<uintptr_t, std::vector<Event>> g_hist;
LARGE_INTEGER g_freq{}, g_t0{};

double Now() {
    LARGE_INTEGER n;
    QueryPerformanceCounter(&n);
    return double(n.QuadPart - g_t0.QuadPart) / double(g_freq.QuadPart);
}
uint32_t Vt(uintptr_t obj) {
    uintptr_t vt = 0;
    return (Read(obj, vt) && InRData(vt)) ? ToRva(vt) : 0;
}
void Add(uintptr_t obj, const char* kind, bool withStack, bool reset = false) {
    Event e{Now(), kind, GetCurrentThreadId(), withStack ? StackRvas(2, 14) : std::string()};
    std::lock_guard l(g_mu);
    auto& h = g_hist[obj];
    if (reset) h.clear();
    if (h.size() < 64) h.push_back(std::move(e));
}

using Fn1 = void(__fastcall*)(void*);
using FnCtor = void*(__fastcall*)(void*, int, void*);
using Fn6 = void*(__fastcall*)(void*, void*, void*, void*, void*, void*);
FnCtor o_Ctor = nullptr;
Fn1 o_BeginInit = nullptr, o_BeginRelease = nullptr, o_MeshRelease = nullptr;
Fn6 o_InitVFs = nullptr;

void* __fastcall hk_Ctor(void* self, int flag, void* data) {
    void* r = o_Ctor(self, flag, data);
    Add(uintptr_t(self), "ctor", true, true);
    return r;
}
void __fastcall hk_BeginInit(void* res) {
    if (Vt(uintptr_t(res)) == kVFVtable) Add(uintptr_t(res), "BeginInitResource", true);
    o_BeginInit(res);
}
void __fastcall hk_BeginRelease(void* res) {
    if (Vt(uintptr_t(res)) == kVFVtable) Add(uintptr_t(res), "BeginReleaseResource", true);
    o_BeginRelease(res);
}
// FSkeletalMeshObjectGPUSkin::ReleaseResources (vtable slot 3 of 0x251A0C0)
void __fastcall hk_MeshRelease(void* self) {
    uintptr_t m = uintptr_t(self);
    struct { uintptr_t data; int32_t num, max; } lods{};
    Read(m + 0x6C, lods);
    std::string list;
    for (int l = 0; l < lods.num && l < 8; ++l)
        for (uint32_t blk : {0x84u, 0xE4u}) {
            struct { uintptr_t data; int32_t num, max; } a{};
            if (!Read(lods.data + l * 0x154 + blk, a)) continue;
            for (int i = 0; i < a.num && i < 32; ++i) {
                uintptr_t f = 0;
                uint32_t fl = 0;
                if (Read(a.data + 8 * i, f) && f && Read(f + 0x20, fl)) {
                    char b[64];
                    snprintf(b, sizeof b, " %p(init=%u)", (void*)f, fl & 1);
                    list += b;
                    Add(f, "owner.ReleaseResources", false);
                }
            }
        }
    LOG("[vf] MeshObject %p ReleaseResources thread=%lu lods=%d factories:%s", self, GetCurrentThreadId(), lods.num,
        list.c_str());
    o_MeshRelease(self);
}
// Per-LOD vertex factory creation (array 0 path, 0x970480). Logs who creates factories and when.
// Uses rcx, rdx, r8, r9 and one stack argument; forward six to be safe.
void* __fastcall hk_InitVFs(void* a, void* b, void* c, void* d, void* e, void* f) {
    LOG("[vf] InitVertexFactories(block=%p, %p, %p, %p) thread=%lu stack: %s", a, b, c, d, GetCurrentThreadId(),
        StackRvas(1, 12).c_str());
    return o_InitVFs(a, b, c, d, e, f);
}

const uint8_t kCtor[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10};
const uint8_t kBeginInit[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x60, 0x48, 0xC7, 0x44, 0x24};
const uint8_t kMeshRel[] = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18};
const uint8_t kInitVFs[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10};
}  // namespace

void OnInit(uintptr_t res, uint32_t vt) {
    if (vt == kVFVtable) Add(res, "InitResource", false);
}
void OnRelease(uintptr_t res, uint32_t vt) {
    if (vt == kVFVtable) Add(res, "ReleaseResource", false);
}
void OnDtor(uintptr_t res, uint32_t vt) {
    if (vt != kVFVtable) return;
    std::lock_guard l(g_mu);
    g_hist.erase(res);
}

void PrintHistory(uintptr_t res) {
    std::vector<Event> h;
    {
        std::lock_guard l(g_mu);
        auto it = g_hist.find(res);
        if (it != g_hist.end()) h = it->second;
    }
    LOG("  history of %p (%zu events):", (void*)res, h.size());
    for (auto& e : h) LOG("    t=%8.3f %-24s thread=%lu %s", e.t, e.kind, e.tid, e.stack.c_str());
}

bool Install() {
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);
    bool ok = true;
    ok &= hooks::Create("GPUSkinVF::ctor(0x61C1A0)", 0x61C1A0, kCtor, sizeof kCtor, (void*)&hk_Ctor, (void**)&o_Ctor);
    ok &= hooks::Create("BeginInitResource", 0x411E10, kBeginInit, sizeof kBeginInit, (void*)&hk_BeginInit,
                        (void**)&o_BeginInit);
    ok &= hooks::Create("BeginReleaseResource", 0x412000, kBeginInit, sizeof kBeginInit, (void*)&hk_BeginRelease,
                        (void**)&o_BeginRelease);
    ok &= hooks::Create("SkelMeshObject::ReleaseResources", 0x95CF00, kMeshRel, sizeof kMeshRel,
                        (void*)&hk_MeshRelease, (void**)&o_MeshRelease);
    ok &= hooks::Create("InitVertexFactories(0x970480)", 0x970480, kInitVFs, sizeof kInitVFs, (void*)&hk_InitVFs,
                        (void**)&o_InitVFs);
    return ok;
}
}  // namespace ggdpvr::diag::vf
