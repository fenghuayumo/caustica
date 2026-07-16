namespace { constexpr int c_SwapchainCount = 3; }

#include <render/worldRenderer/WorldRenderer.h>
#include <render/pipeline/RenderPipelineRegistry.h>
#include <render/SceneGpuResources.h>
#include <render/worldRenderer/PathTracingContext.h>
#include <render/core/RenderDevice.h>
#include <render/core/RenderPassConstants.h>
#include <render/SceneGaussianSplatPasses.h>
#include <render/SceneRayTracingResources.h>

#include <scene/Scene.h>
#include <scene/SceneEcs.h>
#include <scene/SceneLightAccess.h>
#include <render/core/PostProcessAA.h>
#include <render/core/SceneGeometryUpdate.h>
#include <render/core/LightingUpdate.h>
#include <render/core/AccelStructManager.h>
#include <render/core/CameraController.h>
#include <render/core/PathTracingShaderCompiler.h>
#include <render/core/ComputePipelineRegistry.h>
#include <render/core/BindingCache.h>
#include <scene/View.h>
#include <render/core/FramebufferFactory.h>
#include <render/core/AccelerationStructureUtil.h>
#include <render/passes/lighting/distant/EnvMapImportanceSamplingCache.h>
#include <render/passes/lighting/MaterialGpuCache.h>
#include <render/passes/omm/OpacityMicromapBuilder.h>
#include <render/passes/postProcess/DenoisingGuidesPass.h>
#include <render/passes/denoisers/OidnDenoiser.h>
#include <render/passes/gaussian/GaussianSplatGraph.h>
#include <render/passes/gaussian/GaussianSplatSceneRuntime.h>
#include <assets/loader/ShaderFactory.h>
#include <render/gpuSort/GPUSort.h>
#include <backend/GpuDevice.h>
#include <core/path_utils.h>
#include <core/log.h>
#include <core/progress.h>
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

namespace
{
    constexpr float c_envMapRadianceScale = 1.0f / 4.0f;

#if CAUSTICA_WITH_NATIVE_DLSS
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
#endif
}

caustica::render::WorldRenderer::WorldRenderer(PathTracingContext& context)
    : m_context(context)
{
    m_lastRealtimeMode = m_context.activeSettings().RealtimeMode;
}

caustica::render::WorldRenderer::~WorldRenderer() = default;

nvrhi::BindingLayoutHandle caustica::render::WorldRenderer::createBindlessLayout(nvrhi::IDevice* device)
{
    nvrhi::BindlessLayoutDesc bindlessLayoutDesc;
    bindlessLayoutDesc.visibility = nvrhi::ShaderType::All;
    bindlessLayoutDesc.firstSlot = 0;
    bindlessLayoutDesc.maxCapacity = 1024;
    bindlessLayoutDesc.registerSpaces = {
        nvrhi::BindingLayoutItem::RawBuffer_SRV(1),
        nvrhi::BindingLayoutItem::Texture_SRV(2)
    };
    return device->createBindlessLayout(bindlessLayoutDesc);
}

void caustica::render::WorldRenderer::createBindingLayouts(nvrhi::IBindingLayout* precreatedBindless)
{
    nvrhi::IDevice* const gpuDevice = device();
    m_bindlessLayout = precreatedBindless
        ? nvrhi::BindingLayoutHandle(precreatedBindless)
        : createBindlessLayout(gpuDevice);

    nvrhi::BindingLayoutDesc globalBindingLayoutDesc;
    globalBindingLayoutDesc.visibility = nvrhi::ShaderType::All;
    globalBindingLayoutDesc.bindings = {
        nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
        nvrhi::BindingLayoutItem::PushConstants(1, sizeof(SampleMiniConstants)),
        nvrhi::BindingLayoutItem::RayTracingAccelStruct(0),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(5),
        nvrhi::BindingLayoutItem::Texture_SRV(6),
        nvrhi::BindingLayoutItem::RayTracingAccelStruct(7),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(8),
        nvrhi::BindingLayoutItem::Texture_SRV(10),
        nvrhi::BindingLayoutItem::Texture_SRV(11),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(12),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(13),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(14),
        nvrhi::BindingLayoutItem::TypedBuffer_SRV(15),
        nvrhi::BindingLayoutItem::TypedBuffer_SRV(16),
        nvrhi::BindingLayoutItem::TypedBuffer_SRV(17),
        nvrhi::BindingLayoutItem::Texture_SRV(18),
        nvrhi::BindingLayoutItem::Texture_UAV(20),
        nvrhi::BindingLayoutItem::Texture_UAV(21),
        nvrhi::BindingLayoutItem::Sampler(0),
        nvrhi::BindingLayoutItem::Sampler(1),
        nvrhi::BindingLayoutItem::Sampler(2),
        nvrhi::BindingLayoutItem::Texture_UAV(0),
        nvrhi::BindingLayoutItem::Texture_UAV(1),
        nvrhi::BindingLayoutItem::Texture_UAV(2),
        nvrhi::BindingLayoutItem::Texture_UAV(4),
        nvrhi::BindingLayoutItem::Texture_UAV(5),
        nvrhi::BindingLayoutItem::Texture_UAV(6),
        nvrhi::BindingLayoutItem::Texture_UAV(7),
        nvrhi::BindingLayoutItem::Texture_UAV(8),
        nvrhi::BindingLayoutItem::Texture_UAV(31),
        nvrhi::BindingLayoutItem::Texture_UAV(32),
        nvrhi::BindingLayoutItem::Texture_UAV(33),
        nvrhi::BindingLayoutItem::Texture_UAV(34),
        nvrhi::BindingLayoutItem::Texture_UAV(35),
        nvrhi::BindingLayoutItem::Texture_UAV(36),
        nvrhi::BindingLayoutItem::Texture_UAV(37),
        nvrhi::BindingLayoutItem::StructuredBuffer_UAV(51),
        nvrhi::BindingLayoutItem::StructuredBuffer_UAV(52),
        nvrhi::BindingLayoutItem::StructuredBuffer_UAV(53),
        nvrhi::BindingLayoutItem::StructuredBuffer_UAV(54),
        nvrhi::BindingLayoutItem::Texture_UAV(60),
        nvrhi::BindingLayoutItem::Texture_UAV(61),
        nvrhi::BindingLayoutItem::Texture_UAV(70),
        nvrhi::BindingLayoutItem::Texture_UAV(71),
        nvrhi::BindingLayoutItem::Texture_UAV(72),
        nvrhi::BindingLayoutItem::Texture_UAV(73),
        nvrhi::BindingLayoutItem::Texture_UAV(74),
        nvrhi::BindingLayoutItem::Texture_UAV(75),
        nvrhi::BindingLayoutItem::RawBuffer_UAV(SHADER_DEBUG_BUFFER_UAV_INDEX),
        nvrhi::BindingLayoutItem::Texture_UAV(SHADER_DEBUG_VIZ_TEXTURE_UAV_INDEX)
    };

    if (gpuDevice->queryFeatureSupport(nvrhi::Feature::HlslExtensionUAV))
    {
        globalBindingLayoutDesc.bindings.push_back(
            nvrhi::BindingLayoutItem::TypedBuffer_UAV(NV_SHADER_EXTN_SLOT_NUM));
    }

    globalBindingLayoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_UAV(40));
    globalBindingLayoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(42));
    globalBindingLayoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_UAV(44));
    globalBindingLayoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(45));

    globalBindingLayoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_UAV(100));
    globalBindingLayoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_UAV(101));
    globalBindingLayoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_UAV(102));
    globalBindingLayoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_UAV(103));
    globalBindingLayoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_UAV(10));
    globalBindingLayoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(80));
    globalBindingLayoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(81));
    globalBindingLayoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(82));
    globalBindingLayoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(83));
    globalBindingLayoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(84));
    globalBindingLayoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::VolatileConstantBuffer(10));
    globalBindingLayoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_UAV(85));
    globalBindingLayoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(86));
    globalBindingLayoutDesc.bindings.push_back(nvrhi::BindingLayoutItem::Texture_SRV(87));

    m_bindingLayout = gpuDevice->createBindingLayout(globalBindingLayoutDesc);
}

void caustica::render::WorldRenderer::createDeviceResources()
{
    nvrhi::IDevice* device = this->device();

    m_renderTargetPool.setDevice(device);
    m_renderBufferPool.setDevice(device);
    m_frameGraph.setRenderTargetPool(&m_renderTargetPool);
    m_frameGraph.setRenderBufferPool(&m_renderBufferPool);

#if CAUSTICA_WITH_NATIVE_DLSS
    m_nativeDLSS = caustica::render::DLSS::create(device, m_context.shaderFactory, caustica::getDirectoryWithExecutable().string());
    if (m_nativeDLSS)
    {
        m_context.activeSettings().IsDLSSSuported = m_nativeDLSS->isDlssSupported();
        m_context.activeSettings().IsDLSSRRSupported = m_nativeDLSS->isRayReconstructionSupported();
        caustica::info("Native NGX DLSS support: DLSS=%s, DLSS-RR=%s.",
            m_context.activeSettings().IsDLSSSuported ? "yes" : "no",
            m_context.activeSettings().IsDLSSRRSupported ? "yes" : "no");
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
        m_linesVertexShader = m_context.shaderFactory->createShader("caustica/shaders/render/misc/DebugLines.hlsl", "main_vs", &drawLinesMacro, nvrhi::ShaderType::Vertex);
        m_linesPixelShader = m_context.shaderFactory->createShader("caustica/shaders/render/misc/DebugLines.hlsl", "main_ps", &drawLinesMacro, nvrhi::ShaderType::Pixel);

        nvrhi::VertexAttributeDesc attributes[] = {
            nvrhi::VertexAttributeDesc()
                .setName("POSITION")
                .setFormat(nvrhi::Format::RGBA32_FLOAT)
                .setOffset(0)
                .setElementStride(sizeof(DebugLineStruct)),
            nvrhi::VertexAttributeDesc()
                .setName("COLOR")
                .setFormat(nvrhi::Format::RGBA32_FLOAT)
                .setOffset(offsetof(DebugLineStruct, col))
                .setElementStride(sizeof(DebugLineStruct)),
        };
        m_linesInputLayout = device->createInputLayout(attributes, uint32_t(std::size(attributes)), m_linesVertexShader);

        nvrhi::BindingLayoutDesc linesBindingLayoutDesc;
        linesBindingLayoutDesc.visibility = nvrhi::ShaderType::All;
        linesBindingLayoutDesc.bindings = {
            nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
            nvrhi::BindingLayoutItem::Texture_SRV(0)
        };
        m_linesBindingLayout = device->createBindingLayout(linesBindingLayoutDesc);

        nvrhi::BufferDesc bufferDesc;
        bufferDesc.byteSize = sizeof(DebugFeedbackStruct) * 1;
        bufferDesc.isConstantBuffer = false;
        bufferDesc.isVolatile = false;
        bufferDesc.canHaveUAVs = true;
        bufferDesc.cpuAccess = nvrhi::CpuAccessMode::None;
        bufferDesc.maxVersions = caustica::c_MaxRenderPassConstantBufferVersions;
        bufferDesc.structStride = sizeof(DebugFeedbackStruct);
        bufferDesc.keepInitialState = true;
        bufferDesc.initialState = nvrhi::ResourceStates::Common;
        bufferDesc.debugName = "Feedback_Buffer_Gpu";
        m_feedback_Buffer_Gpu = device->createBuffer(bufferDesc);

        bufferDesc.canHaveUAVs = false;
        bufferDesc.cpuAccess = nvrhi::CpuAccessMode::Read;
        bufferDesc.structStride = 0;
        bufferDesc.keepInitialState = false;
        bufferDesc.initialState = nvrhi::ResourceStates::Unknown;
        bufferDesc.debugName = "Feedback_Buffer_Cpu";
        m_feedback_Buffer_Cpu = device->createBuffer(bufferDesc);

        bufferDesc.byteSize = sizeof(DebugLineStruct) * MAX_DEBUG_LINES;
        bufferDesc.isVertexBuffer = true;
        bufferDesc.isConstantBuffer = false;
        bufferDesc.isVolatile = false;
        bufferDesc.canHaveUAVs = true;
        bufferDesc.cpuAccess = nvrhi::CpuAccessMode::None;
        bufferDesc.structStride = sizeof(DebugLineStruct);
        bufferDesc.keepInitialState = true;
        bufferDesc.initialState = nvrhi::ResourceStates::Common;
        bufferDesc.debugName = "DebugLinesCapture";
        m_debugLineBufferCapture = device->createBuffer(bufferDesc);
        bufferDesc.debugName = "DebugLinesDisplay";
        m_debugLineBufferDisplay = device->createBuffer(bufferDesc);

        bufferDesc.byteSize = sizeof(DeltaTreeVizPathVertex) * cDeltaTreeVizMaxVertices;
        bufferDesc.isConstantBuffer = false;
        bufferDesc.isVolatile = false;
        bufferDesc.canHaveUAVs = true;
        bufferDesc.cpuAccess = nvrhi::CpuAccessMode::None;
        bufferDesc.maxVersions = caustica::c_MaxRenderPassConstantBufferVersions;
        bufferDesc.structStride = sizeof(DeltaTreeVizPathVertex);
        bufferDesc.keepInitialState = true;
        bufferDesc.initialState = nvrhi::ResourceStates::Common;
        bufferDesc.debugName = "Feedback_PathDecomp_Gpu";
        m_debugDeltaPathTree_Gpu = device->createBuffer(bufferDesc);

        bufferDesc.canHaveUAVs = false;
        bufferDesc.cpuAccess = nvrhi::CpuAccessMode::Read;
        bufferDesc.structStride = 0;
        bufferDesc.keepInitialState = false;
        bufferDesc.initialState = nvrhi::ResourceStates::Unknown;
        bufferDesc.debugName = "Feedback_PathDecomp_Cpu";
        m_debugDeltaPathTree_Cpu = device->createBuffer(bufferDesc);

        bufferDesc.byteSize = sizeof(PathPayload) * cDeltaTreeVizMaxStackSize;
        bufferDesc.isConstantBuffer = false;
        bufferDesc.isVolatile = false;
        bufferDesc.canHaveUAVs = true;
        bufferDesc.cpuAccess = nvrhi::CpuAccessMode::None;
        bufferDesc.maxVersions = caustica::c_MaxRenderPassConstantBufferVersions;
        bufferDesc.structStride = sizeof(PathPayload);
        bufferDesc.keepInitialState = true;
        bufferDesc.initialState = nvrhi::ResourceStates::Common;
        bufferDesc.debugName = "DebugDeltaPathTreeSearchStack";
        m_debugDeltaPathTreeSearchStack = device->createBuffer(bufferDesc);
    }

    m_constantBuffer = device->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(
        sizeof(SampleConstants), "SampleConstants", caustica::c_MaxRenderPassConstantBufferVersions * 2));

    m_commandList = device->createCommandList();
}

