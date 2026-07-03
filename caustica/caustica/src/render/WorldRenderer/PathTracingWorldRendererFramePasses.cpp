namespace { constexpr int c_SwapchainCount = 3; }

#include <render/WorldRenderer/PathTracingWorldRenderer.h>
#include <render/WorldRenderer/PathTracingFramePipeline.h>
#include <render/WorldRenderer/PathTracingFrameExtension.h>
#include <render/SceneGpuResources.h>
#include <render/SceneGaussianSplatPasses.h>
#include <render/SceneRayTracingResources.h>

#include <scene/SceneLightAccess.h>
#include <render/Core/SceneGeometryUpdate.h>
#include <render/Core/LightingUpdate.h>
#include <render/Core/PathTracingShaderCompiler.h>
#include <render/Core/ComputePipelineRegistry.h>
#include <render/Passes/Lighting/MaterialGpuCache.h>
#include <render/Passes/Lighting/Distant/EnvMapImportanceSamplingCache.h>
#include <render/Passes/OMM/OpacityMicromapBuilder.h>
#include <render/Passes/Gaussian/GaussianSplatPass.h>
#include <render/Passes/Debug/ShaderDebug.h>
#include <render/Core/FramebufferFactory.h>
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

void caustica::render::PathTracingWorldRenderer::ensureFramePipelineBuilt()
{
    if (m_framePipeline)
        return;

    m_framePipeline = std::make_unique<PathTracingFramePipeline>();

    m_framePipeline->registerLambdaPass("FrameSetup", [this](PathTracingFrameContext& ctx) {
        framePassSetup(ctx);
    });
    m_framePipeline->registerLambdaPass("RenderTargets", [this](PathTracingFrameContext& ctx) {
        framePassEnsureRenderTargets(ctx);
    });
    m_framePipeline->registerLambdaPass("RendererInit", [this](PathTracingFrameContext& ctx) {
        framePassRendererInit(ctx);
    });
    m_framePipeline->registerLambdaPass("ShaderUpdate", [this](PathTracingFrameContext& ctx) {
        framePassShaderUpdate(ctx);
    });
    m_framePipeline->registerLambdaPass("BeginCommandList", [this](PathTracingFrameContext& ctx) {
        framePassBeginCommandList(ctx);
    });
    m_framePipeline->registerLambdaPass("SceneUpdate", [this](PathTracingFrameContext& ctx) {
        framePassSceneUpdate(ctx);
    });
    m_framePipeline->registerLambdaPass("PathTracePrepare", [this](PathTracingFrameContext& ctx) {
        framePassPathTracePrepare(ctx);
    });
    m_framePipeline->registerLambdaPass("PathTrace", [this](PathTracingFrameContext& ctx) {
        framePassPathTrace(ctx);
    });
    m_framePipeline->registerLambdaPass("DenoiseAndAA", [this](PathTracingFrameContext& ctx) {
        framePassDenoiseAndAA(ctx);
    });
    m_framePipeline->registerLambdaPass("ToneMapping", [this](PathTracingFrameContext& ctx) {
        framePassToneMapping(ctx);
    });
    m_framePipeline->registerLambdaPass("Composite", [this](PathTracingFrameContext& ctx) {
        framePassComposite(ctx);
    });
    m_framePipeline->registerLambdaPass("Finalize", [this](PathTracingFrameContext& ctx) {
        framePassFinalize(ctx);
    });
}

void caustica::render::PathTracingWorldRenderer::framePassSetup(PathTracingFrameContext& ctx)
{
    ctx.displaySize = m_displaySize;
    ctx.renderSize = m_renderSize;

    preRender();

    const bool realtimeModeChanged = (m_lastRealtimeMode != m_context.settings.RealtimeMode);
    if (realtimeModeChanged)
    {
        m_context.settings.ResetAccumulation = true;
        if (m_context.settings.RealtimeMode)
            m_context.settings.ResetRealtimeCaches = true;
        m_lastRealtimeMode = m_context.settings.RealtimeMode;
    }

#if CAUSTICA_WITH_STREAMLINE
    streamlinePreRender();
#endif
#if CAUSTICA_WITH_NATIVE_DLSS
    nativeDLSSPreRender();
#endif

    m_displayAspectRatio = m_displaySize.x / float(m_displaySize.y);
    ctx.displayAspectRatio = m_displayAspectRatio;

    m_context.renderCore.camera().ensureViews(m_renderSize);
}

