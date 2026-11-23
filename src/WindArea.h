#pragma once
#include "WindMath.h"
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace osfsmp {
// Portable adaptation of GPUSMPPhysicsNG's EstimateBoneWindAreas.
// Each triangle contributes 1/3 of its area per corner, then splits the
// corner's share among its four skinning influences. Unmapped influences
// retain their weights in the normalization denominator but receive no area.
struct AreaVertex {
    Vec3 position{};
    std::array<int,4> rigBone{-1,-1,-1,-1};
    std::array<float,4> weight{};
};
void AccumulateTriangleAreas(std::span<const AreaVertex> vertices,
    std::span<const std::uint32_t> indices,
    std::span<float> accumulated);
float EffectiveWindArea(float measuredArea, float segmentLength,
    float defaultWidth = 3.0f, bool allowSegmentFallback = false);
float HalfToFloat(std::uint16_t bits) noexcept;
}
