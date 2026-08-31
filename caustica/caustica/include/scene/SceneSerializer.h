#pragma once

#include <ecs/Entity.h>
#include <json/json.h>

#include <string>
#include <vector>

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

// Prefab-internal lights/cameras (no SceneAuthoringId) written by path.
void patchEntityOverrides(
    Json::Value& document,
    SceneEntityWorld& world);

// Insert or refresh a scene-JSON entity for an authored ECS node (editor create / save).
void upsertAuthoredEntityNode(
    Json::Value& document,
    SceneEntityWorld& world,
    ecs::Entity entity);

void removeAuthoredEntityNode(Json::Value& document, const std::string& id);

void syncAuthoredEntitiesToDocument(Json::Value& document, SceneEntityWorld& world);

void applyEntityOverrides(
    SceneEntityWorld& world,
    const Json::Value& overrides);

// Disable only the listed paths. Unknown paths are skipped. Never hides unlisted entities.
void applyHiddenEntities(
    SceneEntityWorld& world,
    const std::vector<std::string>& paths);

} // namespace caustica::scene
