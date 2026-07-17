#include <render/features/RenderFeature.h>
#include <render/features/PathTraceGraphResources.h>
#include <render/features/RenderFeatureContext.h>
#include <render/pipeline/RenderGraphRegistry.h>
#include <render/pipeline/RenderPipelineRegistry.h>

namespace { constexpr int c_SwapchainCount = 3; }

#include <render/worldRenderer/WorldRenderer.h>
#include <render/worldRenderer/PathTracingFrameContext.h>
#include <scene/Scene.h>
#include <render/SceneGpuResources.h>
#include <render/SceneGaussianSplatPasses.h>
#include <render/SceneRayTracingResources.h>

#include <scene/SceneLightAccess.h>
#include <render/core/SceneGeometryUpdate.h>
#include <render/core/LightingUpdate.h>
#include <render/core/AccelStructManager.h>
#include <render/core/CameraController.h>
#include <render/core/PathTracingShaderCompiler.h>
#include <render/core/ComputePipelineRegistry.h>
#include <render/passes/lighting/MaterialGpuCache.h>
#include <render/passes/lighting/distant/EnvMapImportanceSamplingCache.h>
#include <render/passes/omm/OpacityMicromapBuilder.h>
#include <render/passes/gaussian/GaussianSplatGraph.h>
#include <render/passes/gaussian/GaussianSplatSceneRuntime.h>
#include <render/passes/debug/ShaderDebug.h>
#include <render/core/FramebufferFactory.h>
#include <assets/loader/ShaderFactory.h>
#include <assets/loader/TextureLoader.h>
#include <backend/GpuDevice.h>
#include <core/path_utils.h>
#include <core/log.h>
#include <scene/View.h>
#include <shaders/SampleConstantBuffer.h>
#include <shaders/view_cb.h>

#include <cstring>
#include <functional>

using namespace caustica;
using namespace caustica::math;
using namespace caustica::render;

namespace
{
    constexpr float c_envMapRadianceScale = 1.0f / 4.0f;

    void abortIfSubmitFailed(PathTracingFrameContext& ctx, const char* stage)
    {
        if (!ctx.submitInitializationStage(stage))
            ctx.aborted = true;
    }

    SimpleViewConstants FromPlanarViewConstants(PlanarViewConstants& view)
    {
        SimpleViewConstants ret;
        ret.matWorldToView = view.matWorldToView;
        ret.matViewToClip = view.matViewToClip;
        ret.matWorldToClipNoOffset = view.matWorldToClipNoOffset;
        ret.matClipToWorldNoOffset = view.matClipToWorldNoOffset;
        ret.matWorldToClip = view.matWorldToClip;
        ret.clipToWindowBias = view.clipToWindowBias;
        ret.clipToWindowScale = view.clipToWindowScale;
        ret.viewportOrigin = view.viewportOrigin;
        ret.viewportSize = view.viewportSize;
        ret.viewportSizeInv = view.viewportSizeInv;
        ret.pixelOffset = view.pixelOffset;
        return ret;
    }
}

void caustica::render::WorldRenderer::populateFrameView(ExtractedFrameView& view)
{
    view.displaySize = m_displaySize;
    view.renderSize = m_renderSize;
    view.displayAspectRatio = m_displayAspectRatio;

    view.postProcessView = *m_context->camera.view();
    ViewportDesc windowViewport(float(m_displaySize.x), float(m_displaySize.y));
    view.postProcessView.setViewport(windowViewport);
    view.postProcessView.updateCache();
}

void caustica::render::WorldRenderer::populateRenderFrameContext(
    nvrhi::IFramebuffer* framebuffer,
    RenderFrameContext& ctx)
{
    ctx = {};
    ctx.frame.renderer = this;
    ctx.frame.framebuffer = framebuffer;
    ctx.frame.displaySize = m_displaySize;
    ctx.frame.renderSize = m_renderSize;
    ctx.graph = &m_frameGraph;

    populateFrameView(ctx.view);

    ctx.scene = m_context->frameScene;
    ctx.sceneStructureChanged = m_context->frameSceneStructureChanged;
    ctx.sceneTransformsChanged = m_context->frameSceneTransformsChanged;
}

