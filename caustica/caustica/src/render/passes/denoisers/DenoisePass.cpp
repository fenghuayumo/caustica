#include <render/passes/denoisers/DenoisePass.h>

#include <render/FrameGraphContext.h>
#include <render/PathTracingContext.h>
#include <render/core/CameraController.h>
#include <render/core/PostProcessAA.h>
#include <render/core/PathTracerSettings.h>
#include <render/core/RenderTargets.h>
#include <render/passes/postProcess/DenoisingGuidesPass.h>
#include <render/passes/postProcess/PostProcess.h>
#include <render/passes/denoisers/NrdIntegration.h>
#include <render/passes/denoisers/OidnDenoiser.h>
#include <render/passes/geometry/TemporalAntiAliasingPass.h>
#include <render/passes/postProcess/AccumulationPass.h>
#include <backend/GpuDevice.h>
#include <core/log.h>
#include <core/scope.h>
#include <math/float.h>
#include <rhi/utils.h>
#include <shaders/FrameConstantBuffer.h>

#include <algorithm>
#include <cmath>
#include <vector>

#if CAUSTICA_WITH_NATIVE_DLSS
#include <render/passes/geometry/DLSS.h>
#endif

using namespace caustica;
using namespace caustica::math;
using namespace caustica::render;

DenoisePass::DenoisePass() = default;
DenoisePass::~DenoisePass() = default;

#if CAUSTICA_WITH_OIDN
namespace
{
    float readR11G11B10FloatChannel(uint32_t packed, uint32_t channel)
    {
        uint16_t halfBits = 0;
        switch (channel)
        {
        case 0: halfBits = uint16_t((packed << 4) & 0x7FF0); break;
        case 1: halfBits = uint16_t((packed >> 7) & 0x7FF0); break;
        default: halfBits = uint16_t((packed >> 17) & 0x7FE0); break;
        }

        const float value = float16ToFloat32(float16_t{ halfBits });
        return std::isfinite(value) ? std::max(value, 0.0f) : 0.0f;
    }

    caustica::rhi::TextureDesc makeReadbackTextureDesc(caustica::rhi::TextureDesc desc, const char* debugName)
    {
        desc.debugName = debugName;
        desc.isRenderTarget = false;
        desc.isUAV = false;
        desc.isTypeless = false;
        desc.initialState = caustica::rhi::ResourceStates::CopyDest;
        desc.keepInitialState = true;
        return desc;
    }

    void readR11G11B10Float3Staging(
        caustica::rhi::Device* device,
        caustica::rhi::StagingTexture* stagingTexture,
        uint32_t width,
        uint32_t height,
        std::vector<float>& output)
    {
        size_t rowPitch = 0;
        const uint8_t* mappedData = static_cast<const uint8_t*>(device->mapStagingTexture(
            stagingTexture, caustica::rhi::TextureSlice(), caustica::rhi::CpuAccessMode::Read, &rowPitch));
        if (mappedData == nullptr)
            return;

        output.resize(size_t(width) * size_t(height) * 3);
        for (uint32_t y = 0; y < height; y++)
        {
            const uint32_t* row = reinterpret_cast<const uint32_t*>(mappedData + size_t(y) * rowPitch);
            for (uint32_t x = 0; x < width; x++)
            {
                const size_t targetOffset = (size_t(y) * size_t(width) + size_t(x)) * 3;
                const uint32_t packed = row[x];
                output[targetOffset + 0] = readR11G11B10FloatChannel(packed, 0);
                output[targetOffset + 1] = readR11G11B10FloatChannel(packed, 1);
                output[targetOffset + 2] = readR11G11B10FloatChannel(packed, 2);
            }
        }

        device->unmapStagingTexture(stagingTexture);
    }

