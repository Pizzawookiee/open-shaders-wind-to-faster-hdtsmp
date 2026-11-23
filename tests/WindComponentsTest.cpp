#include "WindMath.h"
#include <cassert>
#include <cmath>
#include <iostream>
using namespace osfsmp;
int main(){
    constexpr Vec3 base{0.7f,0.f,0.f},gust{0.1f,0.2f,0.f};
    const auto noHavok=SeparateWind(base,gust,{0.4f,0.7f,0.f});
    assert(std::abs(noHavok.ambient.x-0.8f)<1.e-6f);
    assert(std::abs(noHavok.ambient.y-0.2f)<1.e-6f);
    assert(std::abs(noHavok.transient.x-(-0.4f))<1.e-6f);
    assert(std::abs(noHavok.transient.y-0.5f)<1.e-6f);
    assert(Length(noHavok.ambient+noHavok.transient-Vec3{0.4f,0.7f,0.f})<1.e-6f);
    assert(ShouldReprobeTransient(false,{1,0,0},0.f,{0.1f,0,0}));
    assert(!ShouldReprobeTransient(false,{1,0,0},0.f,{0.001f,0,0}));
    assert(!ShouldReprobeTransient(true,{1,0,0},1.f,{1.1f,0,0}));
    assert(ShouldReprobeTransient(true,{1,0,0},1.f,{-1.f,0,0}));
    assert(ShouldReprobeTransient(true,{1,0,0},1.f,{2.f,0,0}));
    for(unsigned i=0;i<100;++i){
        const auto offset=StaggerProbeOffset(static_cast<std::uintptr_t>(i)*16,0.5f);
        assert(offset>=0.f && offset<0.5f);
    }
    std::cout<<"Wind component/probe tests passed\n";
}