RenderFeatureContext caustica::render::WorldRenderer::makeRenderFeatureContext(RenderFrameContext& ctx)
{
    const bool aaReset = ctx.frame.needNewPasses || m_context->activeSettings().ResetRealtimeCaches;

    return RenderFeatureContext{
        .graph = ctx.graph,
        .renderer = this,
        .frame = &ctx.frame,
        .renderTargets = m_renderTargets.get(),
        .settings = &m_context->activeSettings(),
        .sampleConstants = &m_currentConstants,
        .targetFramebuffer = ctx.frame.framebuffer,
        .extractedView = &ctx.view,
        .bindingCache = &m_context->bindingCache,
        .blitPass = &m_context->renderDevice.blit(),
        .hasScene = m_context->hasFrameScene(),
        .aaReset = aaReset,
        .commandListWasClosed = &ctx.commandListWasClosed,
        .gaussianSplatTemporalSampleIndex = &m_gaussianSplatTemporalSampleIndex,
        .gaussianSplatTemporalReset = &m_frameGaussianSplatTemporalReset,
    };
}

void caustica::render::WorldRenderer::addRenderPipelinePlugin(std::unique_ptr<IRenderPipelinePlugin> plugin)
{
    m_pipelineRegistry.addPlugin(std::move(plugin));
}

void caustica::render::WorldRenderer::addRenderPipelinePlugin(IRenderPipelinePlugin& plugin)
{
    m_pipelineRegistry.addPlugin(plugin);
}

void caustica::render::WorldRenderer::buildFrameGraphPasses(
    RenderFrameContext& ctx,
    const RenderGraphRegistry& graphRegistry)
{
    assert(ctx.graph != nullptr);

    rg::GraphBuilder& graph = *ctx.graph;
    graph.reset();
    graph.setDevice(device());
    graph.setRenderTargetPool(&m_renderTargetPool);
    graph.setRenderBufferPool(&m_renderBufferPool);

    ctx.commandListWasClosed = false;
    ctx.graphBuilt = true;

    nvrhi::IFramebuffer* framebuffer = ctx.frame.framebuffer;
    const auto& fbinfo = framebuffer->getFramebufferInfo();

    if (m_context->activeSettings().EnableShaderDebug && m_shaderDebug)
    {
        m_shaderDebug->endFrameAndOutput(
            m_commandList,
            m_renderTargets->ldrFramebuffer->getFramebuffer(ctx.view.postProcessView),
            m_renderTargets->depth,
            fbinfo.getViewport());
    }

    RenderFeatureContext featureCtx = makeRenderFeatureContext(ctx);
    graphRegistry.build(featureCtx);
}

void caustica::render::WorldRenderer::executeFrameRenderGraph(RenderFrameContext& ctx)
{
    assert(ctx.graph != nullptr);

    SampleConstants& constants = m_currentConstants;

    ctx.graph->compile();

#ifndef NDEBUG
    if (!m_context->activeSettings().RealtimeMode)
        validateReferencePathTraceGraph(*ctx.graph, m_context->activeSettings());
#endif

    ctx.graph->execute(m_commandList);
    m_renderTargetPool.endFrame();
    m_renderBufferPool.endFrame();

    if (ctx.commandListWasClosed)
        m_commandList->writeBuffer(m_constantBuffer, &constants, sizeof(constants));

    abortIfSubmitFailed(ctx.frame, "postToneMapping");
    if (ctx.frame.aborted)
        return;

    abortIfSubmitFailed(ctx.frame, "finalBlit");
    if (ctx.frame.aborted)
        return;

    m_commandList->writeBuffer(m_constantBuffer, &constants, sizeof(constants));
}

void caustica::render::WorldRenderer::framePassSetup(PathTracingFrameContext& ctx)
{
    ctx.displaySize = m_displaySize;
    ctx.renderSize = m_renderSize;

    preRender();

    const bool realtimeModeChanged = (m_lastRealtimeMode != m_context->activeSettings().RealtimeMode);
    if (realtimeModeChanged)
    {
        m_context->activeSettings().ResetAccumulation = true;
        if (m_context->activeSettings().RealtimeMode)
        {
            m_context->activeSettings().ResetRealtimeCaches = true;
            m_context->scenePasses.rayTracing.ensureStablePlanePipelines();
        }
        m_lastRealtimeMode = m_context->activeSettings().RealtimeMode;
    }

    if (m_lastScheduledRealtimeAA >= 0 && m_lastScheduledRealtimeAA != m_context->activeSettings().RealtimeAA)
        m_context->activeSettings().ResetRealtimeCaches = true;
    m_lastScheduledRealtimeAA = m_context->activeSettings().RealtimeAA;

#if CAUSTICA_WITH_STREAMLINE
    streamlinePreRender();
#endif
#if CAUSTICA_WITH_NATIVE_DLSS
    nativeDLSSPreRender();
#endif

    m_displayAspectRatio = m_displaySize.x / float(m_displaySize.y);
    ctx.displayAspectRatio = m_displayAspectRatio;

    m_context->camera.ensureViews(m_renderSize);
}

