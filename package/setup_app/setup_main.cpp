// GGDPVR Setup - installs, updates, repairs, switches off / on and uninstalls GGDPVR, the VR mod for
// Gal*Gun: Double Peace. One program: the mod's files are inside it (resources, setup.rc).
// It changes only the game's Binaries\Win64 folder - plus, on uninstall, the graphics settings the mod changes during
// VR if a VR session crashed before putting them back (restored byte for byte), and the mod's own %LOCALAPPDATA%\GGDPVR
// folder. Nothing is downloaded, nothing goes to the registry.
//   GGDPVR-Setup.exe                                                        the window
//   GGDPVR-Setup.exe --op install|uninstall [--game-dir "<folder>"] [--yes]  no window (--yes: default answers,
//                                                                           no messages); exit code 0 = done
#include <windows.h>
#include <objidl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

#include "resource_ids.h"

namespace {
#define WIDEN2(x) L##x
#define WIDEN(x) WIDEN2(x)
const std::wstring kVersion = WIDEN(GGDPVR_SETUP_VERSION);
const wchar_t* const kTitle = L"GGDPVR Setup";
const wchar_t* const kForeignBackup = L"dinput8.dll.before-ggdpvr";  // another mod's dinput8.dll, while ours is in
bool g_yes = false;  // --yes: every question gets its default answer, no messages

// ---- files ---------------------------------------------------------------------------------------------------
std::wstring Join(const std::wstring& a, const std::wstring& b) {
    return a.empty() || a.back() == L'\\' ? a + b : a + L"\\" + b;
}
std::wstring Parent(const std::wstring& p) {
    const size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? L"" : p.substr(0, i);
}
bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
bool ReadAll(const std::wstring& p, std::string& out) {
    HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    out.resize(size_t(sz.QuadPart));
    DWORD rd = 0;
    const bool ok = out.empty() || (ReadFile(h, out.data(), DWORD(out.size()), &rd, nullptr) && rd == out.size());
    CloseHandle(h);
    return ok;
}
DWORD WriteAll(const std::wstring& p, const std::string& data) {  // 0 or the Windows error
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD wr = 0;
    const bool ok = data.empty() || (WriteFile(h, data.data(), DWORD(data.size()), &wr, nullptr) && wr == data.size());
    const DWORD err = ok ? 0 : GetLastError();
    CloseHandle(h);
    return ok ? 0 : (err ? err : ERROR_WRITE_FAULT);
}
std::string Res(int id) {
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));  // RT_RCDATA
    HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
    const void* p = g ? LockResource(g) : nullptr;
    return p ? std::string(static_cast<const char*>(p), SizeofResource(nullptr, r)) : std::string();
}
std::wstring Env(const wchar_t* name) {
    wchar_t buf[MAX_PATH]{};
    GetEnvironmentVariableW(name, buf, MAX_PATH);
    return buf;
}
std::wstring LocalDir() { return Env(L"LOCALAPPDATA") + L"\\GGDPVR"; }
std::wstring Widen(const std::string& s) {
    std::wstring w(s.size(), L'\0');
    w.resize(size_t(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size()))));
    return w;
}
std::string Narrow(const std::wstring& w) {
    std::string s(w.size() * 3, '\0');
    s.resize(size_t(WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), int(s.size()), nullptr, nullptr)));
    return s;
}
void DeleteTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(Join(dir, L"*").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring n = fd.cFileName;
            if (n == L"." || n == L"..") continue;
            const std::wstring p = Join(dir, n);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) DeleteTree(p);
            else SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL), DeleteFileW(p.c_str());
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    RemoveDirectoryW(dir.c_str());
}

// ---- messages ------------------------------------------------------------------------------------------------
bool AskYesNo(HWND w, const std::wstring& text, bool def) {
    if (g_yes) return def;
    return MessageBoxW(w, text.c_str(), kTitle, MB_YESNO | MB_ICONQUESTION | (def ? MB_DEFBUTTON1 : MB_DEFBUTTON2)) == IDYES;
}
void Tell(HWND w, const std::wstring& text, UINT icon = MB_ICONINFORMATION) {
    if (!g_yes) MessageBoxW(w, text.c_str(), kTitle, MB_OK | icon);
}

