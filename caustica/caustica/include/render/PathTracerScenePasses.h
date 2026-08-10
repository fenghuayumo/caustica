#pragma once

#include <render/SceneGaussianSplatPasses.h>
#include <render/SceneLightingPasses.h>
#include <render/SceneRayTracingResources.h>

namespace caustica::render
{

// Renderer-session subsystems owned as one unit by WorldRenderer. Scene assets
// may reset independently while the RT pipeline runtime stays warm.
struct PathTracerScenePasses
{
    SceneLightingPasses lighting;
    SceneRayTracingResources rayTracing;
    SceneGaussianSplatPasses gaussianSplats;

    void initialize(
        const SceneRayTracingResources::Dependencies& rayTracingDependencies,
        const SceneGaussianSplatPasses::Dependencies& gaussianSplatDependencies);
    void reset();
};

} // namespace caustica::render
