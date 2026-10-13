#include "log.h"

#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <atomic>
#include <cctype>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include <share.h>

namespace ggdpvr::log {
namespace {
FILE* g_file = nullptr;
std::mutex g_mutex;
LARGE_INTEGER g_freq{}, g_start{};
// Logging is opt-in ([debug] debug=1; 0.9.102: one log, the full one). Until the config is read the lines are held in
// memory (g_early); then they are written out, or dropped and nothing is ever written.
enum State { kPending = 0, kOn = 1, kOff = 2 };
std::atomic<int> g_state{kPending};
std::vector<std::string> g_early;
std::string g_profile;      // the user profile folder (C:\Users\<name>): masked in every line
std::wstring g_forcedPath;  // GGDPVR_LOG (testing): always logs, to this file

// Privacy: a path under the user profile shows as %USERPROFILE%\... - no Windows user name in a shared log. (Device
// names / IDs are not logged at all: removed where they were logged.)
void Mask(char* msg, size_t cap) {
    if (g_profile.size() < 4) return;
    std::string s(msg), lo(s), pl(g_profile);
    for (auto& c : lo) c = char(tolower(static_cast<unsigned char>(c)));
    for (auto& c : pl) c = char(tolower(static_cast<unsigned char>(c)));
    bool changed = false;
    for (size_t at = lo.find(pl); at != std::string::npos; at = lo.find(pl, at + 13)) {
        s.replace(at, pl.size(), "%USERPROFILE%");
        lo.replace(at, pl.size(), "%userprofile%");
        changed = true;
    }
    if (changed) strncpy_s(msg, cap, s.c_str(), _TRUNCATE);
}
}  // namespace

bool Enabled() { return g_state.load() == kOn; }

std::wstring Directory() {
    wchar_t buf[MAX_PATH]{};
    GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring dir = std::wstring(buf) + L"\\GGDPVR";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

void Init() {
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_start);
    {
        char prof[MAX_PATH]{};
        if (GetEnvironmentVariableA("USERPROFILE", prof, MAX_PATH)) g_profile = prof;
    }
    wchar_t env[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"GGDPVR_LOG", env, MAX_PATH)) g_forcedPath = env;
    // nothing is opened yet: Configure() decides once the config is read
}

void Configure(bool enabled) {
    enabled = enabled || !g_forcedPath.empty();
    std::lock_guard lock(g_mutex);
    if (!enabled) {
        g_early.clear();
        g_early.shrink_to_fit();
        g_state = kOff;
        return;
    }
    std::wstring path = g_forcedPath;
    if (path.empty()) {
        std::wstring dir = Directory() + L"\\logs";
        CreateDirectoryW(dir.c_str(), nullptr);
        path = dir + L"\\ggdpvr.log";
        // keep the last 8 sessions (ggdpvr.prev.log, ggdpvr.prev2.log ... ggdpvr.prev7.log): with only one kept, a
        // reported problem's session was gone after two quick relaunches (0.9.15)
        auto prev = [&](int i) { return dir + (i == 1 ? std::wstring(L"\\ggdpvr.prev.log") : L"\\ggdpvr.prev" + std::to_wstring(i) + L".log"); };
        for (int i = 7; i >= 2; --i) MoveFileExW(prev(i - 1).c_str(), prev(i).c_str(), MOVEFILE_REPLACE_EXISTING);
        MoveFileExW(path.c_str(), prev(1).c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    g_file = _wfsopen(path.c_str(), L"w", _SH_DENYWR);  // readers allowed while the game runs
    for (const auto& l : g_early) {
        OutputDebugStringA(l.c_str());
        if (g_file) fputs(l.c_str(), g_file);
    }
    if (g_file) fflush(g_file);
    g_early.clear();
    g_early.shrink_to_fit();
    g_state = kOn;
}

void Write(const char* fmt, ...) {
    if (g_state.load() == kOff) return;
    char msg[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    Mask(msg, sizeof msg);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double t = double(now.QuadPart - g_start.QuadPart) / double(g_freq.QuadPart);
    char line[4200];
    snprintf(line, sizeof line, "[%9.3f][%5lu] %s\n", t, GetCurrentThreadId(), msg);
    std::lock_guard lock(g_mutex);
    const int st = g_state.load();
    if (st == kPending) {
        if (g_early.size() < 400) g_early.emplace_back(line);
        return;
    }
    if (st != kOn) return;
    OutputDebugStringA(line);
    if (g_file) {
        fputs(line, g_file);
        fflush(g_file);
    }
}
}  // namespace ggdpvr::log
