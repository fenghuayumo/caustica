namespace { constexpr int c_SwapchainCount = 3; }

#include <render/WorldRenderer.h>
#include <render/core/SceneGpuUpdater.h>
#include <engine/GpuSharedCaches.h>
#include <scene/Scene.h>
#include <render/pipeline/RenderPipelineRegistry.h>
#include <render/SceneGpuResources.h>
#include <render/PathTracingContext.h>
#include <render/PathTracerScenePasses.h>
#include <engine/GpuSharedCaches.h>
#include <render/core/RenderDevice.h>
#include <render/core/RenderPassConstants.h>
#include <render/SceneGaussianSplatPasses.h>
#include <render/SceneRayTracingResources.h>

#include <scene/Scene.h>
#include <scene/SceneEcs.h>
#include <scene/SceneLightAccess.h>
#include <scene/scene_utils.h>
#include <render/core/PostProcessAA.h>
#include <render/core/SceneGeometryUpdate.h>
#include <render/core/LightingUpdate.h>
#include <render/core/AccelStructManager.h>
#include <render/core/CameraController.h>
#include <render/core/PathTracingShaderCompiler.h>
#include <render/core/RtPipelineCache.h>
#include <render/core/ComputePipelineRegistry.h>
#include <render/core/BindingCache.h>
#include <scene/View.h>
#include <render/core/FramebufferFactory.h>
#include <render/core/AccelerationStructureUtil.h>
#include <render/passes/lighting/distant/EnvMapImportanceSamplingCache.h>
#include <render/passes/lighting/LightingFrame.h>
#include <render/passes/lighting/MaterialGpuCache.h>
#include <render/passes/omm/OpacityMicromapBuilder.h>
#include <render/passes/postProcess/DenoisingGuidesPass.h>
#include <render/passes/postProcess/AccumulationPass.h>
#include <render/passes/postProcess/PostProcess.h>
#include <render/passes/postProcess/ToneMappingPasses.h>
#include <render/passes/geometry/BloomPass.h>
#include <render/passes/geometry/TemporalAntiAliasingPass.h>
#include <render/passes/denoisers/OidnDenoiser.h>
#include <render/passes/denoisers/DenoisePass.h>
#include <render/passes/gaussian/GaussianSplatGraph.h>
#include <render/passes/gaussian/GaussianSplatSceneRuntime.h>
#include <render/passes/gaussian/GaussianSplatFramePass.h>
#include <render/passes/rtxdi/RtxdiPass.h>
#include <render/passes/pathTrace/PathTracePass.h>
#include <render/passes/debug/ShaderDebug.h>
#include <core/log.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <assets/loader/ShaderFactory.h>
#include <render/gpuSort/GPUSort.h>
#include <backend/GpuDevice.h>
#include <core/path_utils.h>
#include <core/log.h>
#include <core/progress.h>
#include <core/system_utils.h>
#include <assets/loader/TextureLoader.h>
#include <render/core/PathTracerSettings.h>
#include <math/float.h>
#include <math/math.h>
#include <shaders/SampleConstantBuffer.h>
#include <shaders/view_cb.h>
#include <rhi/utils.h>

#include <core/Timer.h>
#include <shaders/light_cb.h>
#include <render/passes/debug/Korgi.h>

#if CAUSTICA_WITH_STREAMLINE
#include <backend/StreamlineInterface.h>
#endif
#if CAUSTICA_WITH_NATIVE_DLSS
#include <render/passes/geometry/DLSS.h>
#endif

extern FPSLimiter g_FPSLimiter;

using namespace caustica;
using namespace caustica::math;
using namespace caustica::render;

caustica::render::WorldRenderer::WorldRenderer() = default;

caustica::render::WorldRenderer::~WorldRenderer()
{
    destroy();
}

bool caustica::render::WorldRenderer::create(const createParams& params)
{
    destroy();

    GpuSharedCaches& infra = params.gpuSharedCaches;
    m_accelStructs = AccelStructManager(params.gpuDevice.getDevice());

    m_pathTracingContext = std::make_unique<PathTracingContext>(PathTracingContext{
        .gpuDevice = params.gpuDevice,
        .camera = m_renderCamera,
        .accelStructs = m_accelStructs,
        .settings = params.settings,
        .runtimeState = params.runtimeState,
        .scenePasses = m_scenePasses,
        .shaderFactory = infra.shaderFactory,
        .renderDevice = *infra.renderDevice,
        .bindingCache = *infra.bindingCache,
        .textureCache = infra.textureLoader,
        .descriptorTable = infra.descriptorTable,
        .sceneTime = params.sceneTime,
        .diagnostics = params.diagnostics,
    });
    m_context = m_pathTracingContext.get();
    SceneGpuUpdater::initialize(
        m_context->sceneGpuResources,
        params.gpuDevice.getDevice(),
        *m_context->shaderFactory);
    m_accelStructs.bindSceneGpuResources(&m_context->sceneGpuResources);
    m_context->renderDevice.setActiveSceneGpuResources(&m_context->sceneGpuResources);
    m_lastRealtimeMode = m_context->activeSettings().RealtimeMode;

    createBindingLayouts(infra.bindlessLayout);
    m_pathTracePass = std::make_unique<PathTracePass>();
    m_denoisePass = std::make_unique<DenoisePass>();
    m_gaussianFramePass = std::make_unique<GaussianSplatFramePass>();

    ScenePassWireParams sceneWireParams{
        .gpuDevice = params.gpuDevice,
        .accelStructs = m_accelStructs,
        .worldRenderer = *this,
        .settings = params.settings,
        .invalidation = params.runtimeState.Invalidation,
        .gaussianSplatsSummary = params.runtimeState.GaussianSplats,
        .lighting = m_scenePasses.lighting,
        .bindingCache = *infra.bindingCache,
        .shaderFactory = infra.shaderFactory,
        .renderDevice = *infra.renderDevice,
    };
    sceneWireParams.onGaussianSplatTemporalReset = [this]() {
        setGaussianSplatTemporalReset(true);
    };
    sceneWireParams.getRenderTargets = [this]() {
        return getRenderTargets();
    };
    sceneWireParams.getShaderDebug = [this]() {
        return getShaderDebug();
    };
    m_scenePasses.wireSession(sceneWireParams);
    return true;
}

void caustica::render::WorldRenderer::destroy()
{
    // Drop graph lambdas and RTXDI (PrepareLightsPass) before releasing scene
    // lighting / env-map ownership. Those paths must not keep EnvMapProcessor
    // alive past TextureLoader / AssetSystem shutdown.
    m_frameGraph.reset();
    m_rtxdiPass.reset();

    if (m_context)
        m_context->renderDevice.setActiveSceneGpuResources(nullptr);
    m_context = nullptr;
    m_pathTracingContext.reset();
    m_accelStructs = AccelStructManager{};
    m_scenePasses = {};
}

caustica::rhi::BindingLayoutHandle caustica::render::WorldRenderer::createBindlessLayout(caustica::rhi::IDevice* device)
{
    caustica::rhi::BindlessLayoutDesc bindlessLayoutDesc;
    bindlessLayoutDesc.visibility = caustica::rhi::ShaderType::All;
    bindlessLayoutDesc.firstSlot = 0;
    bindlessLayoutDesc.maxCapacity = 1024;
    bindlessLayoutDesc.registerSpaces = {
        caustica::rhi::BindingLayoutItem::RawBuffer_SRV(1),
        caustica::rhi::BindingLayoutItem::Texture_SRV(2)
    };
    return device->createBindlessLayout(bindlessLayoutDesc);
}

