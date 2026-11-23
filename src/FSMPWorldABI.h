#pragma once
// Matching-PDB-validated, read-only FSMP private ABI bridge. Accesses occur inside
// FSMP's PreStep callback, when FSMP owns the simulation lock.
#include <RE/Skyrim.h>
#include <btBulletDynamicsCommon.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace osfsmp {
struct PhysicsBone {
    const void* owner{};
    btRigidBody* body{};
    RE::NiNode* node{};
};
struct PhysicsSystem {
    std::uintptr_t identity{};
    RE::NiNode* root{};
    btRigidBody* anchor{};
    std::vector<PhysicsBone> bones;
};
// Requires a PDB matching the installed hdtsmp64.dll and validated layouts.
// When nonnull, diagnostics receives the accepted RSDS identity/getter RVA.
bool InitializeWorldABI(std::string& failure, std::string* diagnostics = nullptr);
// Snapshot diagnostics: non-system Bullet bodies and systems with no verified
// skeleton root are excluded, while malformed owned structures fail closed. All fields are per-call, no globals.
struct SystemSnapshotReport {
    const char* stage{"ok"};
    std::size_t candidates{};
    std::size_t systems{};
    std::size_t registered{};
    std::size_t unmatched{};
    std::size_t rootlessSystems{}; // never treated as valid SkyrimSystem instances
    std::uintptr_t firstRootless{};
    std::uintptr_t suspect{};
};
// Enumerate authoritative FSMP systems, never assigning an unowned Bullet body
// to a guessed system. Rootless systems (possibly base SkinnedMeshSystem
// instances) are excluded entirely. Invalid records of accepted systems reject
// the snapshot.
bool ReadActiveSystems(const std::unordered_set<btRigidBody*>& candidates,
                       std::vector<PhysicsSystem>& out,
                       SystemSnapshotReport* report = nullptr) noexcept;
} // namespace osfsmp
