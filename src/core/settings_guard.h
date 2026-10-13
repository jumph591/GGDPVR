#pragma once

// Protects the user's live UE3 settings (Documents\My Games\UnrealEngine3\GG2Game\Config\GG2SystemSettings.ini)
// from the mod's runtime overrides (`scale set ...`, `setres`), which the engine persists immediately.
//  - OnProcessAttach (DllMain, before the engine reads its config): if the previous run left overrides
//    behind (dirty marker), restore the baseline; otherwise record the user's current values as baseline.
//  - MarkDirty: called before the mod applies any persisted override.
//  - OnProcessDetach (clean exit, after the engine's own config save): restore the baseline.
// Edits are byte-level line replacements in the main [SystemSettings] section (encoding untouched).
#include <string>

namespace ggdpvr::settings_guard {
void OnProcessAttach();
void MarkDirty();
// The user's own value of a managed [SystemSettings] key (fallback if unknown).
std::string BaselineValue(const char* key, const char* fallback);
void OnProcessDetach();
// Hooks TerminateProcess/ExitProcess of this process (UE3 Shipping exits via TerminateProcess, skipping DLL detach).
bool InstallExitHooks();
// Called in the ExitProcess hook after the settings restore, before the process exits (xr_clean_exit).
void SetExitCallback(void (*fn)());
}  // namespace ggdpvr::settings_guard