void caustica::render::WorldRenderer::createBindingLayouts(caustica::rhi::IBindingLayout* precreatedBindless)
{
    caustica::rhi::IDevice* const gpuDevice = device();
    m_bindlessLayout = precreatedBindless
        ? caustica::rhi::BindingLayoutHandle(precreatedBindless)
        : createBindlessLayout(gpuDevice);

    caustica::rhi::BindingLayoutDesc globalBindingLayoutDesc;
    globalBindingLayoutDesc.visibility = caustica::rhi::ShaderType::All;
    globalBindingLayoutDesc.bindings = {
        caustica::rhi::BindingLayoutItem::VolatileConstantBuffer(0),
        caustica::rhi::BindingLayoutItem::PushConstants(1, sizeof(SampleMiniConstants)),
        caustica::rhi::BindingLayoutItem::RayTracingAccelStruct(0),
        caustica::rhi::BindingLayoutItem::StructuredBuffer_SRV(1),
        caustica::rhi::BindingLayoutItem::StructuredBuffer_SRV(2),
        caustica::rhi::BindingLayoutItem::StructuredBuffer_SRV(3),
        caustica::rhi::BindingLayoutItem::StructuredBuffer_SRV(4),
        caustica::rhi::BindingLayoutItem::StructuredBuffer_SRV(5),
        caustica::rhi::BindingLayoutItem::Texture_SRV(6),
        caustica::rhi::BindingLayoutItem::RayTracingAccelStruct(7),
        caustica::rhi::BindingLayoutItem::StructuredBuffer_SRV(8),
        caustica::rhi::BindingLayoutItem::Texture_SRV(10),
        caustica::rhi::BindingLayoutItem::Texture_SRV(11),
        caustica::rhi::BindingLayoutItem::StructuredBuffer_SRV(12),
        caustica::rhi::BindingLayoutItem::StructuredBuffer_SRV(13),
        caustica::rhi::BindingLayoutItem::StructuredBuffer_SRV(14),
        caustica::rhi::BindingLayoutItem::TypedBuffer_SRV(15),
        caustica::rhi::BindingLayoutItem::TypedBuffer_SRV(16),
        caustica::rhi::BindingLayoutItem::TypedBuffer_SRV(17),
        caustica::rhi::BindingLayoutItem::Texture_SRV(18),
        caustica::rhi::BindingLayoutItem::Texture_UAV(20),
        caustica::rhi::BindingLayoutItem::Texture_UAV(21),
        caustica::rhi::BindingLayoutItem::Sampler(0),
        caustica::rhi::BindingLayoutItem::Sampler(1),
        caustica::rhi::BindingLayoutItem::Sampler(2),
        caustica::rhi::BindingLayoutItem::Texture_UAV(0),
        caustica::rhi::BindingLayoutItem::Texture_UAV(1),
        caustica::rhi::BindingLayoutItem::Texture_UAV(2),
        caustica::rhi::BindingLayoutItem::Texture_UAV(4),
        caustica::rhi::BindingLayoutItem::Texture_UAV(5),
        caustica::rhi::BindingLayoutItem::Texture_UAV(6),
        caustica::rhi::BindingLayoutItem::Texture_UAV(7),
        caustica::rhi::BindingLayoutItem::Texture_UAV(8),
        caustica::rhi::BindingLayoutItem::Texture_UAV(31),
        caustica::rhi::BindingLayoutItem::Texture_UAV(32),
        caustica::rhi::BindingLayoutItem::Texture_UAV(33),
        caustica::rhi::BindingLayoutItem::Texture_UAV(34),
        caustica::rhi::BindingLayoutItem::Texture_UAV(35),
        caustica::rhi::BindingLayoutItem::Texture_UAV(36),
        caustica::rhi::BindingLayoutItem::Texture_UAV(37),
        caustica::rhi::BindingLayoutItem::StructuredBuffer_UAV(51),
        caustica::rhi::BindingLayoutItem::StructuredBuffer_UAV(52),
        caustica::rhi::BindingLayoutItem::StructuredBuffer_UAV(53),
        caustica::rhi::BindingLayoutItem::StructuredBuffer_UAV(54),
        caustica::rhi::BindingLayoutItem::Texture_UAV(60),
        caustica::rhi::BindingLayoutItem::Texture_UAV(61),
        caustica::rhi::BindingLayoutItem::Texture_UAV(70),
        caustica::rhi::BindingLayoutItem::Texture_UAV(71),
        caustica::rhi::BindingLayoutItem::Texture_UAV(72),
        caustica::rhi::BindingLayoutItem::Texture_UAV(73),
        caustica::rhi::BindingLayoutItem::Texture_UAV(74),
        caustica::rhi::BindingLayoutItem::Texture_UAV(75),
        caustica::rhi::BindingLayoutItem::RawBuffer_UAV(SHADER_DEBUG_BUFFER_UAV_INDEX),
        caustica::rhi::BindingLayoutItem::Texture_UAV(SHADER_DEBUG_VIZ_TEXTURE_UAV_INDEX)
    };

    if (gpuDevice->queryFeatureSupport(caustica::rhi::Feature::HlslExtensionUAV))
    {
        globalBindingLayoutDesc.bindings.push_back(
            caustica::rhi::BindingLayoutItem::TypedBuffer_UAV(NV_SHADER_EXTN_SLOT_NUM));
    }

    globalBindingLayoutDesc.bindings.push_back(caustica::rhi::BindingLayoutItem::Texture_UAV(40));
    globalBindingLayoutDesc.bindings.push_back(caustica::rhi::BindingLayoutItem::StructuredBuffer_UAV(42));
    globalBindingLayoutDesc.bindings.push_back(caustica::rhi::BindingLayoutItem::Texture_UAV(44));
    globalBindingLayoutDesc.bindings.push_back(caustica::rhi::BindingLayoutItem::StructuredBuffer_UAV(45));

    globalBindingLayoutDesc.bindings.push_back(caustica::rhi::BindingLayoutItem::Texture_UAV(100));
    globalBindingLayoutDesc.bindings.push_back(caustica::rhi::BindingLayoutItem::Texture_UAV(101));
    globalBindingLayoutDesc.bindings.push_back(caustica::rhi::BindingLayoutItem::Texture_UAV(102));
    globalBindingLayoutDesc.bindings.push_back(caustica::rhi::BindingLayoutItem::Texture_UAV(103));
    globalBindingLayoutDesc.bindings.push_back(caustica::rhi::BindingLayoutItem::Texture_UAV(10));
    globalBindingLayoutDesc.bindings.push_back(caustica::rhi::BindingLayoutItem::Texture_SRV(80));
    globalBindingLayoutDesc.bindings.push_back(caustica::rhi::BindingLayoutItem::Texture_SRV(81));
    globalBindingLayoutDesc.bindings.push_back(caustica::rhi::BindingLayoutItem::Texture_SRV(82));
    globalBindingLayoutDesc.bindings.push_back(caustica::rhi::BindingLayoutItem::Texture_SRV(83));
    globalBindingLayoutDesc.bindings.push_back(caustica::rhi::BindingLayoutItem::Texture_SRV(84));
    globalBindingLayoutDesc.bindings.push_back(caustica::rhi::BindingLayoutItem::VolatileConstantBuffer(10));
    globalBindingLayoutDesc.bindings.push_back(caustica::rhi::BindingLayoutItem::Texture_UAV(85));
    globalBindingLayoutDesc.bindings.push_back(caustica::rhi::BindingLayoutItem::Texture_SRV(86));
    globalBindingLayoutDesc.bindings.push_back(caustica::rhi::BindingLayoutItem::Texture_SRV(87));

    m_bindingLayout = gpuDevice->createBindingLayout(globalBindingLayoutDesc);
}

