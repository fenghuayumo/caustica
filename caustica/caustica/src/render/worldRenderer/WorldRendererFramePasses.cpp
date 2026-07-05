#include <render/features/RenderFeature.h>
#include <render/features/RenderFeatureContext.h>

namespace { constexpr int c_SwapchainCount = 3; }

#include <render/worldRenderer/WorldRenderer.h>
#include <render/worldRenderer/PathTracingFrameContext.h>
#include <render/ecs/RenderScheduleSetup.h>
#include <render/ecs/RenderWorldResources.h>
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
#include <render/passes/gaussian/GaussianSplatPass.h>
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
    constexpr float kGaussianSplatKernelMinResponse = 0.0113f;

    uint32_t ResolveGaussianSplatShadowMode(const PathTracerSettings& settings)
    {
        if (!settings.GaussianSplatShadows && settings.GaussianSplatShadowsMode == GAUSSIAN_SPLAT_SHADOWS_DISABLED)
            return GAUSSIAN_SPLAT_SHADOWS_DISABLED;

        const int requestedMode = settings.GaussianSplatShadowsMode == GAUSSIAN_SPLAT_SHADOWS_DISABLED
            ? GAUSSIAN_SPLAT_SHADOWS_HARD
            : settings.GaussianSplatShadowsMode;
        return uint32_t(std::clamp(requestedMode, GAUSSIAN_SPLAT_SHADOWS_HARD, GAUSSIAN_SPLAT_SHADOWS_SOFT));
    }

    uint32_t ClampGaussianSplatSoftShadowSamples(int sampleCount)
    {
        return uint32_t(std::clamp(sampleCount, 1, 16));
    }

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

void caustica::render::WorldRenderer::ensureRenderScheduleBuilt()
{
    if (m_renderScheduleBuilt)
        return;

    buildDefaultRenderSchedule(m_renderSchedule, *this);
    m_renderScheduleBuilt = true;
}

void caustica::render::WorldRenderer::extractFrameView(ecs::World& renderWorld)
{
    ExtractedFrameView extracted{};
    extracted.displaySize = m_displaySize;
    extracted.renderSize = m_renderSize;
    extracted.displayAspectRatio = m_displayAspectRatio;

    extracted.postProcessView = *m_context.camera.view();
    ViewportDesc windowViewport(float(m_displaySize.x), float(m_displaySize.y));
    extracted.postProcessView.setViewport(windowViewport);
    extracted.postProcessView.updateCache();

    setRenderWorldResource(renderWorld, std::move(extracted));
}

void caustica::render::WorldRenderer::buildFrameGraphPasses(
    RenderFrameContext& ctx,
    const ExtractedFrameView& extractedView)
{
    assert(ctx.graph != nullptr);

    rg::GraphBuilder& graph = *ctx.graph;
    graph.reset();
    graph.setDevice(device());
    graph.setRenderTargetPool(&m_renderTargetPool);
    graph.setRenderBufferPool(&m_renderBufferPool);

    ctx.commandListWasClosed = false;

    nvrhi::IFramebuffer* framebuffer = ctx.frame.framebuffer;
    const auto& fbinfo = framebuffer->getFramebufferInfo();

    if (m_context.settings.EnableShaderDebug && m_shaderDebug)
    {
        m_shaderDebug->EndFrameAndOutput(
            m_commandList,
            m_renderTargets->ldrFramebuffer->getFramebuffer(extractedView.postProcessView),
            m_renderTargets->depth,
            fbinfo.getViewport());
    }

    const bool aaReset = ctx.frame.needNewPasses || m_context.settings.ResetRealtimeCaches;

    RenderFeatureContext featureCtx{
        .graph = &graph,
        .renderer = this,
        .frame = &ctx.frame,
        .renderTargets = m_renderTargets.get(),
        .settings = &m_context.settings,
        .sampleConstants = &m_currentConstants,
        .targetFramebuffer = framebuffer,
        .extractedView = &extractedView,
        .bindingCache = &m_context.bindingCache,
        .blitPass = &m_context.renderDevice.blit(),
        .hasScene = m_context.sceneManager.getScene() != nullptr,
        .aaReset = aaReset,
        .commandListWasClosed = &ctx.commandListWasClosed,
        .gaussianSplatTemporalSampleIndex = &m_gaussianSplatTemporalSampleIndex,
        .gaussianSplatTemporalReset = &m_gaussianSplatTemporalReset,
    };

    registerDefaultGraphFeatures(featureCtx);
}

