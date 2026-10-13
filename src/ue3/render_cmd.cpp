#include "render_cmd.h"

#include <windows.h>
#include <cstdint>
#include <cstring>

#include "../core/game.h"
#include "../core/log.h"

namespace ggdpvr::ue3::render {
namespace {
using namespace ggdpvr::game;
constexpr uint32_t kRingAlloc = 0x150440;      // FRingBuffer::AllocationContext::AllocationContext(ring, size)
constexpr uint32_t kRingCommit = 0x14F0F0;     // FRingBuffer::AllocationContext::Commit
constexpr uint32_t kRenderCommandBuffer = 0x325BAC0;
constexpr uint32_t kGIsThreadedRendering = 0x325B9C4;
constexpr uint32_t kSkipCommandVtbl = 0x23297B8;  // FSkipRenderCommand (vtable, UINT NumSkipBytes @ +8)

struct AllocCtx {
    void* ring;
    uint8_t* start;
    uint8_t* end;
};
using AllocFn = AllocCtx*(__fastcall*)(AllocCtx*, void* ring, uint32_t size);
using CommitFn = void(__fastcall*)(AllocCtx*);

// Command object layout (the engine's FRenderCommand: vtable + data; Execute returns sizeof(command))
struct Command {
    const void* const* vtbl;
    Fn fn;
    uint32_t totalSize;
    uint32_t payloadSize;
    alignas(16) uint8_t payload[kMaxPayload];
};

void* __fastcall CmdDtor(Command* self, unsigned) { return self; }
uint32_t __fastcall CmdExecute(Command* self) {
    self->fn(self->payload);
    return self->totalSize;
}
const wchar_t* __fastcall CmdDescribe(Command*) { return L"GGDPVRRenderCommand"; }
const void* const kVtbl[] = {reinterpret_cast<const void*>(&CmdDtor), reinterpret_cast<const void*>(&CmdExecute),
                             reinterpret_cast<const void*>(&CmdDescribe)};

uint32_t AlignUp(uint32_t v, uint32_t a) { return (v + a - 1) & ~(a - 1); }
}  // namespace

bool Enqueue(Fn fn, const void* payload, size_t size) {
    if (size > kMaxPayload) return false;
    uint32_t threaded = 0;
    Read(Addr(kGIsThreadedRendering), threaded);
    if (!threaded) {
        fn(payload);
        return true;
    }
    const uint32_t need = AlignUp(uint32_t(offsetof(Command, payload) + size), 16);
    auto alloc = reinterpret_cast<AllocFn>(Addr(kRingAlloc));
    auto commit = reinterpret_cast<CommitFn>(Addr(kRingCommit));
    void* ring = reinterpret_cast<void*>(Addr(kRenderCommandBuffer));
    AllocCtx ctx{};
    alloc(&ctx, ring, need);
    uint32_t got = uint32_t(ctx.end - ctx.start);
    if (got < need) {  // ring wrapped: fill the tail with a skip command, then allocate again
        *reinterpret_cast<uintptr_t*>(ctx.start) = Addr(kSkipCommandVtbl);
        *reinterpret_cast<uint32_t*>(ctx.start + 8) = got;
        commit(&ctx);
        ctx = {};
        alloc(&ctx, ring, need);
        got = uint32_t(ctx.end - ctx.start);
        if (got < need) {
            LOG("[render] ring allocation failed (%u < %u)", got, need);
            return false;
        }
    }
    auto* cmd = reinterpret_cast<Command*>(ctx.start);
    cmd->vtbl = kVtbl;
    cmd->fn = fn;
    cmd->totalSize = need;
    cmd->payloadSize = uint32_t(size);
    memcpy(cmd->payload, payload, size);
    commit(&ctx);
    return true;
}
}  // namespace ggdpvr::ue3::render
