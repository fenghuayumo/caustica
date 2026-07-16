#pragma once

#include <assets/Handle.h>
#include <assets/TypedAssets.h>
#include <ecs/Entity.h>
#include <scene/SceneApply.h>

#include <filesystem>

namespace caustica
{

class App;

// Bevy-style assets.load + spawn. Extract owns GPU upload / AS rebuild.
[[nodiscard]] Handle<ScenePrefabAsset> load(App& app, const std::filesystem::path& path);
[[nodiscard]] ecs::Entity spawn(
    App& app,
    const Handle<ScenePrefabAsset>& prefab,
    const SceneApplyCallbacks& callbacks = {});
[[nodiscard]] ecs::Entity spawnFromFile(
    App& app,
    const std::filesystem::path& path,
    const SceneApplyCallbacks& callbacks = {});
[[nodiscard]] bool despawn(App& app, ecs::Entity entity);

} // namespace caustica
