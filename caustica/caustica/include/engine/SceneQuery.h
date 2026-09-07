#pragma once

#include <engine/ActiveScene.h>
#include <ecs/Entity.h>
#include <scene/SceneEcs.h>
#include <scene/SceneImport.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace caustica
{

class App;
class Material;
class SceneTypeFactory;
struct GameSettings;

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

struct SceneLoadStatus
{
    const char* phaseName = "Idle";
    bool busy = false;
    int progressPercent = 0;
    bool gpuStreaming = false;
    uint32_t streamStep = 0;
    size_t texturesRemaining = 0;
    size_t meshBegin = 0;
    size_t meshTotal = 0;
    bool stepInFlight = false;
};

[[nodiscard]] SceneLoadStatus sceneLoadStatus(const App& app);

[[nodiscard]] const GameSettings* gameSettings(const App& app);
[[nodiscard]] const std::vector<SceneImportResult>& importedModels(const App& app);
[[nodiscard]] std::shared_ptr<SceneTypeFactory> sceneTypeFactory(const App& app);

// Resolve by path-tracer pick id (StandardMaterial::gpuDataIndex). Not Material::materialID.
[[nodiscard]] std::shared_ptr<Material> findMaterial(const App& app, int materialID);
// Populate a live imported MaterialEx from the path-tracer material cache.
[[nodiscard]] std::shared_ptr<Material> linkRuntimeMaterialData(
    const App& app,
    const std::shared_ptr<Material>& material);
[[nodiscard]] ecs::Entity findEntityByInstanceIndex(const App& app, int instanceIndex);

// Hierarchy path lookup on the live scene ECS (same rules as SceneEntityWorld::findEntity).
[[nodiscard]] ecs::Entity findEntity(
    const App& app,
    const std::filesystem::path& path,
    ecs::Entity context = ecs::NullEntity);

} // namespace caustica