bool caustica::render::WorldRenderer::createPTPipeline()
{
    std::vector<caustica::ShaderMacro> shaderMacros;
    m_exportVBufferCS = m_context.shaderFactory->createShader(
        "caustica/shaders/render/processingPasses/ExportVisibilityBuffer.hlsl", "main", &shaderMacros, nvrhi::ShaderType::Compute);
    nvrhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.bindingLayouts = { m_bindingLayout, m_bindlessLayout };
    pipelineDesc.CS = m_exportVBufferCS;
    m_exportVBufferPSO = device()->createComputePipeline(pipelineDesc);
    return true;
}

void caustica::render::WorldRenderer::onSceneUnloading()
{
#if CAUSTICA_WITH_STREAMLINE
    if (!m_context.gpuDevice.isHeadless())
    {
        auto& streamline = m_context.gpuDevice.getStreamline();
        if (streamline.isDLSSRRAvailable())
            streamline.cleanupDLSSRR(false);
        if (streamline.isDLSSAvailable())
            streamline.cleanupDLSS(false);
        if (streamline.isDLSSGAvailable())
            streamline.cleanupDLSSG(false);
    }

    m_recommendedDLSSSettings = {};
    m_lastDLSSRROptions = {};
    m_context.activeSettings().DLSSFGOptions = {};
    m_context.activeSettings().DLSSFGMultiplier = 1;
    m_context.activeSettings().DLSSFGMaxNumFramesToGenerate = 1;
#endif
    m_context.activeSettings().DLSSMode = PathTracerSettings::DLSSModeDefault;
    m_context.activeSettings().DLSSLastMode = SI::DLSSMode::eOff;
    m_context.activeSettings().DLSSLastDisplaySize = { 0, 0 };
    m_context.activeSettings().DLSSLastRealtimeAA = 0;
    m_lastScheduledRealtimeAA = -1;

    m_bindingSet = nullptr;
    m_context.bindingCache.clear();
    if (m_commandList)
        m_commandList = device()->createCommandList();
    m_gaussianSplatTemporalReset = true;
    m_gaussianSplatEmissionProxies.clear();
    if (m_rtxdiPass != nullptr)
        m_rtxdiPass->reset();
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

void caustica::render::WorldRenderer::onSceneLoaded()
{
    resetFrameIndex();
    m_accumulationSampleIndex = 0;
    m_sampleIndex = 0;
    m_gaussianSplatTemporalReset = true;
    m_context.activeSettings().ResetAccumulation = true;
    m_context.activeSettings().ResetRealtimeCaches = true;
    m_context.activeSettings().DLSSMode = PathTracerSettings::DLSSModeDefault;
    m_context.activeSettings().DLSSLastMode = SI::DLSSMode::eOff;
    m_context.activeSettings().DLSSLastDisplaySize = { 0, 0 };
    m_context.activeSettings().DLSSLastRealtimeAA = 0;
    // Realtime->Realtime switches do not flip RealtimeMode; nudge so framePassSetup resets temporal state.
    m_lastRealtimeMode = !m_context.activeSettings().RealtimeMode;
    m_lastScheduledRealtimeAA = -1;

    if (m_rtxdiPass)
        m_rtxdiPass->reset();
}

void caustica::render::WorldRenderer::onBackBufferResizing()
{
    device()->waitForIdle();
    device()->runGarbageCollection();
    m_context.bindingCache.clear();
    m_renderTargets = nullptr;
    m_linesPipeline = nullptr; // the pipeline is based on the framebuffer so needs a reset
    for (int i=0; i < std::size(m_nrd); i++ )
        m_nrd[i] = nullptr;
    if (m_rtxdiPass)
        m_rtxdiPass->reset();

// NOTE: we're not yet sure if this is necessary to avoid crash with going in/out of fullscreen and FG
#if CAUSTICA_WITH_STREAMLINE
    if (!m_context.gpuDevice.isHeadless() &&
        (m_context.activeSettings().DLSSFGOptions.mode == StreamlineInterface::DLSSGMode::eOn || m_context.activeSettings().actualDLSSFGMode() == StreamlineInterface::DLSSGMode::eOn)) 
    {
        m_context.gpuDevice.getStreamline().cleanupDLSS(false);
        m_context.gpuDevice.getStreamline().cleanupDLSSG(false);

        if (m_context.gpuDevice.getStreamline().isDLSSGAvailable())
        {
            auto dlssgOptions = StreamlineInterface::DLSSGOptions{};
            StreamlineInterface::DLSSGState state;
            m_context.gpuDevice.getStreamline().getDLSSGState(state, dlssgOptions);
            m_context.activeSettings().DLSSFGMultiplier = state.numFramesActuallyPresented;
            m_context.activeSettings().DLSSFGMaxNumFramesToGenerate = state.numFramesToGenerateMax;

            m_context.gpuDevice.getStreamline().setDLSSGOptions(dlssgOptions);
            m_context.activeSettings().DLSSFGOptions = dlssgOptions;
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

void caustica::render::WorldRenderer::createRenderPasses( bool& exposureResetRequired, nvrhi::CommandListHandle initializeCommandList )
{
    m_context.bindingCache.clear();

    const uint2 screenResolution = {m_renderTargets->outputColor->getDesc().width, m_renderTargets->outputColor->getDesc().height};

    m_shaderDebug = std::make_shared<ShaderDebug>(device(), initializeCommandList, m_context.shaderFactory, m_context.renderDevice);

    if (m_context.activeSettings().actualUseRTXDIPasses())
        m_rtxdiPass = std::make_unique<RtxdiPass>(device(), m_context.shaderFactory, m_context.renderDevice, m_bindlessLayout);
    else
        m_rtxdiPass = nullptr;

    m_accumulationPass = std::make_unique<AccumulationPass>(device(), m_context.shaderFactory);
    m_accumulationPass->createPipeline();
    m_accumulationPass->createBindingSet(m_renderTargets->outputColor, m_renderTargets->accumulatedRadiance, m_renderTargets->processedOutputColor);

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

        m_gaussianSplatAccumulationPass = std::make_unique<AccumulationPass>(device(), m_context.shaderFactory);
        m_gaussianSplatAccumulationPass->createPipeline();
        m_gaussianSplatAccumulationPass->createBindingSet(m_gaussianSplatCurrentColor, m_gaussianSplatAccumulatedColor, m_renderTargets->processedOutputColor);
        m_gaussianSplatTemporalReset = true;
    }

    // these get re-created every time intentionally, to pick up changes after at-runtime shader recompile
    m_toneMappingPass = std::make_unique<ToneMappingPass>(device(), m_context.shaderFactory, m_context.renderDevice, m_renderTargets->ldrFramebuffer, *m_context.camera.view(), m_renderTargets->outputColor);
    m_bloomPass = std::make_unique<BloomPass>(device(), m_context.shaderFactory, m_context.renderDevice, m_renderTargets->processedOutputFramebuffer, *m_context.camera.view());
    m_postProcess = std::make_shared<PostProcess>(device(), m_context.shaderFactory, m_context.renderDevice, m_shaderDebug);

    {
        TemporalAntiAliasingPass::CreateParameters taaParams;
        taaParams.sourceDepth = m_renderTargets->depth;
        taaParams.motionVectors = m_renderTargets->screenMotionVectors;
        taaParams.unresolvedColor = m_renderTargets->outputColor;
        taaParams.resolvedColor = m_renderTargets->processedOutputColor;
        taaParams.feedback1 = m_renderTargets->temporalFeedback1;
        taaParams.feedback2 = m_renderTargets->temporalFeedback2;
        taaParams.historyClampRelax = m_renderTargets->combinedHistoryClampRelax;
        taaParams.motionVectorStencilMask = 0; ///*uint32_t motionVectorStencilMask =*/ 0x01;
        taaParams.useCatmullRomFilter = true;

        m_temporalAntiAliasingPass = std::make_unique<TemporalAntiAliasingPass>(device(), m_context.shaderFactory, m_context.renderDevice, *m_context.camera.view(), taaParams);
    }

    if (!createPTPipeline())
        { assert(false); }

    if (m_context.scenePasses.lighting.environment() == nullptr)
        m_context.scenePasses.lighting.environment() = std::make_shared<EnvMapProcessor>(device(), m_context.textureCache, false);
    if (m_context.scenePasses.lighting.lightSampling() == nullptr)
        m_context.scenePasses.lighting.lightSampling() = std::make_shared<LightSamplingCache>(device());
    m_context.scenePasses.lighting.environment()->createRenderPasses(m_shaderDebug, m_context.shaderFactory, m_context.scenePasses.lighting.computePipelines());
    m_context.scenePasses.lighting.environment()->generateBRDFLUT(initializeCommandList.Get(), m_context.bindingCache);  // One-time BRDF LUT generation
    m_context.scenePasses.lighting.lightSampling()->createRenderPasses(m_context.shaderFactory, m_bindlessLayout, m_context.renderDevice, m_shaderDebug, screenResolution, m_context.scenePasses.lighting.environment()->getImportanceSampling()->getImportanceMapResolution());

    prepareGaussianSplatPasses();

    m_denoisingGuidesPass = std::make_shared<DenoisingGuidesPass>(device(), m_context.shaderFactory, m_renderTargets, m_shaderDebug, m_bindingLayout);
}
void caustica::render::WorldRenderer::preUpdateLighting(nvrhi::CommandListHandle commandList, bool& needNewBindings)
{
    std::filesystem::path sceneDirectory;
    if (m_context.sceneManager.getCurrentScenePath() != std::filesystem::path(SceneManager::inlineSceneSentinel()))
        sceneDirectory = m_context.sceneManager.getCurrentScenePath().parent_path();

    std::string envMapActualPath = m_context.scenePasses.lighting.envMapLocalPath();
    if (m_context.scenePasses.lighting.envMapOverride() != "" && m_context.scenePasses.lighting.envMapOverride() != c_EnvMapSceneDefault)
        envMapActualPath = (isProceduralSky(m_context.scenePasses.lighting.envMapOverride().c_str())) ? (m_context.scenePasses.lighting.envMapOverride()) : (std::string(c_EnvMapSubFolder) + "/" + m_context.scenePasses.lighting.envMapOverride());

    if (!envMapActualPath.empty() && !isProceduralSky(envMapActualPath.c_str()))
        envMapActualPath = resolveSceneMediaPath(envMapActualPath, sceneDirectory).generic_string();

    PreUpdateLightingParams params{
        commandList,
        needNewBindings,
        m_context.scenePasses.lighting.environment().get(),
        m_context.renderDevice,
        envMapActualPath,
        sceneDirectory,
    };
    caustica::preUpdateLighting(params);
}
void caustica::render::WorldRenderer::updateLighting(nvrhi::CommandListHandle commandList)
{
    buildGaussianSplatEmissionProxies();

    UpdateLightingParams params{
        m_context.activeSettings(),
        commandList,
        m_context.scenePasses.lighting.environment().get(),
        m_context.scenePasses.lighting.lightSampling().get(),
        &m_context.bindingCache,
        m_context.renderDevice,
        m_context.sceneManager.getScene(),
        m_context.scenePasses.lighting.materials(),
        m_context.scenePasses.lighting.opacityMaps(),
        m_context.scenePasses.lighting.envMapSceneParams(),
        m_context.sceneTime,
        m_frameIndex,
        c_envMapRadianceScale,
    };
    if (!m_gaussianSplatEmissionProxies.empty())
        params.gaussianSplatEmissionProxies = &m_gaussianSplatEmissionProxies;
    caustica::updateLighting(m_context.camera, m_context.accelStructs, params);
}
void caustica::render::WorldRenderer::preUpdatePathTracing( bool resetAccum, nvrhi::CommandListHandle commandList )
{
    const bool resetReferenceOidn = !m_context.activeSettings().RealtimeMode && (resetAccum || m_context.activeSettings().ResetAccumulation || m_context.activeSettings().ReferenceOIDNDenoiserChanged);
    if (resetReferenceOidn || m_context.activeSettings().ReferenceOIDNDenoiserChanged)
    {
        resetReferenceOIDN();
        m_context.activeSettings().ReferenceOIDNDenoiserChanged = false;
    }

    resetAccum |= m_context.activeSettings().ResetAccumulation;
    resetAccum |= m_context.activeSettings().RealtimeMode;

    if (resetAccum)
    {
        m_accumulationSampleIndex = (m_context.activeSettings().AccumulationPreWarmRealtimeCaches)?(-32):(0);
    }
#if ENABLE_DEBUG_VIZUALISATIONS
    if (resetAccum && m_shaderDebug)
        m_shaderDebug->clearDebugVizTexture(commandList);
#endif

    // profile perf - only makes sense with high accumulation sample counts; only start counting after n-th after it stabilizes
    if( m_accumulationSampleIndex < 16 )
    {
        m_context.diagnostics.benchStart = std::chrono::high_resolution_clock::now( );
        m_context.diagnostics.benchLast = m_context.diagnostics.benchStart;
        m_context.diagnostics.benchFrames = 0;
    } else if( m_accumulationSampleIndex < m_context.activeSettings().AccumulationTarget )
    {
        m_context.diagnostics.benchFrames++;
        m_context.diagnostics.benchLast = std::chrono::high_resolution_clock::now( );
    }
    m_accumulationCompleted = false;
    // 'min' in non-realtime path here is to keep looping the last sample for debugging purposes!
    if( !m_context.activeSettings().RealtimeMode )
    {
        m_sampleIndex = (m_accumulationSampleIndex<0)?(m_accumulationSampleIndex+4096):(min(m_accumulationSampleIndex, m_context.activeSettings().AccumulationTarget - 1));
        m_accumulationCompleted |= m_accumulationSampleIndex == m_context.activeSettings().AccumulationTarget - 1;
    }
    else
        m_sampleIndex = (!m_context.activeSettings().DbgFreezeRealtimeNoiseSeed)?( m_frameIndex % 8192 ):0;     // actual sample index
}
void caustica::render::WorldRenderer::postUpdatePathTracing( )
{
    m_accumulationSampleIndex = std::min( m_accumulationSampleIndex+1, m_context.activeSettings().AccumulationTarget );

    if (m_context.activeSettings().actualUseRTXDIPasses())
        m_rtxdiPass->endFrame();

    m_context.activeSettings().ResetAccumulation = false;
    m_context.activeSettings().ResetRealtimeCaches = false;
    m_frameIndex++;
}
void caustica::render::WorldRenderer::updatePathTracerConstants( PathTracerConstants & constants, const PathTracerCameraData & cameraData )
{
#if CAUSTICA_STOCHASTIC_TEXTURE_FILTERING_ENABLE
    auto GetStfMagnificationMethod = [](StfMagnificationMethod method)->int {
        switch (method)
        {
        case StfMagnificationMethod::Default:   return STF_MAGNIFICATION_METHOD_NONE;
        case StfMagnificationMethod::Quad2x2:   return STF_MAGNIFICATION_METHOD_2x2_QUAD;
        case StfMagnificationMethod::Fine2x2:   return STF_MAGNIFICATION_METHOD_2x2_FINE;
        case StfMagnificationMethod::FineTemporal2x2: return STF_MAGNIFICATION_METHOD_2x2_FINE_TEMPORAL;
        case StfMagnificationMethod::FineAlu3x3: return STF_MAGNIFICATION_METHOD_3x3_FINE_ALU;
        case StfMagnificationMethod::FineLut3x3: return STF_MAGNIFICATION_METHOD_3x3_FINE_LUT;
        case StfMagnificationMethod::Fine4x4:    return STF_MAGNIFICATION_METHOD_4x4_FINE;
        default:
            assert(!"Not Implemented");
            return 0;
        }
    };

    auto GetStfFilterMode = [](StfFilterMode mode)->int {
        switch (mode)
        {
        case StfFilterMode::Point:      return STF_FILTER_TYPE_POINT;
        case StfFilterMode::Linear:     return STF_FILTER_TYPE_LINEAR;
        case StfFilterMode::Cubic:      return STF_FILTER_TYPE_CUBIC;
        case StfFilterMode::Gaussian:   return STF_FILTER_TYPE_GAUSSIAN;
        default:
            assert(!"Not Implemented");
            return 0;
        }
    };
#endif // CAUSTICA_STOCHASTIC_TEXTURE_FILTERING_ENABLE

    constants.camera = cameraData;
    constants.prevCamera = cameraData;
    if (m_context.camera.viewPrevious())
        constants.prevCamera.PosW = m_context.camera.viewPrevious()->getInverseViewMatrix().m_translation;

    constants.bounceCount = m_context.activeSettings().BounceCount;
    constants.diffuseBounceCount = m_context.activeSettings().DiffuseBounceCount;
    constants.perPixelJitterAAScale = (m_context.activeSettings().RealtimeMode == false && m_context.activeSettings().AccumulationAA)?(1):( (m_context.activeSettings().RealtimeMode && m_context.activeSettings().RealtimeAA == 3)?(m_context.activeSettings().DLSSRRMicroJitter):(0.0f) );

    // needed to allow super-resolution to work best
    float dlssBias = -dm::log2f(sqrtf((m_displaySize.x * m_displaySize.y) / float(m_renderSize.x * m_renderSize.y)));

    constants.texLODBias = m_context.activeSettings().TexLODBias + dlssBias;
    constants.sampleBaseIndex = m_sampleIndex * m_context.activeSettings().actualSamplesPerPixel();

    //constants.subSampleCount = m_context.activeSettings().actualSamplesPerPixel();
    constants.invSubSampleCount = 1.0f / (float)m_context.activeSettings().actualSamplesPerPixel();

    constants.imageWidth = m_renderSize.x;
    constants.imageHeight = m_renderSize.y;
    if (m_renderTargets != nullptr)
    {
        assert(m_renderSize.x == m_renderTargets->outputColor->getDesc().width);
        assert(m_renderSize.y == m_renderTargets->outputColor->getDesc().height);
    }

    if (m_context.activeSettings().EnableToneMapping && m_toneMappingPass != nullptr)
        constants.preExposedGrayLuminance = dm::luminance(m_toneMappingPass->getPreExposedGray(0));
    else
        constants.preExposedGrayLuminance = 1.0f;

    const float disabledFF = 0.0f;
    if (m_context.activeSettings().RealtimeMode)
        constants.fireflyFilterThreshold = (m_context.activeSettings().RealtimeFireflyFilterEnabled)?(m_context.activeSettings().RealtimeFireflyFilterThreshold*sqrtf(constants.preExposedGrayLuminance)*1e3f):(disabledFF); // it does make sense to make the realtime variant dependent on avg luminance - just didn't have time to try it out yet
    else
        constants.fireflyFilterThreshold = (m_context.activeSettings().ReferenceFireflyFilterEnabled)?(m_context.activeSettings().ReferenceFireflyFilterThreshold*sqrtf(constants.preExposedGrayLuminance)*1e3f):(disabledFF); // making it exposure-adaptive breaks determinism with accumulation (because there's a feedback loop), so that's disabled
    constants.useReSTIRDI = m_context.activeSettings().actualUseReSTIRDI();
    constants.useReSTIRGI = m_context.activeSettings().actualUseReSTIRGI();
    constants.useReSTIRPT = m_context.activeSettings().actualUseReSTIRPT();
    constants.environmentMapVisibleToCamera = m_context.activeSettings().EnvironmentMapParams.VisibleToCamera ? 1u : 0u;
    constants.denoiserRadianceClampK = m_context.activeSettings().DenoiserRadianceClampK;
    constants.DLSSRRBrightnessClampK = (m_context.activeSettings().DLSSRRBrightnessClampK>0)?(m_context.activeSettings().DLSSRRBrightnessClampK * constants.preExposedGrayLuminance):(0.0f);

    // no stable planes by default
    constants.denoisingEnabled = m_context.activeSettings().actualUseStandaloneDenoiser() || m_context.activeSettings().RealtimeAA == 3;

    constants._activeStablePlaneCount           = m_context.activeSettings().StablePlanesActiveCount;
    constants.maxStablePlaneVertexDepth         = std::min( std::min( (uint)m_context.activeSettings().StablePlanesMaxVertexDepth, cStablePlaneMaxVertexIndex ), (uint)m_context.activeSettings().BounceCount );
    constants.allowPrimarySurfaceReplacement    = m_context.activeSettings().AllowPrimarySurfaceReplacement;
    constants.stablePlanesSplitStopThreshold    = m_context.activeSettings().StablePlanesSplitStopThreshold;
    constants._padding3                         = 0;
    constants.stablePlanesSuppressPrimaryIndirectSpecularK  = m_context.activeSettings().StablePlanesSuppressPrimaryIndirectSpecular?m_context.activeSettings().StablePlanesSuppressPrimaryIndirectSpecularK:0.0f;
    constants.stablePlanesAntiAliasingFallthrough = m_context.activeSettings().StablePlanesAntiAliasingFallthrough;
    constants.frameIndex                        = m_frameIndex & 0xFFFFFFFF; //m_context.gpuDevice.getFrameIndex();
    constants.genericTSLineStride               = GenericTSComputeLineStride(constants.imageWidth, constants.imageHeight);
    constants.genericTSPlaneStride              = GenericTSComputePlaneStride(constants.imageWidth, constants.imageHeight);

    constants.NEEEnabled                        = m_context.activeSettings().UseNEE;
    constants.NEEType                           = m_context.activeSettings().NEEType;
    constants.NEECandidateSamples               = m_context.activeSettings().NEECandidateSamples;
    constants.NEEFullSamples                    = m_context.activeSettings().NEEFullSamples;

    constants.EnvironmentMapDiffuseSampleMIPLevel = m_context.activeSettings().EnvironmentMapDiffuseSampleMIPLevel;

#if CAUSTICA_STOCHASTIC_TEXTURE_FILTERING_ENABLE
    // stochastic texture filtering type and size.
    // constants.STFUseBlueNoise                   = m_context.activeSettings().STFUseBlueNoise;
    constants.STFMagnificationMethod            = GetStfMagnificationMethod(m_context.activeSettings().STFMagnificationMethod);
    constants.STFFilterMode                     = GetStfFilterMode(m_context.activeSettings().STFFilterMode);
    constants.STFGaussianSigma                  = m_context.activeSettings().STFGaussianSigma;
#endif
}
void caustica::render::WorldRenderer::rtxdiSetupFrame(nvrhi::IFramebuffer* framebuffer, PathTracerCameraData cameraData, uint2 renderDims)
{
    const bool envMapPresent = m_context.activeSettings().EnvironmentMapParams.enabled;

    RtxdiBridgeParameters bridgeParameters;
	bridgeParameters.frameIndex = m_frameIndex & 0xFFFFFFFF;
	bridgeParameters.frameDims = renderDims;
	bridgeParameters.cameraPosition = m_context.camera.camera().getPosition();
	bridgeParameters.userSettings = m_context.activeSettings().RTXDI;
    bridgeParameters.usingLightSampling = m_context.activeSettings().actualUseReSTIRDI();
    bridgeParameters.usingReGIR = m_context.activeSettings().actualUseReSTIRDI();

    bridgeParameters.userSettings.restirDI.initialSamplingParams.environmentMapImportanceSampling = envMapPresent;

    buildGaussianSplatEmissionProxies();
    if (!m_gaussianSplatEmissionProxies.empty() && isGaussianSplatEmissionEnabled(m_context.activeSettings()))
    {
        bridgeParameters.gaussianSplatEmissionProxies = &m_gaussianSplatEmissionProxies;
        bridgeParameters.gaussianSplatEmissionObjectToWorld = float4x4::identity();
        bridgeParameters.gaussianSplatEmissionIntensity = m_context.activeSettings().GaussianSplatEmissionIntensity;
    }

    if( m_context.activeSettings().ResetRealtimeCaches )
        m_rtxdiPass->reset();

	m_rtxdiPass->prepareResources(m_commandList, *m_renderTargets, envMapPresent ? m_context.scenePasses.lighting.environment() : nullptr, m_context.scenePasses.lighting.envMapSceneParams(),
        m_context.sceneManager.getScene(), m_context.scenePasses.lighting.materials(), m_context.scenePasses.lighting.opacityMaps(), m_context.accelStructs.getSubInstanceBuffer(), bridgeParameters, m_bindingLayout, m_shaderDebug );
 }
#if CAUSTICA_WITH_STREAMLINE
void caustica::render::WorldRenderer::streamlinePreRender()
{
#if CAUSTICA_WITH_STREAMLINE
    if (m_context.gpuDevice.isHeadless())
        return;

    auto& streamline = m_context.gpuDevice.getStreamline();
    m_context.activeSettings().IsDLSSSuported = streamline.isDLSSAvailable();
    m_context.activeSettings().IsDLSSRRSupported = streamline.isDLSSRRAvailable();
    m_context.activeSettings().IsDLSSFGSupported = streamline.isDLSSGAvailable();
    m_context.activeSettings().IsReflexSupported = streamline.isReflexAvailable();

    // Setup Reflex
    {
        auto reflexConsts = caustica::StreamlineInterface::ReflexOptions{};
        reflexConsts.mode = (caustica::StreamlineInterface::ReflexMode) m_context.activeSettings().actualReflexMode();
        reflexConsts.frameLimitUs = m_context.activeSettings().ReflexCappedFps == 0 ? 0 : int(1000000. / m_context.activeSettings().ReflexCappedFps);
        reflexConsts.useMarkersToOptimize = true;
        reflexConsts.virtualKey = VK_F13;
        reflexConsts.idThread = 0; // std::hash<std::thread::id>()(std::this_thread::get_id())
        streamline.setReflexConsts(reflexConsts);

        // Need to update StreamlineIntegration with the ability to query reflex state
        caustica::StreamlineInterface::ReflexState reflexState{};
        streamline.getReflexState(reflexState);
        if (m_context.activeSettings().IsReflexSupported)
        {
            m_context.activeSettings().IsReflexLowLatencyAvailable = reflexState.lowLatencyAvailable;
            m_context.activeSettings().IsReflexFlashIndicatorDriverControlled = reflexState.flashIndicatorDriverControlled;

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

                m_context.activeSettings().ReflexStats = "frameID: " + std::to_string(frameID);
                m_context.activeSettings().ReflexStats += "\ntotalGameToRenderLatencyUs: " + std::to_string(totalGameToRenderLatencyUs);
                m_context.activeSettings().ReflexStats += "\nsimDeltaUs: " + std::to_string(simDeltaUs);
                m_context.activeSettings().ReflexStats += "\nrenderDeltaUs: " + std::to_string(renderDeltaUs);
                m_context.activeSettings().ReflexStats += "\npresentDeltaUs: " + std::to_string(presentDeltaUs);
                m_context.activeSettings().ReflexStats += "\ndriverDeltaUs: " + std::to_string(driverDeltaUs);
                m_context.activeSettings().ReflexStats += "\nosRenderQueueDeltaUs: " + std::to_string(osRenderQueueDeltaUs);
                m_context.activeSettings().ReflexStats += "\ngpuRenderDeltaUs: " + std::to_string(gpuRenderDeltaUs);
            }
            else
            {
                m_context.activeSettings().ReflexStats = "Latency Report Unavailable";
            }
        }
    }

    // DLSS-G Setup
    {
        const auto actualDLSSFGMode = m_context.activeSettings().actualDLSSFGMode();
        const bool wasDLSSFGEnabled = m_context.activeSettings().DLSSFGOptions.mode == StreamlineInterface::DLSSGMode::eOn;

        // If DLSS-G has been turned off, then we tell tell SL to clean it up expressly
        if (wasDLSSFGEnabled && actualDLSSFGMode == StreamlineInterface::DLSSGMode::eOff) {
            streamline.cleanupDLSSG(true);
        }

        // This is where DLSS-G is toggled On and Off (using dlssgOptions.mode) and where we set DLSS-G parameters.
        auto dlssgOptions = StreamlineInterface::DLSSGOptions{};
        dlssgOptions.mode = actualDLSSFGMode;
        dlssgOptions.numFramesToGenerate = m_context.activeSettings().DLSSFGNumFramesToGenerate;

        // This is where we query DLSS-G minimum swapchain size
        if (m_context.activeSettings().IsDLSSFGSupported &&
            (actualDLSSFGMode != StreamlineInterface::DLSSGMode::eOff || wasDLSSFGEnabled))
        {
            StreamlineInterface::DLSSGState state;
            streamline.getDLSSGState(state, dlssgOptions);
            m_context.activeSettings().DLSSFGMultiplier = state.numFramesActuallyPresented;
            m_context.activeSettings().DLSSFGMaxNumFramesToGenerate = state.numFramesToGenerateMax;

            streamline.setDLSSGOptions(dlssgOptions);
            m_context.activeSettings().DLSSFGOptions = dlssgOptions;
        }
        else
        {
            m_context.activeSettings().DLSSFGMultiplier = 1;
            m_context.activeSettings().DLSSFGOptions = dlssgOptions;
        }
    }

    // Ensure DLSS / DLSS-RR is available
    if (m_context.activeSettings().RealtimeAA == 3 && !m_context.activeSettings().IsDLSSRRSupported)
    {
        caustica::warning("Requested DLSS-RR mode not available. Switching to DLSS. ");
        m_context.activeSettings().RealtimeAA = 2;
    }
    if ( m_context.activeSettings().RealtimeAA == 2 && !m_context.activeSettings().IsDLSSSuported )
    {
        caustica::warning("Requested DLSS mode not available. Switching to TAA. ");
        m_context.activeSettings().RealtimeAA = 1;
    }

    // Setup DLSS
    const bool changeToDLSSMode = (m_context.activeSettings().RealtimeAA >= 2 && m_context.activeSettings().RealtimeAA <= 3) && m_context.activeSettings().DLSSLastRealtimeAA != m_context.activeSettings().RealtimeAA;
    {
        // reset DLSS vars if we stop using it
        if (changeToDLSSMode || m_context.activeSettings().DLSSMode == StreamlineInterface::DLSSMode::eOff)
        {
            m_context.activeSettings().DLSSLastMode = PathTracerSettings::DLSSModeDefault;
            m_context.activeSettings().DLSSMode = PathTracerSettings::DLSSModeDefault;
            m_context.activeSettings().DLSSLastDisplaySize = { 0,0 };
        }

        m_context.activeSettings().DLSSLastRealtimeAA = m_context.activeSettings().RealtimeAA;

        // If we are using DLSS set its constants
        if ((m_context.activeSettings().RealtimeAA == 2 || m_context.activeSettings().RealtimeAA == 3) && m_context.activeSettings().RealtimeMode)
        {
            StreamlineInterface::DLSSOptions dlssOptions = {};
            if (m_context.activeSettings().IsDLSSSuported)
            {
                dlssOptions.mode = m_context.activeSettings().DLSSMode;
                dlssOptions.outputWidth = m_displaySize.x;
                dlssOptions.outputHeight = m_displaySize.y;
                dlssOptions.sharpness = 0; //m_recommendedDLSSSettings.sharpness;    // <- is this no longer valid?
                dlssOptions.colorBuffersHDR = true;
                dlssOptions.useAutoExposure = true;     // Optional: provide proper "kBufferTypeExposure" for 0-lag for better precision handling
                dlssOptions.preset = StreamlineInterface::DLSSPreset::eDefault;
                // if (m_context.activeSettings().RealtimeAA < 4) <- docs https://github.com/NVIDIAGameWorks/Streamline/blob/main/docs/ProgrammingGuideDLSS_RR.md#50-provide-dlss--dlss-rr-options seem to imply that these should be set even when DLSS-RR enabled
                    streamline.setDLSSOptions(dlssOptions);
            }
            else
            {
                assert( false ); // shouldn't happen, code above should have dropped us to "m_context.activeSettings().RealtimeAA = 1" - check for recent code changes.
            }

            if (m_context.activeSettings().RealtimeAA == 2 || m_context.activeSettings().RealtimeAA == 3)
            {
                // Check if we need to update the rendertarget size.
                bool dlssResizeRequired = (m_context.activeSettings().DLSSMode != m_context.activeSettings().DLSSLastMode) || (m_displaySize.x != m_context.activeSettings().DLSSLastDisplaySize.x) || (m_displaySize.y != m_context.activeSettings().DLSSLastDisplaySize.y);
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
                        m_context.activeSettings().RealtimeAA = 0;
                        m_context.activeSettings().DLSSMode = PathTracerSettings::DLSSModeDefault;
                        m_renderSize = m_displaySize;
                    }
                    else
                    {
                        m_context.activeSettings().DLSSLastMode = m_context.activeSettings().DLSSMode;
                        m_context.activeSettings().DLSSLastDisplaySize = m_displaySize;
                    }
                }

                m_renderSize = (uint2)m_recommendedDLSSSettings.optimalRenderSize;
            }

            if (m_context.activeSettings().RealtimeAA == 3) // DLSS-RR
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
                dlssRROptions.preset                = m_context.activeSettings().DLSRRPreset;
                m_lastDLSSRROptions = dlssRROptions; // we need to fill them up with view info, but we can only have proper view after it was initialized with correct RT size
            }
        }
        else
        {
            if (m_context.activeSettings().IsDLSSSuported)
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
    if (!m_context.activeSettings().RealtimeMode)
    {
        m_renderSize = m_displaySize;
        return;
    }

    if (m_nativeDLSS)
    {
        m_context.activeSettings().IsDLSSSuported = m_nativeDLSS->isDlssSupported();
        m_context.activeSettings().IsDLSSRRSupported = m_nativeDLSS->isRayReconstructionSupported();
    }

    if (m_context.activeSettings().RealtimeAA == 3 && !m_context.activeSettings().IsDLSSRRSupported)
    {
        caustica::warning("Requested DLSS-RR mode not available. Switching to DLSS.");
        m_context.activeSettings().RealtimeAA = 2;
    }

    if (m_context.activeSettings().RealtimeAA == 2 && !m_context.activeSettings().IsDLSSSuported)
    {
        caustica::warning("Requested DLSS mode not available. Switching to TAA.");
        m_context.activeSettings().RealtimeAA = 1;
    }

    const bool usingDLSS = (m_context.activeSettings().RealtimeAA == 2 || m_context.activeSettings().RealtimeAA == 3);
    const bool changeToDLSSMode = usingDLSS && m_context.activeSettings().DLSSLastRealtimeAA != m_context.activeSettings().RealtimeAA;

    if (changeToDLSSMode || m_context.activeSettings().DLSSMode == SI::DLSSMode::eOff)
    {
        m_context.activeSettings().DLSSLastMode = PathTracerSettings::DLSSModeDefault;
        m_context.activeSettings().DLSSMode = PathTracerSettings::DLSSModeDefault;
        m_context.activeSettings().DLSSLastDisplaySize = { 0, 0 };
    }

    m_context.activeSettings().DLSSLastRealtimeAA = m_context.activeSettings().RealtimeAA;

    if (usingDLSS)
    {
        const bool dlssResizeRequired =
            (m_context.activeSettings().DLSSMode != m_context.activeSettings().DLSSLastMode) ||
            (m_displaySize.x != m_context.activeSettings().DLSSLastDisplaySize.x) ||
            (m_displaySize.y != m_context.activeSettings().DLSSLastDisplaySize.y);

        if (dlssResizeRequired)
        {
            m_context.activeSettings().DLSSLastMode = m_context.activeSettings().DLSSMode;
            m_context.activeSettings().DLSSLastDisplaySize = m_displaySize;
        }

        m_renderSize = GetNativeDLSSRenderSize(m_displaySize, m_context.activeSettings().DLSSMode);
    }
    else
    {
        m_renderSize = m_displaySize;
    }
}
#endif
void caustica::render::WorldRenderer::preRender()
{
    // Limit FPS
    if (m_context.activeSettings().actualFPSLimiter() > 0)
        g_FPSLimiter.framerateLimit(m_context.activeSettings().actualFPSLimiter());

    korgi::update();
}

void caustica::render::WorldRenderer::prepareGaussianSplatPasses()
{
    GaussianSplatPrepareContext context;
    context.device = device();
    context.shaderFactory = m_context.shaderFactory;
    context.renderTargets = m_renderTargets.get();
    context.shaderDebug = m_shaderDebug;
    context.gpuSort = m_gaussianSplatGpuSort;
    prepareGaussianSplatScenePasses(m_context.scenePasses.gaussianSplats, context);
    m_gaussianSplatGpuSort = context.gpuSort;
}

void caustica::render::WorldRenderer::buildGaussianSplatEmissionProxies()
{
    const std::shared_ptr<Scene> scene = m_context.sceneManager.getScene();
    const std::span<const caustica::scene::GaussianSplatRenderProxy> gaussianSplats =
        scene ? std::span<const caustica::scene::GaussianSplatRenderProxy>(scene->getRenderData().gaussianSplats)
              : std::span<const caustica::scene::GaussianSplatRenderProxy>();
    caustica::render::buildGaussianSplatEmissionProxies(
        m_gaussianSplatEmissionProxies,
        gaussianSplats,
        m_context.activeSettings());
}

void caustica::render::WorldRenderer::executeGaussianSplatAccelBuild(nvrhi::ICommandList* commandList)
{
    if (commandList == nullptr || !hasActiveGaussianSplats())
        return;

    const std::shared_ptr<Scene> scene = m_context.sceneManager.getScene();
    const std::span<const caustica::scene::GaussianSplatRenderProxy> gaussianSplats =
        scene ? std::span<const caustica::scene::GaussianSplatRenderProxy>(scene->getRenderData().gaussianSplats)
              : std::span<const caustica::scene::GaussianSplatRenderProxy>();
    buildGaussianSplatSceneAccelStructs(commandList, gaussianSplats, m_context.activeSettings());
}

bool caustica::render::WorldRenderer::hasActiveGaussianSplats() const
{
    const std::shared_ptr<Scene> scene = m_context.sceneManager.getScene();
    if (!scene || !m_context.activeSettings().EnableGaussianSplats)
        return false;

    const auto& gaussianSplats = scene->getRenderData().gaussianSplats;
    return std::any_of(gaussianSplats.begin(), gaussianSplats.end(), isGaussianSplatProxyActive);
}

void caustica::render::WorldRenderer::executeGaussianSplatRender(nvrhi::ICommandList* commandList, bool renderToOutputColor)
{
    m_gaussianSplatCompositeRendered = false;
    if (commandList == nullptr || !hasActiveGaussianSplats())
        return;

    const bool stochasticSplats = m_context.activeSettings().EnableGaussianSplats && m_context.activeSettings().GaussianSplatSortingMode == 1;
    if (stochasticSplats && (m_context.activeSettings().ResetAccumulation || m_context.activeSettings().ResetRealtimeCaches || m_frameGaussianSplatTemporalReset || m_gaussianSplatTemporalReset))
        m_gaussianSplatTemporalSampleIndex = 0;

    const GaussianSplatFrameInputs frameInputs{
        m_context.activeSettings(),
        int(m_frameIndex),
        int(m_sampleIndex),
        m_gaussianSplatTemporalSampleIndex,
        renderToOutputColor,
        dm::float2(float(m_displaySize.x), float(m_displaySize.y)),
        resolveGaussianSplatShadowDirection(m_context.sceneManager),
    };
    const GaussianSplatRenderSettings settings = buildGaussianSplatRenderSettings(frameInputs);

    caustica::PlanarView splatView = *m_context.camera.view();
    if (!renderToOutputColor)
    {
        splatView.setViewport(ViewportDesc(float(m_displaySize.x), float(m_displaySize.y)));
        splatView.setPixelOffset(dm::float2::zero());
    }
    splatView.updateCache();

    const std::shared_ptr<Scene> scene = m_context.sceneManager.getScene();
    const std::span<const caustica::scene::GaussianSplatRenderProxy> gaussianSplats =
        scene ? std::span<const caustica::scene::GaussianSplatRenderProxy>(scene->getRenderData().gaussianSplats)
              : std::span<const caustica::scene::GaussianSplatRenderProxy>();
    bool renderedAny = renderGaussianSplatScene(
        commandList,
        gaussianSplats,
        splatView,
        m_context.accelStructs.getTopLevelAS().Get(),
        *m_renderTargets,
        settings);
    m_gaussianSplatCompositeRendered = renderedAny && !renderToOutputColor;
}

void caustica::render::WorldRenderer::executeGaussianSplatAccumulate(nvrhi::ICommandList* commandList)
{
    if (commandList == nullptr || !m_gaussianSplatCompositeRendered)
        return;

    if (m_gaussianSplatAccumulationPass == nullptr || m_renderTargets == nullptr
        || m_gaussianSplatCurrentColor == nullptr || m_gaussianSplatAccumulatedColor == nullptr)
        return;

    if (m_context.activeSettings().ResetAccumulation || m_context.activeSettings().ResetRealtimeCaches || m_frameGaussianSplatTemporalReset || m_gaussianSplatTemporalReset)
    {
        m_gaussianSplatTemporalSampleIndex = 0;
        m_frameGaussianSplatTemporalReset = false;
        m_gaussianSplatTemporalReset = false;
    }

    const float accumulationWeight = 1.0f / float(m_gaussianSplatTemporalSampleIndex + 1);

    caustica::PlanarView splatView = *m_context.camera.view();
    splatView.setViewport(ViewportDesc(float(m_displaySize.x), float(m_displaySize.y)));
    splatView.setPixelOffset(dm::float2::zero());
    splatView.updateCache();

    commandList->setTextureState(m_renderTargets->processedOutputColor, nvrhi::AllSubresources, nvrhi::ResourceStates::CopySource);
    commandList->setTextureState(m_gaussianSplatCurrentColor, nvrhi::AllSubresources, nvrhi::ResourceStates::CopyDest);
    commandList->commitBarriers();
    commandList->copyTexture(m_gaussianSplatCurrentColor, nvrhi::TextureSlice(), m_renderTargets->processedOutputColor, nvrhi::TextureSlice());

    commandList->setTextureState(m_gaussianSplatCurrentColor, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    commandList->setTextureState(m_gaussianSplatAccumulatedColor, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    commandList->setTextureState(m_renderTargets->processedOutputColor, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    commandList->commitBarriers();

    m_gaussianSplatAccumulationPass->render(commandList, splatView, splatView, accumulationWeight);

    m_gaussianSplatTemporalSampleIndex = std::min(m_gaussianSplatTemporalSampleIndex + 1, 1024 * 1024);
    m_gaussianSplatCompositeRendered = false;
}

void caustica::render::WorldRenderer::render(nvrhi::IFramebuffer* framebuffer)
{
    m_displaySize = m_renderSize = uint2(
        framebuffer->getFramebufferInfo().width,
        framebuffer->getFramebufferInfo().height);

    const uint32_t renderPhaseFrameIndex = m_context.gpuDevice.getRenderPhaseFrameIndex();
    std::shared_ptr<Scene> scene = m_context.sceneManager.getScene();
    if (scene)
    {
        scene->beginGpuReadFrame(renderPhaseFrameIndex);
        const scene::SceneRenderData& renderData = scene->getRenderData();

        m_frameSettingsSnapshot = renderData.renderSettings.settings;
        m_frameRuntimeSnapshot.Invalidation = renderData.renderSettings.invalidation;
        m_frameRuntimeSnapshot.Picking = renderData.renderSettings.picking;
        m_frameRuntimeSnapshot.GaussianSplats = m_context.runtimeState.GaussianSplats;
        m_frameGaussianSplatTemporalReset = renderData.renderSettings.gaussianSplatTemporalReset;
        m_context.sceneTime = renderData.renderSettings.sceneTime;

        m_context.frameSettings = &m_frameSettingsSnapshot;
        m_context.frameRuntime = &m_frameRuntimeSnapshot;

        // Apply extracted camera pose before view update (RT owns view matrices).
        // Skip when snapshot has no session camera (structure-only republish / scene-load extract).
        if (renderData.camera.valid)
        {
            m_context.camera.camera().lookTo(
                renderData.camera.position, renderData.camera.direction, renderData.camera.up);
            m_context.camera.setVerticalFOV(renderData.camera.verticalFovRadians);
            m_context.camera.setZNear(renderData.camera.zNear);
            m_context.camera.setSelectedCameraIndex(renderData.camera.selectedCameraIndex);
            if (renderData.camera.useCustomIntrinsics)
            {
                m_context.camera.setIntrinsics(
                    renderData.camera.intrinsics.x,
                    renderData.camera.intrinsics.y,
                    renderData.camera.intrinsics.z,
                    renderData.camera.intrinsics.w,
                    renderData.camera.intrinsicsViewport.x,
                    renderData.camera.intrinsicsViewport.y);
            }
            else
            {
                m_context.camera.clearIntrinsics();
            }
        }
    }
    else
    {
        m_context.frameSettings = nullptr;
        m_context.frameRuntime = nullptr;
        m_frameGaussianSplatTemporalReset = false;
    }

    populateRenderFrameContext(framebuffer, m_renderFrameCtx);
    m_pipelineRegistry.runFrame(*this, m_renderFrameCtx);

    // Preserve snapshot pick flags for AfterWorldRender resolve/clear. Live
    // runtimeState.Picking can change while older frames are still in flight.
    m_lastRenderedPicking = m_context.frameRuntime
        ? m_context.frameRuntime->Picking
        : RenderPickState{};

    m_context.frameSettings = nullptr;
    m_context.frameRuntime = nullptr;

    if (scene)
        scene->endGpuReadFrame();

    if (m_renderFrameCtx.frame.aborted)
        postUpdatePathTracing();
}
void caustica::render::WorldRenderer::recreateBindingSet()
{
	// WARNING: this must match the layout of the m_bindingLayout (or switch to CreateBindingSetAndLayout)
    nvrhi::rt::IAccelStruct* gaussianSplatAS = m_context.accelStructs.getTopLevelAS();
    nvrhi::IBuffer* gaussianSplatBuffer = m_context.scenePasses.lighting.materials()->getMaterialDataBuffer();
    const std::shared_ptr<Scene> scene = m_context.sceneManager.getScene();
    const std::span<const caustica::scene::GaussianSplatRenderProxy> gaussianSplats =
        scene ? std::span<const caustica::scene::GaussianSplatRenderProxy>(scene->getRenderData().gaussianSplats)
              : std::span<const caustica::scene::GaussianSplatRenderProxy>();
    const GaussianSplatBinding primaryGaussianBinding = getPrimaryGaussianSplatBinding(gaussianSplats);
    const GaussianSplatPass* primaryGaussianSplatPass = primaryGaussianBinding.splatPass;
    if (primaryGaussianSplatPass != nullptr && primaryGaussianSplatPass->getTopLevelAS() != nullptr && primaryGaussianSplatPass->getSplatBuffer() != nullptr)
    {
        gaussianSplatAS = primaryGaussianSplatPass->getTopLevelAS();
        gaussianSplatBuffer = primaryGaussianSplatPass->getSplatBuffer();
    }

    // Fixed resources that do not change between binding sets
    nvrhi::BindingSetDesc bindingSetDescBase;
    bindingSetDescBase.bindings = {
        nvrhi::BindingSetItem::ConstantBuffer(0, m_constantBuffer),
        nvrhi::BindingSetItem::PushConstants(1, sizeof(SampleMiniConstants)),
        //nvrhi::BindingSetItem::ConstantBuffer(2, m_context.scenePasses.lighting.lightSampling()->GetLightingConstants()),
        nvrhi::BindingSetItem::RayTracingAccelStruct(0, m_context.accelStructs.getTopLevelAS()),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(1, m_context.accelStructs.getSubInstanceBuffer()),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(2, m_context.sceneManager.getScene()->getGpuResources().instanceBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(3, m_context.sceneManager.getScene()->getGpuResources().geometryBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(4, m_context.scenePasses.lighting.opacityMaps() ?(m_context.scenePasses.lighting.opacityMaps()->getGeometryDebugBuffer()):(m_context.scenePasses.lighting.materials()->getMaterialDataBuffer().Get()) ),   // YUCK
        nvrhi::BindingSetItem::StructuredBuffer_SRV(5, m_context.scenePasses.lighting.materials()->getMaterialDataBuffer()),
        nvrhi::BindingSetItem::Texture_SRV(6,  m_renderTargets->ldrColorScratch, nvrhi::Format::SRGBA8_UNORM),
        nvrhi::BindingSetItem::RayTracingAccelStruct(7, gaussianSplatAS),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(8, gaussianSplatBuffer),
        nvrhi::BindingSetItem::Texture_SRV(10, m_context.scenePasses.lighting.environment()->getEnvMapCube()),
        nvrhi::BindingSetItem::Texture_SRV(11, m_context.scenePasses.lighting.environment()->getImportanceSampling()->getImportanceMapOnly()),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(12, m_context.scenePasses.lighting.lightSampling()->getControlBuffer()),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(13, m_context.scenePasses.lighting.lightSampling()->getLightBuffer()),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(14, m_context.scenePasses.lighting.lightSampling()->getLightExBuffer()),
        nvrhi::BindingSetItem::TypedBuffer_SRV(15, m_context.scenePasses.lighting.lightSampling()->getLightProxyCounters()),     // t_tightProxyCounters
        nvrhi::BindingSetItem::TypedBuffer_SRV(16, m_context.scenePasses.lighting.lightSampling()->getLightSamplingProxies()),   // t_LightProxyIndices
        nvrhi::BindingSetItem::TypedBuffer_SRV(17, m_context.scenePasses.lighting.lightSampling()->getLocalSamplingBuffer()),    // t_LightLocalSamplingBuffer
        nvrhi::BindingSetItem::Texture_SRV(18, m_context.scenePasses.lighting.lightSampling()->getEnvLightLookupMap()),          // t_EnvLookupMap
        //nvrhi::BindingSetItem::TypedBuffer_SRV(19, ),
        nvrhi::BindingSetItem::Texture_UAV(20, m_context.scenePasses.lighting.lightSampling()->getFeedbackTotalWeight()),        // u_LightFeedbackTotalWeight
        nvrhi::BindingSetItem::Texture_UAV(21, m_context.scenePasses.lighting.lightSampling()->getFeedbackCandidates()),         // u_LightFeedbackCandidates
        nvrhi::BindingSetItem::Sampler(0, m_context.renderDevice.samplers().anisotropicWrap()),
        nvrhi::BindingSetItem::Sampler(1, m_context.scenePasses.lighting.environment()->getEnvMapCubeSampler()),
        nvrhi::BindingSetItem::Sampler(2, m_context.scenePasses.lighting.environment()->getImportanceSampling()->getImportanceMapSampler()),
        nvrhi::BindingSetItem::Texture_UAV(0, m_renderTargets->outputColor),
        nvrhi::BindingSetItem::Texture_UAV(1, m_renderTargets->processedOutputColor),
        nvrhi::BindingSetItem::Texture_UAV(2, m_renderTargets->ldrColor, nvrhi::Format::RGBA8_UNORM),
        nvrhi::BindingSetItem::Texture_UAV(4, m_renderTargets->throughput),
        nvrhi::BindingSetItem::Texture_UAV(5, m_renderTargets->screenMotionVectors),
        nvrhi::BindingSetItem::Texture_UAV(6, m_renderTargets->depth),
        nvrhi::BindingSetItem::Texture_UAV(7, m_renderTargets->specularHitT), 
        nvrhi::BindingSetItem::Texture_UAV(8, m_renderTargets->scratchFloat1), 
        nvrhi::BindingSetItem::Texture_UAV(31, m_renderTargets->denoiserViewspaceZ),
        nvrhi::BindingSetItem::Texture_UAV(32, m_renderTargets->denoiserMotionVectors),
        nvrhi::BindingSetItem::Texture_UAV(33, m_renderTargets->denoiserNormalRoughness),
        nvrhi::BindingSetItem::Texture_UAV(34, m_renderTargets->denoiserDiffRadianceHitDist),
        nvrhi::BindingSetItem::Texture_UAV(35, m_renderTargets->denoiserSpecRadianceHitDist),
        nvrhi::BindingSetItem::Texture_UAV(36, m_renderTargets->denoiserDisocclusionThresholdMix),
        nvrhi::BindingSetItem::Texture_UAV(37, m_renderTargets->combinedHistoryClampRelax),
        nvrhi::BindingSetItem::StructuredBuffer_UAV(51, m_feedback_Buffer_Gpu),
        nvrhi::BindingSetItem::StructuredBuffer_UAV(52, m_debugLineBufferCapture),
        nvrhi::BindingSetItem::StructuredBuffer_UAV(53, m_debugDeltaPathTree_Gpu),
        nvrhi::BindingSetItem::StructuredBuffer_UAV(54, m_debugDeltaPathTreeSearchStack),
        nvrhi::BindingSetItem::Texture_UAV(60, m_renderTargets->secondarySurfacePositionNormal),
        nvrhi::BindingSetItem::Texture_UAV(61, m_renderTargets->secondarySurfaceRadiance),
        nvrhi::BindingSetItem::Texture_UAV(70, m_renderTargets->rrDiffuseAlbedo),
        nvrhi::BindingSetItem::Texture_UAV(71, m_renderTargets->rrSpecAlbedo),
        nvrhi::BindingSetItem::Texture_UAV(72, m_renderTargets->rrNormalsAndRoughness),
        nvrhi::BindingSetItem::Texture_UAV(73, m_renderTargets->rrSpecMotionVectors),
        nvrhi::BindingSetItem::Texture_UAV(74, (m_renderTargets->rrTransparencyLayer!=nullptr)?m_renderTargets->rrTransparencyLayer:m_renderTargets->rrSpecMotionVectors),
        nvrhi::BindingSetItem::Texture_UAV(75, m_renderTargets->denoiserAvgLayerRadianceHalfRes),

        ///***
        nvrhi::BindingSetItem::Texture_UAV(100, m_renderTargets->baseColor),
        nvrhi::BindingSetItem::Texture_UAV(101, m_renderTargets->specNormal),
        nvrhi::BindingSetItem::Texture_UAV(102, m_renderTargets->roughnessMetal),
        nvrhi::BindingSetItem::Texture_UAV(103, m_renderTargets->materialInfo),
        nvrhi::BindingSetItem::Texture_UAV(10, m_renderTargets->localCubemap),  // u_LocalCubemap for RT pass
        ///***

        nvrhi::BindingSetItem::RawBuffer_UAV(SHADER_DEBUG_BUFFER_UAV_INDEX, m_shaderDebug->getGPUWriteBuffer()),
        nvrhi::BindingSetItem::Texture_UAV(SHADER_DEBUG_VIZ_TEXTURE_UAV_INDEX, m_shaderDebug->getDebugVizTexture())
    };

    // NV HLSL extensions - DX12 only - we should probably expose some form of GetNvapiIsInitialized instead
    if (device()->queryFeatureSupport(nvrhi::Feature::HlslExtensionUAV))
    {
        bindingSetDescBase.bindings.push_back(
            nvrhi::BindingSetItem::TypedBuffer_UAV(NV_SHADER_EXTN_SLOT_NUM, nullptr));
    }

    // Main raytracing & etc binding set
    {
        nvrhi::BindingSetDesc bindingSetDesc;

        bindingSetDesc.bindings = bindingSetDescBase.bindings;

        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_UAV(40, m_renderTargets->stablePlanesHeader));
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::StructuredBuffer_UAV(42, m_renderTargets->stablePlanesBuffer));
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_UAV(44, m_renderTargets->stableRadiance));
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::StructuredBuffer_UAV(45, m_renderTargets->surfaceDataBuffer));

        // Reflection system bindings (t80-t83, b3)
        // Derived classes can override AddCustomBindings to provide actual textures
        // Default to black texture fallbacks (NVRHI doesn't allow null textures)
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(80, m_context.renderDevice.builtins().blackCubeMapArray()));  // t_LocalCubemapGGX
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(81, m_context.renderDevice.builtins().blackCubeMapArray()));  // t_DiffuseIrradianceCube
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(82, m_context.renderDevice.builtins().blackTexture()));  // t_SSRBlurChain
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(83, (m_context.scenePasses.lighting.environment()->getBRDFLUT()!=nullptr)?m_context.scenePasses.lighting.environment()->getBRDFLUT():m_context.renderDevice.builtins().blackTexture() ));  // t_BRDFLUT
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(84, m_context.renderDevice.builtins().blackTexture()));  // t_DepthHierarchy placeholder
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::ConstantBuffer(10, m_constantBuffer)); // ReflectionConstants (reuse main constant buffer as placeholder)
        
        // SSR result UAV placeholder
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_UAV(85, m_renderTargets->depth));   // u_SSRResult placeholder

        // GTAO output (default to white = no occlusion; overridden by AddCustomBindings)
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(86, m_context.renderDevice.builtins().whiteTexture()));  // t_GTAOOutput placeholder
        // Previous frame depth (default to black = zero depth; overridden by AddCustomBindings)
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(87, m_context.renderDevice.builtins().blackTexture()));  // t_PrevDepth placeholder

        m_bindingSet = device()->createBindingSet(bindingSetDesc, m_bindingLayout);
    }
}
void caustica::render::WorldRenderer::pathTracePrePass(nvrhi::ICommandList* commandList)
{
    nvrhi::CommandListHandle savedCommandList = m_commandList;
    m_commandList = commandList;

    if (!m_ptPipelineBuildStablePlanes || !m_ptPipelineFillStablePlanes)
    {
        m_context.scenePasses.rayTracing.ensureStablePlanePipelines();
        assert(m_ptPipelineBuildStablePlanes && m_ptPipelineFillStablePlanes);
    }

    nvrhi::rt::State state;
    nvrhi::rt::DispatchRaysArguments args;
    const ViewportDesc viewport = m_context.camera.view()->getViewport();
    const uint32_t width = static_cast<uint32_t>(viewport.width());
    const uint32_t height = static_cast<uint32_t>(viewport.height());
    args.width = width;
    args.height = height;

    SampleMiniConstants miniConstants = { uint4(0, 0, 0, 0) };

    RAII_SCOPE(m_commandList->beginMarker("PathTracePrePass"); , m_commandList->endMarker(); );

    state.shaderTable = m_ptPipelineBuildStablePlanes->getShaderTable();
    state.bindings = { m_bindingSet, m_context.descriptorTable->getDescriptorTable() };
    m_commandList->setRayTracingState(state);
    m_commandList->setPushConstants(&miniConstants, sizeof(miniConstants));
    m_commandList->dispatchRays(args);

    m_commandList = savedCommandList;
}

