#include <render/PathTracerScenePasses.h>

#include <rhi/rhi.h>

namespace caustica::render
{

void PathTracerScenePasses::initialize(const ScenePassDependencies& dependencies)
{
    rayTracing.initialize(dependencies);
    gaussianSplats.initialize(dependencies);

    gaussianSplats.setOnRequestFullRebuild(
        [this]() { rayTracing.requestFullRebuild(); });
}

void PathTracerScenePasses::reset()
{
    // ShaderCompiler refers to lighting.materials(); release the complete RT
    // runtime before destroying its lighting dependencies.
    gaussianSplats = {};
    rayTracing = {};
    lighting = {};
}

} // namespace caustica::render
