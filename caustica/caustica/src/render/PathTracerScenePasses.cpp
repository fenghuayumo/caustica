#include <render/PathTracerScenePasses.h>

#include <rhi/nvrhi.h>

namespace caustica::render
{

void PathTracerScenePasses::wireSession(const ScenePassWireParams& params)
{
    rayTracing.wireSession(params);
    gaussianSplats.wireSession(params);

    gaussianSplats.setOnRequestFullRebuild(
        [this]() { rayTracing.requestFullRebuild(); });
}

void PathTracerScenePasses::bindSessionScene(
    std::shared_ptr<caustica::Scene> scene,
    std::filesystem::path scenePath)
{
    rayTracing.bindSessionScene(scene);
    gaussianSplats.bindSessionScene(std::move(scene), std::move(scenePath));
}

void PathTracerScenePasses::clearSessionScene()
{
    rayTracing.clearSessionScene();
    gaussianSplats.clearSessionScene();
}

} // namespace caustica::render