void caustica::render::WorldRenderer::vBufferExport(nvrhi::ICommandList* commandList)
{
    nvrhi::CommandListHandle savedCommandList = m_commandList;
    m_commandList = commandList;

    const ViewportDesc viewport = m_context.camera.view()->getViewport();
    const uint32_t width = static_cast<uint32_t>(viewport.width());
    const uint32_t height = static_cast<uint32_t>(viewport.height());

    SampleMiniConstants miniConstants = { uint4(0, 0, 0, 0) };

    RAII_SCOPE(m_commandList->beginMarker("VBufferExport"); , m_commandList->endMarker(); );

    nvrhi::ComputeState state;
    state.bindings = { m_bindingSet, m_context.descriptorTable->getDescriptorTable() };
    state.pipeline = m_exportVBufferPSO;
    m_commandList->setComputeState(state);

    const dm::uint2 dispatchSize = {
        (width + NUM_COMPUTE_THREADS_PER_DIM - 1) / NUM_COMPUTE_THREADS_PER_DIM,
        (height + NUM_COMPUTE_THREADS_PER_DIM - 1) / NUM_COMPUTE_THREADS_PER_DIM };
    m_commandList->setPushConstants(&miniConstants, sizeof(miniConstants));
    m_commandList->dispatch(dispatchSize.x, dispatchSize.y);

    m_commandList = savedCommandList;
}

