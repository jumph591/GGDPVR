#include "rr_probe.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "../core/config.h"
#include "../core/game.h"
#include "../core/hooks.h"
#include "../core/log.h"
#include "../ue3/objects.h"
#include "thread_stack.h"
#include "vf_trace.h"

namespace ggdpvr::diag::rr {
namespace {
using namespace ggdpvr::game;

// FRenderResource layout (x64, verified from InitResource/ReleaseResource/dtor):
//   +0x00 vtable, +0x08 TLinkedList node (3 qwords), +0x20 bit0 = bInitialized
constexpr uint32_t kFlags = 0x20;
// FSkeletalMeshObjectGPUSkin (hypothesis, verified by this probe):
//   +0x6C TArray<FSkeletalMeshObjectLOD> (elem 0x154), +0x7C owned polymorphic ptr
// FSkeletalMeshObjectLOD: +0x4C/+0x5C embedded resources (vtbl 0x2329850),
//   +0x84/+0xE4 vertex-factory-data blocks, +0x144/+0x14C owned polymorphic ptrs
// VF data block: TIndirectArray at +0x00/+0x10/+0x20/+0x30 (+0x40 sub-object)
constexpr uint32_t kLodArray = 0x6C, kLodStride = 0x154;
constexpr uint32_t kVfBlocks[2] = {0x84, 0xE4};

uint32_t g_gameThread = 0;
std::mutex g_mu;
struct Counter { uint64_t initGame = 0, initOther = 0, relGame = 0, relOther = 0; };
std::unordered_map<uint32_t, Counter> g_counters;  // key: vtable rva
uint64_t g_meshObjectsDestroyed = 0, g_meshObjectsWithLeak = 0;
// render-thread liveness: last thread that executed Init/ReleaseResource off the game thread
std::atomic<uint32_t> g_renderTid{0};
std::atomic<uint64_t> g_lastRenderInitQpc{0}, g_lastRenderReleaseQpc{0};
uint64_t Qpc() { LARGE_INTEGER n; QueryPerformanceCounter(&n); return uint64_t(n.QuadPart); }
double MsSince(uint64_t q) {
    if (!q) return -1.0;
    LARGE_INTEGER f; QueryPerformanceFrequency(&f);
    return double(Qpc() - q) * 1000.0 / double(f.QuadPart);
}

using FnThis = void(__fastcall*)(void*);
FnThis o_RRDtor = nullptr, o_Init = nullptr, o_Release = nullptr, o_MeshObjDtor = nullptr;

uint32_t VtblRva(uintptr_t obj) {
    uintptr_t vt = 0;
    return (Read(obj, vt) && InRData(vt)) ? ToRva(vt) : 0;
}
uint32_t SlotRva(uintptr_t obj, int slot) {
    uintptr_t vt = 0, fn = 0;
    if (!Read(obj, vt) || !InRData(vt) || !Read(vt + 8 * slot, fn)) return 0;
    return ToRva(fn);
}
bool IsInit(uintptr_t res) {
    uint32_t f = 0;
    return Read(res + kFlags, f) && (f & 1);
}

void __fastcall hk_Init(void* self) {
    const bool wasInit = IsInit(uintptr_t(self));
    o_Init(self);
    if (!wasInit) {
        const uint32_t vt = VtblRva(uintptr_t(self));
        vf::OnInit(uintptr_t(self), vt);
        std::lock_guard l(g_mu);
        auto& c = g_counters[vt];
        if (GetCurrentThreadId() == g_gameThread) ++c.initGame;
        else ++c.initOther, g_renderTid = GetCurrentThreadId(), g_lastRenderInitQpc = Qpc();
    }
}

void __fastcall hk_Release(void* self) {
    const bool wasInit = IsInit(uintptr_t(self));
    o_Release(self);
    if (wasInit) {
        const uint32_t vt = VtblRva(uintptr_t(self));
        vf::OnRelease(uintptr_t(self), vt);
        std::lock_guard l(g_mu);
        auto& c = g_counters[vt];
        if (GetCurrentThreadId() == g_gameThread) ++c.relGame;
        else ++c.relOther, g_renderTid = GetCurrentThreadId(), g_lastRenderReleaseQpc = Qpc();
    }
}

void DescribeOwnerFields(uintptr_t self) {
    // qwords of the owner header; name those that are UObjects (needs E3 bootstrap)
    for (uint32_t off = 0x8; off < kLodArray; off += 4) {
        uintptr_t v = 0;
        if (!Read(self + off, v) || !v) continue;
        std::string n = ue3::DescribeIfObject(v);
        if (!n.empty()) LOG("    owner+0x%02X -> %s", off, n.c_str());
    }
}

// Scans a mesh object before its destructor runs.
void ScanMeshObject(uintptr_t self) {
    struct { uintptr_t data; int32_t num, max; } lods{};
    if (!Read(self + kLodArray, lods) || lods.num < 0 || lods.num > 16) return;
    int leaks = 0;
    for (int l = 0; l < lods.num; ++l) {
        uintptr_t lod = lods.data + uintptr_t(l) * kLodStride;
        for (uint32_t emb : {0x4Cu, 0x5Cu})
            if (IsInit(lod + emb)) {
                ++leaks;
                LOG("  STILL-INIT lod=%d embedded+0x%X obj=%p vtbl=0x%X initRHI=0x%X", l, emb, (void*)(lod + emb),
                    VtblRva(lod + emb), SlotRva(lod + emb, 3));
            }
        for (int b = 0; b < 2; ++b) {
            uintptr_t blk = lod + kVfBlocks[b];
            for (int a = 0; a < 4; ++a) {
                struct { uintptr_t data; int32_t num, max; } arr{};
                if (!Read(blk + 0x10 * a, arr) || arr.num < 0 || arr.num > 256) continue;
                for (int i = 0; i < arr.num; ++i) {
                    uintptr_t res = 0;
                    if (!Read(arr.data + 8 * i, res) || !res) continue;
                    if (IsInit(res)) {
                        ++leaks;
                        LOG("  STILL-INIT lod=%d vfblock=+0x%X array=%d (@+0x%X) idx=%d/%d obj=%p vtbl=0x%X "
                            "initRHI=0x%X slot9=0x%X",
                            l, kVfBlocks[b], a, 0x10 * a, i, arr.num, (void*)res, VtblRva(res), SlotRva(res, 3),
                            SlotRva(res, 9));
                        vf::PrintHistory(res);
                    }
                }
            }
        }
        for (uint32_t p : {0x144u, 0x14Cu}) {
            uintptr_t obj = 0;
            if (Read(lod + p, obj) && obj && IsInit(obj))
                LOG("  (lod=%d ptr+0x%X obj=%p vtbl=0x%X has bit0 set at +0x20)", l, p, (void*)obj, VtblRva(obj));
        }
    }
    ++g_meshObjectsDestroyed;
    if (leaks) {
        ++g_meshObjectsWithLeak;
        LOG("MeshObject %p (vtbl 0x%X) destroyed with %d still-initialised resources; lods=%d; stack: %s",
            (void*)self, VtblRva(self), leaks, lods.num, StackRvas(1, 24).c_str());
        DescribeOwnerFields(self);
        uint32_t threaded = 0, rhiInit = 0;
        Read(Addr(0x325B9C4), threaded);  // GIsThreadedRendering
        Read(Addr(rva::GIsRHIInitialized), rhiInit);
        LOG("  GIsThreadedRendering=%u GIsRHIInitialized=%u renderThread=%u lastRenderInit=%.1fms ago "
            "lastRenderRelease=%.1fms ago",
            threaded, rhiInit, g_renderTid.load(), MsSince(g_lastRenderInitQpc), MsSince(g_lastRenderReleaseQpc));
        LOG("  render thread stack: %s", RemoteStack(g_renderTid.load(), 40).c_str());
        DumpCounters("mesh object leak");
    }
}

void __fastcall hk_MeshObjDtor(void* self) {
    if (VtblRva(uintptr_t(self)) == rva::SkelMeshObject_VTable) ScanMeshObject(uintptr_t(self));
    o_MeshObjDtor(self);
}

void __fastcall hk_RRDtor(void* self) {
    uint32_t critical = 0;
    Read(Addr(rva::GIsCriticalError), critical);
    if (IsInit(uintptr_t(self)) && !critical) {
        // The engine is about to raise its fatal assertion. We only record context.
        LOG("~FRenderResource on STILL-INITIALISED obj=%p vtbl(at dtor time)=0x%X thread=%lu(%s) stack: %s", self,
            VtblRva(uintptr_t(self)), GetCurrentThreadId(), GetCurrentThreadId() == g_gameThread ? "game" : "other",
            StackRvas(1, 32).c_str());
        DumpCounters("assert imminent");
    }
    else
        vf::OnDtor(uintptr_t(self), VtblRva(uintptr_t(self)));
    o_RRDtor(self);  // unchanged behaviour: appError fires if it would have
}

const uint8_t kDtorBytes[] = {0x48, 0x83, 0xEC, 0x28, 0xF6, 0x41, 0x20, 0x01};
const uint8_t kInitBytes[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x40, 0xF6, 0x41, 0x20, 0x01};
const uint8_t kRelBytes[] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x83, 0x3D};
const uint8_t kMeshDtorBytes[] = {0x48, 0x89, 0x4C, 0x24, 0x08, 0x53, 0x48, 0x83, 0xEC, 0x30};
}  // namespace

