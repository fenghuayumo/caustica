#include <render/passes/gaussian/GaussianSplatFramePass.h>

#include <render/PathTracingContext.h>
#include <render/SceneGaussianSplatPasses.h>
#include <render/core/AccelStructManager.h>
#include <render/core/CameraController.h>
#include <render/core/PathTracerSettings.h>
#include <render/core/RenderTargets.h>
#include <render/gpuSort/GPUSort.h>
#include <render/passes/gaussian/GaussianSplatGraph.h>
#include <render/passes/gaussian/GaussianSplatSceneRuntime.h>
#include <render/passes/postProcess/AccumulationPass.h>
#include <scene/View.h>
#include <scene/SceneLightAccess.h>

#include <algorithm>

using namespace caustica;
using namespace caustica::math;
using namespace caustica::render;

GaussianSplatFramePass::GaussianSplatFramePass() = default;
GaussianSplatFramePass::~GaussianSplatFramePass() = default;

void GaussianSplatFramePass::createTemporalResources(
    nvrhi::IDevice* device,
    const std::shared_ptr<caustica::ShaderFactory>& shaderFactory,
    RenderTargets* renderTargets)
{
    assert(device);
    assert(renderTargets);

    nvrhi::TextureDesc gaussianCurrentDesc = renderTargets->processedOutputColor->getDesc();
    gaussianCurrentDesc.debugName = "GaussianSplatTemporalCurrentColor";
    gaussianCurrentDesc.isUAV = false;
    gaussianCurrentDesc.isRenderTarget = false;
    gaussianCurrentDesc.useClearValue = false;
    gaussianCurrentDesc.clearValue = nvrhi::Color(0.0f);
    gaussianCurrentDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    gaussianCurrentDesc.keepInitialState = true;
    m_currentColor = device->createTexture(gaussianCurrentDesc);

    nvrhi::TextureDesc gaussianAccumDesc = renderTargets->processedOutputColor->getDesc();
    gaussianAccumDesc.debugName = "GaussianSplatTemporalAccumulatedColor";
    gaussianAccumDesc.format = nvrhi::Format::RGBA32_FLOAT;
    gaussianAccumDesc.isUAV = true;
    gaussianAccumDesc.isRenderTarget = true;
    gaussianAccumDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    gaussianAccumDesc.keepInitialState = true;
    m_accumulatedColor = device->createTexture(gaussianAccumDesc);

    m_accumulationPass = std::make_unique<AccumulationPass>(device, shaderFactory);
    m_accumulationPass->createPipeline();
    m_accumulationPass->createBindingSet(
        m_currentColor, m_accumulatedColor, renderTargets->processedOutputColor);
}

void GaussianSplatFramePass::bindFrame(const FrameBindings& bindings)
{
    m_bindings = bindings;
}

void GaussianSplatFramePass::prepareScenePasses(const std::shared_ptr<ShaderDebug>& shaderDebug)
{
    assert(m_bindings.context);
    assert(m_bindings.device);
    assert(m_bindings.scenePasses);

    GaussianSplatPrepareContext context;
    context.device = m_bindings.device;
    context.shaderFactory = m_bindings.context->shaderFactory;
    context.renderTargets = m_bindings.renderTargets;
    context.shaderDebug = shaderDebug;
    context.gpuSort = m_gpuSort;
    prepareGaussianSplatScenePasses(*m_bindings.scenePasses, context);
    m_gpuSort = context.gpuSort;
}

bool GaussianSplatFramePass::hasActiveSplats() const
{
    assert(m_bindings.context);
    assert(m_bindings.scenePasses);

    if (!m_bindings.context->hasFrameScene() || !m_bindings.context->activeSettings().EnableGaussianSplats)
        return false;

    const auto gaussianSplats = m_bindings.context->frameGaussianSplats();
    return std::any_of(
        gaussianSplats.begin(),
        gaussianSplats.end(),
        [this](const scene::GaussianSplatRenderProxy& proxy) {
            return isGaussianSplatProxyActive(proxy, *m_bindings.scenePasses);
        });
}

std::vector<GaussianSplatGraphResources>
GaussianSplatFramePass::prepareGraphResources(bool renderToOutputColor)
{
    std::vector<GaussianSplatGraphResources> resources;
    if (!hasActiveSplats())
        return resources;

    assert(m_bindings.context);
    assert(m_bindings.scenePasses);
    assert(m_bindings.temporalSampleIndex);

    const GaussianSplatFrameInputs frameInputs{
        m_bindings.context->activeSettings(),
        int(m_bindings.frameIndex),
        int(m_bindings.sampleIndex),
        *m_bindings.temporalSampleIndex,
        renderToOutputColor,
        dm::float2(float(m_bindings.displaySize.x), float(m_bindings.displaySize.y)),
        resolveGaussianSplatShadowDirection(m_bindings.context->frameLights()),
    };
    const GaussianSplatRenderSettings settings = buildGaussianSplatRenderSettings(frameInputs);

    for (const scene::GaussianSplatRenderProxy& proxy : m_bindings.context->frameGaussianSplats())
    {
        GaussianSplatPass* pass = m_bindings.scenePasses->findPass(proxy.entity);
        if (!proxy.enabled || pass == nullptr || !pass->hasSplats())
            continue;

        GaussianSplatRenderSettings objectSettings = settings;
        objectSettings.objectToWorld = gaussianSplatObjectToWorld(proxy);
        pass->prepareGraphResources(objectSettings);
        resources.push_back(pass->graphResources(objectSettings));
    }
    return resources;
}