void caustica::render::WorldRenderer::pathTraceLightingEndUpdate(nvrhi::ICommandList* commandList)
{
    UpdateLightingEndParams lightingEndParams{
        commandList,
        m_context.scenePasses.lighting.lightSampling().get(),
        &m_context.bindingCache,
        m_context.sceneManager.getScene(),
        m_context.scenePasses.lighting.materials(),
        m_context.scenePasses.lighting.opacityMaps(),
        m_context.accelStructs.getSubInstanceBuffer(),
        m_renderTargets->depth,
        m_renderTargets->screenMotionVectors,
    };
    caustica::updateLightingEnd(lightingEndParams);
}

void caustica::render::WorldRenderer::mainPathTrace(nvrhi::ICommandList* commandList)
{
    nvrhi::CommandListHandle savedCommandList = m_commandList;
    m_commandList = commandList;

    const bool useStablePlanes = m_context.activeSettings().RealtimeMode;

    nvrhi::rt::State state;
    nvrhi::rt::DispatchRaysArguments args;
    const ViewportDesc viewport = m_context.camera.view()->getViewport();
    const uint32_t width = static_cast<uint32_t>(viewport.width());
    const uint32_t height = static_cast<uint32_t>(viewport.height());
    args.width = width;
    args.height = height;

    RAII_SCOPE(m_commandList->beginMarker("PathTrace");, m_commandList->endMarker(); );

    state.shaderTable = (useStablePlanes ? m_ptPipelineFillStablePlanes : m_ptPipelineReference)->getShaderTable();
    state.bindings = { m_bindingSet, m_context.descriptorTable->getDescriptorTable() };

    for (uint subSampleIndex = 0; subSampleIndex < m_context.activeSettings().actualSamplesPerPixel(); subSampleIndex++)
    {
        m_commandList->setRayTracingState(state);

        SampleMiniConstants miniConstants = { uint4(subSampleIndex, 0, 0, 0) };
        m_commandList->setPushConstants(&miniConstants, sizeof(miniConstants));

        m_commandList->dispatchRays(args);
    }

    m_commandList = savedCommandList;
}