void caustica::render::WorldRenderer::framePassEnsureRenderTargets(PathTracingFrameContext& ctx)
{
    if (m_renderTargets == nullptr || m_renderTargets->isUpdateRequired(m_renderSize, m_displaySize))
    {
        device()->waitForIdle();
        device()->runGarbageCollection();
        for (int i = 0; i < std::size(m_nrd); i++)
            m_nrd[i] = nullptr;
        m_renderTargets = nullptr;
        m_oidnDenoisedOutput = nullptr;
        resetReferenceOIDN();
        m_context->bindingCache.clear();
        m_renderTargets = std::make_unique<RenderTargets>();
        m_renderTargets->init(device(), m_renderSize, m_displaySize, true, true, c_SwapchainCount);
        m_renderTargetPool.reset();
        m_renderTargetPool.setDevice(device());
        m_renderBufferPool.reset();
        m_renderBufferPool.setDevice(device());

        ctx.needNewPasses = true;
    }
}

void caustica::render::WorldRenderer::framePassRendererInit(PathTracingFrameContext& ctx)
{
    if (m_context->gpuDevice.isShuttingDown())
    {
        ctx.aborted = true;
        return;
    }

    caustica::syncEnvMapSceneParams(m_context->activeSettings(), m_context->scenePasses.lighting.envMapSceneParams(), c_envMapRadianceScale);

    if (m_context->scenePasses.rayTracing.consumeShaderReloadRequest())
    {
        m_context->shaderFactory->clearCache();
        ctx.needNewPasses = true;
        ctx.forcePathTracingShaderReload = true;
    }

    if (m_context->activeSettings().NRDModeChanged)
    {
        ctx.needNewPasses = true;
        for (int i = 0; i < std::size(m_nrd); i++)
            m_nrd[i] = nullptr;
    }
    if (!m_context->activeSettings().actualUseStandaloneDenoiser())
    {
        for (int i = 0; i < std::size(m_nrd); i++)
            m_nrd[i] = nullptr;
    }

    if (ctx.needNewPasses)
    {
        m_context->diagnostics.progressInitializingRenderer.start("Initializing renderer...");

        if (m_context->scenePasses.lighting.materials() == nullptr)
        {
            m_context->scenePasses.lighting.materials() = std::make_shared<MaterialGpuCache>(
                std::string("PathTracerMaterialSpecializations.hlsl"), device(), m_context->textureCache, m_context->shaderFactory);
            assert(m_pathTracingShaderCompiler == nullptr);

            m_pathTracingShaderCompiler = std::make_shared<PathTracingShaderCompiler>(
                device(), m_context->scenePasses.lighting.materials(), m_bindingLayout, m_bindlessLayout);

            std::vector<std::filesystem::path> additionalShaderPaths;
            m_context->scenePasses.lighting.computePipelines() = std::make_shared<ComputePipelineRegistry>(device(), additionalShaderPaths);

            m_context->scenePasses.rayTracing.createRTPipelines();
        }

        m_context->scenePasses.lighting.materials()->createRenderPassesAndLoadMaterials(
            m_bindlessLayout, m_context->renderDevice, m_context->sessionScene,
            m_context->sessionScenePath, getLocalPath(c_AssetsFolder));
        m_context->diagnostics.progressInitializingRenderer.Set(5);
        if (m_context->scenePasses.lighting.opacityMaps())
            m_context->scenePasses.lighting.opacityMaps()->createRenderPasses(m_bindlessLayout, m_context->renderDevice);
        m_context->diagnostics.progressInitializingRenderer.Set(20);
    }

    if (m_context->sessionScene)
        m_context->scenePasses.rayTracing.recreateAccelStructs(m_commandList, *m_context->sessionScene);
    else
        m_context->scenePasses.rayTracing.accelerationStructRebuildRequested() = false;
    m_commandList = device()->createCommandList();

    if (m_context->activeSettings().actualUseRTXDIPasses() && m_rtxdiPass == nullptr)
        ctx.needNewPasses = true;
    if (!m_context->activeSettings().actualUseRTXDIPasses())
        m_rtxdiPass = nullptr;

    if (ctx.needNewPasses)
    {
        m_context->diagnostics.progressInitializingRenderer.Set(40);
        const bool preCreatePassesWaitOk = device()->waitForIdle();
        if (!preCreatePassesWaitOk)
        {
            ctx.aborted = true;
            return;
        }
        m_commandList->open();
        createRenderPasses(ctx.exposureResetRequired, m_commandList);
        m_commandList->close();
        device()->executeCommandList(m_commandList);
        const bool createPassesWaitOk = device()->waitForIdle();
        if (!createPassesWaitOk)
        {
            ctx.aborted = true;
            return;
        }
        m_context->diagnostics.progressInitializingRenderer.Set(70);
    }
}

