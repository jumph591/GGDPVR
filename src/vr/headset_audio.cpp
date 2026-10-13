#include "headset_audio.h"

#include <windows.h>
#include <objbase.h>
#include <cwctype>
#include <string>

#include "../core/config.h"
#include "../core/hooks.h"
#include "../core/log.h"
#include "../xr/xr_screen.h"

namespace ggdpvr::vr::audio {
namespace {
// XAudio2 2.7 (DirectX SDK June 2010): CLSID_XAudio2 / CLSID_XAudio2_Debug, IID_IXAudio2
constexpr GUID kClsidXAudio2{0x5a508685, 0xa254, 0x4fba, {0x9b, 0x82, 0x9a, 0x24, 0xb0, 0x03, 0x06, 0xaf}};
constexpr GUID kClsidXAudio2Debug{0xdb05ea35, 0x0329, 0x4d4b, {0xa5, 0x3a, 0x6d, 0xea, 0xd0, 0x3d, 0x38, 0x52}};
constexpr GUID kIidXAudio2{0x8bcf1f58, 0x9fe7, 0x4583, {0x8a, 0xc6, 0xe2, 0xad, 0xc4, 0x65, 0xc8, 0xbb}};
// IXAudio2 (2.7) vtable: 3 GetDeviceCount, 4 GetDeviceDetails, 10 CreateMasteringVoice
constexpr int kGetDeviceCount = 3, kGetDeviceDetails = 4, kCreateMasteringVoice = 10;
#pragma pack(push, 1)
struct DeviceDetails {  // XAUDIO2_DEVICE_DETAILS (2.7): only the strings are read
    wchar_t deviceId[256];
    wchar_t displayName[256];
    uint8_t rest[64];  // role + WAVEFORMATEXTENSIBLE (44 bytes) and slack
};
#pragma pack(pop)

using CoCreateFn = HRESULT(WINAPI*)(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID*);
using CountFn = HRESULT(__fastcall*)(void*, UINT32*);
using DetailsFn = HRESULT(__fastcall*)(void*, UINT32, DeviceDetails*);
using MasterFn = HRESULT(__fastcall*)(void*, void**, UINT32, UINT32, UINT32, UINT32, const void*);
CoCreateFn o_CoCreate = nullptr;
CountFn g_count = nullptr;
DetailsFn o_Details = nullptr;
MasterFn o_Master = nullptr;
bool g_vtHooked = false;
int g_target = -1;  // XAudio2 device index of the headset (-1: not found / not resolved yet)
bool g_resolved = false;

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = wchar_t(towlower(c));
    return s;
}
// once, on the first device query: the headset's audio endpoint from the OpenXR runtime, matched against XAudio2's list
void Resolve(void* xa) {
    if (g_resolved) return;
    g_resolved = true;
    // which device: 1) headset_audio_device in ggdpvr.ini (part of its name), 2) the runtime's own answer (Meta's
    // runtime: XR_OCULUS_audio_device_guid; only with a headset connected). None: the Windows default stays - SteamVR,
    // Steam Link, Virtual Desktop and WMR switch it to their headset themselves. (0.9.110: guessing by known VR device
    // names is gone - under Steam Link it picked Meta Link's idle "Oculus Virtual Audio" and the game was silent.)
    std::wstring want;
    const bool connected = xr::ProbeHeadsetAudio(want);
    UINT32 n = 0;
    if (!g_count || FAILED(g_count(xa, &n))) return;
    const std::wstring w = Lower(want), pref = Lower(config::Get().vrHeadsetAudioDevice);
    static const wchar_t* const kKnown[] = {L"oculus virtual audio", L"meta quest", L"virtual desktop", L"valve index", L"vive",
                                            L"reverb", L"mixed reality", L"pimax", L"pico", L"bigscreen", L"varjo"};
    int byPref = -1, byId = -1, byName = -1;
    LOG("[audio] %u XAudio2 output devices", n);
    for (UINT32 i = 0; i < n; ++i) {
        DeviceDetails d{};
        if (FAILED(o_Details(xa, i, &d))) continue;
        d.deviceId[255] = d.displayName[255] = 0;
        const std::wstring id = Lower(d.deviceId), name = Lower(d.displayName);
        const bool idMatch = !w.empty() && (id == w || id.find(w) != std::wstring::npos || w.find(id) != std::wstring::npos);
        const bool prefMatch = !pref.empty() && (name.find(pref) != std::wstring::npos || id.find(pref) != std::wstring::npos);
        bool known = false;
        const wchar_t* kw = L"";
        for (const wchar_t* k : kKnown)
            if (name.find(k) != std::wstring::npos) known = true, kw = k;
        // (0.9.99: no device names or IDs in the log - user-renamed devices, monitor models and the PC-unique endpoint
        // IDs were listed; only the candidates and why they matched)
        if (prefMatch || idMatch || known)
            LOG("[audio] XAudio2 device %u:%s%s%s%ls", i, prefMatch ? " [ini]" : "", idMatch ? " [runtime]" : "",
                known ? " [VR device name: " : "", known ? (std::wstring(kw) + L"]").c_str() : L"");
        if (prefMatch && byPref < 0) byPref = int(i);
        if (idMatch && byId < 0) byId = int(i);
        if (known && byName < 0) byName = int(i);
    }
    (void)byName;  // still logged as a candidate, never chosen
    g_target = byPref >= 0 ? byPref : !connected ? -1 : byId;
    if (!pref.empty() && byPref < 0) LOG("[audio] headset_audio_device '%ls' matches no device", config::Get().vrHeadsetAudioDevice.c_str());
    if (g_target < 0)
        LOG("[audio] %s - the game keeps the Windows default output",
            connected ? "the runtime names no audio device" : "no headset connected at start");
    else
        LOG("[audio] the game's sound goes to XAudio2 device %d (%s)", g_target,
            g_target == byPref ? "headset_audio_device" : "the runtime's headset device");
}
// The game asks for device 0 (the Windows default) only: 0 and the headset's index are swapped
UINT32 Map(UINT32 i) {
    if (g_target <= 0) return i;
    return i == 0 ? UINT32(g_target) : i == UINT32(g_target) ? 0 : i;
}
HRESULT __fastcall hk_Details(void* xa, UINT32 i, DeviceDetails* d) {
    Resolve(xa);
    return o_Details(xa, Map(i), d);
}
HRESULT __fastcall hk_Master(void* xa, void** mv, UINT32 ch, UINT32 rate, UINT32 flags, UINT32 dev, const void* fx) {
    Resolve(xa);
    const UINT32 use = Map(dev);
    const HRESULT hr = o_Master(xa, mv, ch, rate, flags, use, fx);
    LOG("[audio] mastering voice on device %u (asked %u): 0x%08lX", use, dev, hr);
    if (FAILED(hr) && use != dev) return o_Master(xa, mv, ch, rate, flags, dev, fx);  // fall back to the default
    return hr;
}
HRESULT WINAPI hk_CoCreate(REFCLSID clsid, LPUNKNOWN outer, DWORD ctx, REFIID iid, LPVOID* out) {
    const HRESULT hr = o_CoCreate(clsid, outer, ctx, iid, out);
    if (SUCCEEDED(hr) && out && *out && !g_vtHooked && IsEqualGUID(iid, kIidXAudio2) &&
        (IsEqualGUID(clsid, kClsidXAudio2) || IsEqualGUID(clsid, kClsidXAudio2Debug))) {
        g_vtHooked = true;
        void** vt = *static_cast<void***>(*out);
        g_count = reinterpret_cast<CountFn>(vt[kGetDeviceCount]);
        const bool ok = hooks::CreateAt("IXAudio2::GetDeviceDetails", vt[kGetDeviceDetails], (void*)&hk_Details, (void**)&o_Details) &&
                        hooks::CreateAt("IXAudio2::CreateMasteringVoice", vt[kCreateMasteringVoice], (void*)&hk_Master, (void**)&o_Master);
        if (ok) hooks::EnableAll();
        LOG("[audio] XAudio2 2.7 created: device hooks %s", ok ? "installed" : "FAILED");
    }
    return hr;
}
// The game mutes itself when its window loses focus: its window code sets the engine's global volume multiplier to 0
// (UE3's GVolumeMultiplier; found in GG2Game.exe at this rva by the !volscan dev command - the one float that goes 1 -> 0).
// In VR the desktop focus is invisible to the player - clicking OBS or Discord silenced the headset and the recording.
constexpr uintptr_t kVolumeMultiplierRva = 0x303DED8;
int g_bgState = 0;  // 0 unchecked, 1 on, -1 off (unexpected value: another game build)
}  // namespace

void KeepPlayingInBackground() {
    if (g_bgState < 0 || !config::Get().vrEnabled) return;
    auto* v = reinterpret_cast<volatile float*>(reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr)) + kVolumeMultiplierRva);
    const float now = *v;
    if (g_bgState == 0) {
        g_bgState = (now == 0.f || now == 1.f) ? 1 : -1;
        LOG("[audio] background sound: %s", g_bgState > 0 ? "on (the game keeps playing when another window has the focus)"
                                                          : "OFF - unexpected value, not this game build");
        if (g_bgState < 0) return;
    }
    if (now == 0.f) *v = 1.f;
}

void Install() {
    if (!config::Get().vrEnabled || !config::Get().vrHeadsetAudio) return;
    HMODULE m = GetModuleHandleW(L"combase.dll");
    if (!m) m = LoadLibraryW(L"ole32.dll");
    void* fn = m ? reinterpret_cast<void*>(GetProcAddress(m, "CoCreateInstance")) : nullptr;
    if (!fn) {
        LOG("[audio] CoCreateInstance not found: headset audio off");
        return;
    }
    hooks::CreateAt("CoCreateInstance", fn, (void*)&hk_CoCreate, (void**)&o_CoCreate);
}
}  // namespace ggdpvr::vr::audio
