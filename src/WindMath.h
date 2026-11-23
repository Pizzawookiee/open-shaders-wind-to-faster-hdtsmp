#pragma once
#include <cmath>
#include <algorithm>
#include <cstdint>

namespace osfsmp {
struct Vec3 {
    float x{}, y{}, z{};
    friend constexpr Vec3 operator+(Vec3 a, Vec3 b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
    friend constexpr Vec3 operator-(Vec3 a, Vec3 b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
    friend constexpr Vec3 operator*(Vec3 a, float f) { return {a.x*f,a.y*f,a.z*f}; }
    friend constexpr Vec3 operator*(float f, Vec3 a) { return a*f; }
    friend constexpr Vec3 operator/(Vec3 a, float f) { return {a.x/f,a.y/f,a.z/f}; }
    Vec3& operator+=(Vec3 b) { x+=b.x;y+=b.y;z+=b.z;return *this; }
};
// GPUSMPPhysicsNG uses this *only* to normalize coordinates for gust phase.
// FSMP's hdtConvertNi.h copies NiPoint3 coordinates directly into Bullet.
// DO NOT convert Bullet world positions, velocities or forces by this factor.
inline constexpr float kBulletPerSkyrimUnit = 0.01425f;
inline float Dot(Vec3 a, Vec3 b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
inline Vec3 Cross(Vec3 a, Vec3 b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
inline float Length(Vec3 a) { return std::sqrt(Dot(a,a)); }
inline Vec3 Normalized(Vec3 a, Vec3 fallback={0,0,-1}) { const float l=Length(a); return l>1e-7f ? a/l : fallback; }
inline bool Finite(Vec3 a) { return std::isfinite(a.x)&&std::isfinite(a.y)&&std::isfinite(a.z); }
struct WindComponents { Vec3 ambient{}, transient{}; };
// Published Open Shaders vectors: base + gust is ambient, selected is
// either finalVelocity or windExcludingHavokImpulses.
inline WindComponents SeparateWind(Vec3 base,Vec3 gust,Vec3 selected) {
    const auto ambient=base+gust;
    return {ambient,selected-ambient};
}
inline bool ShouldReprobeTransient(bool previouslyActive,Vec3 previousDirection,
                                  float previousMagnitude,Vec3 current,
                                  float threshold=0.02f) {
    const float magnitude=Length(current);
    if(!Finite(current)||magnitude<=threshold)return false;
    if(!previouslyActive)return true;
    const float dot=std::clamp(Dot(Normalized(current),previousDirection),-1.f,1.f);
    return dot<0.75f || magnitude>previousMagnitude*1.5f+0.05f;
}
inline float StaggerProbeOffset(std::uintptr_t identity,float period,unsigned slots=16) {
    return period*static_cast<float>((identity>>4)%slots)/static_cast<float>(slots);
}
// GPU SMP multiplies the per-bone drag multiplier by exposure, not velocity.
// Component weighting retains separate world/actor attenuation for published
// ambient and transient winds without squared-velocity over-attenuation.
struct ShelteredWind { Vec3 wind{}; float exposure{}; };
ShelteredWind ComposeShelteredWind(Vec3 ambient,Vec3 transient,
    float ambientExposure,float transientExposure,float windScale,float maxSpeed) noexcept;
struct WindInput {
    Vec3 wind{};             // Skyrim units/s, from normalized Open Shaders wind * strength * 300
    float windTime{};       // Seconds
    float area{};           // Skyrim units^2
    float mass{};           // FSMP Bullet mass
    float boneFactor{};     // FSMP XML wind-factor (read from verified runtime owner object)
    Vec3 worldAxis{0,0,-1};
    Vec3 worldOrigin{};     // Skyrim world units
    Vec3 linearVelocity{};  // Skyrim units/s (FSMP Bullet uses same coordinate scale)
    float maxWindAcceleration{400.0f}; // global cap from GPU SMP HairWind, not native FSMP
};
// Faithful CPU transcription of GPUSMPPhysicsNG's shaders/Common/HairWind.hlsli.
Vec3 ComputeWindForce(const WindInput& in);
}
