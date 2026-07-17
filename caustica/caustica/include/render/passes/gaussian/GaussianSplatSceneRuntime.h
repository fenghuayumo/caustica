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
class IView;
class PlanarView;
class ShaderFactory;
}

namespace caustica::render
{

struct GaussianSplatPrepareContext
{
    nvrhi::IDevice* device = nullptr;
    std::shared_ptr<caustica::ShaderFactory> shaderFactory;
    RenderTargets* renderTargets = nullptr;
    std::shared_ptr<ShaderDebug> shaderDebug;
    std::shared_ptr<GPUSort> gpuSort;
};

[[nodiscard]] dm::float4x4 gaussianSplatObjectToWorld(const scene::GaussianSplatRenderProxy& proxy);
[[nodiscard]] bool isGaussianSplatProxyActive(
    const scene::GaussianSplatRenderProxy& proxy,
    const SceneGaussianSplatPasses& scenePasses);
[[nodiscard]] GaussianSplatBinding getPrimaryGaussianSplatBinding(
    std::span<const scene::GaussianSplatRenderProxy> gaussianSplats,
    const SceneGaussianSplatPasses& scenePasses);

void prepareGaussianSplatScenePasses(SceneGaussianSplatPasses& scenePasses, GaussianSplatPrepareContext& context);
void prepareGaussianSplatScenePass(GaussianSplatPass& pass, const GaussianSplatPrepareContext& context);

void buildGaussianSplatEmissionProxies(
    std::vector<GaussianSplatEmissionProxy>& out,
    std::span<const scene::GaussianSplatRenderProxy> gaussianSplats,
    SceneGaussianSplatPasses& scenePasses,
    const PathTracerSettings& settings);

[[nodiscard]] bool uploadGaussianSplatScene(
    nvrhi::ICommandList* commandList,
    std::span<const scene::GaussianSplatRenderProxy> gaussianSplats,
    SceneGaussianSplatPasses& scenePasses,
    const caustica::IView& splatView,
    nvrhi::rt::IAccelStruct* meshTopLevelAS,
    RenderTargets& renderTargets,
    const GaussianSplatRenderSettings& settings);

void sortGaussianSplatScene(
    nvrhi::ICommandList* commandList,
    std::span<const scene::GaussianSplatRenderProxy> gaussianSplats,
    SceneGaussianSplatPasses& scenePasses);

[[nodiscard]] bool rasterGaussianSplatScene(
    nvrhi::ICommandList* commandList,
    std::span<const scene::GaussianSplatRenderProxy> gaussianSplats,
    SceneGaussianSplatPasses& scenePasses,
    const caustica::IView& splatView);

void buildGaussianSplatSceneAccelStructs(
    nvrhi::ICommandList* commandList,
    std::span<const scene::GaussianSplatRenderProxy> gaussianSplats,
    SceneGaussianSplatPasses& scenePasses,
    const PathTracerSettings& settings);

} // namespace caustica::render
