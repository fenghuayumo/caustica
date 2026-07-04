namespace { constexpr int c_SwapchainCount = 3; }

#include <render/WorldRenderer/WorldRenderer.h>
#include <render/SceneGpuResources.h>
#include <render/WorldRenderer/PathTracingContext.h>
#include <render/SceneGaussianSplatPasses.h>
#include <render/SceneRayTracingResources.h>

#include <scene/SceneEcs.h>
#include <scene/SceneLightAccess.h>
#include <render/Core/PostProcessAA.h>
#include <render/Core/SceneGeometryUpdate.h>
#include <render/Core/LightingUpdate.h>
#include <render/Core/PathTracingShaderCompiler.h>
#include <render/Core/ComputePipelineRegistry.h>
#include <render/Core/BindingCache.h>
#include <scene/View.h>
#include <render/Core/FramebufferFactory.h>
#include <render/Core/AccelerationStructureUtil.h>
#include <render/Passes/Lighting/Distant/EnvMapImportanceSamplingCache.h>
#include <render/Passes/Lighting/MaterialGpuCache.h>
#include <render/Passes/OMM/OpacityMicromapBuilder.h>
#include <render/Passes/PostProcess/DenoisingGuidesPass.h>
#include <render/Passes/Denoisers/OidnDenoiser.h>
#include <render/Passes/Gaussian/GaussianSplatPass.h>
#include <assets/loader/ShaderFactory.h>
#include <render/GPUSort/GPUSort.h>
#include <backend/GpuDevice.h>
#include <core/path_utils.h>
#include <core/log.h>
#include <core/progress.h>
#include <assets/loader/TextureLoader.h>
#include <render/Core/PathTracerSettings.h>
#include <math/float.h>
#include <math/math.h>
#include <shaders/SampleConstantBuffer.h>
#include <shaders/view_cb.h>
#include <rhi/utils.h>

#include <core/Timer.h>
#include <shaders/light_cb.h>
#include <render/Passes/Debug/Korgi.h>

#if CAUSTICA_WITH_STREAMLINE
#include <backend/StreamlineInterface.h>
#endif
#if CAUSTICA_WITH_NATIVE_DLSS
#include <render/Passes/Geometry/DLSS.h>
#endif

extern FPSLimiter g_FPSLimiter;

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
    m_lastRealtimeMode = m_context.settings.RealtimeMode;
}

caustica::render::WorldRenderer::~WorldRenderer() = default;

nvrhi::BindingLayoutHandle caustica::render::WorldRenderer::CreateBindlessLayout(nvrhi::IDevice* device)
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
        : CreateBindlessLayout(gpuDevice);

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

#if CAUSTICA_WITH_NATIVE_DLSS
    m_nativeDLSS = caustica::render::DLSS::Create(device, m_context.shaderFactory, caustica::GetDirectoryWithExecutable().string());
    if (m_nativeDLSS)
    {
        m_context.settings.IsDLSSSuported = m_nativeDLSS->IsDlssSupported();
        m_context.settings.IsDLSSRRSupported = m_nativeDLSS->IsRayReconstructionSupported();
        caustica::info("Native NGX DLSS support: DLSS=%s, DLSS-RR=%s.",
            m_context.settings.IsDLSSSuported ? "yes" : "no",
            m_context.settings.IsDLSSRRSupported ? "yes" : "no");
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
        m_linesVertexShader = m_context.shaderFactory->CreateShader("caustica/shaders/render/Misc/DebugLines.hlsl", "main_vs", &drawLinesMacro, nvrhi::ShaderType::Vertex);
        m_linesPixelShader = m_context.shaderFactory->CreateShader("caustica/shaders/render/Misc/DebugLines.hlsl", "main_ps", &drawLinesMacro, nvrhi::ShaderType::Pixel);

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
    m_exportVBufferCS = m_context.shaderFactory->CreateShader(
        "caustica/shaders/render/ProcessingPasses/ExportVisibilityBuffer.hlsl", "main", &shaderMacros, nvrhi::ShaderType::Compute);
    nvrhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.bindingLayouts = { m_bindingLayout, m_bindlessLayout };
    pipelineDesc.CS = m_exportVBufferCS;
    m_exportVBufferPSO = device()->createComputePipeline(pipelineDesc);
    return true;
}

void caustica::render::WorldRenderer::onSceneUnloading()
{
    m_bindingSet = nullptr;
    m_context.bindingCache.Clear();
    if (m_commandList)
        m_commandList = device()->createCommandList();
    m_gaussianSplatTemporalReset = true;
    if (m_rtxdiPass != nullptr)
        m_rtxdiPass->Reset();
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
    m_context.settings.ResetAccumulation = true;
    m_context.settings.ResetRealtimeCaches = true;
    // Realtime->Realtime switches do not flip RealtimeMode; nudge so framePassSetup resets temporal state.
    m_lastRealtimeMode = !m_context.settings.RealtimeMode;

    if (m_rtxdiPass)
        m_rtxdiPass->Reset();
}

