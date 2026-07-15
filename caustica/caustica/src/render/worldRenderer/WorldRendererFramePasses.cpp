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

    view.postProcessView = *m_context.camera.view();
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

    if (const std::shared_ptr<Scene> scene = m_context.sceneManager.getScene())
    {
        const uint32_t frameIndex = m_context.gpuDevice.getRenderPhaseFrameIndex();
        ctx.scene = &scene->getRenderData();
        ctx.sceneStructureChanged = scene->hasSceneStructureChanged(frameIndex);
        ctx.sceneTransformsChanged = scene->hasSceneTransformsChanged(frameIndex);
    }
}

RenderFeatureContext caustica::render::WorldRenderer::makeRenderFeatureContext(RenderFrameContext& ctx)
{
    const bool aaReset = ctx.frame.needNewPasses || m_context.activeSettings().ResetRealtimeCaches;

    return RenderFeatureContext{
        .graph = ctx.graph,
        .renderer = this,
        .frame = &ctx.frame,
        .renderTargets = m_renderTargets.get(),
        .settings = &m_context.activeSettings(),
        .sampleConstants = &m_currentConstants,
        .targetFramebuffer = ctx.frame.framebuffer,
        .extractedView = &ctx.view,
        .bindingCache = &m_context.bindingCache,
        .blitPass = &m_context.renderDevice.blit(),
        .hasScene = m_context.sceneManager.getScene() != nullptr,
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

    if (m_context.activeSettings().EnableShaderDebug && m_shaderDebug)
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
    if (!m_context.activeSettings().RealtimeMode)
        validateReferencePathTraceGraph(*ctx.graph, m_context.activeSettings());
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

    const bool realtimeModeChanged = (m_lastRealtimeMode != m_context.activeSettings().RealtimeMode);
    if (realtimeModeChanged)
    {
        m_context.activeSettings().ResetAccumulation = true;
        if (m_context.activeSettings().RealtimeMode)
        {
            m_context.activeSettings().ResetRealtimeCaches = true;
            m_context.scenePasses.rayTracing.ensureStablePlanePipelines();
        }
        m_lastRealtimeMode = m_context.activeSettings().RealtimeMode;
    }

    if (m_lastScheduledRealtimeAA >= 0 && m_lastScheduledRealtimeAA != m_context.activeSettings().RealtimeAA)
        m_context.activeSettings().ResetRealtimeCaches = true;
    m_lastScheduledRealtimeAA = m_context.activeSettings().RealtimeAA;

#if CAUSTICA_WITH_STREAMLINE
    streamlinePreRender();
#endif
#if CAUSTICA_WITH_NATIVE_DLSS
    nativeDLSSPreRender();
#endif

    m_displayAspectRatio = m_displaySize.x / float(m_displaySize.y);
    ctx.displayAspectRatio = m_displayAspectRatio;

    m_context.camera.ensureViews(m_renderSize);
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
        m_context.bindingCache.clear();
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
    caustica::syncEnvMapSceneParams(m_context.activeSettings(), m_context.scenePasses.lighting.envMapSceneParams(), c_envMapRadianceScale);

    if (m_context.scenePasses.rayTracing.consumeShaderReloadRequest())
    {
        m_context.shaderFactory->clearCache();
        ctx.needNewPasses = true;
    }

    if (m_context.activeSettings().NRDModeChanged)
    {
        ctx.needNewPasses = true;
        for (int i = 0; i < std::size(m_nrd); i++)
            m_nrd[i] = nullptr;
    }
    if (!m_context.activeSettings().actualUseStandaloneDenoiser())
    {
        for (int i = 0; i < std::size(m_nrd); i++)
            m_nrd[i] = nullptr;
    }

    if (ctx.needNewPasses)
    {
        m_context.diagnostics.progressInitializingRenderer.start("Initializing renderer...");

        if (m_context.scenePasses.lighting.materials() == nullptr)
        {
            m_context.scenePasses.lighting.materials() = std::make_shared<MaterialGpuCache>(
                std::string("PathTracerMaterialSpecializations.hlsl"), device(), m_context.textureCache, m_context.shaderFactory);
            assert(m_pathTracingShaderCompiler == nullptr);

            m_pathTracingShaderCompiler = std::make_shared<PathTracingShaderCompiler>(
                device(), m_context.scenePasses.lighting.materials(), m_bindingLayout, m_bindlessLayout);

            std::vector<std::filesystem::path> additionalShaderPaths;
            m_context.scenePasses.lighting.computePipelines() = std::make_shared<ComputePipelineRegistry>(device(), additionalShaderPaths);

            m_context.scenePasses.rayTracing.createRTPipelines();
        }

        m_context.scenePasses.lighting.materials()->createRenderPassesAndLoadMaterials(
            m_bindlessLayout, m_context.renderDevice, m_context.sceneManager.getScene(),
            m_context.sceneManager.getCurrentScenePath(), getLocalPath(c_AssetsFolder));
        m_context.diagnostics.progressInitializingRenderer.Set(5);
        if (m_context.scenePasses.lighting.opacityMaps())
            m_context.scenePasses.lighting.opacityMaps()->createRenderPasses(m_bindlessLayout, m_context.renderDevice);
        m_context.diagnostics.progressInitializingRenderer.Set(20);
    }

    m_context.scenePasses.rayTracing.recreateAccelStructs(m_commandList);
    m_commandList = device()->createCommandList();

    if (m_context.activeSettings().actualUseRTXDIPasses() && m_rtxdiPass == nullptr)
        ctx.needNewPasses = true;
    if (!m_context.activeSettings().actualUseRTXDIPasses())
        m_rtxdiPass = nullptr;

    if (ctx.needNewPasses)
    {
        m_context.diagnostics.progressInitializingRenderer.Set(40);
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
        m_context.diagnostics.progressInitializingRenderer.Set(70);
    }
}

void caustica::render::WorldRenderer::framePassShaderUpdate(PathTracingFrameContext& ctx)
{
    if (ctx.aborted || m_pathTracingShaderCompiler == nullptr)
        return;

    // Hit-group rebuild uses instance indices already assigned at Extract.
    if (const auto scene = m_context.sceneManager.getScene())
        scene->syncRenderSnapshotGpuIndices(m_context.gpuDevice.getRenderPhaseFrameIndex());

    m_pathTracingShaderCompiler->update(
        m_context.sceneManager.getScene(),
        static_cast<unsigned int>(m_context.accelStructs.getSubInstanceData().size()),
        [this](std::vector<caustica::ShaderMacro>& macros) { m_context.scenePasses.rayTracing.fillPTPipelineGlobalMacros(macros); },
        ctx.needNewPasses);

    if (m_context.scenePasses.lighting.computePipelines())
        m_context.scenePasses.lighting.computePipelines()->update(ctx.needNewPasses);

    m_context.diagnostics.progressInitializingRenderer.Set(90);
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
        const ViewportDesc viewport = m_context.camera.view()->getViewport();
        float2 jitter = m_context.camera.view()->getPixelOffset();
        float4x4 projMatrix = m_context.camera.view()->getProjectionMatrix();
        float2 viewSize = { viewport.width(), viewport.height() };
        float outputAspectRatio = m_displayAspectRatio;
        bool rowMajor = true;
        float tanHalfFOVY = 1.0f / ((rowMajor) ? (projMatrix.m_data[1 * 4 + 1]) : (projMatrix.m_data[1 + 1 * 4]));
        float fovY = atanf(tanHalfFOVY) * 2.0f;
        ctx.cameraData = BridgeCamera(
            uint(viewSize.x), uint(viewSize.y), outputAspectRatio,
            m_context.camera.camera().getPosition(),
            m_context.camera.camera().getDir(),
            m_context.camera.camera().getUp(),
            fovY, m_context.camera.zNear(), 1e7f,
            m_context.activeSettings().CameraFocalDistance, m_context.activeSettings().CameraAperture, jitter);
    }

    if ((ctx.needNewPasses || ctx.needNewBindings || m_bindingSet == nullptr) && m_shaderDebug)
        m_shaderDebug->createRenderPasses(framebuffer, m_renderTargets->depth);

    if (m_context.activeSettings().EnableShaderDebug && m_shaderDebug)
    {
        dm::float4x4 viewProj = m_context.camera.view()->getViewProjectionMatrix();
        m_shaderDebug->beginFrame(m_commandList, viewProj);
    }

    UpdateSceneGeometryParams geoParams{
        m_context.activeSettings(),
        m_context.scenePasses.rayTracing.accelerationStructRebuildRequested(),
        m_context.sceneManager.getScene(),
        m_commandList,
    };
    geoParams.materials = m_context.scenePasses.lighting.materials().get();
    geoParams.opacityMaps = m_context.scenePasses.lighting.opacityMaps().get();
    geoParams.frameIndex = m_context.gpuDevice.getRenderPhaseFrameIndex();
    geoParams.asyncLoadingInProgress = &m_context.diagnostics.asyncLoadingInProgress;
    caustica::updateSceneGeometry(m_context.accelStructs, geoParams);
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
        m_context.diagnostics.progressInitializingRenderer.Set(95);
        abortIfSubmitFailed(ctx, "preRecreateBindingSet");
        if (ctx.aborted)
            return;

        recreateBindingSet();

        m_context.diagnostics.progressInitializingRenderer.Set(100);

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
        m_context.diagnostics.progressInitializingRenderer.stop();
    }
}