void caustica::render::WorldRenderer::executeFrameRenderGraph(RenderFrameContext& ctx)
{
    assert(ctx.graph != nullptr);

    SampleConstants& constants = m_currentConstants;

    ctx.graph->compile();
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

    const bool realtimeModeChanged = (m_lastRealtimeMode != m_context.settings.RealtimeMode);
    if (realtimeModeChanged)
    {
        m_context.settings.ResetAccumulation = true;
        if (m_context.settings.RealtimeMode)
        {
            m_context.settings.ResetRealtimeCaches = true;
            m_context.scenePasses.rayTracing.ensureStablePlanePipelines();
        }
        m_lastRealtimeMode = m_context.settings.RealtimeMode;
    }

    if (m_lastScheduledRealtimeAA >= 0 && m_lastScheduledRealtimeAA != m_context.settings.RealtimeAA)
        m_context.settings.ResetRealtimeCaches = true;
    m_lastScheduledRealtimeAA = m_context.settings.RealtimeAA;

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
    caustica::syncEnvMapSceneParams(m_context.settings, m_context.scenePasses.lighting.envMapSceneParams(), c_envMapRadianceScale);

    if (m_context.scenePasses.rayTracing.consumeShaderReloadRequest())
    {
        m_context.shaderFactory->ClearCache();
        ctx.needNewPasses = true;
    }

    if (m_context.settings.NRDModeChanged)
    {
        ctx.needNewPasses = true;
        for (int i = 0; i < std::size(m_nrd); i++)
            m_nrd[i] = nullptr;
    }
    if (!m_context.settings.ActualUseStandaloneDenoiser())
    {
        for (int i = 0; i < std::size(m_nrd); i++)
            m_nrd[i] = nullptr;
    }

    if (ctx.needNewPasses)
    {
        m_context.diagnostics.progressInitializingRenderer.Start("Initializing renderer...");

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
            m_context.sceneManager.getCurrentScenePath(), GetLocalPath(c_AssetsFolder));
        m_context.diagnostics.progressInitializingRenderer.Set(5);
        if (m_context.scenePasses.lighting.opacityMaps())
            m_context.scenePasses.lighting.opacityMaps()->CreateRenderPasses(m_bindlessLayout, m_context.renderDevice);
        m_context.diagnostics.progressInitializingRenderer.Set(20);
    }

    m_context.scenePasses.rayTracing.recreateAccelStructs(m_commandList);
    m_commandList = device()->createCommandList();

    if (m_context.settings.ActualUseRTXDIPasses() && m_rtxdiPass == nullptr)
        ctx.needNewPasses = true;
    if (!m_context.settings.ActualUseRTXDIPasses())
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
            m_context.camera.camera().GetPosition(),
            m_context.camera.camera().GetDir(),
            m_context.camera.camera().GetUp(),
            fovY, m_context.camera.zNear(), 1e7f,
            m_context.settings.CameraFocalDistance, m_context.settings.CameraAperture, jitter);
    }

    if ((ctx.needNewPasses || ctx.needNewBindings || m_bindingSet == nullptr) && m_shaderDebug)
        m_shaderDebug->CreateRenderPasses(framebuffer, m_renderTargets->depth);

    if (m_context.settings.EnableShaderDebug && m_shaderDebug)
    {
        dm::float4x4 viewProj = m_context.camera.view()->getViewProjectionMatrix();
        m_shaderDebug->BeginFrame(m_commandList, viewProj);
    }

    UpdateSceneGeometryParams geoParams{
        m_context.settings,
        m_context.scenePasses.rayTracing.accelerationStructRebuildRequested(),
        m_context.sceneManager.getScene(),
        m_commandList,
    };
    geoParams.materials = m_context.scenePasses.lighting.materials().get();
    geoParams.opacityMaps = m_context.scenePasses.lighting.opacityMaps().get();
    geoParams.frameIndex = m_context.gpuDevice.GetRenderPhaseFrameIndex();
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
            m_rtxdiPass->Reset();
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
        m_context.diagnostics.progressInitializingRenderer.Stop();
    }
}

