#pragma once

#include <ecs/Entity.h>
#include <ecs/World.h>
#include <math/math.h>
#include <scene/SceneEcs.h>

#include <optional>
#include <string>

namespace caustica::scene
{

// Camera view-space pose (includes the renderer Z-flip). Not entity TRS:
// aiming a camera with EntityPose / world_pose points the wrong way.
struct CameraWorldLookTo
{
    dm::float3 position = { 0.f, 0.f, 0.f };
    dm::float3 direction = { 0.f, 0.f, -1.f };
    dm::float3 up = { 0.f, 1.f, 0.f };
};

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

[[nodiscard]] const PerspectiveCameraData* tryGetPerspectiveCamera(const SceneEntityWorld& world, ecs::Entity entity);
[[nodiscard]] PerspectiveCameraData* tryGetPerspectiveCamera(SceneEntityWorld& world, ecs::Entity entity);

[[nodiscard]] bool tryGetCameraWorldLookTo(const SceneEntityWorld& world, ecs::Entity entity, CameraWorldLookTo& out);
bool setCameraWorldLookTo(
    SceneEntityWorld& world,
    ecs::Entity entity,
    const dm::float3& position,
    const dm::float3& direction,
    const dm::float3& up);

bool setCameraVerticalFov(SceneEntityWorld& world, ecs::Entity entity, float radians);
[[nodiscard]] float getCameraVerticalFov(const SceneEntityWorld& world, ecs::Entity entity);

bool setCameraZNear(SceneEntityWorld& world, ecs::Entity entity, float zNear);
[[nodiscard]] float getCameraZNear(const SceneEntityWorld& world, ecs::Entity entity);

bool setCameraZFar(SceneEntityWorld& world, ecs::Entity entity, std::optional<float> zFar);
[[nodiscard]] std::optional<float> getCameraZFar(const SceneEntityWorld& world, ecs::Entity entity);

bool setCameraAspectRatio(SceneEntityWorld& world, ecs::Entity entity, std::optional<float> aspectRatio);
[[nodiscard]] std::optional<float> getCameraAspectRatio(const SceneEntityWorld& world, ecs::Entity entity);

bool setCameraIntrinsics(
    SceneEntityWorld& world,
    ecs::Entity entity,
    float fx,
    float fy,
    float cx,
    float cy,
    float width,
    float height);
bool clearCameraIntrinsics(SceneEntityWorld& world, ecs::Entity entity);
[[nodiscard]] const CameraIntrinsics* tryGetCameraIntrinsics(const SceneEntityWorld& world, ecs::Entity entity);

} // namespace caustica::scene