void caustica::render::WorldRenderer::createDeviceResources()
{
    caustica::rhi::IDevice* device = this->device();

    m_renderTargetPool.setDevice(device);
    m_renderBufferPool.setDevice(device);
    m_frameGraph.setRenderTargetPool(&m_renderTargetPool);
    m_frameGraph.setRenderBufferPool(&m_renderBufferPool);

#if CAUSTICA_WITH_NATIVE_DLSS
    m_nativeDLSS = caustica::render::DLSS::create(device, m_context->shaderFactory, caustica::getDirectoryWithExecutable().string());
    if (m_nativeDLSS)
    {
        m_context->activeSettings().IsDLSSSuported = m_nativeDLSS->isDlssSupported();
        m_context->activeSettings().IsDLSSRRSupported = m_nativeDLSS->isRayReconstructionSupported();
        caustica::info("Native NGX DLSS support: DLSS=%s, DLSS-RR=%s.",
            m_context->activeSettings().IsDLSSSuported ? "yes" : "no",
            m_context->activeSettings().IsDLSSRRSupported ? "yes" : "no");
    }
    else
    {
        caustica::warning("Native NGX DLSS object was not created.");
    }
#endif

    memset(&m_feedbackData, 0, sizeof(DebugFeedbackStruct) * 1);
    memset(&m_debugDeltaPathTree, 0, sizeof(DeltaTreeVizPathVertex) * cDeltaTreeVizMaxVertices);

    {
        std::vector<ShaderMacro> drawLinesMacro = { ShaderMacro("DRAW_LINES_SHADERS_OLD", "1") };
        m_linesVertexShader = m_context->shaderFactory->createShader("caustica/shaders/render/misc/DebugLines.hlsl", "main_vs", &drawLinesMacro, caustica::rhi::ShaderType::Vertex);
        m_linesPixelShader = m_context->shaderFactory->createShader("caustica/shaders/render/misc/DebugLines.hlsl", "main_ps", &drawLinesMacro, caustica::rhi::ShaderType::Pixel);

        caustica::rhi::VertexAttributeDesc attributes[] = {
            caustica::rhi::VertexAttributeDesc()
                .setName("POSITION")
                .setFormat(caustica::rhi::Format::RGBA32_FLOAT)
                .setOffset(0)
                .setElementStride(sizeof(DebugLineStruct)),
            caustica::rhi::VertexAttributeDesc()
                .setName("COLOR")
                .setFormat(caustica::rhi::Format::RGBA32_FLOAT)
                .setOffset(offsetof(DebugLineStruct, col))
                .setElementStride(sizeof(DebugLineStruct)),
        };
        m_linesInputLayout = device->createInputLayout(attributes, uint32_t(std::size(attributes)), m_linesVertexShader);

        caustica::rhi::BindingLayoutDesc linesBindingLayoutDesc;
        linesBindingLayoutDesc.visibility = caustica::rhi::ShaderType::All;
        linesBindingLayoutDesc.bindings = {
            caustica::rhi::BindingLayoutItem::VolatileConstantBuffer(0),
            caustica::rhi::BindingLayoutItem::Texture_SRV(0)
        };
        m_linesBindingLayout = device->createBindingLayout(linesBindingLayoutDesc);

        caustica::rhi::BufferDesc bufferDesc;
        bufferDesc.byteSize = sizeof(DebugFeedbackStruct) * 1;
        bufferDesc.isConstantBuffer = false;
        bufferDesc.isVolatile = false;
        bufferDesc.canHaveUAVs = true;
        bufferDesc.cpuAccess = caustica::rhi::CpuAccessMode::None;
        bufferDesc.maxVersions = caustica::c_MaxRenderPassConstantBufferVersions;
        bufferDesc.structStride = sizeof(DebugFeedbackStruct);
        bufferDesc.keepInitialState = true;
        bufferDesc.initialState = caustica::rhi::ResourceStates::Common;
        bufferDesc.debugName = "Feedback_Buffer_Gpu";
        m_feedback_Buffer_Gpu = device->createBuffer(bufferDesc);

        bufferDesc.canHaveUAVs = false;
        bufferDesc.cpuAccess = caustica::rhi::CpuAccessMode::Read;
        bufferDesc.structStride = 0;
        bufferDesc.keepInitialState = false;
        bufferDesc.initialState = caustica::rhi::ResourceStates::Unknown;
        bufferDesc.debugName = "Feedback_Buffer_Cpu";
        m_feedback_Buffer_Cpu = device->createBuffer(bufferDesc);

        bufferDesc.byteSize = sizeof(DebugLineStruct) * MAX_DEBUG_LINES;
        bufferDesc.isVertexBuffer = true;
        bufferDesc.isConstantBuffer = false;
        bufferDesc.isVolatile = false;
        bufferDesc.canHaveUAVs = true;
        bufferDesc.cpuAccess = caustica::rhi::CpuAccessMode::None;
        bufferDesc.structStride = sizeof(DebugLineStruct);
        bufferDesc.keepInitialState = true;
        bufferDesc.initialState = caustica::rhi::ResourceStates::Common;
        bufferDesc.debugName = "DebugLinesCapture";
        m_debugLineBufferCapture = device->createBuffer(bufferDesc);
        bufferDesc.debugName = "DebugLinesDisplay";
        m_debugLineBufferDisplay = device->createBuffer(bufferDesc);

        bufferDesc.byteSize = sizeof(DeltaTreeVizPathVertex) * cDeltaTreeVizMaxVertices;
        bufferDesc.isConstantBuffer = false;
        bufferDesc.isVolatile = false;
        bufferDesc.canHaveUAVs = true;
        bufferDesc.cpuAccess = caustica::rhi::CpuAccessMode::None;
        bufferDesc.maxVersions = caustica::c_MaxRenderPassConstantBufferVersions;
        bufferDesc.structStride = sizeof(DeltaTreeVizPathVertex);
        bufferDesc.keepInitialState = true;
        bufferDesc.initialState = caustica::rhi::ResourceStates::Common;
        bufferDesc.debugName = "Feedback_PathDecomp_Gpu";
        m_debugDeltaPathTree_Gpu = device->createBuffer(bufferDesc);

        bufferDesc.canHaveUAVs = false;
        bufferDesc.cpuAccess = caustica::rhi::CpuAccessMode::Read;
        bufferDesc.structStride = 0;
        bufferDesc.keepInitialState = false;
        bufferDesc.initialState = caustica::rhi::ResourceStates::Unknown;
        bufferDesc.debugName = "Feedback_PathDecomp_Cpu";
        m_debugDeltaPathTree_Cpu = device->createBuffer(bufferDesc);

        bufferDesc.byteSize = sizeof(PathPayload) * cDeltaTreeVizMaxStackSize;
        bufferDesc.isConstantBuffer = false;
        bufferDesc.isVolatile = false;
        bufferDesc.canHaveUAVs = true;
        bufferDesc.cpuAccess = caustica::rhi::CpuAccessMode::None;
        bufferDesc.maxVersions = caustica::c_MaxRenderPassConstantBufferVersions;
        bufferDesc.structStride = sizeof(PathPayload);
        bufferDesc.keepInitialState = true;
        bufferDesc.initialState = caustica::rhi::ResourceStates::Common;
        bufferDesc.debugName = "DebugDeltaPathTreeSearchStack";
        m_debugDeltaPathTreeSearchStack = device->createBuffer(bufferDesc);
    }

    m_constantBuffer = device->createBuffer(caustica::rhi::utils::CreateVolatileConstantBufferDesc(
        sizeof(SampleConstants), "SampleConstants", caustica::c_MaxRenderPassConstantBufferVersions * 2));

    m_commandList = device->createCommandList();
}


void caustica::render::WorldRenderer::onSceneUnloading()
{
    m_scenePasses.gaussianSplats.clearSession();
    m_context->sceneGpuResources.clearSceneResources();
    m_context->sessionScene = nullptr;
    m_context->sessionScenePath.clear();
    m_context->frameScene = nullptr;
    m_context->frameGpu = {};

#if CAUSTICA_WITH_STREAMLINE
    if (!m_context->gpuDevice.isHeadless())
    {
        auto& streamline = m_context->gpuDevice.getStreamline();
        if (streamline.isDLSSRRAvailable())
            streamline.cleanupDLSSRR(false);
        if (streamline.isDLSSAvailable())
            streamline.cleanupDLSS(false);
        if (streamline.isDLSSGAvailable())
            streamline.cleanupDLSSG(false);
    }

    m_recommendedDLSSSettings = {};
    m_lastDLSSRROptions = {};
    m_context->activeSettings().DLSSFGOptions = {};
    m_context->activeSettings().DLSSFGMultiplier = 1;
    m_context->activeSettings().DLSSFGMaxNumFramesToGenerate = 1;
#endif
    m_context->activeSettings().DLSSMode = PathTracerSettings::DLSSModeDefault;
    m_context->activeSettings().DLSSLastMode = SI::DLSSMode::eOff;
    m_context->activeSettings().DLSSLastDisplaySize = { 0, 0 };
    m_context->activeSettings().DLSSLastRealtimeAA = 0;
    m_lastScheduledRealtimeAA = -1;

    m_bindingSet = nullptr;
    m_context->bindingCache.clear();
    if (m_commandList)
        m_commandList = device()->createCommandList();
    m_gaussianSplatTemporalReset = true;
    m_gaussianSplatEmissionProxies.clear();
    if (m_rtxdiPass != nullptr)
        m_rtxdiPass->reset();
    m_rtPipelineCache = nullptr;
    m_pathTracingShaderCompiler = nullptr;
    m_ptPipelineReference = nullptr;
    m_ptPipelineBuildStablePlanes = nullptr;
    m_ptPipelineFillStablePlanes = nullptr;
    m_ptPipelineTestRaygenPPHDR = nullptr;
    m_ptPipelineEdgeDetection = nullptr;
}

void caustica::render::WorldRenderer::resetFrameIndex()
{
    m_frameIndex = 0;
}

uint32_t caustica::render::WorldRenderer::precacheAllRtFeaturePresets(bool showProgress)
{
    if (!m_rtPipelineCache)
        return 0;
    return m_rtPipelineCache->precacheAll(showProgress);
}

void caustica::render::WorldRenderer::onSceneLoaded(
    std::shared_ptr<Scene> scene,
    std::filesystem::path scenePath)
{
    m_context->sceneGpuResources.clearSceneResources();
    m_context->sessionScene = std::move(scene);
    m_context->sessionScenePath = std::move(scenePath);
    m_scenePasses.gaussianSplats.bindSession(
        m_context->sessionScene.get(), m_context->sessionScenePath);

    resetFrameIndex();
    m_accumulationSampleIndex = 0;
    m_sampleIndex = 0;
    m_gaussianSplatTemporalReset = true;
    m_context->activeSettings().ResetAccumulation = true;
    m_context->activeSettings().ResetRealtimeCaches = true;
    m_context->activeSettings().DLSSMode = PathTracerSettings::DLSSModeDefault;
    m_context->activeSettings().DLSSLastMode = SI::DLSSMode::eOff;
    m_context->activeSettings().DLSSLastDisplaySize = { 0, 0 };
    m_context->activeSettings().DLSSLastRealtimeAA = 0;
    // Realtime->Realtime switches do not flip RealtimeMode; nudge so framePassSetup resets temporal state.
    m_lastRealtimeMode = !m_context->activeSettings().RealtimeMode;
    m_lastScheduledRealtimeAA = -1;

    if (m_rtxdiPass)
        m_rtxdiPass->reset();
}

void caustica::render::WorldRenderer::onBackBufferResizing()
{
    device()->waitForIdle();
    device()->runGarbageCollection();
    m_context->bindingCache.clear();
    m_renderTargets = nullptr;
    m_linesPipeline = nullptr; // the pipeline is based on the framebuffer so needs a reset
    if (m_denoisePass)
        m_denoisePass->invalidateNrdIntegrations();
    if (m_rtxdiPass)
        m_rtxdiPass->reset();

// NOTE: we're not yet sure if this is necessary to avoid crash with going in/out of fullscreen and FG
#if CAUSTICA_WITH_STREAMLINE
    if (!m_context->gpuDevice.isHeadless() &&
        (m_context->activeSettings().DLSSFGOptions.mode == StreamlineInterface::DLSSGMode::eOn || m_context->activeSettings().actualDLSSFGMode() == StreamlineInterface::DLSSGMode::eOn)) 
    {
        m_context->gpuDevice.getStreamline().cleanupDLSS(false);
        m_context->gpuDevice.getStreamline().cleanupDLSSG(false);

        if (m_context->gpuDevice.getStreamline().isDLSSGAvailable())
        {
            auto dlssgOptions = StreamlineInterface::DLSSGOptions{};
            StreamlineInterface::DLSSGState state;
            m_context->gpuDevice.getStreamline().getDLSSGState(state, dlssgOptions);
            m_context->activeSettings().DLSSFGMultiplier = state.numFramesActuallyPresented;
            m_context->activeSettings().DLSSFGMaxNumFramesToGenerate = state.numFramesToGenerateMax;

            m_context->gpuDevice.getStreamline().setDLSSGOptions(dlssgOptions);
            m_context->activeSettings().DLSSFGOptions = dlssgOptions;
        }
    }
#endif
}

