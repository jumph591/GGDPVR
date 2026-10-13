#include "settings_guard.h"

#include <windows.h>
#include <atomic>
#include <string>
#include <vector>

#include <MinHook.h>

#include "log.h"

namespace ggdpvr::settings_guard {
namespace {
// (0.9.97: + Fullscreen - the mod's windowed setres persisted Fullscreen=False, a fullscreen player was left windowed;
// + UseVsync - forced off in VR)
const char* const kKeys[] = {"MotionBlur", "MaxMultiSamples", "OneFrameThreadLag", "ResX", "ResY", "DepthOfField", "DynamicShadows",
                             "Fullscreen", "UseVsync"};
std::atomic<bool> g_dirty{false};
std::atomic<bool> g_exitRestored{false};
std::wstring g_live, g_baseline, g_marker;
bool g_ready = false;

std::wstring DocumentsDir() {
    wchar_t buf[MAX_PATH]{};
    DWORD size = sizeof buf;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders",
                     L"Personal", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
        return buf;
    GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
    return std::wstring(buf) + L"\\Documents";
}
std::wstring LocalDir() {
    wchar_t buf[MAX_PATH]{};
    GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::wstring d = std::wstring(buf) + L"\\GGDPVR";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

bool ReadAll(const std::wstring& p, std::string& out) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    out.resize(size_t(sz.QuadPart));
    DWORD rd = 0;
    const bool ok = ReadFile(h, out.data(), DWORD(out.size()), &rd, nullptr) && rd == out.size();
    CloseHandle(h);
    return ok;
}
bool WriteAll(const std::wstring& p, const std::string& data) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    const bool ok = WriteFile(h, data.data(), DWORD(data.size()), &wr, nullptr) && wr == data.size();
    CloseHandle(h);
    return ok;
}

// Locate "Key=value" (whole line) inside the first [SystemSettings] section of a single-byte INI.
bool FindKey(const std::string& s, const char* key, size_t& valBeg, size_t& valEnd) {
    size_t sec = s.find("[SystemSettings]");
    if (sec == std::string::npos || (sec != 0 && s[sec - 1] != '\n')) return false;
    size_t end = s.find("\n[", sec + 1);
    if (end == std::string::npos) end = s.size();
    const std::string pat = std::string(key) + "=";
    for (size_t pos = s.find('\n', sec); pos != std::string::npos && pos < end; pos = s.find('\n', pos + 1)) {
        if (s.compare(pos + 1, pat.size(), pat) == 0) {
            valBeg = pos + 1 + pat.size();
            valEnd = s.find_first_of("\r\n", valBeg);
            if (valEnd == std::string::npos) valEnd = s.size();
            return true;
        }
    }
    return false;
}

bool LooksSingleByte(const std::string& s) {
    return s.size() >= 2 && !(uint8_t(s[0]) == 0xFF && uint8_t(s[1]) == 0xFE) && !(uint8_t(s[0]) == 0xFE && uint8_t(s[1]) == 0xFF);
}

void SaveBaseline(const std::string& live) {
    std::string out = "; GGDPVR: the user's own values for settings GGDPVR overrides at runtime\r\n";
    for (const char* k : kKeys) {
        size_t b, e;
        if (FindKey(live, k, b, e)) out += std::string(k) + "=" + live.substr(b, e - b) + "\r\n";
    }
    WriteAll(g_baseline, out);
}

bool RestoreBaseline(const char* why) {
    std::string live, base;
    if (!ReadAll(g_live, live) || !ReadAll(g_baseline, base) || !LooksSingleByte(live)) return false;
    std::string out = live, changes;
    for (const char* k : kKeys) {
        size_t bb, be, lb, le;
        if (!FindKey("[SystemSettings]\n" + base, k, bb, be)) continue;
        const std::string want = ("[SystemSettings]\n" + base).substr(bb, be - bb);
        if (!FindKey(out, k, lb, le)) continue;
        if (out.compare(lb, le - lb, want) != 0) {
            changes += std::string(" ") + k + ":" + out.substr(lb, le - lb) + "->" + want;
            out.replace(lb, le - lb, want);
        }
    }
    if (out != live && !WriteAll(g_live, out)) return false;
    DeleteFileW(g_marker.c_str());
    g_dirty = false;
    // log may not be open yet during DllMain; OutputDebugString + deferred log
    std::string msg = std::string("[settings] restored user settings (") + why + "):" + (changes.empty() ? " none needed" : changes);
    OutputDebugStringA((msg + "\n").c_str());
    if (g_ready) LOGP("%s", msg.c_str());
    return true;
}
}  // namespace

