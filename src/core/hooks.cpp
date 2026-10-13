#include "hooks.h"

#include <MinHook.h>

#include "game.h"
#include "log.h"

namespace ggdpvr::hooks {
bool Init() {
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
        LOG("MH_Initialize failed: %s", MH_StatusToString(s));
        return false;
    }
    return true;
}

bool CreateAt(const char* name, void* target, void* detour, void** original) {
    MH_STATUS s = MH_CreateHook(target, detour, original);
    if (s != MH_OK) {
        LOG("hook %s @%p: MH_CreateHook failed: %s", name, target, MH_StatusToString(s));
        return false;
    }
    LOG("hook %s @%p (rva 0x%X) created", name, target, game::ToRva(reinterpret_cast<uintptr_t>(target)));
    return true;
}

bool Create(const char* name, uint32_t rva, const uint8_t* expect, size_t expectLen, void* detour, void** original) {
    if (expect && !game::VerifyBytes(rva, expect, expectLen)) {
        LOG("hook %s: prologue mismatch at rva 0x%X, NOT hooked", name, rva);
        return false;
    }
    return CreateAt(name, reinterpret_cast<void*>(game::Addr(rva)), detour, original);
}

bool EnableAll() {
    MH_STATUS s = MH_EnableHook(MH_ALL_HOOKS);
    if (s != MH_OK) LOG("MH_EnableHook(all) failed: %s", MH_StatusToString(s));
    return s == MH_OK;
}

void DisableAll() { MH_DisableHook(MH_ALL_HOOKS); }
}  // namespace ggdpvr::hooks