void caustica::render::PathTracingWorldRenderer::framePassEnsureRenderTargets(PathTracingFrameContext& ctx)
{
    if (m_renderTargets == nullptr || m_renderTargets->IsUpdateRequired(m_renderSize, m_displaySize))
    {
        device()->waitForIdle();
        device()->runGarbageCollection();
        for (int i = 0; i < std::size(m_nrd); i++)
            m_nrd[i] = nullptr;
        m_renderTargets = nullptr;
        m_oidnDenoisedOutput = nullptr;
        resetReferenceOIDN();
        m_context.bindingCache.Clear();
        m_renderTargets = std::make_unique<RenderTargets>();
        m_renderTargets->Init(device(), m_renderSize, m_displaySize, true, true, c_SwapchainCount);

        ctx.needNewPasses = true;
        {
            PathTracingFrameEvent event{ .framePhase = PathTracingFramePhase::RenderTargetsRecreated };
            dispatchFrameExtensions(event);
        }
    }
}

void caustica::render::PathTracingWorldRenderer::framePassRendererInit(PathTracingFrameContext& ctx)
{
    caustica::syncEnvMapSceneParams(m_context.settings, m_context.envMapSceneParams, c_envMapRadianceScale);

    if (m_context.rayTracing.consumeShaderReloadRequest())
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
        info("Renderer init: begin");
        m_context.diagnostics.progressInitializingRenderer.Start("Initializing renderer...");

        if (m_context.materials == nullptr)
        {
            info("Renderer init: create material cache begin");
            m_context.materials = std::make_shared<MaterialGpuCache>(
                std::string("PathTracerMaterialSpecializations.hlsl"), device(), m_context.textureCache, m_context.shaderFactory);
            info("Renderer init: create material cache end");
            assert(m_pathTracingShaderCompiler == nullptr);

            info("Renderer init: create path tracing shader compiler begin");
            m_pathTracingShaderCompiler = std::make_shared<PathTracingShaderCompiler>(
                device(), m_context.materials, m_bindingLayout, m_bindlessLayout);
            info("Renderer init: create path tracing shader compiler end");

            std::vector<std::filesystem::path> additionalShaderPaths;
            info("Renderer init: create compute pipeline registry begin");
            m_context.computePipelines = std::make_shared<ComputePipelineRegistry>(device(), additionalShaderPaths);
            info("Renderer init: create compute pipeline registry end");

            info("Renderer init: create RT pipeline variants begin");
            m_context.rayTracing.createRTPipelines();
            info("Renderer init: create RT pipeline variants end");
        }

        info("Renderer init: material render passes/load begin");
        m_context.materials->CreateRenderPassesAndLoadMaterials(
            m_bindlessLayout, m_context.commonPasses, m_context.sceneManager.getScene(),
            m_context.sceneManager.getCurrentScenePath(), GetLocalPath(c_AssetsFolder));
        info("Renderer init: material render passes/load end");
        m_context.diagnostics.progressInitializingRenderer.Set(5);
        {
            info("Renderer init: idle maintenance extensions begin");
            PathTracingFrameEvent event{ .framePhase = PathTracingFramePhase::IdleMaintenance };
            dispatchFrameExtensions(event);
            info("Renderer init: idle maintenance extensions end");
        }
        info("Renderer init: OMM render passes begin");
        if (m_context.opacityMaps)
            m_context.opacityMaps->CreateRenderPasses(m_bindlessLayout, m_context.commonPasses);
        info("Renderer init: OMM render passes end");
        m_context.diagnostics.progressInitializingRenderer.Set(20);
        info("Renderer init: end");
    }

    m_context.rayTracing.recreateAccelStructs(m_commandList);
    m_commandList = device()->createCommandList();

    if (m_context.settings.ActualUseRTXDIPasses() && m_rtxdiPass == nullptr)
        ctx.needNewPasses = true;
    if (!m_context.settings.ActualUseRTXDIPasses())
        m_rtxdiPass = nullptr;

    if (ctx.needNewPasses)
    {
        m_context.diagnostics.progressInitializingRenderer.Set(40);
        info("Renderer init: pre-createRenderPasses wait begin");
        const bool preCreatePassesWaitOk = device()->waitForIdle();
        info("Renderer init: pre-createRenderPasses wait end, ok=%s", preCreatePassesWaitOk ? "true" : "false");
        if (!preCreatePassesWaitOk)
        {
            ctx.aborted = true;
            return;
        }
        info("Renderer init: createRenderPasses begin");
        m_commandList->open();
        createRenderPasses(ctx.exposureResetRequired, m_commandList);
        m_commandList->close();
        device()->executeCommandList(m_commandList);
        info("Renderer init: createRenderPasses wait begin");
        const bool createPassesWaitOk = device()->waitForIdle();
        info("Renderer init: createRenderPasses wait end, ok=%s", createPassesWaitOk ? "true" : "false");
        if (!createPassesWaitOk)
        {
            ctx.aborted = true;
            return;
        }
        info("Renderer init: createRenderPasses end");
        m_context.diagnostics.progressInitializingRenderer.Set(70);
    }
}

