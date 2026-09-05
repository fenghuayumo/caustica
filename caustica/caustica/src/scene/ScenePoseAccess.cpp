#include <scene/ScenePoseAccess.h>

#include <scene/SceneEcs.h>

#include <cmath>

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

bool setEntityWorldPose(SceneEntityWorld& world, ecs::Entity entity, const EntityPose& pose)
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
    // Therefore world-to-local must multiply the desired world transform on the
    // right by the inverse parent transform.
    const math::daffine3 localToParent = desiredWorld * inverse(parentToWorld);
    math::double3 translation;
    math::dquat rotation;
    math::double3 scaling;
    decomposeAffine<double>(localToParent, &translation, &rotation, &scaling);
    world.setLocalTransform(entity, &translation, &rotation, &scaling);
    world.refreshHierarchy();
    return true;
}

} // namespace caustica::scene
