#include <render/passes/gaussian/GaussianSplatSceneRuntime.h>

#include <render/gpuSort/GPUSort.h>
#include <render/passes/debug/ShaderDebug.h>

#include <algorithm>
#include <cmath>

namespace caustica::render
{

dm::float4x4 gaussianSplatObjectToWorld(const scene::GaussianSplatRenderProxy& proxy)
{
    return dm::affineToHomogeneous(proxy.objectToWorld);
}

bool isGaussianSplatProxyActive(
    const scene::GaussianSplatRenderProxy& proxy,
    const SceneGaussianSplatPasses& scenePasses)
{
    const GaussianSplatPass* pass = scenePasses.findPass(proxy.entity);
    return proxy.enabled && pass != nullptr && pass->hasSplats();
}

GaussianSplatBinding getPrimaryGaussianSplatBinding(
    std::span<const scene::GaussianSplatRenderProxy> gaussianSplats,
    const SceneGaussianSplatPasses& scenePasses)
{
    GaussianSplatBinding binding;
    for (const scene::GaussianSplatRenderProxy& proxy : gaussianSplats)
    {
        const GaussianSplatPass* pass = scenePasses.findPass(proxy.entity);
        if (!proxy.enabled || pass == nullptr || !pass->hasSplats())
            continue;

        binding.splatPass = pass;
        binding.objectToWorld = gaussianSplatObjectToWorld(proxy);
        return binding;
    }
    return binding;
}

void prepareGaussianSplatScenePass(GaussianSplatPass& pass, const GaussianSplatPrepareContext& context)
{
    if (context.renderTargets == nullptr || context.shaderDebug == nullptr || context.device == nullptr)
        return;

    if (context.gpuSort == nullptr)
        return;

    pass.setGpuSort(context.gpuSort);
    pass.createPipeline(*context.renderTargets);
}

void prepareGaussianSplatScenePasses(SceneGaussianSplatPasses& scenePasses, GaussianSplatPrepareContext& context)
{
    if (context.gpuSort == nullptr
        && context.device != nullptr
        && context.shaderFactory != nullptr
        && context.shaderDebug != nullptr)
    {
        context.gpuSort = std::make_shared<GPUSort>(context.device, context.shaderFactory);
        context.gpuSort->createRenderPasses(context.shaderDebug);
    }

    for (SceneGaussianSplatPasses::SceneObject& object : scenePasses.objects())
    {
        if (object.pass != nullptr && object.pass->hasSplats())
            prepareGaussianSplatScenePass(*object.pass, context);
    }
}

void buildGaussianSplatEmissionProxies(
    std::vector<GaussianSplatEmissionProxy>& out,
    std::span<const scene::GaussianSplatRenderProxy> gaussianSplats,
    SceneGaussianSplatPasses& scenePasses,
    const PathTracerSettings& settings)
{
    out.clear();

    if (!isGaussianSplatEmissionEnabled(settings))
        return;

    const uint32_t maxProxyCount = clampGaussianSplatEmissionProxyCount(settings.GaussianSplatEmissionMaxProxyCount);
    for (const scene::GaussianSplatRenderProxy& proxy : gaussianSplats)
    {
        GaussianSplatPass* pass = scenePasses.findPass(proxy.entity);
        if (!proxy.enabled || pass == nullptr || !pass->hasSplats())
            continue;

        const uint32_t remainingProxyCount = maxProxyCount > out.size()
            ? maxProxyCount - uint32_t(out.size())
            : 0u;
        if (remainingProxyCount == 0)
            break;

        pass->buildEmissionProxies(
            remainingProxyCount,
            settings.GaussianSplatScale,
            uint32_t(std::clamp(settings.GaussianSplatRtxKernelDegree, 0, 5)),
            settings.GaussianSplatRtxAdaptiveClamp,
            settings.GaussianSplatTintColor,
            settings.GaussianSplatAlphaCullThreshold);

        const dm::affine3& objectToWorldTransform = proxy.objectToWorld;
        const float radiusScale = std::max({
            length(objectToWorldTransform.transformVector(dm::float3(1.0f, 0.0f, 0.0f))),
            length(objectToWorldTransform.transformVector(dm::float3(0.0f, 1.0f, 0.0f))),
            length(objectToWorldTransform.transformVector(dm::float3(0.0f, 0.0f, 1.0f))) });

        const auto& proxies = pass->getEmissionProxies();
        out.reserve(out.size() + proxies.size());
        for (const GaussianSplatEmissionProxy& proxy : proxies)
        {
            GaussianSplatEmissionProxy transformed = proxy;
            transformed.center = objectToWorldTransform.transformPoint(proxy.center);
            transformed.radius = proxy.radius * radiusScale;
            out.push_back(transformed);
        }
    }
}

bool uploadGaussianSplatScene(
    caustica::rhi::CommandList* commandList,
    std::span<const scene::GaussianSplatRenderProxy> gaussianSplats,
    SceneGaussianSplatPasses& scenePasses,
    const caustica::IView& splatView,
    caustica::rhi::rt::AccelStruct* meshTopLevelAS,
    RenderTargets& renderTargets,
    const GaussianSplatRenderSettings& settings)
{
    bool renderedAny = false;
    for (const scene::GaussianSplatRenderProxy& proxy : gaussianSplats)
    {
        GaussianSplatPass* pass = scenePasses.findPass(proxy.entity);
        if (!proxy.enabled || pass == nullptr || !pass->hasSplats())
            continue;

        GaussianSplatRenderSettings objectSettings = settings;
        objectSettings.objectToWorld = gaussianSplatObjectToWorld(proxy);
        renderedAny |= pass->upload(
            commandList,
            splatView,
            meshTopLevelAS,
            renderTargets,
            objectSettings);
    }
    return renderedAny;
}

void sortGaussianSplatScene(
    caustica::rhi::CommandList* commandList,
    std::span<const scene::GaussianSplatRenderProxy> gaussianSplats,
    SceneGaussianSplatPasses& scenePasses)
{
    for (const scene::GaussianSplatRenderProxy& proxy : gaussianSplats)
    {
        if (GaussianSplatPass* pass = scenePasses.findPass(proxy.entity);
            proxy.enabled && pass != nullptr && pass->hasSplats())
        {
            pass->sort(commandList);
        }
    }
}

bool rasterGaussianSplatScene(
    caustica::rhi::CommandList* commandList,
    std::span<const scene::GaussianSplatRenderProxy> gaussianSplats,
    SceneGaussianSplatPasses& scenePasses,
    const caustica::IView& splatView)
{
    bool renderedAny = false;
    for (const scene::GaussianSplatRenderProxy& proxy : gaussianSplats)
    {
        GaussianSplatPass* pass = scenePasses.findPass(proxy.entity);
        if (!proxy.enabled || pass == nullptr || !pass->hasSplats())
            continue;

        renderedAny |= pass->raster(commandList, splatView);
    }
    return renderedAny;
}

void buildGaussianSplatSceneAccelStructs(
    caustica::rhi::CommandList* commandList,
    std::span<const scene::GaussianSplatRenderProxy> gaussianSplats,
    SceneGaussianSplatPasses& scenePasses,
    const PathTracerSettings& settings)
{
    const bool buildShadowAccelStructs = resolveGaussianSplatShadowMode(settings) != GAUSSIAN_SPLAT_SHADOWS_DISABLED;
    for (const scene::GaussianSplatRenderProxy& proxy : gaussianSplats)
    {
        GaussianSplatPass* pass = scenePasses.findPass(proxy.entity);
        if (!proxy.enabled || pass == nullptr || !pass->hasSplats())
            continue;

        if (buildShadowAccelStructs)
        {
            pass->buildAccelerationStructures(
                commandList,
                settings.GaussianSplatUseAABBs,
                settings.GaussianSplatUseTLASInstances,
                settings.GaussianSplatBlasCompaction,
                settings.GaussianSplatScale,
                uint32_t(std::clamp(settings.GaussianSplatRtxKernelDegree, 0, 5)),
                settings.GaussianSplatRtxAdaptiveClamp);
        }
        else
        {
            pass->releaseAccelerationStructures();
        }
    }
}

} // namespace caustica::render