    void readRGBA16Float3Staging(
        caustica::rhi::Device* device,
        caustica::rhi::StagingTexture* stagingTexture,
        uint32_t width,
        uint32_t height,
        std::vector<float>& output)
    {
        size_t rowPitch = 0;
        const uint8_t* mappedData = static_cast<const uint8_t*>(device->mapStagingTexture(
            stagingTexture, caustica::rhi::TextureSlice(), caustica::rhi::CpuAccessMode::Read, &rowPitch));
        if (mappedData == nullptr)
            return;

        output.resize(size_t(width) * size_t(height) * 3);
        for (uint32_t y = 0; y < height; y++)
        {
            const float16_t4* row = reinterpret_cast<const float16_t4*>(mappedData + size_t(y) * rowPitch);
            for (uint32_t x = 0; x < width; x++)
            {
                const float4 value = float16ToFloat32x4(row[x]);
                const size_t targetOffset = (size_t(y) * size_t(width) + size_t(x)) * 3;
                output[targetOffset + 0] = std::isfinite(value.x) ? std::clamp(value.x, -1.0f, 1.0f) : 0.0f;
                output[targetOffset + 1] = std::isfinite(value.y) ? std::clamp(value.y, -1.0f, 1.0f) : 0.0f;
                output[targetOffset + 2] = std::isfinite(value.z) ? std::clamp(value.z, -1.0f, 1.0f) : 1.0f;
            }
        }

        device->unmapStagingTexture(stagingTexture);
    }
}
#endif

void DenoisePass::createGuides(
    PathTracingContext* context,
    caustica::rhi::Device* device,
    const std::shared_ptr<caustica::ShaderFactory>& shaderFactory,
    const std::unique_ptr<RenderTargets>& renderTargets,
    const std::shared_ptr<ShaderDebug>& shaderDebug,
    caustica::rhi::BindingLayoutHandle bindingLayout)
{
    m_context = context;
    m_device = device;
    m_denoisingGuidesPass = std::make_shared<DenoisingGuidesPass>(
        device, shaderFactory, renderTargets, shaderDebug, bindingLayout);
}

void DenoisePass::bindFrame(const FrameGraphContext& ctx)
{
    m_context = ctx.pathTracingContext ? ctx.pathTracingContext : m_context;
    m_device = ctx.device ? ctx.device : m_device;
    m_renderTargets = ctx.renderTargets;
    m_postProcess = ctx.postProcess;
    m_bindingSet = ctx.bindingSet;
    m_bindingLayout = ctx.bindingLayout;
    m_constantBuffer = ctx.constantBuffer;
    m_commandList = ctx.commandList;
    m_renderSize = ctx.renderSize;
    m_displaySize = ctx.displaySize;
    m_displayAspectRatio = ctx.displayAspectRatio;
    m_cameraJitter = ctx.cameraJitter;
    m_sampleIndex = ctx.sampleIndex;
    m_frameIndex = ctx.frameIndex;
    m_accumulationSampleIndex = ctx.accumulationSampleIndex;
    m_accumulationCompleted = ctx.accumulationCompleted;
    m_gaussianSplatTemporalSampleIndex = ctx.gaussianSplatTemporalSampleIndex;
    m_gaussianSplatTemporalReset = ctx.gaussianSplatTemporalReset;
    m_temporalAntiAliasing = ctx.temporalAntiAliasing;
    m_accumulation = ctx.accumulation;
    m_camera = ctx.camera;
#if CAUSTICA_WITH_STREAMLINE
    m_dlssRROptions = ctx.dlssRROptions;
#endif
#if CAUSTICA_WITH_NATIVE_DLSS
    m_nativeDLSS = ctx.nativeDLSS;
#endif
}

void DenoisePass::denoiseSpecHitT(caustica::rhi::CommandList* commandList)
{
    assert(commandList);
    assert(m_denoisingGuidesPass);
    assert(m_bindingSet);

    m_denoisingGuidesPass->denoiseSpecHitT(commandList, m_bindingSet);
}

void DenoisePass::computeAvgLayerRadiance(caustica::rhi::CommandList* commandList)
{
    assert(commandList);
    assert(m_denoisingGuidesPass);
    assert(m_context);
    assert(m_bindingSet);

    m_denoisingGuidesPass->computeAvgLayerRadiance(commandList, m_bindingSet);

    if (m_context->activeSettings().DebugView != DebugViewType::Disabled)
        m_denoisingGuidesPass->renderDebugViz(
            commandList,
            m_context->activeSettings().DebugView,
            m_bindingSet);
}

