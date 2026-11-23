// OpenShadersFSMPWind 0.3.4: Open Shaders wind -> FSMP Bullet with versioned ABI,
// measured NIF skin areas, shelter rays, and native-transient-aware selection.
// GPL-3.0-or-later; see third_party/LICENSE-THIRD-PARTY.md.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <Windows.h>
#include <winver.h>
#include <btBulletDynamicsCommon.h>
#include <BulletDynamics/ConstraintSolver/btTypedConstraint.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <vector>
#include <spdlog/spdlog.h>
#include "FSMP/PluginAPI.h"
#include "OpenShaders/CSinterface001.h"
#include "WindMath.h"
#include "WindArea.h"
#include "WindScene.h"
#include "WindShelter.h"
#include "FSMPWorldABI.h"

namespace logger=SKSE::log;
using namespace std::literals;
SKSEPluginInfo(
    .Version = {0, 3, 4, 0},
    .Name = "OpenShadersFSMPWind",
    .Author = "OpenShadersFSMPWind contributors",
    .RuntimeCompatibility = SKSE::PluginDeclaration::RuntimeCompatibility(SKSE::RUNTIME_SSE_1_7_104)
);
namespace {
struct Settings {
    bool enabled{true};
    float strength{6.f};
    float maxSpeed{900.f};
    float maxWindAcceleration{400.f};
    float width{3.f};
    bool includeNative{false}; // Default matches GPUSMPPhysicsNG: do not duplicate native Havok effects.
    bool enableShelter{true};
    bool allowFallback{true}; // GPU SMP-style segment fallback, validated system nodes only.
    bool enableSelfShelter{true};
    float selfShelterReach{150.f};
    float maxBoneNodeDistance{180.f};
    float geometryScanSeconds{3.0f};
    float shelterInterval{0.5f};
    float shelterNoWindDistance{50.f};
    float shelterMaxWindDistance{2000.f};
    float shelterThreePointDistance{300.f};
    float shelterFivePointDistance{900.f};
    float transientThreshold{0.02f};
    bool allowWorldFallback{true};
    int logEvery{600};
    // Read-only per-bone force-accumulator diagnostic, rate-limited by default.
    bool logAreaDiagnostics{true};
    bool logBoneForces{true};
    int boneForceLogEvery{600};
    int boneForceLogMaxEntries{64}; // 0 logs every evaluated bone.
};
Settings g_settings{};
std::string ConfigPath(){
    return (std::filesystem::current_path()/"Data"/"SKSE"/"Plugins"/"OpenShadersFSMPWind.ini").string();
}
int ReadInt(const char* key,int fallback){const auto p=ConfigPath();return static_cast<int>(GetPrivateProfileIntA("Wind",key,fallback,p.c_str()));}
float ReadFloat(const char* key,float fallback,float min,float max){
    char buf[80]{};const auto p=ConfigPath();
    GetPrivateProfileStringA("Wind",key,"",buf,sizeof buf,p.c_str());
    if(!buf[0])return fallback;
    char* end=nullptr;float val=std::strtof(buf,&end);
    return end==buf||!std::isfinite(val)?fallback:std::clamp(val,min,max);
}
void LoadSettings(){
    g_settings.enabled=ReadInt("Enabled",1)!=0;
    g_settings.strength=ReadFloat("Strength",6.f,0.f,100.f);
    g_settings.maxSpeed=ReadFloat("MaxSpeed",900.f,0.f,10000.f);
    g_settings.maxWindAcceleration=ReadFloat("MaxWindAcceleration",400.f,0.f,100000.f);
    g_settings.width=ReadFloat("DefaultSegmentWidth",3.f,1.f,40.f);
    g_settings.includeNative=ReadInt("IncludeNativeHavokImpulses",0)!=0;
    g_settings.enableShelter=ReadInt("EnableWorldShelter",1)!=0;
    g_settings.allowFallback=ReadInt("AllowUnmeasuredAreaFallback",1)!=0;
    g_settings.enableSelfShelter=ReadInt("EnableSelfShelter",1)!=0;
    g_settings.selfShelterReach=ReadFloat("SelfShelterReach",150.f,20.f,500.f);
    g_settings.maxBoneNodeDistance=ReadFloat("MaxBoneNodeDistance",180.f,25.f,600.f);
    g_settings.geometryScanSeconds=ReadFloat("GeometryRescanSeconds",3.0f,0.5f,30.0f);
    g_settings.shelterInterval=ReadFloat("ShelterIntervalSeconds",0.5f,0.1f,2.0f);
    g_settings.shelterNoWindDistance=ReadFloat("ShelterNoWindDistance",50.f,0.f,1000.f);
    g_settings.shelterMaxWindDistance=ReadFloat("ShelterMaxWindDistance",2000.f,100.f,3000.f);
    g_settings.shelterMaxWindDistance=std::max(g_settings.shelterMaxWindDistance,g_settings.shelterNoWindDistance+1.f);
    g_settings.shelterThreePointDistance=ReadFloat("ShelterThreePointDistance",300.f,50.f,5000.f);
    g_settings.shelterFivePointDistance=ReadFloat("ShelterFivePointDistance",900.f,100.f,10000.f);
    g_settings.shelterFivePointDistance=std::max(g_settings.shelterFivePointDistance,g_settings.shelterThreePointDistance+1.f);
    g_settings.transientThreshold=ReadFloat("TransientThreshold",0.02f,0.001f,1.0f);
    g_settings.allowWorldFallback=ReadInt("AllowUnmeasuredWorldAreaFallback",1)!=0;
    g_settings.logEvery=std::clamp(ReadInt("LogEveryNFrames",600),0,1000000);
    g_settings.logAreaDiagnostics=ReadInt("LogMissingAreaDiagnostics",1)!=0;
    g_settings.logBoneForces=ReadInt("LogBoneForces",1)!=0;
    g_settings.boneForceLogEvery=std::clamp(ReadInt("BoneForceLogEveryNFrames",600),1,1000000);
    g_settings.boneForceLogMaxEntries=std::clamp(ReadInt("BoneForceLogMaxEntries",64),0,16384);
}

// Compile-time layout contract for the FSMP private SkinnedMeshBone prefix.
// The runtime matching-PDB bridge validates these offsets and Bullet's size
// again against the installed DLL. These fields remain private/unsupported;
// unknown binaries are accepted ONLY if the required ABI actually matches.
struct alignas(16) FSMPBoneVerifiedLayout : RE::BSIntrusiveRefCounted {
    virtual ~FSMPBoneVerifiedLayout();
    RE::BSFixedString name;
    float marginMultiplier;
    float boundingSphereMultiplier;
    float gravityFactor;
    float windFactor;
    alignas(btRigidBody) std::byte bodyStorage[sizeof(btRigidBody)];
};
static_assert(alignof(FSMPBoneVerifiedLayout)>=16);
static_assert(sizeof(RE::BSFixedString)==8);
// Hard stops if the user's Bullet headers/flags don't reproduce THIS binary's layout.
static_assert(offsetof(FSMPBoneVerifiedLayout,name)==0x18, "FSMP bone name offset mismatch");
static_assert(offsetof(FSMPBoneVerifiedLayout,windFactor)==0x2C, "FSMP wind-factor offset mismatch");
static_assert(offsetof(FSMPBoneVerifiedLayout,bodyStorage)==0x30, "FSMP rigid-body offset mismatch");
static_assert(sizeof(btRigidBody)==0x310, "Bullet ABI incompatible with verified FSMP PDB");
// SEH isolation is required: an obsolete mod might keep a stale userPointer.
bool SafeRead(void* dst,const void* src,std::size_t n) noexcept {
    __try {std::memcpy(dst,src,n);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool GetFSMPFileVersion(std::uint16_t& major,std::uint16_t& minor,std::uint16_t& patch){
    auto* mod=GetModuleHandleA("hdtSMP64.dll");
    if(!mod)return false;
    char path[MAX_PATH]{};
    if(!GetModuleFileNameA(mod,path,MAX_PATH))return false;
    const auto size=GetFileVersionInfoSizeA(path,nullptr);
    if(!size||size>1024*1024)return false;
    std::vector<std::byte> data(size);
    if(!GetFileVersionInfoA(path,0,size,data.data()))return false;
    UINT len{};void* ptr{};
    if(!VerQueryValueA(data.data(),"\\",&ptr,&len)||!ptr||len<sizeof(VS_FIXEDFILEINFO))return false;
    const auto* info=static_cast<const VS_FIXEDFILEINFO*>(ptr);
    if(info->dwSignature!=0xFEEF04BD)return false;
    major=HIWORD(info->dwFileVersionMS);
    minor=LOWORD(info->dwFileVersionMS);
    patch=HIWORD(info->dwFileVersionLS);
    return true;
}

// There is deliberately no DLL hash, version, or hardcoded RSDS allowlist.
// InitializeWorldABI validates the matching local PDB, every dereferenced
// private-field offset, and Bullet's exact btRigidBody size before use.
bool SafeBoneName(const void* owner,std::string& name){
    const char* raw{};
    if(!SafeRead(&raw,reinterpret_cast<const std::byte*>(owner)+offsetof(FSMPBoneVerifiedLayout,name),sizeof raw)||!raw)
        return false;
    char temp[192]{};
    bool terminated=false;
    for(std::size_t i=0;i<sizeof temp;++i){
        char ch{};
        if(!SafeRead(&ch,raw+i,1))return false;
        if(!ch){terminated=i>0;break;}
        if(static_cast<unsigned char>(ch)<32)return false;
        temp[i]=ch;
    }
    if(!terminated)return false;
    name=temp;
    return true;
}
bool ReadOwner(const btRigidBody* body,float& factor,const void*& ownerOut,std::string* name=nullptr){
    ownerOut=nullptr;
    const auto* owner=body?body->getUserPointer():nullptr;
    if(!owner)return false;
    const auto base=reinterpret_cast<std::uintptr_t>(owner);
    const auto obj=reinterpret_cast<std::uintptr_t>(body);
    if(base%16 || obj<base || obj-base!=offsetof(FSMPBoneVerifiedLayout,bodyStorage))return false;
    float f{};
    if(!SafeRead(&f,reinterpret_cast<const std::byte*>(owner)+offsetof(FSMPBoneVerifiedLayout,windFactor),sizeof f))return false;
    if(!std::isfinite(f)||f<0.f||f>10000.f)return false;
    if(name && !SafeBoneName(owner,*name))return false;
    factor=f;ownerOut=owner;
    return true;
}
osfsmp::Vec3 V(const btVector3& x){return {x.x(),x.y(),x.z()};}
btVector3 B(osfsmp::Vec3 v){return {v.x,v.y,v.z};}
bool IsSMPBody(const btCollisionObject* obj){
    if(!obj)return false;
    const auto* proxy=obj->getBroadphaseHandle();
    return proxy && proxy->m_collisionFilterGroup==0 && proxy->m_collisionFilterMask==0;
}
bool IsCommunityShadersExpanded(){
    auto mod=GetModuleHandleA("CommunityShaders.dll");if(!mod)return false;
    char path[MAX_PATH]{};
    if(!GetModuleFileNameA(mod,path,MAX_PATH))return false;
    const auto size=GetFileVersionInfoSizeA(path,nullptr);
    if(!size||size>1048576)return false;
    std::vector<char> data(size);UINT len{};void* value{};
    if(!GetFileVersionInfoA(path,0,size,data.data())||
       !VerQueryValueA(data.data(),"\\StringFileInfo\\040904b0\\FileVersion",&value,&len)||!value)
       return false;
    return std::string_view(static_cast<const char*>(value)).find("CSX")!=std::string_view::npos;
}

struct BodyMetadata {
    btRigidBody* body{};
    const void* owner{};
    std::string name;
    RE::NiPointer<RE::NiNode> node;
    std::uintptr_t system{};
    float boneFactor{};
    int refs{};
    int parent{-1};
    int firstChild{-1};
    float segmentLength{};
    osfsmp::Vec3 localAxis{0,0,-1};
    bool matched{};
    bool axisFromSkeleton{};
    bool areaMeasured{};
    bool worldObject{};
    float measuredArea{};
    std::uint32_t reference{};
};
// A snapshot of ONLY this plugin's contribution to a Bullet body's force
// accumulator. The delta isolates applyCentralForce() from gravity or other
// listeners that have already added forces during this PreStep event.
struct BoneForceDiagnostic {
    std::uintptr_t body{};
    std::uintptr_t system{};
    std::uint32_t reference{};
    std::string name;
    bool worldObject{};
    osfsmp::Vec3 wind{};
    osfsmp::Vec3 requested{};
    osfsmp::Vec3 accumulated{};
    osfsmp::Vec3 totalBefore{};
    osfsmp::Vec3 totalAfter{};
    osfsmp::Vec3 linearFactor{};
    float area{};
    float mass{};
    float boneFactor{};
    float ambientExposure{};
    float transientExposure{};
    float selfAmbientExposure{1.f};
    float selfTransientExposure{1.f};
    bool skeletonAxis{};
    bool areaFallback{};
};

struct ShelterPointState {
    btRigidBody* body{};
    float coordinate{};
    osfsmp::Vec3 origin{};
    bool validOrigin{true};
    osfsmp::Vec3 ambientWind{}, transientWind{};
    float ambientExposure{1.f},ambientTarget{1.f};
    float transientExposure{1.f},transientTarget{1.f};
    bool ambientMeasured{},transientMeasured{};
    bool transientActive{},transientDirty{true};
    float transientLastMagnitude{};
    osfsmp::Vec3 transientLastDirection{};
};
struct SystemMetadata {
    std::uintptr_t identity{};
    RE::NiPointer<RE::NiNode> root;
    btRigidBody* anchor{};
    bool anchorIsKinematic{};
    unsigned shelterAxis{2};
    float shelterExtent{};
    std::uint64_t pointEpoch{1};
    std::vector<ShelterPointState> points;
    float nextAmbientAt{},nextTransientAt{};
    float nextSelfAmbientAt{},nextSelfTransientAt{};
    float selfAmbientExposure{1.f},selfAmbientTarget{1.f};
    float selfTransientExposure{1.f},selfTransientTarget{1.f};
    bool selfAmbientMeasured{},selfTransientMeasured{};
};

class WindAdapter final : public hdt::IPreStepListener {
public:
    void SetFSMP(hdt::PluginInterface* api){
        if(!api || smp_)return;
        const auto info=api->getVersionInfo();
        if(info.interfaceVersion.major!=2 ||
            info.bulletVersion.major!=hdt::PluginInterface::BULLET_VERSION.major ||
            info.bulletVersion.minor!=hdt::PluginInterface::BULLET_VERSION.minor ||
            info.bulletVersion.patch!=hdt::PluginInterface::BULLET_VERSION.patch){
            logger::error("FSMP public interface or Bullet ABI mismatch; disabled");return;
        }
        std::string error, abiDetails;
        if(!osfsmp::InitializeWorldABI(error, &abiDetails)){
            logger::error("FSMP private system ABI unavailable: {}",error);return;
        }
        std::uint16_t major{}, minor{}, patch{};
        if (GetFSMPFileVersion(major, minor, patch)) {
            logger::info("FSMP PDB-verified ABI compatible: FileVersion={}.{}.{}, {}",
                         major, minor, patch, abiDetails);
        } else {
            // FILEVERSION is advisory: a compatible rebuild need not keep it.
            logger::info("FSMP PDB-verified ABI compatible (no FILEVERSION), {}", abiDetails);
        }
        smp_=api;
        smp_->addListener(this);
        logger::info("FSMP system-level private ABI ready; subscribed to PreStep");
    }
    void ConnectShaders(){
        if(!g_settings.enabled)return;
        if(IsCommunityShadersExpanded()){
            logger::error("Detected incompatible Community Shaders Expanded; wind disabled");return;
        }
        auto* messaging=SKSE::GetMessagingInterface();
        CSPluginAPI::CSMessage msg{};
        if(!messaging || !messaging->Dispatch(CSPluginAPI::CSInterfaceMessageType,
                &msg,sizeof msg,CSPluginAPI::CSPluginName) || !msg.GetApiFunction){
            logger::error("Open Shaders revision-4 wind API unavailable");return;
        }
        auto* iface=static_cast<CSPluginAPI::ICSInterface001*>(msg.GetApiFunction(4));
        if(!iface || iface->getBuildNumber()!=9){
            logger::error("Open Shaders API revision/build mismatch");return;
        }
        wind_.store(iface,std::memory_order_release);
        logger::info("Open Shaders rev4 wind connected; IncludeNativeHavokImpulses={}",g_settings.includeNative);
    }
    void Shutdown(){
        wind_.store(nullptr,std::memory_order_release);
        if(smp_){smp_->removeListener(this);smp_=nullptr;}
    }
    // SKSE save transitions run on a different lifecycle than FSMP's physics
    // callback. Gate work atomically here; release NiPointers and rebuild all
    // private-ABI-backed metadata on the first safe PreStep after load.
    void OnPreLoadGame(){
        sessionPaused_.store(true,std::memory_order_release);
        sceneEpoch_.fetch_add(1,std::memory_order_acq_rel);
        resetRequired_.store(true,std::memory_order_release);
        osfsmp::RequestSceneCacheReset();
        logger::info("Save transition: suspending wind and invalidating queued scene scans");
    }
    void OnPostLoadGame(){
        resetRequired_.store(true,std::memory_order_release);
        sessionPaused_.store(false,std::memory_order_release);
        logger::info("Save transition: wind will rebuild authoritative systems on next PreStep");
    }
    void OnNewGame(){
        OnPreLoadGame();
        OnPostLoadGame();
    }
    RE::BSEventNotifyControl ProcessEvent(const hdt::PreStepEvent* event,
        RE::BSTEventSource<hdt::PreStepEvent>*) override
    {
        if(!event || !smp_ || !g_settings.enabled ||
           sessionPaused_.load(std::memory_order_acquire))
            return RE::BSEventNotifyControl::kContinue;
        const auto entryEpoch=sceneEpoch_.load(std::memory_order_acquire);
        if(resetRequired_.exchange(false,std::memory_order_acq_rel)) ResetRuntimeState();
        if(entryEpoch!=sceneEpoch_.load(std::memory_order_acquire) ||
           sessionPaused_.load(std::memory_order_acquire))
            return RE::BSEventNotifyControl::kContinue;
        auto* shaders=wind_.load(std::memory_order_acquire);
        if(!shaders)return RE::BSEventNotifyControl::kContinue;
        const float dt=std::isfinite(event->timeStep)?std::clamp(event->timeStep,0.f,0.1f):0.f;
        clock_+=dt;
        if(clock_>86400.f) { // Prevent unbounded float precision loss after long sessions.
            clock_-=43200.f;
            nextGeometryScanAt_-=43200.f;
            for(auto& [id,g]:systems_){g.nextAmbientAt-=43200.f;g.nextTransientAt-=43200.f;}
        }
        sampledThisStep_=0;appliedThisStep_=0;maxBaseResponse_=0;maxAppliedForce_=0;
        std::vector<btRigidBody*> candidates;
        candidates.reserve(event->objects.size());
        for(int i=0;i<event->objects.size();++i){
            auto* obj=event->objects[i];
            if(!IsSMPBody(obj))continue;
            if(auto* b=btRigidBody::upcast(obj))candidates.push_back(b);
        }
        // Some collision objects share FSMP's broadphase filter but are not
        // registered SkinnedMeshSystem bones. Track those as ignored, rather
        // than forcing a rebuild on every physics step.
        bool dirty=candidates.size()!=meta_.size()+ignoredBodies_.size();
        if(!dirty){
            for(auto* b:candidates){
                if (ignoredBodies_.contains(b)) continue;
                const auto it=index_.find(b);
                if(it==index_.end() || it->second>=meta_.size() ||
                   meta_[it->second].owner!=b->getUserPointer() ||
                   meta_[it->second].refs!=b->getNumConstraintRefs()){
                    dirty=true;break;
                }
                if(frames_%120==0){
                    std::string liveName;
                    if(!SafeBoneName(b->getUserPointer(),liveName) || liveName!=meta_[it->second].name){
                        dirty=true;break;
                    }
                }
            }
        }
        if(dirty || frames_%240==0){
            if(!Rebuild(candidates)){
                // Inconsistent system ownership is unsafe; do not fall back to
                // the old, actor-only constraint graph heuristic.
                meta_.clear();index_.clear();systems_.clear();ignoredBodies_.clear();
                if(frames_%120==0)
                    logger::error("Failed FSMP m_systems snapshot; wind skipped (see stage above)");
                ++frames_;
                return RE::BSEventNotifyControl::kContinue;
            }
            dirty=true;
        }
        if(dirty)geometryDirty_=true;
        ConsumeCompleted();
        UpdateShelterPointLayouts(dirty);
        for(auto& [id,g]:systems_){
            const float t=std::clamp(dt*8.0f,0.f,1.f);
            for(auto& p:g.points){
                p.ambientExposure=std::lerp(p.ambientExposure,p.ambientTarget,t);
                p.transientExposure=std::lerp(p.transientExposure,p.transientTarget,t);
            }
            g.selfAmbientExposure=std::lerp(g.selfAmbientExposure,g.selfAmbientTarget,t);
            g.selfTransientExposure=std::lerp(g.selfTransientExposure,g.selfTransientTarget,t);
        }
        ++frames_;
        const bool logBoneForceStep = g_settings.logBoneForces &&
            frames_ % static_cast<std::uint64_t>(g_settings.boneForceLogEvery) == 0;
        SampleShelterPointWinds(shaders);
        QueueSceneWork(dirty,shaders);

        // Aerodynamic force is still evaluated at every bone. Shelter exposure
        // is interpolated from the system's independently sampled points.
        std::vector<CSPluginAPI::WindVector> positions;
        std::vector<std::size_t> indices;
        positions.reserve(meta_.size());indices.reserve(meta_.size());
        std::vector<BoneForceDiagnostic> boneForceDiagnostics;
        if (logBoneForceStep) boneForceDiagnostics.reserve(meta_.size());
        for(std::size_t i=0;i<meta_.size();++i){
            const auto& m=meta_[i];
            if(!m.owner || !m.matched ||
               (!m.areaMeasured && !g_settings.allowFallback &&
                !(g_settings.allowWorldFallback && m.worldObject)) ||
               m.body->isStaticOrKinematicObject())continue;
            if(!systems_.contains(m.system))continue;
            const auto& g=systems_.at(m.system);
            float current{};const void* owner{};
            if(!ReadOwner(m.body,current,owner) || owner!=m.owner || current<=0.f)continue;
            const auto pos=V(m.body->getWorldTransform().getOrigin());
            positions.push_back({pos.x,pos.y,pos.z});indices.push_back(i);
        }
        constexpr auto batch=CSPluginAPI::CSWindMaximumBatchSize;
        for(std::size_t off=0;off<indices.size();off+=batch){
            const auto n=std::min<std::size_t>(batch,indices.size()-off);
            std::vector<CSPluginAPI::WindSampleWithHavokExclusion> samples(n);
            if(!shaders->SampleWindExcludingHavokImpulses(positions.data()+off,samples.data(),
                    static_cast<std::uint32_t>(n)))continue;
            sampledThisStep_+=n;
            for(std::size_t j=0;j<n;++j){
                const auto& sample=samples[j];
                auto& m=meta_[indices[off+j]];
                auto* body=m.body;
                const auto& g=systems_.at(m.system);
                maxBaseResponse_=std::max(maxBaseResponse_,osfsmp::Length(
                    {sample.wind.baseVelocity.x,sample.wind.baseVelocity.y,sample.wind.baseVelocity.z}));
                // The published no-Havok vector is ambient + wind-only transients.
                // Thus subtraction separates them exactly without approximating
                // their directions from ambient and transient intensity scalars.
                const auto& selected=g_settings.includeNative?sample.wind.finalVelocity:
                                      sample.windExcludingHavokImpulses;
                const auto parts=osfsmp::SeparateWind(ToVec(sample.wind.baseVelocity),
                    ToVec(sample.wind.gustVelocity),ToVec(selected));
                const auto ambient=parts.ambient,transient=parts.transient;
                if(!osfsmp::Finite(ambient)||!osfsmp::Finite(transient))continue;
                // Absence of a LOS sample is not evidence of an occluder.
                // Geometry/owner validation remains fail-closed separately.
                const auto bodyPos=V(body->getWorldTransform().getOrigin());
                const float ambientCover=ShelterAt(g,bodyPos,false);
                const float transientCover=ShelterAt(g,bodyPos,true);
                // GPU SMP multiplies the drag multiplier by exposure instead
                // of slowing wind before a quadratic drag calculation. Keep
                // distinct ambient/transient directions and self shielding.
                const float selfAmbient=g_settings.enableSelfShelter?g.selfAmbientExposure:1.f;
                const float selfTransient=g_settings.enableSelfShelter?g.selfTransientExposure:1.f;
                const auto sheltered=osfsmp::ComposeShelteredWind(
                    ambient,transient,ambientCover*selfAmbient,transientCover*selfTransient,
                    g_settings.strength*300.f,g_settings.maxSpeed);
                const auto wind=sheltered.wind;
                const float invMass=body->getInvMass();
                if(!(invMass>0.f)||!std::isfinite(invMass))continue;
                float factor{};const void* owner{};
                if(!ReadOwner(body,factor,owner) || owner!=m.owner || factor<=0.f)continue;
                const bool useFallback=g_settings.allowFallback||(g_settings.allowWorldFallback&&m.worldObject);
                const float area=osfsmp::EffectiveWindArea(m.measuredArea,m.segmentLength,
                    g_settings.width,useFallback);
                if(!(area>0.f))continue;
                const auto& pose=body->getWorldTransform();
                const auto axis=osfsmp::Normalized(V(pose.getBasis()*B(m.localAxis)));
                const osfsmp::WindInput input{wind,clock_,area,1.f/invMass,
                    factor*sheltered.exposure,
                    axis,V(pose.getOrigin()),V(body->getLinearVelocity()),
                    g_settings.maxWindAcceleration};
                const auto force=osfsmp::ComputeWindForce(input);
                if(osfsmp::Finite(force)){
                    const float magnitude=osfsmp::Length(force);
                    // Read only on diagnostic frames: no extra Bullet work on
                    // the other 599 physics steps with default settings.
                    const osfsmp::Vec3 before=logBoneForceStep?
                        V(body->getTotalForce()):osfsmp::Vec3{};
                    if(magnitude>0.f){
                        body->applyCentralForce(B(force));
                        ++appliedThisStep_;
                        maxAppliedForce_=std::max(maxAppliedForce_,magnitude);
                    }
                    if(logBoneForceStep){
                        const osfsmp::Vec3 after=V(body->getTotalForce());
                        boneForceDiagnostics.push_back({
                            reinterpret_cast<std::uintptr_t>(body),m.system,m.reference,
                            m.name,m.worldObject,wind,force,after-before,before,after,
                            V(body->getLinearFactor()),area,1.f/invMass,factor,
                            ambientCover,transientCover,selfAmbient,selfTransient,
                            m.axisFromSkeleton,!m.areaMeasured
                        });
                    }
                }
            }
        }
        if (logBoneForceStep) LogBoneForces(boneForceDiagnostics);
        if(g_settings.logEvery>0 && frames_%g_settings.logEvery==0)LogSummary();
        return RE::BSEventNotifyControl::kContinue;
    }
private:
    void ResetRuntimeState(){
        // Runs on the FSMP PreStep thread, never in a load-message callback.
        // Late tasks from earlier saves carry a different epoch and cannot
        // clear scanPending_ or publish old results into the new session.
        {
            std::lock_guard lock(resultLock_);
            completed_={};
        }
        index_.clear();
        ignoredBodies_.clear();
        meta_.clear();
        systems_.clear();
        areaDiagnosticLogged_.clear();
        scanPending_.store(false,std::memory_order_release);
        activeScanTicket_.store(0,std::memory_order_release);
        frames_=0;
        probeBatches_=0;
        selfProbeBatches_=0;
        sceneScans_.store(0,std::memory_order_release);
        sampledThisStep_=appliedThisStep_=0;
        maxBaseResponse_=maxAppliedForce_=0.f;
        clock_=0.f;
        nextGeometryScanAt_=0.f;
        geometryDirty_=true;
        rootlessWarningIssued_=false;
        lastRootlessWarningFrame_=0;
        logger::info("Save transition: cleared cached FSMP bodies/systems/shelter and area bindings");
    }
    static osfsmp::Vec3 ToVec(const CSPluginAPI::WindVector& v){return {v.x,v.y,v.z};}
    // Called at rebuilds and periodically to capture movement/rotation of large
    // systems. Only authoritative FSMP bones are eligible for probe positions.
    // Retain per-bone shelter history when the selected body remains the same.
    void UpdateShelterPointLayouts(bool dirty){
        if(!g_settings.enableShelter && !g_settings.enableSelfShelter)return;
        bool missing=false;
        for(const auto& [id,g]:systems_)if(g.points.empty())missing=true;
        if(!dirty && !missing && frames_%120!=0)return;
        std::unordered_map<std::uintptr_t,std::vector<osfsmp::ShelterBonePosition>> groups;
        for(const auto& m:meta_){
            if(!m.body||!m.owner||!m.node)continue;
            const auto point=V(m.body->getWorldTransform().getOrigin());
            if(osfsmp::Finite(point))groups[m.system].push_back({
                reinterpret_cast<std::uintptr_t>(m.body),point});
        }
        for(auto& [id,g]:systems_){
            const auto it=groups.find(id);
            const auto bones=it==groups.end()?std::span<const osfsmp::ShelterBonePosition>{}:
                std::span<const osfsmp::ShelterBonePosition>{it->second};
            const auto selection=osfsmp::SelectShelterPoints(bones,
                reinterpret_cast<std::uintptr_t>(g.anchor),
                g_settings.shelterThreePointDistance,g_settings.shelterFivePointDistance);
            bool changed=g.shelterAxis!=selection.axis ||
                g.points.size()!=selection.points.size();
            if(!changed){
                for(std::size_t i=0;i<g.points.size();++i){
                    if(g.points[i].body!=reinterpret_cast<btRigidBody*>(selection.points[i].body)){
                        changed=true;break;
                    }
                }
            }
            if(changed){
                std::vector<ShelterPointState> replacement;
                replacement.reserve(selection.points.size());
                for(const auto& point:selection.points){
                    ShelterPointState next{};
                    auto* body=reinterpret_cast<btRigidBody*>(point.body);
                    for(const auto& previous:g.points){
                        if(previous.body==body){next=previous;break;}
                    }
                    next.body=body;
                    next.transientDirty=true;
                    replacement.push_back(next);
                }
                g.points=std::move(replacement);
                g.pointEpoch=++nextPointEpoch_;
                g.nextAmbientAt=clock_; // fresh probes with new spatial layout
                g.nextTransientAt=clock_;
            }
            g.shelterAxis=selection.axis;
            g.shelterExtent=selection.extent;
            for(std::size_t i=0;i<selection.points.size();++i){
                g.points[i].origin=selection.points[i].origin;
                g.points[i].validOrigin=true;
                g.points[i].coordinate=osfsmp::ShelterCoordinate(selection.points[i].origin,selection.axis);
            }
        }
    }
    static float ShelterAt(const SystemMetadata& g,osfsmp::Vec3 position,bool transient){
        if(!g_settings.enableShelter||g.points.empty())return 1.f;
        std::array<osfsmp::ShelterExposurePoint,5> values{};
        const auto count=std::min<std::size_t>(g.points.size(),values.size());
        for(std::size_t i=0;i<count;++i){
            const auto& p=g.points[i];
            // Newly created and unavailable probes default to unobstructed.
            values[i]={p.coordinate,transient?p.transientExposure:p.ambientExposure};
        }
        return osfsmp::InterpolateShelterExposure(
            std::span<const osfsmp::ShelterExposurePoint>(values.data(),count),
            osfsmp::ShelterCoordinate(position,g.shelterAxis));
    }
    void SampleShelterPointWinds(CSPluginAPI::ICSInterface001* shaders){
        std::vector<CSPluginAPI::WindVector> positions;
        struct PointID {std::uintptr_t system;std::size_t index;};
        std::vector<PointID> ids;
        for(auto& [id,g]:systems_){
            for(std::size_t i=0;i<g.points.size();++i){
                auto& p=g.points[i];
                if(!p.body)continue;
                const auto current=V(p.body->getWorldTransform().getOrigin());
                p.validOrigin=osfsmp::Finite(current);
                if(p.validOrigin){
                    p.origin=current;
                    p.coordinate=osfsmp::ShelterCoordinate(p.origin,g.shelterAxis);
                }
            }
            std::sort(g.points.begin(),g.points.end(),[](const auto& a,const auto& b){
                return a.coordinate<b.coordinate;
            });
            for(std::size_t i=0;i<g.points.size();++i){
                const auto& p=g.points[i];
                if(!p.validOrigin)continue;
                positions.push_back({p.origin.x,p.origin.y,p.origin.z});
                ids.push_back({id,i});
            }
        }
        constexpr auto batch=CSPluginAPI::CSWindMaximumBatchSize;
        for(std::size_t off=0;off<positions.size();off+=batch){
            const auto n=std::min<std::size_t>(batch,positions.size()-off);
            std::vector<CSPluginAPI::WindSampleWithHavokExclusion> samples(n);
            if(!shaders->SampleWindExcludingHavokImpulses(positions.data()+off,samples.data(),
                    static_cast<std::uint32_t>(n)))continue;
            for(std::size_t j=0;j<n;++j){
                const auto key=ids[off+j];
                auto& p=systems_.at(key.system).points[key.index];
                const auto& s=samples[j];
                const auto& selected=g_settings.includeNative?s.wind.finalVelocity:
                                      s.windExcludingHavokImpulses;
                const auto parts=osfsmp::SeparateWind(ToVec(s.wind.baseVelocity),
                    ToVec(s.wind.gustVelocity),ToVec(selected));
                const auto ambient=parts.ambient,transient=parts.transient;
                if(!osfsmp::Finite(ambient)||!osfsmp::Finite(transient))continue;
                p.ambientWind=ambient;
                p.transientWind=transient;
                const float magnitude=osfsmp::Length(transient);
                const bool active=magnitude>g_settings.transientThreshold;
                if(active){
                    const auto direction=osfsmp::Normalized(transient);
                    if(osfsmp::ShouldReprobeTransient(p.transientActive,
                        p.transientLastDirection,p.transientLastMagnitude,transient,
                        g_settings.transientThreshold))p.transientDirty=true;
                    p.transientLastDirection=direction;
                }else{
                    p.transientDirty=false;
                    p.transientTarget=1.f;
                    p.transientExposure=1.f;
                    p.transientMeasured=false;
                }
                p.transientActive=active;
                p.transientLastMagnitude=magnitude;
            }
        }
        // Do not carry stale self shielding from an old transient into a later
        // unrelated burst, especially when the wind API goes quiet.
        for(auto& [id,g]:systems_){
            if(g.points.empty() ||
               !g.points[g.points.size()/2].transientActive){
                g.selfTransientExposure=1.f;
                g.selfTransientTarget=1.f;
                g.selfTransientMeasured=false;
            }
        }
    }
    void ConsumeCompleted(){
        osfsmp::SceneResult result;
        {
            std::lock_guard lock(resultLock_);
            std::swap(result,completed_);
        }
        if(g_settings.logAreaDiagnostics){
            for(const auto& d:result.areaDiagnostics){
                if(d.measuredBodies || !d.bodyCount || d.fromCache) continue;
                // At most one missing-area diagnosis per system per loaded
                // save. Re-equip creates a new system and gets its own entry.
                if(!areaDiagnosticLogged_.insert(d.system).second)continue;
                logger::warn("[AreaScan] sys=0x{:X} ref={:08X} bones={} measured={} geom={} skinned={} rootScoped={} exactSkin={} nameOnlySkin={} partitionExact={} partitionReadable={} triangles={} reason={}",
                    d.system,d.referenceFormID,d.bodyCount,d.measuredBodies,
                    d.geometryMeshes,d.skinnedMeshes,d.rootScopedMeshes,
                    d.exactSkinMeshes,d.nameOnlySkinMeshes,d.exactPartitions,
                    d.readablePartitions,d.triangles,
                    d.exactSkinMeshes==0 ? (d.nameOnlySkinMeshes>0 ? "SKIN_NODE_IDENTITY_MISMATCH" : "NO_MATCHING_SKIN") :
                    (d.readablePartitions==0 ? "MISSING_CPU_TRIANGLES_OR_UNSUPPORTED_PARTITIONS" : "NO_WEIGHTED_TRIANGLE_AREA"));
            }
            if(areaDiagnosticLogged_.size()>512) areaDiagnosticLogged_.clear();
        }
        for(const auto& r:result.bodies){
            auto* body=reinterpret_cast<btRigidBody*>(r.body);
            const auto it=index_.find(body);
            if(it==index_.end())continue;
            auto& m=meta_[it->second];
            const auto group=systems_.find(r.system);
            if(m.owner!=reinterpret_cast<const void*>(r.owner)||m.name!=r.name||m.system!=r.system ||
               m.node.get()!=reinterpret_cast<RE::NiNode*>(r.nodeToken) ||
               group==systems_.end() || group->second.root.get()!=reinterpret_cast<RE::NiNode*>(r.rootToken))continue;
            m.matched=r.nodeMatched;
            if(r.nodeMatched && r.skeletonAxisMeasured && !m.axisFromSkeleton){
                const auto rotated=V(m.body->getWorldTransform().getBasis().transpose()*
                    B(r.skeletonAxisWorld));
                if(osfsmp::Finite(rotated) && osfsmp::Length(rotated)>0.5f){
                    m.localAxis=osfsmp::Normalized(rotated);
                    m.segmentLength=r.skeletonSegmentLength;
                    m.axisFromSkeleton=true;
                }
            }
            m.areaMeasured=r.areaMeasured;
            m.measuredArea=r.measuredArea;
            m.worldObject=r.allowWorldFallback;
            m.reference=r.referenceFormID;
        }
        for(const auto& r:result.shelters){
            const auto it=systems_.find(r.system);
            if(it==systems_.end() || it->second.anchor!=reinterpret_cast<btRigidBody*>(r.anchorBody) ||
               it->second.root.get()!=reinterpret_cast<RE::NiNode*>(r.rootToken) ||
               it->second.pointEpoch!=r.pointEpoch)continue;
            auto& g=it->second;
            auto point=std::find_if(g.points.begin(),g.points.end(),[&](const auto& p){
                return p.body==reinterpret_cast<btRigidBody*>(r.pointBody);
            });
            if(point==g.points.end())continue;
            // A missing world/owner/raycast is not proof of obstruction.
            // Existing measured exposure eases toward 1; new probes start at 1.
            // Invalid FSMP body ownership still excludes that bone entirely.
            const float exposure=r.sampled?std::clamp(r.exposure,0.f,1.f):1.f;
            if(r.component==osfsmp::ShelterComponent::Ambient){
                if(!point->ambientMeasured)point->ambientExposure=exposure;
                point->ambientTarget=exposure;point->ambientMeasured=r.sampled;
            } else {
                if(!point->transientMeasured)point->transientExposure=exposure;
                point->transientTarget=exposure;point->transientMeasured=r.sampled;
            }
        }
        for(const auto& r:result.selfShelters){
            const auto it=systems_.find(r.system);
            if(it==systems_.end() ||
               it->second.anchor!=reinterpret_cast<btRigidBody*>(r.anchorBody) ||
               it->second.root.get()!=reinterpret_cast<RE::NiNode*>(r.rootToken) ||
               it->second.pointEpoch!=r.pointEpoch)continue;
            auto& g=it->second;
            const float exposure=r.sampled?std::clamp(r.exposure,0.f,1.f):1.f;
            if(r.component==osfsmp::ShelterComponent::Ambient){
                if(!g.selfAmbientMeasured)g.selfAmbientExposure=exposure;
                g.selfAmbientTarget=exposure;
                g.selfAmbientMeasured=r.sampled;
            }else{
                if(!g.selfTransientMeasured)g.selfTransientExposure=exposure;
                g.selfTransientTarget=exposure;
                g.selfTransientMeasured=r.sampled;
            }
        }
    }
    bool QueueSceneWork(bool dirty,CSPluginAPI::ICSInterface001*){
        const auto epoch=sceneEpoch_.load(std::memory_order_acquire);
        if(sessionPaused_.load(std::memory_order_acquire))return false;
        if(scanPending_.load(std::memory_order_acquire))return false;
        auto* tasks=SKSE::GetTaskInterface();
        if(!tasks)return false;
        if(dirty)geometryDirty_=true;
        const bool scanGeometry=geometryDirty_ || clock_>=nextGeometryScanAt_;
        std::vector<osfsmp::BodyRequest> bodies;
        if(scanGeometry){
            bodies.reserve(meta_.size());
            for(const auto& m:meta_){
                if(!m.owner||m.name.empty())continue;
                const auto it=systems_.find(m.system);
                if(it==systems_.end() || !m.node || !it->second.root)continue;
                const auto p=V(m.body->getWorldTransform().getOrigin());
                bodies.push_back({reinterpret_cast<std::uintptr_t>(m.body),
                    reinterpret_cast<std::uintptr_t>(m.owner),m.system,m.name,p,
                    m.segmentLength,it->second.root,m.node});
            }
        }
        std::vector<osfsmp::ShelterRequest> probes;
        if(g_settings.enableShelter){
            probes.reserve(systems_.size()*6);
            for(auto& [id,g]:systems_){
                if(!g.root || !g.anchor || g.points.empty())continue;
                const bool ambientDue=clock_>=g.nextAmbientAt;
                const bool transientDue=clock_>=g.nextTransientAt;
                for(const auto& point:g.points){
                    if(!point.body||!point.validOrigin)continue;
                    const auto body=reinterpret_cast<std::uintptr_t>(point.body);
                    if(ambientDue){
                        probes.push_back({id,reinterpret_cast<std::uintptr_t>(g.anchor),
                            body,g.pointEpoch,g.root,point.origin,point.ambientWind,
                            osfsmp::ShelterComponent::Ambient});
                    }
                    // Distinct transient direction at every selected bone.
                    if(point.transientActive && (point.transientDirty || transientDue)){
                        probes.push_back({id,reinterpret_cast<std::uintptr_t>(g.anchor),
                            body,g.pointEpoch,g.root,point.origin,point.transientWind,
                            osfsmp::ShelterComponent::Transient});
                    }
                }
            }
        }
        std::vector<osfsmp::SelfShelterRequest> selfProbes;
        if(g_settings.enableSelfShelter){
            selfProbes.reserve(systems_.size()*2);
            std::unordered_map<std::uintptr_t,std::vector<osfsmp::Vec3>> dynamic;
            for(const auto& m:meta_){
                if(!m.body || !m.owner || !m.node || !m.matched ||
                   m.body->isStaticOrKinematicObject())continue;
                const auto pos=V(m.body->getWorldTransform().getOrigin());
                if(osfsmp::Finite(pos))dynamic[m.system].push_back(pos);
            }
            for(auto& [id,g]:systems_){
                if(!g.root || !g.anchor || g.points.empty())continue;
                const auto it=dynamic.find(id);
                if(it==dynamic.end() || it->second.empty())continue;
                const auto& nodes=it->second;
                const std::size_t wanted=std::min<std::size_t>(8,nodes.size());
                std::vector<osfsmp::Vec3> samples;
                samples.reserve(wanted);
                for(std::size_t j=0;j<wanted;++j){
                    const auto index=wanted==1?0:j*(nodes.size()-1)/(wanted-1);
                    samples.push_back(nodes[index]);
                }
                // Use the middle world-probe's local wind. GPU SMP uses one
                // claim-root wind direction for its self-shelter test.
                const auto& windPoint=g.points[g.points.size()/2];
                if(clock_>=g.nextSelfAmbientAt &&
                   osfsmp::Length(windPoint.ambientWind)>1.e-4f)
                    selfProbes.push_back({id,reinterpret_cast<std::uintptr_t>(g.anchor),
                        g.pointEpoch,g.root,samples,windPoint.ambientWind,
                        osfsmp::ShelterComponent::Ambient});
                if(clock_>=g.nextSelfTransientAt && windPoint.transientActive &&
                   osfsmp::Length(windPoint.transientWind)>1.e-4f)
                    selfProbes.push_back({id,reinterpret_cast<std::uintptr_t>(g.anchor),
                        g.pointEpoch,g.root,samples,windPoint.transientWind,
                        osfsmp::ShelterComponent::Transient});
            }
        }
        if(bodies.empty()&&probes.empty()&&selfProbes.empty()){
            if(scanGeometry){
                nextGeometryScanAt_=clock_+g_settings.geometryScanSeconds;
                geometryDirty_=false;
            }
            return false;
        }
        // Only one task at a time: avoid concurrently walking scene graphs or
        // mutating area caches; overdue probes remain eligible next PreStep.
        if(scanPending_.exchange(true,std::memory_order_acq_rel))return false;
        probeBatches_+=probes.size();
        selfProbeBatches_+=selfProbes.size();
        for(const auto& request:selfProbes){
            auto& g=systems_.at(request.system);
            if(request.component==osfsmp::ShelterComponent::Ambient)
                g.nextSelfAmbientAt=clock_+g_settings.shelterInterval;
            else g.nextSelfTransientAt=clock_+g_settings.shelterInterval;
        }
        for(const auto& request:probes){
            auto& g=systems_.at(request.system);
            if(request.component==osfsmp::ShelterComponent::Ambient)
                g.nextAmbientAt=clock_+g_settings.shelterInterval;
            else {
                g.nextTransientAt=clock_+g_settings.shelterInterval;
                for(auto& p:g.points)if(reinterpret_cast<std::uintptr_t>(p.body)==request.pointBody)
                    p.transientDirty=false;
            }
        }
        if(scanGeometry){
            nextGeometryScanAt_=clock_+g_settings.geometryScanSeconds;
            geometryDirty_=false;
        }
        const osfsmp::SceneOptions options{g_settings.maxBoneNodeDistance,
            g_settings.shelterNoWindDistance,g_settings.shelterMaxWindDistance,
            g_settings.selfShelterReach};
        // Build requests only for the session captured at entry. A load may
        // start and even finish while the physics thread builds them.
        if(sessionPaused_.load(std::memory_order_acquire) ||
           sceneEpoch_.load(std::memory_order_acquire)!=epoch){
            scanPending_.store(false,std::memory_order_release);
            return false;
        }
        const auto ticket=++nextScanTicket_;
        activeScanTicket_.store(ticket,std::memory_order_release);
        tasks->AddTask([this,epoch,ticket,items=std::move(bodies),probes=std::move(probes),
                        selfProbes=std::move(selfProbes),options]() mutable {
            // Do not call GetUserData()/Get3D()/Havok on old save's nodes
            // after kPreLoadGame. Holding NiPointers alone cannot keep the
            // underlying TESObjectREFR or bhkWorld alive through a load.
            if(sceneEpoch_.load(std::memory_order_acquire)!=epoch ||
               sessionPaused_.load(std::memory_order_acquire))return;
            auto results=osfsmp::ScanScene(items,probes,selfProbes,options);
            {
                std::lock_guard lock(resultLock_);
                if(sceneEpoch_.load(std::memory_order_acquire)!=epoch ||
                   sessionPaused_.load(std::memory_order_acquire) ||
                   activeScanTicket_.load(std::memory_order_acquire)!=ticket)return;
                completed_=std::move(results);
            }
            ++sceneScans_;
            // Never clear the pending flag of a newer save's task.
            if(sceneEpoch_.load(std::memory_order_acquire)==epoch &&
               activeScanTicket_.load(std::memory_order_acquire)==ticket)
                scanPending_.store(false,std::memory_order_release);
        });
        return true;
    }
    bool Rebuild(const std::vector<btRigidBody*>& candidates){
        std::unordered_set<btRigidBody*> live(candidates.begin(),candidates.end());
        std::vector<osfsmp::PhysicsSystem> current;
        osfsmp::SystemSnapshotReport snapshot{};
        if(!osfsmp::ReadActiveSystems(live,current,&snapshot)) {
            // Log diagnostic detail on a throttled interval, without ever
            // continuing from a structurally invalid private ABI snapshot.
            if(frames_%120==0)
                logger::error("FSMP snapshot failure stage='{}' systems={} matched={} candidates={} suspect=0x{:X}",
                    snapshot.stage,snapshot.systems,snapshot.registered,
                    snapshot.candidates,snapshot.suspect);
            return false;
        }
        if (snapshot.rootlessSystems &&
            (!rootlessWarningIssued_ || frames_ - lastRootlessWarningFrame_ >= 1800)) {
            logger::warn("FSMP skipped {} system(s) with null m_skeleton (first=0x{:X}); other verified systems remain active; skipped systems receive NO wind",
                snapshot.rootlessSystems, snapshot.firstRootless);
            rootlessWarningIssued_ = true;
            lastRootlessWarningFrame_ = frames_;
        }
        auto oldGroups=std::move(systems_);
        auto oldMeta=std::move(meta_);
        auto oldIndex=std::move(index_);
        systems_.clear();meta_.clear();index_.clear();ignoredBodies_.clear();
        meta_.reserve(candidates.size());
        for(const auto& system:current){
            SystemMetadata group{};
            group.identity=system.identity;
            group.root=RE::NiPointer<RE::NiNode>(system.root);
            group.anchor=system.anchor;
            group.anchorIsKinematic=system.anchor && system.anchor->isStaticOrKinematicObject();
            // Persistent shelter factor when the same system/anchor survives
            // a body-list refresh. New systems receive a staggered first probe.
            const auto it=oldGroups.find(system.identity);
            if(it!=oldGroups.end() && it->second.root.get()==system.root &&
               it->second.anchor==system.anchor){
                group=std::move(it->second);
            } else {
                group.pointEpoch=++nextPointEpoch_;
                group.nextAmbientAt=clock_+osfsmp::StaggerProbeOffset(
                    system.identity,g_settings.shelterInterval);
                group.nextTransientAt=clock_;
                group.nextSelfAmbientAt=group.nextAmbientAt;
                group.nextSelfTransientAt=clock_;
                // Probe-point state is initialized after FSMP bone discovery.
            }
            systems_.emplace(system.identity,std::move(group));
            for(const auto& bone:system.bones){
                BodyMetadata m{};
                m.body=bone.body;m.system=system.identity;
                m.node=RE::NiPointer<RE::NiNode>(bone.node);
                m.refs=bone.body->getNumConstraintRefs();
                if(!ReadOwner(bone.body,m.boneFactor,m.owner,&m.name)||m.owner!=bone.owner)continue;
                const auto old=oldIndex.find(bone.body);
                if(old!=oldIndex.end()){
                    const auto& prev=oldMeta[old->second];
                    if(prev.owner==m.owner&&prev.name==m.name&&prev.system==m.system&&
                       prev.node.get()==m.node.get()){
                        m.matched=prev.matched;m.areaMeasured=prev.areaMeasured;
                        m.axisFromSkeleton=prev.axisFromSkeleton;
                        if(m.axisFromSkeleton){m.localAxis=prev.localAxis;m.segmentLength=prev.segmentLength;}
                        m.measuredArea=prev.measuredArea;m.worldObject=prev.worldObject;
                        m.reference=prev.reference;
                    }
                }
                index_[bone.body]=meta_.size();meta_.push_back(std::move(m));
            }
        }
        // Unmatched/invalid candidates are never touched by wind code.
        // Each retained body is still identified by its authoritative system,
        // exact SkinnedMeshBone owner, and matching NIF bone node.
        for(auto* b:candidates) {
            if(!index_.contains(b)) ignoredBodies_.insert(b);
        }
        if(!ignoredBodies_.empty() && frames_%240==0)
            logger::warn("FSMP PreStep contained {} non-authoritative bodies ({} unmatched systems, {} invalid bone metadata); safely ignored",
                ignoredBodies_.size(),snapshot.unmatched,
                ignoredBodies_.size()>snapshot.unmatched ? ignoredBodies_.size()-snapshot.unmatched : 0);
        // Constraints are used only for approximate segment axes/lengths.
        // Ownership boundaries come *exclusively* from FSMP m_systems.
        std::vector<std::vector<int>> adjacency(meta_.size());
        for(std::size_t i=0;i<meta_.size();++i){
            auto* body=meta_[i].body;
            for(int j=0;j<std::clamp(body->getNumConstraintRefs(),0,10000);++j){
                auto* c=body->getConstraintRef(j);if(!c)continue;
                auto* a=&c->getRigidBodyA();auto* b=&c->getRigidBodyB();
                auto* other=(a==body)?b:(b==body)?a:nullptr;
                const auto it=index_.find(other);
                if(it!=index_.end()&&it->second!=i&&meta_[it->second].system==meta_[i].system)
                    adjacency[i].push_back(static_cast<int>(it->second));
            }
        }
        for(auto& row:adjacency){
            std::sort(row.begin(),row.end());row.erase(std::unique(row.begin(),row.end()),row.end());
        }
        std::queue<int> queue;std::vector<bool> seen(meta_.size(),false);
        for(std::size_t i=0;i<meta_.size();++i){
            if(meta_[i].body->isStaticOrKinematicObject()){
                seen[i]=true;queue.push(static_cast<int>(i));
            }
        }
        const auto traverse=[&](){
            while(!queue.empty()){
                const auto i=queue.front();queue.pop();
                for(const auto j:adjacency[i]){
                    if(seen[j])continue;
                    seen[j]=true;
                    meta_[j].parent=i;
                    if(meta_[i].firstChild<0)meta_[i].firstChild=j;
                    queue.push(j);
                }
            }
        };
        traverse();
        for(std::size_t i=0;i<meta_.size();++i){
            if(seen[i])continue;
            seen[i]=true;queue.push(static_cast<int>(i));traverse();
        }
        for(auto& m:meta_){
            if(m.axisFromSkeleton)continue;
            const auto& tf=m.body->getWorldTransform();
            const auto here=tf.getOrigin();
            btVector3 segment{0,0,-1};
            if(m.firstChild>=0)segment=meta_[m.firstChild].body->getWorldTransform().getOrigin()-here;
            else if(m.parent>=0)segment=here-meta_[m.parent].body->getWorldTransform().getOrigin();
            m.segmentLength=segment.length();
            const auto direction=m.segmentLength>1.e-6f?segment/m.segmentLength:btVector3{0,0,-1};
            m.localAxis=V(tf.getBasis().transpose()*direction);
        }
        logger::info("FSMP authoritative systems={}, registered bones={}, kinematic anchors={}, skippedRootless={}",
            systems_.size(),meta_.size(),std::count_if(systems_.begin(),systems_.end(),
                [](const auto& pair){return pair.second.anchorIsKinematic;}),
            snapshot.rootlessSystems);
        return true;
    }
    void LogBoneForces(std::vector<BoneForceDiagnostic>& rows) const {
        // Emit the largest actual force contributions first.  An entry is the
        // force on ONE FSMP bone in this physics step, not an integrated impulse,
        // and not the force from vanilla FSMP's separate wind implementation.
        std::sort(rows.begin(),rows.end(),[](const auto& lhs,const auto& rhs){
            return osfsmp::Length(lhs.accumulated)>osfsmp::Length(rhs.accumulated);
        });
        const std::size_t nonzeroRequested=std::count_if(rows.begin(),rows.end(),[](const auto& r){
            return osfsmp::Length(r.requested)>1.e-5f;
        });
        const std::size_t nonzeroAccepted=std::count_if(rows.begin(),rows.end(),[](const auto& r){
            return osfsmp::Length(r.accumulated)>1.e-5f;
        });
        const std::size_t blocked=std::count_if(rows.begin(),rows.end(),[](const auto& r){
            return osfsmp::Length(r.requested)>1.e-5f &&
                   osfsmp::Length(r.accumulated)<=1.e-5f;
        });
        const std::size_t limit=g_settings.boneForceLogMaxEntries==0?rows.size():
            std::min(rows.size(),static_cast<std::size_t>(g_settings.boneForceLogMaxEntries));
        logger::info("[BoneForce] frame={} evaluated={} requestedNonzero={} accumulatedNonzero={} suppressed={} showing={} (of {}), highest accumulated first",
            frames_,rows.size(),nonzeroRequested,nonzeroAccepted,blocked,limit,rows.size());
        for(std::size_t i=0;i<limit;++i){
            const auto& r=rows[i];
            logger::info("[BoneForce] #{} ref={:08X} world={} sys=0x{:X} body=0x{:X} bone='{}' windSpeed={:.3f} area={:.2f} mass={:.3f} windFactor={:.3f} shelter=({:.2f},{:.2f}) self=({:.2f},{:.2f}) skeletonAxis={} areaFallback={} requested=({:.3f},{:.3f},{:.3f}) |F|={:.3f} added=({:.3f},{:.3f},{:.3f}) |dF|={:.3f} linearFactor=({:.2f},{:.2f},{:.2f}) totalBefore=({:.3f},{:.3f},{:.3f}) totalAfter=({:.3f},{:.3f},{:.3f})",
                i+1,r.reference,r.worldObject,r.system,r.body,r.name,
                osfsmp::Length(r.wind),r.area,r.mass,r.boneFactor,
                r.ambientExposure,r.transientExposure,
                r.selfAmbientExposure,r.selfTransientExposure,r.skeletonAxis,r.areaFallback,
                r.requested.x,r.requested.y,r.requested.z,osfsmp::Length(r.requested),
                r.accumulated.x,r.accumulated.y,r.accumulated.z,osfsmp::Length(r.accumulated),
                r.linearFactor.x,r.linearFactor.y,r.linearFactor.z,
                r.totalBefore.x,r.totalBefore.y,r.totalBefore.z,
                r.totalAfter.x,r.totalAfter.y,r.totalAfter.z);
        }
    }
    void LogSummary(){
        std::size_t ready=0,world=0,measured=0,connected=0;
        std::size_t shelterPoints=0,multiProbeSystems=0,withSkeletonAxes=0,withFallback=0;
        for(const auto& [id,g]:systems_){
            shelterPoints+=g.points.size();
            if(g.points.size()>1)++multiProbeSystems;
        }
        for(const auto& m:meta_){
            if(m.worldObject)++world;
            if(m.areaMeasured)++measured;
            if(m.axisFromSkeleton)++withSkeletonAxes;
            if(m.matched && !m.areaMeasured && (g_settings.allowFallback ||
                (g_settings.allowWorldFallback && m.worldObject)))++withFallback;
            if(m.matched)++connected;
            if(m.matched&&(m.areaMeasured||g_settings.allowFallback||
               (g_settings.allowWorldFallback&&m.worldObject)))++ready;
        }
        logger::info("Frame {}: systems={} bodies={} nodeMatched={} worldBodies={} NIFarea={} areaReady={} shelterProbeBatches={} sceneTasks={} windSamples={} maxBase={:.3f} nonzeroForces={} maxForce={:.3f} shelterPoints={} multiProbeSystems={} selfProbeBatches={} skeletonAxes={} fallbackAreas={}",
            frames_,systems_.size(),meta_.size(),connected,world,measured,ready,
            probeBatches_,sceneScans_.load(std::memory_order_relaxed),sampledThisStep_,
            maxBaseResponse_,appliedThisStep_,maxAppliedForce_,shelterPoints,multiProbeSystems,
            selfProbeBatches_,withSkeletonAxes,withFallback);
    }
    hdt::PluginInterface* smp_{};
    std::atomic<CSPluginAPI::ICSInterface001*> wind_{nullptr};
    std::unordered_map<btRigidBody*,std::size_t> index_;
    std::unordered_set<btRigidBody*> ignoredBodies_;
    std::vector<BodyMetadata> meta_;
    std::unordered_map<std::uintptr_t,SystemMetadata> systems_;
    std::atomic<bool> scanPending_{false};
    std::atomic<bool> sessionPaused_{false};
    std::atomic<bool> resetRequired_{false};
    std::atomic<std::uint64_t> sceneEpoch_{1};
    std::atomic<std::uint64_t> activeScanTicket_{0};
    std::uint64_t nextScanTicket_{};
    std::unordered_set<std::uintptr_t> areaDiagnosticLogged_;
    std::mutex resultLock_;
    osfsmp::SceneResult completed_;
    std::atomic<std::uint64_t> sceneScans_{0};
    std::uint64_t probeBatches_{},selfProbeBatches_{};
    std::uint64_t frames_{};
    std::uint64_t nextPointEpoch_{1};
    std::uint64_t lastRootlessWarningFrame_{};
    bool rootlessWarningIssued_{};
    float clock_{},nextGeometryScanAt_{};
    bool geometryDirty_{true};
    std::size_t sampledThisStep_{},appliedThisStep_{};
    float maxBaseResponse_{},maxAppliedForce_{};
};
WindAdapter g_adapter;
// FSMP dispatches MSG_STARTUP during its PostPostLoad initialization. Check only
// at DataLoaded, after the normal opportunity for that message has passed.
// Keep this independent of private-ABI validation: receiving MSG_STARTUP with
// an incompatible interface is a different failure from never receiving it.
std::atomic_bool g_fsmpStartupSeen{false};
std::atomic_bool g_fsmpMissingStartupWarned{false};
void OnMessage(SKSE::MessagingInterface::Message* msg){
    if(!msg)return;
    if(msg->sender && _stricmp(msg->sender,"hdtsmp64")==0){
        if(msg->type==hdt::PluginInterface::MSG_STARTUP){
            g_fsmpStartupSeen.store(true,std::memory_order_release);
            if(msg->data)
                g_adapter.SetFSMP(static_cast<hdt::PluginInterface*>(msg->data));
            else
                logger::error("FSMP MSG_STARTUP received with a null PluginInterface; wind integration unavailable");
        }
        return;
    }
    if(!msg->sender||_stricmp(msg->sender,"SKSE")!=0)return;
    switch(msg->type){
        case SKSE::MessagingInterface::kPreLoadGame:
            g_adapter.OnPreLoadGame();
            break;
        case SKSE::MessagingInterface::kPostLoadGame:
            g_adapter.OnPostLoadGame();
            break;
        case SKSE::MessagingInterface::kNewGame:
            g_adapter.OnNewGame();
            break;
        case SKSE::MessagingInterface::kPostLoad:
            if(!SKSE::GetMessagingInterface()->RegisterListener("hdtsmp64",OnMessage))
                logger::error("Could not subscribe to FSMP startup message");
            break;
        case SKSE::MessagingInterface::kDataLoaded:
            if(!g_fsmpStartupSeen.load(std::memory_order_acquire) &&
               !g_fsmpMissingStartupWarned.exchange(true,std::memory_order_acq_rel)){
                logger::warn("FSMP MSG_STARTUP not received by SKSE DataLoaded; wind integration unavailable. Check hdtsmp64.dll loading and FSMP startup logs");
            }
            g_adapter.ConnectShaders();
            break;
        default:break;
    }
}
} // namespace

SKSEPluginLoad(const SKSE::LoadInterface* skse){
    SKSE::Init(skse,{.logName="OpenShadersFSMPWind"});
    LoadSettings();
    const auto* messaging=SKSE::GetMessagingInterface();
    if(!messaging||!messaging->RegisterListener(OnMessage))return false;
    logger::info("OpenShadersFSMPWind 0.3.4 loaded, Skyrim 1.7.104 target; matched NIF skin areas + world shielding");
    logger::info("Bone force diagnostics: enabled={}, interval={} physics steps, max entries={} (0=all)",
        g_settings.logBoneForces,g_settings.boneForceLogEvery,g_settings.boneForceLogMaxEntries);
    logger::info("Wind: strength={}, cap={}, maxAcceleration={}, nativeTaggedTransients={}, shelter={}, allowFallback={}",
        g_settings.strength,g_settings.maxSpeed,g_settings.maxWindAcceleration,g_settings.includeNative,
        g_settings.enableShelter,g_settings.allowFallback);
    logger::info("Self-shelter: enabled={}, reach={} Skyrim units; game-thread actor Havok + posed head/shoulder approximations; unavailable probes unobstructed; missing-area fallback={}",
        g_settings.enableSelfShelter,g_settings.selfShelterReach,g_settings.allowFallback);
    logger::info("Shelter: 1/3/5 points (extent thresholds {} / {} Skyrim units); distance ramp {}..{}; cadence {}s; unavailable probes unobstructed",
        g_settings.shelterThreePointDistance,g_settings.shelterFivePointDistance,
        g_settings.shelterNoWindDistance,g_settings.shelterMaxWindDistance,g_settings.shelterInterval);
    return true;
}
