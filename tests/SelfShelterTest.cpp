#include "SelfShelter.h"
#include <array>
#include <cassert>
#include <cmath>
using namespace osfsmp;
int main(){
    constexpr float reach=150.f;
    const std::array<SelfShelterSphere,1> head{{{{20,0,0},8}}};
    const std::array<SelfShelterCapsule,1> shoulder{{{{30,0,-10},{30,0,10},5}}};
    const std::array<Vec3,2> hair{{{0,0,0},{0,0,30}}};
    const auto exposed=SelfShelterExposure(hair,{1,0,0},reach,shoulder,head);
    assert(std::abs(exposed-0.5f)<1.e-5f);
    assert(SelfShelterExposure(hair,{-1,0,0},reach,shoulder,head)==1.f);
    assert(SelfShelterExposure({}, {1,0,0},reach,shoulder,head)==1.f);
    assert(!SelfSphereBlocks({20,0,0},{1,0,0},reach,head[0])); // origin inside
    assert(!SelfCapsuleBlocks({30,0,0},{1,0,0},reach,shoulder[0]));
    assert(!SelfCapsuleBlocks({0,0,0},{1,0,0},10,shoulder[0]));
    assert(!SelfSphereBlocks({0,0,0},{1,0,0},10,head[0]));
}
