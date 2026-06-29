#pragma once

#include <ecs/Entity.h>
#include <ecs/World.h>
#include <math/math.h>
#include <scene/SceneEcs.h>
#include <scene/SceneObjects.h>

#include <memory>

namespace caustica::scene
{

[[nodiscard]] SceneContentFlags GetCameraContentFlags();

[[nodiscard]] dm::affine3 GetCameraViewToWorldMatrix(const dm::daffine3& globalTransform);
[[nodiscard]] dm::affine3 GetCameraWorldToViewMatrix(const dm::daffine3& globalTransform);

[[nodiscard]] bool IsPerspectiveCamera(const CameraComponent& component);
[[nodiscard]] bool IsOrthographicCamera(const CameraComponent& component);

[[nodiscard]] const PerspectiveCameraData* TryGetPerspectiveCameraData(const CameraComponent& component);
[[nodiscard]] PerspectiveCameraData* TryGetPerspectiveCameraData(CameraComponent& component);
[[nodiscard]] const OrthographicCameraData* TryGetOrthographicCameraData(const CameraComponent& component);
[[nodiscard]] OrthographicCameraData* TryGetOrthographicCameraData(CameraComponent& component);

[[nodiscard]] bool SetCameraProperty(CameraComponent& component, const std::string& propName, const dm::float4& value);

void InitializeCameraComponent(CameraComponent& component, const SceneCamera& camera);
void InitializeCameraComponent(CameraComponent& component, const std::shared_ptr<SceneCamera>& camera);

[[nodiscard]] const CameraComponent* TryGetCamera(const ecs::World& world, ecs::Entity entity);
[[nodiscard]] CameraComponent* TryGetCamera(ecs::World& world, ecs::Entity entity);

} // namespace caustica::scene
