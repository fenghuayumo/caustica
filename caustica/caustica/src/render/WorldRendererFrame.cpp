#include <render/FrameGraphPasses.h>
#include <render/FrameGraphContext.h>
#include <render/core/RenderTargets.h>
#include <render/graph/GraphBuilder.h>
#include <render/passes/pathTrace/PathTraceGraphResources.h>
#include <render/pipeline/RenderGraphRegistry.h>
#include <render/pipeline/RenderPipelineRegistry.h>

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
#include <render/core/PathTracingShaderCompiler.h>
#include <render/core/RtPipelineCache.h>
#include <render/core/PtPipelineFeaturePresets.h>
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

FrameGraphContext caustica::render::WorldRenderer::makeFrameGraphContext(RenderFrameContext& ctx)
{
    const bool aaReset = ctx.frame.needNewPasses || m_context->activeSettings().ResetRealtimeCaches;
    nvrhi::IDescriptorTable* descriptorTable = m_context->descriptorTable
        ? m_context->descriptorTable->getDescriptorTable()
        : nullptr;

    const bool showDebugLines = m_context->activeSettings().ShowDebugLines;
    const bool copyDebugFeedback =
        m_context->activeSettings().ContinuousDebugFeedback
        || m_context->activeRuntime().Picking.hasActivePickRequest();

    FrameGraphContext featureCtx{
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
        .rtxdi = m_rtxdiPass.get(),
        .pathTrace = m_pathTracePass.get(),
        .denoise = m_denoisePass.get(),
        .gaussian = m_gaussianFramePass.get(),
        .environment = m_context->scenePasses.lighting.environment(),
        .bindingLayout = m_bindingLayout,
        .bindingSet = m_bindingSet,
        .descriptorTable = descriptorTable,
        .constantBuffer = m_constantBuffer,
        .ptBuildStablePlanes = m_ptPipelineBuildStablePlanes.get(),
        .ptFillStablePlanes = m_ptPipelineFillStablePlanes.get(),
        .ptReference = m_ptPipelineReference.get(),
        .ptTestRaygenPPHDR = m_ptPipelineTestRaygenPPHDR.get(),
        .ptEdgeDetection = m_ptPipelineEdgeDetection.get(),
        .exportVBufferPSO = m_pathTracePass ? m_pathTracePass->exportVBufferPSO() : nullptr,
        .toneMapping = m_toneMappingPass.get(),
        .bloom = m_bloomPass.get(),
        .temporalAntiAliasing = m_temporalAntiAliasingPass.get(),
        .accumulation = m_accumulationPass.get(),
        .postProcess = m_postProcess.get(),
        .lightSampling = m_context->scenePasses.lighting.lightSampling().get(),
        .materials = m_context->scenePasses.lighting.materials(),
        .opacityMaps = m_context->scenePasses.lighting.opacityMaps(),
        .gpuHandles = m_context->resolveGpuHandles(),
        .subInstanceDataBuffer = m_context->accelStructs.getSubInstanceBuffer(),
        .pathTracingContext = m_context,
        .device = device(),
        .commandList = m_commandList,
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

    FrameGraphContext featureCtx = makeFrameGraphContext(ctx);
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
        if (m_denoisePass)
            m_denoisePass->invalidateNrdIntegrations();
    }
    if (!m_context->activeSettings().actualUseStandaloneDenoiser() && m_denoisePass)
        m_denoisePass->invalidateNrdIntegrations();

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
            m_rtPipelineCache = std::make_shared<RtPipelineCache>(m_pathTracingShaderCompiler);

            std::vector<std::filesystem::path> additionalShaderPaths;
            m_context->scenePasses.lighting.computePipelines() = std::make_shared<ComputePipelineRegistry>(device(), additionalShaderPaths);

            m_context->scenePasses.rayTracing.createRTPipelines();
        }

        std::span<const scene::MaterialRenderResourceSnapshot> materialResources;
        if (m_context->frameScene)
            materialResources = m_context->frameScene->materialSnapshots;
        m_context->scenePasses.lighting.materials()->createRenderPassesAndLoadMaterials(
            m_bindlessLayout, m_context->renderDevice, materialResources,
            m_context->sessionScenePath, getLocalPath(c_AssetsFolder));
        m_context->diagnostics.progressInitializingRenderer.Set(5);
        if (m_context->scenePasses.lighting.opacityMaps())
            m_context->scenePasses.lighting.opacityMaps()->createRenderPasses(m_bindlessLayout, m_context->renderDevice);
        m_context->diagnostics.progressInitializingRenderer.Set(20);
    }

    if (m_context->sessionScene && m_context->frameScene)
        m_context->scenePasses.rayTracing.recreateAccelStructs(
            m_commandList, *m_context->sessionScene, m_context->frameScene);
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

    // UE-style: feature toggles snap to a cooked preset and bind prebuilt RT PSOs.
    // forcePathTracingShaderReload remains for source hot-reload / Ctrl+R / scene load only.
    const PtFeaturePresetId desiredPreset = m_context->scenePasses.rayTracing.resolveFeaturePreset();

    if (m_rtPipelineCache && m_rtPipelineCache->activePreset() != desiredPreset)
        m_context->activeSettings().ResetAccumulation = true;

    m_pathTracingShaderCompiler->update(
        sceneData,
        static_cast<unsigned int>(m_context->accelStructs.getSubInstanceData().size()),
        [this](std::vector<caustica::ShaderMacro>& macros) { m_context->scenePasses.rayTracing.fillPTPipelineGlobalMacros(macros); },
        // needNewPasses covers resize/bindings and must NOT force RT PSO recreation after
        // runtime import (that recreates DXR state objects and can hang close).
        ctx.forcePathTracingShaderReload);

    if (m_rtPipelineCache)
    {
        // After update() has primed hit-group exports: bind cooked preset.
        // CreateStateObject only on cold miss / switch (never during createRTPipelines).
        // No per-frame background warmup — that belongs in cook/load precacheAll.
        if (m_pathTracingShaderCompiler->hasUniqueHitGroups()
            && (m_rtPipelineCache->activePreset() != desiredPreset
                || !m_rtPipelineCache->isReady(desiredPreset)))
        {
            m_context->scenePasses.rayTracing.ensureFeaturePresetReady(
                desiredPreset,
                /*showProgress=*/false);
        }

        m_context->diagnostics.rtPipelineWarmup = m_rtPipelineCache->status();
        m_context->diagnostics.rtPipelineCacheStats = m_rtPipelineCache->stats();
    }

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
        m_context->sessionScene ? &m_context->sceneGpuResources : nullptr,
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

    preUpdateLightingFrame(*m_context, m_commandList, ctx.needNewBindings);
    abortIfSubmitFailed(ctx, "preUpdateLighting");
    if (ctx.aborted)
        return;

    if (m_rtxdiPass != nullptr)
    {
        if (ctx.needNewPasses || ctx.needNewBindings || m_bindingSet == nullptr)
            m_rtxdiPass->reset();

        buildGaussianSplatEmissionProxies();

        const bool envMapPresent = m_context->activeSettings().EnvironmentMapParams.enabled;
        RtxdiPass::SetupParams rtxdiParams{};
        rtxdiParams.commandList = m_commandList;
        rtxdiParams.renderTargets = m_renderTargets.get();
        rtxdiParams.environment = envMapPresent ? m_context->scenePasses.lighting.environment() : nullptr;
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

    if (ctx.needNewPasses || ctx.needNewBindings || m_bindingSet == nullptr)
    {
        m_context->diagnostics.progressInitializingRenderer.Set(95);
        abortIfSubmitFailed(ctx, "preRecreateBindingSet");
        if (ctx.aborted)
            return;

        recreateBindingSet(m_context->frameScene);

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

    const dm::int2 pickPixel = pickActive
        ? displayToRenderPixel(m_context->activeSettings().DebugPixel)
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

    buildGaussianSplatEmissionProxies();
    updateLightingFrame(
        *m_context,
        m_commandList,
        m_frameIndex,
        m_gaussianSplatEmissionProxies.empty() ? nullptr : &m_gaussianSplatEmissionProxies);
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
    if (m_denoisePass)
    {
        FrameGraphContext oidnCtx{};
        oidnCtx.pathTracingContext = m_context;
        oidnCtx.device = device();
        oidnCtx.commandList = m_commandList;
        oidnCtx.renderTargets = m_renderTargets.get();
        oidnCtx.postProcess = m_postProcess.get();
        oidnCtx.bindingSet = m_bindingSet;
        oidnCtx.bindingLayout = m_bindingLayout;
        oidnCtx.constantBuffer = m_constantBuffer;
        oidnCtx.accumulationSampleIndex = m_accumulationSampleIndex;
        oidnCtx.accumulationCompleted = m_accumulationCompleted;
        m_denoisePass->bindFrame(oidnCtx);
        m_denoisePass->applyReferenceOIDN(m_commandList);
    }
    if (m_context->activeSettings().ReferenceOIDNDenoiser)
        m_commandList->writeBuffer(m_constantBuffer, &constants, sizeof(constants));

    abortIfSubmitFailed(ctx, "denoiseAndAA");
    if (ctx.aborted)
        return;

    m_commandList->writeBuffer(m_constantBuffer, &constants, sizeof(constants));
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

    // Full-device idle for pick/debug readback is expensive; skip on shutdown so
    // Close is not blocked behind a path-trace + waitForIdle frame.
    if (!m_context->gpuDevice.isShuttingDown()
        && (m_context->activeSettings().ContinuousDebugFeedback
            || m_context->activeRuntime().Picking.hasActivePickRequest()))
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

namespace caustica::render
{

void registerClearFrameTargetsPass(FrameGraphContext ctx)
{
    if (!ctx.graph || !ctx.renderTargets)
        return;

    const rg::TextureHandle depth = ctx.graph->importTexture(
        ctx.renderTargets->depth,
        rg::TextureAccess::UnorderedAccess);
    const rg::TextureHandle combinedHistoryClampRelax = ctx.graph->importTexture(
        ctx.renderTargets->combinedHistoryClampRelax,
        rg::TextureAccess::UnorderedAccess);

    ctx.graph->addPass(
        "ClearFrameTargets",
        [depth, combinedHistoryClampRelax](rg::PassBuilder& setup) {
            setup.write(depth, rg::TextureAccess::UnorderedAccess);
            setup.write(combinedHistoryClampRelax, rg::TextureAccess::UnorderedAccess);
        },
        [ctx](rg::RenderPassContext& passCtx) {
            ctx.renderTargets->clear(passCtx.commandList());
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
                    nvrhi::AllSubresources,
                    nvrhi::Color(1, 1, 0, 0));
            },
            rg::PassOptions{ .sideEffect = true, .executeAfter = "ClearFrameTargets" });
    }
}

void registerDefaultFrameGraphPasses(FrameGraphContext ctx)
{
    registerClearFrameTargetsPass(ctx);
    registerRtxdiBeginFramePass(ctx);
    registerPathTracePrePass(ctx);
    registerVBufferExportPass(ctx);
    registerPathTraceLightingEndPass(ctx);
    registerGaussianSplatAccelBuildPass(ctx);
    registerMainPathTracePass(ctx);
    registerRtxdiExecutePass(ctx);
    registerDenoiserPreparePass(ctx);
    registerNrdPass(ctx);
    registerGaussianSplatPreAAPass(ctx);
    registerDenoiseAAPass(ctx);
    registerGaussianSplatCompositePass(ctx);
    registerPostProcessGraphPasses(ctx);
    registerCompositeGraphPasses(ctx);
    registerDebugOverlayGraphPasses(ctx);
}

} // namespace caustica::render
