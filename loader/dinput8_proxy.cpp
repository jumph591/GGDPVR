// GGDPVR loader: a transparent dinput8.dll proxy.
// - Forwards every dinput8 export to the real System32\dinput8.dll (loaded lazily on first use).
// - On process attach, loads GGDPVR\GGDPVR.dll from the game's Binaries\Win64 folder unless disabled.
//   Disable without uninstalling: set env GGDPVR_DISABLE=1, or create GGDPVR\disabled (empty file).
#include <windows.h>
#include <unknwn.h>
#include <dxgi.h>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <string>

namespace {
HMODULE g_real = nullptr;
INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK LoadReal(PINIT_ONCE, PVOID, PVOID*) {
    wchar_t sys[MAX_PATH]{};
    GetSystemDirectoryW(sys, MAX_PATH);
    g_real = LoadLibraryW((std::wstring(sys) + L"\\dinput8.dll").c_str());
    if (!g_real) OutputDebugStringW(L"[GGDPVR loader] failed to load system dinput8.dll\n");
    return TRUE;
}

template <typename T> T Real(const char* name) {
    InitOnceExecuteOnce(&g_once, LoadReal, nullptr, nullptr);
    return g_real ? reinterpret_cast<T>(GetProcAddress(g_real, name)) : nullptr;
}

std::wstring ExeDir() {
    wchar_t p[MAX_PATH]{};
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p);
    return s.substr(0, s.find_last_of(L"\\/"));
}

// ---- DirectX 11 (0.9.105): the mod needs the game's D3D11 renderer, which the game only picks with -d3d11 on its
// command line. GG2Game.exe takes its command line from GetCommandLineW (its import from KERNEL32); while the mod is
// enabled that import is pointed here, returning the real command line + " -d3d11". This runs in the loader's
// DllMain - before the game's own startup code reads its command line. An explicit renderer choice wins.
std::wstring g_cmdLine;
LPWSTR WINAPI ForcedCommandLineW() { return g_cmdLine.data(); }

bool PatchExeImport(const char* dll, const char* func, void* replacement, void** original = nullptr) {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    for (auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), dll) != 0 || !imp->OriginalFirstThunk) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(byName->Name), func) != 0) continue;
            DWORD old = 0;
            if (!VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) return false;
            if (original) *original = reinterpret_cast<void*>(slots->u1.Function);
            slots->u1.Function = reinterpret_cast<ULONGLONG>(replacement);
            VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);
            return true;
        }
    }
    return false;
}

// ---- DXGI 1.1 factory (0.9.108): the game makes its D3D11 device on an adapter from a DXGI 1.0 factory
// (CreateDXGIFactory). Such a device can't create keyed-mutex shared textures (E_INVALIDARG) - which capture layers
// like OpenXR OBS Mirror need to copy the headset image. The game is given a DXGI 1.1 factory instead: IDXGIFactory1
// is a superset of IDXGIFactory, so the game's calls work unchanged.
// GG2Game.exe delay-loads dxgi.dll / d3d11.dll: their entries are in the delay-load import table. An entry first
// points at a stub that loads the DLL and overwrites the entry; one replaced before the first call is simply used.
bool PatchExeDelayImport(const char* dll, const char* func, void* replacement, void** original) {
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (!dir.VirtualAddress) return false;
    for (auto* d = reinterpret_cast<IMAGE_DELAYLOAD_DESCRIPTOR*>(base + dir.VirtualAddress); d->DllNameRVA; ++d) {
        if (!d->Attributes.RvaBased || _stricmp(reinterpret_cast<const char*>(base + d->DllNameRVA), dll) != 0) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->ImportNameTableRVA);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->ImportAddressTableRVA);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(byName->Name), func) != 0) continue;
            DWORD old = 0;
            if (!VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) return false;
            if (original) *original = reinterpret_cast<void*>(slots->u1.Function);
            slots->u1.Function = reinterpret_cast<ULONGLONG>(replacement);
            VirtualProtect(&slots->u1.Function, sizeof(void*), old, &old);
            return true;
        }
    }
    return false;
}

using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
CreateFactoryFn g_origCreateFactory = nullptr;
HRESULT WINAPI CreateFactoryAs11(REFIID riid, void** out) {
    // the same dxgi.dll the game's own delay-load would load (normal search order: a dxgi.dll next to the game, such
    // as ReShade's, still comes first)
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    auto create1 = dxgi ? reinterpret_cast<CreateFactoryFn>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
    IUnknown* f = nullptr;
    if (create1 && out && SUCCEEDED(create1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&f)))) {
        const HRESULT hr = f->QueryInterface(riid, out);
        f->Release();
        if (SUCCEEDED(hr)) return hr;
    }
    return g_origCreateFactory ? g_origCreateFactory(riid, out) : E_FAIL;
}

