namespace { constexpr int c_SwapchainCount = 3; }

#include <render/WorldRenderer/PathTracingWorldRenderer.h>

#include <render/Core/PostProcessAA.h>
#include <render/Core/SceneGeometryUpdate.h>
#include <render/Core/LightingUpdate.h>
#include <render/Core/PTPipelineBaker.h>
#include <render/Core/ComputePipelineBaker.h>
#include <render/Core/BindingCache.h>
#include <render/Core/View.h>
#include <render/Core/FramebufferFactory.h>
#include <render/Core/AccelerationStructureUtil.h>
#include <render/Passes/Lighting/Distant/EnvMapImportanceSamplingBaker.h>
#include <render/Passes/Lighting/MaterialsBaker.h>
#include <render/Passes/OMM/OmmBaker.h>
#include <render/Passes/Debug/ZoomTool.h>
#include <render/Passes/PostProcess/DenoisingGuidesBaker.h>
#include <render/Passes/Denoisers/OidnDenoiser.h>
#include <render/Passes/Gaussian/GaussianSplatPass.h>
#include <assets/loader/ShaderFactory.h>
#include <render/GPUSort/GPUSort.h>
#include <backend/GpuDevice.h>
#include <core/path_utils.h>
#include <core/log.h>
#include <assets/cache/TextureCache.h>
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
#include <engine/StreamlineInterface.h>
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

caustica::render::PathTracingWorldRenderer::PathTracingWorldRenderer(WorldRendererServices& services)
    : m_services(services)
{
}

caustica::render::PathTracingWorldRenderer::~PathTracingWorldRenderer() = default;

nvrhi::BindingLayoutHandle caustica::render::PathTracingWorldRenderer::CreateBindlessLayout(nvrhi::IDevice* device)
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

void caustica::render::PathTracingWorldRenderer::createBindingLayouts(nvrhi::IBindingLayout* precreatedBindless)
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

