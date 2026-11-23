// Source/API ABI: DaymareOn/hdtSMP64, src/PluginAPI.h (dev).
// License: GPL-3.0; https://github.com/DaymareOn/hdtSMP64
#pragma once
#include <RE/Skyrim.h>

template <typename T> class btAlignedObjectArray;
class btCollisionObject;

namespace hdt {
    struct PreStepEvent {
        const btAlignedObjectArray<btCollisionObject*>& objects;
        float timeStep{0.0f};
    };
    struct PostStepEvent {
        const btAlignedObjectArray<btCollisionObject*>& objects;
        float timeStep{0.0f};
    };
    using IPreStepListener = RE::BSTEventSink<PreStepEvent>;
    using IPostStepListener = RE::BSTEventSink<PostStepEvent>;
    class PluginInterface {
    public:
        enum MessageType : unsigned long { MSG_STARTUP };
        struct Version { int major; int minor; int patch; };
        struct VersionInfo { Version interfaceVersion; Version bulletVersion; };
        constexpr static Version INTERFACE_VERSION{2, 0, 0};
        constexpr static Version BULLET_VERSION{3, 24, 0};
        virtual ~PluginInterface() = default;
        virtual const VersionInfo& getVersionInfo() const = 0;
        virtual void addListener(IPreStepListener*) = 0;
        virtual void removeListener(IPreStepListener*) = 0;
        virtual void addListener(IPostStepListener*) = 0;
        virtual void removeListener(IPostStepListener*) = 0;
    };
}