void caustica::render::WorldRenderer::framePassShaderUpdate(PathTracingFrameContext& ctx)
{
    if (ctx.aborted || m_pathTracingShaderCompiler == nullptr)
        return;

    if (m_context->gpuDevice.isShuttingDown())
        return;

    // Hit-group rebuild uses mesh proxies from the frame snapshot (indices assigned at Extract).
    const scene::SceneRenderData* sceneData = m_context->frameScene;
    if (sceneData == nullptr && m_context->sessionScene)
        sceneData = &m_context->sessionScene->getRenderData();

    m_pathTracingShaderCompiler->update(
        sceneData,
        static_cast<unsigned int>(m_context->accelStructs.getSubInstanceData().size()),
        [this](std::vector<caustica::ShaderMacro>& macros) { m_context->scenePasses.rayTracing.fillPTPipelineGlobalMacros(macros); },
        // needNewPasses covers resize/bindings and must NOT force RT PSO recreation after
        // runtime import (that recreates DXR state objects and can hang close).
        ctx.forcePathTracingShaderReload);

    if (m_context->scenePasses.lighting.computePipelines())
        m_context->scenePasses.lighting.computePipelines()->update(ctx.needNewPasses);

    m_context->diagnostics.progressInitializingRenderer.Set(90);
}

void caustica::render::WorldRenderer::framePassBeginCommandList(PathTracingFrameContext& ctx)
{
    m_commandList->open();
    ctx.submitInitializationStage = [this, &ctx](const char* stage) -> bool {
        if (!ctx.needNewPasses)
            return true;

        m_commandList->close();
        device()->executeCommandList(m_commandList);
        const bool waitOk = device()->waitForIdle();
        if (!waitOk)
        {
            caustica::error("Renderer init synchronization failed after %s", stage);
            return false;
        }
        m_commandList->open();
        return true;
    };
}

