#pragma once

// Optional simulation module.  Nothing in scene extraction, render proxies, or
// sensor output includes this header or a backend SDK header.

#include <ecs/Entity.h>
#include <math/math.h>

#include <cstdint>
#include <memory>
#include <unordered_map>

namespace caustica
{
class App;
struct Plugin;
}

namespace caustica::physics
{

enum class RigidBodyType : uint8_t
{
    Static,
    Dynamic,
    Kinematic,
};

enum class ColliderShape : uint8_t
{
    Box,
    Sphere,
    Capsule,
};

// Authoring-facing ECS state.  It intentionally contains no Px* types so the
// same scene/components work with PhysX, Jolt, MuJoCo, or a host backend.
struct RigidBodyComponent
{
    RigidBodyType type = RigidBodyType::Dynamic;
    float mass = 1.0f;
    bool gravity = true;
    dm::float3 linearVelocity = 0.f;
    dm::float3 angularVelocity = 0.f;
};

struct ColliderComponent
{
    ColliderShape shape = ColliderShape::Box;
    // Full box dimensions, sphere radius in x, capsule radius/height in x/y.
    dm::float3 dimensions = dm::float3(1.f);
    float staticFriction = 0.5f;
    float dynamicFriction = 0.5f;
    float restitution = 0.f;
    bool isTrigger = false;
};

struct PhysicsPose
{
    dm::double3 translation = 0.0;
    dm::dquat rotation = dm::dquat::identity();
};

// Snapshot of the authoring data used to create an actor. It makes an ECS
// component edit a deterministic actor rebuild rather than leaving stale SDK
// state in the simulation.
struct PhysicsBodyDescriptor
{
    RigidBodyComponent body;
    ColliderComponent collider;
};

// Backend boundary. Backends own their SDK objects and only exchange stable
// ECS entities and plain data across this interface.
class PhysicsBackend
{
public:
    virtual ~PhysicsBackend() = default;
    [[nodiscard]] virtual const char* name() const = 0;
    virtual bool createBody(ecs::Entity entity, const RigidBodyComponent&, const ColliderComponent&, const PhysicsPose&) = 0;
    virtual void destroyBody(ecs::Entity entity) = 0;
    virtual void step(float fixedDeltaSeconds) = 0;
    [[nodiscard]] virtual bool pose(ecs::Entity entity, PhysicsPose& outPose) const = 0;
};

// Runtime resource installed by PhysicsPlugin. `fixedDeltaSeconds` is a
// simulation cadence; presentation remains governed by Caustica render frames.
struct PhysicsRuntime
{
    std::unique_ptr<PhysicsBackend> backend;
    float fixedDeltaSeconds = 1.0f / 60.0f;
    float accumulatorSeconds = 0.f;
    uint32_t maxSubsteps = 4;
    // Kept separately so presentation may interpolate between fixed ticks
    // without changing the simulation state.
    std::unordered_map<ecs::Entity, PhysicsPose> previousPoses;
    std::unordered_map<ecs::Entity, PhysicsPose> currentPoses;
    std::unordered_map<ecs::Entity, PhysicsBodyDescriptor> bodyDescriptors;
};

// Returns nullptr in builds without CAUSTICA_WITH_PHYSX. This makes PhysX a
// feature selected by the host, never a hidden engine dependency.
[[nodiscard]] std::unique_ptr<PhysicsBackend> createPhysXBackend();
[[nodiscard]] bool isPhysXAvailable();

// Adds a fixed-step system in update and a pose synchronization system before
// TransformPropagate.  The plugin is opt-in: add it to EngineApp explicitly.
class PhysicsPlugin;

} // namespace caustica::physics