void GaussianSplatFramePass::executeAccelBuild(nvrhi::ICommandList* commandList)
{
    if (commandList == nullptr || !hasActiveSplats())
        return;

    assert(m_bindings.context);
    assert(m_bindings.scenePasses);

    buildGaussianSplatSceneAccelStructs(
        commandList,
        m_bindings.context->frameGaussianSplats(),
        *m_bindings.scenePasses,
        m_bindings.context->activeSettings());
}

void GaussianSplatFramePass::executeUpload(nvrhi::ICommandList* commandList, bool renderToOutputColor)
{
    m_compositeRendered = false;
    if (commandList == nullptr || !hasActiveSplats())
        return;

    assert(m_bindings.context);
    assert(m_bindings.scenePasses);
    assert(m_bindings.accelStructs);
    assert(m_bindings.renderTargets);
    assert(m_bindings.temporalSampleIndex);

    const bool stochasticSplats = m_bindings.context->activeSettings().EnableGaussianSplats
        && m_bindings.context->activeSettings().GaussianSplatSortingMode == 1;
    const bool temporalReset =
        (m_bindings.frameTemporalReset && *m_bindings.frameTemporalReset)
        || (m_bindings.temporalReset && *m_bindings.temporalReset);
    if (stochasticSplats
        && (m_bindings.context->activeSettings().ResetAccumulation
            || m_bindings.context->activeSettings().ResetRealtimeCaches
            || temporalReset))
    {
        *m_bindings.temporalSampleIndex = 0;
    }

    const GaussianSplatFrameInputs frameInputs{
        m_bindings.context->activeSettings(),
        int(m_bindings.frameIndex),
        int(m_bindings.sampleIndex),
        *m_bindings.temporalSampleIndex,
        renderToOutputColor,
        dm::float2(float(m_bindings.displaySize.x), float(m_bindings.displaySize.y)),
        resolveGaussianSplatShadowDirection(m_bindings.context->frameLights()),
    };
    const GaussianSplatRenderSettings settings = buildGaussianSplatRenderSettings(frameInputs);

    caustica::PlanarView splatView = *m_bindings.context->camera.view();
    if (!renderToOutputColor)
    {
        splatView.setViewport(ViewportDesc(float(m_bindings.displaySize.x), float(m_bindings.displaySize.y)));
        splatView.setPixelOffset(dm::float2::zero());
    }
    splatView.updateCache();

    (void)uploadGaussianSplatScene(
        commandList,
        m_bindings.context->frameGaussianSplats(),
        *m_bindings.scenePasses,
        splatView,
        m_bindings.accelStructs->getTopLevelAS().Get(),
        *m_bindings.renderTargets,
        settings);
}

void GaussianSplatFramePass::executeSort(nvrhi::ICommandList* commandList)
{
    if (commandList == nullptr || !hasActiveSplats())
        return;

    assert(m_bindings.context);
    assert(m_bindings.scenePasses);

    sortGaussianSplatScene(
        commandList,
        m_bindings.context->frameGaussianSplats(),
        *m_bindings.scenePasses);
}

void GaussianSplatFramePass::executeRaster(nvrhi::ICommandList* commandList, bool renderToOutputColor)
{
    if (commandList == nullptr || !hasActiveSplats())
        return;

    assert(m_bindings.context);
    assert(m_bindings.scenePasses);

    caustica::PlanarView splatView = *m_bindings.context->camera.view();
    if (!renderToOutputColor)
    {
        splatView.setViewport(ViewportDesc(float(m_bindings.displaySize.x), float(m_bindings.displaySize.y)));
        splatView.setPixelOffset(dm::float2::zero());
    }
    splatView.updateCache();

    const bool renderedAny = rasterGaussianSplatScene(
        commandList,
        m_bindings.context->frameGaussianSplats(),
        *m_bindings.scenePasses,
        splatView);
    m_compositeRendered = renderedAny && !renderToOutputColor;
}

void GaussianSplatFramePass::executeAccumulate(nvrhi::ICommandList* commandList)
{
    if (commandList == nullptr || !m_compositeRendered)
        return;

    if (m_accumulationPass == nullptr || m_bindings.renderTargets == nullptr
        || m_currentColor == nullptr || m_accumulatedColor == nullptr)
        return;

    assert(m_bindings.context);
    assert(m_bindings.temporalSampleIndex);

    const bool temporalReset =
        (m_bindings.frameTemporalReset && *m_bindings.frameTemporalReset)
        || (m_bindings.temporalReset && *m_bindings.temporalReset);
    if (m_bindings.context->activeSettings().ResetAccumulation
        || m_bindings.context->activeSettings().ResetRealtimeCaches
        || temporalReset)
    {
        *m_bindings.temporalSampleIndex = 0;
        if (m_bindings.frameTemporalReset)
            *m_bindings.frameTemporalReset = false;
        if (m_bindings.temporalReset)
            *m_bindings.temporalReset = false;
    }

    const float accumulationWeight = 1.0f / float(*m_bindings.temporalSampleIndex + 1);

    caustica::PlanarView splatView = *m_bindings.context->camera.view();
    splatView.setViewport(ViewportDesc(float(m_bindings.displaySize.x), float(m_bindings.displaySize.y)));
    splatView.setPixelOffset(dm::float2::zero());
    splatView.updateCache();

    m_accumulationPass->render(commandList, splatView, splatView, accumulationWeight);

    *m_bindings.temporalSampleIndex = std::min(*m_bindings.temporalSampleIndex + 1, 1024 * 1024);
    m_compositeRendered = false;
}
