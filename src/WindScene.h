#pragma once
// Game-thread-only NIF area and Havok shelter work. Requests own NiPointer
// references, never Bullet body pointers (which remain opaque tokens).
#include "WindMath.h"
#include <RE/Skyrim.h>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace osfsmp {
struct BodyRequest {
    std::uintptr_t body{};
    std::uintptr_t owner{};
    std::uintptr_t system{};
    std::string name;
    Vec3 worldOrigin{};
    float segmentLength{};
    RE::NiPointer<RE::NiNode> systemRoot;
    RE::NiPointer<RE::NiNode> boneNode;
    bool refreshArea{}; // event window: bypass cached triangle areas
};
struct BodyBinding {
    std::uintptr_t body{};
    std::uintptr_t owner{};
    std::uintptr_t system{};
    std::string name;
    std::uintptr_t nodeToken{};
    std::uintptr_t rootToken{};
    std::uint32_t referenceFormID{};
    float measuredArea{};
    Vec3 skeletonAxisWorld{0,0,-1};
    float skeletonSegmentLength{};
    bool skeletonAxisMeasured{};
    bool nodeMatched{};
    bool areaMeasured{};
    bool allowWorldFallback{};
};
enum class ShelterComponent : std::uint8_t { Ambient, Transient };
struct ShelterRequest {
    std::uintptr_t system{};
    std::uintptr_t anchorBody{};
    std::uintptr_t pointBody{};   // exact selected FSMP rigid body
    std::uint64_t pointEpoch{};   // invalidates asynchronous obsolete layouts
    RE::NiPointer<RE::NiNode> systemRoot;
    Vec3 origin{};
    Vec3 wind{}; // selected component, in Skyrim coordinates
    ShelterComponent component{ShelterComponent::Ambient};
};
struct ShelterBinding {
    std::uintptr_t system{};
    std::uintptr_t anchorBody{};
    std::uintptr_t pointBody{};
    std::uint64_t pointEpoch{};
    std::uintptr_t rootToken{};
    ShelterComponent component{};
    float exposure{1.0f};
    bool sampled{};
};
struct SelfShelterRequest {
    std::uintptr_t system{};
    std::uintptr_t anchorBody{};
    std::uint64_t pointEpoch{};
    RE::NiPointer<RE::NiNode> systemRoot;
    std::vector<Vec3> samples;  // Up to eight validated dynamic bones.
    Vec3 wind{};
    ShelterComponent component{ShelterComponent::Ambient};
};
struct SelfShelterBinding {
    std::uintptr_t system{};
    std::uintptr_t anchorBody{};
    std::uint64_t pointEpoch{};
    std::uintptr_t rootToken{};
    ShelterComponent component{};
    float exposure{1.f};
    bool sampled{};
};
struct AreaScanDiagnostic {
    std::uintptr_t system{};
    std::uint32_t referenceFormID{};
    std::uint32_t bodyCount{};
    std::uint32_t measuredBodies{};
    std::uint32_t geometryMeshes{};
    std::uint32_t skinnedMeshes{};
    std::uint32_t rootScopedMeshes{};
    std::uint32_t exactSkinMeshes{};
    std::uint32_t nameOnlySkinMeshes{};
    std::uint32_t exactPartitions{};
    std::uint32_t readablePartitions{};
    std::uint32_t triangles{};
    bool fromCache{};
};
struct SceneResult {
    std::vector<AreaScanDiagnostic> areaDiagnostics;
    std::vector<BodyBinding> bodies;
    std::vector<ShelterBinding> shelters;
    std::vector<SelfShelterBinding> selfShelters;
};
struct SceneOptions {
    float maximumBodyNodeDistance{180.0f};
    float shelterNoWindDistance{50.0f};
    float shelterMaxWindDistance{2000.0f};
    float selfShelterReach{150.f};
};
// Thread-safe flag; the next game-thread scan clears all prior-save area
// cache tokens before touching any live scene graph.
void RequestSceneCacheReset() noexcept;
SceneResult ScanScene(std::span<const BodyRequest> bodies,
                     std::span<const ShelterRequest> shelter,
                     std::span<const SelfShelterRequest> selfShelter,
                     SceneOptions settings);
} // namespace osfsmp
