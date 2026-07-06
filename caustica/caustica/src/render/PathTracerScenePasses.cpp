#include <render/PathTracerScenePasses.h>

#include <render/worldRenderer/WorldRenderer.h>
#include <rhi/nvrhi.h>

namespace caustica::render
{

void PathTracerScenePasses::wireSession(const ScenePassWireParams& params)
{
    rayTracing.wireSession(params);
    gaussianSplats.wireSession(params);

    gaussianSplats.setOnRequestFullRebuild(
        [this]() { rayTracing.requestFullRebuild(); });

    WorldRenderer& worldRenderer = params.worldRenderer;
    rayTracing.setAdditionalAccelStructBuilder(
        [&worldRenderer](nvrhi::ICommandList* commandList) {
            worldRenderer.executeGaussianSplatAccelBuild(commandList);
        });
}

} // namespace caustica::render
