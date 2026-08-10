#include <render/PathTracerScenePasses.h>

#include <rhi/rhi.h>

namespace caustica::render
{

void PathTracerScenePasses::initialize(
    const SceneRayTracingResources::Dependencies& rayTracingDependencies,
    const SceneGaussianSplatPasses::Dependencies& gaussianSplatDependencies)
{
    rayTracing.initialize(rayTracingDependencies, lighting);
    gaussianSplats.initialize(gaussianSplatDependencies);

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
