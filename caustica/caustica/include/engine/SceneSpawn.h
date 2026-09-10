#pragma once

#include <assets/Handle.h>
#include <assets/TypedAssets.h>
#include <ecs/Entity.h>
#include <math/math.h>
#include <scene/SceneApply.h>
#include <scene/SceneEcs.h>

#include <filesystem>
#include <optional>
#include <string>

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
// Prefab / builtin source string (e.g. "builtin:cube", "prefabs/foo.prefab.json").
[[nodiscard]] ecs::Entity spawnFromSource(
    App& app,
    const std::string& source,
    const SceneApplyCallbacks& callbacks = {});
[[nodiscard]] bool despawn(App& app, ecs::Entity entity);

[[nodiscard]] ecs::Entity spawnDirectionalLight(
    App& app, scene::DirectionalLightComponent component, const std::string& name = {});
[[nodiscard]] ecs::Entity spawnSpotLight(
    App& app, scene::SpotLightComponent component, const std::string& name = {});
[[nodiscard]] ecs::Entity spawnPointLight(
    App& app, scene::PointLightComponent component, const std::string& name = {});
[[nodiscard]] ecs::Entity spawnRectLight(
    App& app, scene::RectLightComponent component, const std::string& name = {});
[[nodiscard]] ecs::Entity spawnEnvironmentLight(
    App& app, scene::EnvironmentLightComponent component, const std::string& name = {});

struct SpawnCameraDesc
{
    std::string name;
    ecs::Entity parent = ecs::NullEntity;
    math::double3 localTranslation = { 0.0, 0.0, 0.0 };
    math::dquat localRotation = math::dquat::identity();
    math::double3 localScaling = { 1.0, 1.0, 1.0 };
    float verticalFov = 0.7f;
    float zNear = 0.001f;
    std::optional<scene::CameraIntrinsics> intrinsics;
};

[[nodiscard]] ecs::Entity spawnCamera(App& app, SpawnCameraDesc desc);

void ensureRectLightVisual(App& app, ecs::Entity entity);
void syncRectLightVisual(App& app, ecs::Entity entity);

} // namespace caustica
