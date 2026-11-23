// Formula port from alandtse/GPUSMPPhysicsNG/shaders/Common/HairWind.hlsli.
// Copyright of the original implementation remains with its contributors.
// GPL-3.0-or-later WITH LicenseRef-Modding-Exception (see third-party notices).
#include "WindMath.h"
namespace osfsmp {
ShelteredWind ComposeShelteredWind(Vec3 ambient,Vec3 transient,
    float ambientExposure,float transientExposure,float windScale,float maxSpeed) noexcept {
    if(!Finite(ambient)||!Finite(transient)||!std::isfinite(ambientExposure)||
       !std::isfinite(transientExposure)||!std::isfinite(windScale)||
       !std::isfinite(maxSpeed)||windScale<=0.f||maxSpeed<=0.f)return {};
    const float a=std::clamp(ambientExposure,0.f,1.f);
    const float t=std::clamp(transientExposure,0.f,1.f);
    const float am=Length(ambient),tm=Length(transient);
    const float denominator=am+tm;
    if(!(denominator>1.e-7f)||!std::isfinite(denominator))return {};
    const float exposure=std::clamp((am*a+tm*t)/denominator,0.f,1.f);
    if(!(exposure>1.e-7f))return {};
    // Uniform exposure E produces exactly: wind=(ambient+transient)*scale,
    // boneFactor*=E. Nonuniform shelter blends the component directions before
    // drag and uses an energy-weighted exposure. This cannot be perfectly
    // equivalent to two distinct quadratic force fields, but it preserves
    // cancellation and avoids squaring E.
    Vec3 wind=(ambient*a+transient*t)*(windScale/exposure);
    const float magnitude=Length(wind);
    if(!Finite(wind)||!std::isfinite(magnitude))return {};
    if(magnitude>maxSpeed)wind=wind*(maxSpeed/magnitude);
    return {wind,exposure};
}
Vec3 ComputeWindForce(const WindInput& p) {
    constexpr float kMaxGustTime = 4096.0f;
    constexpr float kEpsilon = 1.1920929e-7f;
    constexpr float kWindDrag = 1.0e-6f;
    constexpr float kGustSpeedPerAcceleration = 0.05f;
    constexpr float kMinGustSpeed = 180.0f;
    constexpr float kMaxGustSpeed = 1200.0f;
    constexpr float kCrosswindTimeScale = 0.004f;
    constexpr float kVerticalPhaseScale = 0.006f;

    if (!Finite(p.wind) || !Finite(p.worldAxis) || !Finite(p.worldOrigin) || !Finite(p.linearVelocity) ||
        !std::isfinite(p.area) || !std::isfinite(p.boneFactor) || !std::isfinite(p.mass) ||
        !std::isfinite(p.maxWindAcceleration) || p.maxWindAcceleration < 0.f ||
        p.mass <= 0 || p.area <= kEpsilon || p.boneFactor <= kEpsilon)
        return {};
    const float magnitude = Length(p.wind);
    if (!(magnitude > kEpsilon)) return {};
    const Vec3 axis = Normalized(p.worldAxis);
    const Vec3 relativeWind = p.wind - p.linearVelocity;
    const Vec3 across = relativeWind - axis*Dot(axis,relativeWind);
    const float speed = Length(across);
    if (!(speed > kEpsilon)) return {};
    Vec3 force = across * (kWindDrag * p.area * p.boneFactor * speed);
    float forceLength = Length(force);
    const float maximumForce = p.mass * p.maxWindAcceleration;
    if (forceLength > maximumForce) {
        force = force * (maximumForce / forceLength);
        forceLength = maximumForce;
    }
    const float time = std::fmod(p.windTime, kMaxGustTime);
    const Vec3 direction = p.wind / magnitude;
    const Vec3 up{0,0,1};
    const Vec3 rawSide = Cross(direction,up);
    const Vec3 side = Dot(rawSide,rawSide) < kEpsilon ? Vec3{1,0,0} : Normalized(rawSide);
    const float gustSpeed = std::clamp(magnitude*kGustSpeedPerAcceleration, kMinGustSpeed,kMaxGustSpeed);
    const Vec3 origin = p.worldOrigin * kBulletPerSkyrimUnit;
    const float advected = time - Dot(origin,direction)/gustSpeed - Dot(origin,side)*kCrosswindTimeScale;
    const float vertical = origin.z*kVerticalPhaseScale;
    const float longGust = std::sin(advected*0.55f + vertical*0.35f);
    const float midGust = std::sin(advected*1.35f + vertical + longGust*0.5f);
    const float flutter = std::sin(advected*4.15f + vertical*2.2f + midGust);
    const float pulse = 0.5f+0.5f*std::sin(advected*0.85f+vertical*0.6f);
    const float burst = pulse*pulse;
    const float sideways = (midGust*0.13f+flutter*0.06f+burst*0.04f)*forceLength;
    const float lift = (std::sin(advected*1.9f+vertical*1.4f)*0.018f+flutter*0.01f)*forceLength;
    const Vec3 result = force + side*sideways + up*lift;
    return Finite(result) ? result : Vec3{};
}
}