SimpleViewConstants FromPlanarViewConstants(PlanarViewConstants & view)
{
    SimpleViewConstants ret;
    ret.matWorldToView          = view.matWorldToView;
    ret.matViewToClip           = view.matViewToClip;
    ret.matWorldToClipNoOffset  = view.matWorldToClipNoOffset;
    ret.matClipToWorldNoOffset  = view.matClipToWorldNoOffset;
    ret.matWorldToClip          = view.matWorldToClip;
    ret.clipToWindowBias        = view.clipToWindowBias;
    ret.clipToWindowScale       = view.clipToWindowScale;
    ret.viewportOrigin          = view.viewportOrigin;
    ret.viewportSize            = view.viewportSize;
    ret.viewportSizeInv         = view.viewportSizeInv;
    ret.pixelOffset             = view.pixelOffset;
    return ret;
}

void caustica::render::WorldRenderer::createRenderPasses( bool& exposureResetRequired, caustica::rhi::CommandListHandle initializeCommandList )
{
    (void)exposureResetRequired;
    m_context->bindingCache.clear();

    m_shaderDebug = std::make_shared<ShaderDebug>(device(), initializeCommandList, m_context->shaderFactory, m_context->renderDevice);

    if (m_context->activeSettings().actualUseRTXDIPasses())
        m_rtxdiPass = std::make_unique<RtxdiPass>(device(), m_context->shaderFactory, m_context->renderDevice, m_bindlessLayout);
    else
        m_rtxdiPass = nullptr;

    m_accumulationPass = std::make_unique<AccumulationPass>(device(), m_context->shaderFactory);
    m_accumulationPass->createPipeline();
    m_accumulationPass->createBindingSet(
        m_renderTargets->outputColor, m_renderTargets->accumulatedRadiance, m_renderTargets->processedOutputColor);

    if (!m_gaussianFramePass)
        m_gaussianFramePass = std::make_unique<GaussianSplatFramePass>();
    m_gaussianFramePass->createTemporalResources(device(), m_context->shaderFactory, m_renderTargets.get());
    m_gaussianSplatTemporalReset = true;

    createPostProcessRenderPasses();

    if (!createPTPipeline())
        { assert(false); }

    const uint2 screenResolution = {
        m_renderTargets->outputColor->getDesc().width,
        m_renderTargets->outputColor->getDesc().height };
    createLightingRenderPasses(
        *m_context,
        device(),
        m_shaderDebug,
        m_bindlessLayout,
        initializeCommandList,
        screenResolution);

    prepareGaussianSplatPasses();

    if (!m_denoisePass)
        m_denoisePass = std::make_unique<DenoisePass>();
    m_denoisePass->createGuides(
        m_context,
        device(),
        m_context->shaderFactory,
        m_renderTargets,
        m_shaderDebug,
        m_bindingLayout);
}

bool caustica::render::WorldRenderer::createPTPipeline()
{
    if (!m_pathTracePass)
        m_pathTracePass = std::make_unique<PathTracePass>();
    return m_pathTracePass->createExportPipeline(
        device(), m_context->shaderFactory.get(), m_bindingLayout, m_bindlessLayout);
}

void caustica::render::WorldRenderer::prepareGaussianSplatPasses()
{
    if (!m_gaussianFramePass)
        m_gaussianFramePass = std::make_unique<GaussianSplatFramePass>();

    m_gaussianFramePass->bindStable(
        m_context,
        device(),
        &m_accelStructs,
        &m_context->scenePasses.gaussianSplats);
    m_gaussianFramePass->prepareScenePasses(m_shaderDebug);
}

void caustica::render::WorldRenderer::buildGaussianSplatEmissionProxies()
{
    if (!m_gaussianFramePass)
    {
        m_gaussianSplatEmissionProxies.clear();
        return;
    }
    m_gaussianFramePass->bindStable(
        m_context,
        device(),
        &m_accelStructs,
        &m_context->scenePasses.gaussianSplats);
    m_gaussianFramePass->buildEmissionProxies(
        m_gaussianSplatEmissionProxies,
        m_context->activeSettings());
}

void caustica::render::WorldRenderer::preRender()
{
    // Limit FPS
    if (m_context->activeSettings().actualFPSLimiter() > 0)
        g_FPSLimiter.framerateLimit(m_context->activeSettings().actualFPSLimiter());

    korgi::update();
}


