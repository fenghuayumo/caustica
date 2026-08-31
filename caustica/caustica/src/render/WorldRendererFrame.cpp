#include <render/FrameGraphPasses.h>
#include <render/FrameGraphContext.h>
#include <render/core/RenderTargets.h>
#include <render/graph/GraphBuilder.h>
#include <render/passes/pathTrace/PathTraceGraphResources.h>

namespace { constexpr int c_SwapchainCount = 3; }

#include <render/WorldRenderer.h>
#include <render/PathTracingFrameContext.h>
#include <scene/Scene.h>
#include <render/SceneGpuResources.h>
#include <render/SceneGaussianSplatPasses.h>
#include <render/SceneRayTracingResources.h>

#include <scene/SceneLightAccess.h>
#include <render/core/SceneGeometryUpdate.h>
#include <render/core/LightingUpdate.h>
#include <render/core/AccelStructManager.h>
#include <render/core/CameraController.h>
#include <render/core/ComputePipelineRegistry.h>
#include <render/passes/lighting/LightingFrame.h>
#include <render/passes/lighting/MaterialGpuCache.h>
#include <render/passes/lighting/distant/EnvMapImportanceSamplingCache.h>
#include <render/passes/omm/OpacityMicromapBuilder.h>
#include <render/passes/gaussian/GaussianSplatGraph.h>
#include <render/passes/gaussian/GaussianSplatSceneRuntime.h>
#include <render/passes/gaussian/GaussianSplatFramePass.h>
#include <render/passes/denoisers/DenoisePass.h>
#include <render/passes/pathTrace/PathTracePass.h>
#include <render/passes/rtxdi/RtxdiPass.h>
#include <render/passes/debug/ShaderDebug.h>
#include <render/passes/gaussian/GaussianSplatEmissionProxy.h>
#include <render/core/FramebufferFactory.h>
#include <render/core/GraphicsQueueFence.h>
#include <assets/loader/ShaderFactory.h>
#include <assets/loader/TextureLoader.h>
#include <backend/GpuDevice.h>
#include <core/path_utils.h>
#include <core/log.h>
#include <scene/View.h>
#include <shaders/FrameConstantBuffer.h>
#include <shaders/view_cb.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <utility>
#include <vector>

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
    caustica::rhi::Framebuffer* framebuffer,
    RenderFrameContext& ctx)
{
    ctx = {};
    ctx.frame.framebuffer = framebuffer;
    ctx.frame.displaySize = m_displaySize;
    ctx.frame.renderSize = m_renderSize;
    ctx.graph = &m_frameGraph;

    populateFrameView(ctx.view);

    ctx.scene = m_context->frameScene;
    ctx.sceneStructureChanged = m_context->frameSceneStructureChanged;
    ctx.sceneTransformsChanged = m_context->frameSceneTransformsChanged;
}

FrameGraphContext caustica::render::WorldRenderer::makeFrameGraphContext(RenderFrameContext& ctx)
{
    const bool aaReset = ctx.frame.needNewPasses || m_context->activeSettings().ResetRealtimeCaches;
    caustica::rhi::DescriptorTable* descriptorTable = m_context->descriptorTable
        ? m_context->descriptorTable->getDescriptorTable()
        : nullptr;

    const bool showDebugLines = m_context->activeSettings().ShowDebugLines;
    const bool copyDebugFeedback =
        m_context->activeSettings().ContinuousDebugFeedback
        || m_context->activeRuntime().Picking.hasActivePickRequest();
    const SceneRayTracingResources& rayTracing = m_context->scenePasses.rayTracing;

    FrameGraphContext featureCtx{
        .graph = ctx.graph,
        .renderTargets = m_renderTargets.get(),
        .settings = &m_context->activeSettings(),
        .frameConstants = &m_frameConstants,
        .gaussianSplatEmissionProxies = m_gaussianSplatEmissionProxies.empty()
            ? nullptr
            : &m_gaussianSplatEmissionProxies,
        .targetFramebuffer = ctx.frame.framebuffer,
        .extractedView = &ctx.view,
        .bindingCache = &m_context->bindingCache,
        .blitPass = &m_context->renderDevice.blit(),
        .rtxdi = m_rtxdiPass.get(),
        .pathTrace = m_pathTracePass.get(),
        .denoise = m_denoisePass.get(),
        .gaussian = m_gaussianFramePass.get(),
        .environment = m_context->scenePasses.lighting.environment().get(),
        .bindingLayout = m_bindingLayout,
        .bindingSet = m_sceneBindings.bindingSet(),
        .descriptorTable = descriptorTable,
        .constantBuffer = m_constantBuffer,
        .ptBuildStablePlanes = rayTracing.pipelineBuildStablePlanes(),
        .ptFillStablePlanes = rayTracing.pipelineFillStablePlanes(),
        .ptReference = rayTracing.pipelineReference(),
        .ptEdgeDetection = rayTracing.pipelineEdgeDetection(),
        .exportVBufferPSO = m_pathTracePass ? m_pathTracePass->exportVBufferPSO() : nullptr,
        .toneMapping = m_toneMappingPass.get(),
        .bloom = m_bloomPass.get(),
        .temporalAntiAliasing = m_temporalAntiAliasingPass.get(),
        .accumulation = m_accumulationPass.get(),
        .postProcess = m_postProcess.get(),
        .lightSampling = m_context->scenePasses.lighting.lightSampling().get(),
        .gpuHandles = m_context->resolveGpuHandles(),
        .subInstanceDataBuffer = m_context->accelStructs.getSubInstanceBuffer(),
        .pathTracingContext = m_context,
        .device = device(),
        .commandList = m_frameCommands->primary(),
        .accelStructs = &m_accelStructs,
        .gaussianScenePasses = &m_context->scenePasses.gaussianSplats,
        .camera = &m_context->camera,
        .renderSize = m_renderSize,
        .displaySize = m_displaySize,
        .displayAspectRatio = m_displayAspectRatio,
        .cameraJitter = computeCameraJitter(),
        .sampleIndex = m_sampleIndex,
        .frameIndex = m_frameIndex,
        .accumulationSampleIndex = m_accumulationSampleIndex,
        .accumulationCompleted = m_accumulationCompleted,
        .view = m_context->camera.view().get(),
        .compositeView = m_context->camera.view().get(),
        .hasScene = m_context->hasFrameScene(),
        .aaReset = aaReset,
        .commandListWasClosed = &ctx.commandListWasClosed,
        .gaussianSplatTemporalSampleIndex = &m_gaussianSplatTemporalSampleIndex,
        .gaussianSplatTemporalReset = &m_frameGaussianSplatTemporalReset,
        .gaussianSplatOwnedTemporalReset = &m_gaussianSplatTemporalReset,
#if CAUSTICA_WITH_STREAMLINE
        .dlssRROptions = &m_lastDLSSRROptions,
#endif
#if CAUSTICA_WITH_NATIVE_DLSS
        .nativeDLSS = m_nativeDLSS.get(),
#endif
        .showDebugLines = showDebugLines,
        .copyDebugFeedback = copyDebugFeedback,
        .capturedLineVertexCount = static_cast<uint32_t>(m_feedbackData.lineVertexCount),
        .cpuSideDebugLines = &m_cpuSideDebugLines,
        .debugLineBufferCapture = m_debugLineBufferCapture,
        .debugLineBufferDisplay = m_debugLineBufferDisplay,
        .feedbackBufferCpu = m_feedback_Buffer_Cpu,
        .feedbackBufferGpu = m_feedback_Buffer_Gpu,
        .debugDeltaPathTreeCpu = m_debugDeltaPathTree_Cpu,
        .debugDeltaPathTreeGpu = m_debugDeltaPathTree_Gpu,
        .linesBindingSet = m_linesBindingSet,
        .linesPipeline = m_linesPipeline,
    };

    if (m_denoisePass)
        m_denoisePass->bindFrame(featureCtx);
    if (m_gaussianFramePass)
        m_gaussianFramePass->bindFrame(featureCtx);

    return featureCtx;
}

