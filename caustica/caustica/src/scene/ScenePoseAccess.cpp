#include <scene/ScenePoseAccess.h>

#include <scene/SceneEcs.h>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace
{

bool NormalizePoseRotation(caustica::math::dquat& rotation)
{
    const double norm = caustica::math::length(rotation);
    if (!std::isfinite(norm) || norm <= 1e-12)
        return false;
    rotation /= norm;
    return true;
}

} // namespace

namespace caustica::scene
{

bool getEntityLocalPose(const SceneEntityWorld& world, ecs::Entity entity, EntityPose& out)
{
    const ecs::World& ecsWorld = world.world();
    if (!ecs::isValid(entity) || !ecsWorld.isAlive(entity))
        return false;
    const auto* local = ecsWorld.tryGet<LocalTransformComponent>(entity);
    if (!local)
        return false;
    out.position = local->translation;
    out.rotation = local->rotation;
    out.scaling = local->scaling;
    return true;
}

bool getEntityWorldPose(const SceneEntityWorld& world, ecs::Entity entity, EntityPose& out)
{
    const ecs::World& ecsWorld = world.world();
    if (!ecs::isValid(entity) || !ecsWorld.isAlive(entity))
        return false;
    const auto* global = ecsWorld.tryGet<GlobalTransformComponent>(entity);
    if (!global)
        return false;
    decomposeAffine<double>(global->transform, &out.position, &out.rotation, &out.scaling);
    return true;
}

bool setEntityLocalPose(SceneEntityWorld& world, ecs::Entity entity, const EntityPose& pose)
{
    if (!ecs::isValid(entity) || !world.world().isAlive(entity))
        return false;
    if (!math::all(math::isfinite(pose.position))
        || !math::all(math::isfinite(pose.scaling)))
        return false;
    math::dquat normalizedRotation = pose.rotation;
    if (!NormalizePoseRotation(normalizedRotation))
        return false;
    world.setLocalTransform(entity, &pose.position, &normalizedRotation, &pose.scaling);
    world.refreshHierarchy();
    return true;
}

namespace
{

bool WriteWorldPoseNoRefresh(SceneEntityWorld& world, ecs::Entity entity, const EntityPose& pose)
{
    ecs::World& ecsWorld = world.world();
    if (!ecs::isValid(entity) || !ecsWorld.isAlive(entity))
        return false;
    if (!math::all(math::isfinite(pose.position))
        || !math::all(math::isfinite(pose.scaling)))
        return false;
    math::dquat normalizedRotation = pose.rotation;
    if (!NormalizePoseRotation(normalizedRotation))
        return false;

    math::daffine3 parentToWorld = math::daffine3::identity();
    if (const auto* parent = ecsWorld.tryGet<ParentComponent>(entity))
    {
        if (ecs::isValid(parent->parent))
        {
            const auto* parentGlobal = ecsWorld.tryGet<GlobalTransformComponent>(parent->parent);
            if (!parentGlobal)
                return false;
            parentToWorld = parentGlobal->transform;
        }
    }

    const math::daffine3 desiredWorld =
        math::scaling(pose.scaling) * normalizedRotation.toAffine() * math::translation(pose.position);
    // Scene hierarchy composition is row-vector based: world = local * parent.
    const math::daffine3 localToParent = desiredWorld * inverse(parentToWorld);
    math::double3 translation;
    math::dquat rotation;
    math::double3 scaling;
    decomposeAffine<double>(localToParent, &translation, &rotation, &scaling);
    world.setLocalTransform(entity, &translation, &rotation, &scaling);
    return true;
}

void PublishGlobalFromLocal(SceneEntityWorld& world, ecs::Entity entity)
{
    ecs::World& ecsWorld = world.world();
    auto* local = ecsWorld.tryGet<LocalTransformComponent>(entity);
    auto* global = ecsWorld.tryGet<GlobalTransformComponent>(entity);
    if (!local || !global)
        return;

    local->compose();
    math::daffine3 parentToWorld = math::daffine3::identity();
    if (const auto* parent = ecsWorld.tryGet<ParentComponent>(entity);
        parent && ecs::isValid(parent->parent))
    {
        if (const auto* parentGlobal = ecsWorld.tryGet<GlobalTransformComponent>(parent->parent))
            parentToWorld = parentGlobal->transform;
    }
    global->transform = local->hasLocalTransform ? local->transform * parentToWorld : parentToWorld;
    global->transformFloat = math::affine3(global->transform);
}

int HierarchyDepth(const SceneEntityWorld& world, ecs::Entity entity)
{
    int depth = 0;
    ecs::Entity current = entity;
    while (ecs::isValid(current) && depth < 4096)
    {
        const auto* parent = world.world().tryGet<ParentComponent>(current);
        if (!parent || !ecs::isValid(parent->parent))
            break;
        current = parent->parent;
        ++depth;
    }
    return depth;
}

} // namespace

bool setEntityWorldPose(SceneEntityWorld& world, ecs::Entity entity, const EntityPose& pose)
{
    if (!WriteWorldPoseNoRefresh(world, entity, pose))
        return false;
    world.refreshHierarchy();
    return true;
}

bool setEntityWorldPoses(
    SceneEntityWorld& world,
    const ecs::Entity* entities,
    const EntityPose* poses,
    size_t count)
{
    if (count == 0)
        return true;
    if (!entities || !poses)
        return false;

    std::vector<size_t> order(count);
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        return HierarchyDepth(world, entities[a]) < HierarchyDepth(world, entities[b]);
    });

    for (size_t index : order)
    {
        if (!WriteWorldPoseNoRefresh(world, entities[index], poses[index]))
            return false;
        PublishGlobalFromLocal(world, entities[index]);
    }

    world.refreshHierarchy();
    return true;
}

} // namespace caustica::scene