void caustica::render::WorldRenderer::render(caustica::rhi::IFramebuffer* framebuffer)
{
    m_displaySize = m_renderSize = uint2(
        framebuffer->getFramebufferInfo().width,
        framebuffer->getFramebufferInfo().height);

    const uint32_t renderPhaseFrameIndex = m_context->gpuDevice.getRenderPhaseFrameIndex();
    std::shared_ptr<Scene> scene = m_context->sessionScene;
    if (scene)
    {
        scene->beginGpuReadFrame(renderPhaseFrameIndex);
        const scene::SceneRenderData& renderData = scene->getRenderData();

        m_frameSettingsSnapshot = renderData.renderSettings.settings;
        m_frameRuntimeSnapshot.Invalidation = renderData.renderSettings.invalidation;
        m_frameRuntimeSnapshot.Picking = renderData.renderSettings.picking;
        m_frameRuntimeSnapshot.GaussianSplats = m_context->runtimeState.GaussianSplats;
        m_frameGaussianSplatTemporalReset = renderData.renderSettings.gaussianSplatTemporalReset;
        m_context->sceneTime = renderData.renderSettings.sceneTime;

        m_context->frameSettings = &m_frameSettingsSnapshot;
        m_context->frameRuntime = &m_frameRuntimeSnapshot;
        m_context->frameScene = &renderData;
        m_context->frameGpu = m_context->sceneGpuResources.frameHandles();
        m_context->frameSceneStructureChanged = scene->hasSceneStructureChanged(renderPhaseFrameIndex);
        m_context->frameSceneTransformsChanged = scene->hasSceneTransformsChanged(renderPhaseFrameIndex);

        // Apply extracted camera pose before view update (RT owns view matrices).
        // Skip when snapshot has no session camera (structure-only republish / scene-load extract).
        if (renderData.camera.valid)
        {
            m_context->camera.camera().lookTo(
                renderData.camera.position, renderData.camera.direction, renderData.camera.up);
            m_context->camera.setVerticalFOV(renderData.camera.verticalFovRadians);
            m_context->camera.setZNear(renderData.camera.zNear);
            m_context->camera.setSelectedCameraIndex(renderData.camera.selectedCameraIndex);
            if (renderData.camera.useCustomIntrinsics)
            {
                m_context->camera.setIntrinsics(
                    renderData.camera.intrinsics.x,
                    renderData.camera.intrinsics.y,
                    renderData.camera.intrinsics.z,
                    renderData.camera.intrinsics.w,
                    renderData.camera.intrinsicsViewport.x,
                    renderData.camera.intrinsicsViewport.y);
            }
            else
            {
                m_context->camera.clearIntrinsics();
            }
        }
    }
    else
    {
        m_context->frameSettings = nullptr;
        m_context->frameRuntime = nullptr;
        m_context->frameScene = nullptr;
        m_context->frameGpu = {};
        m_context->frameSceneStructureChanged = false;
        m_context->frameSceneTransformsChanged = false;
        m_frameGaussianSplatTemporalReset = false;
    }

    populateRenderFrameContext(framebuffer, m_renderFrameCtx);
    m_pipelineRegistry.runFrame(*this, m_renderFrameCtx);

    // Preserve snapshot pick flags for AfterWorldRender resolve/clear. Live
    // runtimeState.Picking can change while older frames are still in flight.
    m_lastRenderedPicking = m_context->frameRuntime
        ? m_context->frameRuntime->Picking
        : RenderPickState{};

    m_context->frameSettings = nullptr;
    m_context->frameRuntime = nullptr;
    m_context->frameScene = nullptr;
    m_context->frameGpu = {};
    m_context->frameSceneStructureChanged = false;
    m_context->frameSceneTransformsChanged = false;

    if (scene)
        scene->endGpuReadFrame();

    if (m_renderFrameCtx.frame.aborted)
        postUpdatePathTracing();
}
void caustica::render::WorldRenderer::recreateBindingSet(const scene::SceneRenderData* renderData)
{
	// WARNING: this must match the layout of the m_bindingLayout (or switch to CreateBindingSetAndLayout)
    const SceneGpuFrameHandles gpuHandles = m_context->resolveGpuHandles();
    if (!gpuHandles.valid())
        return;

    caustica::rhi::rt::IAccelStruct* gaussianSplatAS = m_context->accelStructs.getTopLevelAS();
    caustica::rhi::IBuffer* gaussianSplatBuffer = m_context->scenePasses.lighting.materials()->getMaterialDataBuffer();
    // Prefer the explicit published pointer (GPU setup); else frameScene under beginGpuReadFrame.
    const std::span<const caustica::scene::GaussianSplatRenderProxy> gaussianSplats =
        renderData
            ? std::span<const caustica::scene::GaussianSplatRenderProxy>(renderData->gaussianSplats)
            : (m_context->hasFrameScene()
                ? m_context->frameGaussianSplats()
                : std::span<const caustica::scene::GaussianSplatRenderProxy>());
    const GaussianSplatBinding primaryGaussianBinding = getPrimaryGaussianSplatBinding(
        gaussianSplats, m_context->scenePasses.gaussianSplats);
    const GaussianSplatPass* primaryGaussianSplatPass = primaryGaussianBinding.splatPass;
    if (primaryGaussianSplatPass != nullptr && primaryGaussianSplatPass->getTopLevelAS() != nullptr && primaryGaussianSplatPass->getSplatBuffer() != nullptr)
    {
        gaussianSplatAS = primaryGaussianSplatPass->getTopLevelAS();
        gaussianSplatBuffer = primaryGaussianSplatPass->getSplatBuffer();
    }

    // Fixed resources that do not change between binding sets
    caustica::rhi::BindingSetDesc bindingSetDescBase;
    bindingSetDescBase.bindings = {
        caustica::rhi::BindingSetItem::ConstantBuffer(0, m_constantBuffer),
        caustica::rhi::BindingSetItem::PushConstants(1, sizeof(SampleMiniConstants)),
        //caustica::rhi::BindingSetItem::ConstantBuffer(2, m_context->scenePasses.lighting.lightSampling()->GetLightingConstants()),
        caustica::rhi::BindingSetItem::RayTracingAccelStruct(0, m_context->accelStructs.getTopLevelAS()),
        caustica::rhi::BindingSetItem::StructuredBuffer_SRV(1, m_context->accelStructs.getSubInstanceBuffer()),
        caustica::rhi::BindingSetItem::StructuredBuffer_SRV(2, gpuHandles.instanceBuffer),
        caustica::rhi::BindingSetItem::StructuredBuffer_SRV(3, gpuHandles.geometryBuffer),
        caustica::rhi::BindingSetItem::StructuredBuffer_SRV(4, m_context->scenePasses.lighting.opacityMaps() ?(m_context->scenePasses.lighting.opacityMaps()->getGeometryDebugBuffer()):(m_context->scenePasses.lighting.materials()->getMaterialDataBuffer().Get()) ),   // YUCK
        caustica::rhi::BindingSetItem::StructuredBuffer_SRV(5, m_context->scenePasses.lighting.materials()->getMaterialDataBuffer()),
        caustica::rhi::BindingSetItem::Texture_SRV(6,  m_renderTargets->ldrColorScratch, caustica::rhi::Format::SRGBA8_UNORM),
        caustica::rhi::BindingSetItem::RayTracingAccelStruct(7, gaussianSplatAS),
        caustica::rhi::BindingSetItem::StructuredBuffer_SRV(8, gaussianSplatBuffer),
        caustica::rhi::BindingSetItem::Texture_SRV(10, m_context->scenePasses.lighting.environment()->getEnvMapCube()),
        caustica::rhi::BindingSetItem::Texture_SRV(11, m_context->scenePasses.lighting.environment()->getImportanceSampling()->getImportanceMapOnly()),
        caustica::rhi::BindingSetItem::StructuredBuffer_SRV(12, m_context->scenePasses.lighting.lightSampling()->getControlBuffer()),
        caustica::rhi::BindingSetItem::StructuredBuffer_SRV(13, m_context->scenePasses.lighting.lightSampling()->getLightBuffer()),
        caustica::rhi::BindingSetItem::StructuredBuffer_SRV(14, m_context->scenePasses.lighting.lightSampling()->getLightExBuffer()),
        caustica::rhi::BindingSetItem::TypedBuffer_SRV(15, m_context->scenePasses.lighting.lightSampling()->getLightProxyCounters()),     // t_tightProxyCounters
        caustica::rhi::BindingSetItem::TypedBuffer_SRV(16, m_context->scenePasses.lighting.lightSampling()->getLightSamplingProxies()),   // t_LightProxyIndices
        caustica::rhi::BindingSetItem::TypedBuffer_SRV(17, m_context->scenePasses.lighting.lightSampling()->getLocalSamplingBuffer()),    // t_LightLocalSamplingBuffer
        caustica::rhi::BindingSetItem::Texture_SRV(18, m_context->scenePasses.lighting.lightSampling()->getEnvLightLookupMap()),          // t_EnvLookupMap
        //caustica::rhi::BindingSetItem::TypedBuffer_SRV(19, ),
        caustica::rhi::BindingSetItem::Texture_UAV(20, m_context->scenePasses.lighting.lightSampling()->getFeedbackTotalWeight()),        // u_LightFeedbackTotalWeight
        caustica::rhi::BindingSetItem::Texture_UAV(21, m_context->scenePasses.lighting.lightSampling()->getFeedbackCandidates()),         // u_LightFeedbackCandidates
        caustica::rhi::BindingSetItem::Sampler(0, m_context->renderDevice.samplers().anisotropicWrap()),
        caustica::rhi::BindingSetItem::Sampler(1, m_context->scenePasses.lighting.environment()->getEnvMapCubeSampler()),
        caustica::rhi::BindingSetItem::Sampler(2, m_context->scenePasses.lighting.environment()->getImportanceSampling()->getImportanceMapSampler()),
        caustica::rhi::BindingSetItem::Texture_UAV(0, m_renderTargets->outputColor),
        caustica::rhi::BindingSetItem::Texture_UAV(1, m_renderTargets->processedOutputColor),
        caustica::rhi::BindingSetItem::Texture_UAV(2, m_renderTargets->ldrColor, caustica::rhi::Format::RGBA8_UNORM),
        caustica::rhi::BindingSetItem::Texture_UAV(4, m_renderTargets->throughput),
        caustica::rhi::BindingSetItem::Texture_UAV(5, m_renderTargets->screenMotionVectors),
        caustica::rhi::BindingSetItem::Texture_UAV(6, m_renderTargets->depth),
        caustica::rhi::BindingSetItem::Texture_UAV(7, m_renderTargets->specularHitT), 
        caustica::rhi::BindingSetItem::Texture_UAV(8, m_renderTargets->scratchFloat1), 
        caustica::rhi::BindingSetItem::Texture_UAV(31, m_renderTargets->denoiserViewspaceZ),
        caustica::rhi::BindingSetItem::Texture_UAV(32, m_renderTargets->denoiserMotionVectors),
        caustica::rhi::BindingSetItem::Texture_UAV(33, m_renderTargets->denoiserNormalRoughness),
        caustica::rhi::BindingSetItem::Texture_UAV(34, m_renderTargets->denoiserDiffRadianceHitDist),
        caustica::rhi::BindingSetItem::Texture_UAV(35, m_renderTargets->denoiserSpecRadianceHitDist),
        caustica::rhi::BindingSetItem::Texture_UAV(36, m_renderTargets->denoiserDisocclusionThresholdMix),
        caustica::rhi::BindingSetItem::Texture_UAV(37, m_renderTargets->combinedHistoryClampRelax),
        caustica::rhi::BindingSetItem::StructuredBuffer_UAV(51, m_feedback_Buffer_Gpu),
        caustica::rhi::BindingSetItem::StructuredBuffer_UAV(52, m_debugLineBufferCapture),
        caustica::rhi::BindingSetItem::StructuredBuffer_UAV(53, m_debugDeltaPathTree_Gpu),
        caustica::rhi::BindingSetItem::StructuredBuffer_UAV(54, m_debugDeltaPathTreeSearchStack),
        caustica::rhi::BindingSetItem::Texture_UAV(60, m_renderTargets->secondarySurfacePositionNormal),
        caustica::rhi::BindingSetItem::Texture_UAV(61, m_renderTargets->secondarySurfaceRadiance),
        caustica::rhi::BindingSetItem::Texture_UAV(70, m_renderTargets->rrDiffuseAlbedo),
        caustica::rhi::BindingSetItem::Texture_UAV(71, m_renderTargets->rrSpecAlbedo),
        caustica::rhi::BindingSetItem::Texture_UAV(72, m_renderTargets->rrNormalsAndRoughness),
        caustica::rhi::BindingSetItem::Texture_UAV(73, m_renderTargets->rrSpecMotionVectors),
        caustica::rhi::BindingSetItem::Texture_UAV(74, (m_renderTargets->rrTransparencyLayer!=nullptr)?m_renderTargets->rrTransparencyLayer:m_renderTargets->rrSpecMotionVectors),
        caustica::rhi::BindingSetItem::Texture_UAV(75, m_renderTargets->denoiserAvgLayerRadianceHalfRes),

        ///***
        caustica::rhi::BindingSetItem::Texture_UAV(100, m_renderTargets->baseColor),
        caustica::rhi::BindingSetItem::Texture_UAV(101, m_renderTargets->specNormal),
        caustica::rhi::BindingSetItem::Texture_UAV(102, m_renderTargets->roughnessMetal),
        caustica::rhi::BindingSetItem::Texture_UAV(103, m_renderTargets->materialInfo),
        caustica::rhi::BindingSetItem::Texture_UAV(10, m_renderTargets->localCubemap),  // u_LocalCubemap for RT pass
        ///***

        caustica::rhi::BindingSetItem::RawBuffer_UAV(SHADER_DEBUG_BUFFER_UAV_INDEX, m_shaderDebug->getGPUWriteBuffer()),
        caustica::rhi::BindingSetItem::Texture_UAV(SHADER_DEBUG_VIZ_TEXTURE_UAV_INDEX, m_shaderDebug->getDebugVizTexture())
    };

    // NV HLSL extensions - DX12 only - we should probably expose some form of GetNvapiIsInitialized instead
    if (device()->queryFeatureSupport(caustica::rhi::Feature::HlslExtensionUAV))
    {
        bindingSetDescBase.bindings.push_back(
            caustica::rhi::BindingSetItem::TypedBuffer_UAV(NV_SHADER_EXTN_SLOT_NUM, nullptr));
    }

    // Main raytracing & etc binding set
    {
        caustica::rhi::BindingSetDesc bindingSetDesc;

        bindingSetDesc.bindings = bindingSetDescBase.bindings;

        bindingSetDesc.bindings.push_back(caustica::rhi::BindingSetItem::Texture_UAV(40, m_renderTargets->stablePlanesHeader));
        bindingSetDesc.bindings.push_back(caustica::rhi::BindingSetItem::StructuredBuffer_UAV(42, m_renderTargets->stablePlanesBuffer));
        bindingSetDesc.bindings.push_back(caustica::rhi::BindingSetItem::Texture_UAV(44, m_renderTargets->stableRadiance));
        bindingSetDesc.bindings.push_back(caustica::rhi::BindingSetItem::StructuredBuffer_UAV(45, m_renderTargets->surfaceDataBuffer));

        // Reflection system bindings (t80-t83, b3)
        // Derived classes can override AddCustomBindings to provide actual textures
        // Default to black texture fallbacks (RHI doesn't allow null textures)
        bindingSetDesc.bindings.push_back(caustica::rhi::BindingSetItem::Texture_SRV(80, m_context->renderDevice.builtins().blackCubeMapArray()));  // t_LocalCubemapGGX
        bindingSetDesc.bindings.push_back(caustica::rhi::BindingSetItem::Texture_SRV(81, m_context->renderDevice.builtins().blackCubeMapArray()));  // t_DiffuseIrradianceCube
        bindingSetDesc.bindings.push_back(caustica::rhi::BindingSetItem::Texture_SRV(82, m_context->renderDevice.builtins().blackTexture()));  // t_SSRBlurChain
        bindingSetDesc.bindings.push_back(caustica::rhi::BindingSetItem::Texture_SRV(83, (m_context->scenePasses.lighting.environment()->getBRDFLUT()!=nullptr)?m_context->scenePasses.lighting.environment()->getBRDFLUT():m_context->renderDevice.builtins().blackTexture() ));  // t_BRDFLUT
        bindingSetDesc.bindings.push_back(caustica::rhi::BindingSetItem::Texture_SRV(84, m_context->renderDevice.builtins().blackTexture()));  // t_DepthHierarchy placeholder
        bindingSetDesc.bindings.push_back(caustica::rhi::BindingSetItem::ConstantBuffer(10, m_constantBuffer)); // ReflectionConstants (reuse main constant buffer as placeholder)
        
        // SSR result UAV placeholder
        bindingSetDesc.bindings.push_back(caustica::rhi::BindingSetItem::Texture_UAV(85, m_renderTargets->depth));   // u_SSRResult placeholder

        // GTAO output (default to white = no occlusion; overridden by AddCustomBindings)
        bindingSetDesc.bindings.push_back(caustica::rhi::BindingSetItem::Texture_SRV(86, m_context->renderDevice.builtins().whiteTexture()));  // t_GTAOOutput placeholder
        // Previous frame depth (default to black = zero depth; overridden by AddCustomBindings)
        bindingSetDesc.bindings.push_back(caustica::rhi::BindingSetItem::Texture_SRV(87, m_context->renderDevice.builtins().blackTexture()));  // t_PrevDepth placeholder

        m_bindingSet = device()->createBindingSet(bindingSetDesc, m_bindingLayout);
    }
}