void DenoisePass::prepareGuides(caustica::rhi::CommandList* commandList)
{
    assert(commandList);

    RAII_SCOPE(commandList->beginMarker("Denoising Guides Bake"); , commandList->endMarker(); );

    denoiseSpecHitT(commandList);
    computeAvgLayerRadiance(commandList);
}

void DenoisePass::stablePlanesDebugViz(caustica::rhi::CommandList* commandList)
{
    assert(commandList);
    assert(m_postProcess);
    assert(m_renderTargets);

    FrameMiniConstants miniConstants = { uint4(0, 0, 0, 0) };

    commandList->beginMarker("StablePlanesDebugViz");
    caustica::rhi::TextureDesc tdesc = m_renderTargets->outputColor->getDesc();
    m_postProcess->apply(
        commandList,
        PostProcess::ComputePassType::StablePlanesDebugViz,
        m_constantBuffer,
        miniConstants,
        m_bindingSet,
        m_bindingLayout,
        tdesc.width,
        tdesc.height);
    commandList->endMarker();
}

void DenoisePass::ensureNrdIntegrations()
{
    assert(m_context);
    assert(m_device);

    if (!m_context->activeSettings().actualUseStandaloneDenoiser())
        return;

    for (int i = 0; i < std::size(m_nrd); i++)
    {
        if (m_nrd[i] != nullptr)
            continue;

        nrd::Denoiser denoiserMethod = m_context->activeSettings().NRDMethod == NrdConfig::DenoiserMethod::REBLUR
            ? nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR
            : nrd::Denoiser::RELAX_DIFFUSE_SPECULAR;

        m_nrd[i] = std::make_unique<NrdIntegration>(m_device, denoiserMethod);
        m_nrd[i]->initialize(m_renderSize.x, m_renderSize.y, *m_context->shaderFactory);
    }
}

namespace
{

FrameMiniConstants makeNrdPlaneMiniConstants(const PathTracerSettings& settings, int planeIndex)
{
    const int maxPassCount = std::min(
        settings.StablePlanesActiveCount,
        static_cast<int>(cStablePlaneCount));
    const bool initWithStableRadiance = planeIndex == (maxPassCount - 1);
    return { uint4(static_cast<uint>(planeIndex), initWithStableRadiance ? 1u : 0u, 0, 0) };
}

} // namespace

void DenoisePass::prepareNrdInputs(caustica::rhi::CommandList* commandList, int planeIndex)
{
    assert(commandList);
    assert(m_context);
    assert(m_renderTargets);
    assert(m_postProcess);
    assert(planeIndex >= 0 && planeIndex < static_cast<int>(std::size(m_nrd)));

    if (!m_context->activeSettings().actualUseStandaloneDenoiser())
        return;

    const bool nrdUseRelax = m_context->activeSettings().NRDMethod == NrdConfig::DenoiserMethod::RELAX;
    const PostProcess::ComputePassType preparePassType = nrdUseRelax
        ? PostProcess::ComputePassType::RELAXDenoiserPrepareInputs
        : PostProcess::ComputePassType::REBLURDenoiserPrepareInputs;

    FrameMiniConstants miniConstants = makeNrdPlaneMiniConstants(m_context->activeSettings(), planeIndex);
    caustica::rhi::TextureDesc tdesc = m_renderTargets->outputColor->getDesc();

    commandList->beginMarker("PrepareInputs");
    m_postProcess->apply(
        commandList,
        preparePassType,
        m_constantBuffer,
        miniConstants,
        m_bindingSet,
        m_bindingLayout,
        tdesc.width,
        tdesc.height);
    commandList->endMarker();
}

