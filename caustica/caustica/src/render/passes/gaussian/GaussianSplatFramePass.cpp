#include <render/passes/gaussian/GaussianSplatFramePass.h>

#include <render/FrameGraphContext.h>
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
    caustica::rhi::Device* device,
    const std::shared_ptr<caustica::ShaderFactory>& shaderFactory,
    RenderTargets* renderTargets)
{
    assert(device);
    assert(renderTargets);

    m_device = device;
    m_renderTargets = renderTargets;

    caustica::rhi::TextureDesc gaussianCurrentDesc = renderTargets->processedOutputColor->getDesc();
    gaussianCurrentDesc.debugName = "GaussianSplatTemporalCurrentColor";
    gaussianCurrentDesc.isUAV = false;
    gaussianCurrentDesc.isRenderTarget = false;
    gaussianCurrentDesc.useClearValue = false;
    gaussianCurrentDesc.clearValue = caustica::rhi::Color(0.0f);
    gaussianCurrentDesc.initialState = caustica::rhi::ResourceStates::ShaderResource;
    gaussianCurrentDesc.keepInitialState = true;
    m_currentColor = device->createTexture(gaussianCurrentDesc);

    caustica::rhi::TextureDesc gaussianAccumDesc = renderTargets->processedOutputColor->getDesc();
    gaussianAccumDesc.debugName = "GaussianSplatTemporalAccumulatedColor";
    gaussianAccumDesc.format = caustica::rhi::Format::RGBA32_FLOAT;
    gaussianAccumDesc.isUAV = true;
    gaussianAccumDesc.isRenderTarget = true;
    gaussianAccumDesc.initialState = caustica::rhi::ResourceStates::UnorderedAccess;
    gaussianAccumDesc.keepInitialState = true;
    m_accumulatedColor = device->createTexture(gaussianAccumDesc);

    m_accumulationPass = std::make_unique<AccumulationPass>(device, shaderFactory);
    m_accumulationPass->createPipeline();
    m_accumulationPass->createBindingSet(
        m_currentColor, m_accumulatedColor, renderTargets->processedOutputColor);
}

void GaussianSplatFramePass::bindStable(
    PathTracingContext* context,
    caustica::rhi::Device* device,
    caustica::AccelStructManager* accelStructs,
    SceneGaussianSplatPasses* scenePasses)
{
    m_context = context;
    m_device = device;
    m_accelStructs = accelStructs;
    m_scenePasses = scenePasses;
}

void GaussianSplatFramePass::bindFrame(const FrameGraphContext& ctx)
{
    if (ctx.pathTracingContext)
        m_context = ctx.pathTracingContext;
    if (ctx.device)
        m_device = ctx.device;
    if (ctx.renderTargets)
        m_renderTargets = ctx.renderTargets;
    if (ctx.accelStructs)
        m_accelStructs = ctx.accelStructs;
    if (ctx.gaussianScenePasses)
        m_scenePasses = ctx.gaussianScenePasses;

    m_displaySize = ctx.displaySize;
    m_frameIndex = ctx.frameIndex;
    m_sampleIndex = ctx.sampleIndex;
    m_temporalSampleIndex = ctx.gaussianSplatTemporalSampleIndex;
    m_frameTemporalReset = ctx.gaussianSplatTemporalReset;
    m_temporalReset = ctx.gaussianSplatOwnedTemporalReset;
}

void GaussianSplatFramePass::prepareScenePasses(const std::shared_ptr<ShaderDebug>& shaderDebug)
{
    assert(m_context);
    assert(m_device);
    assert(m_scenePasses);

    GaussianSplatPrepareContext context;
    context.device = m_device;
    context.shaderFactory = m_context->shaderFactory;
    context.renderTargets = m_renderTargets;
    context.shaderDebug = shaderDebug;
    context.gpuSort = m_gpuSort;
    prepareGaussianSplatScenePasses(*m_scenePasses, context);
    m_gpuSort = context.gpuSort;
}

void GaussianSplatFramePass::buildEmissionProxies(
    std::vector<GaussianSplatEmissionProxy>& outProxies,
    const PathTracerSettings& settings) const
{
    assert(m_context);
    assert(m_scenePasses);
    caustica::render::buildGaussianSplatEmissionProxies(
        outProxies,
        m_context->frameGaussianSplats(),
        *m_scenePasses,
        settings);
}

bool GaussianSplatFramePass::hasActiveSplats() const
{
    assert(m_context);
    assert(m_scenePasses);

    if (!m_context->hasFrameScene() || !m_context->activeSettings().EnableGaussianSplats)
        return false;

    const auto gaussianSplats = m_context->frameGaussianSplats();
    return std::any_of(
        gaussianSplats.begin(),
        gaussianSplats.end(),
        [this](const scene::GaussianSplatRenderProxy& proxy) {
            return isGaussianSplatProxyActive(proxy, *m_scenePasses);
        });
}

std::vector<GaussianSplatGraphResources>
GaussianSplatFramePass::prepareGraphResources(bool renderToOutputColor)
{
    std::vector<GaussianSplatGraphResources> resources;
    if (!hasActiveSplats())
        return resources;

    assert(m_context);
    assert(m_scenePasses);
    assert(m_temporalSampleIndex);

    const GaussianSplatFrameInputs frameInputs{
        m_context->activeSettings(),
        int(m_frameIndex),
        int(m_sampleIndex),
        *m_temporalSampleIndex,
        renderToOutputColor,
        dm::float2(float(m_displaySize.x), float(m_displaySize.y)),
        resolveGaussianSplatShadowDirection(m_context->frameLights()),
    };
    const GaussianSplatRenderSettings settings = buildGaussianSplatRenderSettings(frameInputs);

    for (const scene::GaussianSplatRenderProxy& proxy : m_context->frameGaussianSplats())
    {
        GaussianSplatPass* pass = m_scenePasses->findPass(proxy.entity);
        if (!proxy.enabled || pass == nullptr || !pass->hasSplats())
            continue;

        GaussianSplatRenderSettings objectSettings = settings;
        objectSettings.objectToWorld = gaussianSplatObjectToWorld(proxy);
        pass->prepareGraphResources(objectSettings);
        resources.push_back(pass->graphResources(objectSettings));
    }
    return resources;
}