void caustica::render::WorldRenderer::denoisedScreenshot(caustica::rhi::ITexture* framebufferTexture) const
{
    std::string noisyImagePath = (caustica::getDirectoryWithExecutable() / "photo.bmp").string();

    auto execute = [&](const std::string& dn = "OptiX")
    {
        const auto p1 = std::chrono::system_clock::now();
        const std::string timestamp = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(p1.time_since_epoch()).count());

        const std::string fileName = "photo-denoised_" + dn + "_" + timestamp + ".bmp";

        std::string denoisedImagePath = (caustica::getDirectoryWithExecutable() / fileName).string();
        std::filesystem::path denoiserPath = getLocalPath("Support/denoiser_" + dn) / "denoiser.exe";
        if (!std::filesystem::exists(denoiserPath))
        {
            caustica::warning("External %s denoiser not found at '%s'.", dn.c_str(), denoiserPath.string().c_str());
            return;
        }

        if (!saveTextureToFile(device(), m_context->renderDevice, framebufferTexture, caustica::rhi::ResourceStates::Common, noisyImagePath.c_str()))
        {
            assert(false);
            return;
        }

        std::string startCmd = "\"" + denoiserPath.string() + "\"" + " -hdr 0 -i \"" + noisyImagePath + "\"" " -o \"" + denoisedImagePath + "\"";
        auto [resNum, resString, errorString] = systemShell(startCmd.c_str());
        if (resString != "")
            caustica::info("result: %s", resString.c_str());
        if (errorString != "")
            caustica::info("error: %s", errorString.c_str());

        std::string viewCmd = "\"" + denoisedImagePath + "\"";
        systemShell(viewCmd.c_str(), true);
    };
    execute("OptiX");
    execute("OIDN");
}

caustica::CameraUpdateParams caustica::render::WorldRenderer::makeCameraUpdateParams() const
{
    CameraUpdateParams params;
    params.renderSize = m_renderSize;
    params.displayAspectRatio = m_displayAspectRatio;
    params.sampleIndex = m_sampleIndex;
    params.frameIndex = static_cast<uint32_t>(m_frameIndex);
    params.realtimeMode = m_context->activeSettings().RealtimeMode;
    params.realtimeAA = m_context->activeSettings().RealtimeAA;
    params.dbgFreezeRealtimeNoiseSeed = m_context->activeSettings().DbgFreezeRealtimeNoiseSeed;
    params.syncPreviousView = m_context->activeSettings().ResetAccumulation || m_context->activeSettings().ResetRealtimeCaches;
    params.temporalAAJitter = m_context->activeSettings().TemporalAntiAliasingJitter;
    params.temporalAAPass = m_temporalAntiAliasingPass.get();
    return params;
}

void caustica::render::WorldRenderer::syncCameraViews()
{
    m_context->camera.updateViews(makeCameraUpdateParams());
    // Stable primary-hit pick: disable TAA/DLSS jitter for the pick frame.
    if (m_context->activeRuntime().Picking.hasActivePickRequest())
        m_context->camera.view()->setPixelOffset(dm::float2::zero());
}

dm::float2 caustica::render::WorldRenderer::computeCameraJitter() const
{
    if (m_context->activeRuntime().Picking.hasActivePickRequest())
        return dm::float2::zero();
    return m_context->camera.computeJitter(makeCameraUpdateParams());
}

void caustica::render::WorldRenderer::createPostProcessRenderPasses()
{
    m_toneMappingPass = std::make_unique<ToneMappingPass>(device(), m_context->shaderFactory, m_context->renderDevice, m_renderTargets->ldrFramebuffer, *m_context->camera.view(), m_renderTargets->outputColor);
    m_bloomPass = std::make_unique<BloomPass>(device(), m_context->shaderFactory, m_context->renderDevice, m_renderTargets->processedOutputFramebuffer, *m_context->camera.view());
    m_postProcess = std::make_shared<PostProcess>(device(), m_context->shaderFactory, m_context->renderDevice, m_shaderDebug);

    {
        TemporalAntiAliasingPass::CreateParameters taaParams;
        taaParams.sourceDepth = m_renderTargets->depth;
        taaParams.motionVectors = m_renderTargets->screenMotionVectors;
        taaParams.unresolvedColor = m_renderTargets->outputColor;
        taaParams.resolvedColor = m_renderTargets->processedOutputColor;
        taaParams.feedback1 = m_renderTargets->temporalFeedback1;
        taaParams.feedback2 = m_renderTargets->temporalFeedback2;
        taaParams.historyClampRelax = m_renderTargets->combinedHistoryClampRelax;
        taaParams.motionVectorStencilMask = 0;
        taaParams.useCatmullRomFilter = true;

        m_temporalAntiAliasingPass = std::make_unique<TemporalAntiAliasingPass>(device(), m_context->shaderFactory, m_context->renderDevice, *m_context->camera.view(), taaParams);
    }
}