void caustica::render::WorldRenderer::framePassSceneUpdate(PathTracingFrameContext& ctx)
{
    nvrhi::IFramebuffer* framebuffer = ctx.framebuffer;

    syncCameraViews();
    {
        const ViewportDesc viewport = m_context->camera.view()->getViewport();
        float2 jitter = m_context->camera.view()->getPixelOffset();
        float4x4 projMatrix = m_context->camera.view()->getProjectionMatrix();
        float2 viewSize = { viewport.width(), viewport.height() };
        float outputAspectRatio = m_displayAspectRatio;
        bool rowMajor = true;
        float tanHalfFOVY = 1.0f / ((rowMajor) ? (projMatrix.m_data[1 * 4 + 1]) : (projMatrix.m_data[1 + 1 * 4]));
        float fovY = atanf(tanHalfFOVY) * 2.0f;
        ctx.cameraData = BridgeCamera(
            uint(viewSize.x), uint(viewSize.y), outputAspectRatio,
            m_context->camera.camera().getPosition(),
            m_context->camera.camera().getDir(),
            m_context->camera.camera().getUp(),
            fovY, m_context->camera.zNear(), 1e7f,
            m_context->activeSettings().CameraFocalDistance, m_context->activeSettings().CameraAperture, jitter);
    }

    if ((ctx.needNewPasses || ctx.needNewBindings || m_bindingSet == nullptr) && m_shaderDebug)
        m_shaderDebug->createRenderPasses(framebuffer, m_renderTargets->depth);

    if (m_context->activeSettings().EnableShaderDebug && m_shaderDebug)
    {
        dm::float4x4 viewProj = m_context->camera.view()->getViewProjectionMatrix();
        m_shaderDebug->beginFrame(m_commandList, viewProj);
    }

    UpdateSceneGeometryParams geoParams{
        m_context->activeSettings(),
        m_context->scenePasses.rayTracing.accelerationStructRebuildRequested(),
        m_context->sessionScene,
        m_context->frameScene,
        m_context->sessionScene ? &m_context->sessionScene->getGpuResources() : nullptr,
        m_commandList,
    };
    geoParams.descriptorTable = m_context->descriptorTable.get();
    geoParams.materials = m_context->scenePasses.lighting.materials().get();
    geoParams.opacityMaps = m_context->scenePasses.lighting.opacityMaps().get();
    geoParams.frameIndex = m_context->gpuDevice.getRenderPhaseFrameIndex();
    geoParams.asyncLoadingInProgress = &m_context->diagnostics.asyncLoadingInProgress;
    caustica::updateSceneGeometry(m_context->accelStructs, geoParams);
    abortIfSubmitFailed(ctx, "updateSceneGeometry");
    if (ctx.aborted)
        return;

    preUpdateLighting(m_commandList, ctx.needNewBindings);
    abortIfSubmitFailed(ctx, "preUpdateLighting");
    if (ctx.aborted)
        return;

    if (m_rtxdiPass != nullptr)
    {
        if (ctx.needNewPasses || ctx.needNewBindings || m_bindingSet == nullptr)
            m_rtxdiPass->reset();
        rtxdiSetupFrame(framebuffer, ctx.cameraData, m_renderSize);
        abortIfSubmitFailed(ctx, "rtxdiSetupFrame");
        if (ctx.aborted)
            return;
    }

    if (ctx.needNewPasses || ctx.needNewBindings || m_bindingSet == nullptr)
    {
        m_context->diagnostics.progressInitializingRenderer.Set(95);
        abortIfSubmitFailed(ctx, "preRecreateBindingSet");
        if (ctx.aborted)
            return;

        recreateBindingSet();

        m_context->diagnostics.progressInitializingRenderer.Set(100);

        {
            nvrhi::BindingSetDesc lineBindingSetDesc;
            lineBindingSetDesc.bindings = {
                nvrhi::BindingSetItem::ConstantBuffer(0, m_constantBuffer),
                nvrhi::BindingSetItem::Texture_SRV(0, m_renderTargets->depth)
            };
            m_linesBindingSet = device()->createBindingSet(lineBindingSetDesc, m_linesBindingLayout);

            nvrhi::GraphicsPipelineDesc psoDesc;
            psoDesc.VS = m_linesVertexShader;
            psoDesc.PS = m_linesPixelShader;
            psoDesc.inputLayout = m_linesInputLayout;
            psoDesc.bindingLayouts = { m_linesBindingLayout };
            psoDesc.primType = nvrhi::PrimitiveType::LineList;
            psoDesc.renderState.depthStencilState.depthTestEnable = false;
            psoDesc.renderState.blendState.targets[0].enableBlend().setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
                .setDestBlend(nvrhi::BlendFactor::InvSrcAlpha).setSrcBlendAlpha(nvrhi::BlendFactor::Zero).setDestBlendAlpha(nvrhi::BlendFactor::One);

            m_linesPipeline = device()->createGraphicsPipeline(psoDesc, framebuffer);
        }
        m_context->diagnostics.progressInitializingRenderer.stop();
    }
}

void caustica::render::WorldRenderer::framePassPathTracePrepare(PathTracingFrameContext& ctx)
{
    if (m_toneMappingPass != nullptr)
        m_toneMappingPass->preRender(m_context->activeSettings().ToneMappingParams);
    preUpdatePathTracing(ctx.needNewPasses, m_commandList);

    abortIfSubmitFailed(ctx, "preUpdatePathTracing");
}