void caustica::render::WorldRenderer::onBackBufferResizing()
{
    device()->waitForIdle();
    device()->runGarbageCollection();
    m_context.bindingCache.Clear();
    m_renderTargets = nullptr;
    m_linesPipeline = nullptr; // the pipeline is based on the framebuffer so needs a reset
    for (int i=0; i < std::size(m_nrd); i++ )
        m_nrd[i] = nullptr;
    if (m_rtxdiPass)
        m_rtxdiPass->Reset();

// NOTE: we're not yet sure if this is necessary to avoid crash with going in/out of fullscreen and FG
#if CAUSTICA_WITH_STREAMLINE
    if (!m_context.gpuDevice.IsHeadless() &&
        (m_context.settings.DLSSFGOptions.mode == StreamlineInterface::DLSSGMode::eOn || m_context.settings.ActualDLSSFGMode() == StreamlineInterface::DLSSGMode::eOn)) 
    {
        m_context.gpuDevice.GetStreamline().CleanupDLSS(false);
        m_context.gpuDevice.GetStreamline().CleanupDLSSG(false);

        if (m_context.gpuDevice.GetStreamline().IsDLSSGAvailable())
        {
            auto dlssgOptions = StreamlineInterface::DLSSGOptions{};
            StreamlineInterface::DLSSGState state;
            m_context.gpuDevice.GetStreamline().GetDLSSGState(state, dlssgOptions);
            m_context.settings.DLSSFGMultiplier = state.numFramesActuallyPresented;
            m_context.settings.DLSSFGMaxNumFramesToGenerate = state.numFramesToGenerateMax;

            m_context.gpuDevice.GetStreamline().SetDLSSGOptions(dlssgOptions);
            m_context.settings.DLSSFGOptions = dlssgOptions;
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
    m_context.bindingCache.Clear();

    const uint2 screenResolution = {m_renderTargets->OutputColor->getDesc().width, m_renderTargets->OutputColor->getDesc().height};

    m_shaderDebug = std::make_shared<ShaderDebug>(device(), initializeCommandList, m_context.shaderFactory, m_context.commonPasses);

    if (m_context.settings.ActualUseRTXDIPasses())
        m_rtxdiPass = std::make_unique<RtxdiPass>(device(), m_context.shaderFactory, m_context.commonPasses, m_bindlessLayout);
    else
        m_rtxdiPass = nullptr;

    m_accumulationPass = std::make_unique<AccumulationPass>(device(), m_context.shaderFactory);
    m_accumulationPass->CreatePipeline();
    m_accumulationPass->CreateBindingSet(m_renderTargets->OutputColor, m_renderTargets->AccumulatedRadiance, m_renderTargets->ProcessedOutputColor);

    {
        nvrhi::TextureDesc gaussianCurrentDesc = m_renderTargets->ProcessedOutputColor->getDesc();
        gaussianCurrentDesc.debugName = "GaussianSplatTemporalCurrentColor";
        gaussianCurrentDesc.isUAV = false;
        gaussianCurrentDesc.isRenderTarget = false;
        gaussianCurrentDesc.useClearValue = false;
        gaussianCurrentDesc.clearValue = nvrhi::Color(0.0f);
        gaussianCurrentDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        gaussianCurrentDesc.keepInitialState = true;
        m_gaussianSplatCurrentColor = device()->createTexture(gaussianCurrentDesc);

        nvrhi::TextureDesc gaussianAccumDesc = m_renderTargets->ProcessedOutputColor->getDesc();
        gaussianAccumDesc.debugName = "GaussianSplatTemporalAccumulatedColor";
        gaussianAccumDesc.format = nvrhi::Format::RGBA32_FLOAT;
        gaussianAccumDesc.isUAV = true;
        gaussianAccumDesc.isRenderTarget = true;
        gaussianAccumDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
        gaussianAccumDesc.keepInitialState = true;
        m_gaussianSplatAccumulatedColor = device()->createTexture(gaussianAccumDesc);

        m_gaussianSplatAccumulationPass = std::make_unique<AccumulationPass>(device(), m_context.shaderFactory);
        m_gaussianSplatAccumulationPass->CreatePipeline();
        m_gaussianSplatAccumulationPass->CreateBindingSet(m_gaussianSplatCurrentColor, m_gaussianSplatAccumulatedColor, m_renderTargets->ProcessedOutputColor);
        m_gaussianSplatTemporalReset = true;
    }

    // these get re-created every time intentionally, to pick up changes after at-runtime shader recompile
    m_toneMappingPass = std::make_unique<ToneMappingPass>(device(), m_context.shaderFactory, m_context.commonPasses, m_renderTargets->LdrFramebuffer, *m_context.renderCore.camera().view(), m_renderTargets->OutputColor);
    m_bloomPass = std::make_unique<BloomPass>(device(), m_context.shaderFactory, m_context.commonPasses, m_renderTargets->ProcessedOutputFramebuffer, *m_context.renderCore.camera().view());
    m_postProcess = std::make_shared<PostProcess>(device(), m_context.shaderFactory, m_context.commonPasses, m_shaderDebug);

    {
        TemporalAntiAliasingPass::CreateParameters taaParams;
        taaParams.sourceDepth = m_renderTargets->Depth;
        taaParams.motionVectors = m_renderTargets->ScreenMotionVectors;
        taaParams.unresolvedColor = m_renderTargets->OutputColor;
        taaParams.resolvedColor = m_renderTargets->ProcessedOutputColor;
        taaParams.feedback1 = m_renderTargets->TemporalFeedback1;
        taaParams.feedback2 = m_renderTargets->TemporalFeedback2;
        taaParams.historyClampRelax = m_renderTargets->CombinedHistoryClampRelax;
        taaParams.motionVectorStencilMask = 0; ///*uint32_t motionVectorStencilMask =*/ 0x01;
        taaParams.useCatmullRomFilter = true;

        m_temporalAntiAliasingPass = std::make_unique<TemporalAntiAliasingPass>(device(), m_context.shaderFactory, m_context.commonPasses, *m_context.renderCore.camera().view(), taaParams);
    }

    if (!createPTPipeline())
        { assert(false); }

    if (m_context.scenePasses.lighting.environment() == nullptr)
        m_context.scenePasses.lighting.environment() = std::make_shared<EnvMapProcessor>(device(), m_context.textureCache, false);
    if (m_context.scenePasses.lighting.lightSampling() == nullptr)
        m_context.scenePasses.lighting.lightSampling() = std::make_shared<LightSamplingCache>(device());
    m_context.scenePasses.lighting.environment()->CreateRenderPasses(m_shaderDebug, m_context.shaderFactory, m_context.scenePasses.lighting.computePipelines());
    m_context.scenePasses.lighting.environment()->GenerateBRDFLUT(initializeCommandList.Get(), m_context.bindingCache);  // One-time BRDF LUT generation
    m_context.scenePasses.lighting.lightSampling()->CreateRenderPasses(m_context.shaderFactory, m_bindlessLayout, m_context.commonPasses, m_shaderDebug, screenResolution, m_context.scenePasses.lighting.environment()->GetImportanceSampling()->GetImportanceMapResolution());

    m_context.scenePasses.gaussianSplats.preparePasses();

    m_denoisingGuidesPass = std::make_shared<DenoisingGuidesPass>(device(), m_context.shaderFactory, m_renderTargets, m_shaderDebug, m_bindingLayout);
}
void caustica::render::WorldRenderer::preUpdateLighting(nvrhi::CommandListHandle commandList, bool& needNewBindings)
{
    std::filesystem::path sceneDirectory;
    if (m_context.sceneManager.getCurrentScenePath() != std::filesystem::path(SceneManager::inlineSceneSentinel()))
        sceneDirectory = m_context.sceneManager.getCurrentScenePath().parent_path();

    std::string envMapActualPath = m_context.scenePasses.lighting.envMapLocalPath();
    if (m_context.scenePasses.lighting.envMapOverride() != "" && m_context.scenePasses.lighting.envMapOverride() != c_EnvMapSceneDefault)
        envMapActualPath = (IsProceduralSky(m_context.scenePasses.lighting.envMapOverride().c_str())) ? (m_context.scenePasses.lighting.envMapOverride()) : (std::string(c_EnvMapSubFolder) + "/" + m_context.scenePasses.lighting.envMapOverride());

    if (!envMapActualPath.empty() && !IsProceduralSky(envMapActualPath.c_str()))
        envMapActualPath = ResolveSceneMediaPath(envMapActualPath, sceneDirectory).generic_string();

    PreUpdateLightingParams params{
        commandList,
        needNewBindings,
        m_context.scenePasses.lighting.environment().get(),
        m_context.commonPasses,
        envMapActualPath,
        sceneDirectory,
    };
    m_context.renderCore.preUpdateLighting(params);
}
void caustica::render::WorldRenderer::updateLighting(nvrhi::CommandListHandle commandList)
{
    m_context.scenePasses.gaussianSplats.buildEmissionProxyList();

    UpdateLightingParams params{
        m_context.settings,
        commandList,
        m_context.scenePasses.lighting.environment().get(),
        m_context.scenePasses.lighting.lightSampling().get(),
        &m_context.bindingCache,
        m_context.commonPasses,
        m_context.sceneManager.getScene(),
        m_context.scenePasses.lighting.materials(),
        m_context.scenePasses.lighting.opacityMaps(),
        m_context.scenePasses.lighting.envMapSceneParams(),
        m_context.sceneTime,
        m_frameIndex,
        c_envMapRadianceScale,
    };
    if (!m_context.scenePasses.gaussianSplats.emissionProxies().empty())
        params.gaussianSplatEmissionProxies = &m_context.scenePasses.gaussianSplats.emissionProxies();
    m_context.renderCore.updateLighting(params);
}
void caustica::render::WorldRenderer::preUpdatePathTracing( bool resetAccum, nvrhi::CommandListHandle commandList )
{
    const bool resetReferenceOidn = !m_context.settings.RealtimeMode && (resetAccum || m_context.settings.ResetAccumulation || m_context.settings.ReferenceOIDNDenoiserChanged);
    if (resetReferenceOidn || m_context.settings.ReferenceOIDNDenoiserChanged)
    {
        resetReferenceOIDN();
        m_context.settings.ReferenceOIDNDenoiserChanged = false;
    }

    resetAccum |= m_context.settings.ResetAccumulation;
    resetAccum |= m_context.settings.RealtimeMode;

    if (resetAccum)
    {
        m_accumulationSampleIndex = (m_context.settings.AccumulationPreWarmRealtimeCaches)?(-32):(0);
    }
#if ENABLE_DEBUG_VIZUALISATIONS
    if (resetAccum && m_shaderDebug)
        m_shaderDebug->ClearDebugVizTexture(commandList);
#endif

    // profile perf - only makes sense with high accumulation sample counts; only start counting after n-th after it stabilizes
    if( m_accumulationSampleIndex < 16 )
    {
        m_context.diagnostics.benchStart = std::chrono::high_resolution_clock::now( );
        m_context.diagnostics.benchLast = m_context.diagnostics.benchStart;
        m_context.diagnostics.benchFrames = 0;
    } else if( m_accumulationSampleIndex < m_context.settings.AccumulationTarget )
    {
        m_context.diagnostics.benchFrames++;
        m_context.diagnostics.benchLast = std::chrono::high_resolution_clock::now( );
    }
    m_accumulationCompleted = false;
    // 'min' in non-realtime path here is to keep looping the last sample for debugging purposes!
    if( !m_context.settings.RealtimeMode )
    {
        m_sampleIndex = (m_accumulationSampleIndex<0)?(m_accumulationSampleIndex+4096):(min(m_accumulationSampleIndex, m_context.settings.AccumulationTarget - 1));
        m_accumulationCompleted |= m_accumulationSampleIndex == m_context.settings.AccumulationTarget - 1;
    }
    else
        m_sampleIndex = (!m_context.settings.DbgFreezeRealtimeNoiseSeed)?( m_frameIndex % 8192 ):0;     // actual sample index
}
void caustica::render::WorldRenderer::postUpdatePathTracing( )
{
    m_accumulationSampleIndex = std::min( m_accumulationSampleIndex+1, m_context.settings.AccumulationTarget );

    if (m_context.settings.ActualUseRTXDIPasses())
        m_rtxdiPass->EndFrame();

    m_context.settings.ResetAccumulation = false;
    m_context.settings.ResetRealtimeCaches = false;
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
    if (m_context.renderCore.camera().viewPrevious())
        constants.prevCamera.PosW = m_context.renderCore.camera().viewPrevious()->GetInverseViewMatrix().m_translation;

    constants.bounceCount = m_context.settings.BounceCount;
    constants.diffuseBounceCount = m_context.settings.DiffuseBounceCount;
    constants.perPixelJitterAAScale = (m_context.settings.RealtimeMode == false && m_context.settings.AccumulationAA)?(1):( (m_context.settings.RealtimeMode && m_context.settings.RealtimeAA == 3)?(m_context.settings.DLSSRRMicroJitter):(0.0f) );

    // needed to allow super-resolution to work best
    float dlssBias = -dm::log2f(sqrtf((m_displaySize.x * m_displaySize.y) / float(m_renderSize.x * m_renderSize.y)));

    constants.texLODBias = m_context.settings.TexLODBias + dlssBias;
    constants.sampleBaseIndex = m_sampleIndex * m_context.settings.ActualSamplesPerPixel();

    //constants.subSampleCount = m_context.settings.ActualSamplesPerPixel();
    constants.invSubSampleCount = 1.0f / (float)m_context.settings.ActualSamplesPerPixel();

    constants.imageWidth = m_renderSize.x; assert( m_renderSize.x == m_renderTargets->OutputColor->getDesc().width );
    constants.imageHeight = m_renderSize.y; assert( m_renderSize.y == m_renderTargets->OutputColor->getDesc().height );

    // this is the dynamic luminance that when passed through current tonemapper with current exposure settings, produces the same 50% gray
    constants.preExposedGrayLuminance = m_context.settings.EnableToneMapping?(dm::luminance(m_toneMappingPass->GetPreExposedGray(0))):(1.0f);

    const float disabledFF = 0.0f;
    if (m_context.settings.RealtimeMode)
        constants.fireflyFilterThreshold = (m_context.settings.RealtimeFireflyFilterEnabled)?(m_context.settings.RealtimeFireflyFilterThreshold*sqrtf(constants.preExposedGrayLuminance)*1e3f):(disabledFF); // it does make sense to make the realtime variant dependent on avg luminance - just didn't have time to try it out yet
    else
        constants.fireflyFilterThreshold = (m_context.settings.ReferenceFireflyFilterEnabled)?(m_context.settings.ReferenceFireflyFilterThreshold*sqrtf(constants.preExposedGrayLuminance)*1e3f):(disabledFF); // making it exposure-adaptive breaks determinism with accumulation (because there's a feedback loop), so that's disabled
    constants.useReSTIRDI = m_context.settings.ActualUseReSTIRDI();
    constants.useReSTIRGI = m_context.settings.ActualUseReSTIRGI();
    constants.useReSTIRPT = m_context.settings.ActualUseReSTIRPT();
    constants.environmentMapVisibleToCamera = m_context.settings.EnvironmentMapParams.VisibleToCamera ? 1u : 0u;
    constants.denoiserRadianceClampK = m_context.settings.DenoiserRadianceClampK;
    constants.DLSSRRBrightnessClampK = (m_context.settings.DLSSRRBrightnessClampK>0)?(m_context.settings.DLSSRRBrightnessClampK * constants.preExposedGrayLuminance):(0.0f);

    // no stable planes by default
    constants.denoisingEnabled = m_context.settings.ActualUseStandaloneDenoiser() || m_context.settings.RealtimeAA == 3;

    constants._activeStablePlaneCount           = m_context.settings.StablePlanesActiveCount;
    constants.maxStablePlaneVertexDepth         = std::min( std::min( (uint)m_context.settings.StablePlanesMaxVertexDepth, cStablePlaneMaxVertexIndex ), (uint)m_context.settings.BounceCount );
    constants.allowPrimarySurfaceReplacement    = m_context.settings.AllowPrimarySurfaceReplacement;
    constants.stablePlanesSplitStopThreshold    = m_context.settings.StablePlanesSplitStopThreshold;
    constants._padding3                         = 0;
    constants.stablePlanesSuppressPrimaryIndirectSpecularK  = m_context.settings.StablePlanesSuppressPrimaryIndirectSpecular?m_context.settings.StablePlanesSuppressPrimaryIndirectSpecularK:0.0f;
    constants.stablePlanesAntiAliasingFallthrough = m_context.settings.StablePlanesAntiAliasingFallthrough;
    constants.frameIndex                        = m_frameIndex & 0xFFFFFFFF; //m_context.gpuDevice.GetFrameIndex();
    constants.genericTSLineStride               = GenericTSComputeLineStride(constants.imageWidth, constants.imageHeight);
    constants.genericTSPlaneStride              = GenericTSComputePlaneStride(constants.imageWidth, constants.imageHeight);

    constants.NEEEnabled                        = m_context.settings.UseNEE;
    constants.NEEType                           = m_context.settings.NEEType;
    constants.NEECandidateSamples               = m_context.settings.NEECandidateSamples;
    constants.NEEFullSamples                    = m_context.settings.NEEFullSamples;

    constants.EnvironmentMapDiffuseSampleMIPLevel = m_context.settings.EnvironmentMapDiffuseSampleMIPLevel;

#if CAUSTICA_STOCHASTIC_TEXTURE_FILTERING_ENABLE
    // stochastic texture filtering type and size.
    // constants.STFUseBlueNoise                   = m_context.settings.STFUseBlueNoise;
    constants.STFMagnificationMethod            = GetStfMagnificationMethod(m_context.settings.STFMagnificationMethod);
    constants.STFFilterMode                     = GetStfFilterMode(m_context.settings.STFFilterMode);
    constants.STFGaussianSigma                  = m_context.settings.STFGaussianSigma;
#endif
}
void caustica::render::WorldRenderer::rtxdiSetupFrame(nvrhi::IFramebuffer* framebuffer, PathTracerCameraData cameraData, uint2 renderDims)
{
    const bool envMapPresent = m_context.settings.EnvironmentMapParams.Enabled;

    RtxdiBridgeParameters bridgeParameters;
	bridgeParameters.frameIndex = m_frameIndex & 0xFFFFFFFF;
	bridgeParameters.frameDims = renderDims;
	bridgeParameters.cameraPosition = m_context.renderCore.camera().camera().GetPosition();
	bridgeParameters.userSettings = m_context.settings.RTXDI;
    bridgeParameters.usingLightSampling = m_context.settings.ActualUseReSTIRDI();
    bridgeParameters.usingReGIR = m_context.settings.ActualUseReSTIRDI();

    bridgeParameters.userSettings.restirDI.initialSamplingParams.environmentMapImportanceSampling = envMapPresent;

    m_context.scenePasses.gaussianSplats.buildEmissionProxyList();
    if (!m_context.scenePasses.gaussianSplats.emissionProxies().empty() && m_context.scenePasses.gaussianSplats.isEmissionEnabled())
    {
        bridgeParameters.gaussianSplatEmissionProxies = &m_context.scenePasses.gaussianSplats.emissionProxies();
        bridgeParameters.gaussianSplatEmissionObjectToWorld = float4x4::identity();
        bridgeParameters.gaussianSplatEmissionIntensity = m_context.settings.GaussianSplatEmissionIntensity;
    }

    if( m_context.settings.ResetRealtimeCaches )
        m_rtxdiPass->Reset();

	m_rtxdiPass->PrepareResources(m_commandList, *m_renderTargets, envMapPresent ? m_context.scenePasses.lighting.environment() : nullptr, m_context.scenePasses.lighting.envMapSceneParams(),
        m_context.sceneManager.getScene(), m_context.scenePasses.lighting.materials(), m_context.scenePasses.lighting.opacityMaps(), m_context.renderCore.accelStructs().getSubInstanceBuffer(), bridgeParameters, m_bindingLayout, m_shaderDebug );
 }
#if CAUSTICA_WITH_STREAMLINE
void caustica::render::WorldRenderer::streamlinePreRender()
{
#if CAUSTICA_WITH_STREAMLINE
    if (m_context.gpuDevice.IsHeadless())
        return;

    auto& streamline = m_context.gpuDevice.GetStreamline();
    m_context.settings.IsDLSSSuported = streamline.IsDLSSAvailable();
    m_context.settings.IsDLSSRRSupported = streamline.IsDLSSRRAvailable();
    m_context.settings.IsDLSSFGSupported = streamline.IsDLSSGAvailable();
    m_context.settings.IsReflexSupported = streamline.IsReflexAvailable();

    // Setup Reflex
    {
        auto reflexConsts = caustica::StreamlineInterface::ReflexOptions{};
        reflexConsts.mode = (caustica::StreamlineInterface::ReflexMode) m_context.settings.ActualReflexMode();
        reflexConsts.frameLimitUs = m_context.settings.ReflexCappedFps == 0 ? 0 : int(1000000. / m_context.settings.ReflexCappedFps);
        reflexConsts.useMarkersToOptimize = true;
        reflexConsts.virtualKey = VK_F13;
        reflexConsts.idThread = 0; // std::hash<std::thread::id>()(std::this_thread::get_id())
        streamline.SetReflexConsts(reflexConsts);

        // Need to update StreamlineIntegration with the ability to query reflex state
        caustica::StreamlineInterface::ReflexState reflexState{};
        streamline.GetReflexState(reflexState);
        if (m_context.settings.IsReflexSupported)
        {
            m_context.settings.IsReflexLowLatencyAvailable = reflexState.lowLatencyAvailable;
            m_context.settings.IsReflexFlashIndicatorDriverControlled = reflexState.flashIndicatorDriverControlled;

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

                m_context.settings.ReflexStats = "frameID: " + std::to_string(frameID);
                m_context.settings.ReflexStats += "\ntotalGameToRenderLatencyUs: " + std::to_string(totalGameToRenderLatencyUs);
                m_context.settings.ReflexStats += "\nsimDeltaUs: " + std::to_string(simDeltaUs);
                m_context.settings.ReflexStats += "\nrenderDeltaUs: " + std::to_string(renderDeltaUs);
                m_context.settings.ReflexStats += "\npresentDeltaUs: " + std::to_string(presentDeltaUs);
                m_context.settings.ReflexStats += "\ndriverDeltaUs: " + std::to_string(driverDeltaUs);
                m_context.settings.ReflexStats += "\nosRenderQueueDeltaUs: " + std::to_string(osRenderQueueDeltaUs);
                m_context.settings.ReflexStats += "\ngpuRenderDeltaUs: " + std::to_string(gpuRenderDeltaUs);
            }
            else
            {
                m_context.settings.ReflexStats = "Latency Report Unavailable";
            }
        }
    }

    // DLSS-G Setup
    {
        const auto actualDLSSFGMode = m_context.settings.ActualDLSSFGMode();
        const bool wasDLSSFGEnabled = m_context.settings.DLSSFGOptions.mode == StreamlineInterface::DLSSGMode::eOn;

        // If DLSS-G has been turned off, then we tell tell SL to clean it up expressly
        if (wasDLSSFGEnabled && actualDLSSFGMode == StreamlineInterface::DLSSGMode::eOff) {
            streamline.CleanupDLSSG(true);
        }

        // This is where DLSS-G is toggled On and Off (using dlssgOptions.mode) and where we set DLSS-G parameters.
        auto dlssgOptions = StreamlineInterface::DLSSGOptions{};
        dlssgOptions.mode = actualDLSSFGMode;
        dlssgOptions.numFramesToGenerate = m_context.settings.DLSSFGNumFramesToGenerate;

        // This is where we query DLSS-G minimum swapchain size
        if (m_context.settings.IsDLSSFGSupported &&
            (actualDLSSFGMode != StreamlineInterface::DLSSGMode::eOff || wasDLSSFGEnabled))
        {
            StreamlineInterface::DLSSGState state;
            streamline.GetDLSSGState(state, dlssgOptions);
            m_context.settings.DLSSFGMultiplier = state.numFramesActuallyPresented;
            m_context.settings.DLSSFGMaxNumFramesToGenerate = state.numFramesToGenerateMax;

            streamline.SetDLSSGOptions(dlssgOptions);
            m_context.settings.DLSSFGOptions = dlssgOptions;
        }
        else
        {
            m_context.settings.DLSSFGMultiplier = 1;
            m_context.settings.DLSSFGOptions = dlssgOptions;
        }
    }

    // Ensure DLSS / DLSS-RR is available
    if (m_context.settings.RealtimeAA == 3 && !m_context.settings.IsDLSSRRSupported)
    {
        caustica::warning("Requested DLSS-RR mode not available. Switching to DLSS. ");
        m_context.settings.RealtimeAA = 2;
    }
    if ( m_context.settings.RealtimeAA == 2 && !m_context.settings.IsDLSSSuported )
    {
        caustica::warning("Requested DLSS mode not available. Switching to TAA. ");
        m_context.settings.RealtimeAA = 1;
    }

    // Setup DLSS
    const bool changeToDLSSMode = (m_context.settings.RealtimeAA >= 2 && m_context.settings.RealtimeAA <= 3) && m_context.settings.DLSSLastRealtimeAA != m_context.settings.RealtimeAA;
    {
        // Reset DLSS vars if we stop using it
        if (changeToDLSSMode || m_context.settings.DLSSMode == StreamlineInterface::DLSSMode::eOff)
        {
            m_context.settings.DLSSLastMode = PathTracerSettings::DLSSModeDefault;
            m_context.settings.DLSSMode = PathTracerSettings::DLSSModeDefault;
            m_context.settings.DLSSLastDisplaySize = { 0,0 };
        }

        m_context.settings.DLSSLastRealtimeAA = m_context.settings.RealtimeAA;

        // If we are using DLSS set its constants
        if ((m_context.settings.RealtimeAA == 2 || m_context.settings.RealtimeAA == 3) && m_context.settings.RealtimeMode)
        {
            StreamlineInterface::DLSSOptions dlssOptions = {};
            if (m_context.settings.IsDLSSSuported)
            {
                dlssOptions.mode = m_context.settings.DLSSMode;
                dlssOptions.outputWidth = m_displaySize.x;
                dlssOptions.outputHeight = m_displaySize.y;
                dlssOptions.sharpness = 0; //m_recommendedDLSSSettings.sharpness;    // <- is this no longer valid?
                dlssOptions.colorBuffersHDR = true;
                dlssOptions.useAutoExposure = true;     // Optional: provide proper "kBufferTypeExposure" for 0-lag for better precision handling
                dlssOptions.preset = StreamlineInterface::DLSSPreset::eDefault;
                // if (m_context.settings.RealtimeAA < 4) <- docs https://github.com/NVIDIAGameWorks/Streamline/blob/main/docs/ProgrammingGuideDLSS_RR.md#50-provide-dlss--dlss-rr-options seem to imply that these should be set even when DLSS-RR enabled
                    streamline.SetDLSSOptions(dlssOptions);
            }
            else
            {
                assert( false ); // shouldn't happen, code above should have dropped us to "m_context.settings.RealtimeAA = 1" - check for recent code changes.
            }

            if (m_context.settings.RealtimeAA == 2 || m_context.settings.RealtimeAA == 3)
            {
                // Check if we need to update the rendertarget size.
                bool dlssResizeRequired = (m_context.settings.DLSSMode != m_context.settings.DLSSLastMode) || (m_displaySize.x != m_context.settings.DLSSLastDisplaySize.x) || (m_displaySize.y != m_context.settings.DLSSLastDisplaySize.y);
                if (dlssResizeRequired)
                {
                    // Only quality, target width and height matter here
                    streamline.QueryDLSSOptimalSettings(dlssOptions, m_recommendedDLSSSettings);

                    // this is an example on how to override defaults - overriding default 2/3 to higher res 3/4
                    if (dlssOptions.mode == SI::DLSSMode::eMaxQuality)
                    {
                        m_recommendedDLSSSettings.optimalRenderSize.x = dm::clamp((int)(dlssOptions.outputWidth * 3 / 4 + 0.5f), m_recommendedDLSSSettings.minRenderSize.x, m_recommendedDLSSSettings.maxRenderSize.x);
                        m_recommendedDLSSSettings.optimalRenderSize.y = dm::clamp((int)(dlssOptions.outputHeight * 3 / 4 + 0.5f), m_recommendedDLSSSettings.minRenderSize.y, m_recommendedDLSSSettings.maxRenderSize.y);
                    }

                    if (m_recommendedDLSSSettings.optimalRenderSize.x <= 0 || m_recommendedDLSSSettings.optimalRenderSize.y <= 0)
                    {
                        m_context.settings.RealtimeAA = 0;
                        m_context.settings.DLSSMode = PathTracerSettings::DLSSModeDefault;
                        m_renderSize = m_displaySize;
                    }
                    else
                    {
                        m_context.settings.DLSSLastMode = m_context.settings.DLSSMode;
                        m_context.settings.DLSSLastDisplaySize = m_displaySize;
                    }
                }

                m_renderSize = (uint2)m_recommendedDLSSSettings.optimalRenderSize;
            }

            if (m_context.settings.RealtimeAA == 3) // DLSS-RR
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
                dlssRROptions.preset                = m_context.settings.DLSRRPreset;
                m_lastDLSSRROptions = dlssRROptions; // we need to fill them up with view info, but we can only have proper view after it was initialized with correct RT size
            }
        }
        else
        {
            if (m_context.settings.IsDLSSSuported)
            {
                StreamlineInterface::DLSSOptions dlssOptions = {};
                dlssOptions.mode = StreamlineInterface::DLSSMode::eOff;
                streamline.SetDLSSOptions(dlssOptions);
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
    if (!m_context.settings.RealtimeMode)
    {
        m_renderSize = m_displaySize;
        return;
    }

    if (m_nativeDLSS)
    {
        m_context.settings.IsDLSSSuported = m_nativeDLSS->IsDlssSupported();
        m_context.settings.IsDLSSRRSupported = m_nativeDLSS->IsRayReconstructionSupported();
    }

    if (m_context.settings.RealtimeAA == 3 && !m_context.settings.IsDLSSRRSupported)
    {
        caustica::warning("Requested DLSS-RR mode not available. Switching to DLSS.");
        m_context.settings.RealtimeAA = 2;
    }

    if (m_context.settings.RealtimeAA == 2 && !m_context.settings.IsDLSSSuported)
    {
        caustica::warning("Requested DLSS mode not available. Switching to TAA.");
        m_context.settings.RealtimeAA = 1;
    }

    const bool usingDLSS = (m_context.settings.RealtimeAA == 2 || m_context.settings.RealtimeAA == 3);
    const bool changeToDLSSMode = usingDLSS && m_context.settings.DLSSLastRealtimeAA != m_context.settings.RealtimeAA;

    if (changeToDLSSMode || m_context.settings.DLSSMode == SI::DLSSMode::eOff)
    {
        m_context.settings.DLSSLastMode = PathTracerSettings::DLSSModeDefault;
        m_context.settings.DLSSMode = PathTracerSettings::DLSSModeDefault;
        m_context.settings.DLSSLastDisplaySize = { 0, 0 };
    }

    m_context.settings.DLSSLastRealtimeAA = m_context.settings.RealtimeAA;

    if (usingDLSS)
    {
        const bool dlssResizeRequired =
            (m_context.settings.DLSSMode != m_context.settings.DLSSLastMode) ||
            (m_displaySize.x != m_context.settings.DLSSLastDisplaySize.x) ||
            (m_displaySize.y != m_context.settings.DLSSLastDisplaySize.y);

        if (dlssResizeRequired)
        {
            m_context.settings.DLSSLastMode = m_context.settings.DLSSMode;
            m_context.settings.DLSSLastDisplaySize = m_displaySize;
        }

        m_renderSize = GetNativeDLSSRenderSize(m_displaySize, m_context.settings.DLSSMode);
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
    if (m_context.settings.ActualFPSLimiter() > 0)
        g_FPSLimiter.FramerateLimit(m_context.settings.ActualFPSLimiter());

    korgi::Update();
}
void caustica::render::WorldRenderer::postProcessPreToneMapping(nvrhi::ICommandList* commandList, const caustica::ICompositeView& compositeView)
{
    (void)compositeView;

    HdrPostProcessParams hdrParams{
        m_context.settings,
        commandList,
        m_renderTargets.get(),
        m_displaySize,
        m_bloomPass.get(),
    };
    m_context.renderCore.hdrPostProcess(hdrParams);

    if (m_context.settings.PostProcessTestPassHDR)
    {
        commandList->beginMarker("TestRaygenPP_HDR");

        commandList->setTextureState(m_renderTargets->ProcessedOutputColor, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);

        nvrhi::rt::DispatchRaysArguments args;
        args.width = m_displaySize.x;
        args.height = m_displaySize.y;

        nvrhi::rt::State state;
        state.shaderTable = m_ptPipelineTestRaygenPPHDR->GetShaderTable();
        state.bindings = { m_bindingSet, m_context.descriptorTable->GetDescriptorTable() };
        commandList->setRayTracingState(state);

        SampleMiniConstants miniConstants = { uint4(0, 0, 0, 0) };
        commandList->setPushConstants(&miniConstants, sizeof(miniConstants));
        commandList->dispatchRays(args);

        commandList->setTextureState(m_renderTargets->ProcessedOutputColor, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);

        commandList->endMarker();
    }
}
void caustica::render::WorldRenderer::postProcessPostToneMapping(nvrhi::ICommandList* commandList, const caustica::ICompositeView& compositeView)
{ // a.k.a. LDR post-process (e.g. colour filters go here)
    if (m_context.settings.PostProcessEdgeDetection)
    {
        m_commandList->beginMarker("PPEdgeDetection");

        m_commandList->copyTexture(m_renderTargets->LdrColorScratch, nvrhi::TextureSlice(), m_renderTargets->LdrColor, nvrhi::TextureSlice());

        nvrhi::rt::DispatchRaysArguments args;
        args.width  = m_displaySize.x;
        args.height = m_displaySize.y;

        nvrhi::rt::State state;
        state.shaderTable = m_ptPipelineEdgeDetection->GetShaderTable();
        state.bindings = { m_bindingSet, m_context.descriptorTable->GetDescriptorTable() };
        m_commandList->setRayTracingState(state);

        SampleMiniConstants miniConstants = { uint4( *reinterpret_cast<uint*>(&m_context.settings.PostProcessEdgeDetectionThreshold), 0, 0, 0) };
        m_commandList->setPushConstants(&miniConstants, sizeof(miniConstants));
        m_commandList->dispatchRays(args);

        m_commandList->setTextureState(m_renderTargets->LdrColor, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);

        m_commandList->endMarker();
    }
}
void caustica::render::WorldRenderer::renderGaussianSplats(bool renderToOutputColor)
{
    if (!m_context.settings.EnableGaussianSplats || m_context.scenePasses.gaussianSplats.objectsEmpty())
        return;

    const bool stochasticSplats = m_context.settings.EnableGaussianSplats && m_context.settings.GaussianSplatSortingMode == 1;
    if (stochasticSplats && (m_context.settings.ResetAccumulation || m_context.settings.ResetRealtimeCaches || m_gaussianSplatTemporalReset))
        m_gaussianSplatTemporalSampleIndex = 0;

    const uint32_t gaussianSplatShadowMode = ResolveGaussianSplatShadowMode(m_context.settings);
    GaussianSplatRenderSettings settings;
    settings.enabled = m_context.settings.EnableGaussianSplats;
    settings.depthTest = m_context.settings.GaussianSplatDepthTest;
    settings.sortingMode = m_context.settings.GaussianSplatSortingMode == 1 ? GaussianSplatSortMode::StochasticSplats : GaussianSplatSortMode::GpuSort;
    settings.renderTarget = renderToOutputColor ? GaussianSplatRenderTarget::OutputColor : GaussianSplatRenderTarget::ProcessedOutputColor;
    settings.frustumCulling = static_cast<GaussianSplatFrustumCulling>(dm::clamp(m_context.settings.GaussianSplatFrustumCulling, 0, 2));
    settings.projectionMethod = GaussianSplatProjectionMethod::Eigen;
    settings.shFormat = static_cast<GaussianSplatStorageFormat>(dm::clamp(m_context.settings.GaussianSplatSHFormat, 0, 2));
    settings.rgbaFormat = static_cast<GaussianSplatStorageFormat>(dm::clamp(m_context.settings.GaussianSplatRGBAFormat, 0, 2));
    settings.screenSizeCulling = m_context.settings.GaussianSplatScreenSizeCulling;
    settings.mipSplattingAntialiasing = m_context.settings.GaussianSplatMipAntialiasing;
    settings.useAABBs = m_context.settings.GaussianSplatUseAABBs;
    settings.useTLASInstances = m_context.settings.GaussianSplatUseTLASInstances;
    settings.blasCompaction = m_context.settings.GaussianSplatBlasCompaction;
    settings.splatScale = m_context.settings.GaussianSplatScale;
    settings.alphaScale = m_context.settings.GaussianSplatAlphaScale;
    settings.brightness = m_context.settings.GaussianSplatBrightness;
    settings.tintColor = m_context.settings.GaussianSplatTintColor;
    settings.alphaCullThreshold = m_context.settings.GaussianSplatAlphaCullThreshold;
    settings.shadowsEnabled = gaussianSplatShadowMode != GAUSSIAN_SPLAT_SHADOWS_DISABLED;
    settings.shadowMode = gaussianSplatShadowMode;
    settings.shadowStrength = m_context.settings.GaussianSplatShadowStrength;
    settings.shadowRayOffset = m_context.settings.GaussianSplatRtxParticleShadowOffset;
    settings.shadowSoftRadius = m_context.settings.GaussianSplatShadowSoftRadius;
    settings.shadowSoftSampleCount = ClampGaussianSplatSoftShadowSamples(m_context.settings.GaussianSplatShadowSoftSampleCount);
    settings.shadowFrameIndex = uint32_t(m_frameIndex & 0xffffffffu);
    settings.frustumDilation = m_context.settings.GaussianSplatFrustumDilation;
    settings.minPixelCoverage = m_context.settings.GaussianSplatMinPixelCoverage;
    if (stochasticSplats && m_context.settings.RealtimeMode)
        settings.stochasticFrameIndex = uint32_t(m_gaussianSplatTemporalSampleIndex);
    else
        settings.stochasticFrameIndex = uint32_t(m_sampleIndex >= 0
            ? uint32_t(m_sampleIndex)
            : uint32_t(m_frameIndex & 0xffffffffu));
    {
        auto scene = m_context.sceneManager.getScene();
        if (scene)
        {
            const auto* ew = scene->GetEntityWorld();
            if (ew)
            {
                for (ecs::Entity entity : scene->GetLightEntities())
                {
                    const auto* lightComp = caustica::scene::TryGetLight(ew->world(), entity);
                    if (!lightComp || !caustica::scene::TryGetDirectionalLightData(*lightComp))
                        continue;
                    const auto* globalComp = ew->world().get<caustica::scene::GlobalTransformComponent>(entity);
                    if (!globalComp)
                        continue;
                    LightConstants lightConstants;
                    caustica::scene::FillLightConstants(*lightComp, globalComp->transform, lightConstants);
                    settings.shadowDirectionToLight = -lightConstants.direction;
                    break;
                }
            }
        }
    }

    caustica::PlanarView splatView = *m_context.renderCore.camera().view();
    if (!renderToOutputColor)
    {
        splatView.SetViewport(nvrhi::Viewport(float(m_displaySize.x), float(m_displaySize.y)));
        splatView.SetPixelOffset(dm::float2::zero());
    }
    splatView.UpdateCache();

    bool renderedAny = false;
    m_context.scenePasses.gaussianSplats.renderSceneGaussianSplats(m_commandList, splatView, *m_renderTargets, settings, renderedAny);

    if (renderedAny && stochasticSplats && !renderToOutputColor)
        accumulateGaussianSplats(splatView);
}
void caustica::render::WorldRenderer::accumulateGaussianSplats(const caustica::IView& splatView)
{
    if (m_gaussianSplatAccumulationPass == nullptr || m_renderTargets == nullptr || m_gaussianSplatCurrentColor == nullptr || m_gaussianSplatAccumulatedColor == nullptr)
        return;

    if (m_context.settings.ResetAccumulation || m_context.settings.ResetRealtimeCaches || m_gaussianSplatTemporalReset)
    {
        m_gaussianSplatTemporalSampleIndex = 0;
        m_gaussianSplatTemporalReset = false;
    }

    const float accumulationWeight = 1.0f / float(m_gaussianSplatTemporalSampleIndex + 1);

    m_commandList->setTextureState(m_renderTargets->ProcessedOutputColor, nvrhi::AllSubresources, nvrhi::ResourceStates::CopySource);
    m_commandList->setTextureState(m_gaussianSplatCurrentColor, nvrhi::AllSubresources, nvrhi::ResourceStates::CopyDest);
    m_commandList->commitBarriers();
    m_commandList->copyTexture(m_gaussianSplatCurrentColor, nvrhi::TextureSlice(), m_renderTargets->ProcessedOutputColor, nvrhi::TextureSlice());

    m_commandList->setTextureState(m_gaussianSplatCurrentColor, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    m_commandList->setTextureState(m_gaussianSplatAccumulatedColor, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    m_commandList->setTextureState(m_renderTargets->ProcessedOutputColor, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    m_commandList->commitBarriers();

    m_gaussianSplatAccumulationPass->Render(m_commandList, splatView, splatView, accumulationWeight);

    m_gaussianSplatTemporalSampleIndex = std::min(m_gaussianSplatTemporalSampleIndex + 1, 1024 * 1024);
}
void caustica::render::WorldRenderer::render(nvrhi::IFramebuffer* framebuffer)
{
    m_displaySize = m_renderSize = uint2(
        framebuffer->getFramebufferInfo().width,
        framebuffer->getFramebufferInfo().height);

    PathTracingFrameContext ctx{};
    ctx.renderer = this;
    ctx.framebuffer = framebuffer;
    ctx.displaySize = m_displaySize;
    ctx.renderSize = m_renderSize;

    ensureFramePipelineBuilt();
    m_framePipeline->executeAll(ctx);

    if (ctx.aborted)
        postUpdatePathTracing();
}
void caustica::render::WorldRenderer::recreateBindingSet()
{
	// WARNING: this must match the layout of the m_bindingLayout (or switch to CreateBindingSetAndLayout)
    nvrhi::rt::IAccelStruct* gaussianSplatAS = m_context.renderCore.accelStructs().getTopLevelAS();
    nvrhi::IBuffer* gaussianSplatBuffer = m_context.scenePasses.lighting.materials()->GetMaterialDataBuffer();
    const GaussianSplatBinding primaryGaussianBinding = m_context.scenePasses.gaussianSplats.getPrimaryBinding();
    const GaussianSplatPass* primaryGaussianSplatPass = primaryGaussianBinding.splatPass;
    if (primaryGaussianSplatPass != nullptr && primaryGaussianSplatPass->GetTopLevelAS() != nullptr && primaryGaussianSplatPass->GetSplatBuffer() != nullptr)
    {
        gaussianSplatAS = primaryGaussianSplatPass->GetTopLevelAS();
        gaussianSplatBuffer = primaryGaussianSplatPass->GetSplatBuffer();
    }

    // Fixed resources that do not change between binding sets
    nvrhi::BindingSetDesc bindingSetDescBase;
    bindingSetDescBase.bindings = {
        nvrhi::BindingSetItem::ConstantBuffer(0, m_constantBuffer),
        nvrhi::BindingSetItem::PushConstants(1, sizeof(SampleMiniConstants)),
        //nvrhi::BindingSetItem::ConstantBuffer(2, m_context.scenePasses.lighting.lightSampling()->GetLightingConstants()),
        nvrhi::BindingSetItem::RayTracingAccelStruct(0, m_context.renderCore.accelStructs().getTopLevelAS()),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(1, m_context.renderCore.accelStructs().getSubInstanceBuffer()),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(2, m_context.sceneManager.getScene()->GetGpuResources().instanceBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(3, m_context.sceneManager.getScene()->GetGpuResources().geometryBuffer),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(4, m_context.scenePasses.lighting.opacityMaps() ?(m_context.scenePasses.lighting.opacityMaps()->GetGeometryDebugBuffer()):(m_context.scenePasses.lighting.materials()->GetMaterialDataBuffer().Get()) ),   // YUCK
        nvrhi::BindingSetItem::StructuredBuffer_SRV(5, m_context.scenePasses.lighting.materials()->GetMaterialDataBuffer()),
        nvrhi::BindingSetItem::Texture_SRV(6,  m_renderTargets->LdrColorScratch, nvrhi::Format::SRGBA8_UNORM),
        nvrhi::BindingSetItem::RayTracingAccelStruct(7, gaussianSplatAS),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(8, gaussianSplatBuffer),
        nvrhi::BindingSetItem::Texture_SRV(10, m_context.scenePasses.lighting.environment()->GetEnvMapCube()), //m_app.m_EnvironmentMap->IsEnvMapLoaded() ? m_app.m_EnvironmentMap->GetEnvironmentMap() : (m_context.commonPasses)->m_BlackTexture),
        nvrhi::BindingSetItem::Texture_SRV(11, m_context.scenePasses.lighting.environment()->GetImportanceSampling()->GetImportanceMapOnly()), //m_app.m_EnvironmentMap->IsImportanceMapLoaded() ? m_app.m_EnvironmentMap->GetImportanceMap() : (m_context.commonPasses)->m_BlackTexture),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(12, m_context.scenePasses.lighting.lightSampling()->GetControlBuffer()),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(13, m_context.scenePasses.lighting.lightSampling()->GetLightBuffer()),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(14, m_context.scenePasses.lighting.lightSampling()->GetLightExBuffer()),
        nvrhi::BindingSetItem::TypedBuffer_SRV(15, m_context.scenePasses.lighting.lightSampling()->GetLightProxyCounters()),     // t_tightProxyCounters
        nvrhi::BindingSetItem::TypedBuffer_SRV(16, m_context.scenePasses.lighting.lightSampling()->GetLightSamplingProxies()),   // t_LightProxyIndices
        nvrhi::BindingSetItem::TypedBuffer_SRV(17, m_context.scenePasses.lighting.lightSampling()->GetLocalSamplingBuffer()),    // t_LightLocalSamplingBuffer
        nvrhi::BindingSetItem::Texture_SRV(18, m_context.scenePasses.lighting.lightSampling()->GetEnvLightLookupMap()),          // t_EnvLookupMap
        //nvrhi::BindingSetItem::TypedBuffer_SRV(19, ),
        nvrhi::BindingSetItem::Texture_UAV(20, m_context.scenePasses.lighting.lightSampling()->GetFeedbackTotalWeight()),        // u_LightFeedbackTotalWeight
        nvrhi::BindingSetItem::Texture_UAV(21, m_context.scenePasses.lighting.lightSampling()->GetFeedbackCandidates()),         // u_LightFeedbackCandidates
        nvrhi::BindingSetItem::Sampler(0, (m_context.commonPasses)->m_AnisotropicWrapSampler),
        nvrhi::BindingSetItem::Sampler(1, m_context.scenePasses.lighting.environment()->GetEnvMapCubeSampler()),
        nvrhi::BindingSetItem::Sampler(2, m_context.scenePasses.lighting.environment()->GetImportanceSampling()->GetImportanceMapSampler()),
        nvrhi::BindingSetItem::Texture_UAV(0, m_renderTargets->OutputColor),
        nvrhi::BindingSetItem::Texture_UAV(1, m_renderTargets->ProcessedOutputColor),
        nvrhi::BindingSetItem::Texture_UAV(2, m_renderTargets->LdrColor, nvrhi::Format::RGBA8_UNORM),
        nvrhi::BindingSetItem::Texture_UAV(4, m_renderTargets->Throughput),
        nvrhi::BindingSetItem::Texture_UAV(5, m_renderTargets->ScreenMotionVectors),
        nvrhi::BindingSetItem::Texture_UAV(6, m_renderTargets->Depth),
        nvrhi::BindingSetItem::Texture_UAV(7, m_renderTargets->SpecularHitT), 
        nvrhi::BindingSetItem::Texture_UAV(8, m_renderTargets->ScratchFloat1), 
        nvrhi::BindingSetItem::Texture_UAV(31, m_renderTargets->DenoiserViewspaceZ),
        nvrhi::BindingSetItem::Texture_UAV(32, m_renderTargets->DenoiserMotionVectors),
        nvrhi::BindingSetItem::Texture_UAV(33, m_renderTargets->DenoiserNormalRoughness),
        nvrhi::BindingSetItem::Texture_UAV(34, m_renderTargets->DenoiserDiffRadianceHitDist),
        nvrhi::BindingSetItem::Texture_UAV(35, m_renderTargets->DenoiserSpecRadianceHitDist),
        nvrhi::BindingSetItem::Texture_UAV(36, m_renderTargets->DenoiserDisocclusionThresholdMix),
        nvrhi::BindingSetItem::Texture_UAV(37, m_renderTargets->CombinedHistoryClampRelax),
        nvrhi::BindingSetItem::StructuredBuffer_UAV(51, m_feedback_Buffer_Gpu),
        nvrhi::BindingSetItem::StructuredBuffer_UAV(52, m_debugLineBufferCapture),
        nvrhi::BindingSetItem::StructuredBuffer_UAV(53, m_debugDeltaPathTree_Gpu),
        nvrhi::BindingSetItem::StructuredBuffer_UAV(54, m_debugDeltaPathTreeSearchStack),
        nvrhi::BindingSetItem::Texture_UAV(60, m_renderTargets->SecondarySurfacePositionNormal),
        nvrhi::BindingSetItem::Texture_UAV(61, m_renderTargets->SecondarySurfaceRadiance),
        nvrhi::BindingSetItem::Texture_UAV(70, m_renderTargets->RRDiffuseAlbedo),
        nvrhi::BindingSetItem::Texture_UAV(71, m_renderTargets->RRSpecAlbedo),
        nvrhi::BindingSetItem::Texture_UAV(72, m_renderTargets->RRNormalsAndRoughness),
        nvrhi::BindingSetItem::Texture_UAV(73, m_renderTargets->RRSpecMotionVectors),
        nvrhi::BindingSetItem::Texture_UAV(74, (m_renderTargets->RRTransparencyLayer!=nullptr)?m_renderTargets->RRTransparencyLayer:m_renderTargets->RRSpecMotionVectors),
        nvrhi::BindingSetItem::Texture_UAV(75, m_renderTargets->DenoiserAvgLayerRadianceHalfRes),

        ///***
        nvrhi::BindingSetItem::Texture_UAV(100, m_renderTargets->BaseColor),
        nvrhi::BindingSetItem::Texture_UAV(101, m_renderTargets->SpecNormal),
        nvrhi::BindingSetItem::Texture_UAV(102, m_renderTargets->RoughnessMetal),
        nvrhi::BindingSetItem::Texture_UAV(103, m_renderTargets->MaterialInfo),
        nvrhi::BindingSetItem::Texture_UAV(10, m_renderTargets->LocalCubemap),  // u_LocalCubemap for RT pass
        ///***

        nvrhi::BindingSetItem::RawBuffer_UAV(SHADER_DEBUG_BUFFER_UAV_INDEX, m_shaderDebug->GetGPUWriteBuffer()),
        nvrhi::BindingSetItem::Texture_UAV(SHADER_DEBUG_VIZ_TEXTURE_UAV_INDEX, m_shaderDebug->GetDebugVizTexture())
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

        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_UAV(40, m_renderTargets->StablePlanesHeader));
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::StructuredBuffer_UAV(42, m_renderTargets->StablePlanesBuffer));
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_UAV(44, m_renderTargets->StableRadiance));
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::StructuredBuffer_UAV(45, m_renderTargets->SurfaceDataBuffer));

        // Reflection system bindings (t80-t83, b3)
        // Derived classes can override AddCustomBindings to provide actual textures
        // Default to black texture fallbacks (NVRHI doesn't allow null textures)
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(80, (m_context.commonPasses)->m_BlackCubeMapArray));  // t_LocalCubemapGGX
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(81, (m_context.commonPasses)->m_BlackCubeMapArray));  // t_DiffuseIrradianceCube
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(82, (m_context.commonPasses)->m_BlackTexture));  // t_SSRBlurChain
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(83, (m_context.scenePasses.lighting.environment()->GetBRDFLUT()!=nullptr)?m_context.scenePasses.lighting.environment()->GetBRDFLUT():(m_context.commonPasses)->m_BlackTexture ));  // t_BRDFLUT
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(84, (m_context.commonPasses)->m_BlackTexture));  // t_DepthHierarchy placeholder
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::ConstantBuffer(10, m_constantBuffer)); // ReflectionConstants (reuse main constant buffer as placeholder)
        
        // SSR result UAV placeholder
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_UAV(85, m_renderTargets->Depth));   // u_SSRResult placeholder

        // GTAO output (default to white = no occlusion; overridden by AddCustomBindings)
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(86, (m_context.commonPasses)->m_WhiteTexture));  // t_GTAOOutput placeholder
        // Previous frame depth (default to black = zero depth; overridden by AddCustomBindings)
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(87, (m_context.commonPasses)->m_BlackTexture));  // t_PrevDepth placeholder

        m_bindingSet = device()->createBindingSet(bindingSetDesc, m_bindingLayout);
    }
}
void caustica::render::WorldRenderer::pathTrace(nvrhi::IFramebuffer* framebuffer, const SampleConstants & constants)
{
    //m_commandList->beginMarker("MainRendering"); <- removed (for now) since added hierarchy reduces readability
    bool useStablePlanes = m_context.settings.RealtimeMode;

    nvrhi::rt::State state;

    nvrhi::rt::DispatchRaysArguments args;
    nvrhi::Viewport viewport = m_context.renderCore.camera().view()->GetViewport();
    uint32_t width = (uint32_t)(viewport.maxX - viewport.minX);
    uint32_t height = (uint32_t)(viewport.maxY - viewport.minY);
    args.width = width;
    args.height = height;

    // default miniConstants
    SampleMiniConstants miniConstants = { uint4(0, 0, 0, 0) };

    if (useStablePlanes)
    {
        if (!m_ptPipelineBuildStablePlanes || !m_ptPipelineFillStablePlanes)
        {
            m_context.scenePasses.rayTracing.ensureStablePlanePipelines();
            assert(m_ptPipelineBuildStablePlanes && m_ptPipelineFillStablePlanes);
        }

        {
            RAII_SCOPE(m_commandList->beginMarker("PathTracePrePass"); , m_commandList->endMarker(); );

            m_commandList->setTextureState(m_renderTargets->Depth, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            m_commandList->setTextureState(m_renderTargets->ScreenMotionVectors, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            m_commandList->setTextureState(m_renderTargets->Throughput, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            m_commandList->setTextureState(m_renderTargets->SpecularHitT, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            state.shaderTable = m_ptPipelineBuildStablePlanes->GetShaderTable();
            state.bindings = { m_bindingSet, m_context.descriptorTable->GetDescriptorTable() };
            m_commandList->setRayTracingState(state);
            m_commandList->setPushConstants(&miniConstants, sizeof(miniConstants));
            m_commandList->dispatchRays(args);
            m_commandList->setBufferState(m_renderTargets->StablePlanesBuffer, nvrhi::ResourceStates::UnorderedAccess);
            m_commandList->setTextureState(m_renderTargets->Depth, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            m_commandList->setTextureState(m_renderTargets->ScreenMotionVectors, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            m_commandList->setTextureState(m_renderTargets->Throughput, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        }

        {
            RAII_SCOPE(m_commandList->beginMarker("VBufferExport"); , m_commandList->endMarker(); );

            nvrhi::ComputeState state;
		    state.bindings = { m_bindingSet, m_context.descriptorTable->GetDescriptorTable() };
            state.pipeline = m_exportVBufferPSO;
            m_commandList->setComputeState(state);

		    const dm::uint2 dispatchSize = { (width + NUM_COMPUTE_THREADS_PER_DIM - 1) / NUM_COMPUTE_THREADS_PER_DIM, (height + NUM_COMPUTE_THREADS_PER_DIM - 1) / NUM_COMPUTE_THREADS_PER_DIM };
            m_commandList->setPushConstants(&miniConstants, sizeof(miniConstants));
		    m_commandList->dispatch(dispatchSize.x, dispatchSize.y);
        }
    }
    else
    {

    }

    // In realtime mode, ScreenMotionVectors reference mode ScreenMotionVectors should be 0
    UpdateLightingEndParams lightingEndParams{
        m_commandList,
        m_context.scenePasses.lighting.lightSampling().get(),
        &m_context.bindingCache,
        m_context.sceneManager.getScene(),
        m_context.scenePasses.lighting.materials(),
        m_context.scenePasses.lighting.opacityMaps(),
        m_context.renderCore.accelStructs().getSubInstanceBuffer(),
        m_renderTargets->Depth,
        m_renderTargets->ScreenMotionVectors,
    };
    m_context.renderCore.updateLightingEnd(lightingEndParams);

    {
        RAII_SCOPE( m_commandList->beginMarker("PathTrace");, m_commandList->endMarker(); );

        state.shaderTable = ((useStablePlanes) ? (m_ptPipelineFillStablePlanes) : (m_ptPipelineReference))->GetShaderTable();
        state.bindings = { m_bindingSet, m_context.descriptorTable->GetDescriptorTable() };

        for (uint subSampleIndex = 0; subSampleIndex < m_context.settings.ActualSamplesPerPixel(); subSampleIndex++)
        {
            // required to avoid race conditions in back to back dispatchRays
            m_commandList->setBufferState(m_renderTargets->StablePlanesBuffer, nvrhi::ResourceStates::UnorderedAccess);
            m_commandList->setTextureState(m_renderTargets->SpecularHitT, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);

            m_commandList->setRayTracingState(state);

            // tell path tracer which subSampleIndex we're processing
            SampleMiniConstants miniConstants = { uint4(subSampleIndex, 0, 0, 0) };//     <- use subSampleIndex to try to figure out why we're losing radiance - is the first one what's left, or the last one?
            m_commandList->setPushConstants(&miniConstants, sizeof(miniConstants));

            m_commandList->dispatchRays(args);
            m_commandList->setTextureState(m_renderTargets->SpecularHitT, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        }

        m_commandList->setBufferState(m_renderTargets->StablePlanesBuffer, nvrhi::ResourceStates::UnorderedAccess);
    }

    // this is a performance optimization where final 2 passes from ReSTIR DI and ReSTIR GI are combined to avoid loading GBuffer twice
    static bool enableFusedDIGIFinal = true;
    bool useFusedDIGIFinal = m_context.settings.ActualUseReSTIRDI() && m_context.settings.ActualUseReSTIRGI() && enableFusedDIGIFinal;

    if (m_context.settings.ActualUseRTXDIPasses())
    {
        RAII_SCOPE( m_commandList->beginMarker("RTXDI");, m_commandList->endMarker(); );

        // this does all ReSTIR DI magic including applying the final sample into correct radiance buffer (depending on denoiser state)
        if (m_context.settings.ActualUseReSTIRDI())
            m_rtxdiPass->Execute(m_commandList, m_bindingSet, useFusedDIGIFinal);

        if (m_context.settings.ActualUseReSTIRGI())
            m_rtxdiPass->ExecuteGI(m_commandList, m_bindingSet, useFusedDIGIFinal);

        if (useFusedDIGIFinal)
            m_rtxdiPass->ExecuteFusedDIGIFinal(m_commandList, m_bindingSet);

        if (m_context.settings.ActualUseReSTIRPT())
            m_rtxdiPass->ExecutePT(m_commandList, m_bindingSet);
    }

    {
        RAII_SCOPE(m_commandList->beginMarker("Denoising Guides Bake"); , m_commandList->endMarker(); );

        m_denoisingGuidesPass->DenoiseSpecHitT(m_commandList, m_bindingSet);
        m_denoisingGuidesPass->ComputeAvgLayerRadiance(m_commandList, m_bindingSet);

        if (m_context.settings.DebugView != DebugViewType::Disabled)
            m_denoisingGuidesPass->RenderDebugViz(m_commandList, m_context.settings.DebugView, m_bindingSet);
    }

    if (useStablePlanes && (m_context.settings.DebugView > DebugViewType::Disabled && m_context.settings.DebugView <= DebugViewType::StablePlane_SpecRadiance || m_context.settings.DebugView == DebugViewType::StableRadiance) )
    {
        m_commandList->beginMarker("StablePlanesDebugViz");
        nvrhi::TextureDesc tdesc = m_renderTargets->OutputColor->getDesc();
        m_postProcess->Apply(m_commandList, PostProcess::ComputePassType::StablePlanesDebugViz, m_constantBuffer, miniConstants, m_bindingSet, m_bindingLayout, tdesc.width, tdesc.height);
        m_commandList->endMarker();

    }
}
void caustica::render::WorldRenderer::denoise(nvrhi::IFramebuffer* framebuffer)
{
    if( !m_context.settings.ActualUseStandaloneDenoiser() )
        return;

    for (int i = 0; i < std::size(m_nrd); i++)
    {
        if (m_nrd[i] == nullptr)
        {
            nrd::Denoiser denoiserMethod = m_context.settings.NRDMethod == NrdConfig::DenoiserMethod::REBLUR ?
                nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR : nrd::Denoiser::RELAX_DIFFUSE_SPECULAR;

            m_nrd[i] = std::make_unique<NrdIntegration>(device(), denoiserMethod);
            m_nrd[i]->Initialize(m_renderSize.x, m_renderSize.y, *m_context.shaderFactory);
        }
    }

    //const auto& fbinfo = framebuffer->getFramebufferInfo();
    const char* passNames[] = { "Denoising plane 0", "Denoising plane 1", "Denoising plane 2", "Denoising plane 3" }; assert( std::size(m_nrd) <= std::size(passNames) );

    bool nrdUseRelax = m_context.settings.NRDMethod == NrdConfig::DenoiserMethod::RELAX;
    PostProcess::ComputePassType preparePassType = nrdUseRelax ? PostProcess::ComputePassType::RELAXDenoiserPrepareInputs : PostProcess::ComputePassType::REBLURDenoiserPrepareInputs;
    PostProcess::ComputePassType mergePassType = nrdUseRelax ? PostProcess::ComputePassType::RELAXDenoiserFinalMerge : PostProcess::ComputePassType::REBLURDenoiserFinalMerge;

    bool resetHistory = m_context.settings.ResetRealtimeCaches;

    int maxPassCount = std::min(m_context.settings.StablePlanesActiveCount, (int)std::size(m_nrd));
    bool initWithStableRadiance = true;
    for (int pass = maxPassCount-1; pass >= 0; pass--)
    {
        m_commandList->beginMarker(passNames[pass]);

        SampleMiniConstants miniConstants = { uint4((uint)pass, initWithStableRadiance?1:0, 0, 0) };
        initWithStableRadiance = false;

        // Direct inputs to denoiser are reused between passes; there's redundant copies but it makes interfacing simpler
        nvrhi::TextureDesc tdesc = m_renderTargets->OutputColor->getDesc();
        m_commandList->beginMarker("PrepareInputs");
        m_postProcess->Apply(m_commandList, preparePassType, m_constantBuffer, miniConstants, m_bindingSet, m_bindingLayout, tdesc.width, tdesc.height);
        m_commandList->endMarker();

        const float timeDeltaBetweenFrames = m_context.gpuDevice.IsHeadless() ? 1.f/60.f : -1.f; // if we're rendering without a window we set a fix timeDeltaBetweenFrames to ensure that output is deterministic
        bool enableValidation = m_context.settings.DebugView == DebugViewType::StablePlane_DenoiserValidation;
        if (nrdUseRelax)
        {
            m_nrd[pass]->RunDenoiserPasses(m_commandList, *m_renderTargets, pass, *m_context.renderCore.camera().view(), *m_context.renderCore.camera().viewPrevious(), m_context.gpuDevice.GetFrameIndex(), m_context.settings.NRDDisocclusionThreshold, m_context.settings.NRDDisocclusionThresholdAlternate, m_context.settings.NRDUseAlternateDisocclusionThresholdMix, timeDeltaBetweenFrames, enableValidation, resetHistory, &m_context.settings.RelaxSettings);
        }
        else
        {
            m_nrd[pass]->RunDenoiserPasses(m_commandList, *m_renderTargets, pass, *m_context.renderCore.camera().view(), *m_context.renderCore.camera().viewPrevious(), m_context.gpuDevice.GetFrameIndex(), m_context.settings.NRDDisocclusionThreshold, m_context.settings.NRDDisocclusionThresholdAlternate, m_context.settings.NRDUseAlternateDisocclusionThresholdMix, timeDeltaBetweenFrames, enableValidation, resetHistory, &m_context.settings.ReblurSettings);
        }

        m_commandList->beginMarker("MergeOutputs");
        m_postProcess->Apply(m_commandList, mergePassType, pass, m_constantBuffer, miniConstants, m_renderTargets->OutputColor, *m_renderTargets, nullptr);
        m_commandList->endMarker();

        m_commandList->endMarker();
    }
}
#if CAUSTICA_WITH_NATIVE_DLSS
bool caustica::render::WorldRenderer::evaluateNativeDLSS(bool reset)
{
    if (!m_nativeDLSS || !(m_context.settings.RealtimeAA == 2 || m_context.settings.RealtimeAA == 3))
        return false;

    const bool useRayReconstruction = m_context.settings.RealtimeAA == 3;
    if (useRayReconstruction && !m_nativeDLSS->IsRayReconstructionSupported())
        return false;
    if (!useRayReconstruction && !m_nativeDLSS->IsDlssSupported())
        return false;

    if (useRayReconstruction)
    {
        RAII_SCOPE(m_commandList->beginMarker("DLSSRR_PrepareInputs");, m_commandList->endMarker(););

        SampleMiniConstants miniConstants = { uint4(0, 0, 0, 0) };
        nvrhi::TextureDesc tdesc = m_renderTargets->OutputColor->getDesc();
        m_postProcess->Apply(m_commandList, PostProcess::ComputePassType::DLSSRRDenoiserPrepareInputs,
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

    m_nativeDLSS->Init(initParams);

    const bool initialized = useRayReconstruction
        ? m_nativeDLSS->IsRayReconstructionInitialized()
        : m_nativeDLSS->IsDlssInitialized();
    if (!initialized)
        return false;

    caustica::render::DLSS::EvaluateParameters evaluateParams;
    evaluateParams.inputColorTexture = m_renderTargets->OutputColor;
    evaluateParams.outputColorTexture = m_renderTargets->ProcessedOutputColor;
    evaluateParams.depthTexture = m_renderTargets->Depth;
    evaluateParams.motionVectorsTexture = m_renderTargets->ScreenMotionVectors;
    evaluateParams.motionVectorScaleX = 1.0f / float(m_renderSize.x);
    evaluateParams.motionVectorScaleY = 1.0f / float(m_renderSize.y);
    evaluateParams.resetHistory = reset || m_context.settings.ResetRealtimeCaches;

    if (useRayReconstruction)
    {
        evaluateParams.diffuseAlbedo = m_renderTargets->RRDiffuseAlbedo;
        evaluateParams.specularAlbedo = m_renderTargets->RRSpecAlbedo;
        evaluateParams.normalRoughness = m_renderTargets->RRNormalsAndRoughness;
    }

    const bool evaluated = m_nativeDLSS->Evaluate(m_commandList, evaluateParams, *m_context.renderCore.camera().view());
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
void caustica::render::WorldRenderer::postProcessAA(nvrhi::IFramebuffer* framebuffer, bool reset)
{
    (void)framebuffer;

    PostProcessAAParams params{
        m_context.settings,
        m_commandList,
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
    params.gaussianSplatTemporalReset = &m_gaussianSplatTemporalReset;
#if CAUSTICA_WITH_STREAMLINE
    params.dlssRROptions = &m_lastDLSSRROptions;
#endif

    m_context.renderCore.postProcessAA(params);

#if CAUSTICA_WITH_NATIVE_DLSS
    if (m_context.settings.RealtimeMode)
    {
        bool nativeDLSSEvaluated = false;
        if (m_context.settings.RealtimeAA == 2 || m_context.settings.RealtimeAA == 3)
            nativeDLSSEvaluated = evaluateNativeDLSS(reset);

        if (!nativeDLSSEvaluated && (m_context.settings.RealtimeAA == 2 || m_context.settings.RealtimeAA == 3))
        {
            if (m_context.settings.ActualUseStandaloneDenoiser())
            {
                m_commandList->copyTexture(
                    m_renderTargets->ProcessedOutputColor, nvrhi::TextureSlice(),
                    m_renderTargets->OutputColor, nvrhi::TextureSlice());
            }
            else
            {
                SampleMiniConstants miniConstants = { uint4(0, 0, 0, 0) };
                nvrhi::TextureDesc tdesc = m_renderTargets->OutputColor->getDesc();
                m_commandList->beginMarker("NoDenoiserFinalMerge");
                m_postProcess->Apply(
                    m_commandList,
                    PostProcess::ComputePassType::NoDenoiserFinalMerge,
                    m_constantBuffer,
                    miniConstants,
                    m_bindingSet,
                    m_bindingLayout,
                    tdesc.width,
                    tdesc.height);
                m_commandList->endMarker();
            }
        }
    }
#endif
}
void caustica::render::WorldRenderer::resetReferenceOIDN()
{
    m_oidnDenoisedOutputValid = false;
    m_oidnDenoiserFailed = false;

    if (m_oidnDenoiser)
        m_oidnDenoiser->Reset();
}
void caustica::render::WorldRenderer::applyReferenceOIDN()
{
    if (m_context.settings.RealtimeMode || !m_context.settings.ReferenceOIDNDenoiser || m_renderTargets == nullptr)
        return;

#if CAUSTICA_WITH_OIDN
    const bool accumulationReady = m_accumulationCompleted || m_accumulationSampleIndex >= m_context.settings.AccumulationTarget;
    if (!accumulationReady)
        return;

    if (m_oidnDenoiserFailed)
        return;

    const nvrhi::TextureDesc processedDesc = m_renderTargets->ProcessedOutputColor->getDesc();
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
        m_commandList->copyTexture(m_renderTargets->ProcessedOutputColor, nvrhi::TextureSlice(), m_oidnDenoisedOutput, nvrhi::TextureSlice());
        return;
    }

    nvrhi::ITexture* sourceTexture = m_renderTargets->AccumulatedRadiance;
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
    oidnOptions.UseGPU = m_context.settings.ReferenceOIDNUseGPU;
    oidnOptions.GuidePasses = static_cast<OidnDenoiser::Passes>(std::clamp(m_context.settings.ReferenceOIDNPasses, 0, 2));
    oidnOptions.GuidePrefilter = static_cast<OidnDenoiser::Prefilter>(std::clamp(m_context.settings.ReferenceOIDNPrefilter, 0, 2));
    oidnOptions.FilterQuality = static_cast<OidnDenoiser::Quality>(std::clamp(m_context.settings.ReferenceOIDNQuality, 0, 2));

    const bool requestAlbedoGuide = oidnOptions.GuidePasses == OidnDenoiser::Passes::Albedo || oidnOptions.GuidePasses == OidnDenoiser::Passes::AlbedoNormal;
    const bool requestNormalGuide = oidnOptions.GuidePasses == OidnDenoiser::Passes::AlbedoNormal;
    if (requestAlbedoGuide || requestNormalGuide)
    {
        SampleMiniConstants miniConstants = { uint4(0, 0, 0, 0) };
        nvrhi::TextureDesc tdesc = m_renderTargets->OutputColor->getDesc();
        m_commandList->beginMarker("OIDN_PrepareGuides");
        m_postProcess->Apply(m_commandList, PostProcess::ComputePassType::DLSSRRDenoiserPrepareInputs, m_constantBuffer, miniConstants, m_bindingSet, m_bindingLayout, tdesc.width, tdesc.height);
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
    if (requestAlbedoGuide && m_renderTargets->RRDiffuseAlbedo != nullptr)
    {
        albedoStagingTexture = device()->createStagingTexture(
            MakeReadbackTextureDesc(m_renderTargets->RRDiffuseAlbedo->getDesc(), "ReferenceOIDN Albedo Readback"),
            nvrhi::CpuAccessMode::Read);
        if (albedoStagingTexture != nullptr)
            m_commandList->copyTexture(albedoStagingTexture, nvrhi::TextureSlice(), m_renderTargets->RRDiffuseAlbedo, nvrhi::TextureSlice());
    }
    if (requestNormalGuide && m_renderTargets->RRNormalsAndRoughness != nullptr)
    {
        normalStagingTexture = device()->createStagingTexture(
            MakeReadbackTextureDesc(m_renderTargets->RRNormalsAndRoughness->getDesc(), "ReferenceOIDN Normal Readback"),
            nvrhi::CpuAccessMode::Read);
        if (normalStagingTexture != nullptr)
            m_commandList->copyTexture(normalStagingTexture, nvrhi::TextureSlice(), m_renderTargets->RRNormalsAndRoughness, nvrhi::TextureSlice());
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
    const bool success = m_oidnDenoiser->Denoise(inputRgb.data(), width, height, oidnOptions, outputRgb);

    m_commandList->open();

    if (!success)
    {
        caustica::warning("OIDN reference denoiser failed: %s", m_oidnDenoiser->GetLastError().c_str());
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
        outputHalf[pixel] = Float32ToFloat16x4(float4(r, g, b, 1.0f));
    }

    m_commandList->writeTexture(m_oidnDenoisedOutput, 0, 0, outputHalf.data(), size_t(width) * sizeof(float16_t4));
    m_commandList->copyTexture(m_renderTargets->ProcessedOutputColor, nvrhi::TextureSlice(), m_oidnDenoisedOutput, nvrhi::TextureSlice());
    m_oidnDenoisedOutputValid = true;

    caustica::info("OIDN reference denoiser completed on %s for %ux%u image.",
        m_oidnDenoiser->GetDeviceDescription().c_str(), width, height);
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
    std::string noisyImagePath = (caustica::GetDirectoryWithExecutable( ) / "photo.bmp").string();

    auto execute = [&](const std::string & dn = "OptiX")
    {
	    const auto p1 = std::chrono::system_clock::now();
		const std::string timestamp = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(p1.time_since_epoch()).count());

        const std::string fileName = "photo-denoised_" + dn + "_" + timestamp + ".bmp";

        std::string denoisedImagePath = (caustica::GetDirectoryWithExecutable() / fileName).string();
        std::filesystem::path denoiserPath = GetLocalPath("Support/denoiser_"+dn) / "denoiser.exe";
        if (!std::filesystem::exists(denoiserPath))
        {
            caustica::warning("External %s denoiser not found at '%s'.", dn.c_str(), denoiserPath.string().c_str());
            return;
        }

        if (!SaveTextureToFile(device(), m_context.commonPasses.get(), framebufferTexture, nvrhi::ResourceStates::Common, noisyImagePath.c_str()))
        { assert(false); return; }

        std::string startCmd = "\"" + denoiserPath.string() + "\"" + " -hdr 0 -i \"" + noisyImagePath + "\"" " -o \"" + denoisedImagePath + "\"";
        auto [resNum, resString, errorString] =  SystemShell(startCmd.c_str());
        if (resString!="")
            caustica::info("result: %s", resString.c_str());
        if (errorString != "")
            caustica::info("error: %s", errorString.c_str());

        std::string viewCmd = "\"" + denoisedImagePath + "\"";
        SystemShell(viewCmd.c_str(), true);
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
    params.realtimeMode = m_context.settings.RealtimeMode;
    params.realtimeAA = m_context.settings.RealtimeAA;
    params.dbgFreezeRealtimeNoiseSeed = m_context.settings.DbgFreezeRealtimeNoiseSeed;
    params.syncPreviousView = m_context.settings.ResetAccumulation || m_context.settings.ResetRealtimeCaches;
    params.temporalAAJitter = m_context.settings.TemporalAntiAliasingJitter;
    params.temporalAAPass = m_temporalAntiAliasingPass.get();
    return params;
}

void caustica::render::WorldRenderer::syncCameraViews()
{
    m_context.renderCore.camera().updateViews(makeCameraUpdateParams());
}

dm::float2 caustica::render::WorldRenderer::computeCameraJitter() const
{
    return m_context.renderCore.camera().computeJitter(makeCameraUpdateParams());
}