void DenoisePass::runNrd(caustica::rhi::CommandList* commandList, int planeIndex)
{
    assert(commandList);
    assert(m_context);
    assert(m_renderTargets);
    assert(planeIndex >= 0 && planeIndex < static_cast<int>(std::size(m_nrd)));
    assert(m_nrd[planeIndex] != nullptr);

    if (!m_context->activeSettings().actualUseStandaloneDenoiser())
        return;

    const bool nrdUseRelax = m_context->activeSettings().NRDMethod == NrdConfig::DenoiserMethod::RELAX;
    const bool resetHistory = m_context->activeSettings().ResetRealtimeCaches;
    const float timeDeltaBetweenFrames = m_context->gpuDevice.isHeadless() ? 1.f / 60.f : -1.f;
    const bool enableValidation =
        m_context->activeSettings().DebugView == DebugViewType::StablePlane_DenoiserValidation;

    commandList->beginMarker("NRD");
    if (nrdUseRelax)
    {
        m_nrd[planeIndex]->runDenoiserPasses(
            commandList,
            *m_renderTargets,
            planeIndex,
            *m_context->camera.view(),
            *m_context->camera.viewPrevious(),
            m_context->gpuDevice.getRenderPhaseFrameIndex(),
            m_context->activeSettings().NRDDisocclusionThreshold,
            m_context->activeSettings().NRDDisocclusionThresholdAlternate,
            m_context->activeSettings().NRDUseAlternateDisocclusionThresholdMix,
            timeDeltaBetweenFrames,
            enableValidation,
            resetHistory,
            &m_context->activeSettings().RelaxSettings);
    }
    else
    {
        m_nrd[planeIndex]->runDenoiserPasses(
            commandList,
            *m_renderTargets,
            planeIndex,
            *m_context->camera.view(),
            *m_context->camera.viewPrevious(),
            m_context->gpuDevice.getRenderPhaseFrameIndex(),
            m_context->activeSettings().NRDDisocclusionThreshold,
            m_context->activeSettings().NRDDisocclusionThresholdAlternate,
            m_context->activeSettings().NRDUseAlternateDisocclusionThresholdMix,
            timeDeltaBetweenFrames,
            enableValidation,
            resetHistory,
            &m_context->activeSettings().ReblurSettings);
    }
    commandList->endMarker();
}

void DenoisePass::mergeNrdOutputs(caustica::rhi::CommandList* commandList, int planeIndex)
{
    assert(commandList);
    assert(m_context);
    assert(m_renderTargets);
    assert(m_postProcess);
    assert(planeIndex >= 0 && planeIndex < static_cast<int>(std::size(m_nrd)));

    if (!m_context->activeSettings().actualUseStandaloneDenoiser())
        return;

    const bool nrdUseRelax = m_context->activeSettings().NRDMethod == NrdConfig::DenoiserMethod::RELAX;
    const PostProcess::ComputePassType mergePassType = nrdUseRelax
        ? PostProcess::ComputePassType::RELAXDenoiserFinalMerge
        : PostProcess::ComputePassType::REBLURDenoiserFinalMerge;

    FrameMiniConstants miniConstants = makeNrdPlaneMiniConstants(m_context->activeSettings(), planeIndex);

    commandList->beginMarker("MergeOutputs");
    m_postProcess->apply(
        commandList,
        mergePassType,
        planeIndex,
        m_constantBuffer,
        miniConstants,
        m_renderTargets->outputColor,
        *m_renderTargets,
        nullptr);
    commandList->endMarker();
}

void DenoisePass::denoiseStablePlane(
    caustica::rhi::CommandList* commandList,
    caustica::rhi::Framebuffer* framebuffer,
    int planeIndex)
{
    (void)framebuffer;

    assert(commandList);
    assert(m_context);

    if (!m_context->activeSettings().actualUseStandaloneDenoiser())
        return;

    const char* passNames[] = { "Denoising plane 0", "Denoising plane 1", "Denoising plane 2", "Denoising plane 3" };
    assert(planeIndex >= 0 && planeIndex < static_cast<int>(std::size(passNames)));

    commandList->beginMarker(passNames[planeIndex]);
    prepareNrdInputs(commandList, planeIndex);
    runNrd(commandList, planeIndex);
    mergeNrdOutputs(commandList, planeIndex);
    commandList->endMarker();
}

