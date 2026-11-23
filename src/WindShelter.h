#pragma once
#include "WindMath.h"
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace osfsmp {
// The three GPUSMPPhysicsNG upwind rays, in degrees, with unchanged weights.
inline constexpr std::array<float,3> ShelterAzimuth{0.0f,-20.0f,20.0f};
inline constexpr std::array<float,3> ShelterElevation{6.0f,14.0f,14.0f};
inline constexpr std::array<float,3> ShelterWeight{0.55f,0.225f,0.225f};
inline constexpr float ShelterRayReach=3000.0f;

// Native FSMP actor wind shelter uses a linear distance ramp from 50 to 2000
// Skyrim units. Apply the same per hit; a clear ray contributes no obstruction.
float ShelterExposure(std::array<float,3> hitDistances,
                      float noWindDistance=50.0f,
                      float maxWindDistance=2000.0f) noexcept;

struct ShelterBonePosition {
    std::uintptr_t body{};
    Vec3 origin{};
};
struct ShelterPointSelection {
    unsigned axis{2};
    float extent{};
    std::vector<ShelterBonePosition> points; // sorted along the chosen axis
};
// Choose 1/3/5 actual rigid-body positions from each independent FSMP system.
// For one probe prefer the system's kinematic anchor; for 3/5 use projected
// 20/50/80 or 10/30/50/70/90 percentiles along the longest world-space axis.
ShelterPointSelection SelectShelterPoints(
    std::span<const ShelterBonePosition> bones,
    std::uintptr_t anchorBody,
    float threePointThreshold=300.0f,
    float fivePointThreshold=900.0f);

struct ShelterExposurePoint { float coordinate{}, exposure{1.0f}; };
// Linear interpolation between sampled probes; endpoint-clamped outside them.
float InterpolateShelterExposure(std::span<const ShelterExposurePoint> samples,
                                float coordinate) noexcept;
float ShelterCoordinate(Vec3 position,unsigned axis) noexcept;
} // namespace osfsmp
