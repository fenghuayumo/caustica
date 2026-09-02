#pragma once

#include <ecs/Entity.h>
#include <math/math.h>
#include <scene/SceneEcs.h>

namespace caustica::scene
{

// Entity TRS. Compose is S * R * T (same as LocalTransformComponent::compose).
// Quaternion order is XYZW; scaling is component-wise.
// This is not camera view space — aim cameras with setCameraWorldLookTo / look_to.
struct EntityPose
{
    dm::double3 position = dm::double3(0.0);
    dm::dquat rotation = dm::dquat::identity();
    dm::double3 scaling = dm::double3(1.0);
};

[[nodiscard]] bool getEntityLocalPose(
    const SceneEntityWorld& world, ecs::Entity entity, EntityPose& out);
[[nodiscard]] bool getEntityWorldPose(
    const SceneEntityWorld& world, ecs::Entity entity, EntityPose& out);
bool setEntityLocalPose(SceneEntityWorld& world, ecs::Entity entity, const EntityPose& pose);
bool setEntityWorldPose(SceneEntityWorld& world, ecs::Entity entity, const EntityPose& pose);

} // namespace caustica::scene
