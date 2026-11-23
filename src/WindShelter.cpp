// Three weighted upwind shelter rays from GPUSMPPhysicsNG; distance attenuation
// follows native FSMP's linear near/far wind-factor ramp (50..2000 by default).
#include "WindShelter.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace osfsmp {
float ShelterCoordinate(Vec3 p,unsigned axis) noexcept {
    return axis==0 ? p.x : axis==1 ? p.y : p.z;
}

float ShelterExposure(std::array<float,3> hits,float nearDistance,float farDistance) noexcept {
    if(!std::isfinite(nearDistance)||!std::isfinite(farDistance)||
       nearDistance<0.f||farDistance<=nearDistance) return 1.f;
    // Retain GPUSMPPhysicsNG's close center-wall rule, consistent with
    // native FSMP's full shelter at/inside the near distance.
    if(std::isfinite(hits[0]) && hits[0]>=0.f && hits[0]<=nearDistance)return 0.f;
    float obstruction=0.f;
    for(std::size_t i=0;i<hits.size();++i){
        const float distance=hits[i];
        if(!std::isfinite(distance)||distance<0.f)continue; // clear ray
        const float exposed=std::clamp((distance-nearDistance)/(farDistance-nearDistance),0.f,1.f);
        obstruction+=ShelterWeight[i]*(1.f-exposed);
    }
    return std::clamp(1.f-obstruction,0.f,1.f);
}

ShelterPointSelection SelectShelterPoints(std::span<const ShelterBonePosition> bones,
    std::uintptr_t anchorBody,float threeThreshold,float fiveThreshold)
{
    ShelterPointSelection result;
    std::vector<ShelterBonePosition> valid;
    valid.reserve(bones.size());
    std::unordered_set<std::uintptr_t> seen;
    seen.reserve(bones.size());
    Vec3 minimum{std::numeric_limits<float>::max(),std::numeric_limits<float>::max(),std::numeric_limits<float>::max()};
    Vec3 maximum{-minimum.x,-minimum.y,-minimum.z};
    for(const auto& b:bones){
        if(!b.body||!Finite(b.origin))continue;
        if(!seen.insert(b.body).second)continue;
        valid.push_back(b);
        minimum.x=std::min(minimum.x,b.origin.x);minimum.y=std::min(minimum.y,b.origin.y);minimum.z=std::min(minimum.z,b.origin.z);
        maximum.x=std::max(maximum.x,b.origin.x);maximum.y=std::max(maximum.y,b.origin.y);maximum.z=std::max(maximum.z,b.origin.z);
    }
    if(valid.empty())return result;
    const Vec3 spans=maximum-minimum;
    if(spans.x>=spans.y&&spans.x>=spans.z)result.axis=0;
    else if(spans.y>=spans.z)result.axis=1;
    result.extent=ShelterCoordinate(spans,result.axis);
    std::sort(valid.begin(),valid.end(),[&](const auto& a,const auto& b){
        const float x=ShelterCoordinate(a.origin,result.axis), y=ShelterCoordinate(b.origin,result.axis);
        return x<y||(x==y&&a.body<b.body);
    });
    unsigned wanted=result.extent>fiveThreshold?5:result.extent>threeThreshold?3:1;
    wanted=std::min<unsigned>(wanted,static_cast<unsigned>(valid.size()));
    if(wanted==1){
        const auto it=std::find_if(valid.begin(),valid.end(),[&](const auto& p){return p.body==anchorBody;});
        result.points.push_back(it!=valid.end()?*it:valid[valid.size()/2]);
        return result;
    }
    const float low=ShelterCoordinate(valid.front().origin,result.axis);
    const float high=ShelterCoordinate(valid.back().origin,result.axis);
    for(unsigned i=0;i<wanted;++i){
        // For 2/4 sparse bones distribute uniformly instead of reusing points.
        const float t=wanted==3?(0.2f+0.3f*i):wanted==5?(0.1f+0.2f*i):
            static_cast<float>(i)/static_cast<float>(wanted-1);
        const float target=std::lerp(low,high,t);
        auto choice=valid.end();
        float best=std::numeric_limits<float>::infinity();
        for(auto it=valid.begin();it!=valid.end();++it){
            const bool used=std::any_of(result.points.begin(),result.points.end(),
                [&](const auto& p){return p.body==it->body;});
            if(used)continue;
            const float distance=std::abs(ShelterCoordinate(it->origin,result.axis)-target);
            if(distance<best){best=distance;choice=it;}
        }
        if(choice!=valid.end())result.points.push_back(*choice);
    }
    std::sort(result.points.begin(),result.points.end(),[&](const auto& a,const auto& b){
        return ShelterCoordinate(a.origin,result.axis)<ShelterCoordinate(b.origin,result.axis);
    });
    return result;
}

float InterpolateShelterExposure(std::span<const ShelterExposurePoint> samples,
                                 float coordinate) noexcept {
    if(samples.empty()||!std::isfinite(coordinate))return 1.f;
    if(samples.size()==1||coordinate<=samples.front().coordinate)
        return std::clamp(samples.front().exposure,0.f,1.f);
    for(std::size_t i=1;i<samples.size();++i){
        if(coordinate>samples[i].coordinate)continue;
        const float x0=samples[i-1].coordinate,x1=samples[i].coordinate;
        const float t=x1>x0?std::clamp((coordinate-x0)/(x1-x0),0.f,1.f):0.f;
        return std::clamp(std::lerp(samples[i-1].exposure,samples[i].exposure,t),0.f,1.f);
    }
    return std::clamp(samples.back().exposure,0.f,1.f);
}
} // namespace osfsmp