void OnProcessAttach() {
    g_live = DocumentsDir() + L"\\My Games\\UnrealEngine3\\GG2Game\\Config\\GG2SystemSettings.ini";
    const std::wstring dir = LocalDir();
    g_baseline = dir + L"\\user_settings_baseline.ini";
    g_marker = dir + L"\\settings_dirty";
    std::string live;
    if (!ReadAll(g_live, live) || !LooksSingleByte(live)) return;  // nothing we can safely manage
    g_ready = true;
    if (GetFileAttributesW(g_marker.c_str()) != INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(g_baseline.c_str()) != INVALID_FILE_ATTRIBUTES) {
        g_ready = false;  // logging is not up yet
        RestoreBaseline("previous run did not exit cleanly");
        g_ready = true;
        // a baseline written before a key was guarded lacks it (0.9.97: Fullscreen, UseVsync - a test run's fullscreen
        // stayed): those keys are added from the restored file, so they're guarded from now on
        std::string base, now;
        if (ReadAll(g_baseline, base) && ReadAll(g_live, now)) {
            bool added = false;
            for (const char* k : kKeys) {
                size_t b0, e0;
                if (FindKey("[SystemSettings]\n" + base, k, b0, e0) || !FindKey(now, k, b0, e0)) continue;
                base += std::string(k) + "=" + now.substr(b0, e0 - b0) + "\r\n", added = true;
            }
            if (added) WriteAll(g_baseline, base);
        }
    } else {
        SaveBaseline(live);
    }
}

std::string BaselineValue(const char* key, const char* fallback) {
    std::string base;
    size_t b, e;
    if (g_ready && ReadAll(g_baseline, base) && FindKey("[SystemSettings]\n" + base, key, b, e))
        return ("[SystemSettings]\n" + base).substr(b, e - b);
    return fallback;
}

void MarkDirty() {
    if (!g_ready || g_dirty.exchange(true)) return;
    WriteAll(g_marker, "GGDPVR runtime overrides active\r\n");
    LOGP("[settings] runtime overrides active; your settings will be restored on exit");
}

void OnProcessDetach() {
    if (g_ready && g_dirty && !g_exitRestored.exchange(true)) RestoreBaseline("DLL detach");
}
namespace {
using TerminateProcessFn = BOOL(WINAPI*)(HANDLE, UINT);
using ExitProcessFn = void(WINAPI*)(UINT);
TerminateProcessFn o_TerminateProcess = nullptr;
ExitProcessFn o_ExitProcess = nullptr;
void RestoreOnExit(const char* how) {
    if (g_ready && g_dirty && !g_exitRestored.exchange(true)) RestoreBaseline(how);
}
BOOL WINAPI hk_TerminateProcess(HANDLE h, UINT code) {
    if (h == GetCurrentProcess() || GetProcessId(h) == GetCurrentProcessId()) RestoreOnExit("process exit (TerminateProcess)");
    return o_TerminateProcess(h, code);
}
void (*g_exitCallback)() = nullptr;
void WINAPI hk_ExitProcess(UINT code) {
    RestoreOnExit("process exit (ExitProcess)");
    if (g_exitCallback) g_exitCallback();
    o_ExitProcess(code);
}
}  // namespace
void SetExitCallback(void (*fn)()) { g_exitCallback = fn; }

bool InstallExitHooks() {
    bool ok = MH_CreateHookApi(L"kernel32", "TerminateProcess", (void*)&hk_TerminateProcess, (void**)&o_TerminateProcess) == MH_OK;
    ok &= MH_CreateHookApi(L"kernel32", "ExitProcess", (void*)&hk_ExitProcess, (void**)&o_ExitProcess) == MH_OK;
    ok &= MH_EnableHook(MH_ALL_HOOKS) == MH_OK;
    LOG("[settings] exit hooks %s", ok ? "installed" : "FAILED");
    return ok;
}
}  // namespace ggdpvr::settings_guard
