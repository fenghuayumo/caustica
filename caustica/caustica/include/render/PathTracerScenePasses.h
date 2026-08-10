#pragma once

#include <render/SceneGaussianSplatPasses.h>
#include <render/SceneLightingPasses.h>
#include <render/SceneRayTracingResources.h>

#include <functional>
#include <memory>

namespace caustica
{
class AccelStructManager;
class BindingCache;
class GpuDevice;
class ShaderFactory;

namespace render
{
class RenderDevice;
}
} // namespace caustica

class RenderTargets;

namespace caustica::render
{
class WorldRenderer;
struct GaussianSplatSceneSummary;
struct RenderInvalidationState;
} // namespace caustica::render

class PathTracerSettings;

namespace caustica::render
{

// Non-owning services shared by scene-scoped render subsystems.
// The caller owns every referenced service for the lifetime of PathTracerScenePasses.
struct ScenePassDependencies
{
    caustica::GpuDevice& gpuDevice;
    caustica::AccelStructManager& accelStructs;
    WorldRenderer& worldRenderer;
    PathTracerSettings& settings;
    RenderInvalidationState& invalidation;
    GaussianSplatSceneSummary& gaussianSplatsSummary;
    SceneLightingPasses& lighting;
    caustica::BindingCache& bindingCache;
    std::shared_ptr<caustica::ShaderFactory>& shaderFactory;
    caustica::render::RenderDevice& renderDevice;
    std::function<void()> onGaussianSplatTemporalReset;
    std::function<RenderTargets*()> getRenderTargets;
};

// Renderer-session subsystems owned as one unit by WorldRenderer. Scene assets
// may reset independently while the RT pipeline runtime stays warm.
struct PathTracerScenePasses
{
    SceneLightingPasses lighting;
    SceneRayTracingResources rayTracing;
    SceneGaussianSplatPasses gaussianSplats;

    void initialize(const ScenePassDependencies& dependencies);
    void reset();
};

} // namespace caustica::render