void caustica::render::WorldRenderer::executeRtxdi(nvrhi::ICommandList* commandList)
{
    nvrhi::CommandListHandle savedCommandList = m_commandList;
    m_commandList = commandList;

    static bool enableFusedDIGIFinal = true;
    const bool useFusedDIGIFinal = m_context.activeSettings().actualUseReSTIRDI()
        && m_context.activeSettings().actualUseReSTIRGI()
        && enableFusedDIGIFinal;

    RAII_SCOPE(m_commandList->beginMarker("RTXDI");, m_commandList->endMarker(); );

    if (m_context.activeSettings().actualUseReSTIRDI())
        m_rtxdiPass->execute(m_commandList, m_bindingSet, useFusedDIGIFinal);

    if (m_context.activeSettings().actualUseReSTIRGI())
        m_rtxdiPass->executeGI(m_commandList, m_bindingSet, useFusedDIGIFinal);

    if (useFusedDIGIFinal)
        m_rtxdiPass->executeFusedDIGIFinal(m_commandList, m_bindingSet);

    if (m_context.activeSettings().actualUseReSTIRPT())
        m_rtxdiPass->executePT(m_commandList, m_bindingSet);

    m_commandList = savedCommandList;
}

void caustica::render::WorldRenderer::prepareDenoiserGuides(nvrhi::ICommandList* commandList)
{
    nvrhi::CommandListHandle savedCommandList = m_commandList;
    m_commandList = commandList;

    RAII_SCOPE(m_commandList->beginMarker("Denoising Guides Bake"); , m_commandList->endMarker(); );

    m_denoisingGuidesPass->denoiseSpecHitT(m_commandList, m_bindingSet);
    m_denoisingGuidesPass->computeAvgLayerRadiance(m_commandList, m_bindingSet);

    if (m_context.activeSettings().DebugView != DebugViewType::Disabled)
        m_denoisingGuidesPass->renderDebugViz(m_commandList, m_context.activeSettings().DebugView, m_bindingSet);

    m_commandList = savedCommandList;
}

