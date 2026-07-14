#pragma once

#include <scene/SceneEcs.h>

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
    EnvironmentLightComponent>;

[[nodiscard]] bool isJsonLightLeafType(const std::string& type);
[[nodiscard]] bool isJsonCameraLeafType(const std::string& type);

// Build ECS components from scene-JSON leaf nodes (no OO Light/SceneCamera).
[[nodiscard]] std::optional<AnyLightComponent> makeLightComponentFromJson(
    const std::string& type, const Json::Value& src);
[[nodiscard]] std::optional<CameraComponent> makeCameraComponentFromJson(
    const std::string& type, const Json::Value& src);

} // namespace caustica::scene
