#pragma once

#include <ecs/Entity.h>

#include <filesystem>

namespace caustica::render
{
class SceneGaussianSplatPasses;
}

namespace caustica
{

// Logic-domain adapter for Gaussian splat ECS authoring. The render library owns
// only GPU-facing pass state and has no dependency on this downstream engine layer.
class SceneGaussianSplatLogic
{
public:
    static void onSceneLoaded(render::SceneGaussianSplatPasses& passes);
    static bool loadFromFile(
        render::SceneGaussianSplatPasses& passes,
        const std::filesystem::path& fileName,
        bool convertRdfToRub = true);
    static bool removeObjectsUnderEntity(
        render::SceneGaussianSplatPasses& passes,
        ecs::Entity rootEntity);

private:
    static void loadFromSceneEntities(render::SceneGaussianSplatPasses& passes);
    static bool attachToScene(
        render::SceneGaussianSplatPasses& passes,
        const std::filesystem::path& fileName,
        bool convertRdfToRub);
};

} // namespace caustica