void SetGameThread(uint32_t tid) { g_gameThread = tid; }

void DumpCounters(const char* why) {
    std::vector<std::pair<uint32_t, Counter>> v;
    {
        std::lock_guard l(g_mu);
        v.assign(g_counters.begin(), g_counters.end());
    }
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) {
        auto live = [](const Counter& c) { return int64_t(c.initGame + c.initOther) - int64_t(c.relGame + c.relOther); };
        return live(a.second) > live(b.second);
    });
    LOG("render-resource counters (%s): meshObjectsDestroyed=%llu withLeak=%llu; top live by vtable:", why,
        g_meshObjectsDestroyed, g_meshObjectsWithLeak);
    int n = 0;
    for (auto& [vt, c] : v) {
        if (++n > 25) break;
        LOG("  vtbl 0x%07X init(game=%llu other=%llu) release(game=%llu other=%llu) live=%lld", vt, c.initGame,
            c.initOther, c.relGame, c.relOther,
            (long long)(c.initGame + c.initOther) - (long long)(c.relGame + c.relOther));
    }
}

bool Install() {
    const auto& cfg = config::Get();
    bool ok = true;
    if (cfg.rrProbe) {
        ok &= hooks::Create("FRenderResource::~FRenderResource", rva::FRenderResource_Dtor, kDtorBytes,
                            sizeof kDtorBytes, (void*)&hk_RRDtor, (void**)&o_RRDtor);
        ok &= vf::Install();
        ok &= hooks::Create("SkelMeshObject::~dtor", rva::SkelMeshObject_Dtor, kMeshDtorBytes, sizeof kMeshDtorBytes,
                            (void*)&hk_MeshObjDtor, (void**)&o_MeshObjDtor);
    }
    if (cfg.rrTraceLifecycle) {
        ok &= hooks::Create("FRenderResource::InitResource", rva::FRenderResource_InitResource, kInitBytes,
                            sizeof kInitBytes, (void*)&hk_Init, (void**)&o_Init);
        ok &= hooks::Create("FRenderResource::ReleaseResource", rva::FRenderResource_ReleaseResource, kRelBytes,
                            sizeof kRelBytes, (void*)&hk_Release, (void**)&o_Release);
    }
    return ok;
}
}  // namespace ggdpvr::diag::rr