void caustica::render::WorldRenderer::framePassPathTracePrepare(PathTracingFrameContext& ctx)
{
    if (m_toneMappingPass != nullptr)
        m_toneMappingPass->preRender(m_context.activeSettings().ToneMappingParams);
    preUpdatePathTracing(ctx.needNewPasses, m_commandList);

    abortIfSubmitFailed(ctx, "preUpdatePathTracing");
}

void caustica::render::WorldRenderer::framePassPathTrace(PathTracingFrameContext& ctx)
{
    SampleConstants& constants = m_currentConstants;
    memset(&constants, 0, sizeof(constants));

    if (m_context.sceneManager.getScene() == nullptr)
    {
        m_commandList->clearTextureFloat(m_renderTargets->outputColor, nvrhi::AllSubresources, nvrhi::Color(1, 1, 0, 0));
        m_commandList->writeBuffer(m_constantBuffer, &constants, sizeof(constants));
        return;
    }

    if (m_toneMappingPass != nullptr && m_context.activeSettings().EnableToneMapping)
        m_toneMappingPass->preRender(m_context.activeSettings().ToneMappingParams);

    updatePathTracerConstants(constants.ptConsts, ctx.cameraData);
    constants.MaterialCount = m_context.scenePasses.lighting.materials()->getMaterialDataCount();
    fillGaussianSplatShadowConstants(
        constants,
        m_context.activeSettings(),
        getPrimaryGaussianSplatBinding(m_context.scenePasses.gaussianSplats),
        uint32_t(m_frameIndex & 0xffffffffu));

    constants.envMapSceneParams = m_context.scenePasses.lighting.envMapSceneParams();
    constants.envMapImportanceSamplingParams = m_context.scenePasses.lighting.environment()->getImportanceSampling()->getShaderParams();

    PlanarViewConstants view;
    m_context.camera.view()->fillPlanarViewConstants(view);
    PlanarViewConstants previousView;
    m_context.camera.viewPrevious()->fillPlanarViewConstants(previousView);
    constants.view = FromPlanarViewConstants(view);
    constants.previousView = FromPlanarViewConstants(previousView);

    constants.debug = {};
    // Use the frame snapshot (activeRuntime), not live runtimeState — with a
    // pipelined render thread an older in-flight frame must not steal a new click.
    const bool pickActive = m_context.activeRuntime().Picking.hasActivePickRequest()
        || m_context.activeSettings().ContinuousDebugFeedback;
    constants.debug.pick = pickActive;
    constants.debug.pickX = pickActive ? (m_context.activeSettings().DebugPixel.x) : (-1);
    constants.debug.pickY = pickActive ? (m_context.activeSettings().DebugPixel.y) : (-1);
    constants.debug.debugLineScale = (m_context.activeSettings().ShowDebugLines) ? (m_context.activeSettings().DebugLineScale) : (0.0f);
    constants.debug.showWireframe = m_context.activeSettings().ShowWireframe;
    constants.debug.debugViewType = (int)m_context.activeSettings().DebugView;
    constants.debug.debugViewStablePlaneIndex = (m_context.activeSettings().StablePlanesActiveCount == 1) ? (0) : (m_context.activeSettings().DebugViewStablePlaneIndex);
#if ENABLE_DEBUG_DELTA_TREE_VIZUALISATION
    constants.debug.exploreDeltaTree = (m_context.activeSettings().DebugExploreDeltaTree && constants.debug.pick) ? 1 : 0;
#else
    constants.debug.exploreDeltaTree = false;
#endif
    constants.debug.imageWidth = constants.ptConsts.imageWidth;
    constants.debug.imageHeight = constants.ptConsts.imageHeight;
    constants.debug.mouseX = m_context.activeSettings().MousePos.x;
    constants.debug.mouseY = m_context.activeSettings().MousePos.y;
    constants.debug.cameraPosW = constants.ptConsts.camera.PosW;
    constants.debug._padding0 = 0;

    constants.denoisingHitParamConsts = {
        m_context.activeSettings().ReblurSettings.hitDistanceParameters.A,
        m_context.activeSettings().ReblurSettings.hitDistanceParameters.B,
        m_context.activeSettings().ReblurSettings.hitDistanceParameters.C,
        m_context.activeSettings().ReblurSettings.hitDistanceParameters.D
    };

    updateLighting(m_commandList);
    m_context.scenePasses.rayTracing.uploadSubInstanceData(m_commandList);
    abortIfSubmitFailed(ctx, "updateLighting");
    if (ctx.aborted)
        return;

    m_commandList->writeBuffer(m_constantBuffer, &constants, sizeof(constants));
}