void caustica::render::WorldRenderer::runFramePipeline(RenderFrameContext& ctx)
{
    if (ctx.frame.aborted)
        return;

    framePassSetup(ctx.frame);
    if (ctx.frame.aborted)
        return;

    framePassEnsureRenderTargets(ctx.frame);
    if (ctx.frame.aborted)
        return;

    framePassRendererInit(ctx.frame);
    if (ctx.frame.aborted)
        return;

    framePassShaderUpdate(ctx.frame);
    if (ctx.frame.aborted)
        return;

    framePassBeginCommandList(ctx.frame);
    if (ctx.frame.aborted)
        return;

    framePassSceneUpdate(ctx.frame);
    if (ctx.frame.aborted)
        return;

    framePassPathTracePrepare(ctx.frame);
    if (ctx.frame.aborted)
        return;

    framePassPathTrace(ctx.frame);
    if (ctx.frame.aborted)
        return;

    framePassDenoiseAndAA(ctx.frame);
    if (ctx.frame.aborted)
        return;

    FrameGraphContext graphContext = beginFrameGraph(ctx);
    registerDefaultFrameGraphPasses(graphContext);
    if (ctx.frame.aborted)
        return;

    executeFrameRenderGraph(ctx);
    if (ctx.frame.aborted)
        return;

    framePassFinalize(ctx.frame);
}

FrameGraphContext caustica::render::WorldRenderer::beginFrameGraph(RenderFrameContext& ctx)
{
    assert(ctx.graph != nullptr);
    const uint32_t telemetryFrame = m_context->gpuDevice.getRenderPhaseFrameIndex();
    ScopedFrameCpuTimer graphBuildTimer(
        &m_context->diagnostics.frameTelemetry,
        telemetryFrame,
        FrameCpuStage::GraphBuild);

    // Refresh after framePassSetup (DLSS/native DLSS may have changed m_renderSize and
    // synced camera views). populateRenderFrameContext runs before that and would leave
    // a stale display-sized snapshot — path-trace would over-dispatch into render-sized
    // buffers and corrupt the lower portion of the frame.
    populateFrameView(ctx.view);
    ctx.frame.renderSize = m_renderSize;
    ctx.frame.displaySize = m_displaySize;

    rg::GraphBuilder& graph = *ctx.graph;
    graph.reset();
    graph.setDevice(device());
    graph.setRenderTargetPool(&m_renderTargetPool);
    graph.setRenderBufferPool(&m_renderBufferPool);

    ctx.commandListWasClosed = false;
    ctx.graphBuilt = true;

    caustica::rhi::Framebuffer* framebuffer = ctx.frame.framebuffer;
    const auto& fbinfo = framebuffer->getFramebufferInfo();

    if (m_context->activeSettings().EnableShaderDebug && m_shaderDebug)
    {
        m_shaderDebug->endFrameAndOutput(
            m_frameCommands->primary(),
            m_renderTargets->ldrFramebuffer->getFramebuffer(ctx.view.postProcessView),
            m_renderTargets->depth,
            fbinfo.getViewport());
    }

    return makeFrameGraphContext(ctx);
}

