#include "WindArea.h"
#include "WindShelter.h"
#include <cassert>
#include <cmath>
#include <array>
#include <vector>
#include <set>
using namespace osfsmp;
int main() {
    // Triangle area = 6; each corner 2; two vertices bone 0, one bone 1.
    std::array<AreaVertex,3> vertices{};
    vertices[0].position={0,0,0};vertices[1].position={3,0,0};vertices[2].position={0,4,0};
    vertices[0].rigBone[0]=0;vertices[1].rigBone[0]=0;vertices[2].rigBone[0]=1;
    for(auto& v:vertices)v.weight[0]=1.0f;
    std::array<std::uint32_t,3> indices{0,1,2};
    std::array<float,2> areas{};
    AccumulateTriangleAreas(vertices,indices,areas);
    assert(std::abs(areas[0]-4.0f)<1e-5f && std::abs(areas[1]-2.0f)<1e-5f);
    // Uniform scaling must preserve attribution and scale every area by s^2.
    areas.fill(0.f);
    AccumulateTriangleAreas(vertices,indices,areas,2.f);
    assert(std::abs(areas[0]-16.f)<1e-5f && std::abs(areas[1]-8.f)<1e-5f);
    areas.fill(0.f);
    AccumulateTriangleAreas(vertices,indices,areas,0.5f);
    assert(std::abs(areas[0]-1.f)<1e-5f && std::abs(areas[1]-0.5f)<1e-5f);
    areas.fill(0.f);
    AccumulateTriangleAreas(vertices,indices,areas,0.f);
    AccumulateTriangleAreas(vertices,indices,areas,-1.f);
    AccumulateTriangleAreas(vertices,indices,areas,NAN);
    AccumulateTriangleAreas(vertices,indices,areas,INFINITY);
    assert(areas[0]==0.f && areas[1]==0.f);
    vertices[1].rigBone[1]=1;vertices[1].weight[0]=0.5f;vertices[1].weight[1]=0.5f;
    areas.fill(0);
    AccumulateTriangleAreas(vertices,indices,areas);
    assert(std::abs(areas[0]-3.0f)<1e-5f && std::abs(areas[1]-3.0f)<1e-5f);
    // GPU SMP parity: a vertex's unmapped influences still count in the
    // denominator.  Only the mapped part can contribute to a simulated bone.
    vertices[1].rigBone[1]=-1;
    areas.fill(0.0f);
    AccumulateTriangleAreas(vertices,indices,areas);
    assert(std::abs(areas[0]-3.0f)<1e-5f); // 2 + 2*0.5
    assert(std::abs(areas[1]-2.0f)<1e-5f); // 2 (third vertex only)
    assert(std::abs(areas[0]+areas[1]-5.0f)<1e-5f); // 1 area unit unmapped
    // Unmapped influence can have >50% of a corner without being redistributed.
    vertices[1].weight[0]=0.25f;
    vertices[1].weight[1]=0.75f;
    areas.fill(0.0f);
    AccumulateTriangleAreas(vertices,indices,areas);
    assert(std::abs(areas[0]-2.5f)<1e-5f);
    assert(std::abs(areas[1]-2.0f)<1e-5f);
    // A completely unmapped vertex contributes no area, never renormalizes.
    vertices[1].rigBone[0]=-1;
    areas.fill(0.0f);
    AccumulateTriangleAreas(vertices,indices,areas);
    assert(std::abs(areas[0]-2.0f)<1e-5f);
    assert(std::abs(areas[1]-2.0f)<1e-5f);
    assert(EffectiveWindArea(0,12,3,false)==0.0f);
    assert(EffectiveWindArea(0,12,3,true)==36.0f);
    assert(EffectiveWindArea(10000,12)==480.0f);
    assert(std::abs(HalfToFloat(0x3c00)-1.0f)<1e-6f);
    assert(std::abs(HalfToFloat(0x3800)-0.5f)<1e-6f);
    assert(ShelterExposure({-1,-1,-1})==1.0f);
    assert(ShelterExposure({10,-1,-1})==0.0f);
    assert(std::abs(ShelterExposure({-1,100,-1})-(1.f-0.225f*(1.f-50.f/1950.f)))<1e-5f);
    assert(ShelterExposure({-1,-1,-1})==1.f);
    assert(ShelterExposure({3000,3000,3000})==1.f);
    assert(ShelterExposure({50,-1,-1})==0.f);
    assert(ShelterExposure({-1,50,-1})==0.775f);
    assert(ShelterExposure({2000,2000,2000})==1.f);
    assert(ShelterExposure({-1,1050,-1})>0.8f);
    std::vector<ShelterBonePosition> longBones;
    for(int i=0;i<11;++i)longBones.push_back({static_cast<std::uintptr_t>(i+1),{0,0,static_cast<float>(i*100)}});
    const auto five=SelectShelterPoints(longBones,1);
    assert(five.axis==2 && five.extent==1000.f && five.points.size()==5);
    std::set<std::uintptr_t> unique;
    for(const auto& p:five.points)unique.insert(p.body);
    assert(unique.size()==5);
    for(std::size_t i=1;i<five.points.size();++i)
        assert(five.points[i].origin.z>five.points[i-1].origin.z);
    const auto three=SelectShelterPoints(longBones,1,300.f,1100.f);
    assert(three.points.size()==3);
    const auto one=SelectShelterPoints(longBones,1,1100.f,1200.f);
    assert(one.points.size()==1 && one.points[0].body==1);
    std::array<ShelterExposurePoint,3> exposure{{{100,0.f},{500,0.5f},{900,1.f}}};
    assert(std::abs(InterpolateShelterExposure(exposure,300)-0.25f)<1e-5f);
    assert(InterpolateShelterExposure(exposure,0)==0.f);
    assert(InterpolateShelterExposure(exposure,1000)==1.f);
}
