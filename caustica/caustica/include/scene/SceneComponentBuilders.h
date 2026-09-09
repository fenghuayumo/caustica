#pragma once

#include <scene/SceneEcs.h>
#include <physics/Physics.h>

#include <optional>
#include <string>
#include <variant>

namespace Json
{
class Value;
}

namespace caustica::scene
{

using AnyLightComponent = std::variant<
    DirectionalLightComponent,
    SpotLightComponent,
    PointLightComponent,
    RectLightComponent,
    EnvironmentLightComponent>;

[[nodiscard]] bool isJsonLightLeafType(const std::string& type);
[[nodiscard]] bool isJsonCameraLeafType(const std::string& type);

// Build ECS components from scene-JSON leaf nodes (no OO Light/SceneCamera).
[[nodiscard]] std::optional<AnyLightComponent> makeLightComponentFromJson(
    const std::string& type, const Json::Value& src);
[[nodiscard]] std::optional<CameraComponent> makeCameraComponentFromJson(
    const std::string& type, const Json::Value& src);

[[nodiscard]] bool isJsonPhysicsLeafType(const std::string& type);
[[nodiscard]] std::optional<physics::RigidBodyComponent> makeRigidBodyComponentFromJson(
    const Json::Value& src);
[[nodiscard]] std::optional<physics::ColliderComponent> makeColliderComponentFromJson(
    const Json::Value& src);
void writeRigidBodyComponent(Json::Value& dst, const physics::RigidBodyComponent& body);
void writeColliderComponent(Json::Value& dst, const physics::ColliderComponent& collider);

} // namespace caustica::scene