void caustica::render::WorldRenderer::executeFrameRenderGraph(RenderFrameContext& ctx)
{
    assert(ctx.graph != nullptr);

    FrameConstants& constants = m_frameConstants;
    const uint32_t telemetryFrame = m_context->gpuDevice.getRenderPhaseFrameIndex();
    {
        ScopedFrameCpuTimer graphCompileTimer(
            &m_context->diagnostics.frameTelemetry,
            telemetryFrame,
            FrameCpuStage::GraphCompile);
        ctx.graph->compile();
    }

    while (auto timingFrame = ctx.graph->collectCompletedGpuTimings())
    {
        m_context->diagnostics.frameTelemetry.setGpuPassTimes(
            timingFrame->frameIndex,
            std::move(timingFrame->passes));
    }
    ctx.graph->beginGpuTimingFrame(telemetryFrame);

#ifndef NDEBUG
    if (!m_context->activeSettings().RealtimeMode)
        validateReferencePathTraceGraph(*ctx.graph, m_context->activeSettings());
#endif

    // Shared volatiles (ADR 0001 R2 binder): applied on every recordPass so parallel
    // waves (flush + fork) keep NVRHI per-list addresses valid.
    auto& volatiles = ctx.graph->volatileConstants();
    volatiles.clear();
    if (m_constantBuffer)
        volatiles.bind(m_constantBuffer.Get(), &constants, sizeof(constants));
    if (m_rtxdiPass)
    {
        if (caustica::rhi::BufferHandle rtxdiCb = m_rtxdiPass->getRTXDIConstants())
        {
            volatiles.bind(
                rtxdiCb.Get(),
                &m_rtxdiPass->bridgeConstantsCpu(),
                sizeof(RtxdiBridgeConstants));
        }
    }
    {
        ScopedFrameCpuTimer commandRecordTimer(
            &m_context->diagnostics.frameTelemetry,
            telemetryFrame,
            FrameCpuStage::CommandRecord);
        const PathTracerSettings& settings = m_context->activeSettings();
        ctx.graph->execute(*m_frameCommands, {
            .parallelWaves = settings.ParallelRenderGraphRecording,
            .minParallelRecordingCost = uint32_t(std::max(1, settings.RenderGraphMinParallelRecordingCost)),
            .maxParallelRecordingJobs = uint32_t(std::max(0, settings.RenderGraphMaxRecordingJobs)),
        });
    }
    m_context->diagnostics.frameTelemetry.setGraphStats(
        telemetryFrame,
        static_cast<uint32_t>(ctx.graph->activePassCount()),
        static_cast<uint32_t>(ctx.graph->compiledWaves().size()),
        ctx.graph->lastParallelBatchCount(),
        ctx.graph->lastCompileCacheHit());
    m_renderTargetPool.endFrame();
    m_renderBufferPool.endFrame();

    // ToneMapping / ReferenceOIDN may close+reopen primary mid-graph; they rewrite
    // FrameConstants before later passes. A final rewrite covers any leftover use.
    if (ctx.commandListWasClosed)
        m_frameCommands->primary()->writeBuffer(m_constantBuffer, &constants, sizeof(constants));

    abortIfSubmitFailed(ctx.frame, "postToneMapping");
    if (ctx.frame.aborted)
        return;

    abortIfSubmitFailed(ctx.frame, "finalBlit");
}

