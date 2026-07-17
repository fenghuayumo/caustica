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

class SceneManager;
class RenderTargets;
class ShaderDebug;

namespace caustica::render
{
class WorldRenderer;
struct GaussianSplatSceneSummary;
struct RenderInvalidationState;
} // namespace caustica::render

class PathTracerSettings;

namespace caustica::render
{

struct ScenePassWireParams
{
    caustica::GpuDevice& gpuDevice;
    SceneManager& sceneManager;
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
    std::function<std::shared_ptr<ShaderDebug>()> getShaderDebug;
};

// Scene-scoped render pass bundles owned by PathTracingRuntime (not Application hosts).
struct PathTracerScenePasses
{
    SceneLightingPasses lighting;
    SceneRayTracingResources rayTracing;
    SceneGaussianSplatPasses gaussianSplats;

    void wireSession(const ScenePassWireParams& params);
};

} // namespace caustica::render