void caustica::render::PathTracingWorldRenderer::createDeviceResources()
{
    nvrhi::IDevice* device = this->device();

#if CAUSTICA_WITH_NATIVE_DLSS
    m_nativeDLSS = caustica::render::DLSS::Create(device, m_services.shaderFactory, caustica::GetDirectoryWithExecutable().string());
    if (m_nativeDLSS)
    {
        m_services.settings.IsDLSSSuported = m_nativeDLSS->IsDlssSupported();
        m_services.settings.IsDLSSRRSupported = m_nativeDLSS->IsRayReconstructionSupported();
        caustica::info("Native NGX DLSS support: DLSS=%s, DLSS-RR=%s.",
            m_services.settings.IsDLSSSuported ? "yes" : "no",
            m_services.settings.IsDLSSRRSupported ? "yes" : "no");
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
        m_linesVertexShader = m_services.shaderFactory->CreateShader("caustica/shaders/render/Misc/DebugLines.hlsl", "main_vs", &drawLinesMacro, nvrhi::ShaderType::Vertex);
        m_linesPixelShader = m_services.shaderFactory->CreateShader("caustica/shaders/render/Misc/DebugLines.hlsl", "main_ps", &drawLinesMacro, nvrhi::ShaderType::Pixel);

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

bool caustica::render::PathTracingWorldRenderer::createPTPipeline()
{
    std::vector<caustica::ShaderMacro> shaderMacros;
    m_exportVBufferCS = m_services.shaderFactory->CreateShader(
        "caustica/shaders/render/ProcessingPasses/ExportVisibilityBuffer.hlsl", "main", &shaderMacros, nvrhi::ShaderType::Compute);
    nvrhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.bindingLayouts = { m_bindingLayout, m_bindlessLayout };
    pipelineDesc.CS = m_exportVBufferCS;
    m_exportVBufferPSO = device()->createComputePipeline(pipelineDesc);
    return true;
}

void caustica::render::PathTracingWorldRenderer::onSceneUnloading()
{
    m_bindingSet = nullptr;
    m_gaussianSplatTemporalReset = true;
    if (m_rtxdiPass != nullptr)
        m_rtxdiPass->Reset();
    m_ptPipelineBaker = nullptr;
    m_ptPipelineReference = nullptr;
    m_ptPipelineBuildStablePlanes = nullptr;
    m_ptPipelineFillStablePlanes = nullptr;
    m_ptPipelineTestRaygenPPHDR = nullptr;
    m_ptPipelineEdgeDetection = nullptr;
}

void caustica::render::PathTracingWorldRenderer::resetFrameIndex()
{
    m_frameIndex = 0;
}
void caustica::render::PathTracingWorldRenderer::onBackBufferResizing()
{
    device()->waitForIdle();
    device()->runGarbageCollection();
    m_services.bindingCache.Clear();
    m_renderTargets = nullptr;
    m_linesPipeline = nullptr; // the pipeline is based on the framebuffer so needs a reset
    for (int i=0; i < std::size(m_nrd); i++ )
        m_nrd[i] = nullptr;
    if (m_rtxdiPass)
        m_rtxdiPass->Reset();

// NOTE: we're not yet sure if this is necessary to avoid crash with going in/out of fullscreen and FG
#if CAUSTICA_WITH_STREAMLINE
    if (!m_services.gpuDevice.GetDeviceParams().headlessDevice &&
        (m_services.settings.DLSSFGOptions.mode == StreamlineInterface::DLSSGMode::eOn || m_services.settings.ActualDLSSFGMode() == StreamlineInterface::DLSSGMode::eOn)) 
    {
        m_services.gpuDevice.GetStreamline().CleanupDLSS(false);
        m_services.gpuDevice.GetStreamline().CleanupDLSSG(false);

        if (m_services.gpuDevice.GetStreamline().IsDLSSGAvailable())
        {
            auto dlssgOptions = StreamlineInterface::DLSSGOptions{};
            StreamlineInterface::DLSSGState state;
            m_services.gpuDevice.GetStreamline().GetDLSSGState(state, dlssgOptions);
            m_services.settings.DLSSFGMultiplier = state.numFramesActuallyPresented;
            m_services.settings.DLSSFGMaxNumFramesToGenerate = state.numFramesToGenerateMax;

            m_services.gpuDevice.GetStreamline().SetDLSSGOptions(dlssgOptions);
            m_services.settings.DLSSFGOptions = dlssgOptions;
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

void caustica::render::PathTracingWorldRenderer::createRenderPasses( bool& exposureResetRequired, nvrhi::CommandListHandle initializeCommandList )
{
    m_services.bindingCache.Clear();

    const uint2 screenResolution = {m_renderTargets->OutputColor->getDesc().width, m_renderTargets->OutputColor->getDesc().height};

    m_shaderDebug = std::make_shared<ShaderDebug>(device(), initializeCommandList, m_services.shaderFactory, m_services.commonPasses);

    if (m_services.settings.ActualUseRTXDIPasses())
        m_rtxdiPass = std::make_unique<RtxdiPass>(device(), m_services.shaderFactory, m_services.commonPasses, m_bindlessLayout);
    else
        m_rtxdiPass = nullptr;

    m_accumulationPass = std::make_unique<AccumulationPass>(device(), m_services.shaderFactory);
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

        m_gaussianSplatAccumulationPass = std::make_unique<AccumulationPass>(device(), m_services.shaderFactory);
        m_gaussianSplatAccumulationPass->CreatePipeline();
        m_gaussianSplatAccumulationPass->CreateBindingSet(m_gaussianSplatCurrentColor, m_gaussianSplatAccumulatedColor, m_renderTargets->ProcessedOutputColor);
        m_gaussianSplatTemporalReset = true;
    }

    // these get re-created every time intentionally, to pick up changes after at-runtime shader recompile
    m_toneMappingPass = std::make_unique<ToneMappingPass>(device(), m_services.shaderFactory, m_services.commonPasses, m_renderTargets->LdrFramebuffer, *m_services.renderCore.camera().view(), m_renderTargets->OutputColor);
    m_bloomPass = std::make_unique<BloomPass>(device(), m_services.shaderFactory, m_services.commonPasses, m_renderTargets->ProcessedOutputFramebuffer, *m_services.renderCore.camera().view());
    m_postProcess = std::make_shared<PostProcess>(device(), m_services.shaderFactory, m_services.commonPasses, m_shaderDebug);

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

        m_temporalAntiAliasingPass = std::make_unique<TemporalAntiAliasingPass>(device(), m_services.shaderFactory, m_services.commonPasses, *m_services.renderCore.camera().view(), taaParams);
    }

    if (!createPTPipeline())
        { assert(false); }

    if (m_services.envMapBaker == nullptr)
        m_services.envMapBaker = std::make_shared<EnvMapBaker>(device(), m_services.textureCache, m_services.hooks.needsRasterPrecompute());
    if (m_services.lightsBaker == nullptr)
        m_services.lightsBaker = std::make_shared<LightsBaker>(device());
    m_services.envMapBaker->CreateRenderPasses(m_shaderDebug, m_services.shaderFactory, m_services.computePipelineBaker);
    m_services.envMapBaker->GenerateBRDFLUT(initializeCommandList.Get(), m_services.bindingCache);  // One-time BRDF LUT generation
    m_services.lightsBaker->CreateRenderPasses(m_services.shaderFactory, m_bindlessLayout, m_services.commonPasses, m_shaderDebug, screenResolution, m_services.envMapBaker->GetImportanceSampling()->GetImportanceMapResolution());

    m_services.hooks.prepareGaussianSplatPasses();

    m_denoisingGuidesBaker = std::make_shared<DenoisingGuidesBaker>(device(), m_services.shaderFactory, m_renderTargets, m_shaderDebug, m_bindingLayout);
}
void caustica::render::PathTracingWorldRenderer::preUpdateLighting(nvrhi::CommandListHandle commandList, bool& needNewBindings)
{
    std::filesystem::path sceneDirectory;
    if (m_services.sceneManager.getCurrentScenePath() != std::filesystem::path(SceneManager::inlineSceneSentinel()))
        sceneDirectory = m_services.sceneManager.getCurrentScenePath().parent_path();

    std::string envMapActualPath = m_services.envMapLocalPath;
    if (m_services.envMapOverride != "" && m_services.envMapOverride != c_EnvMapSceneDefault)
        envMapActualPath = (IsProceduralSky(m_services.envMapOverride.c_str())) ? (m_services.envMapOverride) : (std::string(c_EnvMapSubFolder) + "/" + m_services.envMapOverride);

    if (!envMapActualPath.empty() && !IsProceduralSky(envMapActualPath.c_str()))
        envMapActualPath = ResolveSceneMediaPath(envMapActualPath, sceneDirectory).generic_string();

    PreUpdateLightingParams params{
        commandList,
        needNewBindings,
        m_services.envMapBaker.get(),
        m_services.commonPasses,
        envMapActualPath,
        sceneDirectory,
    };
    m_services.renderCore.preUpdateLighting(params);
}
void caustica::render::PathTracingWorldRenderer::updateLighting(nvrhi::CommandListHandle commandList)
{
    m_services.hooks.buildGaussianSplatEmissionProxyList();

    UpdateLightingParams params{
        m_services.settings,
        commandList,
        m_services.envMapBaker.get(),
        m_services.lightsBaker.get(),
        &m_services.bindingCache,
        m_services.commonPasses,
        &m_services.lights,
        m_services.sceneManager.getScene(),
        m_services.materialsBaker,
        m_services.ommBaker,
        m_services.envMapSceneParams,
        m_services.sceneTime,
        m_frameIndex,
        c_envMapRadianceScale,
    };
    if (!m_services.gaussianSplatEmissionProxies.empty())
        params.gaussianSplatEmissionProxies = &m_services.gaussianSplatEmissionProxies;
    m_services.renderCore.updateLighting(params);
}
void caustica::render::PathTracingWorldRenderer::preUpdatePathTracing( bool resetAccum, nvrhi::CommandListHandle commandList )
{
    const bool resetReferenceOidn = !m_services.settings.RealtimeMode && (resetAccum || m_services.settings.ResetAccumulation || m_services.settings.ReferenceOIDNDenoiserChanged);
    if (resetReferenceOidn || m_services.settings.ReferenceOIDNDenoiserChanged)
    {
        resetReferenceOIDN();
        m_services.settings.ReferenceOIDNDenoiserChanged = false;
    }

    resetAccum |= m_services.settings.ResetAccumulation;
    resetAccum |= m_services.settings.RealtimeMode;

    if (resetAccum)
    {
        m_accumulationSampleIndex = (m_services.settings.AccumulationPreWarmRealtimeCaches)?(-32):(0);
    }
#if ENABLE_DEBUG_VIZUALISATIONS
    if (resetAccum)
        m_shaderDebug->ClearDebugVizTexture(commandList);
#endif

    // profile perf - only makes sense with high accumulation sample counts; only start counting after n-th after it stabilizes
    if( m_accumulationSampleIndex < 16 )
    {
        m_services.benchStart = std::chrono::high_resolution_clock::now( );
        m_services.benchLast = m_services.benchStart;
        m_services.benchFrames = 0;
    } else if( m_accumulationSampleIndex < m_services.settings.AccumulationTarget )
    {
        m_services.benchFrames++;
        m_services.benchLast = std::chrono::high_resolution_clock::now( );
    }
    m_accumulationCompleted = false;
    // 'min' in non-realtime path here is to keep looping the last sample for debugging purposes!
    if( !m_services.settings.RealtimeMode )
    {
        m_sampleIndex = (m_accumulationSampleIndex<0)?(m_accumulationSampleIndex+4096):(min(m_accumulationSampleIndex, m_services.settings.AccumulationTarget - 1));
        m_accumulationCompleted |= m_accumulationSampleIndex == m_services.settings.AccumulationTarget - 1;
    }
    else
        m_sampleIndex = (!m_services.settings.DbgFreezeRealtimeNoiseSeed)?( m_frameIndex % 8192 ):0;     // actual sample index
}
void caustica::render::PathTracingWorldRenderer::postUpdatePathTracing( )
{
    m_accumulationSampleIndex = std::min( m_accumulationSampleIndex+1, m_services.settings.AccumulationTarget );

    if (m_services.settings.ActualUseRTXDIPasses())
        m_rtxdiPass->EndFrame();

    m_services.settings.ResetAccumulation = false;
    m_services.settings.ResetRealtimeCaches = false;
    m_frameIndex++;
}
void caustica::render::PathTracingWorldRenderer::updatePathTracerConstants( PathTracerConstants & constants, const PathTracerCameraData & cameraData )
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
    if (m_services.renderCore.camera().viewPrevious())
        constants.prevCamera.PosW = m_services.renderCore.camera().viewPrevious()->GetInverseViewMatrix().m_translation;

    constants.bounceCount = m_services.settings.BounceCount;
    constants.diffuseBounceCount = m_services.settings.DiffuseBounceCount;
    constants.perPixelJitterAAScale = (m_services.settings.RealtimeMode == false && m_services.settings.AccumulationAA)?(1):( (m_services.settings.RealtimeMode && m_services.settings.RealtimeAA == 3)?(m_services.settings.DLSSRRMicroJitter):(0.0f) );

    // needed to allow super-resolution to work best
    float dlssBias = -dm::log2f(sqrtf((m_displaySize.x * m_displaySize.y) / float(m_renderSize.x * m_renderSize.y)));

    constants.texLODBias = m_services.settings.TexLODBias + dlssBias;
    constants.sampleBaseIndex = m_sampleIndex * m_services.settings.ActualSamplesPerPixel();

    //constants.subSampleCount = m_services.settings.ActualSamplesPerPixel();
    constants.invSubSampleCount = 1.0f / (float)m_services.settings.ActualSamplesPerPixel();

    constants.imageWidth = m_renderSize.x; assert( m_renderSize.x == m_renderTargets->OutputColor->getDesc().width );
    constants.imageHeight = m_renderSize.y; assert( m_renderSize.y == m_renderTargets->OutputColor->getDesc().height );

    // this is the dynamic luminance that when passed through current tonemapper with current exposure settings, produces the same 50% gray
    constants.preExposedGrayLuminance = m_services.settings.EnableToneMapping?(dm::luminance(m_toneMappingPass->GetPreExposedGray(0))):(1.0f);

    const float disabledFF = 0.0f;
    if (m_services.settings.RealtimeMode)
        constants.fireflyFilterThreshold = (m_services.settings.RealtimeFireflyFilterEnabled)?(m_services.settings.RealtimeFireflyFilterThreshold*sqrtf(constants.preExposedGrayLuminance)*1e3f):(disabledFF); // it does make sense to make the realtime variant dependent on avg luminance - just didn't have time to try it out yet
    else
        constants.fireflyFilterThreshold = (m_services.settings.ReferenceFireflyFilterEnabled)?(m_services.settings.ReferenceFireflyFilterThreshold*sqrtf(constants.preExposedGrayLuminance)*1e3f):(disabledFF); // making it exposure-adaptive breaks determinism with accumulation (because there's a feedback loop), so that's disabled
    constants.useReSTIRDI = m_services.settings.ActualUseReSTIRDI();
    constants.useReSTIRGI = m_services.settings.ActualUseReSTIRGI();
    constants.useReSTIRPT = m_services.settings.ActualUseReSTIRPT();
    constants.environmentMapVisibleToCamera = m_services.settings.EnvironmentMapParams.VisibleToCamera ? 1u : 0u;
    constants.denoiserRadianceClampK = m_services.settings.DenoiserRadianceClampK;
    constants.DLSSRRBrightnessClampK = (m_services.settings.DLSSRRBrightnessClampK>0)?(m_services.settings.DLSSRRBrightnessClampK * constants.preExposedGrayLuminance):(0.0f);

    // no stable planes by default
    constants.denoisingEnabled = m_services.settings.ActualUseStandaloneDenoiser() || m_services.settings.RealtimeAA == 3;

    constants._activeStablePlaneCount           = m_services.settings.StablePlanesActiveCount;
    constants.maxStablePlaneVertexDepth         = std::min( std::min( (uint)m_services.settings.StablePlanesMaxVertexDepth, cStablePlaneMaxVertexIndex ), (uint)m_services.settings.BounceCount );
    constants.allowPrimarySurfaceReplacement    = m_services.settings.AllowPrimarySurfaceReplacement;
    constants.stablePlanesSplitStopThreshold    = m_services.settings.StablePlanesSplitStopThreshold;
    constants._padding3                         = 0;
    constants.stablePlanesSuppressPrimaryIndirectSpecularK  = m_services.settings.StablePlanesSuppressPrimaryIndirectSpecular?m_services.settings.StablePlanesSuppressPrimaryIndirectSpecularK:0.0f;
    constants.stablePlanesAntiAliasingFallthrough = m_services.settings.StablePlanesAntiAliasingFallthrough;
    constants.frameIndex                        = m_frameIndex & 0xFFFFFFFF; //m_services.gpuDevice.GetFrameIndex();
    constants.genericTSLineStride               = GenericTSComputeLineStride(constants.imageWidth, constants.imageHeight);
    constants.genericTSPlaneStride              = GenericTSComputePlaneStride(constants.imageWidth, constants.imageHeight);

    constants.NEEEnabled                        = m_services.settings.UseNEE;
    constants.NEEType                           = m_services.settings.NEEType;
    constants.NEECandidateSamples               = m_services.settings.NEECandidateSamples;
    constants.NEEFullSamples                    = m_services.settings.NEEFullSamples;

    constants.EnvironmentMapDiffuseSampleMIPLevel = m_services.settings.EnvironmentMapDiffuseSampleMIPLevel;

#if CAUSTICA_STOCHASTIC_TEXTURE_FILTERING_ENABLE
    // stochastic texture filtering type and size.
    // constants.STFUseBlueNoise                   = m_services.settings.STFUseBlueNoise;
    constants.STFMagnificationMethod            = GetStfMagnificationMethod(m_services.settings.STFMagnificationMethod);
    constants.STFFilterMode                     = GetStfFilterMode(m_services.settings.STFFilterMode);
    constants.STFGaussianSigma                  = m_services.settings.STFGaussianSigma;
#endif
}
void caustica::render::PathTracingWorldRenderer::rtxdiSetupFrame(nvrhi::IFramebuffer* framebuffer, PathTracerCameraData cameraData, uint2 renderDims)
{
    const bool envMapPresent = m_services.settings.EnvironmentMapParams.Enabled;

    RtxdiBridgeParameters bridgeParameters;
	bridgeParameters.frameIndex = m_frameIndex & 0xFFFFFFFF;
	bridgeParameters.frameDims = renderDims;
	bridgeParameters.cameraPosition = m_services.renderCore.camera().camera().GetPosition();
	bridgeParameters.userSettings = m_services.settings.RTXDI;
    bridgeParameters.usingLightSampling = m_services.settings.ActualUseReSTIRDI();
    bridgeParameters.usingReGIR = m_services.settings.ActualUseReSTIRDI();

    bridgeParameters.userSettings.restirDI.initialSamplingParams.environmentMapImportanceSampling = envMapPresent;

    m_services.hooks.buildGaussianSplatEmissionProxyList();
    if (!m_services.gaussianSplatEmissionProxies.empty() && m_services.hooks.isGaussianSplatEmissionEnabled())
    {
        bridgeParameters.gaussianSplatEmissionProxies = &m_services.gaussianSplatEmissionProxies;
        bridgeParameters.gaussianSplatEmissionObjectToWorld = float4x4::identity();
        bridgeParameters.gaussianSplatEmissionIntensity = m_services.settings.GaussianSplatEmissionIntensity;
    }

    if( m_services.settings.ResetRealtimeCaches )
        m_rtxdiPass->Reset();

	m_rtxdiPass->PrepareResources(m_commandList, *m_renderTargets, envMapPresent ? m_services.envMapBaker : nullptr, m_services.envMapSceneParams,
        m_services.sceneManager.getScene(), m_services.materialsBaker, m_services.ommBaker, m_services.renderCore.accelStructs().getSubInstanceBuffer(), bridgeParameters, m_bindingLayout, m_shaderDebug );
 }
#if CAUSTICA_WITH_STREAMLINE
void caustica::render::PathTracingWorldRenderer::streamlinePreRender()
{
#if CAUSTICA_WITH_STREAMLINE
    if (m_services.gpuDevice.GetDeviceParams().headlessDevice)
        return;

    // Setup Reflex
    {
        auto reflexConsts = caustica::StreamlineInterface::ReflexOptions{};
        reflexConsts.mode = (caustica::StreamlineInterface::ReflexMode) m_services.settings.ActualReflexMode();
        reflexConsts.frameLimitUs = m_services.settings.ReflexCappedFps == 0 ? 0 : int(1000000. / m_services.settings.ReflexCappedFps);
        reflexConsts.useMarkersToOptimize = true;
        reflexConsts.virtualKey = VK_F13;
        reflexConsts.idThread = 0; // std::hash<std::thread::id>()(std::this_thread::get_id())
        m_services.gpuDevice.GetStreamline().SetReflexConsts(reflexConsts);

        // Need to update StreamlineIntegration with the ability to query reflex state
        caustica::StreamlineInterface::ReflexState reflexState{};
        m_services.gpuDevice.GetStreamline().GetReflexState(reflexState);
        if (m_services.settings.IsReflexSupported)
        {
            m_services.settings.IsReflexLowLatencyAvailable = reflexState.lowLatencyAvailable;
            m_services.settings.IsReflexFlashIndicatorDriverControlled = reflexState.flashIndicatorDriverControlled;

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

                m_services.settings.ReflexStats = "frameID: " + std::to_string(frameID);
                m_services.settings.ReflexStats += "\ntotalGameToRenderLatencyUs: " + std::to_string(totalGameToRenderLatencyUs);
                m_services.settings.ReflexStats += "\nsimDeltaUs: " + std::to_string(simDeltaUs);
                m_services.settings.ReflexStats += "\nrenderDeltaUs: " + std::to_string(renderDeltaUs);
                m_services.settings.ReflexStats += "\npresentDeltaUs: " + std::to_string(presentDeltaUs);
                m_services.settings.ReflexStats += "\ndriverDeltaUs: " + std::to_string(driverDeltaUs);
                m_services.settings.ReflexStats += "\nosRenderQueueDeltaUs: " + std::to_string(osRenderQueueDeltaUs);
                m_services.settings.ReflexStats += "\ngpuRenderDeltaUs: " + std::to_string(gpuRenderDeltaUs);
            }
            else
            {
                m_services.settings.ReflexStats = "Latency Report Unavailable";
            }
        }
    }

    // DLSS-G Setup
    {
        // If DLSS-G has been turned off, then we tell tell SL to clean it up expressly
        if (m_services.settings.DLSSFGOptions.mode == StreamlineInterface::DLSSGMode::eOn && m_services.settings.ActualDLSSFGMode() == StreamlineInterface::DLSSGMode::eOff) {
            m_services.gpuDevice.GetStreamline().CleanupDLSSG(true);
        }

        // This is where DLSS-G is toggled On and Off (using dlssgOptions.mode) and where we set DLSS-G parameters.
        auto dlssgOptions = StreamlineInterface::DLSSGOptions{};
        dlssgOptions.mode = m_services.settings.ActualDLSSFGMode();
        dlssgOptions.numFramesToGenerate = m_services.settings.DLSSFGNumFramesToGenerate;

        // This is where we query DLSS-G minimum swapchain size
        if (m_services.gpuDevice.GetStreamline().IsDLSSGAvailable())
        {
            StreamlineInterface::DLSSGState state;
            m_services.gpuDevice.GetStreamline().GetDLSSGState(state, dlssgOptions);
            m_services.settings.DLSSFGMultiplier = state.numFramesActuallyPresented;
            m_services.settings.DLSSFGMaxNumFramesToGenerate = state.numFramesToGenerateMax;

            m_services.gpuDevice.GetStreamline().SetDLSSGOptions(dlssgOptions);
            m_services.settings.DLSSFGOptions = dlssgOptions;
        }
    }

    // Ensure DLSS / DLSS-RR is available
    if (m_services.settings.RealtimeAA == 3 && !m_services.settings.IsDLSSRRSupported)
    {
        caustica::warning("Requested DLSS-RR mode not available. Switching to DLSS. ");
        m_services.settings.RealtimeAA = 2;
    }
    if ( m_services.settings.RealtimeAA == 2 && !m_services.settings.IsDLSSSuported )
    {
        caustica::warning("Requested DLSS mode not available. Switching to TAA. ");
        m_services.settings.RealtimeAA = 1;
    }

    // Setup DLSS
    const bool changeToDLSSMode = (m_services.settings.RealtimeAA >= 2 && m_services.settings.RealtimeAA <= 3) && m_services.settings.DLSSLastRealtimeAA != m_services.settings.RealtimeAA;
    {
        // Reset DLSS vars if we stop using it
        if (changeToDLSSMode || m_services.settings.DLSSMode == StreamlineInterface::DLSSMode::eOff)
        {
            m_services.settings.DLSSLastMode = PathTracerSettings::DLSSModeDefault;
            m_services.settings.DLSSMode = PathTracerSettings::DLSSModeDefault;
            m_services.settings.DLSSLastDisplaySize = { 0,0 };
        }

        m_services.settings.DLSSLastRealtimeAA = m_services.settings.RealtimeAA;

        // If we are using DLSS set its constants
        if ((m_services.settings.RealtimeAA == 2 || m_services.settings.RealtimeAA == 3) && m_services.settings.RealtimeMode)
        {
            StreamlineInterface::DLSSOptions dlssOptions = {};
            if (m_services.settings.IsDLSSSuported)
            {
                dlssOptions.mode = m_services.settings.DLSSMode;
                dlssOptions.outputWidth = m_displaySize.x;
                dlssOptions.outputHeight = m_displaySize.y;
                dlssOptions.sharpness = 0; //m_recommendedDLSSSettings.sharpness;    // <- is this no longer valid?
                dlssOptions.colorBuffersHDR = true;
                dlssOptions.useAutoExposure = true;     // Optional: provide proper "kBufferTypeExposure" for 0-lag for better precision handling
                dlssOptions.preset = StreamlineInterface::DLSSPreset::eDefault;
                // if (m_services.settings.RealtimeAA < 4) <- docs https://github.com/NVIDIAGameWorks/Streamline/blob/main/docs/ProgrammingGuideDLSS_RR.md#50-provide-dlss--dlss-rr-options seem to imply that these should be set even when DLSS-RR enabled
                    m_services.gpuDevice.GetStreamline().SetDLSSOptions(dlssOptions);
            }
            else
            {
                assert( false ); // shouldn't happen, code above should have dropped us to "m_services.settings.RealtimeAA = 1" - check for recent code changes.
            }

            if (m_services.settings.RealtimeAA == 2 || m_services.settings.RealtimeAA == 3)
            {
                // Check if we need to update the rendertarget size.
                bool dlssResizeRequired = (m_services.settings.DLSSMode != m_services.settings.DLSSLastMode) || (m_displaySize.x != m_services.settings.DLSSLastDisplaySize.x) || (m_displaySize.y != m_services.settings.DLSSLastDisplaySize.y);
                if (dlssResizeRequired)
                {
                    // Only quality, target width and height matter here
                    m_services.gpuDevice.GetStreamline().QueryDLSSOptimalSettings(dlssOptions, m_recommendedDLSSSettings);

                    // this is an example on how to override defaults - overriding default 2/3 to higher res 3/4
                    if (dlssOptions.mode == SI::DLSSMode::eMaxQuality)
                    {
                        m_recommendedDLSSSettings.optimalRenderSize.x = dm::clamp((int)(dlssOptions.outputWidth * 3 / 4 + 0.5f), m_recommendedDLSSSettings.minRenderSize.x, m_recommendedDLSSSettings.maxRenderSize.x);
                        m_recommendedDLSSSettings.optimalRenderSize.y = dm::clamp((int)(dlssOptions.outputHeight * 3 / 4 + 0.5f), m_recommendedDLSSSettings.minRenderSize.y, m_recommendedDLSSSettings.maxRenderSize.y);
                    }

                    if (m_recommendedDLSSSettings.optimalRenderSize.x <= 0 || m_recommendedDLSSSettings.optimalRenderSize.y <= 0)
                    {
                        m_services.settings.RealtimeAA = 0;
                        m_services.settings.DLSSMode = PathTracerSettings::DLSSModeDefault;
                        m_renderSize = m_displaySize;
                    }
                    else
                    {
                        m_services.settings.DLSSLastMode = m_services.settings.DLSSMode;
                        m_services.settings.DLSSLastDisplaySize = m_displaySize;
                    }
                }

                m_renderSize = (uint2)m_recommendedDLSSSettings.optimalRenderSize;
            }

            if (m_services.settings.RealtimeAA == 3) // DLSS-RR
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
                dlssRROptions.preset                = m_services.settings.DLSRRPreset;
                m_lastDLSSRROptions = dlssRROptions; // we need to fill them up with view info, but we can only have proper view after it was initialized with correct RT size
            }
        }
        else
        {
            if (m_services.settings.IsDLSSSuported)
            {
                StreamlineInterface::DLSSOptions dlssOptions = {};
                dlssOptions.mode = StreamlineInterface::DLSSMode::eOff;
                m_services.gpuDevice.GetStreamline().SetDLSSOptions(dlssOptions);
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
void caustica::render::PathTracingWorldRenderer::nativeDLSSPreRender()
{
    if (!m_services.settings.RealtimeMode)
    {
        m_renderSize = m_displaySize;
        return;
    }

    if (m_nativeDLSS)
    {
        m_services.settings.IsDLSSSuported = m_nativeDLSS->IsDlssSupported();
        m_services.settings.IsDLSSRRSupported = m_nativeDLSS->IsRayReconstructionSupported();
    }

    if (m_services.settings.RealtimeAA == 3 && !m_services.settings.IsDLSSRRSupported)
    {
        caustica::warning("Requested DLSS-RR mode not available. Switching to DLSS.");
        m_services.settings.RealtimeAA = 2;
    }

    if (m_services.settings.RealtimeAA == 2 && !m_services.settings.IsDLSSSuported)
    {
        caustica::warning("Requested DLSS mode not available. Switching to TAA.");
        m_services.settings.RealtimeAA = 1;
    }

    const bool usingDLSS = (m_services.settings.RealtimeAA == 2 || m_services.settings.RealtimeAA == 3);
    const bool changeToDLSSMode = usingDLSS && m_services.settings.DLSSLastRealtimeAA != m_services.settings.RealtimeAA;

    if (changeToDLSSMode || m_services.settings.DLSSMode == SI::DLSSMode::eOff)
    {
        m_services.settings.DLSSLastMode = PathTracerSettings::DLSSModeDefault;
        m_services.settings.DLSSMode = PathTracerSettings::DLSSModeDefault;
        m_services.settings.DLSSLastDisplaySize = { 0, 0 };
    }

    m_services.settings.DLSSLastRealtimeAA = m_services.settings.RealtimeAA;

    if (usingDLSS)
    {
        const bool dlssResizeRequired =
            (m_services.settings.DLSSMode != m_services.settings.DLSSLastMode) ||
            (m_displaySize.x != m_services.settings.DLSSLastDisplaySize.x) ||
            (m_displaySize.y != m_services.settings.DLSSLastDisplaySize.y);

        if (dlssResizeRequired)
        {
            m_services.settings.DLSSLastMode = m_services.settings.DLSSMode;
            m_services.settings.DLSSLastDisplaySize = m_displaySize;
        }

        m_renderSize = GetNativeDLSSRenderSize(m_displaySize, m_services.settings.DLSSMode);
    }
    else
    {
        m_renderSize = m_displaySize;
    }
}
#endif
void caustica::render::PathTracingWorldRenderer::preRender()
{
    // Limit FPS
    if (m_services.settings.ActualFPSLimiter() > 0)
        g_FPSLimiter.FramerateLimit(m_services.settings.ActualFPSLimiter());

    korgi::Update();

    m_services.hooks.captureScriptPreRender();
}
void caustica::render::PathTracingWorldRenderer::postProcessPreToneMapping(nvrhi::ICommandList* commandList, const caustica::ICompositeView& compositeView)
{
    (void)compositeView;

    HdrPostProcessParams hdrParams{
        m_services.settings,
        commandList,
        m_renderTargets.get(),
        m_displaySize,
        m_bloomPass.get(),
    };
    m_services.renderCore.hdrPostProcess(hdrParams);

    if (m_services.settings.PostProcessTestPassHDR)
    {
        commandList->beginMarker("TestRaygenPP_HDR");

        commandList->setTextureState(m_renderTargets->ProcessedOutputColor, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);

        nvrhi::rt::DispatchRaysArguments args;
        args.width = m_displaySize.x;
        args.height = m_displaySize.y;

        nvrhi::rt::State state;
        state.shaderTable = m_ptPipelineTestRaygenPPHDR->GetShaderTable();
        state.bindings = { m_bindingSet, m_services.descriptorTable->GetDescriptorTable() };
        commandList->setRayTracingState(state);

        SampleMiniConstants miniConstants = { uint4(0, 0, 0, 0) };
        commandList->setPushConstants(&miniConstants, sizeof(miniConstants));
        commandList->dispatchRays(args);

        commandList->setTextureState(m_renderTargets->ProcessedOutputColor, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);

        commandList->endMarker();
    }
}
void caustica::render::PathTracingWorldRenderer::postProcessPostToneMapping(nvrhi::ICommandList* commandList, const caustica::ICompositeView& compositeView)
{ // a.k.a. LDR post-process (e.g. colour filters go here)
    if (m_services.settings.PostProcessEdgeDetection)
    {
        m_commandList->beginMarker("PPEdgeDetection");

        m_commandList->copyTexture(m_renderTargets->LdrColorScratch, nvrhi::TextureSlice(), m_renderTargets->LdrColor, nvrhi::TextureSlice());

        nvrhi::rt::DispatchRaysArguments args;
        args.width  = m_displaySize.x;
        args.height = m_displaySize.y;

        nvrhi::rt::State state;
        state.shaderTable = m_ptPipelineEdgeDetection->GetShaderTable();
        state.bindings = { m_bindingSet, m_services.descriptorTable->GetDescriptorTable() };
        m_commandList->setRayTracingState(state);

        SampleMiniConstants miniConstants = { uint4( *reinterpret_cast<uint*>(&m_services.settings.PostProcessEdgeDetectionThreshold), 0, 0, 0) };
        m_commandList->setPushConstants(&miniConstants, sizeof(miniConstants));
        m_commandList->dispatchRays(args);

        m_commandList->setTextureState(m_renderTargets->LdrColor, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);

        m_commandList->endMarker();
    }
}
void caustica::render::PathTracingWorldRenderer::renderGaussianSplats(bool renderToOutputColor)
{
    if (!m_services.settings.EnableGaussianSplats || m_services.hooks.gaussianSplatObjectsEmpty())
        return;

    const bool stochasticSplats = m_services.settings.EnableGaussianSplats && m_services.settings.GaussianSplatSortingMode == 1;
    if (stochasticSplats && (m_services.settings.ResetAccumulation || m_services.settings.ResetRealtimeCaches || m_gaussianSplatTemporalReset))
        m_gaussianSplatTemporalSampleIndex = 0;

    const uint32_t gaussianSplatShadowMode = ResolveGaussianSplatShadowMode(m_services.settings);
    GaussianSplatRenderSettings settings;
    settings.enabled = m_services.settings.EnableGaussianSplats;
    settings.depthTest = m_services.settings.GaussianSplatDepthTest;
    settings.sortingMode = m_services.settings.GaussianSplatSortingMode == 1 ? GaussianSplatSortMode::StochasticSplats : GaussianSplatSortMode::GpuSort;
    settings.renderTarget = renderToOutputColor ? GaussianSplatRenderTarget::OutputColor : GaussianSplatRenderTarget::ProcessedOutputColor;
    settings.frustumCulling = static_cast<GaussianSplatFrustumCulling>(dm::clamp(m_services.settings.GaussianSplatFrustumCulling, 0, 2));
    settings.projectionMethod = GaussianSplatProjectionMethod::Eigen;
    settings.shFormat = static_cast<GaussianSplatStorageFormat>(dm::clamp(m_services.settings.GaussianSplatSHFormat, 0, 2));
    settings.rgbaFormat = static_cast<GaussianSplatStorageFormat>(dm::clamp(m_services.settings.GaussianSplatRGBAFormat, 0, 2));
    settings.screenSizeCulling = m_services.settings.GaussianSplatScreenSizeCulling;
    settings.mipSplattingAntialiasing = m_services.settings.GaussianSplatMipAntialiasing;
    settings.useAABBs = m_services.settings.GaussianSplatUseAABBs;
    settings.useTLASInstances = m_services.settings.GaussianSplatUseTLASInstances;
    settings.blasCompaction = m_services.settings.GaussianSplatBlasCompaction;
    settings.splatScale = m_services.settings.GaussianSplatScale;
    settings.alphaScale = m_services.settings.GaussianSplatAlphaScale;
    settings.brightness = m_services.settings.GaussianSplatBrightness;
    settings.tintColor = m_services.settings.GaussianSplatTintColor;
    settings.alphaCullThreshold = m_services.settings.GaussianSplatAlphaCullThreshold;
    settings.shadowsEnabled = gaussianSplatShadowMode != GAUSSIAN_SPLAT_SHADOWS_DISABLED;
    settings.shadowMode = gaussianSplatShadowMode;
    settings.shadowStrength = m_services.settings.GaussianSplatShadowStrength;
    settings.shadowRayOffset = m_services.settings.GaussianSplatRtxParticleShadowOffset;
    settings.shadowSoftRadius = m_services.settings.GaussianSplatShadowSoftRadius;
    settings.shadowSoftSampleCount = ClampGaussianSplatSoftShadowSamples(m_services.settings.GaussianSplatShadowSoftSampleCount);
    settings.shadowFrameIndex = uint32_t(m_frameIndex & 0xffffffffu);
    settings.frustumDilation = m_services.settings.GaussianSplatFrustumDilation;
    settings.minPixelCoverage = m_services.settings.GaussianSplatMinPixelCoverage;
    if (stochasticSplats && m_services.settings.RealtimeMode)
        settings.stochasticFrameIndex = uint32_t(m_gaussianSplatTemporalSampleIndex);
    else
        settings.stochasticFrameIndex = uint32_t(m_sampleIndex >= 0
            ? uint32_t(m_sampleIndex)
            : uint32_t(m_frameIndex & 0xffffffffu));
    for (const auto& light : m_services.lights)
    {
        std::shared_ptr<DirectionalLight> dirLight = std::dynamic_pointer_cast<DirectionalLight>(light);
        if (dirLight != nullptr)
        {
            LightConstants lightConstants;
            dirLight->FillLightConstants(lightConstants);
            settings.shadowDirectionToLight = -lightConstants.direction;
            break;
        }
    }

    caustica::PlanarView splatView = *m_services.renderCore.camera().view();
    if (!renderToOutputColor)
    {
        splatView.SetViewport(nvrhi::Viewport(float(m_displaySize.x), float(m_displaySize.y)));
        splatView.SetPixelOffset(dm::float2::zero());
    }
    splatView.UpdateCache();

    bool renderedAny = false;
    m_services.hooks.renderSceneGaussianSplats(m_commandList, splatView, *m_renderTargets, settings, renderedAny);

    if (renderedAny && stochasticSplats && !renderToOutputColor)
        accumulateGaussianSplats(splatView);
}
void caustica::render::PathTracingWorldRenderer::accumulateGaussianSplats(const caustica::IView& splatView)
{
    if (m_gaussianSplatAccumulationPass == nullptr || m_renderTargets == nullptr || m_gaussianSplatCurrentColor == nullptr || m_gaussianSplatAccumulatedColor == nullptr)
        return;

    if (m_services.settings.ResetAccumulation || m_services.settings.ResetRealtimeCaches || m_gaussianSplatTemporalReset)
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
void caustica::render::PathTracingWorldRenderer::render(nvrhi::IFramebuffer* framebuffer)
{
    const auto& fbinfo = framebuffer->getFramebufferInfo();
    m_displaySize = m_renderSize = uint2(fbinfo.width, fbinfo.height);
    float lodBias = 0.f;

    preRender();

#if CAUSTICA_WITH_STREAMLINE
    streamlinePreRender();
#endif
#if CAUSTICA_WITH_NATIVE_DLSS
    nativeDLSSPreRender();
#endif

 
    m_displayAspectRatio = m_displaySize.x/(float)m_displaySize.y;

    m_services.renderCore.camera().ensureViews(m_renderSize);

    bool needNewPasses = false;
    if( m_renderTargets == nullptr || m_renderTargets->IsUpdateRequired( m_renderSize, m_displaySize ) )
    {
        device()->waitForIdle();
        device()->runGarbageCollection();
        for (int i = 0; i < std::size(m_nrd); i++)
            m_nrd[i] = nullptr;
        m_renderTargets = nullptr;
        m_oidnDenoisedOutput = nullptr;
        resetReferenceOIDN();
        m_services.bindingCache.Clear( );
        m_renderTargets = std::make_unique<RenderTargets>( );
        m_renderTargets->Init(device(), m_renderSize, m_displaySize, true, true, c_SwapchainCount);

        needNewPasses = true;
        m_services.hooks.onRenderTargetsRecreated();
    }

    // Environment map settings
    caustica::syncEnvMapSceneParams(m_services.settings, m_services.envMapSceneParams, c_envMapRadianceScale);

    if (m_services.hooks.consumeShaderReloadRequest())
    {
        m_services.shaderFactory->ClearCache();
        needNewPasses = true;
    }

    bool exposureResetRequired = false;

    if (m_services.settings.NRDModeChanged) // if changing between ReLAX and ReBLUR
    {
        needNewPasses = true;
        for (int i = 0; i < std::size(m_nrd); i++)
            m_nrd[i] = nullptr;
    }
    if (!m_services.settings.ActualUseStandaloneDenoiser()) // clean up the memory if not used
    {
        for (int i = 0; i < std::size(m_nrd); i++)
            m_nrd[i] = nullptr;
    }

    // Acceleration structures need some material info, whilst other passes need acceleration structures, so first set up materials if needed
    if (needNewPasses)
    {
        m_services.progressInitializingRenderer.Start("Initializing renderer...");

        if (m_services.materialsBaker == nullptr)
        {
            m_services.materialsBaker = std::make_shared<MaterialsBaker>(m_services.hooks.getMaterialSpecializationShader(), device(), m_services.textureCache, m_services.shaderFactory);
            assert( m_ptPipelineBaker == nullptr ); // there should be no cases where materials baker is null but ptPipelineBaker isn't
            
            m_ptPipelineBaker = std::make_shared<PTPipelineBaker>(device(), m_services.materialsBaker, m_bindingLayout, m_bindlessLayout);
            
            std::vector<std::filesystem::path> additionalShaderPaths;
            m_services.computePipelineBaker = std::make_shared<ComputePipelineBaker>(device(), additionalShaderPaths);
            
            m_services.hooks.createRTPipelines();
        }

        m_services.materialsBaker->CreateRenderPassesAndLoadMaterials(m_bindlessLayout, m_services.commonPasses, m_services.sceneManager.getScene(), m_services.sceneManager.getCurrentScenePath(), GetLocalPath(c_AssetsFolder));
        m_services.progressInitializingRenderer.Set(5);
        m_services.hooks.collectUncompressedTextures();
        if(m_services.ommBaker) m_services.ommBaker->CreateRenderPasses(m_bindlessLayout, m_services.commonPasses);
        m_services.progressInitializingRenderer.Set(20);

        (void)m_services.hooks.getOrCreateZoomTool();
    }

    // Changes to material properties and settings can require a BLAS/TLAS or subInstanceBuffer rebuild (alpha tested/exclusion flags etc); otherwise this is a no-op.
    m_services.hooks.recreateAccelStructs(m_commandList);

    if (m_services.settings.ActualUseRTXDIPasses() && m_rtxdiPass == nullptr )
        needNewPasses = true; // this will initialize rtxdi passes
    if (!m_services.settings.ActualUseRTXDIPasses())
        m_rtxdiPass = nullptr;

    // this will also create or update materials which can trigger the need to update acceleration structures
    if (needNewPasses)
    {
        m_services.progressInitializingRenderer.Set(40);
        device()->waitForIdle();    // some subsystems have resources that could still be in use and might be deleted - make sure that's safe
        m_commandList->open();
        createRenderPasses(exposureResetRequired, m_commandList);
        m_commandList->close();
        device()->executeCommandList(m_commandList);
        m_services.progressInitializingRenderer.Set(70);
    }

    // this is the point where main ray tracing pipelines will actually get compiled
    m_ptPipelineBaker->Update(m_services.sceneManager.getScene(), (unsigned int)m_services.renderCore.accelStructs().getSubInstanceData().size(), [this](std::vector<caustica::ShaderMacro> & macros){ m_services.hooks.fillPTPipelineGlobalMacros(macros); }, needNewPasses);
    
    // Update compute shaders (compile if needed)
    if (m_services.computePipelineBaker)
        m_services.computePipelineBaker->Update(needNewPasses);
    
    m_services.progressInitializingRenderer.Set(90);

    m_commandList->open();

    bool needNewBindings = false;
    PathTracerCameraData cameraData;
    {
        // Update camera data used by the path tracer & other systems
        m_services.hooks.updateViews(framebuffer);
        {   // TODO: pull all this to BridgeCamera - sizeX and sizeY are already inputs so we just need to pass projMatrix
            nvrhi::Viewport viewport = m_services.renderCore.camera().view()->GetViewport();
            float2 jitter = m_services.renderCore.camera().view()->GetPixelOffset();
            float4x4 projMatrix = m_services.renderCore.camera().view()->GetProjectionMatrix();
            float2 viewSize = { viewport.maxX - viewport.minX, viewport.maxY - viewport.minY };
            float outputAspectRatio = m_displayAspectRatio; //windowViewport.width() / windowViewport.height();    // render and display outputs might not match in case of lower DLSS/etc resolution rounding!
            bool rowMajor = true;
            float tanHalfFOVY = 1.0f / ((rowMajor) ? (projMatrix.m_data[1 * 4 + 1]) : (projMatrix.m_data[1 + 1 * 4]));
            float fovY = atanf(tanHalfFOVY) * 2.0f;
            cameraData = BridgeCamera(uint(viewSize.x), uint(viewSize.y), outputAspectRatio, m_services.renderCore.camera().camera().GetPosition(), m_services.renderCore.camera().camera().GetDir(), m_services.renderCore.camera().camera().GetUp(), fovY, m_services.renderCore.camera().zNear(), 1e7f, m_services.settings.CameraFocalDistance, m_services.settings.CameraAperture, jitter);
        }

        if (needNewPasses || needNewBindings || m_bindingSet == nullptr)
            m_shaderDebug->CreateRenderPasses(framebuffer, m_renderTargets->Depth);

        if (m_services.settings.EnableShaderDebug)
        {
            dm::float4x4 viewProj = m_services.renderCore.camera().view()->GetViewProjectionMatrix();
            m_shaderDebug->BeginFrame(m_commandList, viewProj);
        }

        // Scene refresh, accel structs, materials (partial sub-instance upload for lighting).
        UpdateSceneGeometryParams geoParams{
            m_services.settings,
            m_services.hooks.accelerationStructRebuildRequested(),
            m_services.sceneManager.getScene(),
            m_commandList,
        };
        geoParams.materialsBaker = m_services.materialsBaker.get();
        geoParams.ommBaker = m_services.ommBaker.get();
        geoParams.frameIndex = m_services.gpuDevice.GetFrameIndex();
        geoParams.asyncLoadingInProgress = &m_services.asyncLoadingInProgress;
        m_services.renderCore.updateSceneGeometry(geoParams);

        // Update input lighting, environment map, etc.
        preUpdateLighting(m_commandList, needNewBindings);

        // Early init for RTXDI
        if (m_rtxdiPass != nullptr) 
        {
            if (needNewPasses || needNewBindings || m_bindingSet == nullptr)
                m_rtxdiPass->Reset();
            rtxdiSetupFrame(framebuffer, cameraData, m_renderSize);
        }
    }

	if( needNewPasses || needNewBindings || m_bindingSet == nullptr )
    {
        m_services.progressInitializingRenderer.Set(95);
        RAII_SCOPE( m_commandList->close(); device()->executeCommandList(m_commandList);, m_commandList->open(););

        recreateBindingSet();

        m_services.progressInitializingRenderer.Set(100);

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
        m_services.progressInitializingRenderer.Stop();
    }

    m_toneMappingPass->PreRender(m_services.settings.ToneMappingParams);

    preUpdatePathTracing(needNewPasses, m_commandList);

    // I suppose we need to clear depth for right-click picking at least
    m_renderTargets->Clear( m_commandList );

    SampleConstants & constants = m_currentConstants; memset(&constants, 0, sizeof(constants));
    SampleMiniConstants miniConstants = { uint4(0, 0, 0, 0) }; // accessible but unused in path tracing at the moment
    if( m_services.sceneManager.getScene() == nullptr )
    {
        m_commandList->clearTextureFloat( m_renderTargets->OutputColor, nvrhi::AllSubresources, nvrhi::Color( 1, 1, 0, 0 ) );
        m_commandList->writeBuffer(m_constantBuffer, &constants, sizeof(constants));
    }
    else
    {
        updatePathTracerConstants(constants.ptConsts, cameraData);
        constants.MaterialCount = m_services.materialsBaker->GetMaterialDataCount(); // m_services.sceneManager.getScene()->GetSceneGraph()->GetMaterials().size();
        const uint32_t gaussianSplatShadowMode = ResolveGaussianSplatShadowMode(m_services.settings);
        const WorldRendererGaussianSplatBinding primaryGaussianBinding = m_services.hooks.getPrimaryGaussianSplatBinding();
        GaussianSplatPass* primaryGaussianSplatPass = const_cast<GaussianSplatPass*>(primaryGaussianBinding.pass);
        constants.GaussianSplatShadowCount = (m_services.settings.EnableGaussianSplats
                && gaussianSplatShadowMode != GAUSSIAN_SPLAT_SHADOWS_DISABLED
                && primaryGaussianSplatPass != nullptr
                && primaryGaussianSplatPass->GetTopLevelAS() != nullptr)
            ? primaryGaussianSplatPass->GetSplatCount()
            : 0;
        constants.GaussianSplatShadowsEnabled = constants.GaussianSplatShadowCount > 0 ? 1u : 0u;
        constants.GaussianSplatShadowScale = m_services.settings.GaussianSplatScale;
        constants.GaussianSplatShadowAlphaThreshold = m_services.settings.GaussianSplatAlphaCullThreshold;
        constants.GaussianSplatShadowUseTLASInstances =
            (primaryGaussianSplatPass != nullptr && primaryGaussianSplatPass->GetShadowUsesTLASInstances()) ? 1u : 0u;
        constants.GaussianSplatShadowPrimitiveCountPerSplat =
            primaryGaussianSplatPass != nullptr ? primaryGaussianSplatPass->GetShadowPrimitiveCountPerSplat() : 1u;
        constants.GaussianSplatShadowMode = constants.GaussianSplatShadowsEnabled != 0
            ? gaussianSplatShadowMode
            : GAUSSIAN_SPLAT_SHADOWS_DISABLED;
        constants.GaussianSplatShadowSoftRadius = m_services.settings.GaussianSplatShadowSoftRadius;
        constants.GaussianSplatShadowSoftSampleCount = ClampGaussianSplatSoftShadowSamples(m_services.settings.GaussianSplatShadowSoftSampleCount);
        constants.GaussianSplatShadowFrameIndex = uint32_t(m_frameIndex & 0xffffffffu);
        constants.GaussianSplatShadowRayOffset = m_services.settings.GaussianSplatRtxParticleShadowOffset;
        constants.GaussianSplatShadowAlphaScale = m_services.settings.GaussianSplatAlphaScale;
        constants.GaussianSplatShadowKernelMinResponse = kGaussianSplatKernelMinResponse;
        constants.GaussianSplatShadowKernelDegree = uint32_t(std::clamp(m_services.settings.GaussianSplatRtxKernelDegree, 0, 5));
        constants.GaussianSplatShadowAdaptiveClamp = m_services.settings.GaussianSplatRtxAdaptiveClamp ? 1u : 0u;
        constants.GaussianSplatShadowWorldToObject = primaryGaussianBinding.pass != nullptr
            ? inverse(primaryGaussianBinding.objectToWorld)
            : float4x4::identity();

        constants.envMapSceneParams = m_services.envMapSceneParams;
        constants.envMapImportanceSamplingParams = m_services.envMapBaker->GetImportanceSampling()->GetShaderParams();

        PlanarViewConstants view;           m_services.renderCore.camera().view()->FillPlanarViewConstants(view);
        PlanarViewConstants previousView;   m_services.renderCore.camera().viewPrevious()->FillPlanarViewConstants(previousView);
        constants.view          = FromPlanarViewConstants(view);
        constants.previousView  = FromPlanarViewConstants(previousView);

        constants.debug = {};
        constants.debug.pick = m_services.hooks.hasActivePickRequest() || m_services.settings.ContinuousDebugFeedback;
        constants.debug.pickX = (constants.debug.pick)?(m_services.settings.DebugPixel.x):(-1);
        constants.debug.pickY = (constants.debug.pick)?(m_services.settings.DebugPixel.y):(-1);
        constants.debug.debugLineScale = (m_services.settings.ShowDebugLines)?(m_services.settings.DebugLineScale):(0.0f);
        constants.debug.showWireframe = m_services.settings.ShowWireframe;
        constants.debug.debugViewType = (int)m_services.settings.DebugView;
        constants.debug.debugViewStablePlaneIndex = (m_services.settings.StablePlanesActiveCount==1)?(0):(m_services.settings.DebugViewStablePlaneIndex);
#if ENABLE_DEBUG_DELTA_TREE_VIZUALISATION
        constants.debug.exploreDeltaTree = (m_services.hooks.showDeltaTree() && constants.debug.pick)?(1):(0);
#else
        constants.debug.exploreDeltaTree = false;
#endif
        constants.debug.imageWidth = constants.ptConsts.imageWidth;
        constants.debug.imageHeight = constants.ptConsts.imageHeight;
        constants.debug.mouseX = m_services.settings.MousePos.x;
        constants.debug.mouseY = m_services.settings.MousePos.y;
        constants.debug.cameraPosW = constants.ptConsts.camera.PosW;
        constants.debug._padding0 = 0;

        constants.denoisingHitParamConsts = { m_services.settings.ReblurSettings.hitDistanceParameters.A, m_services.settings.ReblurSettings.hitDistanceParameters.B,
                                              m_services.settings.ReblurSettings.hitDistanceParameters.C, m_services.settings.ReblurSettings.hitDistanceParameters.D };

        // This updates all lighting: distant (environment maps and directional analytic lights) and local (analytic lights and emissive triangle lights)
        // Must go before m_constantBuffer as when saving screenshots it closes and re-opens command list, flushing the volatile constant buffer!
        updateLighting(m_commandList);
        m_services.hooks.uploadSubInstanceData(m_commandList); // this is now full subInstance data

        m_commandList->writeBuffer(m_constantBuffer, &constants, sizeof(constants));

        m_services.hooks.sampleRenderCode(framebuffer, m_commandList, constants);

        const bool stochasticSplats = m_services.settings.EnableGaussianSplats && m_services.settings.GaussianSplatSortingMode == 1;
        const bool stochasticUsesMainTemporal = stochasticSplats && (!m_services.settings.RealtimeMode || m_services.settings.RealtimeAA == 1);
        if (stochasticUsesMainTemporal)
            renderGaussianSplats(true);

        postProcessAA(framebuffer, needNewPasses || m_services.settings.ResetRealtimeCaches);
        applyReferenceOIDN();
        if (m_services.settings.ReferenceOIDNDenoiser)
            m_commandList->writeBuffer(m_constantBuffer, &constants, sizeof(constants));

        if (!stochasticUsesMainTemporal)
            renderGaussianSplats(false);
    }

    caustica::PlanarView fullscreenView = *m_services.renderCore.camera().view();
    nvrhi::Viewport windowViewport(float(m_displaySize.x), float(m_displaySize.y));
    fullscreenView.SetViewport(windowViewport);
    fullscreenView.UpdateCache();

    postProcessPreToneMapping(m_commandList, fullscreenView);   // writing to m_renderTargets->ProcessedOutputColor

    //Tone Mapping; it will read from m_renderTargets->ProcessedOutputColor and write into m_renderTargets->LdrColor; in case tonemapping is disabled, it's just a passthrough
    if (m_toneMappingPass->Render(m_commandList, fullscreenView, m_renderTargets->ProcessedOutputColor, m_services.settings.EnableToneMapping))
    {
        // first run tonemapper can close & re-open command list - when that happens, we have to re-upload volatile constants
        m_commandList->writeBuffer(m_constantBuffer, &constants, sizeof(constants));
    }

    postProcessPostToneMapping(m_commandList, fullscreenView);  // writing to m_renderTargets->LdrColor

    //m_postProcess->Render(m_commandList, m_renderTargets->LdrColor);

    if (m_services.settings.EnableShaderDebug)
        m_shaderDebug->EndFrameAndOutput(m_commandList, m_renderTargets->LdrFramebuffer->GetFramebuffer(fullscreenView), m_renderTargets->Depth, fbinfo.getViewport());

    m_services.hooks.getOrCreateZoomTool()->Render(m_commandList, m_renderTargets->LdrColor);

    m_commandList->beginMarker("Blit");
    (m_services.commonPasses)->BlitTexture(m_commandList, framebuffer, m_renderTargets->LdrColor, &m_services.bindingCache);
    m_commandList->endMarker();

    if (m_services.settings.ShowDebugLines == true)
    {
        m_commandList->beginMarker("Debug Lines");

        // this draws the debug lines - should be the only actual rasterization around :)
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
            // using m_debugLineBufferCapture for direct drawing here
            m_commandList->writeBuffer( m_debugLineBufferCapture, m_cpuSideDebugLines.data(), sizeof(DebugLineStruct) * m_cpuSideDebugLines.size() );

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

    if( m_services.settings.ContinuousDebugFeedback || m_services.hooks.pickMaterialRequested() )
    {
        m_commandList->copyBuffer(m_feedback_Buffer_Cpu, 0, m_feedback_Buffer_Gpu, 0, sizeof(DebugFeedbackStruct) * 1);
        m_commandList->copyBuffer(m_debugLineBufferDisplay, 0, m_debugLineBufferCapture, 0, sizeof(DebugLineStruct) * MAX_DEBUG_LINES );
        m_commandList->copyBuffer(m_debugDeltaPathTree_Cpu, 0, m_debugDeltaPathTree_Gpu, 0, sizeof(DeltaTreeVizPathVertex) * cDeltaTreeVizMaxVertices);
	}

    nvrhi::ITexture* framebufferTexture = framebuffer->getDesc().colorAttachments[0].texture;


	m_commandList->close();
	device()->executeCommandList(m_commandList);

    // resolve picking and debug info
    if (m_services.settings.ContinuousDebugFeedback || m_services.hooks.hasActivePickRequest())
    {
        device()->waitForIdle();
        void* pData = device()->mapBuffer(m_feedback_Buffer_Cpu, nvrhi::CpuAccessMode::Read);
        assert(pData);
        memcpy(&m_feedbackData, pData, sizeof(DebugFeedbackStruct)* 1);
        device()->unmapBuffer(m_feedback_Buffer_Cpu);

        pData = device()->mapBuffer(m_debugDeltaPathTree_Cpu, nvrhi::CpuAccessMode::Read);
        assert(pData);
        memcpy(&m_debugDeltaPathTree, pData, sizeof(DeltaTreeVizPathVertex) * cDeltaTreeVizMaxVertices);
        device()->unmapBuffer(m_debugDeltaPathTree_Cpu);

        m_services.hooks.resolvePickFeedback(m_feedbackData);
        m_services.hooks.clearPickRequests();
    }

    m_services.hooks.captureScriptPostRender([this, framebufferTexture](const char* fileName) -> bool {
        return SaveTextureToFile(device(), m_services.commonPasses.get(), framebufferTexture, nvrhi::ResourceStates::Common, fileName);
    });

    if (m_services.hooks.consumeExperimentalPhotoScreenshot())
    {
        denoisedScreenshot(framebufferTexture);
    }

    if (m_temporalAntiAliasingPass != nullptr)
        m_temporalAntiAliasingPass->AdvanceFrame();

	m_services.renderCore.camera().swapViews();
	m_services.gpuDevice.SetVsyncEnabled(m_services.settings.ActualEnableVsync());

    postUpdatePathTracing();
}
void caustica::render::PathTracingWorldRenderer::recreateBindingSet()
{
	// WARNING: this must match the layout of the m_bindingLayout (or switch to CreateBindingSetAndLayout)
    nvrhi::rt::IAccelStruct* gaussianSplatAS = m_services.renderCore.accelStructs().getTopLevelAS();
    nvrhi::IBuffer* gaussianSplatBuffer = m_services.materialsBaker->GetMaterialDataBuffer();
    const WorldRendererGaussianSplatBinding primaryGaussianBinding = m_services.hooks.getPrimaryGaussianSplatBinding();
    const GaussianSplatPass* primaryGaussianSplatPass = primaryGaussianBinding.pass;
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
        //nvrhi::BindingSetItem::ConstantBuffer(2, m_services.lightsBaker->GetLightingConstants()),
        nvrhi::BindingSetItem::RayTracingAccelStruct(0, m_services.renderCore.accelStructs().getTopLevelAS()),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(1, m_services.renderCore.accelStructs().getSubInstanceBuffer()),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(2, m_services.sceneManager.getScene()->GetInstanceBuffer()),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(3, m_services.sceneManager.getScene()->GetGeometryBuffer()),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(4, m_services.ommBaker ?(m_services.ommBaker->GetGeometryDebugBuffer()):(m_services.materialsBaker->GetMaterialDataBuffer().Get()) ),   // YUCK
        nvrhi::BindingSetItem::StructuredBuffer_SRV(5, m_services.materialsBaker->GetMaterialDataBuffer()),
        nvrhi::BindingSetItem::Texture_SRV(6,  m_renderTargets->LdrColorScratch, nvrhi::Format::SRGBA8_UNORM),
        nvrhi::BindingSetItem::RayTracingAccelStruct(7, gaussianSplatAS),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(8, gaussianSplatBuffer),
        nvrhi::BindingSetItem::Texture_SRV(10, m_services.envMapBaker->GetEnvMapCube()), //m_app.m_EnvironmentMap->IsEnvMapLoaded() ? m_app.m_EnvironmentMap->GetEnvironmentMap() : (m_services.commonPasses)->m_BlackTexture),
        nvrhi::BindingSetItem::Texture_SRV(11, m_services.envMapBaker->GetImportanceSampling()->GetImportanceMapOnly()), //m_app.m_EnvironmentMap->IsImportanceMapLoaded() ? m_app.m_EnvironmentMap->GetImportanceMap() : (m_services.commonPasses)->m_BlackTexture),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(12, m_services.lightsBaker->GetControlBuffer()),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(13, m_services.lightsBaker->GetLightBuffer()),
        nvrhi::BindingSetItem::StructuredBuffer_SRV(14, m_services.lightsBaker->GetLightExBuffer()),
        nvrhi::BindingSetItem::TypedBuffer_SRV(15, m_services.lightsBaker->GetLightProxyCounters()),     // t_tightProxyCounters
        nvrhi::BindingSetItem::TypedBuffer_SRV(16, m_services.lightsBaker->GetLightSamplingProxies()),   // t_LightProxyIndices
        nvrhi::BindingSetItem::TypedBuffer_SRV(17, m_services.lightsBaker->GetLocalSamplingBuffer()),    // t_LightLocalSamplingBuffer
        nvrhi::BindingSetItem::Texture_SRV(18, m_services.lightsBaker->GetEnvLightLookupMap()),          // t_EnvLookupMap
        //nvrhi::BindingSetItem::TypedBuffer_SRV(19, ),
        nvrhi::BindingSetItem::Texture_UAV(20, m_services.lightsBaker->GetFeedbackTotalWeight()),        // u_LightFeedbackTotalWeight
        nvrhi::BindingSetItem::Texture_UAV(21, m_services.lightsBaker->GetFeedbackCandidates()),         // u_LightFeedbackCandidates
        nvrhi::BindingSetItem::Sampler(0, (m_services.commonPasses)->m_AnisotropicWrapSampler),
        nvrhi::BindingSetItem::Sampler(1, m_services.envMapBaker->GetEnvMapCubeSampler()),
        nvrhi::BindingSetItem::Sampler(2, m_services.envMapBaker->GetImportanceSampling()->GetImportanceMapSampler()),
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
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(80, (m_services.commonPasses)->m_BlackCubeMapArray));  // t_LocalCubemapGGX
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(81, (m_services.commonPasses)->m_BlackCubeMapArray));  // t_DiffuseIrradianceCube
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(82, (m_services.commonPasses)->m_BlackTexture));  // t_SSRBlurChain
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(83, (m_services.envMapBaker->GetBRDFLUT()!=nullptr)?m_services.envMapBaker->GetBRDFLUT():(m_services.commonPasses)->m_BlackTexture ));  // t_BRDFLUT
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(84, (m_services.commonPasses)->m_BlackTexture));  // t_DepthHierarchy placeholder
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::ConstantBuffer(10, m_constantBuffer)); // ReflectionConstants (reuse main constant buffer as placeholder)
        
        // SSR result UAV placeholder
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_UAV(85, m_renderTargets->Depth));   // u_SSRResult placeholder

        // GTAO output (default to white = no occlusion; overridden by AddCustomBindings)
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(86, (m_services.commonPasses)->m_WhiteTexture));  // t_GTAOOutput placeholder
        // Previous frame depth (default to black = zero depth; overridden by AddCustomBindings)
        bindingSetDesc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(87, (m_services.commonPasses)->m_BlackTexture));  // t_PrevDepth placeholder

        // Allow derived classes to customize bindings (e.g., add reflection textures, GTAO output)
        m_services.hooks.addCustomBindings(bindingSetDesc);

        m_bindingSet = device()->createBindingSet(bindingSetDesc, m_bindingLayout);
    }
}
void caustica::render::PathTracingWorldRenderer::pathTrace(nvrhi::IFramebuffer* framebuffer, const SampleConstants & constants)
{
    //m_commandList->beginMarker("MainRendering"); <- removed (for now) since added hierarchy reduces readability

    bool useStablePlanes = m_services.settings.RealtimeMode;

    nvrhi::rt::State state;

    nvrhi::rt::DispatchRaysArguments args;
    nvrhi::Viewport viewport = m_services.renderCore.camera().view()->GetViewport();
    uint32_t width = (uint32_t)(viewport.maxX - viewport.minX);
    uint32_t height = (uint32_t)(viewport.maxY - viewport.minY);
    args.width = width;
    args.height = height;

    // default miniConstants
    SampleMiniConstants miniConstants = { uint4(0, 0, 0, 0) };

    if (useStablePlanes)
    {
        {
            RAII_SCOPE(m_commandList->beginMarker("PathTracePrePass"); , m_commandList->endMarker(); );

            m_commandList->setTextureState(m_renderTargets->Depth, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            m_commandList->setTextureState(m_renderTargets->ScreenMotionVectors, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            m_commandList->setTextureState(m_renderTargets->Throughput, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            m_commandList->setTextureState(m_renderTargets->SpecularHitT, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            state.shaderTable = m_ptPipelineBuildStablePlanes->GetShaderTable();
            state.bindings = { m_bindingSet, m_services.descriptorTable->GetDescriptorTable() };
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
		    state.bindings = { m_bindingSet, m_services.descriptorTable->GetDescriptorTable() };
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
        m_services.lightsBaker.get(),
        &m_services.bindingCache,
        m_services.sceneManager.getScene(),
        m_services.materialsBaker,
        m_services.ommBaker,
        m_services.renderCore.accelStructs().getSubInstanceBuffer(),
        m_renderTargets->Depth,
        m_renderTargets->ScreenMotionVectors,
    };
    m_services.renderCore.updateLightingEnd(lightingEndParams);

    {
        RAII_SCOPE( m_commandList->beginMarker("PathTrace");, m_commandList->endMarker(); );

        state.shaderTable = ((useStablePlanes) ? (m_ptPipelineFillStablePlanes) : (m_ptPipelineReference))->GetShaderTable();
        state.bindings = { m_bindingSet, m_services.descriptorTable->GetDescriptorTable() };

        for (uint subSampleIndex = 0; subSampleIndex < m_services.settings.ActualSamplesPerPixel(); subSampleIndex++)
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
    bool useFusedDIGIFinal = m_services.settings.ActualUseReSTIRDI() && m_services.settings.ActualUseReSTIRGI() && enableFusedDIGIFinal;

    if (m_services.settings.ActualUseRTXDIPasses())
    {
        RAII_SCOPE( m_commandList->beginMarker("RTXDI");, m_commandList->endMarker(); );

        // this does all ReSTIR DI magic including applying the final sample into correct radiance buffer (depending on denoiser state)
        if (m_services.settings.ActualUseReSTIRDI())
            m_rtxdiPass->Execute(m_commandList, m_bindingSet, useFusedDIGIFinal);

        if (m_services.settings.ActualUseReSTIRGI())
            m_rtxdiPass->ExecuteGI(m_commandList, m_bindingSet, useFusedDIGIFinal);

        if (useFusedDIGIFinal)
            m_rtxdiPass->ExecuteFusedDIGIFinal(m_commandList, m_bindingSet);

        if (m_services.settings.ActualUseReSTIRPT())
            m_rtxdiPass->ExecutePT(m_commandList, m_bindingSet);
    }

    {
        RAII_SCOPE(m_commandList->beginMarker("Denoising Guides Bake"); , m_commandList->endMarker(); );

        m_denoisingGuidesBaker->DenoiseSpecHitT(m_commandList, m_bindingSet);
        m_denoisingGuidesBaker->ComputeAvgLayerRadiance(m_commandList, m_bindingSet);

        if (m_services.settings.DebugView != DebugViewType::Disabled)
            m_denoisingGuidesBaker->RenderDebugViz(m_commandList, m_services.settings.DebugView, m_bindingSet);
    }

    if (useStablePlanes && (m_services.settings.DebugView > DebugViewType::Disabled && m_services.settings.DebugView <= DebugViewType::StablePlane_SpecRadiance || m_services.settings.DebugView == DebugViewType::StableRadiance) )
    {
        m_commandList->beginMarker("StablePlanesDebugViz");
        nvrhi::TextureDesc tdesc = m_renderTargets->OutputColor->getDesc();
        m_postProcess->Apply(m_commandList, PostProcess::ComputePassType::StablePlanesDebugViz, m_constantBuffer, miniConstants, m_bindingSet, m_bindingLayout, tdesc.width, tdesc.height);
        m_commandList->endMarker();

    }
}
void caustica::render::PathTracingWorldRenderer::denoise(nvrhi::IFramebuffer* framebuffer)
{
    if( !m_services.settings.ActualUseStandaloneDenoiser() )
        return;

    for (int i = 0; i < std::size(m_nrd); i++)
    {
        if (m_nrd[i] == nullptr)
        {
            nrd::Denoiser denoiserMethod = m_services.settings.NRDMethod == NrdConfig::DenoiserMethod::REBLUR ?
                nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR : nrd::Denoiser::RELAX_DIFFUSE_SPECULAR;

            m_nrd[i] = std::make_unique<NrdIntegration>(device(), denoiserMethod);
            m_nrd[i]->Initialize(m_renderSize.x, m_renderSize.y, *m_services.shaderFactory);
        }
    }

    //const auto& fbinfo = framebuffer->getFramebufferInfo();
    const char* passNames[] = { "Denoising plane 0", "Denoising plane 1", "Denoising plane 2", "Denoising plane 3" }; assert( std::size(m_nrd) <= std::size(passNames) );

    bool nrdUseRelax = m_services.settings.NRDMethod == NrdConfig::DenoiserMethod::RELAX;
    PostProcess::ComputePassType preparePassType = nrdUseRelax ? PostProcess::ComputePassType::RELAXDenoiserPrepareInputs : PostProcess::ComputePassType::REBLURDenoiserPrepareInputs;
    PostProcess::ComputePassType mergePassType = nrdUseRelax ? PostProcess::ComputePassType::RELAXDenoiserFinalMerge : PostProcess::ComputePassType::REBLURDenoiserFinalMerge;

    bool resetHistory = m_services.settings.ResetRealtimeCaches;

    int maxPassCount = std::min(m_services.settings.StablePlanesActiveCount, (int)std::size(m_nrd));
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

        const float timeDeltaBetweenFrames = m_services.gpuDevice.GetDeviceParams().headlessDevice ? 1.f/60.f : -1.f; // if we're rendering without a window we set a fix timeDeltaBetweenFrames to ensure that output is deterministic
        bool enableValidation = m_services.settings.DebugView == DebugViewType::StablePlane_DenoiserValidation;
        if (nrdUseRelax)
        {
            m_nrd[pass]->RunDenoiserPasses(m_commandList, *m_renderTargets, pass, *m_services.renderCore.camera().view(), *m_services.renderCore.camera().viewPrevious(), m_services.gpuDevice.GetFrameIndex(), m_services.settings.NRDDisocclusionThreshold, m_services.settings.NRDDisocclusionThresholdAlternate, m_services.settings.NRDUseAlternateDisocclusionThresholdMix, timeDeltaBetweenFrames, enableValidation, resetHistory, &m_services.settings.RelaxSettings);
        }
        else
        {
            m_nrd[pass]->RunDenoiserPasses(m_commandList, *m_renderTargets, pass, *m_services.renderCore.camera().view(), *m_services.renderCore.camera().viewPrevious(), m_services.gpuDevice.GetFrameIndex(), m_services.settings.NRDDisocclusionThreshold, m_services.settings.NRDDisocclusionThresholdAlternate, m_services.settings.NRDUseAlternateDisocclusionThresholdMix, timeDeltaBetweenFrames, enableValidation, resetHistory, &m_services.settings.ReblurSettings);
        }

        m_commandList->beginMarker("MergeOutputs");
        m_postProcess->Apply(m_commandList, mergePassType, pass, m_constantBuffer, miniConstants, m_renderTargets->OutputColor, *m_renderTargets, nullptr);
        m_commandList->endMarker();

        m_commandList->endMarker();
    }
}
#if CAUSTICA_WITH_NATIVE_DLSS
bool caustica::render::PathTracingWorldRenderer::evaluateNativeDLSS(bool reset)
{
    if (!m_nativeDLSS || !(m_services.settings.RealtimeAA == 2 || m_services.settings.RealtimeAA == 3))
        return false;

    const bool useRayReconstruction = m_services.settings.RealtimeAA == 3;
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
    evaluateParams.resetHistory = reset || m_services.settings.ResetRealtimeCaches;

    if (useRayReconstruction)
    {
        evaluateParams.diffuseAlbedo = m_renderTargets->RRDiffuseAlbedo;
        evaluateParams.specularAlbedo = m_renderTargets->RRSpecAlbedo;
        evaluateParams.normalRoughness = m_renderTargets->RRNormalsAndRoughness;
    }

    const bool evaluated = m_nativeDLSS->Evaluate(m_commandList, evaluateParams, *m_services.renderCore.camera().view());
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
void caustica::render::PathTracingWorldRenderer::postProcessAA(nvrhi::IFramebuffer* framebuffer, bool reset)
{
    (void)framebuffer;

    PostProcessAAParams params{
        m_services.settings,
        m_commandList,
        m_renderTargets.get(),
        &m_services.gpuDevice,
    };
    params.renderSize = m_renderSize;
    params.displaySize = m_displaySize;
    params.displayAspectRatio = m_displayAspectRatio;
    params.cameraJitter = m_services.hooks.computeCameraJitter(m_sampleIndex);
    params.sampleIndex = m_sampleIndex;
    params.frameIndex = m_services.gpuDevice.GetFrameIndex();
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

    m_services.renderCore.postProcessAA(params);

#if CAUSTICA_WITH_NATIVE_DLSS
    if (m_services.settings.RealtimeMode)
    {
        bool nativeDLSSEvaluated = false;
        if (m_services.settings.RealtimeAA == 2 || m_services.settings.RealtimeAA == 3)
            nativeDLSSEvaluated = evaluateNativeDLSS(reset);

        if (!nativeDLSSEvaluated && (m_services.settings.RealtimeAA == 2 || m_services.settings.RealtimeAA == 3))
        {
            if (m_services.settings.ActualUseStandaloneDenoiser())
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
void caustica::render::PathTracingWorldRenderer::resetReferenceOIDN()
{
    m_oidnDenoisedOutputValid = false;
    m_oidnDenoiserFailed = false;

    if (m_oidnDenoiser)
        m_oidnDenoiser->Reset();
}
void caustica::render::PathTracingWorldRenderer::applyReferenceOIDN()
{
    if (m_services.settings.RealtimeMode || !m_services.settings.ReferenceOIDNDenoiser || m_renderTargets == nullptr)
        return;

#if CAUSTICA_WITH_OIDN
    const bool accumulationReady = m_accumulationCompleted || m_accumulationSampleIndex >= m_services.settings.AccumulationTarget;
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
    oidnOptions.UseGPU = m_services.settings.ReferenceOIDNUseGPU;
    oidnOptions.GuidePasses = static_cast<OidnDenoiser::Passes>(std::clamp(m_services.settings.ReferenceOIDNPasses, 0, 2));
    oidnOptions.GuidePrefilter = static_cast<OidnDenoiser::Prefilter>(std::clamp(m_services.settings.ReferenceOIDNPrefilter, 0, 2));
    oidnOptions.FilterQuality = static_cast<OidnDenoiser::Quality>(std::clamp(m_services.settings.ReferenceOIDNQuality, 0, 2));

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
void caustica::render::PathTracingWorldRenderer::denoisedScreenshot(nvrhi::ITexture * framebufferTexture) const
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

        if (!SaveTextureToFile(device(), m_services.commonPasses.get(), framebufferTexture, nvrhi::ResourceStates::Common, noisyImagePath.c_str()))
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