void caustica::render::WorldRenderer::framePassSetup(PathTracingFrameContext& ctx)
{
    ctx.displaySize = m_displaySize;
    ctx.renderSize = m_renderSize;

    preRender();

    if (m_context->scenePasses.rayTracing.consumeAccumulationResetRequest())
        m_context->activeSettings().ResetAccumulation = true;

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
        if (!waitGraphicsQueueFence("recreate render targets", /*runGc=*/true))
        {
            caustica::error("WorldRenderer: graphics fence failed before render-target recreate");
            ctx.aborted = true;
            return;
        }
        if (m_denoisePass)
        {
            m_denoisePass->invalidateNrdIntegrations();
            m_denoisePass->invalidateOidnOutput();
        }
        m_renderTargets = nullptr;
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

    const bool environmentLightPresent = std::any_of(
        m_context->frameLights().begin(),
        m_context->frameLights().end(),
        [](const scene::LightRenderProxy& light) {
            return scene::tryGetEnvironmentLightData(light.data) != nullptr;
        });
    caustica::syncEnvMapSceneParams(
        m_context->activeSettings(),
        m_context->scenePasses.lighting.envMapSceneParams(),
        c_envMapRadianceScale,
        environmentLightPresent);

    if (m_context->scenePasses.rayTracing.consumeShaderReloadRequest())
    {
        m_context->shaderFactory->clearCache();
        ctx.needNewPasses = true;
        ctx.forcePathTracingShaderReload = true;
    }

    if (m_context->activeSettings().NRDModeChanged)
    {
        ctx.needNewPasses = true;
        if (m_denoisePass)
            m_denoisePass->invalidateNrdIntegrations();
    }
    if (!m_context->activeSettings().actualUseStandaloneDenoiser() && m_denoisePass)
        m_denoisePass->invalidateNrdIntegrations();

    if (ctx.needNewPasses)
    {
        // Only show the OS progress dialog on cold init / first material bootstrap.
        // Viewport resize also sets needNewPasses; flashing a progress card
        // on every dock drag is poor UX and causes visible flicker.
        const bool coldInit = (m_context->scenePasses.lighting.materials() == nullptr);
        if (coldInit)
        {
            caustica::info("WorldRenderer: coldInit begin (materials were null)");
            m_context->diagnostics.progressInitializingRenderer.start("Preparing renderer...");
        }

        if (coldInit)
        {
            m_context->scenePasses.lighting.materials() = std::make_shared<MaterialGpuCache>(
                std::string("PathTracerMaterialSpecializations.hlsl"), device(), m_context->textureCache, m_context->shaderFactory);
            assert(!m_context->scenePasses.rayTracing.hasPipelineRuntime());
            m_context->scenePasses.rayTracing.initializePipelineRuntime(
                m_bindingLayout, m_bindlessLayout, m_context->activeSettings());

            std::vector<std::filesystem::path> additionalShaderPaths;
            m_context->scenePasses.lighting.computePipelines() = std::make_shared<ComputePipelineRegistry>(device(), additionalShaderPaths);

            caustica::info("WorldRenderer: coldInit pipelines created");
        }

        std::span<const scene::MaterialRenderResourceSnapshot> materialResources;
        if (m_context->frameScene)
            materialResources = m_context->frameScene->staticData().materialSnapshots;
        m_context->scenePasses.lighting.materials()->createRenderPassesAndLoadMaterials(
            m_bindlessLayout, m_context->renderDevice, materialResources,
            m_context->sessionScenePath, getLocalPath(c_AssetsFolder));
        if (coldInit)
            m_context->diagnostics.progressInitializingRenderer.Set(5);
        if (m_context->scenePasses.lighting.opacityMaps())
            m_context->scenePasses.lighting.opacityMaps()->createRenderPasses(m_bindlessLayout, m_context->renderDevice);
        if (coldInit)
            m_context->diagnostics.progressInitializingRenderer.Set(20);
    }

    if (m_context->sessionScene && m_context->frameScene
        && !m_context->sessionScene->structureGpuBuildInFlight())
    {
        m_frameCommands->ensurePrimary();
        if (!m_context->scenePasses.rayTracing.recreateAccelStructs(
            m_frameCommands->primary(),
            *m_context->sessionScene,
            m_context->activeSettings(),
            m_context->frameScene))
        {
            caustica::error("WorldRenderer: acceleration-structure transaction failed");
            ctx.aborted = true;
            return;
        }
    }
    else if (!m_context->sessionScene || !m_context->frameScene)
        m_context->scenePasses.rayTracing.accelerationStructRebuildRequested() = false;

    if (m_context->activeSettings().actualUseRTXDIPasses() && m_rtxdiPass == nullptr)
        ctx.needNewPasses = true;
    if (!m_context->activeSettings().actualUseRTXDIPasses())
        m_rtxdiPass = nullptr;

    if (ctx.needNewPasses)
    {
        if (m_context->diagnostics.progressInitializingRenderer.Active())
            m_context->diagnostics.progressInitializingRenderer.Set(40);
        caustica::info("WorldRenderer: needNewPasses graphics fence (pre createRenderPasses)");
        if (!waitGraphicsQueueFence("pre createRenderPasses", /*runGc=*/true))
        {
            caustica::error("WorldRenderer: pre-createRenderPasses graphics fence failed");
            ctx.aborted = true;
            return;
        }
        m_frameCommands->beginPrimary();
        createRenderPasses(ctx.exposureResetRequired, m_frameCommands->primaryHandle());
        m_frameCommands->endFrame();
        caustica::info("WorldRenderer: needNewPasses graphics fence (post createRenderPasses)");
        if (!waitGraphicsQueueFence("post createRenderPasses", /*runGc=*/false))
        {
            caustica::error("WorldRenderer: post-createRenderPasses graphics fence failed");
            ctx.aborted = true;
            return;
        }
        if (m_context->diagnostics.progressInitializingRenderer.Active())
            m_context->diagnostics.progressInitializingRenderer.Set(70);
        caustica::info("WorldRenderer: needNewPasses complete");
    }
}

void caustica::render::WorldRenderer::framePassShaderUpdate(PathTracingFrameContext& ctx)
{
    SceneRayTracingResources& rayTracing = m_context->scenePasses.rayTracing;
    if (ctx.aborted || !rayTracing.hasPipelineRuntime())
        return;

    if (m_context->gpuDevice.isShuttingDown())
        return;

    // Hit-group rebuild uses mesh proxies from the frame snapshot (indices assigned at Extract).
    rayTracing.updatePipelineRuntime(
        m_context->frameScene,
        static_cast<unsigned int>(m_context->accelStructs.getSubInstanceData().size()),
        // needNewPasses covers resize/bindings and must NOT force RT PSO recreation after
        // runtime import (that recreates DXR state objects and can hang close).
        ctx.forcePathTracingShaderReload,
        m_context->activeSettings());

    m_context->diagnostics.rtPipelineWarmup = rayTracing.pipelineWarmupStatus();
    m_context->diagnostics.rtPipelineCacheStats = rayTracing.pipelineCacheStats();

    if (m_context->scenePasses.lighting.computePipelines())
        m_context->scenePasses.lighting.computePipelines()->update(ctx.needNewPasses);

    m_context->diagnostics.progressInitializingRenderer.Set(90);
}

