#include <render/passes/gaussian/GaussianSplatSceneRuntime.h>

#include <render/gpuSort/GPUSort.h>
#include <render/passes/debug/ShaderDebug.h>

#include <algorithm>
#include <cmath>

namespace caustica::render
{

dm::float4x4 gaussianSplatObjectToWorld(const SceneGaussianSplatPasses::SceneObject& object)
{
    if (!object.splat)
        return dm::float4x4::identity();

    return dm::affineToHomogeneous(dm::affine3(object.splat->cachedGlobalTransform));
}

bool isGaussianSplatSceneObjectActive(const SceneGaussianSplatPasses::SceneObject& object)
{
    return object.splat != nullptr
        && object.splat->enabled
        && object.pass != nullptr
        && object.pass->hasSplats();
}

GaussianSplatBinding getPrimaryGaussianSplatBinding(const SceneGaussianSplatPasses& scenePasses)
{
    GaussianSplatBinding binding;
    for (const SceneGaussianSplatPasses::SceneObject& object : scenePasses.objects())
    {
        if (!isGaussianSplatSceneObjectActive(object))
            continue;

        binding.splatPass = object.pass.get();
        binding.objectToWorld = gaussianSplatObjectToWorld(object);
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
    const SceneGaussianSplatPasses& scenePasses,
    const PathTracerSettings& settings)
{
    out.clear();

    if (!isGaussianSplatEmissionEnabled(settings))
        return;

    const uint32_t maxProxyCount = clampGaussianSplatEmissionProxyCount(settings.GaussianSplatEmissionMaxProxyCount);
    for (const SceneGaussianSplatPasses::SceneObject& object : scenePasses.objects())
    {
        if (!isGaussianSplatSceneObjectActive(object))
            continue;

        const uint32_t remainingProxyCount = maxProxyCount > out.size()
            ? maxProxyCount - uint32_t(out.size())
            : 0u;
        if (remainingProxyCount == 0)
            break;

        object.pass->buildEmissionProxies(
            remainingProxyCount,
            settings.GaussianSplatScale,
            uint32_t(std::clamp(settings.GaussianSplatRtxKernelDegree, 0, 5)),
            settings.GaussianSplatRtxAdaptiveClamp,
            settings.GaussianSplatTintColor,
            settings.GaussianSplatAlphaCullThreshold);

        const dm::affine3 objectToWorldTransform = dm::affine3(object.splat->cachedGlobalTransform);
        const float radiusScale = std::max({
            length(objectToWorldTransform.transformVector(dm::float3(1.0f, 0.0f, 0.0f))),
            length(objectToWorldTransform.transformVector(dm::float3(0.0f, 1.0f, 0.0f))),
            length(objectToWorldTransform.transformVector(dm::float3(0.0f, 0.0f, 1.0f))) });

        const auto& proxies = object.pass->getEmissionProxies();
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

bool renderGaussianSplatScene(
    nvrhi::ICommandList* commandList,
    const SceneGaussianSplatPasses& scenePasses,
    const caustica::IView& splatView,
    nvrhi::rt::IAccelStruct* meshTopLevelAS,
    RenderTargets& renderTargets,
    const GaussianSplatRenderSettings& settings)
{
    bool renderedAny = false;
    for (const SceneGaussianSplatPasses::SceneObject& object : scenePasses.objects())
    {
        if (!isGaussianSplatSceneObjectActive(object))
            continue;

        GaussianSplatRenderSettings objectSettings = settings;
        objectSettings.objectToWorld = gaussianSplatObjectToWorld(object);
        object.pass->render(commandList, splatView, meshTopLevelAS, renderTargets, objectSettings);
        renderedAny = true;
    }
    return renderedAny;
}

void buildGaussianSplatSceneAccelStructs(
    nvrhi::ICommandList* commandList,
    SceneGaussianSplatPasses& scenePasses,
    const PathTracerSettings& settings)
{
    const bool buildShadowAccelStructs = resolveGaussianSplatShadowMode(settings) != GAUSSIAN_SPLAT_SHADOWS_DISABLED;
    for (SceneGaussianSplatPasses::SceneObject& object : scenePasses.objects())
    {
        if (!isGaussianSplatSceneObjectActive(object))
            continue;

        if (buildShadowAccelStructs)
        {
            object.pass->buildAccelerationStructures(
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
            object.pass->releaseAccelerationStructures();
        }
    }
}

} // namespace caustica::render
