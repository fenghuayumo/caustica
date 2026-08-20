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

    caustica::rhi::BindingLayoutDesc colorSpaceLayoutDesc;
    colorSpaceLayoutDesc.visibility = caustica::rhi::ShaderType::Compute;
    colorSpaceLayoutDesc.bindings = {
        caustica::rhi::BindingLayoutItem::Texture_UAV(0),
        caustica::rhi::BindingLayoutItem::PushConstants(0, sizeof(dm::uint4)),
    };
    m_colorSpaceBindingLayout = device->createBindingLayout(colorSpaceLayoutDesc);

    caustica::rhi::BindingSetDesc colorSpaceBindingSetDesc;
    colorSpaceBindingSetDesc.bindings = {
        caustica::rhi::BindingSetItem::Texture_UAV(0, renderTargets->processedOutputColor),
        caustica::rhi::BindingSetItem::PushConstants(0, sizeof(dm::uint4)),
    };
    m_colorSpaceBindingSet = device->createBindingSet(
        colorSpaceBindingSetDesc,
        m_colorSpaceBindingLayout);

    m_colorSpaceShader = shaderFactory->createShader(
        "caustica/shaders/render/processingPasses/GaussianSplatColorSpace.hlsl",
        "main",
        nullptr,
        caustica::rhi::ShaderType::Compute);
    caustica::rhi::ComputePipelineDesc colorSpacePipelineDesc;
    colorSpacePipelineDesc.bindingLayouts = { m_colorSpaceBindingLayout };
    colorSpacePipelineDesc.CS = m_colorSpaceShader;
    m_colorSpacePipeline = device->createComputePipeline(colorSpacePipelineDesc);
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
    assert(m_renderTargets);

    GaussianSplatPrepareContext context;
    context.device = m_device;
    context.shaderFactory = m_context->shaderFactory;
    context.shaderDebug = shaderDebug;
    context.gpuSort = m_gpuSort;
    prepareGaussianSplatScenePasses(*m_scenePasses, context, *m_renderTargets);
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
GaussianSplatFramePass::prepareGraphResources(GaussianSplatRenderTarget renderTarget)
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
        renderTarget,
        dm::float2(float(m_displaySize.x), float(m_displaySize.y)),
        m_context->frameLights(),
    };
    const GaussianSplatRenderSettings settings = buildGaussianSplatRenderSettings(frameInputs);

    for (const scene::GaussianSplatRenderProxy& proxy : m_context->frameGaussianSplats())
    {
        GaussianSplatPass* pass = m_scenePasses->findPass(proxy.entity);
        if (!proxy.enabled || pass == nullptr || !pass->hasSplats())
            continue;

        GaussianSplatRenderSettings objectSettings = settings;
        objectSettings.objectToWorld = gaussianSplatObjectToWorld(proxy);

        // A .ply dropped into a running editor is created on the logic thread,
        // after prepareScenePasses(). Finish its GPU setup here on the render
        // thread before publishing resources to the frame graph. Otherwise its
        // shadow TLAS can be active while raster upload silently fails.
        if (m_gpuSort)
            pass->setGpuSort(m_gpuSort);
        assert(m_renderTargets);
        if (!pass->rasterPipelinesReady())
            pass->createPipeline(*m_renderTargets);

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

void GaussianSplatFramePass::executeUpload(
    caustica::rhi::CommandList* commandList,
    GaussianSplatRenderTarget renderTarget)
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
        renderTarget,
        dm::float2(float(m_displaySize.x), float(m_displaySize.y)),
        m_context->frameLights(),
    };
    const GaussianSplatRenderSettings settings = buildGaussianSplatRenderSettings(frameInputs);

    caustica::PlanarView splatView = *m_context->camera.view();
    if (renderTarget != GaussianSplatRenderTarget::OutputColor)
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

void GaussianSplatFramePass::executeColorSpaceConversion(
    caustica::rhi::CommandList* commandList,
    bool toLinear)
{
    if (commandList == nullptr || !m_colorSpacePipeline || !m_colorSpaceBindingSet
        || m_displaySize.x == 0 || m_displaySize.y == 0)
    {
        return;
    }

    commandList->beginMarker(toLinear
        ? "GaussianSplatsSrgbToLinear"
        : "GaussianSplatsLinearToSrgb");

    caustica::rhi::ComputeState state;
    state.pipeline = m_colorSpacePipeline;
    state.bindings = { m_colorSpaceBindingSet };
    commandList->setComputeState(state);

    const dm::uint4 constants(
        toLinear ? 1u : 0u,
        m_displaySize.x,
        m_displaySize.y,
        0u);
    commandList->setPushConstants(&constants, sizeof(constants));
    commandList->dispatch(
        dm::div_ceil(m_displaySize.x, 8u),
        dm::div_ceil(m_displaySize.y, 8u),
        1);

    commandList->endMarker();
}

void GaussianSplatFramePass::executeRaster(
    caustica::rhi::CommandList* commandList,
    GaussianSplatRenderTarget renderTarget)
{
    if (commandList == nullptr || !hasActiveSplats())
        return;

    assert(m_context);
    assert(m_scenePasses);

    caustica::PlanarView splatView = *m_context->camera.view();
    if (renderTarget != GaussianSplatRenderTarget::OutputColor)
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
    m_compositeRendered = renderedAny && renderTarget == GaussianSplatRenderTarget::ProcessedOutputColor;
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
