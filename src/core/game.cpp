#include "game.h"

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstring>

#include "log.h"

namespace ggdpvr::game {
namespace {
uintptr_t g_base = 0;
uintptr_t g_size = 0;
uintptr_t g_textBeg = 0, g_textEnd = 0, g_rdataBeg = 0, g_rdataEnd = 0, g_dataBeg = 0, g_dataEnd = 0;
constexpr uint32_t kTimeDateStamp = 0x57FC3FE2;
constexpr uint32_t kSizeOfImage = 0x352D000;
}  // namespace

bool Init() {
    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
    g_size = nt->OptionalHeader.SizeOfImage;
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        char name[9]{};
        memcpy(name, sec->Name, 8);
        uintptr_t b = g_base + sec->VirtualAddress, e = b + sec->Misc.VirtualSize;
        if (!strcmp(name, ".text")) g_textBeg = b, g_textEnd = e;
        if (!strcmp(name, ".rdata")) g_rdataBeg = b, g_rdataEnd = e;
        if (!strcmp(name, ".data")) g_dataBeg = b, g_dataEnd = e;
    }
    LOG("GG2Game.exe base=%p TimeDateStamp=0x%08X SizeOfImage=0x%X", (void*)g_base,
        nt->FileHeader.TimeDateStamp, (unsigned)g_size);
    if (nt->FileHeader.TimeDateStamp != kTimeDateStamp || g_size != kSizeOfImage) {
        LOG("UNSUPPORTED GG2Game.exe build (expected TimeDateStamp 0x%08X, SizeOfImage 0x%X). Hooks disabled.",
            kTimeDateStamp, kSizeOfImage);
        return false;
    }
    return true;
}

uintptr_t Base() { return g_base; }
uint32_t ToRva(uintptr_t a) { return (a >= g_base && a < g_base + g_size) ? uint32_t(a - g_base) : 0; }
bool InImage(uintptr_t a) { return a >= g_base && a < g_base + g_size; }
bool InRData(uintptr_t a) { return a >= g_rdataBeg && a < g_rdataEnd; }
bool InText(uintptr_t a) { return a >= g_textBeg && a < g_textEnd; }
void DataRange(uintptr_t& beg, uintptr_t& end) { beg = g_dataBeg, end = g_dataEnd; }

bool VerifyBytes(uint32_t rva, const uint8_t* expected, size_t n) {
    if (!memcmp(reinterpret_cast<const void*>(Addr(rva)), expected, n)) return true;
    LOG("VerifyBytes FAILED at rva 0x%X", rva);
    return false;
}

bool SafeRead(const void* p, void* out, size_t n) {
    __try {
        memcpy(out, p, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string StackRvas(int skip, int max) {
    void* frames[64]{};
    USHORT n = RtlCaptureStackBackTrace(skip + 1, max < 64 ? max : 64, frames, nullptr);
    std::string s;
    char buf[64];
    for (USHORT i = 0; i < n; ++i) {
        uintptr_t a = reinterpret_cast<uintptr_t>(frames[i]);
        if (uint32_t r = ToRva(a)) snprintf(buf, sizeof buf, "%X ", r);
        else snprintf(buf, sizeof buf, "?%p ", frames[i]);
        s += buf;
    }
    return s;
}

uint32_t MainThreadId() {
    static uint32_t cached = 0;
    if (cached) return cached;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te{sizeof te};
    ULONGLONG best = ~0ull;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        FILETIME c, e, k, u;
        if (GetThreadTimes(h, &c, &e, &k, &u)) {
            ULONGLONG t = (ULONGLONG(c.dwHighDateTime) << 32) | c.dwLowDateTime;
            if (t < best) best = t, cached = te.th32ThreadID;
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    return cached;
}

std::wstring ModDir() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ModDir), &self);
    wchar_t p[MAX_PATH]{};
    GetModuleFileNameW(self, p, MAX_PATH);
    std::wstring s(p);
    return s.substr(0, s.find_last_of(L"\\/"));
}
}  // namespace ggdpvr::game