void caustica::render::WorldRenderer::stablePlanesDebugViz(nvrhi::ICommandList* commandList)
{
    nvrhi::CommandListHandle savedCommandList = m_commandList;
    m_commandList = commandList;

    SampleMiniConstants miniConstants = { uint4(0, 0, 0, 0) };

    m_commandList->beginMarker("StablePlanesDebugViz");
    nvrhi::TextureDesc tdesc = m_renderTargets->outputColor->getDesc();
    m_postProcess->apply(
        m_commandList,
        PostProcess::ComputePassType::StablePlanesDebugViz,
        m_constantBuffer,
        miniConstants,
        m_bindingSet,
        m_bindingLayout,
        tdesc.width,
        tdesc.height);
    m_commandList->endMarker();

    m_commandList = savedCommandList;
}

void caustica::render::WorldRenderer::ensureNrdIntegrations()
{
    if (!m_context.activeSettings().actualUseStandaloneDenoiser())
        return;

    for (int i = 0; i < std::size(m_nrd); i++)
    {
        if (m_nrd[i] != nullptr)
            continue;

        nrd::Denoiser denoiserMethod = m_context.activeSettings().NRDMethod == NrdConfig::DenoiserMethod::REBLUR
            ? nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR
            : nrd::Denoiser::RELAX_DIFFUSE_SPECULAR;

        m_nrd[i] = std::make_unique<NrdIntegration>(device(), denoiserMethod);
        m_nrd[i]->initialize(m_renderSize.x, m_renderSize.y, *m_context.shaderFactory);
    }
}

