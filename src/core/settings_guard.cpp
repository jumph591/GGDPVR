#include "settings_guard.h"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
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

std::string g_note;      // lines for the log once it is open (attach runs before logging starts)
bool g_logOpen = false;  // set by InstallExitHooks, which runs after the log is configured
void Note(const std::string& s) {
    OutputDebugStringA((s + "\n").c_str());
    if (g_logOpen) LOGP("%s", s.c_str());
    else g_note += (g_note.empty() ? "" : "\n") + s;
}

// A resolution no display can show can't be the player's own: the game clamps its window to the desktop, and
// fullscreen uses a display mode. The mod's VR render size (e.g. 3640x3782) once became the "own" value (1.0.0: the
// startup logos then drew into a squeezed, near-square window).
bool PlausibleResolution(int w, int h) {
    if (w <= 0 || h <= 0) return true;  // not a number: leave it alone
    int maxW = 0, maxH = 0;
    bool exact = false;
    DISPLAY_DEVICEW dd{sizeof dd};
    for (DWORD d = 0; EnumDisplayDevicesW(nullptr, d, &dd, 0); ++d, dd = {sizeof dd}) {
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
        DEVMODEW dm{};
        dm.dmSize = sizeof dm;
        for (DWORD m = 0; EnumDisplaySettingsW(dd.DeviceName, m, &dm); ++m) {
            maxW = std::max(maxW, int(dm.dmPelsWidth)), maxH = std::max(maxH, int(dm.dmPelsHeight));
            if (int(dm.dmPelsWidth) == w && int(dm.dmPelsHeight) == h) exact = true;
        }
    }
    if (!maxW || !maxH || exact) return true;  // no mode list (some systems): can't judge
    // larger than every mode, or near-square / portrait without being a real mode (VR eye sizes are near-square)
    return w <= maxW && h <= maxH && h * 10 < w * 9;
}
bool ResOf(const std::string& ini, int& w, int& h) {
    size_t b, e;
    if (!FindKey(ini, "ResX", b, e)) return false;
    w = atoi(ini.substr(b, e - b).c_str());
    if (!FindKey(ini, "ResY", b, e)) return false;
    h = atoi(ini.substr(b, e - b).c_str());
    return true;
}
std::string SetValue(std::string ini, const char* key, const std::string& v) {
    size_t b, e;
    if (FindKey(ini, key, b, e)) ini.replace(b, e - b, v);
    return ini;
}

void SaveBaseline(const std::string& live) {
    std::string out = "; GGDPVR: the user's own values for settings GGDPVR overrides at runtime\r\n";
    std::string vals = "[SystemSettings]\n";
    for (const char* k : kKeys) {
        size_t b, e;
        if (FindKey(live, k, b, e)) vals += std::string(k) + "=" + live.substr(b, e - b) + "\r\n";
    }
    int w = 0, h = 0;
    if (ResOf(vals, w, h) && !PlausibleResolution(w, h)) {
        // not the player's: the previous baseline's resolution if that one is real, else the game's default
        std::string old;
        int ow = 0, oh = 0;
        const bool keepOld = ReadAll(g_baseline, old) && ResOf("[SystemSettings]\n" + old, ow, oh) && ow > 0 && oh > 0 &&
                             PlausibleResolution(ow, oh);
        const std::string nx = keepOld ? std::to_string(ow) : "1280", ny = keepOld ? std::to_string(oh) : "720";
        vals = SetValue(SetValue(vals, "ResX", nx), "ResY", ny);
        Note("[settings] the game's resolution " + std::to_string(w) + "x" + std::to_string(h) +
             " is no display's (a VR render size left behind): the player's resolution is taken as " + nx + "x" + ny);
    }
    out += vals.substr(17);  // without the "[SystemSettings]\n" used for the lookups
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
    // the log may not be open yet (DllMain): Note keeps the line until it is
    Note(std::string("[settings] restored user settings (") + why + "):" + (changes.empty() ? " none needed" : changes));
    return true;
}
}  // namespace

namespace {
// Takes charge of the settings file: at DLL attach, or - when the file doesn't exist yet then (the first launch, or
// the game regenerating its config) - at the first MarkDirty, before any override. 1.0.0 gave up for the whole
// session instead: the overrides persisted unguarded and became the next start's baseline.
void Start() {
    std::string live;
    if (!ReadAll(g_live, live) || !LooksSingleByte(live)) return;  // nothing we can safely manage (yet)
    g_ready = true;
    {  // a baseline holding a resolution no display has (a VR render size, 1.0.0) is repaired before anything uses it
        std::string base;
        int w = 0, h = 0;
        if (ReadAll(g_baseline, base) && ResOf("[SystemSettings]\n" + base, w, h) && !PlausibleResolution(w, h)) {
            std::string fixed = SetValue(SetValue("[SystemSettings]\n" + base, "ResX", "1280"), "ResY", "720");
            WriteAll(g_baseline, fixed.substr(17));
            Note("[settings] the saved resolution " + std::to_string(w) + "x" + std::to_string(h) +
                 " is no display's (a VR render size): repaired to the game's default 1280x720");
        }
    }
    if (GetFileAttributesW(g_marker.c_str()) != INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(g_baseline.c_str()) != INVALID_FILE_ATTRIBUTES) {
        RestoreBaseline("previous run did not exit cleanly");
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
    // the file itself holding such a resolution (no dirty marker left to say so): the baseline's goes back in now,
    // before the engine reads it - the startup logos drew into a squeezed near-square window otherwise
    std::string now;
    int w = 0, h = 0;
    if (ReadAll(g_live, now) && ResOf(now, w, h) && !PlausibleResolution(w, h)) RestoreBaseline("resolution no display has");
}
}  // namespace

void OnProcessAttach() {
    g_live = DocumentsDir() + L"\\My Games\\UnrealEngine3\\GG2Game\\Config\\GG2SystemSettings.ini";
    const std::wstring dir = LocalDir();
    g_baseline = dir + L"\\user_settings_baseline.ini";
    g_marker = dir + L"\\settings_dirty";
    Start();
}

std::string BaselineValue(const char* key, const char* fallback) {
    std::string base;
    size_t b = 0, e = 0;
    if (g_ready && ReadAll(g_baseline, base) && FindKey("[SystemSettings]\n" + base, key, b, e))
        return ("[SystemSettings]\n" + base).substr(b, e - b);
    return fallback;
}

void MarkDirty() {
    if (!g_ready && !g_live.empty()) {  // the file didn't exist at attach: the engine has written it by now
        Start();
        LOGP("[settings] GG2SystemSettings.ini %s", g_ready ? "appeared after start: guarding it from now" : "still unreadable: NOT guarded");
    }
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
    // the log is configured by now: the lines from DLL attach
    g_logOpen = true;
    for (size_t a = 0; a < g_note.size();) {
        size_t b = g_note.find('\n', a);
        if (b == std::string::npos) b = g_note.size();
        LOGP("%s", g_note.substr(a, b - a).c_str());
        a = b + 1;
    }
    g_note.clear();
    if (!g_ready) LOGP("[settings] GG2SystemSettings.ini not found yet (first launch?): guarded once the game writes it");
    return ok;
}
}  // namespace ggdpvr::settings_guard
