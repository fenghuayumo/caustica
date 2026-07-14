#pragma once

#include <ecs/Entity.h>
#include <ecs/World.h>
#include <math/math.h>
#include <scene/SceneEcs.h>

namespace caustica::scene
{

[[nodiscard]] SceneContentFlags getCameraContentFlags();

[[nodiscard]] dm::affine3 getCameraViewToWorldMatrix(const dm::daffine3& globalTransform);
[[nodiscard]] dm::affine3 getCameraWorldToViewMatrix(const dm::daffine3& globalTransform);

[[nodiscard]] bool isPerspectiveCamera(const CameraComponent& component);
[[nodiscard]] bool isOrthographicCamera(const CameraComponent& component);

[[nodiscard]] const PerspectiveCameraData* tryGetPerspectiveCameraData(const CameraComponent& component);
[[nodiscard]] PerspectiveCameraData* tryGetPerspectiveCameraData(CameraComponent& component);
[[nodiscard]] const OrthographicCameraData* tryGetOrthographicCameraData(const CameraComponent& component);
[[nodiscard]] OrthographicCameraData* tryGetOrthographicCameraData(CameraComponent& component);

[[nodiscard]] bool setCameraProperty(CameraComponent& component, const std::string& propName, const dm::float4& value);

[[nodiscard]] const CameraComponent* tryGetCamera(const ecs::World& world, ecs::Entity entity);
[[nodiscard]] CameraComponent* tryGetCamera(ecs::World& world, ecs::Entity entity);

} // namespace caustica::scene
