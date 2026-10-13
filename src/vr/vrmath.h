#pragma once
#include <cmath>
#include <cstdint>

// Small math helpers for OpenXR <-> UE3 conversion.
// OpenXR: right-handed, +X right, +Y up, -Z forward, metres.
// UE3:    left-handed,  +X forward, +Y right, +Z up, Unreal units; FRotator in 65536 units per turn.
namespace ggdpvr::vr {
struct V3 { float x = 0, y = 0, z = 0; };
struct Q4 { float x = 0, y = 0, z = 0, w = 1; };
struct Rot { int32_t pitch = 0, yaw = 0, roll = 0; };  // UE3 FRotator layout (Pitch, Yaw, Roll)

constexpr float kPi = 3.14159265358979f;
inline float RotToRad(int32_t u) { return float(u) * (2.f * kPi / 65536.f); }
inline int32_t RadToRot(float r) { return int32_t(std::lround(r * (65536.f / (2.f * kPi)))); }

inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

inline Q4 Mul(Q4 a, Q4 b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline V3 Rotate(Q4 q, V3 v) {  // q * v * q^-1
    const V3 u{q.x, q.y, q.z};
    const float s = q.w;
    V3 c1{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
    V3 c2{u.y * c1.z - u.z * c1.y, u.z * c1.x - u.x * c1.z, u.x * c1.y - u.y * c1.x};
    return v + c1 * (2.f * s) + c2 * 2.f;
}
inline Q4 YawQ(float yaw) { return {0, std::sin(yaw / 2), 0, std::cos(yaw / 2)}; }  // about +Y (OpenXR)
inline float YawOf(Q4 q) {  // OpenXR yaw about +Y (forward = -Z)
    return std::atan2(2.f * (q.w * q.y + q.x * q.z), 1.f - 2.f * (q.y * q.y + q.x * q.x));
}

// OpenXR direction/offset (metres) -> UE3 local axes
inline V3 XrToUe(V3 v) { return {-v.z, v.x, v.y}; }

// UE3 FRotationMatrix rows: X (forward), Y (right), Z (up)
struct Axes { V3 x, y, z; };
inline Axes AxesOf(Rot r) {
    const float p = RotToRad(r.pitch), y = RotToRad(r.yaw), ro = RotToRad(r.roll);
    const float sp = std::sin(p), cp = std::cos(p), sy = std::sin(y), cy = std::cos(y), sr = std::sin(ro), cr = std::cos(ro);
    return {{cp * cy, cp * sy, sp},
            {sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp},
            {-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp}};
}
inline V3 ToWorld(const Axes& a, V3 local) { return a.x * local.x + a.y * local.y + a.z * local.z; }
inline Rot RotFromAxes(V3 fwd, V3 up) {
    const float pitch = std::atan2(fwd.z, std::sqrt(fwd.x * fwd.x + fwd.y * fwd.y));
    const float yaw = std::atan2(fwd.y, fwd.x);
    const float sp = std::sin(pitch), cp = std::cos(pitch), sy = std::sin(yaw), cy = std::cos(yaw);
    const V3 y0{-sy, cy, 0}, z0{-sp * cy, -sp * sy, cp};
    const float roll = std::atan2(Dot(up, y0), Dot(up, z0));
    return {RadToRot(pitch), RadToRot(yaw), RadToRot(roll)};
}
}  // namespace ggdpvr::vr