void caustica::render::WorldRenderer::preUpdatePathTracing( bool resetAccum, caustica::rhi::CommandListHandle commandList )
{
    const bool resetReferenceOidn = !m_context->activeSettings().RealtimeMode && (resetAccum || m_context->activeSettings().ResetAccumulation || m_context->activeSettings().ReferenceOIDNDenoiserChanged);
    if (resetReferenceOidn || m_context->activeSettings().ReferenceOIDNDenoiserChanged)
    {
        if (m_denoisePass)
            m_denoisePass->resetReferenceOIDN();
        m_context->activeSettings().ReferenceOIDNDenoiserChanged = false;
    }

    resetAccum |= m_context->activeSettings().ResetAccumulation;
    resetAccum |= m_context->activeSettings().RealtimeMode;

    if (resetAccum)
    {
        m_accumulationSampleIndex = (m_context->activeSettings().AccumulationPreWarmRealtimeCaches)?(-32):(0);
    }
#if ENABLE_DEBUG_VIZUALISATIONS
    if (resetAccum && m_shaderDebug)
        m_shaderDebug->clearDebugVizTexture(commandList);
#endif

    if( m_accumulationSampleIndex < 16 )
    {
        m_context->diagnostics.benchStart = std::chrono::high_resolution_clock::now( );
        m_context->diagnostics.benchLast = m_context->diagnostics.benchStart;
        m_context->diagnostics.benchFrames = 0;
    } else if( m_accumulationSampleIndex < m_context->activeSettings().AccumulationTarget )
    {
        m_context->diagnostics.benchFrames++;
        m_context->diagnostics.benchLast = std::chrono::high_resolution_clock::now( );
    }
    m_accumulationCompleted = false;
    if( !m_context->activeSettings().RealtimeMode )
    {
        m_sampleIndex = (m_accumulationSampleIndex<0)?(m_accumulationSampleIndex+4096):(min(m_accumulationSampleIndex, m_context->activeSettings().AccumulationTarget - 1));
        m_accumulationCompleted |= m_accumulationSampleIndex == m_context->activeSettings().AccumulationTarget - 1;
    }
    else
        m_sampleIndex = (!m_context->activeSettings().DbgFreezeRealtimeNoiseSeed)?( m_frameIndex % 8192 ):0;
}

void caustica::render::WorldRenderer::postUpdatePathTracing( )
{
    m_accumulationSampleIndex = std::min( m_accumulationSampleIndex+1, m_context->activeSettings().AccumulationTarget );

    if (m_context->activeSettings().actualUseRTXDIPasses() && m_rtxdiPass)
        m_rtxdiPass->endFrame();

    m_context->activeSettings().ResetAccumulation = false;
    m_context->activeSettings().ResetRealtimeCaches = false;
    m_frameIndex++;
}

#if CAUSTICA_WITH_NATIVE_DLSS
namespace
{
float GetNativeDLSSResolutionScale(SI::DLSSMode mode)
{
    switch (mode)
    {
    case SI::DLSSMode::eUltraPerformance: return 1.0f / 3.0f;
    case SI::DLSSMode::eMaxPerformance:   return 0.5f;
    case SI::DLSSMode::eBalanced:         return 0.58f;
    case SI::DLSSMode::eMaxQuality:       return 2.0f / 3.0f;
    case SI::DLSSMode::eUltraQuality:     return 0.77f;
    case SI::DLSSMode::eDLAA:             return 1.0f;
    default:                              return 0.58f;
    }
}

uint2 GetNativeDLSSRenderSize(uint2 displaySize, SI::DLSSMode mode)
{
    const float scale = GetNativeDLSSResolutionScale(mode);
    return uint2(
        std::max(1u, uint32_t(std::round(float(displaySize.x) * scale))),
        std::max(1u, uint32_t(std::round(float(displaySize.y) * scale))));
}
}
#endif
#if CAUSTICA_WITH_STREAMLINE
void caustica::render::WorldRenderer::streamlinePreRender()
{
#if CAUSTICA_WITH_STREAMLINE
    if (m_context->gpuDevice.isHeadless())
        return;

    auto& streamline = m_context->gpuDevice.getStreamline();
    m_context->activeSettings().IsDLSSSuported = streamline.isDLSSAvailable();
    m_context->activeSettings().IsDLSSRRSupported = streamline.isDLSSRRAvailable();
    m_context->activeSettings().IsDLSSFGSupported = streamline.isDLSSGAvailable();
    m_context->activeSettings().IsReflexSupported = streamline.isReflexAvailable();

    // Setup Reflex
    {
        auto reflexConsts = caustica::StreamlineInterface::ReflexOptions{};
        reflexConsts.mode = (caustica::StreamlineInterface::ReflexMode) m_context->activeSettings().actualReflexMode();
        reflexConsts.frameLimitUs = m_context->activeSettings().ReflexCappedFps == 0 ? 0 : int(1000000. / m_context->activeSettings().ReflexCappedFps);
        reflexConsts.useMarkersToOptimize = true;
        reflexConsts.virtualKey = VK_F13;
        reflexConsts.idThread = 0; // std::hash<std::thread::id>()(std::this_thread::get_id())
        streamline.setReflexConsts(reflexConsts);

        // Need to update StreamlineIntegration with the ability to query reflex state
        caustica::StreamlineInterface::ReflexState reflexState{};
        streamline.getReflexState(reflexState);
        if (m_context->activeSettings().IsReflexSupported)
        {
            m_context->activeSettings().IsReflexLowLatencyAvailable = reflexState.lowLatencyAvailable;
            m_context->activeSettings().IsReflexFlashIndicatorDriverControlled = reflexState.flashIndicatorDriverControlled;

            auto report = reflexState.frameReport[63];
            if (reflexState.lowLatencyAvailable && report.gpuRenderEndTime != 0)
            {
                auto frameID = report.frameID;
                auto totalGameToRenderLatencyUs = report.gpuRenderEndTime - report.inputSampleTime;
                auto simDeltaUs = report.simEndTime - report.simStartTime;
                auto renderDeltaUs = report.renderSubmitEndTime - report.renderSubmitStartTime;
                auto presentDeltaUs = report.presentEndTime - report.presentStartTime;
                auto driverDeltaUs = report.driverEndTime - report.driverStartTime;
                auto osRenderQueueDeltaUs = report.osRenderQueueEndTime - report.osRenderQueueStartTime;
                auto gpuRenderDeltaUs = report.gpuRenderEndTime - report.gpuRenderStartTime;

                m_context->activeSettings().ReflexStats = "frameID: " + std::to_string(frameID);
                m_context->activeSettings().ReflexStats += "\ntotalGameToRenderLatencyUs: " + std::to_string(totalGameToRenderLatencyUs);
                m_context->activeSettings().ReflexStats += "\nsimDeltaUs: " + std::to_string(simDeltaUs);
                m_context->activeSettings().ReflexStats += "\nrenderDeltaUs: " + std::to_string(renderDeltaUs);
                m_context->activeSettings().ReflexStats += "\npresentDeltaUs: " + std::to_string(presentDeltaUs);
                m_context->activeSettings().ReflexStats += "\ndriverDeltaUs: " + std::to_string(driverDeltaUs);
                m_context->activeSettings().ReflexStats += "\nosRenderQueueDeltaUs: " + std::to_string(osRenderQueueDeltaUs);
                m_context->activeSettings().ReflexStats += "\ngpuRenderDeltaUs: " + std::to_string(gpuRenderDeltaUs);
            }
            else
            {
                m_context->activeSettings().ReflexStats = "Latency Report Unavailable";
            }
        }
    }

    // DLSS-G Setup
    {
        const auto actualDLSSFGMode = m_context->activeSettings().actualDLSSFGMode();
        const bool wasDLSSFGEnabled = m_context->activeSettings().DLSSFGOptions.mode == StreamlineInterface::DLSSGMode::eOn;

        // If DLSS-G has been turned off, then we tell tell SL to clean it up expressly
        if (wasDLSSFGEnabled && actualDLSSFGMode == StreamlineInterface::DLSSGMode::eOff) {
            streamline.cleanupDLSSG(true);
        }

        // This is where DLSS-G is toggled On and Off (using dlssgOptions.mode) and where we set DLSS-G parameters.
        auto dlssgOptions = StreamlineInterface::DLSSGOptions{};
        dlssgOptions.mode = actualDLSSFGMode;
        dlssgOptions.numFramesToGenerate = m_context->activeSettings().DLSSFGNumFramesToGenerate;

        // This is where we query DLSS-G minimum swapchain size
        if (m_context->activeSettings().IsDLSSFGSupported &&
            (actualDLSSFGMode != StreamlineInterface::DLSSGMode::eOff || wasDLSSFGEnabled))
        {
            StreamlineInterface::DLSSGState state;
            streamline.getDLSSGState(state, dlssgOptions);
            m_context->activeSettings().DLSSFGMultiplier = state.numFramesActuallyPresented;
            m_context->activeSettings().DLSSFGMaxNumFramesToGenerate = state.numFramesToGenerateMax;

            streamline.setDLSSGOptions(dlssgOptions);
            m_context->activeSettings().DLSSFGOptions = dlssgOptions;
        }
        else
        {
            m_context->activeSettings().DLSSFGMultiplier = 1;
            m_context->activeSettings().DLSSFGOptions = dlssgOptions;
        }
    }

    // Ensure DLSS / DLSS-RR is available
    if (m_context->activeSettings().RealtimeAA == 3 && !m_context->activeSettings().IsDLSSRRSupported)
    {
        caustica::warning("Requested DLSS-RR mode not available. Switching to DLSS. ");
        m_context->activeSettings().RealtimeAA = 2;
    }
    if ( m_context->activeSettings().RealtimeAA == 2 && !m_context->activeSettings().IsDLSSSuported )
    {
        caustica::warning("Requested DLSS mode not available. Switching to TAA. ");
        m_context->activeSettings().RealtimeAA = 1;
    }

    // Setup DLSS
    const bool changeToDLSSMode = (m_context->activeSettings().RealtimeAA >= 2 && m_context->activeSettings().RealtimeAA <= 3) && m_context->activeSettings().DLSSLastRealtimeAA != m_context->activeSettings().RealtimeAA;
    {
        // reset DLSS vars if we stop using it
        if (changeToDLSSMode || m_context->activeSettings().DLSSMode == StreamlineInterface::DLSSMode::eOff)
        {
            m_context->activeSettings().DLSSLastMode = PathTracerSettings::DLSSModeDefault;
            m_context->activeSettings().DLSSMode = PathTracerSettings::DLSSModeDefault;
            m_context->activeSettings().DLSSLastDisplaySize = { 0,0 };
        }

        m_context->activeSettings().DLSSLastRealtimeAA = m_context->activeSettings().RealtimeAA;

        // If we are using DLSS set its constants
        if ((m_context->activeSettings().RealtimeAA == 2 || m_context->activeSettings().RealtimeAA == 3) && m_context->activeSettings().RealtimeMode)
        {
            StreamlineInterface::DLSSOptions dlssOptions = {};
            if (m_context->activeSettings().IsDLSSSuported)
            {
                dlssOptions.mode = m_context->activeSettings().DLSSMode;
                dlssOptions.outputWidth = m_displaySize.x;
                dlssOptions.outputHeight = m_displaySize.y;
                dlssOptions.sharpness = 0; //m_recommendedDLSSSettings.sharpness;    // <- is this no longer valid?
                dlssOptions.colorBuffersHDR = true;
                dlssOptions.useAutoExposure = true;     // Optional: provide proper "kBufferTypeExposure" for 0-lag for better precision handling
                dlssOptions.preset = StreamlineInterface::DLSSPreset::eDefault;
                // if (m_context->activeSettings().RealtimeAA < 4) <- docs https://github.com/NVIDIAGameWorks/Streamline/blob/main/docs/ProgrammingGuideDLSS_RR.md#50-provide-dlss--dlss-rr-options seem to imply that these should be set even when DLSS-RR enabled
                    streamline.setDLSSOptions(dlssOptions);
            }
            else
            {
                assert( false ); // shouldn't happen, code above should have dropped us to "m_context->activeSettings().RealtimeAA = 1" - check for recent code changes.
            }

            if (m_context->activeSettings().RealtimeAA == 2 || m_context->activeSettings().RealtimeAA == 3)
            {
                // Check if we need to update the rendertarget size.
                bool dlssResizeRequired = (m_context->activeSettings().DLSSMode != m_context->activeSettings().DLSSLastMode) || (m_displaySize.x != m_context->activeSettings().DLSSLastDisplaySize.x) || (m_displaySize.y != m_context->activeSettings().DLSSLastDisplaySize.y);
                if (dlssResizeRequired)
                {
                    // Only quality, target width and height matter here
                    streamline.queryDLSSOptimalSettings(dlssOptions, m_recommendedDLSSSettings);

                    // this is an example on how to override defaults - overriding default 2/3 to higher res 3/4
                    if (dlssOptions.mode == SI::DLSSMode::eMaxQuality)
                    {
                        m_recommendedDLSSSettings.optimalRenderSize.x = dm::clamp((int)(dlssOptions.outputWidth * 3 / 4 + 0.5f), m_recommendedDLSSSettings.minRenderSize.x, m_recommendedDLSSSettings.maxRenderSize.x);
                        m_recommendedDLSSSettings.optimalRenderSize.y = dm::clamp((int)(dlssOptions.outputHeight * 3 / 4 + 0.5f), m_recommendedDLSSSettings.minRenderSize.y, m_recommendedDLSSSettings.maxRenderSize.y);
                    }

                    if (m_recommendedDLSSSettings.optimalRenderSize.x <= 0 || m_recommendedDLSSSettings.optimalRenderSize.y <= 0)
                    {
                        m_context->activeSettings().RealtimeAA = 0;
                        m_context->activeSettings().DLSSMode = PathTracerSettings::DLSSModeDefault;
                        m_renderSize = m_displaySize;
                    }
                    else
                    {
                        m_context->activeSettings().DLSSLastMode = m_context->activeSettings().DLSSMode;
                        m_context->activeSettings().DLSSLastDisplaySize = m_displaySize;
                    }
                }

                m_renderSize = (uint2)m_recommendedDLSSSettings.optimalRenderSize;
            }

            if (m_context->activeSettings().RealtimeAA == 3) // DLSS-RR
            {
                StreamlineInterface::DLSSRROptions dlssRROptions = {};
                dlssRROptions.mode              	= dlssOptions.mode;
                dlssRROptions.outputWidth       	= dlssOptions.outputWidth;
                dlssRROptions.outputHeight      	= dlssOptions.outputHeight;
                dlssRROptions.sharpness         	= dlssOptions.sharpness;
                dlssRROptions.preExposure       	= dlssOptions.preExposure;
                dlssRROptions.exposureScale     	= dlssOptions.exposureScale;
                dlssRROptions.colorBuffersHDR   	= dlssOptions.colorBuffersHDR;
                dlssRROptions.indicatorInvertAxisX 	= dlssOptions.indicatorInvertAxisX;
                dlssRROptions.indicatorInvertAxisY 	= dlssOptions.indicatorInvertAxisY;
                dlssRROptions.normalRoughnessMode 	= StreamlineInterface::DLSSRRNormalRoughnessMode::ePacked;
                dlssRROptions.alphaUpscalingEnabled = false;
                dlssRROptions.preset                = m_context->activeSettings().DLSRRPreset;
                m_lastDLSSRROptions = dlssRROptions; // we need to fill them up with view info, but we can only have proper view after it was initialized with correct RT size
            }
        }
        else
        {
            if (m_context->activeSettings().IsDLSSSuported)
            {
                StreamlineInterface::DLSSOptions dlssOptions = {};
                dlssOptions.mode = StreamlineInterface::DLSSMode::eOff;
                streamline.setDLSSOptions(dlssOptions);
            }

            m_renderSize = m_displaySize;
        }
    }
#else
    const bool changeToDLSSMode = false;
#endif // #if CAUSTICA_WITH_STREAMLINE
}
#endif