void GaussianSplatFramePass::executeAccelBuild(caustica::rhi::CommandList* commandList)
{
    if (commandList == nullptr || !hasActiveSplats())
        return;

    assert(m_context);
    assert(m_scenePasses);

    buildGaussianSplatSceneAccelStructs(
        commandList,
        m_context->frameGaussianSplats(),
        *m_scenePasses,
        m_context->activeSettings());
}

void GaussianSplatFramePass::executeUpload(caustica::rhi::CommandList* commandList, bool renderToOutputColor)
{
    m_compositeRendered = false;
    if (commandList == nullptr || !hasActiveSplats())
        return;

    assert(m_context);
    assert(m_scenePasses);
    assert(m_accelStructs);
    assert(m_renderTargets);
    assert(m_temporalSampleIndex);

    const bool stochasticSplats = m_context->activeSettings().EnableGaussianSplats
        && m_context->activeSettings().GaussianSplatSortingMode == 1;
    const bool temporalReset =
        (m_frameTemporalReset && *m_frameTemporalReset)
        || (m_temporalReset && *m_temporalReset);
    if (stochasticSplats
        && (m_context->activeSettings().ResetAccumulation
            || m_context->activeSettings().ResetRealtimeCaches
            || temporalReset))
    {
        *m_temporalSampleIndex = 0;
    }

    const GaussianSplatFrameInputs frameInputs{
        m_context->activeSettings(),
        int(m_frameIndex),
        int(m_sampleIndex),
        *m_temporalSampleIndex,
        renderToOutputColor,
        dm::float2(float(m_displaySize.x), float(m_displaySize.y)),
        resolveGaussianSplatShadowDirection(m_context->frameLights()),
    };
    const GaussianSplatRenderSettings settings = buildGaussianSplatRenderSettings(frameInputs);

    caustica::PlanarView splatView = *m_context->camera.view();
    if (!renderToOutputColor)
    {
        splatView.setViewport(ViewportDesc(float(m_displaySize.x), float(m_displaySize.y)));
        splatView.setPixelOffset(dm::float2::zero());
    }
    splatView.updateCache();

    (void)uploadGaussianSplatScene(
        commandList,
        m_context->frameGaussianSplats(),
        *m_scenePasses,
        splatView,
        m_accelStructs->getTopLevelAS().Get(),
        *m_renderTargets,
        settings);
}

void GaussianSplatFramePass::executeSort(caustica::rhi::CommandList* commandList)
{
    if (commandList == nullptr || !hasActiveSplats())
        return;

    assert(m_context);
    assert(m_scenePasses);

    sortGaussianSplatScene(
        commandList,
        m_context->frameGaussianSplats(),
        *m_scenePasses);
}

void GaussianSplatFramePass::executeRaster(caustica::rhi::CommandList* commandList, bool renderToOutputColor)
{
    if (commandList == nullptr || !hasActiveSplats())
        return;

    assert(m_context);
    assert(m_scenePasses);

    caustica::PlanarView splatView = *m_context->camera.view();
    if (!renderToOutputColor)
    {
        splatView.setViewport(ViewportDesc(float(m_displaySize.x), float(m_displaySize.y)));
        splatView.setPixelOffset(dm::float2::zero());
    }
    splatView.updateCache();

    const bool renderedAny = rasterGaussianSplatScene(
        commandList,
        m_context->frameGaussianSplats(),
        *m_scenePasses,
        splatView);
    m_compositeRendered = renderedAny && !renderToOutputColor;
}

void GaussianSplatFramePass::executeAccumulate(caustica::rhi::CommandList* commandList)
{
    if (commandList == nullptr || !m_compositeRendered)
        return;

    if (m_accumulationPass == nullptr || m_renderTargets == nullptr
        || m_currentColor == nullptr || m_accumulatedColor == nullptr)
        return;

    assert(m_context);
    assert(m_temporalSampleIndex);

    const bool temporalReset =
        (m_frameTemporalReset && *m_frameTemporalReset)
        || (m_temporalReset && *m_temporalReset);
    if (m_context->activeSettings().ResetAccumulation
        || m_context->activeSettings().ResetRealtimeCaches
        || temporalReset)
    {
        *m_temporalSampleIndex = 0;
        if (m_frameTemporalReset)
            *m_frameTemporalReset = false;
        if (m_temporalReset)
            *m_temporalReset = false;
    }

    const float accumulationWeight = 1.0f / float(*m_temporalSampleIndex + 1);

    caustica::PlanarView splatView = *m_context->camera.view();
    splatView.setViewport(ViewportDesc(float(m_displaySize.x), float(m_displaySize.y)));
    splatView.setPixelOffset(dm::float2::zero());
    splatView.updateCache();

    m_accumulationPass->render(commandList, splatView, splatView, accumulationWeight);

    *m_temporalSampleIndex = std::min(*m_temporalSampleIndex + 1, 1024 * 1024);
    m_compositeRendered = false;
}
