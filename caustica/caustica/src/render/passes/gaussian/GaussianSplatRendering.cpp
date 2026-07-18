#include <render/WorldRenderer.h>

#include <render/PathTracingContext.h>
#include <render/SceneGaussianSplatPasses.h>
#include <render/passes/gaussian/GaussianSplatGraph.h>
#include <render/passes/gaussian/GaussianSplatSceneRuntime.h>
#include <render/passes/postProcess/AccumulationPass.h>
#include <render/core/AccelStructManager.h>
#include <render/core/PathTracerSettings.h>
#include <render/gpuSort/GPUSort.h>
#include <scene/View.h>
#include <scene/SceneLightAccess.h>

#include <algorithm>

using namespace caustica;
using namespace caustica::math;
using namespace caustica::render;

void caustica::render::WorldRenderer::createGaussianTemporalRenderPasses()
{
    nvrhi::TextureDesc gaussianCurrentDesc = m_renderTargets->processedOutputColor->getDesc();
    gaussianCurrentDesc.debugName = "GaussianSplatTemporalCurrentColor";
    gaussianCurrentDesc.isUAV = false;
    gaussianCurrentDesc.isRenderTarget = false;
    gaussianCurrentDesc.useClearValue = false;
    gaussianCurrentDesc.clearValue = nvrhi::Color(0.0f);
    gaussianCurrentDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    gaussianCurrentDesc.keepInitialState = true;
    m_gaussianSplatCurrentColor = device()->createTexture(gaussianCurrentDesc);

    nvrhi::TextureDesc gaussianAccumDesc = m_renderTargets->processedOutputColor->getDesc();
    gaussianAccumDesc.debugName = "GaussianSplatTemporalAccumulatedColor";
    gaussianAccumDesc.format = nvrhi::Format::RGBA32_FLOAT;
    gaussianAccumDesc.isUAV = true;
    gaussianAccumDesc.isRenderTarget = true;
    gaussianAccumDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    gaussianAccumDesc.keepInitialState = true;
    m_gaussianSplatAccumulatedColor = device()->createTexture(gaussianAccumDesc);

    m_gaussianSplatAccumulationPass = std::make_unique<AccumulationPass>(device(), m_context->shaderFactory);
    m_gaussianSplatAccumulationPass->createPipeline();
    m_gaussianSplatAccumulationPass->createBindingSet(m_gaussianSplatCurrentColor, m_gaussianSplatAccumulatedColor, m_renderTargets->processedOutputColor);
    m_gaussianSplatTemporalReset = true;
}

void caustica::render::WorldRenderer::prepareGaussianSplatPasses()
{
    GaussianSplatPrepareContext context;
    context.device = device();
    context.shaderFactory = m_context->shaderFactory;
    context.renderTargets = m_renderTargets.get();
    context.shaderDebug = m_shaderDebug;
    context.gpuSort = m_gaussianSplatGpuSort;
    prepareGaussianSplatScenePasses(m_context->scenePasses.gaussianSplats, context);
    m_gaussianSplatGpuSort = context.gpuSort;
}

void caustica::render::WorldRenderer::buildGaussianSplatEmissionProxies()
{
    caustica::render::buildGaussianSplatEmissionProxies(
        m_gaussianSplatEmissionProxies,
        m_context->frameGaussianSplats(),
        m_context->scenePasses.gaussianSplats,
        m_context->activeSettings());
}

void caustica::render::WorldRenderer::executeGaussianSplatAccelBuild(nvrhi::ICommandList* commandList)
{
    if (commandList == nullptr || !hasActiveGaussianSplats())
        return;

    buildGaussianSplatSceneAccelStructs(
        commandList,
        m_context->frameGaussianSplats(),
        m_context->scenePasses.gaussianSplats,
        m_context->activeSettings());
}

bool caustica::render::WorldRenderer::hasActiveGaussianSplats() const
{
    if (!m_context->hasFrameScene() || !m_context->activeSettings().EnableGaussianSplats)
        return false;

    const auto gaussianSplats = m_context->frameGaussianSplats();
    return std::any_of(
        gaussianSplats.begin(),
        gaussianSplats.end(),
        [this](const scene::GaussianSplatRenderProxy& proxy) {
            return isGaussianSplatProxyActive(proxy, m_context->scenePasses.gaussianSplats);
        });
}

