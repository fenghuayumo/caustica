#include <physics/PhysicsPlugin.h>

#include <engine/App.h>
#include <engine/AppSchedules.h>
#include <engine/SystemSets.h>
#include <scene/SceneEcs.h>

#include <algorithm>
#include <cmath>

namespace caustica::physics
{
namespace
{

struct PhysicsFixedStep
{
    static constexpr const char* name = "Physics.FixedStep";
};

struct PhysicsSyncTransforms
{
    static constexpr const char* name = "Physics.SyncTransforms";
};

bool same(const RigidBodyComponent& a, const RigidBodyComponent& b)
{
    return a.type == b.type && a.mass == b.mass && a.gravity == b.gravity
        && all(a.linearVelocity == b.linearVelocity) && all(a.angularVelocity == b.angularVelocity);
}

bool same(const ColliderComponent& a, const ColliderComponent& b)
{
    return a.shape == b.shape && all(a.dimensions == b.dimensions)
        && a.staticFriction == b.staticFriction && a.dynamicFriction == b.dynamicFriction
        && a.restitution == b.restitution && a.isTrigger == b.isTrigger;
}

bool same(const PhysicsBodyDescriptor& a, const PhysicsBodyDescriptor& b)
{
    return same(a.body, b.body) && same(a.collider, b.collider);
}

PhysicsPose makeWorldPose(
    const scene::LocalTransformComponent& local,
    const scene::GlobalTransformComponent& global)
{
    PhysicsPose pose{ local.translation, local.rotation };
    math::double3 ignoredScale;
    decomposeAffine<double>(global.transform, &pose.translation, &pose.rotation, &ignoredScale);
    return pose;
}

bool writeLocalPoseFromWorld(
    ecs::World& world,
    ecs::Entity entity,
    scene::LocalTransformComponent& local,
    const PhysicsPose& pose)
{
    math::daffine3 parentToWorld = math::daffine3::identity();
    if (const auto* parent = world.tryGet<scene::ParentComponent>(entity);
        parent && ecs::isValid(parent->parent))
    {
        const auto* parentGlobal = world.tryGet<scene::GlobalTransformComponent>(parent->parent);
        if (!parentGlobal)
            return false;
        parentToWorld = parentGlobal->transform;
    }

    const math::daffine3 desiredWorld =
        math::scaling(local.scaling) * pose.rotation.toAffine() * math::translation(pose.translation);
    // Scene transforms are row-vector based: world = local * parent.
    const math::daffine3 localToParent = desiredWorld * inverse(parentToWorld);
    math::double3 translation;
    math::dquat rotation;
    math::double3 scaling;
    decomposeAffine<double>(localToParent, &translation, &rotation, &scaling);
    if (any(local.translation != translation) || any(local.rotation != rotation) || any(local.scaling != scaling))
    {
        local.translation = translation;
        local.rotation = rotation;
        local.scaling = scaling;
        local.hasLocalTransform = true;
        local.compose();
        world.notifyComponentChanged<scene::LocalTransformComponent>(entity);
    }
    return true;
}

PhysicsPose interpolate(const PhysicsPose& from, const PhysicsPose& to, float alpha)
{
    const double t = std::clamp(double(alpha), 0.0, 1.0);
    PhysicsPose result;
    result.translation = from.translation * (1.0 - t) + to.translation * t;
    // Normalized lerp is robust for the short intervals between rigid-body
    // ticks. Canonicalize the sign first because q and -q are identical.
    math::dquat target = to.rotation;
    if (math::dot(from.rotation, target) < 0.0)
        target = -target;
    const double w = from.rotation.w * (1.0 - t) + target.w * t;
    const double x = from.rotation.x * (1.0 - t) + target.x * t;
    const double y = from.rotation.y * (1.0 - t) + target.y * t;
    const double z = from.rotation.z * (1.0 - t) + target.z * t;
    const double length = std::sqrt(w * w + x * x + y * y + z * z);
    result.rotation = length > 0.0 ? math::dquat(w / length, x / length, y / length, z / length) : math::dquat::identity();
    return result;
}

void createMissingBodies(ecs::World& world, PhysicsRuntime& runtime)
{
    for (auto it = runtime.previousPoses.begin(); it != runtime.previousPoses.end();)
    {
        if (!world.isAlive(it->first))
        {
            runtime.backend->destroyBody(it->first);
            runtime.currentPoses.erase(it->first);
            runtime.bodyDescriptors.erase(it->first);
            it = runtime.previousPoses.erase(it);
        }
        else
            ++it;
    }

    // Actors whose required ECS components were removed must leave the scene
    // even though their entity may remain alive.
    for (auto it = runtime.previousPoses.begin(); it != runtime.previousPoses.end();)
    {
        const ecs::Entity entity = it->first;
        if (!world.tryGet<RigidBodyComponent>(entity) || !world.tryGet<ColliderComponent>(entity)
            || !world.tryGet<scene::LocalTransformComponent>(entity))
        {
            runtime.backend->destroyBody(entity);
            runtime.currentPoses.erase(entity);
            runtime.bodyDescriptors.erase(entity);
            it = runtime.previousPoses.erase(it);
        }
        else
            ++it;
    }

    world.each<RigidBodyComponent, ColliderComponent, scene::LocalTransformComponent, scene::GlobalTransformComponent>(
        [&runtime](ecs::Entity entity, RigidBodyComponent& body, ColliderComponent& collider,
            scene::LocalTransformComponent& local, scene::GlobalTransformComponent& global) {
            const PhysicsBodyDescriptor descriptor{ body, collider };
            if (const auto existing = runtime.bodyDescriptors.find(entity);
                existing != runtime.bodyDescriptors.end() && same(existing->second, descriptor))
                return;

            if (runtime.previousPoses.contains(entity))
            {
                runtime.backend->destroyBody(entity);
                runtime.previousPoses.erase(entity);
                runtime.currentPoses.erase(entity);
            }
            const PhysicsPose pose = makeWorldPose(local, global);
            if (runtime.backend->createBody(entity, body, collider, pose))
            {
                runtime.previousPoses.emplace(entity, pose);
                runtime.currentPoses.emplace(entity, pose);
                runtime.bodyDescriptors.insert_or_assign(entity, descriptor);
            }
        });
}

void synchronizePoses(ecs::World& world, PhysicsRuntime& runtime)
{
    world.each<RigidBodyComponent, scene::LocalTransformComponent>(
        [&runtime, &world](ecs::Entity entity, RigidBodyComponent&, scene::LocalTransformComponent& transform) {
            const auto current = runtime.currentPoses.find(entity);
            if (current == runtime.currentPoses.end())
                return;
            const auto previous = runtime.previousPoses.find(entity);
            const PhysicsPose pose = previous != runtime.previousPoses.end()
                ? interpolate(previous->second, current->second, runtime.accumulatorSeconds / runtime.fixedDeltaSeconds)
                : current->second;
            writeLocalPoseFromWorld(world, entity, transform, pose);
        });
}

} // namespace

void PhysicsPlugin::build(App& app)
{
    if (!m_backend || app.tryResource<PhysicsRuntime>())
        return;

    PhysicsRuntime runtime;
    runtime.backend = std::move(m_backend);
    app.insertResource(std::move(runtime));
}

void PhysicsPlugin::configureSchedules(App& app)
{
    // Systems remain absent when PhysX was not compiled or no custom backend
    // was supplied. Pure rendering retains its original binary/runtime path.
    if (!app.tryResource<PhysicsRuntime>())
        return;

    app.addSystem<PhysicsFixedStep>(
        AppSchedule::update,
        [](SystemContext& ctx) {
            PhysicsRuntime& runtime = ctx.resMut<PhysicsRuntime>();
            if (!runtime.backend || ctx.deltaTimeSeconds <= 0.f)
                return;

            createMissingBodies(ctx.world, runtime);
            runtime.accumulatorSeconds += ctx.deltaTimeSeconds;
            const float maxAccumulated = runtime.fixedDeltaSeconds * float(runtime.maxSubsteps);
            runtime.accumulatorSeconds = std::min(runtime.accumulatorSeconds, maxAccumulated);
            uint32_t steps = 0;
            while (runtime.accumulatorSeconds >= runtime.fixedDeltaSeconds && steps++ < runtime.maxSubsteps)
            {
                runtime.backend->step(runtime.fixedDeltaSeconds);
                for (auto& [entity, current] : runtime.currentPoses)
                {
                    runtime.previousPoses[entity] = current;
                    PhysicsPose next;
                    if (runtime.backend->pose(entity, next))
                        current = next;
                }
                runtime.accumulatorSeconds -= runtime.fixedDeltaSeconds;
            }
        },
        AppSystemOrdering{}.inSet<system_set::Simulation>());

    // This executes before the engine's PostUpdate transform propagation, so
    // Extract observes the current physics pose through normal render proxies.
    app.addSystem<PhysicsSyncTransforms>(
        AppSchedule::PostUpdate,
        [](SystemContext& ctx) {
            PhysicsRuntime& runtime = ctx.resMut<PhysicsRuntime>();
            if (runtime.backend)
                synchronizePoses(ctx.world, runtime);
        },
        AppSystemOrdering{}.runBefore<system_set::TransformPropagate>());
}

} // namespace caustica::physics