void caustica::render::WorldRenderer::framePassPathTrace(PathTracingFrameContext& ctx)
{
    SampleConstants& constants = m_currentConstants;
    memset(&constants, 0, sizeof(constants));

    if (!m_context->hasFrameScene())
    {
        m_commandList->writeBuffer(m_constantBuffer, &constants, sizeof(constants));
        return;
    }

    if (m_toneMappingPass != nullptr && m_context->activeSettings().EnableToneMapping)
        m_toneMappingPass->preRender(m_context->activeSettings().ToneMappingParams);

    updatePathTracerConstants(constants.ptConsts, ctx.cameraData);
    constants.MaterialCount = m_context->scenePasses.lighting.materials()->getMaterialDataCount();
    fillGaussianSplatShadowConstants(
        constants,
        m_context->activeSettings(),
        getPrimaryGaussianSplatBinding(m_context->frameGaussianSplats()),
        uint32_t(m_frameIndex & 0xffffffffu));

    constants.envMapSceneParams = m_context->scenePasses.lighting.envMapSceneParams();
    constants.envMapImportanceSamplingParams = m_context->scenePasses.lighting.environment()->getImportanceSampling()->getShaderParams();

    PlanarViewConstants view;
    m_context->camera.view()->fillPlanarViewConstants(view);
    PlanarViewConstants previousView;
    m_context->camera.viewPrevious()->fillPlanarViewConstants(previousView);
    constants.view = FromPlanarViewConstants(view);
    constants.previousView = FromPlanarViewConstants(previousView);

    constants.debug = {};
    // Use the frame snapshot (activeRuntime), not live runtimeState — with a
    // pipelined render thread an older in-flight frame must not steal a new click.
    const bool pickActive = m_context->activeRuntime().Picking.hasActivePickRequest()
        || m_context->activeSettings().ContinuousDebugFeedback;
    constants.debug.pick = pickActive;
    constants.debug.pickX = pickActive ? (m_context->activeSettings().DebugPixel.x) : (-1);
    constants.debug.pickY = pickActive ? (m_context->activeSettings().DebugPixel.y) : (-1);
    constants.debug.debugLineScale = (m_context->activeSettings().ShowDebugLines) ? (m_context->activeSettings().DebugLineScale) : (0.0f);
    constants.debug.showWireframe = m_context->activeSettings().ShowWireframe;
    constants.debug.debugViewType = (int)m_context->activeSettings().DebugView;
    constants.debug.debugViewStablePlaneIndex = (m_context->activeSettings().StablePlanesActiveCount == 1) ? (0) : (m_context->activeSettings().DebugViewStablePlaneIndex);
#if ENABLE_DEBUG_DELTA_TREE_VIZUALISATION
    constants.debug.exploreDeltaTree = (m_context->activeSettings().DebugExploreDeltaTree && constants.debug.pick) ? 1 : 0;
#else
    constants.debug.exploreDeltaTree = false;
#endif
    constants.debug.imageWidth = constants.ptConsts.imageWidth;
    constants.debug.imageHeight = constants.ptConsts.imageHeight;
    constants.debug.mouseX = m_context->activeSettings().MousePos.x;
    constants.debug.mouseY = m_context->activeSettings().MousePos.y;
    constants.debug.cameraPosW = constants.ptConsts.camera.PosW;
    constants.debug._padding0 = 0;

    constants.denoisingHitParamConsts = {
        m_context->activeSettings().ReblurSettings.hitDistanceParameters.A,
        m_context->activeSettings().ReblurSettings.hitDistanceParameters.B,
        m_context->activeSettings().ReblurSettings.hitDistanceParameters.C,
        m_context->activeSettings().ReblurSettings.hitDistanceParameters.D
    };

    updateLighting(m_commandList);
    m_context->scenePasses.rayTracing.uploadSubInstanceData(m_commandList);
    abortIfSubmitFailed(ctx, "updateLighting");
    if (ctx.aborted)
        return;

    m_commandList->writeBuffer(m_constantBuffer, &constants, sizeof(constants));
}

void caustica::render::WorldRenderer::framePassDenoiseAndAA(PathTracingFrameContext& ctx)
{
    if (!m_context->hasFrameScene())
        return;

    SampleConstants& constants = m_currentConstants;

    // Copy, TAA, DLSS, accumulation, and Gaussian splats run in the frame graph after path trace and NRD.
    applyReferenceOIDN();
    if (m_context->activeSettings().ReferenceOIDNDenoiser)
        m_commandList->writeBuffer(m_constantBuffer, &constants, sizeof(constants));

    abortIfSubmitFailed(ctx, "denoiseAndAA");
    if (ctx.aborted)
        return;

    m_commandList->writeBuffer(m_constantBuffer, &constants, sizeof(constants));
}