void caustica::render::WorldRenderer::framePassDenoiseAndAA(PathTracingFrameContext& ctx)
{
    if (m_context.sceneManager.getScene() == nullptr)
        return;

    SampleConstants& constants = m_currentConstants;

    // Copy, TAA, DLSS, accumulation, and Gaussian splats run in the frame graph after path trace and NRD.
    applyReferenceOIDN();
    if (m_context.activeSettings().ReferenceOIDNDenoiser)
        m_commandList->writeBuffer(m_constantBuffer, &constants, sizeof(constants));

    abortIfSubmitFailed(ctx, "denoiseAndAA");
    if (ctx.aborted)
        return;

    m_commandList->writeBuffer(m_constantBuffer, &constants, sizeof(constants));
}

void caustica::render::WorldRenderer::framePassDebugOverlay(PathTracingFrameContext& ctx)
{
    nvrhi::IFramebuffer* framebuffer = ctx.framebuffer;
    const auto& fbinfo = framebuffer->getFramebufferInfo();

    if (m_context.activeSettings().ShowDebugLines == true)
    {
        m_commandList->beginMarker("Debug Lines");

        {
            nvrhi::GraphicsState state;
            state.bindings = { m_linesBindingSet };
            state.vertexBuffers = { {m_debugLineBufferDisplay, 0, 0} };
            state.pipeline = m_linesPipeline;
            state.framebuffer = framebuffer;
            state.viewport.addViewportAndScissorRect(fbinfo.getViewport());

            m_commandList->setGraphicsState(state);

            nvrhi::DrawArguments args;
            args.vertexCount = m_feedbackData.lineVertexCount;
            m_commandList->draw(args);
        }

        if (m_cpuSideDebugLines.size() > 0)
        {
            m_commandList->writeBuffer(m_debugLineBufferCapture, m_cpuSideDebugLines.data(), sizeof(DebugLineStruct) * m_cpuSideDebugLines.size());

            nvrhi::GraphicsState state;
            state.bindings = { m_linesBindingSet };
            state.vertexBuffers = { {m_debugLineBufferCapture, 0, 0} };
            state.pipeline = m_linesPipeline;
            state.framebuffer = framebuffer;
            state.viewport.addViewportAndScissorRect(fbinfo.getViewport());

            m_commandList->setGraphicsState(state);

            nvrhi::DrawArguments args;
            args.vertexCount = (uint32_t)m_cpuSideDebugLines.size();
            m_commandList->draw(args);
        }

        m_commandList->endMarker();
    }
    m_cpuSideDebugLines.clear();

    if (m_context.activeSettings().ContinuousDebugFeedback || m_context.activeRuntime().Picking.hasActivePickRequest())
    {
        m_commandList->copyBuffer(m_feedback_Buffer_Cpu, 0, m_feedback_Buffer_Gpu, 0, sizeof(DebugFeedbackStruct) * 1);
        m_commandList->copyBuffer(m_debugLineBufferDisplay, 0, m_debugLineBufferCapture, 0, sizeof(DebugLineStruct) * MAX_DEBUG_LINES);
        m_commandList->copyBuffer(m_debugDeltaPathTree_Cpu, 0, m_debugDeltaPathTree_Gpu, 0, sizeof(DeltaTreeVizPathVertex) * cDeltaTreeVizMaxVertices);
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

    if (m_context.activeSettings().ContinuousDebugFeedback || m_context.activeRuntime().Picking.hasActivePickRequest())
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

    m_context.camera.swapViews();
    m_context.gpuDevice.setVsyncEnabled(m_context.activeSettings().actualEnableVsync());

    postUpdatePathTracing();
}