// ---- finding the game ----------------------------------------------------------------------------------------
bool IsGameDir(const std::wstring& d) { return !d.empty() && Exists(Join(d, L"Binaries\\Win64\\GG2Game.exe")); }
// the folder's name as it is on disk (Steam keeps its path in lower case)
std::wstring RealCase(const std::wstring& d) {
    HANDLE h = CreateFileW(d.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return d;
    wchar_t buf[1024]{};
    const DWORD n = GetFinalPathNameByHandleW(h, buf, 1024, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    if (!n || n >= 1024) return d;
    const std::wstring r = buf;
    if (r.rfind(L"\\\\?\\UNC\\", 0) == 0) return L"\\\\" + r.substr(8);
    if (r.rfind(L"\\\\?\\", 0) == 0) return r.substr(4);
    return r;
}
// the game's main folder from what the player picked: that folder, Binaries or Binaries\Win64 (or one below)
std::wstring NormalizeGameDir(std::wstring d) {
    while (!d.empty() && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
    for (int up = 0; up < 4 && !d.empty(); ++up, d = Parent(d))
        if (IsGameDir(d)) return RealCase(d);
    return L"";
}
std::wstring RegStr(HKEY root, const wchar_t* key, const wchar_t* value) {
    wchar_t buf[1024]{};
    DWORD size = sizeof buf;
    return RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS ? buf : L"";
}
// the quoted strings of a Steam .vdf / .acf file after "key"
std::vector<std::string> VdfValues(const std::string& s, const std::string& key) {
    std::vector<std::string> out;
    const std::string k = "\"" + key + "\"";
    for (size_t p = s.find(k); p != std::string::npos; p = s.find(k, p + 1)) {
        size_t a = s.find('"', p + k.size());
        if (a == std::string::npos) break;
        std::string v;
        for (size_t i = a + 1; i < s.size() && s[i] != '"'; ++i) {
            if (s[i] == '\\' && i + 1 < s.size()) ++i;  // "D:\\SteamLibrary"
            v += s[i];
        }
        out.push_back(v);
    }
    return out;
}
std::vector<std::wstring> SteamLibraries() {
    std::wstring steam = RegStr(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (steam.empty()) steam = RegStr(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath");
    if (steam.empty()) steam = RegStr(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath");
    std::vector<std::wstring> libs;
    if (steam.empty()) return libs;
    std::replace(steam.begin(), steam.end(), L'/', L'\\');
    libs.push_back(steam);
    std::string vdf;
    if (ReadAll(Join(steam, L"steamapps\\libraryfolders.vdf"), vdf))
        for (const std::string& p : VdfValues(vdf, "path")) libs.push_back(Widen(p));
    return libs;
}
std::wstring FindSteamGame() {
    for (const std::wstring& lib : SteamLibraries()) {
        std::wstring dir = L"GalGun Double Peace";
        std::string acf;
        if (ReadAll(Join(lib, L"steamapps\\appmanifest_511740.acf"), acf)) {
            const auto v = VdfValues(acf, "installdir");
            if (!v.empty()) dir = Widen(v[0]);
        }
        const std::wstring cand = Join(lib, L"steamapps\\common\\" + dir);
        if (IsGameDir(cand)) return RealCase(cand);
    }
    return L"";
}
bool IsSteamCopy(const std::wstring& game) {
    for (const std::wstring& lib : SteamLibraries()) {
        const std::wstring common = Join(lib, L"steamapps\\common\\");
        if (game.size() > common.size() && !_wcsnicmp(game.c_str(), common.c_str(), common.size())) return true;
    }
    return false;
}
std::wstring RememberFile() { return Join(LocalDir(), L"setup_game_folder.txt"); }
void Remember(const std::wstring& game) {
    CreateDirectoryW(LocalDir().c_str(), nullptr);
    WriteAll(RememberFile(), Narrow(game));
}
std::wstring DetectGame() {
    std::string r;
    if (ReadAll(RememberFile(), r) && IsGameDir(Widen(r))) return RealCase(Widen(r));  // the folder chosen last time
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::wstring here = NormalizeGameDir(Parent(exe));  // this program is in the game folder
    if (!here.empty()) return here;
    return FindSteamGame();
}

// ---- the installed state -------------------------------------------------------------------------------------
enum Part { kMissing, kSame, kOther, kForeign };
struct State {
    bool game = false;
    Part loader = kMissing, mod = kMissing;
    bool ini = false, disabled = false, foreignBackup = false, logging = false;
    bool Ours(Part p) const { return p == kSame || p == kOther; }
    bool Any() const { return Ours(loader) || mod != kMissing || ini; }
    bool Complete() const { return Ours(loader) && mod != kMissing && ini; }
    bool Current() const { return loader == kSame && mod == kSame && ini; }
};
bool IsOurLoader(const std::string& bytes) {
    for (const wchar_t* m : {L"GGDPVR loader"}) {  // text inside our dinput8.dll
        const std::string needle(reinterpret_cast<const char*>(m), wcslen(m) * sizeof(wchar_t));
        if (bytes.find(needle) != std::string::npos) return true;
    }
    return false;
}
// The mod's log switch, log= in the player's ggdpvr.ini: looked up like the mod does (src\core\config.cpp Sec: the
// first of [advanced], [vr], [debug] that has the key; on when it is missing - the default since 1.0.1).
bool LoggingOn(const std::wstring& ini) {
    for (const wchar_t* sec : {L"advanced", L"vr", L"debug"}) {
        wchar_t buf[8]{};
        GetPrivateProfileStringW(sec, L"log", L"\x01", buf, 8, ini.c_str());
        if (buf[0] != 1) return GetPrivateProfileIntW(sec, L"log", 1, ini.c_str()) != 0;
    }
    return true;
}
// Sets log= in those sections. Only the value changes - the rest of the file stays byte for byte (no BOM or line
// ending changes); a file without the key gets "log=" under [vr]. 0 or the Windows error.
DWORD SetLogging(const std::wstring& ini, bool on) {
    std::string s;
    if (!ReadAll(ini, s)) return GetLastError() ? GetLastError() : ERROR_READ_FAULT;
    const char v = on ? '1' : '0';
    const auto blank = [&](size_t i) { return s[i] == ' ' || s[i] == '\t'; };
    std::string sec;
    size_t vrEnd = std::string::npos;  // just after the [vr] header line
    bool found = false;
    std::vector<std::pair<size_t, size_t>> values;
    for (size_t a = 0; a < s.size();) {
        size_t b = s.find('\n', a);
        if (b == std::string::npos) b = s.size();
        size_t e = b;
        if (e > a && s[e - 1] == '\r') --e;
        size_t k = a;
        while (k < e && blank(k)) ++k;
        if (k < e && s[k] == '[') {
            const size_t c = s.find(']', k);
            sec = c != std::string::npos && c < e ? s.substr(k + 1, c - k - 1) : "";
            for (auto& ch : sec) ch = char(tolower(static_cast<unsigned char>(ch)));
            if (sec == "vr") vrEnd = b < s.size() ? b + 1 : b;
        } else if (k < e && s[k] != ';' && s[k] != '#' && (sec == "advanced" || sec == "vr" || sec == "debug")) {
            const size_t eq = s.find('=', k);
            if (eq != std::string::npos && eq < e) {
                size_t ke = eq;
                while (ke > k && blank(ke - 1)) --ke;
                if (ke - k == 3 && _strnicmp(s.c_str() + k, "log", 3) == 0) {
                    size_t vb = eq + 1, ve = e;
                    while (vb < e && blank(vb)) ++vb;
                    while (ve > vb && blank(ve - 1)) --ve;
                    values.emplace_back(vb, ve);
                    found = true;
                }
            }
        }
        a = b + 1;
    }
    std::string out = s;
    for (auto it = values.rbegin(); it != values.rend(); ++it) out.replace(it->first, it->second - it->first, 1, v);
    if (!found) {
        // the file's own line ending; Windows' when it has none yet
        const std::string nl = s.find('\n') != std::string::npos && s.find("\r\n") == std::string::npos ? "\n" : "\r\n";
        const std::string line = std::string("log=") + v + nl;
        if (vrEnd != std::string::npos) {
            if (vrEnd == out.size() && !out.empty() && out.back() != '\n') out += nl, vrEnd = out.size();
            out.insert(vrEnd, line);
        } else {
            if (!out.empty() && out.back() != '\n') out += nl;
            out += "[vr]" + nl + line;
        }
    }
    return out == s ? 0 : WriteAll(ini, out);
}
State Inspect(const std::wstring& game) {
    State s;
    s.game = IsGameDir(game);
    if (!s.game) return s;
    const std::wstring win = Join(game, L"Binaries\\Win64"), mod = Join(win, L"GGDPVR");
    std::string b;
    if (ReadAll(Join(win, L"dinput8.dll"), b)) s.loader = !IsOurLoader(b) ? kForeign : b == Res(IDR_LOADER) ? kSame : kOther;
    if (ReadAll(Join(mod, L"GGDPVR.dll"), b)) s.mod = b == Res(IDR_MOD) ? kSame : kOther;
    s.ini = Exists(Join(mod, L"ggdpvr.ini"));
    s.logging = s.ini && LoggingOn(Join(mod, L"ggdpvr.ini"));
    s.disabled = Exists(Join(mod, L"disabled"));
    s.foreignBackup = Exists(Join(win, kForeignBackup));
    return s;
}

std::vector<DWORD> GamePids() {
    std::vector<DWORD> pids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return pids;
    PROCESSENTRY32W pe{sizeof pe};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (!_wcsicmp(pe.szExeFile, L"GG2Game.exe")) pids.push_back(pe.th32ProcessID);
    CloseHandle(snap);
    return pids;
}
bool CloseGameIfRunning(HWND w) {
    const auto pids = GamePids();
    if (pids.empty()) return true;
    if (!AskYesNo(w, L"Gal*Gun: Double Peace is running.\n\nClose it now? Unsaved progress is lost.", false)) return false;
    for (DWORD pid : pids) {
        if (HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid)) {
            TerminateProcess(h, 1);
            WaitForSingleObject(h, 5000);
            CloseHandle(h);
        }
    }
    Sleep(500);
    return GamePids().empty();
}

// ---- install / uninstall -------------------------------------------------------------------------------------
struct Outcome {
    int code = 0;  // 0 done, 1 not done, 2 Windows refused (administrator rights needed)
    std::wstring text;
};
Outcome Failed(DWORD err, const std::wstring& path) {
    if (err == ERROR_ACCESS_DENIED) return {2, L"Windows didn't allow changes in the game folder."};
    if (err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION)
        return {1, L"A file is in use (" + path + L"). Close the game, then try again."};
    return {1, L"Could not change " + path + L" (Windows error " + std::to_wstring(err) + L")."};
}

Outcome Install(HWND w, const std::wstring& game) {
    const std::wstring win = Join(game, L"Binaries\\Win64"), mod = Join(win, L"GGDPVR");
    const std::string loaderRes = Res(IDR_LOADER), modRes = Res(IDR_MOD), iniRes = Res(IDR_INI);
    if (loaderRes.empty() || modRes.empty() || iniRes.empty())
        return {1, L"This setup program is damaged (the mod's files are missing). Please download it again."};
    if (!CloseGameIfRunning(w)) return {1, L"Close the game, then try again."};
    const std::wstring loader = Join(win, L"dinput8.dll");
    std::string cur;
    if (ReadAll(loader, cur) && !IsOurLoader(cur)) {
        if (!AskYesNo(w, L"The game folder already has a dinput8.dll from another mod or tool. GGDPVR needs that file "
                         L"name for its loader.\n\nKeep the other one as " + std::wstring(kForeignBackup) +
                         L" and install GGDPVR's?\n\nThe other mod won't load while GGDPVR is installed. Uninstalling "
                         L"GGDPVR puts it back.", false))
            return {1, L"Not installed - nothing was changed."};
        std::wstring bk = Join(win, kForeignBackup);
        for (int i = 2; Exists(bk); ++i) bk = Join(win, std::wstring(kForeignBackup) + L"-" + std::to_wstring(i));
        if (!MoveFileW(loader.c_str(), bk.c_str())) return Failed(GetLastError(), loader);
    }
    if (!CreateDirectoryW(mod.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return Failed(GetLastError(), mod);
    if (DWORD e = WriteAll(loader, loaderRes)) return Failed(e, loader);
    if (DWORD e = WriteAll(Join(mod, L"GGDPVR.dll"), modRes)) return Failed(e, Join(mod, L"GGDPVR.dll"));
    const std::wstring ini = Join(mod, L"ggdpvr.ini");
    const bool keptIni = Exists(ini);
    if (!keptIni)
        if (DWORD e = WriteAll(ini, iniRes)) return Failed(e, ini);
    for (const wchar_t* old : {L"ggdpvr_debug.ini", L"GGDPVR.pdb"}) DeleteFileW(Join(mod, old).c_str());  // test builds
    const std::wstring off = Join(mod, L"disabled");
    if (Exists(off) && AskYesNo(w, L"GGDPVR is switched off at the moment. Switch it back on?", true)) DeleteFileW(off.c_str());
    if (!Inspect(game).Current()) return {1, L"Something didn't install correctly. Please try again, or install by hand (README.txt)."};
    Remember(game);
    return {0, L"Installed version " + kVersion + (keptIni ? L" - your settings were kept." : L".") +
                   L" Start your VR software first, then the game."};
}

// While VR runs, the mod changes a few graphics settings in the game's GG2SystemSettings.ini and puts the player's own
// values back when the game closes; after a crash at the next start. Uninstalling puts them back instead (the mod's
// own code, src\core\settings_guard.cpp: same file, same keys, byte for byte). false = they could not be put back.
bool LooksSingleByte(const std::string& s) {
    return s.size() >= 2 && !(uint8_t(s[0]) == 0xFF && uint8_t(s[1]) == 0xFE) && !(uint8_t(s[0]) == 0xFE && uint8_t(s[1]) == 0xFF);
}
bool FindKey(const std::string& s, const std::string& key, size_t& valBeg, size_t& valEnd) {
    const size_t sec = s.find("[SystemSettings]");
    if (sec == std::string::npos || (sec != 0 && s[sec - 1] != '\n')) return false;
    size_t end = s.find("\n[", sec + 1);
    if (end == std::string::npos) end = s.size();
    const std::string pat = key + "=";
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
std::wstring DocumentsDir() {
    std::wstring d = RegStr(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Shell Folders", L"Personal");
    return d.empty() ? Env(L"USERPROFILE") + L"\\Documents" : d;
}
// A resolution no display can show can't be the player's own (the mod's settings_guard.cpp has the same test): 1.0.0
// could leave the VR render size (e.g. 3640x3782) as the game's resolution.
bool PlausibleResolution(int w, int h) {
    if (w <= 0 || h <= 0) return true;
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
    if (!maxW || !maxH || exact) return true;
    return w <= maxW && h <= maxH && h * 10 < w * 9;
}
bool RestoreGraphicsSettings(bool& changed) {
    changed = false;
    const std::wstring marker = Join(LocalDir(), L"settings_dirty"), baseline = Join(LocalDir(), L"user_settings_baseline.ini");
    const bool dirty = Exists(marker) && Exists(baseline);  // false: the last VR session ended normally
    const std::wstring live = DocumentsDir() + L"\\My Games\\UnrealEngine3\\GG2Game\\Config\\GG2SystemSettings.ini";
    std::string text, base;
    if (!ReadAll(live, text)) {
        if (dirty) DeleteFileW(marker.c_str());
        return true;
    }
    if (!LooksSingleByte(text)) return !dirty;
    if (dirty && !ReadAll(baseline, base)) return false;
    std::string out = text;
    for (size_t a = 0; dirty && a < base.size();) {
        size_t b = base.find('\n', a);
        if (b == std::string::npos) b = base.size();
        std::string line = base.substr(a, b - a);
        a = b + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (line.empty() || line[0] == ';' || eq == std::string::npos || eq == 0) continue;
        const std::string key = line.substr(0, eq), want = line.substr(eq + 1);
        size_t vb, ve;
        if (FindKey(out, key, vb, ve) && out.compare(vb, ve - vb, want) != 0) out.replace(vb, ve - vb, want);
    }
    {  // a resolution no display has: the game's default instead, so the game without the mod starts normally
        size_t xb = 0, xe = 0, yb = 0, ye = 0;
        if (FindKey(out, "ResX", xb, xe) && FindKey(out, "ResY", yb, ye) &&
            !PlausibleResolution(atoi(out.substr(xb, xe - xb).c_str()), atoi(out.substr(yb, ye - yb).c_str()))) {
            out.replace(yb, ye - yb, "720");  // positions may shift: ResX is looked up again
            FindKey(out, "ResX", xb, xe);
            out.replace(xb, xe - xb, "1280");
        }
    }
    if (out != text) {
        if (WriteAll(live, out)) return false;
        changed = true;
    }
    if (dirty) DeleteFileW(marker.c_str());
    return true;
}

Outcome Uninstall(HWND w, const std::wstring& game) {
    const std::wstring win = Join(game, L"Binaries\\Win64"), mod = Join(win, L"GGDPVR");
    if (!CloseGameIfRunning(w)) return {1, L"Close the game, then try again."};
    bool settingsChanged = false;
    const bool restored = RestoreGraphicsSettings(settingsChanged);
    const std::wstring loader = Join(win, L"dinput8.dll");
    std::string cur;
    if (ReadAll(loader, cur) && IsOurLoader(cur) && !DeleteFileW(loader.c_str())) return Failed(GetLastError(), loader);
    const std::wstring bk = Join(win, kForeignBackup);
    if (Exists(bk) && !Exists(loader) && !MoveFileW(bk.c_str(), loader.c_str())) return Failed(GetLastError(), bk);
    bool keptIni = false;
    if (Exists(mod)) {
        const std::wstring ini = Join(mod, L"ggdpvr.ini");
        keptIni = Exists(ini) && AskYesNo(w, L"Keep your GGDPVR settings file (ggdpvr.ini) for a later reinstall?", false);
        for (const wchar_t* f : {L"GGDPVR.dll", L"GGDPVR.pdb", L"ggdpvr_debug.ini", L"disabled"}) {
            const std::wstring p = Join(mod, f);
            if (Exists(p) && !DeleteFileW(p.c_str())) return Failed(GetLastError(), p);
        }
        if (!keptIni && Exists(ini) && !DeleteFileW(ini.c_str())) return Failed(GetLastError(), ini);
        RemoveDirectoryW(mod.c_str());  // only when empty
    }
    std::wstring text = L"GGDPVR is uninstalled - the game starts normally again.";
    if (settingsChanged) text += L" Your own graphics settings were put back.";
    if (keptIni) text += L" Your settings file was kept.";
    if (restored) {
        DeleteTree(LocalDir());  // logs, settings backup, the remembered game folder
    } else {
        text += L" Your graphics settings could not be put back: start the game once with the mod and quit it normally "
                L"(the mod's backup is kept in %LOCALAPPDATA%\\GGDPVR).";
    }
    return {0, text};
}

// ---- administrator retry -------------------------------------------------------------------------------------
// The same program, elevated, does the one job (--op) and exits; this window waits, still repainting.
int RunElevated(HWND w, const std::wstring& op, const std::wstring& game) {
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const std::wstring args = L"--op " + op + L" --game-dir \"" + game + L"\"";
    SHELLEXECUTEINFOW si{sizeof si};
    si.fMask = SEE_MASK_NOCLOSEPROCESS;
    si.hwnd = w, si.lpVerb = L"runas", si.lpFile = exe, si.lpParameters = args.c_str(), si.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&si) || !si.hProcess) return -1;  // the player said no
    EnableWindow(w, FALSE);
    while (MsgWaitForMultipleObjects(1, &si.hProcess, FALSE, INFINITE, QS_ALLINPUT) == WAIT_OBJECT_0 + 1) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) TranslateMessage(&m), DispatchMessageW(&m);
    }
    EnableWindow(w, TRUE);
    SetForegroundWindow(w);
    DWORD code = 1;
    GetExitCodeProcess(si.hProcess, &code);
    CloseHandle(si.hProcess);
    return int(code);
}

std::wstring PickFolder(HWND w, const std::wstring& start) {
    std::wstring result;
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return result;
    DWORD o = 0;
    d->GetOptions(&o);
    d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    d->SetTitle(L"Choose your Gal*Gun: Double Peace folder");
    IShellItem* si = nullptr;
    if (!start.empty() && SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&si)))) {
        d->SetFolder(si);
        si->Release();
    }
    if (SUCCEEDED(d->Show(w))) {
        IShellItem* r = nullptr;
        PWSTR p = nullptr;
        if (SUCCEEDED(d->GetResult(&r)) && SUCCEEDED(r->GetDisplayName(SIGDN_FILESYSPATH, &p))) result = p, CoTaskMemFree(p);
        if (r) r->Release();
    }
    d->Release();
    return result;
}

// ---- the window ----------------------------------------------------------------------------------------------
// Everything is drawn by hand with GDI+ in "logical" pixels (96 dpi) and scaled to the monitor.
using namespace Gdiplus;
constexpr float kW = 640, kH = 568;
enum ButtonId { kChange = 1, kPrimary, kPlay, kSettings, kToggle, kLogs, kLogging, kUninstall };
enum ButtonKind { kBig, kNormal, kSmall, kLink, kLinkLeft };
struct Button {
    int id;
    ButtonKind kind;
    RectF r;
    std::wstring text;
    bool enabled = true;
};
std::vector<Button> g_buttons;
int g_hover = -1, g_pressed = -1, g_focus = -1;
bool g_keyboard = false;  // show the focus ring once the keyboard was used
HWND g_wnd = nullptr;
float g_scale = 1.f;
std::wstring g_game;
State g_state;
std::wstring g_message;  // the last action's result, shown under the status
bool g_messageBad = false;
Bitmap* g_logo = nullptr;
std::unique_ptr<Bitmap> g_logoScaled;
float g_logoScaledFor = 0;
const RectF kLogoRect(70, 14, 500, 500.f * 421 / 1100);

Bitmap* LoadPng(int id) {
    const std::string b = Res(id);
    IStream* s = SHCreateMemStream(reinterpret_cast<const BYTE*>(b.data()), UINT(b.size()));
    if (!s) return nullptr;
    Bitmap* src = Bitmap::FromStream(s);
    Bitmap* copy = nullptr;  // decoded copy: GDI+ keeps reading the stream otherwise
    if (src && src->GetLastStatus() == Ok) {
        copy = new Bitmap(int(src->GetWidth()), int(src->GetHeight()), PixelFormat32bppPARGB);
        Graphics g(copy);
        g.DrawImage(src, 0, 0, int(src->GetWidth()), int(src->GetHeight()));
    }
    delete src;
    s->Release();
    return copy;
}

void Layout() {
    g_buttons.clear();
    g_buttons.push_back({kChange, kSmall, RectF(500, 238, 96, 32), L"Change..."});
    g_buttons.push_back({kPrimary, kBig, RectF(24, 372, 592, 56), L"Install"});
    const float bw = (592 - 3 * 10) / 4.f;
    const wchar_t* names[4] = {L"Start game", L"Settings file", L"Turn mod off", L"Log folder"};
    const int ids[4] = {kPlay, kSettings, kToggle, kLogs};
    for (int i = 0; i < 4; ++i) g_buttons.push_back({ids[i], kNormal, RectF(24 + i * (bw + 10), 440, bw, 42), names[i]});
    g_buttons.push_back({kLogging, kLinkLeft, RectF(24, 496, 420, 30), L"Turn logging on (for bug reports)"});
    g_buttons.push_back({kUninstall, kLink, RectF(506, 496, 110, 30), L"Uninstall"});
}
Button* Find(int id) {
    for (auto& b : g_buttons)
        if (b.id == id) return &b;
    return nullptr;
}
void Refresh() {
    g_state = Inspect(g_game);
    const State& s = g_state;
    Button* p = Find(kPrimary);
    p->text = !s.game ? L"Install" : !s.Any() ? L"Install GGDPVR " + kVersion : s.Current() ? L"Reinstall"
              : s.Complete() ? L"Update to version " + kVersion : L"Repair";
    p->enabled = s.game;
    Find(kPlay)->enabled = s.game;
    Find(kSettings)->enabled = s.game && s.ini;
    Find(kToggle)->enabled = s.game && s.Complete();
    Find(kToggle)->text = s.disabled ? L"Turn mod on" : L"Turn mod off";
    Find(kLogs)->enabled = true;
    Find(kLogging)->enabled = s.game && s.ini;
    Find(kLogging)->text = s.logging ? L"Logging is on - turn it off" : L"Turn logging on (for bug reports)";
    Find(kUninstall)->enabled = s.game && (s.Any() || s.foreignBackup);
    if (g_wnd) InvalidateRect(g_wnd, nullptr, FALSE);
}

void RoundRect(GraphicsPath& p, const RectF& r, float rad) {
    const float d = rad * 2;
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
    p.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
    p.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
    p.CloseFigure();
}
void Text(Graphics& g, const std::wstring& t, const RectF& r, float size, int style, Color c, StringAlignment h,
          StringAlignment v, bool ellipsisPath = false, bool wrap = false) {
    FontFamily ff(L"Segoe UI");
    Font f(&ff, size, style, UnitPixel);
    StringFormat sf;
    sf.SetAlignment(h);
    sf.SetLineAlignment(v);
    if (!wrap) sf.SetFormatFlags(StringFormatFlagsNoWrap);
    sf.SetTrimming(ellipsisPath ? StringTrimmingEllipsisPath : StringTrimmingEllipsisCharacter);
    SolidBrush b(c);
    g.DrawString(t.c_str(), -1, &f, r, &sf, &b);
}

void DrawButton(Graphics& g, const Button& b, int index) {
    const bool hover = index == g_hover && b.enabled, down = index == g_pressed && hover;
    RectF r = b.r;
    if (down) r.Offset(0, 1);
    if (b.kind == kLink || b.kind == kLinkLeft) {
        const Color c = !b.enabled ? Color(255, 176, 160, 176) : hover ? Color(255, 200, 40, 110) : Color(255, 140, 60, 115);
        Text(g, b.text, r, 14, hover ? FontStyleUnderline : FontStyleRegular, c,
             b.kind == kLink ? StringAlignmentFar : StringAlignmentNear, StringAlignmentCenter);
    } else {
        GraphicsPath path;
        RoundRect(path, r, b.kind == kBig ? 14.f : 10.f);
        if (b.kind == kBig) {
            const Color top = !b.enabled ? Color(255, 214, 200, 210) : hover ? Color(255, 255, 122, 182) : Color(255, 255, 98, 166);
            const Color bot = !b.enabled ? Color(255, 196, 180, 192) : hover ? Color(255, 236, 66, 146) : Color(255, 218, 44, 126);
            LinearGradientBrush br(PointF(0, r.Y), PointF(0, r.GetBottom()), top, bot);
            g.FillPath(&br, &path);
            Pen edge(Color(255, 255, 255, 255), 2.f);
            g.DrawPath(&edge, &path);
            Text(g, b.text, r, 21, FontStyleBold, Color(255, 255, 255, 255), StringAlignmentCenter, StringAlignmentCenter);
        } else {
            SolidBrush fill(!b.enabled ? Color(170, 255, 255, 255) : hover ? Color(255, 255, 238, 247) : Color(235, 255, 255, 255));
            g.FillPath(&fill, &path);
            Pen edge(!b.enabled ? Color(255, 226, 210, 222) : Color(255, 240, 139, 190), 1.5f);
            g.DrawPath(&edge, &path);
            Text(g, b.text, r, b.kind == kSmall ? 13.f : 14.f, FontStyleBold,
                 b.enabled ? Color(255, 122, 31, 92) : Color(255, 190, 170, 184), StringAlignmentCenter, StringAlignmentCenter);
        }
    }
    if (g_keyboard && index == g_focus && b.enabled) {
        GraphicsPath f;
        RectF fr = b.r;
        fr.Inflate(3, 3);
        RoundRect(f, fr, 13.f);
        Pen p(Color(255, 122, 31, 92), 1.5f);
        p.SetDashStyle(DashStyleDot);
        g.DrawPath(&p, &f);
    }
}

void Paint(HDC hdc) {
    RECT rc;
    GetClientRect(g_wnd, &rc);
    const int w = rc.right, h = rc.bottom;
    if (w <= 0 || h <= 0) return;
    Bitmap buf(w, h, PixelFormat32bppPARGB);
    Graphics g(&buf);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.ScaleTransform(g_scale, g_scale);
    // background: the game's pinks, with soft light spots
    LinearGradientBrush bg(PointF(0, 0), PointF(0, kH), Color(255, 255, 228, 242), Color(255, 236, 222, 255));
    g.FillRectangle(&bg, RectF(0, 0, kW + 2, kH + 2));
    const float spots[][3] = {{40, 60, 90}, {600, 40, 70}, {560, 300, 110}, {20, 420, 80}, {330, 520, 60}};
    for (auto& sp : spots) {
        SolidBrush b(Color(70, 255, 255, 255));
        g.FillEllipse(&b, sp[0] - sp[2], sp[1] - sp[2], sp[2] * 2, sp[2] * 2);
    }
    // logo (pre-scaled once per monitor scale: smooth and cheap)
    if (g_logo) {
        if (!g_logoScaled || g_logoScaledFor != g_scale) {
            const int lw = int(kLogoRect.Width * g_scale + 0.5f), lh = int(kLogoRect.Height * g_scale + 0.5f);
            g_logoScaled = std::make_unique<Bitmap>(lw, lh, PixelFormat32bppPARGB);
            Graphics lg(g_logoScaled.get());
            lg.SetInterpolationMode(InterpolationModeHighQualityBicubic);
            lg.SetPixelOffsetMode(PixelOffsetModeHighQuality);
            lg.DrawImage(g_logo, 0, 0, lw, lh);
            g_logoScaledFor = g_scale;
        }
        g.ResetTransform();
        g.DrawImage(g_logoScaled.get(), int(kLogoRect.X * g_scale + 0.5f), int(kLogoRect.Y * g_scale + 0.5f));
        g.ScaleTransform(g_scale, g_scale);
    }
    // panel: game folder + status
    const RectF panel(24, 220, 592, 140);
    {
        GraphicsPath p;
        RoundRect(p, panel, 14);
        SolidBrush fill(Color(225, 255, 255, 255));
        g.FillPath(&fill, &p);
        Pen edge(Color(255, 243, 182, 214), 1.5f);
        g.DrawPath(&edge, &p);
    }
    const Color ink(255, 74, 30, 66), soft(255, 128, 92, 120);
    Text(g, L"GAME FOLDER", RectF(44, 232, 440, 18), 11.5f, FontStyleBold, soft, StringAlignmentNear, StringAlignmentNear);
    Text(g, g_game.empty() ? L"(not found)" : g_game, RectF(44, 251, 446, 22), 13, FontStyleRegular, ink,
         StringAlignmentNear, StringAlignmentNear, true);
    Pen sep(Color(255, 246, 214, 232), 1.f);
    g.DrawLine(&sep, 44.f, 284.f, 596.f, 284.f);
    const State& s = g_state;
    Color dot;
    std::wstring title, detail;
    if (!s.game) {
        dot = Color(255, 224, 75, 75), title = L"Game folder not found";
        detail = L"Click Change... and choose your Gal*Gun: Double Peace folder - the one with the Binaries folder in it.";
    } else if (s.Current() && s.disabled) {
        dot = Color(255, 245, 166, 35), title = L"Installed - switched off";
        detail = L"The game starts without VR. Click Turn mod on to play in VR again.";
    } else if (s.Current()) {
        dot = Color(255, 43, 182, 115), title = L"Installed - version " + kVersion;
        detail = L"Start your VR software (for example Meta Quest Link or SteamVR) first, then start the game.";
    } else if (s.Complete()) {
        dot = Color(255, 245, 166, 35), title = L"Installed - a different version";
        detail = L"Update installs version " + kVersion + L". Your settings are kept.";
    } else if (s.Any()) {
        dot = Color(255, 245, 166, 35), title = L"Partly installed";
        detail = L"Some of the mod's files are missing. Repair puts them back and keeps your settings.";
    } else {
        dot = Color(255, 160, 150, 160), title = L"Not installed";
        detail = s.loader == kForeign ? L"Another mod's dinput8.dll is in the game folder - Install asks you what to do with it."
                                      : L"Install adds GGDPVR to the game folder. None of the game's own files are replaced.";
    }
    if (!g_message.empty()) detail = g_message;
    SolidBrush db(dot);
    g.FillEllipse(&db, 44.f, 300.f, 12.f, 12.f);
    Text(g, title, RectF(64, 293, 530, 24), 16, FontStyleBold, ink, StringAlignmentNear, StringAlignmentNear);
    Text(g, detail, RectF(44, 318, 552, 38), 13, FontStyleRegular, g_messageBad ? Color(255, 196, 40, 70) : soft,
         StringAlignmentNear, StringAlignmentNear, false, true);
    for (size_t i = 0; i < g_buttons.size(); ++i) DrawButton(g, g_buttons[i], int(i));
    Text(g, L"GGDPVR " + kVersion + L"  -  fan-made VR mod for Gal*Gun: Double Peace", RectF(24, 530, 592, 28), 12,
         FontStyleRegular, soft, StringAlignmentNear, StringAlignmentCenter);
    Graphics screen(hdc);
    screen.DrawImage(&buf, 0, 0, w, h);
}

int HitTest(LPARAM lp) {
    const float x = float(short(LOWORD(lp))) / g_scale, y = float(short(HIWORD(lp))) / g_scale;
    for (size_t i = 0; i < g_buttons.size(); ++i)
        if (g_buttons[i].r.Contains(x, y)) return int(i);
    return -1;
}

void RunOp(const std::wstring& op) {
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    Outcome o = op == L"install" ? Install(g_wnd, g_game) : Uninstall(g_wnd, g_game);
    SetCursor(old);
    if (o.code == 2) {
        if (AskYesNo(g_wnd, o.text + L"\n\nTry again with administrator rights?", true)) {
            const int r = RunElevated(g_wnd, op, g_game);
            if (r == 0) o = {0, op == L"install" ? L"Installed version " + kVersion + L". Start your VR software first, then the game."
                                                 : L"GGDPVR is uninstalled - the game starts normally again."};
            else o = {1, r < 0 ? L"Not done - administrator rights weren't given." : L"Not done - see the message from the administrator window."};
            if (r == 0 && op == L"install") Remember(g_game);
        } else {
            o.code = 1;
        }
    }
    g_message = o.text;
    g_messageBad = o.code != 0;
    Refresh();
}

void OnClick(int id) {
    const std::wstring win = Join(g_game, L"Binaries\\Win64");
    switch (id) {
        case kChange: {
            const std::wstring picked = PickFolder(g_wnd, g_game);
            if (picked.empty()) return;
            const std::wstring game = NormalizeGameDir(picked);
            if (game.empty()) {
                Tell(g_wnd, L"Gal*Gun: Double Peace isn't in that folder (Binaries\\Win64\\GG2Game.exe was not found).\n\n"
                            L"Please choose the game's main folder - the one with the Binaries folder in it.", MB_ICONWARNING);
                return;
            }
            g_game = game, g_message.clear();
            Remember(game);
            Refresh();
            return;
        }
        case kPrimary:
            RunOp(L"install");
            return;
        case kUninstall:
            if (AskYesNo(g_wnd, L"Remove GGDPVR from this game?", true)) RunOp(L"uninstall");
            return;
        case kPlay:
            if (IsSteamCopy(g_game)) ShellExecuteW(g_wnd, L"open", L"steam://rungameid/511740", nullptr, nullptr, SW_SHOWNORMAL);
            else ShellExecuteW(g_wnd, L"open", Join(win, L"GG2Game.exe").c_str(), nullptr, win.c_str(), SW_SHOWNORMAL);
            return;
        case kSettings: {
            const std::wstring arg = L"\"" + Join(win, L"GGDPVR\\ggdpvr.ini") + L"\"";
            ShellExecuteW(g_wnd, L"open", L"notepad.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
            return;
        }
        case kToggle: {
            const std::wstring off = Join(win, L"GGDPVR\\disabled");
            DWORD e = 0;
            if (g_state.disabled) e = DeleteFileW(off.c_str()) ? 0 : GetLastError();
            else e = WriteAll(off, "");
            g_message = e ? L"Could not change it (Windows error " + std::to_wstring(e) + L")."
                          : g_state.disabled ? L"Switched on - the game starts in VR again."
                                             : L"Switched off - the game starts without VR, as if the mod wasn't installed.";
            g_messageBad = e != 0;
            Refresh();
            return;
        }
        case kLogs: {
            const std::wstring logs = Join(LocalDir(), L"logs");
            if (Exists(logs)) ShellExecuteW(g_wnd, L"open", logs.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            else Tell(g_wnd, L"There's no log yet.\n\nThe mod writes one each time the game runs with logging on (if you "
                             L"turned it off, click Turn logging on first). Play until the problem happens; the log then "
                             L"appears in this folder:\n%LOCALAPPDATA%\\GGDPVR\\logs");
            return;
        }
        case kLogging: {
            const bool on = !g_state.logging;
            const DWORD e = SetLogging(Join(win, L"GGDPVR\\ggdpvr.ini"), on);
            g_message = e ? L"Could not change the settings file (Windows error " + std::to_wstring(e) + L")."
                      : on ? L"Logging is on from the next game start. After the problem happens, quit the game, click Log "
                             L"folder and send ggdpvr.log."
                           : L"Logging is off from the next game start - the mod writes no log.";
            g_messageBad = e != 0;
            Refresh();
            return;
        }
        default:
            return;
    }
}

void SetScaleForDpi(UINT dpi) {
    g_scale = float(dpi) / 96.f;
    g_logoScaled.reset();
}

LRESULT CALLBACK WndProc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(w, &ps);
            Paint(hdc);
            EndPaint(w, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_MOUSEMOVE: {
            const int h = HitTest(lp);
            if (h != g_hover) {
                g_hover = h;
                InvalidateRect(w, nullptr, FALSE);
                TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, w, 0};
                TrackMouseEvent(&t);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            g_hover = -1;
            InvalidateRect(w, nullptr, FALSE);
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT && g_hover >= 0 && g_buttons[size_t(g_hover)].enabled) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        case WM_LBUTTONDOWN:
            g_pressed = HitTest(lp);
            SetCapture(w);
            InvalidateRect(w, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP: {
            ReleaseCapture();
            const int h = HitTest(lp), p = g_pressed;
            g_pressed = -1;
            InvalidateRect(w, nullptr, FALSE);
            if (h >= 0 && h == p && g_buttons[size_t(h)].enabled) g_focus = h, OnClick(g_buttons[size_t(h)].id);
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_TAB) {
                g_keyboard = true;
                const int n = int(g_buttons.size()), dir = GetKeyState(VK_SHIFT) < 0 ? -1 : 1;
                for (int i = 0, f = g_focus; i < n; ++i) {
                    f = (f + dir + n) % n;
                    if (g_buttons[size_t(f)].enabled) {
                        g_focus = f;
                        break;
                    }
                }
                InvalidateRect(w, nullptr, FALSE);
                return 0;
            }
            if (wp == VK_RETURN || wp == VK_SPACE) {
                const int f = g_focus >= 0 && g_keyboard ? g_focus : 1;  // Enter alone: the big button
                if (f < int(g_buttons.size()) && g_buttons[size_t(f)].enabled) OnClick(g_buttons[size_t(f)].id);
                return 0;
            }
            if (wp == VK_ESCAPE) {
                DestroyWindow(w);
                return 0;
            }
            break;
        case WM_ACTIVATE:
            if (LOWORD(wp) != WA_INACTIVE) Refresh();  // files may have changed meanwhile
            break;
        case WM_DPICHANGED: {
            SetScaleForDpi(HIWORD(wp));
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(w, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(w, nullptr, FALSE);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(w, msg, wp, lp);
}

void SizeWindow(HWND w) {
    const UINT dpi = GetDpiForWindow(w);
    SetScaleForDpi(dpi);
    RECT r{0, 0, LONG(kW * g_scale + 0.5f), LONG(kH * g_scale + 0.5f)};
    const DWORD style = DWORD(GetWindowLongW(w, GWL_STYLE)), ex = DWORD(GetWindowLongW(w, GWL_EXSTYLE));
    AdjustWindowRectExForDpi(&r, style, FALSE, ex, dpi);
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTOPRIMARY), &mi);
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    const int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - ww) / 2;
    const int y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - wh) / 2;
    SetWindowPos(w, nullptr, x, y, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
}
}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    // load Windows' own DLLs only from System32 (a stray DLL in the Downloads folder is never picked up)
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring op, dir;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--op" && i + 1 < argc) op = argv[++i];
        else if (a == L"--game-dir" && i + 1 < argc) dir = argv[++i];
        else if (a == L"--yes") g_yes = true;
    }
    LocalFree(argv);
    if (!op.empty()) {  // no window: one job (also how the administrator retry runs) - always on a named folder
        const std::wstring game = NormalizeGameDir(dir);
        if (game.empty()) {
            Tell(nullptr, L"Gal*Gun: Double Peace was not found in the folder given with --game-dir \"<game folder>\".", MB_ICONWARNING);
            return 3;
        }
        Outcome o;
        if (op == L"install") o = Install(nullptr, game);
        else if (op == L"uninstall") o = Uninstall(nullptr, game);
        else o = {1, L"Unknown --op (install or uninstall)."};
        if (o.code) Tell(nullptr, o.text, MB_ICONWARNING);
        return o.code;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    GdiplusStartupInput gsi;
    ULONG_PTR token = 0;
    GdiplusStartup(&token, &gsi, nullptr);
    g_logo = LoadPng(IDR_LOGO);
    Layout();

    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                             GetSystemMetrics(SM_CYICON), 0));
    wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                               GetSystemMetrics(SM_CYSMICON), 0));
    wc.lpszClassName = L"GGDPVRSetup";
    RegisterClassExW(&wc);
    g_wnd = CreateWindowExW(0, wc.lpszClassName, kTitle, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                            CW_USEDEFAULT, CW_USEDEFAULT, 640, 548, nullptr, nullptr, inst, nullptr);
    SizeWindow(g_wnd);
    g_game = DetectGame();
    Refresh();
    ShowWindow(g_wnd, show);
    UpdateWindow(g_wnd);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) TranslateMessage(&m), DispatchMessageW(&m);

    g_logoScaled.reset();
    delete g_logo;
    GdiplusShutdown(token);
    CoUninitialize();
    return 0;
}