void caustica::render::WorldRenderer::registerDebugOverlayGraphPasses(RenderFeatureContext ctx)
{
    assert(ctx.graph);
    assert(ctx.targetFramebuffer);

    nvrhi::IFramebuffer* framebuffer = ctx.targetFramebuffer;
    const auto& fbinfo = framebuffer->getFramebufferInfo();
    const bool showDebugLines = m_context->activeSettings().ShowDebugLines;
    const bool copyDebugFeedback =
        m_context->activeSettings().ContinuousDebugFeedback
        || m_context->activeRuntime().Picking.hasActivePickRequest();

    std::vector<DebugLineStruct> cpuSideDebugLines = std::move(m_cpuSideDebugLines);
    m_cpuSideDebugLines.clear();

    rg::BufferHandle debugLineCapture{};
    rg::BufferHandle debugLineDisplay{};

    if (showDebugLines || copyDebugFeedback)
    {
        debugLineCapture = ctx.graph->importBuffer(
            m_debugLineBufferCapture,
            nvrhi::ResourceStates::Common);
        debugLineDisplay = ctx.graph->importBuffer(
            m_debugLineBufferDisplay,
            nvrhi::ResourceStates::Common);

        ctx.graph->extractBuffer(debugLineCapture, nvrhi::ResourceStates::Common);
        ctx.graph->extractBuffer(debugLineDisplay, nvrhi::ResourceStates::Common);
    }

    if (showDebugLines)
    {
        nvrhi::ITexture* targetColor = framebuffer->getDesc().colorAttachments[0].texture;
        assert(targetColor);

        const rg::TextureHandle targetColorHandle = ctx.graph->importTexture(
            targetColor,
            rg::TextureAccess::RenderTarget);
        const rg::TextureHandle depth = ctx.graph->importTexture(
            m_renderTargets->depth,
            rg::TextureAccess::ShaderResource);
        const rg::BufferHandle constantBuffer = ctx.graph->importBuffer(
            m_constantBuffer,
            rg::BufferAccess::ConstantBuffer);
        const uint32_t capturedLineVertexCount = m_feedbackData.lineVertexCount;
        const uint32_t cpuLineVertexCount = static_cast<uint32_t>(cpuSideDebugLines.size());

        if (!cpuSideDebugLines.empty())
        {
            ctx.graph->addPass(
                "UploadCpuDebugLines",
                [debugLineCapture](rg::PassBuilder& setup) {
                    setup.write(debugLineCapture, rg::BufferAccess::CopyDest);
                },
                [debugLineCapture, cpuSideDebugLines = std::move(cpuSideDebugLines)](
                    rg::RenderPassContext& passCtx) {
                    passCtx.commandList()->writeBuffer(
                        passCtx.buffer(debugLineCapture),
                        cpuSideDebugLines.data(),
                        sizeof(DebugLineStruct) * cpuSideDebugLines.size());
                },
                rg::PassOptions{ .sideEffect = true });
        }

        ctx.graph->addPass(
            "DebugLines",
            [targetColorHandle, depth, constantBuffer, debugLineCapture, debugLineDisplay](rg::PassBuilder& setup) {
                setup.write(targetColorHandle, rg::TextureAccess::RenderTarget);
                setup.read(depth, rg::TextureAccess::ShaderResource);
                setup.read(constantBuffer, rg::BufferAccess::ConstantBuffer);
                setup.read(debugLineCapture, rg::BufferAccess::VertexBuffer);
                setup.read(debugLineDisplay, rg::BufferAccess::VertexBuffer);
            },
            [this, framebuffer, viewport = fbinfo.getViewport(), capturedLineVertexCount,
                cpuLineVertexCount, debugLineCapture, debugLineDisplay](rg::RenderPassContext& passCtx) {
                nvrhi::ICommandList* commandList = passCtx.commandList();
                commandList->beginMarker("Debug Lines");

                nvrhi::GraphicsState state;
                state.bindings = { m_linesBindingSet };
                state.vertexBuffers = { {passCtx.buffer(debugLineDisplay), 0, 0} };
                state.pipeline = m_linesPipeline;
                state.framebuffer = framebuffer;
                state.viewport.addViewportAndScissorRect(viewport);
                commandList->setGraphicsState(state);

                nvrhi::DrawArguments args;
                args.vertexCount = capturedLineVertexCount;
                commandList->draw(args);

                if (cpuLineVertexCount > 0)
                {
                    state.vertexBuffers = { {passCtx.buffer(debugLineCapture), 0, 0} };
                    commandList->setGraphicsState(state);

                    args.vertexCount = cpuLineVertexCount;
                    commandList->draw(args);
                }

                commandList->endMarker();
            },
            rg::PassOptions{ .sideEffect = true, .executeAfter = "Blit" });
    }

    if (copyDebugFeedback)
    {
        const rg::BufferHandle feedbackCpu = ctx.graph->importBuffer(
            m_feedback_Buffer_Cpu,
            rg::BufferAccess::CopyDest);
        const rg::BufferHandle feedbackGpu = ctx.graph->importBuffer(
            m_feedback_Buffer_Gpu,
            nvrhi::ResourceStates::Common);
        const rg::BufferHandle debugDeltaPathTreeCpu = ctx.graph->importBuffer(
            m_debugDeltaPathTree_Cpu,
            rg::BufferAccess::CopyDest);
        const rg::BufferHandle debugDeltaPathTreeGpu = ctx.graph->importBuffer(
            m_debugDeltaPathTree_Gpu,
            nvrhi::ResourceStates::Common);

        ctx.graph->extractBuffer(feedbackCpu, rg::BufferAccess::CopyDest);
        ctx.graph->extractBuffer(feedbackGpu, nvrhi::ResourceStates::Common);
        ctx.graph->extractBuffer(debugDeltaPathTreeCpu, rg::BufferAccess::CopyDest);
        ctx.graph->extractBuffer(debugDeltaPathTreeGpu, nvrhi::ResourceStates::Common);

        ctx.graph->addPass(
            "DebugFeedbackCopies",
            [feedbackCpu, feedbackGpu, debugLineCapture, debugLineDisplay,
                debugDeltaPathTreeCpu, debugDeltaPathTreeGpu](rg::PassBuilder& setup) {
                setup.read(feedbackGpu, rg::BufferAccess::CopySource);
                setup.write(feedbackCpu, rg::BufferAccess::CopyDest);
                setup.read(debugLineCapture, rg::BufferAccess::CopySource);
                setup.write(debugLineDisplay, rg::BufferAccess::CopyDest);
                setup.read(debugDeltaPathTreeGpu, rg::BufferAccess::CopySource);
                setup.write(debugDeltaPathTreeCpu, rg::BufferAccess::CopyDest);
            },
            [feedbackCpu, feedbackGpu, debugLineCapture, debugLineDisplay,
                debugDeltaPathTreeCpu, debugDeltaPathTreeGpu](rg::RenderPassContext& passCtx) {
                nvrhi::ICommandList* commandList = passCtx.commandList();
                commandList->copyBuffer(
                    passCtx.buffer(feedbackCpu), 0,
                    passCtx.buffer(feedbackGpu), 0,
                    sizeof(DebugFeedbackStruct));
                commandList->copyBuffer(
                    passCtx.buffer(debugLineDisplay), 0,
                    passCtx.buffer(debugLineCapture), 0,
                    sizeof(DebugLineStruct) * MAX_DEBUG_LINES);
                commandList->copyBuffer(
                    passCtx.buffer(debugDeltaPathTreeCpu), 0,
                    passCtx.buffer(debugDeltaPathTreeGpu), 0,
                    sizeof(DeltaTreeVizPathVertex) * cDeltaTreeVizMaxVertices);
            },
            rg::PassOptions{
                .sideEffect = true,
                .executeAfter = showDebugLines ? "DebugLines" : "Blit",
            });
    }
}

