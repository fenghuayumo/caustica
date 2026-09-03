#pragma once

#include <render/SceneGaussianSplatPasses.h>
#include <render/passes/gaussian/GaussianSplatGraph.h>
#include <render/passes/gaussian/GaussianSplatEmissionProxy.h>
#include <scene/SceneRenderData.h>

#include <memory>
#include <span>
#include <vector>

class GPUSort;
class RenderTargets;
class ShaderDebug;

namespace caustica
{
class ViewInfo;
class ShaderFactory;
}

namespace caustica::render
{

struct GaussianSplatPrepareContext
{
    caustica::rhi::Device* device = nullptr;
    std::shared_ptr<caustica::ShaderFactory> shaderFactory;
    std::shared_ptr<ShaderDebug> shaderDebug;
    std::shared_ptr<::GPUSort> gpuSort;
};

[[nodiscard]] dm::float4x4 gaussianSplatObjectToWorld(const scene::GaussianSplatRenderProxy& proxy);
[[nodiscard]] bool isGaussianSplatProxyActive(
    const scene::GaussianSplatRenderProxy& proxy,
    const SceneGaussianSplatPasses& scenePasses);
[[nodiscard]] GaussianSplatBinding getPrimaryGaussianSplatBinding(
    std::span<const scene::GaussianSplatRenderProxy> gaussianSplats,
    const SceneGaussianSplatPasses& scenePasses);

void prepareGaussianSplatScenePasses(
    SceneGaussianSplatPasses& scenePasses,
    GaussianSplatPrepareContext& context,
    RenderTargets& renderTargets);
void prepareGaussianSplatScenePass(
    GaussianSplatPass& pass,
    const GaussianSplatPrepareContext& context,
    RenderTargets& renderTargets);
void buildGaussianSplatEmissionProxies(
    std::vector<GaussianSplatEmissionProxy>& out,
    std::span<const scene::GaussianSplatRenderProxy> gaussianSplats,
    SceneGaussianSplatPasses& scenePasses,
    const PathTracerSettings& settings);

[[nodiscard]] bool uploadGaussianSplatScene(
    caustica::rhi::CommandList* commandList,
    std::span<const scene::GaussianSplatRenderProxy> gaussianSplats,
    SceneGaussianSplatPasses& scenePasses,
    const caustica::ViewInfo& splatView,
    caustica::rhi::rt::AccelStruct* meshTopLevelAS,
    RenderTargets& renderTargets,
    const GaussianSplatRenderSettings& settings);

void sortGaussianSplatScene(
    caustica::rhi::CommandList* commandList,
    std::span<const scene::GaussianSplatRenderProxy> gaussianSplats,
    SceneGaussianSplatPasses& scenePasses);

[[nodiscard]] bool rasterGaussianSplatScene(
    caustica::rhi::CommandList* commandList,
    std::span<const scene::GaussianSplatRenderProxy> gaussianSplats,
    SceneGaussianSplatPasses& scenePasses,
    const caustica::ViewInfo& splatView);

void buildGaussianSplatSceneAccelStructs(
    caustica::rhi::CommandList* commandList,
    std::span<const scene::GaussianSplatRenderProxy> gaussianSplats,
    SceneGaussianSplatPasses& scenePasses,
    const PathTracerSettings& settings);

} // namespace caustica::render