void caustica::render::PathTracingWorldRenderer::framePassShaderUpdate(PathTracingFrameContext& ctx)
{
    m_pathTracingShaderCompiler->Update(
        m_context.sceneManager.getScene(),
        static_cast<unsigned int>(m_context.renderCore.accelStructs().getSubInstanceData().size()),
        [this](std::vector<caustica::ShaderMacro>& macros) { m_context.rayTracing.fillPTPipelineGlobalMacros(macros); },
        ctx.needNewPasses);

    if (m_context.computePipelines)
        m_context.computePipelines->Update(ctx.needNewPasses);

    m_context.diagnostics.progressInitializingRenderer.Set(90);
}

void caustica::render::PathTracingWorldRenderer::framePassBeginCommandList(PathTracingFrameContext& ctx)
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

void caustica::render::PathTracingWorldRenderer::framePassSceneUpdate(PathTracingFrameContext& ctx)
{
    nvrhi::IFramebuffer* framebuffer = ctx.framebuffer;

    syncCameraViews();
    {
        nvrhi::Viewport viewport = m_context.renderCore.camera().view()->GetViewport();
        float2 jitter = m_context.renderCore.camera().view()->GetPixelOffset();
        float4x4 projMatrix = m_context.renderCore.camera().view()->GetProjectionMatrix();
        float2 viewSize = { viewport.maxX - viewport.minX, viewport.maxY - viewport.minY };
        float outputAspectRatio = m_displayAspectRatio;
        bool rowMajor = true;
        float tanHalfFOVY = 1.0f / ((rowMajor) ? (projMatrix.m_data[1 * 4 + 1]) : (projMatrix.m_data[1 + 1 * 4]));
        float fovY = atanf(tanHalfFOVY) * 2.0f;
        ctx.cameraData = BridgeCamera(
            uint(viewSize.x), uint(viewSize.y), outputAspectRatio,
            m_context.renderCore.camera().camera().GetPosition(),
            m_context.renderCore.camera().camera().GetDir(),
            m_context.renderCore.camera().camera().GetUp(),
            fovY, m_context.renderCore.camera().zNear(), 1e7f,
            m_context.settings.CameraFocalDistance, m_context.settings.CameraAperture, jitter);
    }

    if ((ctx.needNewPasses || ctx.needNewBindings || m_bindingSet == nullptr) && m_shaderDebug)
        m_shaderDebug->CreateRenderPasses(framebuffer, m_renderTargets->Depth);

    if (m_context.settings.EnableShaderDebug && m_shaderDebug)
    {
        dm::float4x4 viewProj = m_context.renderCore.camera().view()->GetViewProjectionMatrix();
        m_shaderDebug->BeginFrame(m_commandList, viewProj);
    }

    UpdateSceneGeometryParams geoParams{
        m_context.settings,
        m_context.rayTracing.accelerationStructRebuildRequested(),
        m_context.sceneManager.getScene(),
        m_commandList,
    };
    geoParams.materials = m_context.materials.get();
    geoParams.opacityMaps = m_context.opacityMaps.get();
    geoParams.frameIndex = m_context.gpuDevice.GetFrameIndex();
    geoParams.asyncLoadingInProgress = &m_context.diagnostics.asyncLoadingInProgress;
    m_context.renderCore.updateSceneGeometry(geoParams);
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
                nvrhi::BindingSetItem::Texture_SRV(0, m_renderTargets->Depth)
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

void caustica::render::PathTracingWorldRenderer::framePassPathTracePrepare(PathTracingFrameContext& ctx)
{
    m_toneMappingPass->PreRender(m_context.settings.ToneMappingParams);
    preUpdatePathTracing(ctx.needNewPasses, m_commandList);

    m_renderTargets->Clear(m_commandList);
    abortIfSubmitFailed(ctx, "clearRenderTargets");
}

void caustica::render::PathTracingWorldRenderer::framePassPathTrace(PathTracingFrameContext& ctx)
{
    SampleConstants& constants = m_currentConstants;
    memset(&constants, 0, sizeof(constants));

    if (m_context.sceneManager.getScene() == nullptr)
    {
        m_commandList->clearTextureFloat(m_renderTargets->OutputColor, nvrhi::AllSubresources, nvrhi::Color(1, 1, 0, 0));
        m_commandList->writeBuffer(m_constantBuffer, &constants, sizeof(constants));
        return;
    }

    updatePathTracerConstants(constants.ptConsts, ctx.cameraData);
    constants.MaterialCount = m_context.materials->GetMaterialDataCount();
    const uint32_t gaussianSplatShadowMode = ResolveGaussianSplatShadowMode(m_context.settings);
    const GaussianSplatBinding primaryGaussianBinding = m_context.gaussianSplats.getPrimaryBinding();
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

    constants.envMapSceneParams = m_context.envMapSceneParams;
    constants.envMapImportanceSamplingParams = m_context.environment->GetImportanceSampling()->GetShaderParams();

    PlanarViewConstants view;
    m_context.renderCore.camera().view()->FillPlanarViewConstants(view);
    PlanarViewConstants previousView;
    m_context.renderCore.camera().viewPrevious()->FillPlanarViewConstants(previousView);
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
    PathTracingFrameEvent::PathTraceDebug pathTraceDebug{};
    pathTraceDebug.pickActive = constants.debug.pick;
    {
        PathTracingFrameEvent event{
            .framePhase = PathTracingFramePhase::BeforePathTrace,
            .pathTraceDebug = &pathTraceDebug,
        };
        dispatchFrameExtensions(event);
    }
    constants.debug.exploreDeltaTree = (pathTraceDebug.exploreDeltaTree && constants.debug.pick) ? 1 : 0;
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
    m_context.rayTracing.uploadSubInstanceData(m_commandList);
    abortIfSubmitFailed(ctx, "updateLighting");
    if (ctx.aborted)
        return;

    m_commandList->writeBuffer(m_constantBuffer, &constants, sizeof(constants));

    m_context.rayTracing.sampleRenderCode(ctx.framebuffer, m_commandList, constants);
    abortIfSubmitFailed(ctx, "sampleRenderCode");
    if (ctx.aborted)
        return;

    m_commandList->writeBuffer(m_constantBuffer, &constants, sizeof(constants));
}

void caustica::render::PathTracingWorldRenderer::framePassDenoiseAndAA(PathTracingFrameContext& ctx)
{
    if (m_context.sceneManager.getScene() == nullptr)
        return;

    SampleConstants& constants = m_currentConstants;

    const bool stochasticSplats = m_context.settings.EnableGaussianSplats && m_context.settings.GaussianSplatSortingMode == 1;
    const bool stochasticUsesMainTemporal = stochasticSplats && (!m_context.settings.RealtimeMode || m_context.settings.RealtimeAA == 1);
    if (stochasticUsesMainTemporal)
        renderGaussianSplats(true);

    postProcessAA(ctx.framebuffer, ctx.needNewPasses || m_context.settings.ResetRealtimeCaches);
    applyReferenceOIDN();
    if (m_context.settings.ReferenceOIDNDenoiser)
        m_commandList->writeBuffer(m_constantBuffer, &constants, sizeof(constants));

    if (!stochasticUsesMainTemporal)
        renderGaussianSplats(false);
    abortIfSubmitFailed(ctx, "postProcessAA");
    if (ctx.aborted)
        return;

    m_commandList->writeBuffer(m_constantBuffer, &constants, sizeof(constants));
}

void caustica::render::PathTracingWorldRenderer::framePassToneMapping(PathTracingFrameContext& ctx)
{
    SampleConstants& constants = m_currentConstants;

    caustica::PlanarView fullscreenView = *m_context.renderCore.camera().view();
    nvrhi::Viewport windowViewport(float(m_displaySize.x), float(m_displaySize.y));
    fullscreenView.SetViewport(windowViewport);
    fullscreenView.UpdateCache();

    postProcessPreToneMapping(m_commandList, fullscreenView);

    if (m_toneMappingPass->Render(m_commandList, fullscreenView, m_renderTargets->ProcessedOutputColor, m_context.settings.EnableToneMapping))
        m_commandList->writeBuffer(m_constantBuffer, &constants, sizeof(constants));

    postProcessPostToneMapping(m_commandList, fullscreenView);
    abortIfSubmitFailed(ctx, "postToneMapping");
    if (ctx.aborted)
        return;

    m_commandList->writeBuffer(m_constantBuffer, &constants, sizeof(constants));
}

void caustica::render::PathTracingWorldRenderer::framePassComposite(PathTracingFrameContext& ctx)
{
    SampleConstants& constants = m_currentConstants;
    nvrhi::IFramebuffer* framebuffer = ctx.framebuffer;
    const auto& fbinfo = framebuffer->getFramebufferInfo();

    caustica::PlanarView fullscreenView = *m_context.renderCore.camera().view();
    nvrhi::Viewport windowViewport(float(m_displaySize.x), float(m_displaySize.y));
    fullscreenView.SetViewport(windowViewport);
    fullscreenView.UpdateCache();

    if (m_context.settings.EnableShaderDebug && m_shaderDebug)
        m_shaderDebug->EndFrameAndOutput(m_commandList, m_renderTargets->LdrFramebuffer->GetFramebuffer(fullscreenView), m_renderTargets->Depth, fbinfo.getViewport());

    {
        PathTracingFrameEvent event{
            .framePhase = PathTracingFramePhase::BeforeFinalBlit,
            .commandList = m_commandList,
            .ldrColor = m_renderTargets->LdrColor,
        };
        dispatchFrameExtensions(event);
    }

    m_commandList->beginMarker("Blit");
    (m_context.commonPasses)->BlitTexture(m_commandList, framebuffer, m_renderTargets->LdrColor, &m_context.bindingCache);
    m_commandList->endMarker();
    abortIfSubmitFailed(ctx, "finalBlit");
    if (ctx.aborted)
        return;
    m_commandList->writeBuffer(m_constantBuffer, &constants, sizeof(constants));

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

void caustica::render::PathTracingWorldRenderer::framePassFinalize(PathTracingFrameContext& ctx)
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

        {
            PathTracingFrameEvent event{
                .framePhase = PathTracingFramePhase::AfterPickResolved,
                .pickFeedback = &m_feedbackData,
            };
            dispatchFrameExtensions(event);
        }
        m_context.runtimeState.Picking.clearPickRequests();
    }

    {
        std::function<bool(const char* fileName)> saveFramebuffer =
            [this, framebufferTexture](const char* fileName) -> bool {
                return SaveTextureToFile(
                    device(), m_context.commonPasses.get(), framebufferTexture, nvrhi::ResourceStates::Common, fileName);
            };
        PathTracingFrameEvent::PostRenderData postRender{
            .framebufferTexture = framebufferTexture,
            .saveFramebuffer = &saveFramebuffer,
        };
        PathTracingFrameEvent event{
            .framePhase = PathTracingFramePhase::PostRender,
            .postRender = &postRender,
        };
        dispatchFrameExtensions(event);
        if (postRender.experimentalScreenshotRequested)
            denoisedScreenshot(framebufferTexture);
    }

    if (m_temporalAntiAliasingPass != nullptr)
        m_temporalAntiAliasingPass->AdvanceFrame();

    m_context.renderCore.camera().swapViews();
    m_context.gpuDevice.SetVsyncEnabled(m_context.settings.ActualEnableVsync());

    postUpdatePathTracing();
}
