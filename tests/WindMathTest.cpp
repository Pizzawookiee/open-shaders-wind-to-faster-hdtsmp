#include "WindMath.h"
#include <cassert>
#include <cmath>
#include <iostream>
using namespace osfsmp;
int main(){
    WindInput in{};
    in.wind={900,0,0}; in.worldAxis={0,0,-1}; in.worldOrigin={1,2,3};
    in.area=30; in.mass=1; in.boneFactor=1;
    // FSMP's Bullet positions are in Skyrim coordinate units (see hdtConvertNi.h).
    // 0.01425 is used only inside the gust phase calculation, not for the force.
    static_assert(kBulletPerSkyrimUnit > 0.014f && kBulletPerSkyrimUnit < 0.015f);
    assert(in.worldOrigin.x == 1.f);
    const Vec3 f=ComputeWindForce(in);
    assert(Finite(f)); assert(f.x>0);
    in.boneFactor=0; assert(Length(ComputeWindForce(in))==0);
    in.boneFactor=1; in.worldAxis={1,0,0}; assert(Length(ComputeWindForce(in))==0);
    in.worldAxis={0,0,-1}; in.wind={0,0,0}; assert(Length(ComputeWindForce(in))==0);
    in.wind={900,0,0}; in.area=5000.f; in.mass=0.208f;
    in.maxWindAcceleration=400.f;
    const auto capped=Length(ComputeWindForce(in));
    assert(capped>0.f && capped<90.f);
    in.maxWindAcceleration=800.f;
    const auto doubled=Length(ComputeWindForce(in));
    assert(doubled>capped*1.9f && doubled<capped*2.1f);
    in.maxWindAcceleration=0.f;
    assert(Length(ComputeWindForce(in))==0.f);
    // With stationary hair and uncapped drag, 50% shelter must halve force,
    // not quarter it (which happens when shelter scales wind velocity).
    WindInput exposed{};
    exposed.wind={300,0,0};
    exposed.area=100.f; exposed.mass=10.f;
    exposed.boneFactor=1.f;
    exposed.worldAxis={0,0,1};
    exposed.worldOrigin={0,0,0};
    const float allForce=Length(ComputeWindForce(exposed));
    const auto half=ComposeShelteredWind({300,0,0},{0,0,0},0.5f,1.f,1.f,900.f);
    assert(std::abs(half.wind.x-300.f)<1.e-5f);
    assert(std::abs(half.exposure-0.5f)<1.e-5f);
    exposed.wind=half.wind;
    exposed.boneFactor=half.exposure;
    const float halfForce=Length(ComputeWindForce(exposed));
    assert(std::abs(halfForce/allForce-0.5f)<1.e-4f);
    // Separate component exposure changes the effective force's direction.
    const auto mixed=ComposeShelteredWind({100,0,0},{0,100,0},1.f,0.f,1.f,900.f);
    assert(mixed.exposure==0.5f && mixed.wind.x==200.f && mixed.wind.y==0.f);
    const auto cancelled=ComposeShelteredWind({100,0,0},{-100,0,0},0.5f,0.5f,1.f,900.f);
    assert(Length(cancelled.wind)==0.f);
    const auto blocked=ComposeShelteredWind({100,0,0},{0,0,0},0.f,0.f,1.f,900.f);
    assert(Length(blocked.wind)==0.f && blocked.exposure==0.f);
    std::cout<<"WindMath tests passed\n";
}
