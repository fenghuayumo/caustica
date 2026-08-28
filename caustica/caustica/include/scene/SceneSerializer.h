#pragma once

#include <ecs/Entity.h>
#include <json/json.h>

#include <string>

namespace Json
{
class Value;
}

namespace caustica::scene
{

class SceneEntityWorld;

[[nodiscard]] Json::Value mergeSceneOverlay(const Json::Value& base, const Json::Value& overlay);

// `procedural:sky` → engine sky sentinel. Other strings pass through.
[[nodiscard]] std::string canonicalizeEnvSource(std::string source);

void applyAuthoringTransform(
    SceneEntityWorld& world,
    ecs::Entity entity,
    const Json::Value& transform);

void patchEntityTransforms(
    Json::Value& entities,
    SceneEntityWorld& world);

} // namespace caustica::scene
