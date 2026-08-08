#pragma once

#include <engine/ActiveScene.h>
#include <ecs/Entity.h>
#include <scene/SceneEcs.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace caustica
{

class App;
class Material;

// Metadata only — does not expose extract/GPU Scene digs.
[[nodiscard]] const ActiveScene* tryActiveScene(const App& app);

[[nodiscard]] scene::SceneEntityWorld* entityWorld(const App& app);
[[nodiscard]] ecs::World* sceneEcs(const App& app);

[[nodiscard]] const std::vector<std::string>& availableScenes(const App& app);
[[nodiscard]] std::string currentSceneName(const App& app);
[[nodiscard]] std::filesystem::path currentScenePath(const App& app);
// True only while starting another scene switch would race an active load or
// structure edit. Background OMM/opacity streaming is cancelled by teardown.
[[nodiscard]] bool isSceneSwitchBusy(const App& app);
[[nodiscard]] bool isSceneStructureBusy(const App& app);
[[nodiscard]] bool isSceneLoading(const App& app);
[[nodiscard]] bool isSceneLoaded(const App& app);
[[nodiscard]] bool shouldSkipRender(const App& app);
[[nodiscard]] bool shouldRenderWhenUnfocused(const App& app);

// Resolve by path-tracer pick id (StandardMaterial::gpuDataIndex). Not Material::materialID.
[[nodiscard]] std::shared_ptr<Material> findMaterial(const App& app, int materialID);
[[nodiscard]] ecs::Entity findEntityByInstanceIndex(const App& app, int instanceIndex);

// Hierarchy path lookup on the live scene ECS (same rules as SceneEntityWorld::findEntity).
[[nodiscard]] ecs::Entity findEntity(
    const App& app,
    const std::filesystem::path& path,
    ecs::Entity context = ecs::NullEntity);

} // namespace caustica
