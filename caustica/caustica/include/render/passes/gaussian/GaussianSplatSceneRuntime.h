#pragma once

#include <render/SceneGaussianSplatPasses.h>
#include <render/passes/gaussian/GaussianSplatGraph.h>
#include <render/passes/gaussian/GaussianSplatEmissionProxy.h>

#include <memory>
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

[[nodiscard]] dm::float4x4 gaussianSplatObjectToWorld(const SceneGaussianSplatPasses::SceneObject& object);
[[nodiscard]] bool isGaussianSplatSceneObjectActive(const SceneGaussianSplatPasses::SceneObject& object);
[[nodiscard]] GaussianSplatBinding getPrimaryGaussianSplatBinding(const SceneGaussianSplatPasses& scenePasses);

void prepareGaussianSplatScenePasses(SceneGaussianSplatPasses& scenePasses, GaussianSplatPrepareContext& context);
void prepareGaussianSplatScenePass(GaussianSplatPass& pass, const GaussianSplatPrepareContext& context);

void buildGaussianSplatEmissionProxies(
    std::vector<GaussianSplatEmissionProxy>& out,
    const SceneGaussianSplatPasses& scenePasses,
    const PathTracerSettings& settings);

[[nodiscard]] bool renderGaussianSplatScene(
    nvrhi::ICommandList* commandList,
    const SceneGaussianSplatPasses& scenePasses,
    const caustica::IView& splatView,
    nvrhi::rt::IAccelStruct* meshTopLevelAS,
    RenderTargets& renderTargets,
    const GaussianSplatRenderSettings& settings);

void buildGaussianSplatSceneAccelStructs(
    nvrhi::ICommandList* commandList,
    SceneGaussianSplatPasses& scenePasses,
    const PathTracerSettings& settings);

} // namespace caustica::render