void caustica::render::WorldRenderer::framePassBeginCommandList(PathTracingFrameContext& ctx)
{
    m_frameCommands->beginPrimary();

    for (GpuFrameTimerSlot& slot : m_gpuFrameTimers)
    {
        if (!slot.pending || !slot.query || !device()->pollTimerQuery(slot.query))
            continue;
        const float seconds = device()->getTimerQueryTime(slot.query);
        m_context->diagnostics.frameTelemetry.setGpuTime(slot.frameIndex, double(seconds) * 1000.0);
        device()->resetTimerQuery(slot.query);
        slot.pending = false;
    }

    const uint32_t frameIndex = m_context->gpuDevice.getRenderPhaseFrameIndex();
    GpuFrameTimerSlot& timerSlot = m_gpuFrameTimers[frameIndex % m_gpuFrameTimers.size()];
    m_activeGpuFrameTimer = -1;
    if (!timerSlot.pending)
    {
        if (!timerSlot.query)
            timerSlot.query = device()->createTimerQuery();
        if (timerSlot.query)
        {
            device()->resetTimerQuery(timerSlot.query);
            timerSlot.frameIndex = frameIndex;
            m_frameCommands->primary()->beginTimerQuery(timerSlot.query);
            m_activeGpuFrameTimer = int(frameIndex % m_gpuFrameTimers.size());
        }
    }
    ctx.submitInitializationStage = [this, frame = &ctx](const char* stage) -> bool {
        if (!frame->needNewPasses)
            return true;

        // Flush init work, then wait the graphics fence (not device-wide idle).
        m_frameCommands->flushPrimary();
        if (!waitGraphicsQueueFence(stage, /*runGc=*/false))
        {
            caustica::error("Renderer init synchronization failed after %s", stage);
            return false;
        }
        return true;
    };
}

