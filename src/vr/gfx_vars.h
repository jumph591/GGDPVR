#pragma once
#include <cstdint>
#include <string>

// Scaleform movie variables through the game's own GFxMoviePlayer functions (GetVariableString / GetVariableBool /
// SetVariableBool), e.g. "_root.Chara_01._visible". Game thread (ProcessEvent).
namespace ggdpvr::vr::gfx {
uintptr_t FindPlayer(const char* classSubstr);  // a live GFxMoviePlayer whose class name contains classSubstr
bool GetString(uintptr_t player, const std::string& path, std::string& out);
bool GetBool(uintptr_t player, const std::string& path, bool& out);
bool SetBool(uintptr_t player, const std::string& path, bool value);
bool GetNumber(uintptr_t player, const std::string& path, float& out);
bool SetNumber(uintptr_t player, const std::string& path, float value);
}  // namespace ggdpvr::vr::gfx
