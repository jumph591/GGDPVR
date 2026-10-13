#include "gfx_vars.h"

#include <cstring>
#include <vector>

#include "../core/game.h"
#include "../ue3/objects.h"

namespace ggdpvr::vr::gfx {
namespace {
struct FString {  // UE3 TArray<TCHAR>
    const wchar_t* data;
    int32_t num, max;
};
struct Fn {
    uintptr_t fn = 0;
    uint32_t path = 0, ret = 0, b = 0, size = 0;
};
Fn Lookup(const char* name) {
    Fn f;
    f.fn = ue3::FindObject(name, "Function");
    if (!f.fn) return f;
    for (auto& p : ue3::Properties(f.fn)) {
        if (p.name == "Path") f.path = p.offset;
        if (p.name == "ReturnValue") f.ret = p.offset;
        if (p.name == "B" || p.name == "F") f.b = p.offset;
        f.size = std::max(f.size, p.offset + p.elementSize);
    }
    return f;
}
void Call(uintptr_t obj, uintptr_t fn, void* parms) {
    uintptr_t vt = 0, pe = 0;
    game::Read(obj, vt), game::Read(vt + game::rva::ProcessEvent_VSlot, pe);
    using PEFn = void(__fastcall*)(uintptr_t, uintptr_t, void*, void*);
    reinterpret_cast<PEFn>(pe)(obj, fn, parms, nullptr);
}
FString Str(const std::wstring& w) { return {w.c_str(), int32_t(w.size() + 1), int32_t(w.size() + 1)}; }
}  // namespace

uintptr_t FindPlayer(const char* classSubstr) {
    for (int32_t i = 0, n = ue3::NumObjects(); i < n; ++i) {
        const uintptr_t o = ue3::ObjectAt(i);
        if (!o || !ue3::IsA(o, "GFxMoviePlayer")) continue;
        if (ue3::ObjName(ue3::ClassOf(o)).find(classSubstr) == std::string::npos) continue;
        if (ue3::PathName(o).find("Default__") != std::string::npos) continue;
        uint32_t bits = 0;  // GFxMoviePlayer.bMovieIsOpen (bit 0 at +0x118): a closed movie lingers until GC
        if (!game::Read(o + 0x118, bits) || !(bits & 1)) continue;
        return o;
    }
    return 0;
}

bool GetString(uintptr_t player, const std::string& path, std::string& out) {
    static const Fn f = Lookup("GFxUI.GFxMoviePlayer.GetVariableString");
    if (!f.fn || !player) return false;
    const std::wstring wp(path.begin(), path.end());
    std::vector<uint8_t> parms(f.size + 16, 0);
    const FString s = Str(wp);
    memcpy(parms.data() + f.path, &s, sizeof s);
    Call(player, f.fn, parms.data());
    FString r{};
    memcpy(&r, parms.data() + f.ret, sizeof r);
    out.clear();
    for (int32_t i = 0; r.data && i < r.num - 1; ++i) out += char(r.data[i] < 128 ? r.data[i] : '?');
    return true;  // the returned string's buffer is the engine's (left allocated: diagnostic/rare use)
}

bool GetBool(uintptr_t player, const std::string& path, bool& out) {
    static const Fn f = Lookup("GFxUI.GFxMoviePlayer.GetVariableBool");
    if (!f.fn || !player) return false;
    const std::wstring wp(path.begin(), path.end());
    std::vector<uint8_t> parms(f.size + 16, 0);
    const FString s = Str(wp);
    memcpy(parms.data() + f.path, &s, sizeof s);
    Call(player, f.fn, parms.data());
    uint32_t v = 0;
    memcpy(&v, parms.data() + f.ret, sizeof v);
    out = v & 1;
    return true;
}

bool SetBool(uintptr_t player, const std::string& path, bool value) {
    static const Fn f = Lookup("GFxUI.GFxMoviePlayer.SetVariableBool");
    if (!f.fn || !player) return false;
    const std::wstring wp(path.begin(), path.end());
    std::vector<uint8_t> parms(f.size + 16, 0);
    const FString s = Str(wp);
    memcpy(parms.data() + f.path, &s, sizeof s);
    const uint32_t v = value ? 1u : 0u;
    memcpy(parms.data() + f.b, &v, sizeof v);
    Call(player, f.fn, parms.data());
    return true;
}
bool GetNumber(uintptr_t player, const std::string& path, float& out) {
    static const Fn f = Lookup("GFxUI.GFxMoviePlayer.GetVariableNumber");
    if (!f.fn || !player) return false;
    const std::wstring wp(path.begin(), path.end());
    std::vector<uint8_t> parms(f.size + 16, 0);
    const FString s = Str(wp);
    memcpy(parms.data() + f.path, &s, sizeof s);
    Call(player, f.fn, parms.data());
    memcpy(&out, parms.data() + f.ret, sizeof out);
    return true;
}

bool SetNumber(uintptr_t player, const std::string& path, float value) {
    static const Fn f = Lookup("GFxUI.GFxMoviePlayer.SetVariableNumber");
    if (!f.fn || !player) return false;
    const std::wstring wp(path.begin(), path.end());
    std::vector<uint8_t> parms(f.size + 16, 0);
    const FString s = Str(wp);
    memcpy(parms.data() + f.path, &s, sizeof s);
    memcpy(parms.data() + f.b, &value, sizeof value);
    Call(player, f.fn, parms.data());
    return true;
}
}  // namespace ggdpvr::vr::gfx