void caustica::render::WorldRenderer::framePassSceneUpdate(PathTracingFrameContext& ctx)
{
    caustica::rhi::Framebuffer* framebuffer = ctx.framebuffer;

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

    if ((ctx.needNewPasses || ctx.needNewBindings || !m_sceneBindings.ready()) && m_shaderDebug)
        m_shaderDebug->createRenderPasses(framebuffer, m_renderTargets->depth);

    if (m_context->activeSettings().EnableShaderDebug && m_shaderDebug)
    {
        dm::float4x4 viewProj = m_context->camera.view()->getViewProjectionMatrix();
        m_shaderDebug->beginFrame(m_frameCommands->primary(), viewProj);
    }

    UpdateSceneGeometryParams geoParams{
        m_context->activeSettings(),
        m_context->scenePasses.rayTracing.accelerationStructRebuildRequested(),
        m_context->sessionScene,
        m_context->frameScene,
        m_context->sessionScene ? &m_context->sceneGpuResources : nullptr,
        m_frameCommands->primary(),
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

    // Gaussian BLAS/TLAS construction happens in the frame graph. The first
    // graph execution can therefore publish a new t7/t8 pair after the global
    // scene binding set has already been made. Detect that transition (and
    // later rebuilds/releases) here so the next path-tracing dispatch cannot
    // accidentally keep the mesh TLAS/material-buffer fallbacks.
    const GaussianSplatBinding currentGaussianBinding = getPrimaryGaussianSplatBinding(
        m_context->frameGaussianSplats(),
        m_context->scenePasses.gaussianSplats);
    const GaussianSplatPass* const currentGaussianPass = currentGaussianBinding.splatPass;
    caustica::rhi::rt::AccelStruct* const currentGaussianAS = currentGaussianPass != nullptr
        ? currentGaussianPass->getTopLevelAS()
        : nullptr;
    caustica::rhi::Buffer* const currentGaussianBuffer = currentGaussianPass != nullptr
        ? currentGaussianPass->getSplatBuffer()
        : nullptr;
    if (!m_sceneBindings.matchesGaussianResources(currentGaussianAS, currentGaussianBuffer))
    {
        ctx.needNewBindings = true;
    }

    preUpdateLightingFrame(*m_context, m_frameCommands->primaryHandle(), ctx.needNewBindings);
    abortIfSubmitFailed(ctx, "preUpdateLighting");
    if (ctx.aborted)
        return;

    if (m_rtxdiPass != nullptr)
    {
        if (ctx.needNewPasses || ctx.needNewBindings || !m_sceneBindings.ready())
            m_rtxdiPass->reset();

        buildGaussianSplatEmissionProxies();

        const bool envMapPresent =
            m_context->scenePasses.lighting.envMapSceneParams().Enabled != 0.f;
        RtxdiPass::SetupParams rtxdiParams{};
        rtxdiParams.commandList = m_frameCommands->primaryHandle();
        rtxdiParams.renderTargets = m_renderTargets.get();
        rtxdiParams.environment = envMapPresent
            ? m_context->scenePasses.lighting.environment().get()
            : nullptr;
        rtxdiParams.envMapSceneParams = m_context->scenePasses.lighting.envMapSceneParams();
        rtxdiParams.renderData = m_context->frameScene;
        rtxdiParams.descriptorTable = m_context->descriptorTable
            ? m_context->descriptorTable->getDescriptorTable()
            : nullptr;
        rtxdiParams.gpuHandles = m_context->resolveGpuHandles();
        rtxdiParams.materials = m_context->scenePasses.lighting.materials();
        rtxdiParams.opacityMaps = m_context->scenePasses.lighting.opacityMaps();
        rtxdiParams.subInstanceDataBuffer = m_context->accelStructs.getSubInstanceBuffer();
        rtxdiParams.bindingLayout = m_bindingLayout;
        rtxdiParams.shaderDebug = m_shaderDebug;
        rtxdiParams.frameIndex = m_frameIndex & 0xFFFFFFFFu;
        rtxdiParams.frameDims = m_renderSize;
        rtxdiParams.cameraPosition = m_context->camera.camera().getPosition();
        rtxdiParams.userSettings = m_context->activeSettings().RTXDI;
        rtxdiParams.usingLightSampling = m_context->activeSettings().actualUseReSTIRDI();
        rtxdiParams.usingReGIR = m_context->activeSettings().actualUseReSTIRDI();
        rtxdiParams.environmentMapImportanceSampling = envMapPresent;
        rtxdiParams.resetRealtimeCaches = m_context->activeSettings().ResetRealtimeCaches;
        if (!m_gaussianSplatEmissionProxies.empty()
            && isGaussianSplatEmissionEnabled(m_context->activeSettings()))
        {
            rtxdiParams.gaussianSplatEmissionProxies = &m_gaussianSplatEmissionProxies;
            rtxdiParams.gaussianSplatEmissionObjectToWorld = float4x4::identity();
            rtxdiParams.gaussianSplatEmissionIntensity =
                m_context->activeSettings().GaussianSplatEmissionIntensity;
        }
        m_rtxdiPass->setupFrame(rtxdiParams);
        abortIfSubmitFailed(ctx, "rtxdiSetupFrame");
        if (ctx.aborted)
            return;
    }

    if (ctx.needNewPasses || ctx.needNewBindings || !m_sceneBindings.ready())
    {
        m_context->diagnostics.progressInitializingRenderer.Set(95);
        abortIfSubmitFailed(ctx, "preRecreateBindingSet");
        if (ctx.aborted)
            return;

        recreateBindingSet(m_context->frameScene);
        if (!m_sceneBindings.ready())
        {
            caustica::error("WorldRenderer: scene binding resources are not ready; aborting frame safely");
            ctx.aborted = true;
            return;
        }

        m_context->diagnostics.progressInitializingRenderer.Set(100);

        {
            caustica::rhi::BindingSetDesc lineBindingSetDesc;
            lineBindingSetDesc.bindings = {
                caustica::rhi::BindingSetItem::ConstantBuffer(0, m_constantBuffer),
                caustica::rhi::BindingSetItem::Texture_SRV(0, m_renderTargets->depth)
            };
            m_linesBindingSet = device()->createBindingSet(lineBindingSetDesc, m_linesBindingLayout);

            caustica::rhi::GraphicsPipelineDesc psoDesc;
            psoDesc.VS = m_linesVertexShader;
            psoDesc.PS = m_linesPixelShader;
            psoDesc.inputLayout = m_linesInputLayout;
            psoDesc.bindingLayouts = { m_linesBindingLayout };
            psoDesc.primType = caustica::rhi::PrimitiveType::LineList;
            psoDesc.renderState.depthStencilState.depthTestEnable = false;
            psoDesc.renderState.blendState.targets[0].enableBlend().setSrcBlend(caustica::rhi::BlendFactor::SrcAlpha)
                .setDestBlend(caustica::rhi::BlendFactor::InvSrcAlpha).setSrcBlendAlpha(caustica::rhi::BlendFactor::Zero).setDestBlendAlpha(caustica::rhi::BlendFactor::One);

            m_linesPipeline = device()->createGraphicsPipeline(psoDesc, framebuffer->getFramebufferInfo());
        }
        m_context->diagnostics.progressInitializingRenderer.stop();
    }
}

void caustica::render::WorldRenderer::framePassPathTracePrepare(PathTracingFrameContext& ctx)
{
    // RT work earlier in this frame may have invalidated accumulation after frame setup.
    if (m_context->scenePasses.rayTracing.consumeAccumulationResetRequest())
        m_context->activeSettings().ResetAccumulation = true;

    if (m_toneMappingPass != nullptr)
        m_toneMappingPass->preRender(m_context->activeSettings().ToneMappingParams);
    preUpdatePathTracing(ctx.needNewPasses, m_frameCommands->primaryHandle());

    abortIfSubmitFailed(ctx, "preUpdatePathTracing");
}

void caustica::render::WorldRenderer::submitImmediateMaterialPick(const RenderPickState& picking)
{
    if (!picking.MaterialRequested)
        return;

    const uint64_t packedPosition = (uint64_t(picking.Position.x) << 32u)
        | uint64_t(picking.Position.y);
    m_immediateMaterialPickPosition.store(packedPosition, std::memory_order_relaxed);
    m_immediateMaterialPickRequestId.store(
        picking.MaterialRequestId,
        std::memory_order_release);
}

void caustica::render::WorldRenderer::mergeImmediateMaterialPick()
{
    const uint64_t requestId = m_immediateMaterialPickRequestId.load(std::memory_order_acquire);
    const uint64_t completedId =
        m_completedImmediateMaterialPickRequestId.load(std::memory_order_acquire);
    if (requestId <= completedId
        || requestId <= m_frameRuntimeSnapshot.Picking.MaterialRequestId)
        return;

    const uint64_t packedPosition =
        m_immediateMaterialPickPosition.load(std::memory_order_relaxed);
    m_frameRuntimeSnapshot.Picking.Position = dm::uint2{
        uint32_t(packedPosition >> 32u),
        uint32_t(packedPosition & 0xffffffffu)};
    m_frameRuntimeSnapshot.Picking.MaterialRequestId = requestId;
    m_frameRuntimeSnapshot.Picking.MaterialRequested = true;
}

void caustica::render::WorldRenderer::framePassPathTrace(PathTracingFrameContext& ctx)
{
    // A click can arrive after this frame's Extract snapshot was captured. Merge
    // it at the last safe point before constants and ray dispatch are recorded.
    mergeImmediateMaterialPick();

    FrameConstants& constants = m_frameConstants;
    memset(&constants, 0, sizeof(constants));

    if (!m_context->hasFrameScene())
        return;

    if (m_toneMappingPass != nullptr && m_context->activeSettings().EnableToneMapping)
        m_toneMappingPass->preRender(m_context->activeSettings().ToneMappingParams);

    if (m_pathTracePass)
    {
        PathTracePass::FillConstantsParams fillParams{};
        fillParams.context = m_context;
        fillParams.toneMapping = m_toneMappingPass.get();
        fillParams.renderTargets = m_renderTargets.get();
        fillParams.renderSize = m_renderSize;
        fillParams.displaySize = m_displaySize;
        fillParams.sampleIndex = m_sampleIndex;
        fillParams.frameIndex = m_frameIndex;
        m_pathTracePass->fillConstants(constants.ptConsts, ctx.cameraData, fillParams);
    }
    constants.MaterialCount = m_context->scenePasses.lighting.materials()->getMaterialDataCount();
    fillGaussianSplatShadowConstants(
        constants,
        m_context->activeSettings(),
        getPrimaryGaussianSplatBinding(
            m_context->frameGaussianSplats(),
            m_context->scenePasses.gaussianSplats),
        uint32_t(m_frameIndex & 0xffffffffu),
        resolveGaussianSplatShadowDirection(m_context->frameLights()));

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

    // DebugPixel / MousePos are display/window pixels from the host. Convert to
    // this frame's path-trace space only here — after DLSS has settled m_renderSize.
    // Input must not pre-scale with live getRenderSize() (render thread resets it
    // to framebuffer size at the start of every render()).
    auto displayToRenderPixel = [this](dm::uint2 displayPixel) -> dm::int2 {
        if (m_displaySize.x == 0 || m_displaySize.y == 0
            || m_renderSize.x == 0 || m_renderSize.y == 0)
            return { -1, -1 };
        const int x = int(displayPixel.x * m_renderSize.x / m_displaySize.x);
        const int y = int(displayPixel.y * m_renderSize.y / m_displaySize.y);
        if (x < 0 || y < 0 || x >= int(m_renderSize.x) || y >= int(m_renderSize.y))
            return { -1, -1 };
        return { x, y };
    };

    const dm::uint2 pickDisplayPixel =
        m_context->activeRuntime().Picking.hasActivePickRequest()
        ? m_context->activeRuntime().Picking.Position
        : m_context->activeSettings().DebugPixel;
    const dm::int2 pickPixel = pickActive
        ? displayToRenderPixel(pickDisplayPixel)
        : dm::int2{ -1, -1 };
    const dm::int2 mousePixel = displayToRenderPixel(m_context->activeSettings().MousePos);

    constants.debug.pickX = pickPixel.x;
    constants.debug.pickY = pickPixel.y;
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
    constants.debug.mouseX = mousePixel.x;
    constants.debug.mouseY = mousePixel.y;
    constants.debug.cameraPosW = constants.ptConsts.camera.PosW;
    constants.debug._padding0 = 0;

    constants.denoisingHitParamConsts = {
        m_context->activeSettings().ReblurSettings.hitDistanceParameters.A,
        m_context->activeSettings().ReblurSettings.hitDistanceParameters.B,
        m_context->activeSettings().ReblurSettings.hitDistanceParameters.C,
        m_context->activeSettings().ReblurSettings.hitDistanceParameters.D
    };

    // FrameConstants / EnvMap / LightSampling / SubInstance / OIDN / AA are graph-owned.
    buildGaussianSplatEmissionProxies();
}

void caustica::render::WorldRenderer::framePassDenoiseAndAA(PathTracingFrameContext& ctx)
{
    // Denoise / TAA / DLSS / Accumulation / ReferenceOIDN record inside the frame graph.
    (void)ctx;
}

void caustica::render::WorldRenderer::mapDebugFeedbackReadback()
{
    void* pData = device()->mapBuffer(m_feedback_Buffer_Cpu, caustica::rhi::CpuAccessMode::Read);
    assert(pData);
    memcpy(&m_feedbackData, pData, sizeof(DebugFeedbackStruct) * 1);
    device()->unmapBuffer(m_feedback_Buffer_Cpu);

    pData = device()->mapBuffer(m_debugDeltaPathTree_Cpu, caustica::rhi::CpuAccessMode::Read);
    assert(pData);
    memcpy(&m_debugDeltaPathTree, pData, sizeof(DeltaTreeVizPathVertex) * cDeltaTreeVizMaxVertices);
    device()->unmapBuffer(m_debugDeltaPathTree_Cpu);
}

bool caustica::render::WorldRenderer::waitGraphicsQueueFence(const char* reason, bool runGc)
{
    return syncGraphicsQueueFence(device(), m_graphicsSyncQuery, runGc, reason);
}

void caustica::render::WorldRenderer::framePassFinalize(PathTracingFrameContext& ctx)
{
    caustica::rhi::Framebuffer* framebuffer = ctx.framebuffer;
    caustica::rhi::Texture* framebufferTexture = framebuffer->getDesc().colorAttachments[0].texture;

    if (m_activeGpuFrameTimer >= 0)
    {
        GpuFrameTimerSlot& timerSlot = m_gpuFrameTimers[size_t(m_activeGpuFrameTimer)];
        m_frameCommands->primary()->endTimerQuery(timerSlot.query);
        timerSlot.pending = true;
        m_activeGpuFrameTimer = -1;
    }
    m_frameCommands->endFrame();

    if (m_toneMappingPass != nullptr)
        m_toneMappingPass->onFrameSubmitted();

    if (ctx.needNewPasses)
    {
        if (!waitGraphicsQueueFence("needNewPasses final", /*runGc=*/false))
        {
            caustica::error("Renderer init synchronization failed after final submit");
            ctx.aborted = true;
            return;
        }
    }

    // ADR 0002 S1: graphics-queue EventQuery instead of device-wide waitForIdle.
    // ContinuousDebugFeedback maps last frame (1-frame lag); active pick waits the
    // just-submitted queue fence so click-to-select stays responsive.
    const bool wantFeedback = !m_context->gpuDevice.isShuttingDown()
        && (m_context->activeSettings().ContinuousDebugFeedback
            || m_context->activeRuntime().Picking.hasActivePickRequest());
    const bool pickActive = !m_context->gpuDevice.isShuttingDown()
        && m_context->activeRuntime().Picking.hasActivePickRequest();

    if (m_feedbackReadbackPending && m_feedbackReadbackQuery)
    {
        if (pickActive)
        {
            // Current-frame fence wait below supersedes a lagged ContinuousDebug sample.
            device()->resetEventQuery(m_feedbackReadbackQuery);
            m_feedbackReadbackPending = false;
        }
        else if (device()->pollEventQuery(m_feedbackReadbackQuery))
        {
            mapDebugFeedbackReadback();
            device()->resetEventQuery(m_feedbackReadbackQuery);
            m_feedbackReadbackPending = false;
        }
    }

    if (wantFeedback)
    {
        if (!m_feedbackReadbackQuery)
            m_feedbackReadbackQuery = device()->createEventQuery();
        if (m_feedbackReadbackQuery)
        {
            // THREADING: queue fence, RT-only — ADR 0002 S1 (debug/pick readback).
            device()->resetEventQuery(m_feedbackReadbackQuery);
            device()->setEventQuery(m_feedbackReadbackQuery, caustica::rhi::CommandQueue::Graphics);
            if (pickActive)
            {
                device()->waitEventQuery(m_feedbackReadbackQuery);
                mapDebugFeedbackReadback();
                device()->resetEventQuery(m_feedbackReadbackQuery);
                m_feedbackReadbackPending = false;
            }
            else
            {
                m_feedbackReadbackPending = true;
            }
        }
    }

    if (m_temporalAntiAliasingPass != nullptr)
        m_temporalAntiAliasingPass->advanceFrame();

    m_context->camera.swapViews();
    m_context->gpuDevice.setVsyncEnabled(m_context->activeSettings().actualEnableVsync());

    postUpdatePathTracing();
}

namespace caustica::render
{

rg::PassHandle registerClearFrameTargetsPass(FrameGraphContext ctx)
{
    if (!ctx.graph || !ctx.renderTargets)
        return {};

    const rg::TextureHandle depth = ctx.graph->importTexture(
        ctx.renderTargets->depth,
        rg::TextureAccess::UnorderedAccess);
    const rg::TextureHandle combinedHistoryClampRelax = ctx.graph->importTexture(
        ctx.renderTargets->combinedHistoryClampRelax,
        rg::TextureAccess::UnorderedAccess);
    RenderTargets* const renderTargets = ctx.renderTargets;

    const rg::PassHandle clearFrameTargets = ctx.graph->addPass(
        "ClearFrameTargets",
        [depth, combinedHistoryClampRelax](rg::PassBuilder& setup) {
            setup.write(depth, rg::TextureAccess::UnorderedAccess);
            setup.write(combinedHistoryClampRelax, rg::TextureAccess::UnorderedAccess);
        },
        [renderTargets](rg::RenderPassContext& passCtx) {
            renderTargets->clear(passCtx.commandList());
        },
        rg::PassOptions{ .sideEffect = true });

    if (!ctx.hasScene)
    {
        const rg::TextureHandle outputColor = ctx.graph->importTexture(
            ctx.renderTargets->outputColor,
            rg::TextureAccess::UnorderedAccess);

        ctx.graph->addPass(
            "ClearNoSceneOutput",
            [outputColor](rg::PassBuilder& setup) {
                setup.write(outputColor, rg::TextureAccess::UnorderedAccess);
            },
            [outputColor](rg::RenderPassContext& passCtx) {
                passCtx.commandList()->clearTextureFloat(
                    passCtx.texture(outputColor),
                    caustica::rhi::AllSubresources,
                    caustica::rhi::Color(1, 1, 0, 0));
            },
            rg::PassOptions{ .sideEffect = true, .after = clearFrameTargets });
    }

    return clearFrameTargets;
}

void registerDefaultFrameGraphPasses(FrameGraphContext ctx)
{
    assert(ctx.settings);

    const rg::PassHandle clear = registerClearFrameTargetsPass(ctx);
    const rg::PassHandle frameConstants = registerUploadFrameConstantsPass(ctx, clear);
    const rg::PassHandle lightingReady = registerLightingGraphPasses(ctx, frameConstants);
    const rg::PassHandle rtxdiBeginReady = registerRtxdiBeginFramePass(ctx, lightingReady);
    const rg::PassHandle pathTracePreReady = registerPathTracePrePass(ctx, rtxdiBeginReady);
    const rg::PassHandle vbufferReady = registerVBufferExportPass(ctx, pathTracePreReady);

    const rg::PassHandle pathTraceInputsReady = ctx.settings->RealtimeMode
        ? vbufferReady
        : lightingReady;
    const rg::PassHandle lightingEndReady =
        registerPathTraceLightingEndPass(ctx, pathTraceInputsReady);
    const rg::PassHandle gaussianAccelReady =
        registerGaussianSplatAccelBuildPass(ctx, lightingEndReady);
    const rg::PassHandle mainPathTraceReady = registerMainPathTracePass(ctx, gaussianAccelReady);
    const rg::PassHandle rtxdiExecuteReady = registerRtxdiExecutePass(ctx, mainPathTraceReady);
    const rg::PassHandle denoiseGuidesReady = registerDenoiserPreparePass(ctx, rtxdiExecuteReady);
    (void)registerNrdPass(ctx, denoiseGuidesReady);
    (void)registerGaussianSplatPreAAPass(ctx);
    (void)registerDenoiseAAPass(ctx);
    if (ctx.settings->GaussianSplatApplyToneMapping)
        (void)registerGaussianSplatCompositePass(ctx);
    registerPostProcessGraphPasses(ctx);
    const rg::PassHandle blitReady = registerCompositeGraphPasses(ctx);
    (void)registerDebugOverlayGraphPasses(ctx, blitReady);
}

} // namespace caustica::render