#if CAUSTICA_WITH_NATIVE_DLSS
void caustica::render::WorldRenderer::nativeDLSSPreRender()
{
    if (!m_context->activeSettings().RealtimeMode)
    {
        m_renderSize = m_displaySize;
        return;
    }

    if (m_nativeDLSS)
    {
        m_context->activeSettings().IsDLSSSuported = m_nativeDLSS->isDlssSupported();
        m_context->activeSettings().IsDLSSRRSupported = m_nativeDLSS->isRayReconstructionSupported();
    }

    if (m_context->activeSettings().RealtimeAA == 3 && !m_context->activeSettings().IsDLSSRRSupported)
    {
        caustica::warning("Requested DLSS-RR mode not available. Switching to DLSS.");
        m_context->activeSettings().RealtimeAA = 2;
    }

    if (m_context->activeSettings().RealtimeAA == 2 && !m_context->activeSettings().IsDLSSSuported)
    {
        caustica::warning("Requested DLSS mode not available. Switching to TAA.");
        m_context->activeSettings().RealtimeAA = 1;
    }

    const bool usingDLSS = (m_context->activeSettings().RealtimeAA == 2 || m_context->activeSettings().RealtimeAA == 3);
    const bool changeToDLSSMode = usingDLSS && m_context->activeSettings().DLSSLastRealtimeAA != m_context->activeSettings().RealtimeAA;

    if (changeToDLSSMode || m_context->activeSettings().DLSSMode == SI::DLSSMode::eOff)
    {
        m_context->activeSettings().DLSSLastMode = PathTracerSettings::DLSSModeDefault;
        m_context->activeSettings().DLSSMode = PathTracerSettings::DLSSModeDefault;
        m_context->activeSettings().DLSSLastDisplaySize = { 0, 0 };
    }

    m_context->activeSettings().DLSSLastRealtimeAA = m_context->activeSettings().RealtimeAA;

    if (usingDLSS)
    {
        const bool dlssResizeRequired =
            (m_context->activeSettings().DLSSMode != m_context->activeSettings().DLSSLastMode) ||
            (m_displaySize.x != m_context->activeSettings().DLSSLastDisplaySize.x) ||
            (m_displaySize.y != m_context->activeSettings().DLSSLastDisplaySize.y);

        if (dlssResizeRequired)
        {
            m_context->activeSettings().DLSSLastMode = m_context->activeSettings().DLSSMode;
            m_context->activeSettings().DLSSLastDisplaySize = m_displaySize;
        }

        m_renderSize = GetNativeDLSSRenderSize(m_displaySize, m_context->activeSettings().DLSSMode);
    }
    else
    {
        m_renderSize = m_displaySize;
    }
}
#endif