void caustica::render::WorldRenderer::framePassPathTracePrepare(PathTracingFrameContext& ctx)
{
    if (m_toneMappingPass != nullptr)
        m_toneMappingPass->PreRender(m_context.settings.ToneMappingParams);
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

    if (m_toneMappingPass != nullptr && m_context.settings.EnableToneMapping)
        m_toneMappingPass->PreRender(m_context.settings.ToneMappingParams);

    updatePathTracerConstants(constants.ptConsts, ctx.cameraData);
    constants.MaterialCount = m_context.scenePasses.lighting.materials()->getMaterialDataCount();
    const uint32_t gaussianSplatShadowMode = ResolveGaussianSplatShadowMode(m_context.settings);
    const GaussianSplatBinding primaryGaussianBinding = m_context.scenePasses.gaussianSplats.getPrimaryBinding();
    GaussianSplatPass* primaryGaussianSplatPass = const_cast<GaussianSplatPass*>(primaryGaussianBinding.splatPass);
    constants.GaussianSplatShadowCount = (m_context.settings.EnableGaussianSplats
            && gaussianSplatShadowMode != GAUSSIAN_SPLAT_SHADOWS_DISABLED
            && primaryGaussianSplatPass != nullptr
            && primaryGaussianSplatPass->GetTopLevelAS() != nullptr)
        ? primaryGaussianSplatPass->GetSplatCount()
        : 0;
    constants.GaussianSplatShadowsEnabled = constants.GaussianSplatShadowCount > 0 ? 1u : 0u;
    constants.GaussianSplatShadowScale = m_context.settings.GaussianSplatScale;
    constants.GaussianSplatShadowAlphaThreshold = m_context.settings.GaussianSplatAlphaCullThreshold;
    constants.GaussianSplatShadowUseTLASInstances =
        (primaryGaussianSplatPass != nullptr && primaryGaussianSplatPass->GetShadowUsesTLASInstances()) ? 1u : 0u;
    constants.GaussianSplatShadowPrimitiveCountPerSplat =
        primaryGaussianSplatPass != nullptr ? primaryGaussianSplatPass->GetShadowPrimitiveCountPerSplat() : 1u;
    constants.GaussianSplatShadowMode = constants.GaussianSplatShadowsEnabled != 0
        ? gaussianSplatShadowMode
        : GAUSSIAN_SPLAT_SHADOWS_DISABLED;
    constants.GaussianSplatShadowSoftRadius = m_context.settings.GaussianSplatShadowSoftRadius;
    constants.GaussianSplatShadowSoftSampleCount = ClampGaussianSplatSoftShadowSamples(m_context.settings.GaussianSplatShadowSoftSampleCount);
    constants.GaussianSplatShadowFrameIndex = uint32_t(m_frameIndex & 0xffffffffu);
    constants.GaussianSplatShadowRayOffset = m_context.settings.GaussianSplatRtxParticleShadowOffset;
    constants.GaussianSplatShadowAlphaScale = m_context.settings.GaussianSplatAlphaScale;
    constants.GaussianSplatShadowKernelMinResponse = kGaussianSplatKernelMinResponse;
    constants.GaussianSplatShadowKernelDegree = uint32_t(std::clamp(m_context.settings.GaussianSplatRtxKernelDegree, 0, 5));
    constants.GaussianSplatShadowAdaptiveClamp = m_context.settings.GaussianSplatRtxAdaptiveClamp ? 1u : 0u;
    constants.GaussianSplatShadowWorldToObject = primaryGaussianBinding.splatPass != nullptr
        ? inverse(primaryGaussianBinding.objectToWorld)
        : float4x4::identity();

    constants.envMapSceneParams = m_context.scenePasses.lighting.envMapSceneParams();
    constants.envMapImportanceSamplingParams = m_context.scenePasses.lighting.environment()->GetImportanceSampling()->GetShaderParams();

    PlanarViewConstants view;
    m_context.camera.view()->fillPlanarViewConstants(view);
    PlanarViewConstants previousView;
    m_context.camera.viewPrevious()->fillPlanarViewConstants(previousView);
    constants.view = FromPlanarViewConstants(view);
    constants.previousView = FromPlanarViewConstants(previousView);

    constants.debug = {};
    constants.debug.pick = m_context.runtimeState.Picking.hasActivePickRequest() || m_context.settings.ContinuousDebugFeedback;
    constants.debug.pickX = (constants.debug.pick) ? (m_context.settings.DebugPixel.x) : (-1);
    constants.debug.pickY = (constants.debug.pick) ? (m_context.settings.DebugPixel.y) : (-1);
    constants.debug.debugLineScale = (m_context.settings.ShowDebugLines) ? (m_context.settings.DebugLineScale) : (0.0f);
    constants.debug.showWireframe = m_context.settings.ShowWireframe;
    constants.debug.debugViewType = (int)m_context.settings.DebugView;
    constants.debug.debugViewStablePlaneIndex = (m_context.settings.StablePlanesActiveCount == 1) ? (0) : (m_context.settings.DebugViewStablePlaneIndex);
#if ENABLE_DEBUG_DELTA_TREE_VIZUALISATION
    constants.debug.exploreDeltaTree = (m_context.settings.DebugExploreDeltaTree && constants.debug.pick) ? 1 : 0;
#else
    constants.debug.exploreDeltaTree = false;
#endif
    constants.debug.imageWidth = constants.ptConsts.imageWidth;
    constants.debug.imageHeight = constants.ptConsts.imageHeight;
    constants.debug.mouseX = m_context.settings.MousePos.x;
    constants.debug.mouseY = m_context.settings.MousePos.y;
    constants.debug.cameraPosW = constants.ptConsts.camera.PosW;
    constants.debug._padding0 = 0;

    constants.denoisingHitParamConsts = {
        m_context.settings.ReblurSettings.hitDistanceParameters.A,
        m_context.settings.ReblurSettings.hitDistanceParameters.B,
        m_context.settings.ReblurSettings.hitDistanceParameters.C,
        m_context.settings.ReblurSettings.hitDistanceParameters.D
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
    if (m_context.settings.ReferenceOIDNDenoiser)
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

    if (m_context.settings.ShowDebugLines == true)
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

    if (m_context.settings.ContinuousDebugFeedback || m_context.runtimeState.Picking.MaterialRequested)
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

    if (m_context.settings.ContinuousDebugFeedback || m_context.runtimeState.Picking.hasActivePickRequest())
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
        m_temporalAntiAliasingPass->AdvanceFrame();

    m_context.camera.swapViews();
    m_context.gpuDevice.SetVsyncEnabled(m_context.settings.ActualEnableVsync());

    postUpdatePathTracing();
}
