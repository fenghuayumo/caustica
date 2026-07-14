#pragma once

#include <scene/SceneEcs.h>

#include <optional>
#include <string>

namespace Json
{
class Value;
}

namespace caustica::scene
{

[[nodiscard]] bool isJsonLightLeafType(const std::string& type);
[[nodiscard]] bool isJsonCameraLeafType(const std::string& type);

// Build ECS components from scene-JSON leaf nodes (no OO Light/SceneCamera).
[[nodiscard]] std::optional<LightComponent> makeLightComponentFromJson(
    const std::string& type, const Json::Value& src);
[[nodiscard]] std::optional<CameraComponent> makeCameraComponentFromJson(
    const std::string& type, const Json::Value& src);

} // namespace caustica::scene