bool ModEnabled(const std::wstring& dir) {
    // the mod's own switch: [advanced] or [vr] enabled=0 turns VR off - then the game keeps its usual setup
    const std::wstring ini = dir + L"\\ggdpvr.ini";
    int enabled = GetPrivateProfileIntW(L"advanced", L"enabled", -1, ini.c_str());
    if (enabled < 0) enabled = GetPrivateProfileIntW(L"vr", L"enabled", 1, ini.c_str());
    return enabled != 0;
}

void UseDxgi11Factory() {
    void* orig = nullptr;
    if (PatchExeDelayImport("dxgi.dll", "CreateDXGIFactory", reinterpret_cast<void*>(&CreateFactoryAs11), &orig) ||
        PatchExeImport("dxgi.dll", "CreateDXGIFactory", reinterpret_cast<void*>(&CreateFactoryAs11), &orig)) {
        g_origCreateFactory = reinterpret_cast<CreateFactoryFn>(orig);
        SetEnvironmentVariableW(L"GGDPVR_DXGI11", L"1");
        OutputDebugStringW(L"[GGDPVR loader] the game gets a DXGI 1.1 factory\n");
    }
}

void ForceD3D11() {
    std::wstring cmd = GetCommandLineW(), lower = cmd;
    for (auto& c : lower) c = wchar_t(towlower(c));
    for (const wchar_t* f : {L"-d3d11", L"-dx11", L"-d3d9", L"-dx9", L"-sm3", L"-d3d10", L"-dx10"})
        if (lower.find(f) != std::wstring::npos) return;  // already chosen (by the player or Steam's launch options)
    g_cmdLine = cmd + L" -d3d11";
    if (PatchExeImport("KERNEL32.dll", "GetCommandLineW", reinterpret_cast<void*>(&ForcedCommandLineW))) {
        SetEnvironmentVariableW(L"GGDPVR_D3D11", L"1");  // the mod reads the real command line: it learns from this
        OutputDebugStringW(L"[GGDPVR loader] DirectX 11 forced (-d3d11 added)\n");
    } else {
        OutputDebugStringW(L"[GGDPVR loader] could not add -d3d11 (GetCommandLineW import not found)\n");
    }
}

void LoadMod() {
    wchar_t env[8]{};
    if (GetEnvironmentVariableW(L"GGDPVR_DISABLE", env, 8) && env[0] == L'1') {
        OutputDebugStringW(L"[GGDPVR loader] disabled by GGDPVR_DISABLE\n");
        return;
    }
    const std::wstring dir = ExeDir() + L"\\GGDPVR";
    if (GetFileAttributesW((dir + L"\\disabled").c_str()) != INVALID_FILE_ATTRIBUTES) {
        OutputDebugStringW(L"[GGDPVR loader] disabled by GGDPVR\\disabled\n");
        return;
    }
    if (GetFileAttributesW((dir + L"\\GGDPVR.dll").c_str()) == INVALID_FILE_ATTRIBUTES) {
        OutputDebugStringW(L"[GGDPVR loader] GGDPVR\\GGDPVR.dll missing - the game runs without the mod\n");
        return;
    }
    if (ModEnabled(dir)) {
        ForceD3D11();
        UseDxgi11Factory();
    }
    // GGDPVR.dll's DllMain only spawns its init thread, so loading it here is safe.
    if (!LoadLibraryW((dir + L"\\GGDPVR.dll").c_str()))
        OutputDebugStringW(L"[GGDPVR loader] GGDPVR\\GGDPVR.dll not loaded\n");
}
}  // namespace

extern "C" {
HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE h, DWORD v, REFIID r, LPVOID* o, LPUNKNOWN u) {
    using F = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    auto f = Real<F>("DirectInput8Create");
    return f ? f(h, v, r, o, u) : E_FAIL;
}
HRESULT WINAPI Proxy_DllCanUnloadNow() {
    auto f = Real<HRESULT(WINAPI*)()>("DllCanUnloadNow");
    return f ? f() : S_FALSE;
}
HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID c, REFIID r, LPVOID* o) {
    auto f = Real<HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*)>("DllGetClassObject");
    return f ? f(c, r, o) : CLASS_E_CLASSNOTAVAILABLE;
}
HRESULT WINAPI Proxy_DllRegisterServer() {
    auto f = Real<HRESULT(WINAPI*)()>("DllRegisterServer");
    return f ? f() : E_FAIL;
}
HRESULT WINAPI Proxy_DllUnregisterServer() {
    auto f = Real<HRESULT(WINAPI*)()>("DllUnregisterServer");
    return f ? f() : E_FAIL;
}
const void* WINAPI Proxy_GetdfDIJoystick() {
    auto f = Real<const void*(WINAPI*)()>("GetdfDIJoystick");
    return f ? f() : nullptr;
}
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(mod);
        LoadMod();
    }
    return TRUE;
}
