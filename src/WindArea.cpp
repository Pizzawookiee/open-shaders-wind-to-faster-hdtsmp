// GPUSMPPhysicsNG EstimateBoneWindAreas algorithm, adapted to self-contained input.
// GPL-3.0-or-later WITH LicenseRef-Modding-Exception; see third_party notices.
#include "WindArea.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace osfsmp {
void AccumulateTriangleAreas(std::span<const AreaVertex> vertices,
    std::span<const std::uint32_t> indices, std::span<float> accumulated)
{
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        const std::uint32_t a = indices[i], b = indices[i+1], c = indices[i+2];
        if (a >= vertices.size() || b >= vertices.size() || c >= vertices.size()) continue;
        const auto& A=vertices[a]; const auto& B=vertices[b]; const auto& C=vertices[c];
        if (!Finite(A.position) || !Finite(B.position) || !Finite(C.position)) continue;
        const float triangle = 0.5f * Length(Cross(B.position-A.position, C.position-A.position));
        if (!(triangle > 0.0f) || !std::isfinite(triangle) || triangle > 1.0e7f) continue;
        for (const AreaVertex* vert : {&A, &B, &C}) {
            float total=0.0f;
            for (int k=0;k<4;++k) if (std::isfinite(vert->weight[k]) && vert->weight[k]>0.0f) total+=vert->weight[k];
            if (!(total>0.0f) || !std::isfinite(total)) continue;
            for(int k=0;k<4;++k) {
                const int dst=vert->rigBone[k];
                const float w=vert->weight[k];
                if(dst<0 || static_cast<std::size_t>(dst)>=accumulated.size() || !(w>0.0f) || !std::isfinite(w)) continue;
                accumulated[dst]+=(triangle/3.0f)*(w/total);
            }
        }
    }
}
float EffectiveWindArea(float measuredArea, float segmentLength,
    float defaultWidth, bool allowSegmentFallback)
{
    if (!std::isfinite(segmentLength) || !std::isfinite(measuredArea) || !std::isfinite(defaultWidth)) return 0.0f;
    // GPU SMP uses a 1-unit minimum, not a 250-unit upper clamp. Extremely
    // long or invalid geometry is rejected rather than silently made a sail.
    if(segmentLength>10000.0f)return 0.0f;
    const float length=std::max(segmentLength,1.0f);
    if (measuredArea>0.0f) return std::clamp(measuredArea,length*1.0f,length*40.0f);
    return allowSegmentFallback ? length*std::clamp(defaultWidth,1.0f,40.0f) : 0.0f;
}
float HalfToFloat(std::uint16_t h) noexcept
{
    const bool neg=(h&0x8000u)!=0;
    const int exp=(h>>10)&31;
    const int mant=h&1023;
    if (exp==31) return 0.0f;
    const float mag=exp==0 ? std::ldexp(static_cast<float>(mant),-24) :
        std::ldexp(1.0f+static_cast<float>(mant)/1024.0f,exp-15);
    return neg ? -mag : mag;
}
}
