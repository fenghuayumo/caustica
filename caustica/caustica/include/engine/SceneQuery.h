#pragma once

#include <engine/SceneAccess.h>
#include <ecs/Entity.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace caustica
{

class App;
class Material;
class Scene;

[[nodiscard]] std::shared_ptr<Scene> activeScene(const App& app);
void syncSceneAccess(App& app);
[[nodiscard]] scene::SceneEntityWorld* entityWorld(const App& app);
[[nodiscard]] ecs::World* sceneEcs(const App& app);

[[nodiscard]] const std::vector<std::string>& availableScenes(const App& app);
[[nodiscard]] std::string currentSceneName(const App& app);
[[nodiscard]] std::filesystem::path currentScenePath(const App& app);
[[nodiscard]] bool isSceneStructureBusy(const App& app);
[[nodiscard]] bool isSceneLoading(const App& app);
[[nodiscard]] bool isSceneLoaded(const App& app);
[[nodiscard]] bool shouldSkipRender(const App& app);
[[nodiscard]] bool shouldRenderWhenUnfocused(const App& app);

[[nodiscard]] std::shared_ptr<Material> findMaterial(const App& app, int materialID);
[[nodiscard]] ecs::Entity findEntityByInstanceIndex(const App& app, int instanceIndex);

} // namespace caustica