std::vector<GaussianSplatGraphResources>
caustica::render::WorldRenderer::prepareGaussianSplatGraphResources(bool renderToOutputColor)
{
    std::vector<GaussianSplatGraphResources> resources;
    if (!hasActiveGaussianSplats())
        return resources;

    const GaussianSplatFrameInputs frameInputs{
        m_context->activeSettings(),
        int(m_frameIndex),
        int(m_sampleIndex),
        m_gaussianSplatTemporalSampleIndex,
        renderToOutputColor,
        dm::float2(float(m_displaySize.x), float(m_displaySize.y)),
        resolveGaussianSplatShadowDirection(m_context->frameLights()),
    };
    const GaussianSplatRenderSettings settings = buildGaussianSplatRenderSettings(frameInputs);

    for (const scene::GaussianSplatRenderProxy& proxy : m_context->frameGaussianSplats())
    {
        GaussianSplatPass* pass = m_context->scenePasses.gaussianSplats.findPass(proxy.entity);
        if (!proxy.enabled || pass == nullptr || !pass->hasSplats())
            continue;

        GaussianSplatRenderSettings objectSettings = settings;
        objectSettings.objectToWorld = gaussianSplatObjectToWorld(proxy);
        pass->prepareGraphResources(objectSettings);
        resources.push_back(pass->graphResources(objectSettings));
    }
    return resources;
}

void caustica::render::WorldRenderer::executeGaussianSplatUpload(
    nvrhi::ICommandList* commandList,
    bool renderToOutputColor)
{
    m_gaussianSplatCompositeRendered = false;
    if (commandList == nullptr || !hasActiveGaussianSplats())
        return;

    const bool stochasticSplats = m_context->activeSettings().EnableGaussianSplats && m_context->activeSettings().GaussianSplatSortingMode == 1;
    if (stochasticSplats && (m_context->activeSettings().ResetAccumulation || m_context->activeSettings().ResetRealtimeCaches || m_frameGaussianSplatTemporalReset || m_gaussianSplatTemporalReset))
        m_gaussianSplatTemporalSampleIndex = 0;

    const GaussianSplatFrameInputs frameInputs{
        m_context->activeSettings(),
        int(m_frameIndex),
        int(m_sampleIndex),
        m_gaussianSplatTemporalSampleIndex,
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
        m_context->scenePasses.gaussianSplats,
        splatView,
        m_context->accelStructs.getTopLevelAS().Get(),
        *m_renderTargets,
        settings);
}

void caustica::render::WorldRenderer::executeGaussianSplatSort(nvrhi::ICommandList* commandList)
{
    if (commandList == nullptr || !hasActiveGaussianSplats())
        return;

    sortGaussianSplatScene(
        commandList,
        m_context->frameGaussianSplats(),
        m_context->scenePasses.gaussianSplats);
}

void caustica::render::WorldRenderer::executeGaussianSplatRaster(
    nvrhi::ICommandList* commandList,
    bool renderToOutputColor)
{
    if (commandList == nullptr || !hasActiveGaussianSplats())
        return;

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
        m_context->scenePasses.gaussianSplats,
        splatView);
    m_gaussianSplatCompositeRendered = renderedAny && !renderToOutputColor;
}

void caustica::render::WorldRenderer::executeGaussianSplatAccumulate(nvrhi::ICommandList* commandList)
{
    if (commandList == nullptr || !m_gaussianSplatCompositeRendered)
        return;

    if (m_gaussianSplatAccumulationPass == nullptr || m_renderTargets == nullptr
        || m_gaussianSplatCurrentColor == nullptr || m_gaussianSplatAccumulatedColor == nullptr)
        return;

    if (m_context->activeSettings().ResetAccumulation || m_context->activeSettings().ResetRealtimeCaches || m_frameGaussianSplatTemporalReset || m_gaussianSplatTemporalReset)
    {
        m_gaussianSplatTemporalSampleIndex = 0;
        m_frameGaussianSplatTemporalReset = false;
        m_gaussianSplatTemporalReset = false;
    }

    const float accumulationWeight = 1.0f / float(m_gaussianSplatTemporalSampleIndex + 1);

    caustica::PlanarView splatView = *m_context->camera.view();
    splatView.setViewport(ViewportDesc(float(m_displaySize.x), float(m_displaySize.y)));
    splatView.setPixelOffset(dm::float2::zero());
    splatView.updateCache();

    m_gaussianSplatAccumulationPass->render(commandList, splatView, splatView, accumulationWeight);

    m_gaussianSplatTemporalSampleIndex = std::min(m_gaussianSplatTemporalSampleIndex + 1, 1024 * 1024);
    m_gaussianSplatCompositeRendered = false;
}

