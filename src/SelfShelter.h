#pragma once
#include "WindMath.h"
#include <span>

namespace osfsmp {
// Portable analogue of GPUSMPPhysicsNG's own-body collision shelter tests.
// Geometry is captured on Skyrim's game thread from the actor's posed skeleton.
struct SelfShelterCapsule { Vec3 a{},b{}; float radius{}; };
struct SelfShelterSphere { Vec3 center{}; float radius{}; };
// Both tests deliberately reject starts inside a collider: strands embedded in
// their own head or shoulder do not become incorrectly permanently sheltered.
bool SelfCapsuleBlocks(Vec3 origin,Vec3 towardWind,float reach,
                       const SelfShelterCapsule& capsule) noexcept;
bool SelfSphereBlocks(Vec3 origin,Vec3 towardWind,float reach,
                      const SelfShelterSphere& sphere) noexcept;
// Returns 0..1 for the fraction of tested hair/cloth nodes that remain exposed.
// Zero samples and invalid geometry fail OPEN, never fabricate an obstruction.
float SelfShelterExposure(std::span<const Vec3> samples,Vec3 towardWind,float reach,
                          std::span<const SelfShelterCapsule> capsules,
                          std::span<const SelfShelterSphere> spheres) noexcept;
} // namespace osfsmp
