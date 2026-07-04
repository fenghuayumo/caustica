#pragma once

#include <render/SceneGaussianSplatPasses.h>
#include <render/SceneLightingPasses.h>
#include <render/SceneRayTracingResources.h>

#include <memory>

namespace caustica
{
class BindingCache;
class CommonRenderPasses;
class GpuDevice;
class RenderCore;
class ShaderFactory;
} // namespace caustica

class SceneManager;

namespace caustica::render
{
class PathTracingWorldRenderer;
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
    caustica::RenderCore& renderCore;
    PathTracingWorldRenderer& worldRenderer;
    PathTracerSettings& settings;
    RenderInvalidationState& invalidation;
    GaussianSplatSceneSummary& gaussianSplatsSummary;
    SceneLightingPasses& lighting;
    caustica::BindingCache& bindingCache;
    std::shared_ptr<caustica::ShaderFactory>& shaderFactory;
    std::shared_ptr<caustica::CommonRenderPasses>& commonPasses;
};

// Scene-scoped render pass bundles owned by EngineRenderer (not Application hosts).
struct PathTracerScenePasses
{
    SceneLightingPasses lighting;
    SceneRayTracingResources rayTracing;
    SceneGaussianSplatPasses gaussianSplats;

    void wireSession(const ScenePassWireParams& params);
};

} // namespace caustica::render
