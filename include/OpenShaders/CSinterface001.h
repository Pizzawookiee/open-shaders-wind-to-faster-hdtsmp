// Binary ABI contract based on alandtse/open-shaders/include/VRAPI/CSinterface001.h (dev).
// Original interface: LGPL-3.0-or-later. See LICENSE-THIRD-PARTY.md.
// All virtual slots through revision 4 MUST remain in order.
#pragma once
#include <cstdint>
#include <type_traits>

namespace CSPluginAPI {
    inline constexpr const char* CSPluginName = "CommunityShaders";
    inline constexpr std::uint32_t CSInterfaceMessageType = 0x43534150;
    inline constexpr std::uint32_t CSInterfaceRevision = 4;
    inline constexpr std::uint32_t CSWindMaximumBatchSize = 16384;
    struct CSMessage {
        enum : std::uint32_t { kMessage_GetInterface = CSInterfaceMessageType };
        void* (*GetApiFunction)(unsigned int) = nullptr;
    };
    enum class UpscalePreset : std::uint32_t {
        kNativeAA = 0, kDLAA = 0, kQuality = 1, kBalanced = 2,
        kPerformance = 3, kUltraPerformance = 4, kHoshipa = 5, kUltraQuality = 6
    };
    using DLSSMode = UpscalePreset;
    enum class DLSSProfile : std::uint32_t { kJ, kK, kL, kM, kF };
    enum class UpscaleMethod : std::uint32_t { kNone, kTAA, kFSR, kDLSS };
    struct WindVector { float x{}, y{}, z{}; };
    struct WindSample {
        WindVector baseVelocity{}, gustVelocity{}, transientVelocity{}, finalVelocity{};
        float ambientGust{}, transientIntensity{};
        std::uint64_t frameId{};
    };
    struct WindSampleWithHavokExclusion {
        WindSample wind{};
        WindVector windExcludingHavokImpulses{};
    };
    static_assert(sizeof(WindVector) == 12);
    static_assert(sizeof(WindSample) == 64);
    static_assert(sizeof(WindSampleWithHavokExclusion) == 80);
    static_assert(std::is_standard_layout_v<WindSampleWithHavokExclusion>);
    struct ICSInterface001 {
        virtual unsigned int getBuildNumber() = 0;
        virtual bool GetSSSEnabled() = 0;
        virtual void SetSSSEnabled(bool enabled) = 0;
        virtual bool GetSSGIEnabled() = 0;
        virtual void SetSSGIEnabled(bool enabled) = 0;
        virtual bool GetVolumetricLightingExteriorEnabled() = 0;
        virtual void SetVolumetricLightingExteriorEnabled(bool enabled) = 0;
        virtual UpscalePreset GetUpscalePreset() = 0;
        virtual void SetUpscalePreset(UpscalePreset preset) = 0;
        virtual bool GetLightLimitFixContactShadowsEnabled() = 0;
        virtual void SetLightLimitFixContactShadowsEnabled(bool enabled) = 0;
        virtual DLSSProfile GetDLSSProfile() = 0;
        virtual void SetDLSSProfile(DLSSProfile profile) = 0;
        virtual bool GetRenderAtUpscaleResEnabled() = 0;
        virtual void SetRenderAtUpscaleResEnabled(bool enabled) = 0;
        virtual bool GetRenderAtUpscaleResActive() = 0;
        virtual void SetVRUpscalingTransitionProfile(bool enabled, UpscalePreset preset, DLSSProfile profile) = 0;
        virtual UpscaleMethod GetUpscaleMethod() = 0;
        virtual void SetUpscaleMethod(UpscaleMethod method) = 0;
        virtual void SetVRUpscalingTransitionProfileForMethod(UpscaleMethod method, bool enabled, UpscalePreset preset, DLSSProfile profile) = 0;
        virtual std::uint32_t GetVRUpscalingApplyBlockReasons() = 0;
        virtual bool IsVRUpscalingProfileApplyAllowed() = 0;
        virtual bool SampleWind(const WindVector* positions, WindSample* samples, std::uint32_t count) = 0;
        virtual bool SampleWindExcludingHavokImpulses(const WindVector* positions, WindSampleWithHavokExclusion* samples, std::uint32_t count) = 0;
    };
}