void caustica::render::WorldRenderer::framePassFinalize(PathTracingFrameContext& ctx)
{
    nvrhi::IFramebuffer* framebuffer = ctx.framebuffer;
    nvrhi::ITexture* framebufferTexture = framebuffer->getDesc().colorAttachments[0].texture;

    m_commandList->close();
    device()->executeCommandList(m_commandList);
    if (ctx.needNewPasses)
    {
        const bool finalWaitOk = device()->waitForIdle();
        if (!finalWaitOk)
        {
            caustica::error("Renderer init synchronization failed after final submit");
            ctx.aborted = true;
            return;
        }
    }

    if (m_context->activeSettings().ContinuousDebugFeedback || m_context->activeRuntime().Picking.hasActivePickRequest())
    {
        device()->waitForIdle();
        void* pData = device()->mapBuffer(m_feedback_Buffer_Cpu, nvrhi::CpuAccessMode::Read);
        assert(pData);
        memcpy(&m_feedbackData, pData, sizeof(DebugFeedbackStruct) * 1);
        device()->unmapBuffer(m_feedback_Buffer_Cpu);

        pData = device()->mapBuffer(m_debugDeltaPathTree_Cpu, nvrhi::CpuAccessMode::Read);
        assert(pData);
        memcpy(&m_debugDeltaPathTree, pData, sizeof(DeltaTreeVizPathVertex) * cDeltaTreeVizMaxVertices);
        device()->unmapBuffer(m_debugDeltaPathTree_Cpu);
    }

    if (m_temporalAntiAliasingPass != nullptr)
        m_temporalAntiAliasingPass->advanceFrame();

    m_context->camera.swapViews();
    m_context->gpuDevice.setVsyncEnabled(m_context->activeSettings().actualEnableVsync());

    postUpdatePathTracing();
}