void DenoisePass::denoise(caustica::rhi::CommandList* commandList, caustica::rhi::Framebuffer* framebuffer)
{
    assert(m_context);

    if (!m_context->activeSettings().actualUseStandaloneDenoiser())
        return;

    ensureNrdIntegrations();

    const int maxPassCount = std::min(
        m_context->activeSettings().StablePlanesActiveCount,
        static_cast<int>(std::size(m_nrd)));
    for (int pass = maxPassCount - 1; pass >= 0; pass--)
        denoiseStablePlane(commandList, framebuffer, pass);
}

void DenoisePass::runNoDenoiserFinalMerge(caustica::rhi::CommandList* commandList)
{
    assert(commandList);
    assert(m_context);
    assert(m_renderTargets);
    assert(m_postProcess);

    if (!m_context->activeSettings().RealtimeMode
        || m_context->activeSettings().actualUseStandaloneDenoiser())
        return;

    if (m_context->activeSettings().RealtimeAA == 2
        || m_context->activeSettings().RealtimeAA == 3)
        return;

    FrameMiniConstants miniConstants = { uint4(0, 0, 0, 0) };
    caustica::rhi::TextureDesc tdesc = m_renderTargets->outputColor->getDesc();
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

#if CAUSTICA_WITH_NATIVE_DLSS
bool DenoisePass::evaluateNativeDLSS(caustica::rhi::CommandList* commandList, bool reset)
{
    assert(commandList);
    assert(m_context);
    assert(m_renderTargets);

    if (!m_nativeDLSS
        || !(m_context->activeSettings().RealtimeAA == 2
            || m_context->activeSettings().RealtimeAA == 3))
        return false;

    const bool useRayReconstruction = m_context->activeSettings().RealtimeAA == 3;
    if (useRayReconstruction && !m_nativeDLSS->isRayReconstructionSupported())
        return false;
    if (!useRayReconstruction && !m_nativeDLSS->isDlssSupported())
        return false;

    if (useRayReconstruction)
    {
        RAII_SCOPE(commandList->beginMarker("DLSSRR_PrepareInputs");, commandList->endMarker(););

        FrameMiniConstants miniConstants = { uint4(0, 0, 0, 0) };
        caustica::rhi::TextureDesc tdesc = m_renderTargets->outputColor->getDesc();
        m_postProcess->apply(
            commandList,
            PostProcess::ComputePassType::DLSSRRDenoiserPrepareInputs,
            m_constantBuffer,
            miniConstants,
            m_bindingSet,
            m_bindingLayout,
            tdesc.width,
            tdesc.height);
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
    evaluateParams.resetHistory = reset || m_context->activeSettings().ResetRealtimeCaches;

    if (useRayReconstruction)
    {
        evaluateParams.diffuseAlbedo = m_renderTargets->rrDiffuseAlbedo;
        evaluateParams.specularAlbedo = m_renderTargets->rrSpecAlbedo;
        evaluateParams.normalRoughness = m_renderTargets->rrNormalsAndRoughness;
    }

    const bool evaluated = m_nativeDLSS->evaluate(
        commandList, evaluateParams, *m_context->camera.view());
    if (evaluated)
    {
        static bool loggedNativeDLSSEvaluation = false;
        if (!loggedNativeDLSSEvaluation)
        {
            caustica::info("Native NGX %s evaluated successfully at %ux%u -> %ux%u.",
                useRayReconstruction ? "DLSS-RR" : "DLSS",
                m_renderSize.x, m_renderSize.y,
                m_displaySize.x, m_displaySize.y);
            loggedNativeDLSSEvaluation = true;
        }
    }

    return evaluated;
}
#endif

void DenoisePass::runDlssUpscale(caustica::rhi::CommandList* commandList, bool reset)
{
    assert(commandList);
    assert(m_context);
    assert(m_camera);

    if (!m_context->activeSettings().RealtimeMode)
        return;

    if (!(m_context->activeSettings().RealtimeAA == 2
        || m_context->activeSettings().RealtimeAA == 3))
        return;

    PostProcessAAParams params{
        m_context->activeSettings(),
        commandList,
        m_renderTargets,
        &m_context->gpuDevice,
    };
    params.renderSize = m_renderSize;
    params.displaySize = m_displaySize;
    params.displayAspectRatio = m_displayAspectRatio;
    params.cameraJitter = m_cameraJitter;
    params.sampleIndex = m_sampleIndex;
    params.frameIndex = static_cast<uint32_t>(m_frameIndex);
    params.reset = reset;
    params.temporalAAPass = m_temporalAntiAliasing;
    params.accumulationPass = m_accumulation;
    params.postProcess = m_postProcess;
    params.bindingSet = m_bindingSet;
    params.bindingLayout = m_bindingLayout;
    params.constantBuffer = m_constantBuffer;
    params.accumulationSampleIndex = m_accumulationSampleIndex;
    params.gaussianSplatTemporalSampleIndex = m_gaussianSplatTemporalSampleIndex;
    params.gaussianSplatTemporalReset = m_gaussianSplatTemporalReset;
#if CAUSTICA_WITH_STREAMLINE
    params.dlssRROptions = m_dlssRROptions;
#endif

    caustica::postProcessAA(*m_camera, params);

#if CAUSTICA_WITH_NATIVE_DLSS
    bool nativeDLSSEvaluated = evaluateNativeDLSS(commandList, reset);

    if (!nativeDLSSEvaluated)
    {
        // Never 1:1-copy render-res output into display-res processedOutputColor —
        // that leaves the unfilled region as uninitialized garbage (black/white bands).
        if (m_renderSize.x == m_displaySize.x && m_renderSize.y == m_displaySize.y)
        {
            if (m_context->activeSettings().actualUseStandaloneDenoiser())
            {
                commandList->copyTexture(
                    m_renderTargets->processedOutputColor, caustica::rhi::TextureSlice(),
                    m_renderTargets->outputColor, caustica::rhi::TextureSlice());
            }
            else
            {
                FrameMiniConstants miniConstants = { uint4(0, 0, 0, 0) };
                caustica::rhi::TextureDesc tdesc = m_renderTargets->outputColor->getDesc();
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
        else
        {
            caustica::warning(
                "Native DLSS fallback skipped: render size %ux%u != display size %ux%u",
                m_renderSize.x, m_renderSize.y, m_displaySize.x, m_displaySize.y);
        }
    }
#endif
}

void DenoisePass::invalidateNrdIntegrations()
{
    for (auto& nrd : m_nrd)
        nrd = nullptr;
}

void DenoisePass::invalidateOidnOutput()
{
    m_oidnDenoisedOutput = nullptr;
    resetReferenceOIDN();
}

void DenoisePass::resetReferenceOIDN()
{
    m_oidnDenoisedOutputValid = false;
    m_oidnDenoiserFailed = false;

    if (m_oidnDenoiser)
        m_oidnDenoiser->reset();
}

bool DenoisePass::applyReferenceOIDN(caustica::rhi::CommandList* commandList)
{
    assert(m_context);
    assert(commandList);

    if (m_context->activeSettings().RealtimeMode
        || !m_context->activeSettings().ReferenceOIDNDenoiser
        || m_renderTargets == nullptr)
        return false;

#if CAUSTICA_WITH_OIDN
    caustica::rhi::Device* device = m_device;
    assert(device);

    const bool accumulationReady = m_accumulationCompleted
        || m_accumulationSampleIndex >= m_context->activeSettings().AccumulationTarget;
    if (!accumulationReady)
        return false;

    if (m_oidnDenoiserFailed)
        return false;

    const caustica::rhi::TextureDesc processedDesc = m_renderTargets->processedOutputColor->getDesc();
    if (m_oidnDenoisedOutput == nullptr
        || m_oidnDenoisedOutput->getDesc().width != processedDesc.width
        || m_oidnDenoisedOutput->getDesc().height != processedDesc.height
        || m_oidnDenoisedOutput->getDesc().format != processedDesc.format)
    {
        caustica::rhi::TextureDesc oidnOutputDesc = processedDesc;
        oidnOutputDesc.debugName = "ReferenceOIDNDenoisedOutput";
        oidnOutputDesc.initialState = caustica::rhi::ResourceStates::CopySource;
        oidnOutputDesc.keepInitialState = true;
        m_oidnDenoisedOutput = device->createTexture(oidnOutputDesc);
        m_oidnDenoisedOutputValid = false;
    }

    if (m_oidnDenoisedOutputValid)
    {
        commandList->copyTexture(
            m_renderTargets->processedOutputColor, caustica::rhi::TextureSlice(),
            m_oidnDenoisedOutput, caustica::rhi::TextureSlice());
        return false;
    }

    caustica::rhi::Texture* sourceTexture = m_renderTargets->accumulatedRadiance;
    caustica::rhi::TextureDesc sourceDesc = sourceTexture->getDesc();
    if (sourceDesc.format != caustica::rhi::Format::RGBA32_FLOAT)
    {
        caustica::warning(
            "OIDN reference denoiser expected RGBA32_FLOAT accumulation buffer, got %s.",
            caustica::rhi::utils::FormatToString(sourceDesc.format));
        m_oidnDenoiserFailed = true;
        return false;
    }

    const uint32_t width = sourceDesc.width;
    const uint32_t height = sourceDesc.height;

    OidnDenoiser::Options oidnOptions;
    oidnOptions.UseGPU = m_context->activeSettings().ReferenceOIDNUseGPU;
    oidnOptions.GuidePasses = static_cast<OidnDenoiser::Passes>(
        std::clamp(m_context->activeSettings().ReferenceOIDNPasses, 0, 2));
    oidnOptions.GuidePrefilter = static_cast<OidnDenoiser::Prefilter>(
        std::clamp(m_context->activeSettings().ReferenceOIDNPrefilter, 0, 2));
    oidnOptions.FilterQuality = static_cast<OidnDenoiser::Quality>(
        std::clamp(m_context->activeSettings().ReferenceOIDNQuality, 0, 2));

    const bool requestAlbedoGuide = oidnOptions.GuidePasses == OidnDenoiser::Passes::Albedo
        || oidnOptions.GuidePasses == OidnDenoiser::Passes::AlbedoNormal;
    const bool requestNormalGuide = oidnOptions.GuidePasses == OidnDenoiser::Passes::AlbedoNormal;
    if (requestAlbedoGuide || requestNormalGuide)
    {
        FrameMiniConstants miniConstants = { uint4(0, 0, 0, 0) };
        caustica::rhi::TextureDesc tdesc = m_renderTargets->outputColor->getDesc();
        commandList->beginMarker("OIDN_PrepareGuides");
        m_postProcess->apply(
            commandList,
            PostProcess::ComputePassType::DLSSRRDenoiserPrepareInputs,
            m_constantBuffer,
            miniConstants,
            m_bindingSet,
            m_bindingLayout,
            tdesc.width,
            tdesc.height);
        commandList->endMarker();
    }

    caustica::rhi::StagingTextureHandle stagingTexture = device->createStagingTexture(
        makeReadbackTextureDesc(sourceDesc, "ReferenceOIDN AccumulatedRadiance Readback"),
        caustica::rhi::CpuAccessMode::Read);
    if (stagingTexture == nullptr)
    {
        caustica::warning("OIDN reference denoiser failed to create a staging texture.");
        m_oidnDenoiserFailed = true;
        return false;
    }

    caustica::rhi::StagingTextureHandle albedoStagingTexture;
    caustica::rhi::StagingTextureHandle normalStagingTexture;
    if (requestAlbedoGuide && m_renderTargets->rrDiffuseAlbedo != nullptr)
    {
        albedoStagingTexture = device->createStagingTexture(
            makeReadbackTextureDesc(
                m_renderTargets->rrDiffuseAlbedo->getDesc(),
                "ReferenceOIDN Albedo Readback"),
            caustica::rhi::CpuAccessMode::Read);
        if (albedoStagingTexture != nullptr)
            commandList->copyTexture(
                albedoStagingTexture, caustica::rhi::TextureSlice(),
                m_renderTargets->rrDiffuseAlbedo, caustica::rhi::TextureSlice());
    }
    if (requestNormalGuide && m_renderTargets->rrNormalsAndRoughness != nullptr)
    {
        normalStagingTexture = device->createStagingTexture(
            makeReadbackTextureDesc(
                m_renderTargets->rrNormalsAndRoughness->getDesc(),
                "ReferenceOIDN Normal Readback"),
            caustica::rhi::CpuAccessMode::Read);
        if (normalStagingTexture != nullptr)
            commandList->copyTexture(
                normalStagingTexture, caustica::rhi::TextureSlice(),
                m_renderTargets->rrNormalsAndRoughness, caustica::rhi::TextureSlice());
    }

    commandList->copyTexture(stagingTexture, caustica::rhi::TextureSlice(), sourceTexture, caustica::rhi::TextureSlice());
    commandList->close();
    device->executeCommandList(commandList);
    if (!device->waitForIdle())
    {
        commandList->open();
        caustica::warning("OIDN reference denoiser readback failed because the GPU device was lost.");
        m_oidnDenoiserFailed = true;
        return true;
    }

    size_t rowPitch = 0;
    const uint8_t* mappedData = static_cast<const uint8_t*>(device->mapStagingTexture(
        stagingTexture, caustica::rhi::TextureSlice(), caustica::rhi::CpuAccessMode::Read, &rowPitch));
    if (mappedData == nullptr)
    {
        commandList->open();
        caustica::warning("OIDN reference denoiser failed to map the accumulation buffer.");
        m_oidnDenoiserFailed = true;
        return true;
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

    device->unmapStagingTexture(stagingTexture);

    std::vector<float> albedoRgb;
    std::vector<float> normalRgb;
    if (albedoStagingTexture != nullptr)
    {
        readR11G11B10Float3Staging(device, albedoStagingTexture, width, height, albedoRgb);
        if (!albedoRgb.empty())
            oidnOptions.AlbedoRgb = albedoRgb.data();
    }
    if (normalStagingTexture != nullptr)
    {
        readRGBA16Float3Staging(device, normalStagingTexture, width, height, normalRgb);
        if (!normalRgb.empty())
            oidnOptions.NormalRgb = normalRgb.data();
    }

    if (m_oidnDenoiser == nullptr)
        m_oidnDenoiser = std::make_unique<OidnDenoiser>();

    std::vector<float> outputRgb;
    const bool success = m_oidnDenoiser->denoise(inputRgb.data(), width, height, oidnOptions, outputRgb);

    commandList->open();

    if (!success)
    {
        caustica::warning("OIDN reference denoiser failed: %s", m_oidnDenoiser->getLastError().c_str());
        m_oidnDenoiserFailed = true;
        return true;
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

    commandList->writeTexture(m_oidnDenoisedOutput, 0, 0, outputHalf.data(), size_t(width) * sizeof(float16_t4));
    commandList->copyTexture(
        m_renderTargets->processedOutputColor, caustica::rhi::TextureSlice(),
        m_oidnDenoisedOutput, caustica::rhi::TextureSlice());
    m_oidnDenoisedOutputValid = true;

    caustica::info(
        "OIDN reference denoiser completed on %s for %ux%u image.",
        m_oidnDenoiser->getDeviceDescription().c_str(), width, height);
    return true;
#else
    if (!m_oidnDenoiserFailed)
    {
        caustica::warning("OIDN reference denoiser requested, but CAUSTICA_WITH_OIDN is disabled in this build.");
        m_oidnDenoiserFailed = true;
    }
    return false;
#endif
}
