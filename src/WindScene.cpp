// Skyrim-side system-specific weighted NIF surface area + independent,
// game-thread-only ambient/transient Havok shelter probes.
// GPL-3.0-or-later WITH LicenseRef-Modding-Exception; see third_party notices.
#include "WindScene.h"
#include "WindArea.h"
#include "WindShelter.h"
#include "SelfShelter.h"
#include "OpenShaders/CSinterface001.h"
#include <RE/Skyrim.h>
#include <Windows.h>  // Guard reads from incomplete/detaching skin instances.
#include <algorithm>
#include <atomic>
#include <bit>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <limits>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace osfsmp {
namespace {
// Called only on the main thread. The fingerprint includes geometry and buffer
// identities and the exact currently matched owner set, so equip changes and
// physics resets invalidate cached skin areas without parsing all triangles
// every shelter update.
struct AreaCache {
    std::uintptr_t root{};
    std::uint64_t fingerprint{};
    std::unordered_map<std::uintptr_t,float> byBody;
};
std::unordered_map<std::uintptr_t,AreaCache> g_areaCache;
std::atomic_bool g_cacheResetRequested{false};
void HashMix(std::uint64_t& seed, std::uint64_t value) noexcept {
    seed ^= value + 0x9e3779b97f4a7c15ull + (seed<<6) + (seed>>2);
}

// Skin instances can be incomplete while actors are attaching/detaching.
// Keep SEH in leaf helpers with no C++ objects requiring stack unwinding.
// A failed snapshot is never used to attribute NIF area to an FSMP bone.
bool TryGetSkinBoneCount(const RE::NiSkinInstance* skin, std::uint32_t& count) noexcept {
    count=0;
    __try {
        if(!skin || !skin->skinData.get() || !skin->bones)return false;
        count=skin->skinData->GetBoneCount();
        return count>0 && count<=4096;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        count=0;
        return false;
    }
}
bool TryCopySkinBones(const RE::NiSkinInstance* skin,
    RE::NiAVObject** destination, std::uint32_t count) noexcept
{
    __try {
        if(!skin || !skin->bones || !destination || count==0 || count>4096)return false;
        std::memcpy(destination,skin->bones,static_cast<std::size_t>(count)*sizeof(*destination));
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool SnapshotSkinBones(const RE::NiSkinInstance* skin,
    std::vector<RE::NiAVObject*>& bones)
{
    std::uint32_t count{};
    if(!TryGetSkinBoneCount(skin,count))return false;
    bones.resize(count);
    if(TryCopySkinBones(skin,bones.data(),count))return true;
    bones.clear();
    return false;
}

// Preserve authoritative bone name verification without constructing an
// unbounded std::string from a potentially invalid BSFixedString pointer.
// The diagnostic-only path no longer reads unrelated skin bone names at all.
bool SafeNodeNameEquals(const RE::NiAVObject* node, std::string_view expected) noexcept {
    if(!node || expected.empty() || expected.size()>191)return false;
    __try {
        const char* text=node->name.c_str();
        if(!text || reinterpret_cast<std::uintptr_t>(text)<0x10000)return false;
        for(std::size_t i=0;i<expected.size();++i){
            if(text[i]!=expected[i])return false;
        }
        return text[expected.size()]=='\0';
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
std::uint64_t AreaFingerprint(RE::NiAVObject* root,
    const std::unordered_map<std::string,std::size_t>& names, std::span<const BodyRequest> bodies)
{
    std::uint64_t hash=0x0a15c0deULL;
    std::vector<std::pair<std::string,std::size_t>> sorted(names.begin(),names.end());
    std::sort(sorted.begin(),sorted.end());
    for(const auto& [name,index]:sorted){
        HashMix(hash,std::hash<std::string>{}(name));
        HashMix(hash,bodies[index].body);
        HashMix(hash,bodies[index].owner);
        // Same name/body with a different validated NiNode must miss the cache.
        HashMix(hash,reinterpret_cast<std::uintptr_t>(bodies[index].boneNode.get()));
    }
    // Reuse one snapshot buffer across geometries to avoid per-mesh allocations.
    std::vector<RE::NiAVObject*> skinBones;
    RE::BSVisit::TraverseScenegraphGeometries(root,[&](RE::BSGeometry* geometry){
        HashMix(hash,reinterpret_cast<std::uintptr_t>(geometry));
        if(geometry){
            // Uniform world-scale changes invalidate rest-area measurements.
            HashMix(hash,std::bit_cast<std::uint32_t>(geometry->world.scale));
            const auto* skin=geometry->GetGeometryRuntimeData().skinInstance.get();
            HashMix(hash,reinterpret_cast<std::uintptr_t>(skin));
            // Skin bindings may change without replacing the geometry or partition.
            // Exact NiNode identity is authoritative for area attribution.
            skinBones.clear();
            if(SnapshotSkinBones(skin,skinBones)){
                HashMix(hash,skinBones.size());
                for(const auto* bone:skinBones)
                    HashMix(hash,reinterpret_cast<std::uintptr_t>(bone));
            }
            const auto* parts=skin?skin->skinPartition.get():nullptr;
            HashMix(hash,reinterpret_cast<std::uintptr_t>(parts));
            if(parts && parts->numPartitions<1024){
                HashMix(hash,parts->numPartitions);
                for(std::uint32_t i=0;i<parts->numPartitions;++i){
                    const auto& p=parts->partitions[i];
                    HashMix(hash,reinterpret_cast<std::uintptr_t>(p.buffData));
                    HashMix(hash,p.buffData?reinterpret_cast<std::uintptr_t>(p.buffData->rawVertexData):0);
                    HashMix(hash,reinterpret_cast<std::uintptr_t>(p.triList));
                    HashMix(hash,(static_cast<std::uint32_t>(p.vertices)<<16)|p.triangles);
                }
            }
        }
        return RE::BSVisit::BSVisitControl::kContinue;
    });
    return hash;
}
static Vec3 ToVec(const RE::NiPoint3& v) noexcept { return {v.x,v.y,v.z}; }
// A node is accepted only if it still belongs to its captured FSMP system
// root. This avoids accidental cross-system bone-name matches.
bool HasAncestor(const RE::NiAVObject* node,const RE::NiAVObject* ancestor){
    for(unsigned depth=0;node && depth<128;++depth,node=node->parent){
        if(node==ancestor)return true;
    }
    return false;
}
RE::TESObjectREFR* FindOwningReference(RE::NiAVObject* root){
    if(!root)return nullptr;
    auto* n=static_cast<RE::NiAVObject*>(root);
    for(unsigned depth=0;n && depth<128;++depth,n=n->parent){
        if(auto* ref=n->GetUserData()){
            auto* attached=ref->Get3D();
            if(attached && HasAncestor(root,attached))return ref;
        }
    }
    return nullptr;
}
// GPUSMPPhysicsNG's first-occluder collector generalized to TESObjectREFR.
// Ignore the object whose cloth is being tested, including world banners.
class OccluderCollector final : public RE::hkpClosestRayHitCollector {
public:
    const RE::TESObjectREFR* ignore{};
    void AddRayHit(const RE::hkpCdBody& body, const RE::hkpShapeRayCastCollectorOutput& hit) override {
        const RE::hkpCdBody* root=&body;
        for(const RE::hkpCdBody* p=body.parent;p;p=p->parent) root=p;
        const auto* ref=RE::TESHavokUtilities::FindCollidableRef(*static_cast<const RE::hkpCollidable*>(root));
        if(ref==ignore)return;
        RE::hkpClosestRayHitCollector::AddRayHit(body,hit);
    }
};
float RayDistance(RE::TESObjectREFR& ref,const RE::NiPoint3& origin,
                  const RE::NiPoint3& destination) {
    const auto* cell=ref.GetParentCell();
    auto* world=cell?cell->GetbhkWorld():nullptr;
    if(!world)return -1.0f;
    const float scale=RE::bhkWorld::GetWorldScale();
    RE::bhkPickData pick{};
    pick.rayInput.from=origin*scale;
    pick.rayInput.to=destination*scale;
    pick.rayInput.enableShapeCollectionFilter=false;
    pick.rayInput.filterInfo.SetCollisionLayer(RE::COL_LAYER::kLOS);
    OccluderCollector collector;
    collector.ignore=&ref;
    pick.closestRayHitCollector=&collector;
    if(!world->PickObject(pick))return -1.0f;
    return pick.rayOutput.hitFraction * (destination-origin).Length();
}
ShelterBinding ProbeShelter(const ShelterRequest& request,const SceneOptions& settings){
    ShelterBinding result{};
    result.system=request.system;
    result.anchorBody=request.anchorBody;
    result.pointBody=request.pointBody;
    result.pointEpoch=request.pointEpoch;
    result.rootToken=reinterpret_cast<std::uintptr_t>(request.systemRoot.get());
    result.component=request.component;
    auto* root=request.systemRoot.get();
    auto* ref=FindOwningReference(root);
    if(!ref || !ref->GetParentCell() || !ref->GetParentCell()->GetbhkWorld()) return result;
    if(!Finite(request.wind)||!Finite(request.origin))return result;
    const float magnitude=std::hypot(request.wind.x,request.wind.y);
    if(magnitude<=1.0e-4f){result.sampled=true;return result;}
    const RE::NiPoint3 origin{request.origin.x,request.origin.y,request.origin.z};
    const float heading=std::atan2(-request.wind.y,-request.wind.x);
    constexpr float deg=std::numbers::pi_v<float>/180.0f;
    std::array<float,3> distances{-1,-1,-1};
    for(std::size_t i=0;i<3;++i){
        const float az=heading+ShelterAzimuth[i]*deg;
        const float el=ShelterElevation[i]*deg;
        const RE::NiPoint3 dir{std::cos(el)*std::cos(az),std::cos(el)*std::sin(az),std::sin(el)};
        distances[i]=RayDistance(*ref,origin,origin+dir*ShelterRayReach);
    }
    result.exposure=ShelterExposure(distances,settings.shelterNoWindDistance,
                                    settings.shelterMaxWindDistance);
    result.sampled=true;
    return result;
}

// Like GPU SMP, use a representative selection of dynamic strand nodes and
// search *toward* the source for the wearer's body shielding the hair. FSMP's
// private collision-shape graph is not ABI-public, so we deliberately avoid
// reading it. Actor-owned Havok contacts plus posed head/neck/shoulder capsules
// are the available conservative approximation; it is not GPU SMP's exact XML
// armor-surface collision mesh. Missing colliders count as EXPOSED.
class SelfOccluderCollector final : public RE::hkpClosestRayHitCollector {
public:
    const RE::Actor* wanted{};
    void AddRayHit(const RE::hkpCdBody& body,
                   const RE::hkpShapeRayCastCollectorOutput& hit) override {
        const RE::hkpCdBody* root=&body;
        for(const RE::hkpCdBody* p=body.parent;p;p=p->parent)root=p;
        const auto* ref=RE::TESHavokUtilities::FindCollidableRef(
            *static_cast<const RE::hkpCollidable*>(root));
        if(ref!=wanted)return;
        RE::hkpClosestRayHitCollector::AddRayHit(body,hit);
    }
};
bool ActorHavokBlocks(RE::Actor& actor,Vec3 origin,Vec3 toward,float reach){
    auto* cell=actor.GetParentCell();
    auto* world=cell?cell->GetbhkWorld():nullptr;
    if(!world || !Finite(origin)||!Finite(toward)||!(reach>0.f))return false;
    const float scale=RE::bhkWorld::GetWorldScale();
    const Vec3 end=origin+toward*reach;
    const RE::NiPoint3 startPoint{origin.x,origin.y,origin.z};
    const RE::NiPoint3 endPoint{end.x,end.y,end.z};
    RE::bhkPickData pick{};
    pick.rayInput.from=startPoint*scale;
    pick.rayInput.to=endPoint*scale;
    pick.rayInput.enableShapeCollectionFilter=false;
    pick.rayInput.filterInfo.SetCollisionLayer(RE::COL_LAYER::kLOS);
    SelfOccluderCollector collector;
    collector.wanted=&actor;
    pick.closestRayHitCollector=&collector;
    if(!world->PickObject(pick))return false;
    // A contact immediately at the start usually means the hair sample is
    // embedded in the owner's collision hull. Do not mistake this for shelter.
    const float distance=pick.rayOutput.hitFraction*reach;
    return std::isfinite(distance) && distance>3.f && distance<=reach;
}
SelfShelterBinding ProbeSelfShelter(const SelfShelterRequest& request,float reach){
    SelfShelterBinding result{};
    result.system=request.system;
    result.anchorBody=request.anchorBody;
    result.pointEpoch=request.pointEpoch;
    result.rootToken=reinterpret_cast<std::uintptr_t>(request.systemRoot.get());
    result.component=request.component;
    auto* ref=FindOwningReference(request.systemRoot.get());
    auto* actor=ref?ref->As<RE::Actor>():nullptr;
    if(!actor || !actor->Get3D() || request.samples.empty() || !Finite(request.wind))return result;
    const float magnitude=Length(request.wind);
    if(!(magnitude>1.e-4f))return result;
    const Vec3 toward=request.wind*(-1.f/magnitude);
    auto* root=actor->Get3D();
    const auto named=[&](const char* name)->RE::NiAVObject* {
        return root->GetObjectByName(RE::BSFixedString(name));
    };
    auto* head=named("NPC Head [Head]");
    auto* neck=named("NPC Neck [Neck]");
    auto* spine=named("NPC Spine2 [Spn2]");
    auto* left=named("NPC L Clavicle [LClv]");
    auto* right=named("NPC R Clavicle [RClv]");
    std::vector<SelfShelterCapsule> capsules;
    std::vector<SelfShelterSphere> spheres;
    const auto pos=[](const RE::NiAVObject* node){return ToVec(node->world.translate);};
    if(head && neck){
        const Vec3 h=pos(head),n=pos(neck);
        const float span=Length(h-n);
        if(Finite(h)&&Finite(n) && span>0.01f && span<80.f){
            const float size=std::clamp(span*0.75f,7.f,14.f);
            spheres.push_back({h,size});
            capsules.push_back({n,h,size*0.65f});
        }
    }
    if(neck && spine){
        const Vec3 n=pos(neck),s=pos(spine);
        const float span=Length(n-s);
        if(Finite(n)&&Finite(s) && span>0.01f && span<100.f)
            capsules.push_back({s,n,std::clamp(span*0.45f,8.f,17.f)});
    }
    const std::array<RE::NiAVObject*,2> shoulders{left,right};
    for(const auto* shoulder:shoulders){
        if(neck && shoulder){
            const Vec3 n=pos(neck),s=pos(shoulder);
            const float span=Length(s-n);
            if(Finite(n)&&Finite(s) && span>0.01f && span<80.f)
                capsules.push_back({n,s,std::clamp(span*0.35f,5.f,11.f)});
        }
    }
    unsigned count=0,exposed=0;
    for(const Vec3& point:request.samples){
        if(!Finite(point))continue;
        ++count;
        const std::array<Vec3,1> one{point};
        const bool analyticBlocked=SelfShelterExposure(one,toward,reach,capsules,spheres)<0.5f;
        if(!analyticBlocked && !ActorHavokBlocks(*actor,point,toward,reach))++exposed;
    }
    if(count){
        result.exposure=static_cast<float>(exposed)/static_cast<float>(count);
        result.sampled=true;
    }
    return result;
}

// GPU SMP's CPU-side card access adapted for a standalone plugin. Counts triangles
// from the actual NiSkinPartition's CPU vertex/index copies, where available.
// The map contains only exact, validated nodes for ONE authoritative FSMP system.
// A matching name on an unrelated accessory/skin is not sufficient attribution.
void CollectSkinAreas(RE::NiAVObject* root, RE::NiAVObject* systemRoot,
    const std::unordered_map<const RE::NiAVObject*,std::size_t>& nodeToBody,
    std::span<float> areas,AreaScanDiagnostic& stats)
{
    if(!root || nodeToBody.empty())return;
    // No diagnostic bone-name comparisons. Only validated NiNode pointer
    // identity may associate skinned geometry with this FSMP system.
    std::vector<RE::NiAVObject*> skinBones;
    RE::BSVisit::TraverseScenegraphGeometries(root,[&](RE::BSGeometry* geometry){
        if(!geometry) return RE::BSVisit::BSVisitControl::kContinue;
        ++stats.geometryMeshes;
        if(systemRoot && HasAncestor(geometry,systemRoot))++stats.rootScopedMeshes;
        auto* skin=geometry->GetGeometryRuntimeData().skinInstance.get();
        skinBones.clear();
        if(!SnapshotSkinBones(skin,skinBones))
            return RE::BSVisit::BSVisitControl::kContinue;
        ++stats.skinnedMeshes;
        const auto boneCount=static_cast<std::uint32_t>(skinBones.size());
        // Do not dereference unrelated skin nodes (or their names). A snapshot
        // might include invalid pointers while an NPC is being attached.
        bool belongsToSystem=false;
        for(const auto* boneNode:skinBones){
            if(boneNode && nodeToBody.contains(boneNode)){
                belongsToSystem=true;
                break;
            }
        }
        if(belongsToSystem)++stats.exactSkinMeshes;
        if(!belongsToSystem)return RE::BSVisit::BSVisitControl::kContinue;
        auto* parts=skin->skinPartition.get();
        if(!parts || parts->numPartitions>1024)
            return RE::BSVisit::BSVisitControl::kContinue;
        for(std::uint32_t p=0;p<parts->numPartitions;++p) {
            const auto& part=parts->partitions[p];
            if(!part.bones || !part.triList || !part.buffData || !part.buffData->rawVertexData ||
                !part.vertices || !part.triangles || part.strips || part.numBones==0 || part.numBones>4096)
                continue;
            bool partitionHasSystemBone=false;
            for(std::uint32_t palette=0;palette<part.numBones;++palette){
                const auto skinBone=part.bones[palette];
                if(skinBone<boneCount && skinBones[skinBone] &&
                   nodeToBody.contains(skinBones[skinBone])){
                    partitionHasSystemBone=true;
                    break;
                }
            }
            if(!partitionHasSystemBone)continue;
            ++stats.exactPartitions;
            auto desc=part.vertexDesc;
            if (!desc.HasFlag(RE::BSGraphics::Vertex::VF_SKINNED) ||
                !desc.HasFlag(RE::BSGraphics::Vertex::VF_VERTEX)) continue;
            const auto stride=desc.GetSize();
            const auto offset=desc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_SKINNING);
            if(stride<12 || stride>1024 || offset+12>stride || part.vertices>60000 || part.triangles>60000) continue;
            ++stats.readablePartitions;
            stats.triangles+=part.triangles;
            std::vector<AreaVertex> vertices(part.vertices);
            for(std::uint16_t v=0;v<part.vertices;++v){
                const auto* raw=part.buffData->rawVertexData+static_cast<std::size_t>(v)*stride;
                float coords[3];std::memcpy(coords,raw,sizeof coords);
                vertices[v].position={coords[0],coords[1],coords[2]};
                const auto* block=raw+offset;
                for(std::size_t slot=0;slot<4;++slot){
                    std::uint16_t half{};std::memcpy(&half,block+slot*2,sizeof half);
                    const auto w=HalfToFloat(half);
                    // GPU SMP normalizes by ALL positive skin weights, even those
                    // mapped to bones that are not part of this physics system.
                    if(!(w>0.0f) || !std::isfinite(w))continue;
                    vertices[v].weight[slot]=w;
                    const auto palette=block[8+slot];
                    if(palette>=part.numBones)continue;
                    const auto skinBone=part.bones[palette];
                    if(skinBone>=boneCount || !skinBones[skinBone])continue;
                    // Exact node identity prevents unrelated geometry that happens
                    // to use the same bone name from contributing surface area.
                    const auto it=nodeToBody.find(skinBones[skinBone]);
                    if(it==nodeToBody.end())continue;
                    vertices[v].rigBone[slot]=static_cast<int>(it->second);
                }
            }
            std::vector<std::uint32_t> indices;
            indices.reserve(static_cast<std::size_t>(part.triangles)*3);
            for(std::uint32_t t=0;t<static_cast<std::uint32_t>(part.triangles)*3;++t)indices.push_back(part.triList[t]);
            AccumulateTriangleAreas(vertices,indices,areas,geometry->world.scale);
        }
        return RE::BSVisit::BSVisitControl::kContinue;
    });
}
} // namespace

void RequestSceneCacheReset() noexcept {
    g_cacheResetRequested.store(true,std::memory_order_release);
}

SceneResult ScanScene(std::span<const BodyRequest> bodies,
    std::span<const ShelterRequest> shelter,
    std::span<const SelfShelterRequest> selfShelter, SceneOptions settings)
{
    // Reset only on the game thread; any queued pre-load task is rejected by
    // its session ticket before reaching this function.
    if(g_cacheResetRequested.exchange(false,std::memory_order_acq_rel))
        g_areaCache.clear();
    SceneResult output;
    output.bodies.reserve(bodies.size());
    output.shelters.reserve(shelter.size());
    output.selfShelters.reserve(selfShelter.size());
    // Independent raycast requests: shelter is not tied to expensive NIF scans.
    for(const auto& request:shelter) output.shelters.push_back(ProbeShelter(request,settings));
    for(const auto& request:selfShelter)
        output.selfShelters.push_back(ProbeSelfShelter(request,settings.selfShelterReach));
    for(const auto& b:bodies)
        output.bodies.push_back({b.body,b.owner,b.system,b.name,
            reinterpret_cast<std::uintptr_t>(b.boneNode.get()),
            reinterpret_cast<std::uintptr_t>(b.systemRoot.get())});
    if(bodies.empty())return output;

    // Exact system identity partitions the bone-name mapping, rather than
    // scanning *all actors* and guessing owners from spatial proximity.
    std::unordered_map<std::uintptr_t,std::vector<std::size_t>> groups;
    for(std::size_t i=0;i<bodies.size();++i)groups[bodies[i].system].push_back(i);
    std::vector<float> measured(bodies.size());
    for(auto& [id,indices]:groups){
        if(!id || indices.empty())continue;
        auto* root=bodies[indices.front()].systemRoot.get();
        auto* ref=FindOwningReference(root);
        if(!root || !ref)continue; // No trustworthy scene-graph ownership.
        auto* geometryRoot=ref->Get3D();
        if(!geometryRoot || !HasAncestor(root,geometryRoot))continue;
        const bool worldObject=ref->As<RE::Actor>()==nullptr;
        std::unordered_map<std::string,std::size_t> byName;
        std::unordered_set<std::string> duplicates;
        std::vector<std::size_t> selected;
        for(const auto index:indices){
            const auto& b=bodies[index];
            auto* boneNode=b.boneNode.get();
            if(!boneNode || b.name.empty() || !HasAncestor(boneNode,root) ||
               !SafeNodeNameEquals(boneNode,b.name))continue;
            const float distance=Length(ToVec(boneNode->world.translate)-b.worldOrigin);
            if(!std::isfinite(distance)||distance>settings.maximumBodyNodeDistance)continue;
            if(duplicates.contains(b.name))continue;
            const auto [it,inserted]=byName.emplace(b.name,index);
            if(!inserted){
                output.bodies[it->second].nodeMatched=false;
                byName.erase(it); duplicates.insert(b.name);
                continue;
            }
            output.bodies[index].nodeMatched=true;
            output.bodies[index].referenceFormID=ref->GetFormID();
            output.bodies[index].allowWorldFallback=worldObject;
            selected.push_back(index);
        }
        // Duplicate-name systems are fail-closed for affected names.
        for(const auto idx:selected){
            if(duplicates.contains(bodies[idx].name))output.bodies[idx].nodeMatched=false;
        }
        if(byName.empty())continue;
        // Build an exact skin-bone -> FSMP body map. Names alone are insufficient
        // because other equipped meshes may reuse identical bone names.
        std::unordered_map<const RE::NiAVObject*,std::size_t> byNode;
        std::unordered_set<const RE::NiAVObject*> ambiguousNodes;
        for(const auto& [name,index]:byName){
            const auto* node=bodies[index].boneNode.get();
            if(!node || ambiguousNodes.contains(node)){
                output.bodies[index].nodeMatched=false;
                continue;
            }
            const auto [it,inserted]=byNode.emplace(node,index);
            if(!inserted){
                // Two bodies bound to one scene node: neither is safely attributable.
                output.bodies[it->second].nodeMatched=false;
                output.bodies[index].nodeMatched=false;
                byNode.erase(it);
                ambiguousNodes.insert(node);
            }
        }
        if(byNode.empty())continue;
        // Build stable axes from the NIF bone hierarchy, rather than the
        // position of dynamically displaced Bullet bodies at the next rebuild.
        // Prefer an actual descendant in the SAME authoritative FSMP system;
        // tips use the nearest valid parent. Unavailable/nonfinite paths retain
        // the original constrained-Bullet fallback from Plugin.cpp.
        std::unordered_map<std::size_t,std::size_t> parentOf;
        std::unordered_map<std::size_t,std::size_t> firstChild;
        for(const auto& [node,index]:byNode){
            for(auto* ancestor=node->parent;ancestor;ancestor=ancestor->parent){
                const auto it=byNode.find(ancestor);
                if(it!=byNode.end()){
                    parentOf[index]=it->second;
                    const auto existing=firstChild.find(it->second);
                    if(existing==firstChild.end() || index<existing->second)
                        firstChild[it->second]=index;
                    break;
                }
                if(ancestor==root)break;
            }
        }
        for(const auto& [node,index]:byNode){
            Vec3 a=ToVec(node->world.translate),segment{};
            if(auto c=firstChild.find(index);c!=firstChild.end()){
                auto* child=bodies[c->second].boneNode.get();
                if(child)segment=ToVec(child->world.translate)-a;
            }else if(auto par=parentOf.find(index);par!=parentOf.end()){
                auto* parent=bodies[par->second].boneNode.get();
                if(parent)segment=a-ToVec(parent->world.translate);
            }
            const float length=Length(segment);
            if(Finite(a)&&Finite(segment)&&std::isfinite(length)&&length>0.01f&&length<=10000.f){
                output.bodies[index].skeletonAxisWorld=segment/length;
                output.bodies[index].skeletonSegmentLength=length;
                output.bodies[index].skeletonAxisMeasured=true;
            }
        }
        AreaScanDiagnostic stats{};
        stats.system=id;
        stats.referenceFormID=ref->GetFormID();
        stats.bodyCount=static_cast<std::uint32_t>(byNode.size());
        const auto fingerprint=AreaFingerprint(geometryRoot,byName,bodies);
        auto found=g_areaCache.find(id);
        const bool refresh=std::any_of(indices.begin(),indices.end(),[&](std::size_t idx){
            return bodies[idx].refreshArea;
        });
        if(!refresh && found!=g_areaCache.end() && found->second.root==reinterpret_cast<std::uintptr_t>(geometryRoot) &&
           found->second.fingerprint==fingerprint){
            stats.fromCache=true;
            for(const auto idx:indices){
                const auto it=found->second.byBody.find(bodies[idx].body);
                if(it!=found->second.byBody.end())measured[idx]=it->second;
            }
        } else {
            CollectSkinAreas(geometryRoot,root,byNode,measured,stats);
            AreaCache cache;
            cache.root=reinterpret_cast<std::uintptr_t>(geometryRoot);
            cache.fingerprint=fingerprint;
            for(const auto idx:indices)cache.byBody[bodies[idx].body]=measured[idx];
            g_areaCache.insert_or_assign(id,std::move(cache));
            if(g_areaCache.size()>256)g_areaCache.clear();
        }
        for(const auto idx:indices){
            if(!output.bodies[idx].nodeMatched)continue;
            output.bodies[idx].measuredArea=measured[idx];
            output.bodies[idx].areaMeasured=measured[idx]>0.0f;
            if(output.bodies[idx].areaMeasured)++stats.measuredBodies;
        }
        if(!stats.fromCache && stats.bodyCount && !stats.measuredBodies)
            output.areaDiagnostics.push_back(stats);
    }
    return output;
}
} // namespace osfsmp