void caustica::render::WorldRenderer::denoiseStablePlane(
    nvrhi::ICommandList* commandList,
    nvrhi::IFramebuffer* framebuffer,
    int planeIndex)
{
    (void)framebuffer;

    if (!m_context.activeSettings().actualUseStandaloneDenoiser())
        return;

    nvrhi::CommandListHandle savedCommandList = m_commandList;
    m_commandList = commandList;

    const char* passNames[] = { "Denoising plane 0", "Denoising plane 1", "Denoising plane 2", "Denoising plane 3" };
    assert(planeIndex >= 0 && planeIndex < static_cast<int>(std::size(m_nrd)));
    assert(planeIndex < static_cast<int>(std::size(passNames)));

    const bool nrdUseRelax = m_context.activeSettings().NRDMethod == NrdConfig::DenoiserMethod::RELAX;
    const PostProcess::ComputePassType preparePassType = nrdUseRelax
        ? PostProcess::ComputePassType::RELAXDenoiserPrepareInputs
        : PostProcess::ComputePassType::REBLURDenoiserPrepareInputs;
    const PostProcess::ComputePassType mergePassType = nrdUseRelax
        ? PostProcess::ComputePassType::RELAXDenoiserFinalMerge
        : PostProcess::ComputePassType::REBLURDenoiserFinalMerge;

    const bool resetHistory = m_context.activeSettings().ResetRealtimeCaches;
    const int maxPassCount = std::min(m_context.activeSettings().StablePlanesActiveCount, static_cast<int>(std::size(m_nrd)));
    const bool initWithStableRadiance = planeIndex == (maxPassCount - 1);

    m_commandList->beginMarker(passNames[planeIndex]);

    SampleMiniConstants miniConstants = { uint4(static_cast<uint>(planeIndex), initWithStableRadiance ? 1u : 0u, 0, 0) };

    nvrhi::TextureDesc tdesc = m_renderTargets->outputColor->getDesc();
    m_commandList->beginMarker("PrepareInputs");
    m_postProcess->apply(
        m_commandList,
        preparePassType,
        m_constantBuffer,
        miniConstants,
        m_bindingSet,
        m_bindingLayout,
        tdesc.width,
        tdesc.height);
    m_commandList->endMarker();

    const float timeDeltaBetweenFrames = m_context.gpuDevice.isHeadless() ? 1.f / 60.f : -1.f;
    const bool enableValidation = m_context.activeSettings().DebugView == DebugViewType::StablePlane_DenoiserValidation;
    if (nrdUseRelax)
    {
        m_nrd[planeIndex]->runDenoiserPasses(
            m_commandList,
            *m_renderTargets,
            planeIndex,
            *m_context.camera.view(),
            *m_context.camera.viewPrevious(),
            m_context.gpuDevice.getRenderPhaseFrameIndex(),
            m_context.activeSettings().NRDDisocclusionThreshold,
            m_context.activeSettings().NRDDisocclusionThresholdAlternate,
            m_context.activeSettings().NRDUseAlternateDisocclusionThresholdMix,
            timeDeltaBetweenFrames,
            enableValidation,
            resetHistory,
            &m_context.activeSettings().RelaxSettings);
    }
    else
    {
        m_nrd[planeIndex]->runDenoiserPasses(
            m_commandList,
            *m_renderTargets,
            planeIndex,
            *m_context.camera.view(),
            *m_context.camera.viewPrevious(),
            m_context.gpuDevice.getRenderPhaseFrameIndex(),
            m_context.activeSettings().NRDDisocclusionThreshold,
            m_context.activeSettings().NRDDisocclusionThresholdAlternate,
            m_context.activeSettings().NRDUseAlternateDisocclusionThresholdMix,
            timeDeltaBetweenFrames,
            enableValidation,
            resetHistory,
            &m_context.activeSettings().ReblurSettings);
    }

    m_commandList->beginMarker("MergeOutputs");
    m_postProcess->apply(
        m_commandList,
        mergePassType,
        planeIndex,
        m_constantBuffer,
        miniConstants,
        m_renderTargets->outputColor,
        *m_renderTargets,
        nullptr);
    m_commandList->endMarker();

    m_commandList->endMarker();
    m_commandList = savedCommandList;
}

void caustica::render::WorldRenderer::denoise(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer)
{
    if (!m_context.activeSettings().actualUseStandaloneDenoiser())
        return;

    ensureNrdIntegrations();

    const int maxPassCount = std::min(m_context.activeSettings().StablePlanesActiveCount, static_cast<int>(std::size(m_nrd)));
    for (int pass = maxPassCount - 1; pass >= 0; pass--)
        denoiseStablePlane(commandList, framebuffer, pass);
}
#if CAUSTICA_WITH_NATIVE_DLSS
bool caustica::render::WorldRenderer::evaluateNativeDLSS(bool reset)
{
    if (!m_nativeDLSS || !(m_context.activeSettings().RealtimeAA == 2 || m_context.activeSettings().RealtimeAA == 3))
        return false;

    const bool useRayReconstruction = m_context.activeSettings().RealtimeAA == 3;
    if (useRayReconstruction && !m_nativeDLSS->isRayReconstructionSupported())
        return false;
    if (!useRayReconstruction && !m_nativeDLSS->isDlssSupported())
        return false;

    if (useRayReconstruction)
    {
        RAII_SCOPE(m_commandList->beginMarker("DLSSRR_PrepareInputs");, m_commandList->endMarker(););

        SampleMiniConstants miniConstants = { uint4(0, 0, 0, 0) };
        nvrhi::TextureDesc tdesc = m_renderTargets->outputColor->getDesc();
        m_postProcess->apply(m_commandList, PostProcess::ComputePassType::DLSSRRDenoiserPrepareInputs,
            m_constantBuffer, miniConstants, m_bindingSet, m_bindingLayout, tdesc.width, tdesc.height);
    }

    caustica::render::DLSS::InitParameters initParams;
    initParams.inputWidth = m_renderSize.x;
    initParams.inputHeight = m_renderSize.y;
    initParams.outputWidth = m_displaySize.x;
    initParams.outputHeight = m_displaySize.y;
    initParams.useLinearDepth = false;
    initParams.useAutoExposure = true;
    initParams.useRayReconstruction = useRayReconstruction;

    m_nativeDLSS->init(initParams);

    const bool initialized = useRayReconstruction
        ? m_nativeDLSS->isRayReconstructionInitialized()
        : m_nativeDLSS->isDlssInitialized();
    if (!initialized)
        return false;

    caustica::render::DLSS::EvaluateParameters evaluateParams;
    evaluateParams.inputColorTexture = m_renderTargets->outputColor;
    evaluateParams.outputColorTexture = m_renderTargets->processedOutputColor;
    evaluateParams.depthTexture = m_renderTargets->depth;
    evaluateParams.motionVectorsTexture = m_renderTargets->screenMotionVectors;
    evaluateParams.motionVectorScaleX = 1.0f / float(m_renderSize.x);
    evaluateParams.motionVectorScaleY = 1.0f / float(m_renderSize.y);
    evaluateParams.resetHistory = reset || m_context.activeSettings().ResetRealtimeCaches;

    if (useRayReconstruction)
    {
        evaluateParams.diffuseAlbedo = m_renderTargets->rrDiffuseAlbedo;
        evaluateParams.specularAlbedo = m_renderTargets->rrSpecAlbedo;
        evaluateParams.normalRoughness = m_renderTargets->rrNormalsAndRoughness;
    }

    const bool evaluated = m_nativeDLSS->evaluate(m_commandList, evaluateParams, *m_context.camera.view());
    if (evaluated)
    {
        static bool loggedNativeDLSSEvaluation = false;
        if (!loggedNativeDLSSEvaluation)
        {
            caustica::info("Native NGX %s evaluated successfully at %ux%u -> %ux%u.",
                useRayReconstruction ? "DLSS-RR" : "DLSS",
                m_renderSize.x, m_renderSize.y, m_displaySize.x, m_displaySize.y);
            loggedNativeDLSSEvaluation = true;
        }
    }

    return evaluated;
}
#endif
void caustica::render::WorldRenderer::runNoDenoiserFinalMerge(nvrhi::ICommandList* commandList)
{
    if (!m_context.activeSettings().RealtimeMode || m_context.activeSettings().actualUseStandaloneDenoiser())
        return;

    if (m_context.activeSettings().RealtimeAA == 2 || m_context.activeSettings().RealtimeAA == 3)
        return;

    SampleMiniConstants miniConstants = { uint4(0, 0, 0, 0) };
    nvrhi::TextureDesc tdesc = m_renderTargets->outputColor->getDesc();
    commandList->beginMarker("NoDenoiserFinalMerge");
    m_postProcess->apply(
        commandList,
        PostProcess::ComputePassType::NoDenoiserFinalMerge,
        m_constantBuffer,
        miniConstants,
        m_bindingSet,
        m_bindingLayout,
        tdesc.width,
        tdesc.height);
    commandList->endMarker();
}

void caustica::render::WorldRenderer::runDlssUpscale(nvrhi::ICommandList* commandList, bool reset)
{
    if (!m_context.activeSettings().RealtimeMode)
        return;

    if (!(m_context.activeSettings().RealtimeAA == 2 || m_context.activeSettings().RealtimeAA == 3))
        return;

    PostProcessAAParams params{
        m_context.activeSettings(),
        commandList,
        m_renderTargets.get(),
        &m_context.gpuDevice,
    };
    params.renderSize = m_renderSize;
    params.displaySize = m_displaySize;
    params.displayAspectRatio = m_displayAspectRatio;
    params.cameraJitter = computeCameraJitter();
    params.sampleIndex = m_sampleIndex;
    params.frameIndex = static_cast<uint32_t>(m_frameIndex);
    params.reset = reset;
    params.temporalAAPass = m_temporalAntiAliasingPass.get();
    params.accumulationPass = m_accumulationPass.get();
    params.postProcess = m_postProcess.get();
    params.bindingSet = m_bindingSet;
    params.bindingLayout = m_bindingLayout;
    params.constantBuffer = m_constantBuffer;
    params.accumulationSampleIndex = m_accumulationSampleIndex;
    params.gaussianSplatTemporalSampleIndex = &m_gaussianSplatTemporalSampleIndex;
    params.gaussianSplatTemporalReset = &m_frameGaussianSplatTemporalReset;
#if CAUSTICA_WITH_STREAMLINE
    params.dlssRROptions = &m_lastDLSSRROptions;
#endif

    caustica::postProcessAA(m_context.camera, params);

#if CAUSTICA_WITH_NATIVE_DLSS
    nvrhi::CommandListHandle savedCommandList = m_commandList;
    m_commandList = commandList;

    bool nativeDLSSEvaluated = evaluateNativeDLSS(reset);

    if (!nativeDLSSEvaluated)
    {
        if (m_context.activeSettings().actualUseStandaloneDenoiser())
        {
            commandList->copyTexture(
                m_renderTargets->processedOutputColor, nvrhi::TextureSlice(),
                m_renderTargets->outputColor, nvrhi::TextureSlice());
        }
        else
        {
            SampleMiniConstants miniConstants = { uint4(0, 0, 0, 0) };
            nvrhi::TextureDesc tdesc = m_renderTargets->outputColor->getDesc();
            commandList->beginMarker("NoDenoiserFinalMerge");
            m_postProcess->apply(
                commandList,
                PostProcess::ComputePassType::NoDenoiserFinalMerge,
                m_constantBuffer,
                miniConstants,
                m_bindingSet,
                m_bindingLayout,
                tdesc.width,
                tdesc.height);
            commandList->endMarker();
        }
    }

    m_commandList = savedCommandList;
#endif
}

