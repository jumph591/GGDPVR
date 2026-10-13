#include "thread_stack.h"

#include <windows.h>
#include <cstdio>

#include "../core/game.h"

namespace ggdpvr::diag {
std::string RemoteStack(uint32_t tid, int maxFrames) {
    if (!tid || tid == GetCurrentThreadId()) return "<invalid thread>";
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!h) return "<OpenThread failed>";
    std::string out;
    if (SuspendThread(h) != DWORD(-1)) {
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(h, &ctx)) {
            char buf[64];
            for (int i = 0; i < maxFrames && ctx.Rip; ++i) {
                uint32_t r = game::ToRva(ctx.Rip);
                if (r) snprintf(buf, sizeof buf, "%X ", r);
                else snprintf(buf, sizeof buf, "?%llX ", (unsigned long long)ctx.Rip);
                out += buf;
                DWORD64 imageBase = 0;
                PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
                if (!fn) {  // leaf function: return address is at [rsp]
                    ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
                    ctx.Rsp += 8;
                    continue;
                }
                PVOID handlerData = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
            }
        } else {
            out = "<GetThreadContext failed>";
        }
        ResumeThread(h);
    }
    CloseHandle(h);
    return out;
}
}  // namespace ggdpvr::diag
