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

bool isGaussianSplatProxyActive(const scene::GaussianSplatRenderProxy& proxy)
{
    return proxy.enabled && proxy.pass != nullptr && proxy.pass->hasSplats();
}

GaussianSplatBinding getPrimaryGaussianSplatBinding(
    std::span<const scene::GaussianSplatRenderProxy> gaussianSplats)
{
    GaussianSplatBinding binding;
    for (const scene::GaussianSplatRenderProxy& proxy : gaussianSplats)
    {
        if (!isGaussianSplatProxyActive(proxy))
            continue;

        binding.splatPass = proxy.pass.get();
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
    const PathTracerSettings& settings)
{
    out.clear();

    if (!isGaussianSplatEmissionEnabled(settings))
        return;

    const uint32_t maxProxyCount = clampGaussianSplatEmissionProxyCount(settings.GaussianSplatEmissionMaxProxyCount);
    for (const scene::GaussianSplatRenderProxy& proxy : gaussianSplats)
    {
        if (!isGaussianSplatProxyActive(proxy))
            continue;

        const uint32_t remainingProxyCount = maxProxyCount > out.size()
            ? maxProxyCount - uint32_t(out.size())
            : 0u;
        if (remainingProxyCount == 0)
            break;

        proxy.pass->buildEmissionProxies(
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

        const auto& proxies = proxy.pass->getEmissionProxies();
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
    nvrhi::ICommandList* commandList,
    std::span<const scene::GaussianSplatRenderProxy> gaussianSplats,
    const caustica::IView& splatView,
    nvrhi::rt::IAccelStruct* meshTopLevelAS,
    RenderTargets& renderTargets,
    const GaussianSplatRenderSettings& settings)
{
    bool renderedAny = false;
    for (const scene::GaussianSplatRenderProxy& proxy : gaussianSplats)
    {
        if (!isGaussianSplatProxyActive(proxy))
            continue;

        GaussianSplatRenderSettings objectSettings = settings;
        objectSettings.objectToWorld = gaussianSplatObjectToWorld(proxy);
        renderedAny |= proxy.pass->upload(
            commandList,
            splatView,
            meshTopLevelAS,
            renderTargets,
            objectSettings);
    }
    return renderedAny;
}

void sortGaussianSplatScene(
    nvrhi::ICommandList* commandList,
    std::span<const scene::GaussianSplatRenderProxy> gaussianSplats)
{
    for (const scene::GaussianSplatRenderProxy& proxy : gaussianSplats)
    {
        if (isGaussianSplatProxyActive(proxy))
            proxy.pass->sort(commandList);
    }
}

bool rasterGaussianSplatScene(
    nvrhi::ICommandList* commandList,
    std::span<const scene::GaussianSplatRenderProxy> gaussianSplats,
    const caustica::IView& splatView)
{
    bool renderedAny = false;
    for (const scene::GaussianSplatRenderProxy& proxy : gaussianSplats)
    {
        if (!isGaussianSplatProxyActive(proxy))
            continue;

        renderedAny |= proxy.pass->raster(commandList, splatView);
    }
    return renderedAny;
}

void buildGaussianSplatSceneAccelStructs(
    nvrhi::ICommandList* commandList,
    std::span<const scene::GaussianSplatRenderProxy> gaussianSplats,
    const PathTracerSettings& settings)
{
    const bool buildShadowAccelStructs = resolveGaussianSplatShadowMode(settings) != GAUSSIAN_SPLAT_SHADOWS_DISABLED;
    for (const scene::GaussianSplatRenderProxy& proxy : gaussianSplats)
    {
        if (!isGaussianSplatProxyActive(proxy))
            continue;

        if (buildShadowAccelStructs)
        {
            proxy.pass->buildAccelerationStructures(
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
            proxy.pass->releaseAccelerationStructures();
        }
    }
}

} // namespace caustica::render
