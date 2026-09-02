#pragma once

#include <ecs/Entity.h>

#include <filesystem>

struct PathTracerSettings;

namespace caustica::render
{
class SceneGaussianSplatPasses;
}

namespace caustica
{

class App;

// Logic-domain adapter for Gaussian splat ECS authoring. The render library owns
// only GPU-facing pass state and has no dependency on this downstream engine layer.
class SceneGaussianSplatLogic
{
public:
    static void onSceneLoaded(
        render::SceneGaussianSplatPasses& passes,
        PathTracerSettings& settings,
        App& app);
    static bool loadFromFile(
        render::SceneGaussianSplatPasses& passes,
        PathTracerSettings& settings,
        const std::filesystem::path& fileName,
        App& app,
        bool convertRdfToRub = true);
    static bool removeObjectsUnderEntity(
        render::SceneGaussianSplatPasses& passes,
        ecs::Entity rootEntity,
        App& app);

private:
    static void loadFromSceneEntities(
        render::SceneGaussianSplatPasses& passes,
        PathTracerSettings& settings);
    static bool attachToScene(
        render::SceneGaussianSplatPasses& passes,
        PathTracerSettings& settings,
        const std::filesystem::path& fileName,
        bool convertRdfToRub);
};

} // namespace caustica
