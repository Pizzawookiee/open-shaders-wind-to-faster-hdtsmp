#include "SelfShelter.h"
#include <algorithm>
#include <cmath>

namespace osfsmp {
namespace {
constexpr float eps=1.e-8f;
float SegmentDistanceSquared(Vec3 p1,Vec3 q1,Vec3 p2,Vec3 q2) noexcept {
    const auto d1=q1-p1,d2=q2-p2,r=p1-p2;
    const float a=Dot(d1,d1),e=Dot(d2,d2),f=Dot(d2,r);
    float s=0.f,t=0.f;
    if(a<=eps && e<=eps)return Dot(r,r);
    if(a<=eps)t=std::clamp(f/e,0.f,1.f);
    else {
        const float c=Dot(d1,r);
        if(e<=eps)s=std::clamp(-c/a,0.f,1.f);
        else {
            const float b=Dot(d1,d2),denom=a*e-b*b;
            s=denom>eps?std::clamp((b*f-c*e)/denom,0.f,1.f):0.f;
            t=(b*s+f)/e;
            if(t<0.f){t=0.f;s=std::clamp(-c/a,0.f,1.f);}
            else if(t>1.f){t=1.f;s=std::clamp((b-c)/a,0.f,1.f);}
        }
    }
    const Vec3 delta=(p1+d1*s)-(p2+d2*t);
    return Dot(delta,delta);
}
}
bool SelfCapsuleBlocks(Vec3 origin,Vec3 toward,float reach,
                       const SelfShelterCapsule& capsule) noexcept {
    if(!Finite(origin)||!Finite(toward)||!Finite(capsule.a)||!Finite(capsule.b)||
       !std::isfinite(capsule.radius)||!(capsule.radius>0.f)||!(reach>0.f))return false;
    const auto dir=Normalized(toward,{0,0,0});
    if(!(Length(dir)>0.f))return false;
    const float radius2=capsule.radius*capsule.radius;
    if(SegmentDistanceSquared(origin,origin,capsule.a,capsule.b)<radius2)return false;
    return SegmentDistanceSquared(origin,origin+dir*reach,capsule.a,capsule.b)<radius2;
}
bool SelfSphereBlocks(Vec3 origin,Vec3 toward,float reach,
                      const SelfShelterSphere& sphere) noexcept {
    if(!Finite(origin)||!Finite(toward)||!Finite(sphere.center)||
       !std::isfinite(sphere.radius)||!(sphere.radius>0.f)||!(reach>0.f))return false;
    const auto dir=Normalized(toward,{0,0,0});
    if(!(Length(dir)>0.f))return false;
    const float r2=sphere.radius*sphere.radius;
    if(Dot(origin-sphere.center,origin-sphere.center)<r2)return false;
    return SegmentDistanceSquared(origin,origin+dir*reach,sphere.center,sphere.center)<r2;
}
float SelfShelterExposure(std::span<const Vec3> samples,Vec3 toward,float reach,
                          std::span<const SelfShelterCapsule> capsules,
                          std::span<const SelfShelterSphere> spheres) noexcept {
    if(samples.empty()||!Finite(toward)||!(Length(toward)>1.e-5f)||!(reach>0.f))return 1.f;
    unsigned valid=0,exposed=0;
    for(const auto& pos:samples){
        if(!Finite(pos))continue;
        ++valid;
        bool blocked=false;
        for(const auto& shape:capsules)if(SelfCapsuleBlocks(pos,toward,reach,shape)){blocked=true;break;}
        if(!blocked)for(const auto& shape:spheres)if(SelfSphereBlocks(pos,toward,reach,shape)){blocked=true;break;}
        if(!blocked)++exposed;
    }
    return valid?static_cast<float>(exposed)/static_cast<float>(valid):1.f;
}
} // namespace osfsmp
