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

    rayTracing.setAdditionalAccelStructBuilder(
        [this](nvrhi::ICommandList* commandList) {
            gaussianSplats.buildAccelStructs(commandList);
        });
}

} // namespace caustica::render