void caustica::render::WorldRenderer::postProcessAA(nvrhi::IFramebuffer* framebuffer, bool reset)
{
    (void)framebuffer;
    runDlssUpscale(m_commandList, reset);
}
void caustica::render::WorldRenderer::resetReferenceOIDN()
{
    m_oidnDenoisedOutputValid = false;
    m_oidnDenoiserFailed = false;

    if (m_oidnDenoiser)
        m_oidnDenoiser->reset();
}
void caustica::render::WorldRenderer::applyReferenceOIDN()
{
    if (m_context.activeSettings().RealtimeMode || !m_context.activeSettings().ReferenceOIDNDenoiser || m_renderTargets == nullptr)
        return;

#if CAUSTICA_WITH_OIDN
    const bool accumulationReady = m_accumulationCompleted || m_accumulationSampleIndex >= m_context.activeSettings().AccumulationTarget;
    if (!accumulationReady)
        return;

    if (m_oidnDenoiserFailed)
        return;

    const nvrhi::TextureDesc processedDesc = m_renderTargets->processedOutputColor->getDesc();
    if (m_oidnDenoisedOutput == nullptr ||
        m_oidnDenoisedOutput->getDesc().width != processedDesc.width ||
        m_oidnDenoisedOutput->getDesc().height != processedDesc.height ||
        m_oidnDenoisedOutput->getDesc().format != processedDesc.format)
    {
        nvrhi::TextureDesc oidnOutputDesc = processedDesc;
        oidnOutputDesc.debugName = "ReferenceOIDNDenoisedOutput";
        oidnOutputDesc.initialState = nvrhi::ResourceStates::CopySource;
        oidnOutputDesc.keepInitialState = true;
        m_oidnDenoisedOutput = device()->createTexture(oidnOutputDesc);
        m_oidnDenoisedOutputValid = false;
    }

    if (m_oidnDenoisedOutputValid)
    {
        m_commandList->copyTexture(m_renderTargets->processedOutputColor, nvrhi::TextureSlice(), m_oidnDenoisedOutput, nvrhi::TextureSlice());
        return;
    }

    nvrhi::ITexture* sourceTexture = m_renderTargets->accumulatedRadiance;
    nvrhi::TextureDesc sourceDesc = sourceTexture->getDesc();
    if (sourceDesc.format != nvrhi::Format::RGBA32_FLOAT)
    {
        caustica::warning("OIDN reference denoiser expected RGBA32_FLOAT accumulation buffer, got %s.", nvrhi::utils::FormatToString(sourceDesc.format));
        m_oidnDenoiserFailed = true;
        return;
    }

    const uint32_t width = sourceDesc.width;
    const uint32_t height = sourceDesc.height;

    OidnDenoiser::Options oidnOptions;
    oidnOptions.UseGPU = m_context.activeSettings().ReferenceOIDNUseGPU;
    oidnOptions.GuidePasses = static_cast<OidnDenoiser::Passes>(std::clamp(m_context.activeSettings().ReferenceOIDNPasses, 0, 2));
    oidnOptions.GuidePrefilter = static_cast<OidnDenoiser::Prefilter>(std::clamp(m_context.activeSettings().ReferenceOIDNPrefilter, 0, 2));
    oidnOptions.FilterQuality = static_cast<OidnDenoiser::Quality>(std::clamp(m_context.activeSettings().ReferenceOIDNQuality, 0, 2));

    const bool requestAlbedoGuide = oidnOptions.GuidePasses == OidnDenoiser::Passes::Albedo || oidnOptions.GuidePasses == OidnDenoiser::Passes::AlbedoNormal;
    const bool requestNormalGuide = oidnOptions.GuidePasses == OidnDenoiser::Passes::AlbedoNormal;
    if (requestAlbedoGuide || requestNormalGuide)
    {
        SampleMiniConstants miniConstants = { uint4(0, 0, 0, 0) };
        nvrhi::TextureDesc tdesc = m_renderTargets->outputColor->getDesc();
        m_commandList->beginMarker("OIDN_PrepareGuides");
        m_postProcess->apply(m_commandList, PostProcess::ComputePassType::DLSSRRDenoiserPrepareInputs, m_constantBuffer, miniConstants, m_bindingSet, m_bindingLayout, tdesc.width, tdesc.height);
        m_commandList->endMarker();
    }

    nvrhi::StagingTextureHandle stagingTexture = device()->createStagingTexture(
        MakeReadbackTextureDesc(sourceDesc, "ReferenceOIDN AccumulatedRadiance Readback"),
        nvrhi::CpuAccessMode::Read);
    if (stagingTexture == nullptr)
    {
        caustica::warning("OIDN reference denoiser failed to create a staging texture.");
        m_oidnDenoiserFailed = true;
        return;
    }

    nvrhi::StagingTextureHandle albedoStagingTexture;
    nvrhi::StagingTextureHandle normalStagingTexture;
    if (requestAlbedoGuide && m_renderTargets->rrDiffuseAlbedo != nullptr)
    {
        albedoStagingTexture = device()->createStagingTexture(
            MakeReadbackTextureDesc(m_renderTargets->rrDiffuseAlbedo->getDesc(), "ReferenceOIDN Albedo Readback"),
            nvrhi::CpuAccessMode::Read);
        if (albedoStagingTexture != nullptr)
            m_commandList->copyTexture(albedoStagingTexture, nvrhi::TextureSlice(), m_renderTargets->rrDiffuseAlbedo, nvrhi::TextureSlice());
    }
    if (requestNormalGuide && m_renderTargets->rrNormalsAndRoughness != nullptr)
    {
        normalStagingTexture = device()->createStagingTexture(
            MakeReadbackTextureDesc(m_renderTargets->rrNormalsAndRoughness->getDesc(), "ReferenceOIDN Normal Readback"),
            nvrhi::CpuAccessMode::Read);
        if (normalStagingTexture != nullptr)
            m_commandList->copyTexture(normalStagingTexture, nvrhi::TextureSlice(), m_renderTargets->rrNormalsAndRoughness, nvrhi::TextureSlice());
    }

    m_commandList->copyTexture(stagingTexture, nvrhi::TextureSlice(), sourceTexture, nvrhi::TextureSlice());
    m_commandList->close();
    device()->executeCommandList(m_commandList);
    if (!device()->waitForIdle())
    {
        m_commandList->open();
        caustica::warning("OIDN reference denoiser readback failed because the GPU device was lost.");
        m_oidnDenoiserFailed = true;
        return;
    }

    size_t rowPitch = 0;
    const uint8_t* mappedData = static_cast<const uint8_t*>(device()->mapStagingTexture(stagingTexture, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch));
    if (mappedData == nullptr)
    {
        m_commandList->open();
        caustica::warning("OIDN reference denoiser failed to map the accumulation buffer.");
        m_oidnDenoiserFailed = true;
        return;
    }

    std::vector<float> inputRgb(size_t(width) * size_t(height) * 3);

    for (uint32_t y = 0; y < height; y++)
    {
        const float* row = reinterpret_cast<const float*>(mappedData + size_t(y) * rowPitch);
        for (uint32_t x = 0; x < width; x++)
        {
            const size_t sourceOffset = size_t(x) * 4;
            const size_t targetOffset = (size_t(y) * size_t(width) + size_t(x)) * 3;
            for (uint32_t channel = 0; channel < 3; channel++)
            {
                const float value = row[sourceOffset + channel];
                inputRgb[targetOffset + channel] = (std::isfinite(value) && value > 0.0f) ? value : 0.0f;
            }
        }
    }

    device()->unmapStagingTexture(stagingTexture);

    std::vector<float> albedoRgb;
    std::vector<float> normalRgb;
    if (albedoStagingTexture != nullptr)
    {
        ReadR11G11B10Float3Staging(device(), albedoStagingTexture, width, height, albedoRgb);
        if (!albedoRgb.empty())
            oidnOptions.AlbedoRgb = albedoRgb.data();
    }
    if (normalStagingTexture != nullptr)
    {
        ReadRGBA16Float3Staging(device(), normalStagingTexture, width, height, normalRgb);
        if (!normalRgb.empty())
            oidnOptions.NormalRgb = normalRgb.data();
    }

    if (m_oidnDenoiser == nullptr)
        m_oidnDenoiser = std::make_unique<OidnDenoiser>();

    std::vector<float> outputRgb;
    const bool success = m_oidnDenoiser->denoise(inputRgb.data(), width, height, oidnOptions, outputRgb);

    m_commandList->open();

    if (!success)
    {
        caustica::warning("OIDN reference denoiser failed: %s", m_oidnDenoiser->getLastError().c_str());
        m_oidnDenoiserFailed = true;
        return;
    }

    std::vector<float16_t4> outputHalf(size_t(width) * size_t(height));
    constexpr float maxHalf = 65504.0f;
    for (size_t pixel = 0; pixel < outputHalf.size(); pixel++)
    {
        const size_t rgbOffset = pixel * 3;
        const float r = std::clamp(std::isfinite(outputRgb[rgbOffset + 0]) ? outputRgb[rgbOffset + 0] : 0.0f, 0.0f, maxHalf);
        const float g = std::clamp(std::isfinite(outputRgb[rgbOffset + 1]) ? outputRgb[rgbOffset + 1] : 0.0f, 0.0f, maxHalf);
        const float b = std::clamp(std::isfinite(outputRgb[rgbOffset + 2]) ? outputRgb[rgbOffset + 2] : 0.0f, 0.0f, maxHalf);
        outputHalf[pixel] = float32ToFloat16x4(float4(r, g, b, 1.0f));
    }

    m_commandList->writeTexture(m_oidnDenoisedOutput, 0, 0, outputHalf.data(), size_t(width) * sizeof(float16_t4));
    m_commandList->copyTexture(m_renderTargets->processedOutputColor, nvrhi::TextureSlice(), m_oidnDenoisedOutput, nvrhi::TextureSlice());
    m_oidnDenoisedOutputValid = true;

    caustica::info("OIDN reference denoiser completed on %s for %ux%u image.",
        m_oidnDenoiser->getDeviceDescription().c_str(), width, height);
#else
    if (!m_oidnDenoiserFailed)
    {
        caustica::warning("OIDN reference denoiser requested, but CAUSTICA_WITH_OIDN is disabled in this build.");
        m_oidnDenoiserFailed = true;
    }
#endif
}
void caustica::render::WorldRenderer::denoisedScreenshot(nvrhi::ITexture * framebufferTexture) const
{
    std::string noisyImagePath = (caustica::getDirectoryWithExecutable( ) / "photo.bmp").string();

    auto execute = [&](const std::string & dn = "OptiX")
    {
	    const auto p1 = std::chrono::system_clock::now();
		const std::string timestamp = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(p1.time_since_epoch()).count());

        const std::string fileName = "photo-denoised_" + dn + "_" + timestamp + ".bmp";

        std::string denoisedImagePath = (caustica::getDirectoryWithExecutable() / fileName).string();
        std::filesystem::path denoiserPath = getLocalPath("Support/denoiser_"+dn) / "denoiser.exe";
        if (!std::filesystem::exists(denoiserPath))
        {
            caustica::warning("External %s denoiser not found at '%s'.", dn.c_str(), denoiserPath.string().c_str());
            return;
        }

        if (!saveTextureToFile(device(), m_context.renderDevice, framebufferTexture, nvrhi::ResourceStates::Common, noisyImagePath.c_str()))
        { assert(false); return; }

        std::string startCmd = "\"" + denoiserPath.string() + "\"" + " -hdr 0 -i \"" + noisyImagePath + "\"" " -o \"" + denoisedImagePath + "\"";
        auto [resNum, resString, errorString] =  systemShell(startCmd.c_str());
        if (resString!="")
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
    params.realtimeMode = m_context.activeSettings().RealtimeMode;
    params.realtimeAA = m_context.activeSettings().RealtimeAA;
    params.dbgFreezeRealtimeNoiseSeed = m_context.activeSettings().DbgFreezeRealtimeNoiseSeed;
    params.syncPreviousView = m_context.activeSettings().ResetAccumulation || m_context.activeSettings().ResetRealtimeCaches;
    params.temporalAAJitter = m_context.activeSettings().TemporalAntiAliasingJitter;
    params.temporalAAPass = m_temporalAntiAliasingPass.get();
    return params;
}

void caustica::render::WorldRenderer::syncCameraViews()
{
    m_context.camera.updateViews(makeCameraUpdateParams());
    // Stable primary-hit pick: disable TAA/DLSS jitter for the pick frame.
    if (m_context.activeRuntime().Picking.hasActivePickRequest())
        m_context.camera.view()->setPixelOffset(dm::float2::zero());
}

dm::float2 caustica::render::WorldRenderer::computeCameraJitter() const
{
    if (m_context.activeRuntime().Picking.hasActivePickRequest())
        return dm::float2::zero();
    return m_context.camera.computeJitter(makeCameraUpdateParams());
}
