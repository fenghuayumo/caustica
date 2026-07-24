#include <render/PathTracerScenePasses.h>

#include <rhi/rhi.h>

namespace caustica::render
{

void PathTracerScenePasses::wireSession(const ScenePassWireParams& params)
{
    rayTracing.wireSession(params);
    gaussianSplats.wireSession(params);

    gaussianSplats.setOnRequestFullRebuild(
        [this]() { rayTracing.requestFullRebuild(); });
}

} // namespace caustica::render
