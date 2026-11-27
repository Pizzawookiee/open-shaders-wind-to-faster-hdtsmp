#pragma once
#include <Windows.h>
#include "OpenShaders/CSinterface001.h"
namespace osfsmp {
// SEH leaf: no C++ objects requiring unwinding in this function.
// Inspired by GPUSMPPhysicsNG GuardedCall (fdd74793). Do not use partially
// written samples after a fault. Ordinary false returns remain retryable.
inline bool GuardedWindSample(CSPluginAPI::ICSInterface001* api,
    const CSPluginAPI::WindVector* positions,
    CSPluginAPI::WindSampleWithHavokExclusion* samples,
    std::uint32_t count, bool& faulted) noexcept
{
    faulted=false;
    if(!api || !positions || !samples || !count ||
       count>CSPluginAPI::CSWindMaximumBatchSize)return false;
    __try {
        return api->SampleWindExcludingHavokImpulses(positions,samples,count);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        faulted=true;
        return false;
    }
}
} // namespace osfsmp
