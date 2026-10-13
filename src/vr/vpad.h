#pragma once
#include <cstdint>

// Stage 7: virtual XInput gamepad.
// GG:DP reads gamepads through xinput1_3 (imported by ordinal). XInputGetState is hooked and a virtual pad,
// driven by the VR controllers (or the developer console), is merged into player 0's state: buttons OR-ed, triggers and
// sticks take the larger magnitude. When no physical pad is connected the virtual one reports as connected while
// it is in use, so the game's own gamepad bindings (shoot X, Doki Y, zoom RB, touch RT, pause Start, ...) apply.
namespace ggdpvr::vr::vpad {
// XINPUT_GAMEPAD button bits
enum : uint16_t {
    kUp = 0x0001, kDown = 0x0002, kLeft = 0x0004, kRight = 0x0008, kStart = 0x0010, kBack = 0x0020,
    kL3 = 0x0040, kR3 = 0x0080, kLB = 0x0100, kRB = 0x0200, kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000,
};
struct State {
    uint16_t buttons = 0;
    float lt = 0, rt = 0;        // 0..1
    float lx = 0, ly = 0;        // -1..1 (y up)
    float rx = 0, ry = 0;
};
bool Install();            // hooks xinput1_3 if it is loaded (safe from DllMain); idempotent
bool Installed();
unsigned long long LastDpadPressMs();  // GetTickCount64 of the last D-pad press seen by the game
void Configure(bool controllersEnabled);  // after the config is loaded
void SetFromControllers(const State& s);  // replaces the controller-driven part (render thread, each frame)
// Debug: hold `buttons` (and/or analog values) for `ms` milliseconds on top of the controller state.
void Pulse(const State& s, uint32_t ms);
// Parses a button name (A, B, X, Y, LB, RB, START, BACK, L3, R3, UP, DOWN, LEFT, RIGHT); 0 if unknown.
uint16_t ButtonByName(const char* name);
}  // namespace ggdpvr::vr::vpad
