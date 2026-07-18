#include <render/passes/denoisers/DenoisePass.h>

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
#include <shaders/SampleConstantBuffer.h>

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

    nvrhi::TextureDesc makeReadbackTextureDesc(nvrhi::TextureDesc desc, const char* debugName)
    {
        desc.debugName = debugName;
        desc.isRenderTarget = false;
        desc.isUAV = false;
        desc.isTypeless = false;
        desc.initialState = nvrhi::ResourceStates::CopyDest;
        desc.keepInitialState = true;
        return desc;
    }

    void readR11G11B10Float3Staging(
        nvrhi::IDevice* device,
        nvrhi::IStagingTexture* stagingTexture,
        uint32_t width,
        uint32_t height,
        std::vector<float>& output)
    {
        size_t rowPitch = 0;
        const uint8_t* mappedData = static_cast<const uint8_t*>(device->mapStagingTexture(
            stagingTexture, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch));
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
        nvrhi::IDevice* device,
        nvrhi::IStagingTexture* stagingTexture,
        uint32_t width,
        uint32_t height,
        std::vector<float>& output)
    {
        size_t rowPitch = 0;
        const uint8_t* mappedData = static_cast<const uint8_t*>(device->mapStagingTexture(
            stagingTexture, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch));
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
    nvrhi::IDevice* device,
    const std::shared_ptr<caustica::ShaderFactory>& shaderFactory,
    const std::unique_ptr<RenderTargets>& renderTargets,
    const std::shared_ptr<ShaderDebug>& shaderDebug,
    nvrhi::BindingLayoutHandle bindingLayout)
{
    m_denoisingGuidesPass = std::make_shared<DenoisingGuidesPass>(
        device, shaderFactory, renderTargets, shaderDebug, bindingLayout);
}

void DenoisePass::bindFrame(const FrameBindings& bindings)
{
    m_bindings = bindings;
}

void DenoisePass::prepareGuides(nvrhi::ICommandList* commandList)
{
    assert(commandList);
    assert(m_denoisingGuidesPass);
    assert(m_bindings.context);
    assert(m_bindings.bindingSet);

    RAII_SCOPE(commandList->beginMarker("Denoising Guides Bake"); , commandList->endMarker(); );

    m_denoisingGuidesPass->denoiseSpecHitT(commandList, m_bindings.bindingSet);
    m_denoisingGuidesPass->computeAvgLayerRadiance(commandList, m_bindings.bindingSet);

    if (m_bindings.context->activeSettings().DebugView != DebugViewType::Disabled)
        m_denoisingGuidesPass->renderDebugViz(
            commandList,
            m_bindings.context->activeSettings().DebugView,
            m_bindings.bindingSet);
}

void DenoisePass::stablePlanesDebugViz(nvrhi::ICommandList* commandList)
{
    assert(commandList);
    assert(m_bindings.postProcess);
    assert(m_bindings.renderTargets);

    SampleMiniConstants miniConstants = { uint4(0, 0, 0, 0) };

    commandList->beginMarker("StablePlanesDebugViz");
    nvrhi::TextureDesc tdesc = m_bindings.renderTargets->outputColor->getDesc();
    m_bindings.postProcess->apply(
        commandList,
        PostProcess::ComputePassType::StablePlanesDebugViz,
        m_bindings.constantBuffer,
        miniConstants,
        m_bindings.bindingSet,
        m_bindings.bindingLayout,
        tdesc.width,
        tdesc.height);
    commandList->endMarker();
}

void DenoisePass::ensureNrdIntegrations()
{
    assert(m_bindings.context);
    assert(m_bindings.device);

    if (!m_bindings.context->activeSettings().actualUseStandaloneDenoiser())
        return;

    for (int i = 0; i < std::size(m_nrd); i++)
    {
        if (m_nrd[i] != nullptr)
            continue;

        nrd::Denoiser denoiserMethod = m_bindings.context->activeSettings().NRDMethod == NrdConfig::DenoiserMethod::REBLUR
            ? nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR
            : nrd::Denoiser::RELAX_DIFFUSE_SPECULAR;

        m_nrd[i] = std::make_unique<NrdIntegration>(m_bindings.device, denoiserMethod);
        m_nrd[i]->initialize(m_bindings.renderSize.x, m_bindings.renderSize.y, *m_bindings.context->shaderFactory);
    }
}

void DenoisePass::denoiseStablePlane(
    nvrhi::ICommandList* commandList,
    nvrhi::IFramebuffer* framebuffer,
    int planeIndex)
{
    (void)framebuffer;

    assert(commandList);
    assert(m_bindings.context);
    assert(m_bindings.renderTargets);
    assert(m_bindings.postProcess);

    if (!m_bindings.context->activeSettings().actualUseStandaloneDenoiser())
        return;

    const char* passNames[] = { "Denoising plane 0", "Denoising plane 1", "Denoising plane 2", "Denoising plane 3" };
    assert(planeIndex >= 0 && planeIndex < static_cast<int>(std::size(m_nrd)));
    assert(planeIndex < static_cast<int>(std::size(passNames)));

    const bool nrdUseRelax = m_bindings.context->activeSettings().NRDMethod == NrdConfig::DenoiserMethod::RELAX;
    const PostProcess::ComputePassType preparePassType = nrdUseRelax
        ? PostProcess::ComputePassType::RELAXDenoiserPrepareInputs
        : PostProcess::ComputePassType::REBLURDenoiserPrepareInputs;
    const PostProcess::ComputePassType mergePassType = nrdUseRelax
        ? PostProcess::ComputePassType::RELAXDenoiserFinalMerge
        : PostProcess::ComputePassType::REBLURDenoiserFinalMerge;

    const bool resetHistory = m_bindings.context->activeSettings().ResetRealtimeCaches;
    const int maxPassCount = std::min(
        m_bindings.context->activeSettings().StablePlanesActiveCount,
        static_cast<int>(std::size(m_nrd)));
    const bool initWithStableRadiance = planeIndex == (maxPassCount - 1);

    commandList->beginMarker(passNames[planeIndex]);

    SampleMiniConstants miniConstants = { uint4(static_cast<uint>(planeIndex), initWithStableRadiance ? 1u : 0u, 0, 0) };

    nvrhi::TextureDesc tdesc = m_bindings.renderTargets->outputColor->getDesc();
    commandList->beginMarker("PrepareInputs");
    m_bindings.postProcess->apply(
        commandList,
        preparePassType,
        m_bindings.constantBuffer,
        miniConstants,
        m_bindings.bindingSet,
        m_bindings.bindingLayout,
        tdesc.width,
        tdesc.height);
    commandList->endMarker();

    const float timeDeltaBetweenFrames = m_bindings.context->gpuDevice.isHeadless() ? 1.f / 60.f : -1.f;
    const bool enableValidation =
        m_bindings.context->activeSettings().DebugView == DebugViewType::StablePlane_DenoiserValidation;
    if (nrdUseRelax)
    {
        m_nrd[planeIndex]->runDenoiserPasses(
            commandList,
            *m_bindings.renderTargets,
            planeIndex,
            *m_bindings.context->camera.view(),
            *m_bindings.context->camera.viewPrevious(),
            m_bindings.context->gpuDevice.getRenderPhaseFrameIndex(),
            m_bindings.context->activeSettings().NRDDisocclusionThreshold,
            m_bindings.context->activeSettings().NRDDisocclusionThresholdAlternate,
            m_bindings.context->activeSettings().NRDUseAlternateDisocclusionThresholdMix,
            timeDeltaBetweenFrames,
            enableValidation,
            resetHistory,
            &m_bindings.context->activeSettings().RelaxSettings);
    }
    else
    {
        m_nrd[planeIndex]->runDenoiserPasses(
            commandList,
            *m_bindings.renderTargets,
            planeIndex,
            *m_bindings.context->camera.view(),
            *m_bindings.context->camera.viewPrevious(),
            m_bindings.context->gpuDevice.getRenderPhaseFrameIndex(),
            m_bindings.context->activeSettings().NRDDisocclusionThreshold,
            m_bindings.context->activeSettings().NRDDisocclusionThresholdAlternate,
            m_bindings.context->activeSettings().NRDUseAlternateDisocclusionThresholdMix,
            timeDeltaBetweenFrames,
            enableValidation,
            resetHistory,
            &m_bindings.context->activeSettings().ReblurSettings);
    }

    commandList->beginMarker("MergeOutputs");
    m_bindings.postProcess->apply(
        commandList,
        mergePassType,
        planeIndex,
        m_bindings.constantBuffer,
        miniConstants,
        m_bindings.renderTargets->outputColor,
        *m_bindings.renderTargets,
        nullptr);
    commandList->endMarker();

    commandList->endMarker();
}

void DenoisePass::denoise(nvrhi::ICommandList* commandList, nvrhi::IFramebuffer* framebuffer)
{
    assert(m_bindings.context);

    if (!m_bindings.context->activeSettings().actualUseStandaloneDenoiser())
        return;

    ensureNrdIntegrations();

    const int maxPassCount = std::min(
        m_bindings.context->activeSettings().StablePlanesActiveCount,
        static_cast<int>(std::size(m_nrd)));
    for (int pass = maxPassCount - 1; pass >= 0; pass--)
        denoiseStablePlane(commandList, framebuffer, pass);
}

void DenoisePass::runNoDenoiserFinalMerge(nvrhi::ICommandList* commandList)
{
    assert(commandList);
    assert(m_bindings.context);
    assert(m_bindings.renderTargets);
    assert(m_bindings.postProcess);

    if (!m_bindings.context->activeSettings().RealtimeMode
        || m_bindings.context->activeSettings().actualUseStandaloneDenoiser())
        return;

    if (m_bindings.context->activeSettings().RealtimeAA == 2
        || m_bindings.context->activeSettings().RealtimeAA == 3)
        return;

    SampleMiniConstants miniConstants = { uint4(0, 0, 0, 0) };
    nvrhi::TextureDesc tdesc = m_bindings.renderTargets->outputColor->getDesc();
    commandList->beginMarker("NoDenoiserFinalMerge");
    m_bindings.postProcess->apply(
        commandList,
        PostProcess::ComputePassType::NoDenoiserFinalMerge,
        m_bindings.constantBuffer,
        miniConstants,
        m_bindings.bindingSet,
        m_bindings.bindingLayout,
        tdesc.width,
        tdesc.height);
    commandList->endMarker();
}

#if CAUSTICA_WITH_NATIVE_DLSS
bool DenoisePass::evaluateNativeDLSS(nvrhi::ICommandList* commandList, bool reset)
{
    assert(commandList);
    assert(m_bindings.context);
    assert(m_bindings.renderTargets);

    if (!m_bindings.nativeDLSS
        || !(m_bindings.context->activeSettings().RealtimeAA == 2
            || m_bindings.context->activeSettings().RealtimeAA == 3))
        return false;

    const bool useRayReconstruction = m_bindings.context->activeSettings().RealtimeAA == 3;
    if (useRayReconstruction && !m_bindings.nativeDLSS->isRayReconstructionSupported())
        return false;
    if (!useRayReconstruction && !m_bindings.nativeDLSS->isDlssSupported())
        return false;

    if (useRayReconstruction)
    {
        RAII_SCOPE(commandList->beginMarker("DLSSRR_PrepareInputs");, commandList->endMarker(););

        SampleMiniConstants miniConstants = { uint4(0, 0, 0, 0) };
        nvrhi::TextureDesc tdesc = m_bindings.renderTargets->outputColor->getDesc();
        m_bindings.postProcess->apply(
            commandList,
            PostProcess::ComputePassType::DLSSRRDenoiserPrepareInputs,
            m_bindings.constantBuffer,
            miniConstants,
            m_bindings.bindingSet,
            m_bindings.bindingLayout,
            tdesc.width,
            tdesc.height);
    }

    caustica::render::DLSS::InitParameters initParams;
    initParams.inputWidth = m_bindings.renderSize.x;
    initParams.inputHeight = m_bindings.renderSize.y;
    initParams.outputWidth = m_bindings.displaySize.x;
    initParams.outputHeight = m_bindings.displaySize.y;
    initParams.useLinearDepth = false;
    initParams.useAutoExposure = true;
    initParams.useRayReconstruction = useRayReconstruction;

    m_bindings.nativeDLSS->init(initParams);

    const bool initialized = useRayReconstruction
        ? m_bindings.nativeDLSS->isRayReconstructionInitialized()
        : m_bindings.nativeDLSS->isDlssInitialized();
    if (!initialized)
        return false;

    caustica::render::DLSS::EvaluateParameters evaluateParams;
    evaluateParams.inputColorTexture = m_bindings.renderTargets->outputColor;
    evaluateParams.outputColorTexture = m_bindings.renderTargets->processedOutputColor;
    evaluateParams.depthTexture = m_bindings.renderTargets->depth;
    evaluateParams.motionVectorsTexture = m_bindings.renderTargets->screenMotionVectors;
    evaluateParams.motionVectorScaleX = 1.0f / float(m_bindings.renderSize.x);
    evaluateParams.motionVectorScaleY = 1.0f / float(m_bindings.renderSize.y);
    evaluateParams.resetHistory = reset || m_bindings.context->activeSettings().ResetRealtimeCaches;

    if (useRayReconstruction)
    {
        evaluateParams.diffuseAlbedo = m_bindings.renderTargets->rrDiffuseAlbedo;
        evaluateParams.specularAlbedo = m_bindings.renderTargets->rrSpecAlbedo;
        evaluateParams.normalRoughness = m_bindings.renderTargets->rrNormalsAndRoughness;
    }

    const bool evaluated = m_bindings.nativeDLSS->evaluate(
        commandList, evaluateParams, *m_bindings.context->camera.view());
    if (evaluated)
    {
        static bool loggedNativeDLSSEvaluation = false;
        if (!loggedNativeDLSSEvaluation)
        {
            caustica::info("Native NGX %s evaluated successfully at %ux%u -> %ux%u.",
                useRayReconstruction ? "DLSS-RR" : "DLSS",
                m_bindings.renderSize.x, m_bindings.renderSize.y,
                m_bindings.displaySize.x, m_bindings.displaySize.y);
            loggedNativeDLSSEvaluation = true;
        }
    }

    return evaluated;
}
#endif

void DenoisePass::runDlssUpscale(nvrhi::ICommandList* commandList, bool reset)
{
    assert(commandList);
    assert(m_bindings.context);
    assert(m_bindings.camera);

    if (!m_bindings.context->activeSettings().RealtimeMode)
        return;

    if (!(m_bindings.context->activeSettings().RealtimeAA == 2
        || m_bindings.context->activeSettings().RealtimeAA == 3))
        return;

    PostProcessAAParams params{
        m_bindings.context->activeSettings(),
        commandList,
        m_bindings.renderTargets,
        &m_bindings.context->gpuDevice,
    };
    params.renderSize = m_bindings.renderSize;
    params.displaySize = m_bindings.displaySize;
    params.displayAspectRatio = m_bindings.displayAspectRatio;
    params.cameraJitter = m_bindings.cameraJitter;
    params.sampleIndex = m_bindings.sampleIndex;
    params.frameIndex = static_cast<uint32_t>(m_bindings.frameIndex);
    params.reset = reset;
    params.temporalAAPass = m_bindings.temporalAntiAliasing;
    params.accumulationPass = m_bindings.accumulation;
    params.postProcess = m_bindings.postProcess;
    params.bindingSet = m_bindings.bindingSet;
    params.bindingLayout = m_bindings.bindingLayout;
    params.constantBuffer = m_bindings.constantBuffer;
    params.accumulationSampleIndex = m_bindings.accumulationSampleIndex;
    params.gaussianSplatTemporalSampleIndex = m_bindings.gaussianSplatTemporalSampleIndex;
    params.gaussianSplatTemporalReset = m_bindings.gaussianSplatTemporalReset;
#if CAUSTICA_WITH_STREAMLINE
    params.dlssRROptions = m_bindings.dlssRROptions;
#endif

    caustica::postProcessAA(*m_bindings.camera, params);

#if CAUSTICA_WITH_NATIVE_DLSS
    bool nativeDLSSEvaluated = evaluateNativeDLSS(commandList, reset);

    if (!nativeDLSSEvaluated)
    {
        if (m_bindings.context->activeSettings().actualUseStandaloneDenoiser())
        {
            commandList->copyTexture(
                m_bindings.renderTargets->processedOutputColor, nvrhi::TextureSlice(),
                m_bindings.renderTargets->outputColor, nvrhi::TextureSlice());
        }
        else
        {
            SampleMiniConstants miniConstants = { uint4(0, 0, 0, 0) };
            nvrhi::TextureDesc tdesc = m_bindings.renderTargets->outputColor->getDesc();
            commandList->beginMarker("NoDenoiserFinalMerge");
            m_bindings.postProcess->apply(
                commandList,
                PostProcess::ComputePassType::NoDenoiserFinalMerge,
                m_bindings.constantBuffer,
                miniConstants,
                m_bindings.bindingSet,
                m_bindings.bindingLayout,
                tdesc.width,
                tdesc.height);
            commandList->endMarker();
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

void DenoisePass::applyReferenceOIDN()
{
    assert(m_bindings.context);

    if (m_bindings.context->activeSettings().RealtimeMode
        || !m_bindings.context->activeSettings().ReferenceOIDNDenoiser
        || m_bindings.renderTargets == nullptr)
        return;

#if CAUSTICA_WITH_OIDN
    nvrhi::ICommandList* commandList = m_bindings.commandList;
    nvrhi::IDevice* device = m_bindings.device;
    assert(commandList);
    assert(device);

    const bool accumulationReady = m_bindings.accumulationCompleted
        || m_bindings.accumulationSampleIndex >= m_bindings.context->activeSettings().AccumulationTarget;
    if (!accumulationReady)
        return;

    if (m_oidnDenoiserFailed)
        return;

    const nvrhi::TextureDesc processedDesc = m_bindings.renderTargets->processedOutputColor->getDesc();
    if (m_oidnDenoisedOutput == nullptr
        || m_oidnDenoisedOutput->getDesc().width != processedDesc.width
        || m_oidnDenoisedOutput->getDesc().height != processedDesc.height
        || m_oidnDenoisedOutput->getDesc().format != processedDesc.format)
    {
        nvrhi::TextureDesc oidnOutputDesc = processedDesc;
        oidnOutputDesc.debugName = "ReferenceOIDNDenoisedOutput";
        oidnOutputDesc.initialState = nvrhi::ResourceStates::CopySource;
        oidnOutputDesc.keepInitialState = true;
        m_oidnDenoisedOutput = device->createTexture(oidnOutputDesc);
        m_oidnDenoisedOutputValid = false;
    }

    if (m_oidnDenoisedOutputValid)
    {
        commandList->copyTexture(
            m_bindings.renderTargets->processedOutputColor, nvrhi::TextureSlice(),
            m_oidnDenoisedOutput, nvrhi::TextureSlice());
        return;
    }

    nvrhi::ITexture* sourceTexture = m_bindings.renderTargets->accumulatedRadiance;
    nvrhi::TextureDesc sourceDesc = sourceTexture->getDesc();
    if (sourceDesc.format != nvrhi::Format::RGBA32_FLOAT)
    {
        caustica::warning(
            "OIDN reference denoiser expected RGBA32_FLOAT accumulation buffer, got %s.",
            nvrhi::utils::FormatToString(sourceDesc.format));
        m_oidnDenoiserFailed = true;
        return;
    }

    const uint32_t width = sourceDesc.width;
    const uint32_t height = sourceDesc.height;

    OidnDenoiser::Options oidnOptions;
    oidnOptions.UseGPU = m_bindings.context->activeSettings().ReferenceOIDNUseGPU;
    oidnOptions.GuidePasses = static_cast<OidnDenoiser::Passes>(
        std::clamp(m_bindings.context->activeSettings().ReferenceOIDNPasses, 0, 2));
    oidnOptions.GuidePrefilter = static_cast<OidnDenoiser::Prefilter>(
        std::clamp(m_bindings.context->activeSettings().ReferenceOIDNPrefilter, 0, 2));
    oidnOptions.FilterQuality = static_cast<OidnDenoiser::Quality>(
        std::clamp(m_bindings.context->activeSettings().ReferenceOIDNQuality, 0, 2));

    const bool requestAlbedoGuide = oidnOptions.GuidePasses == OidnDenoiser::Passes::Albedo
        || oidnOptions.GuidePasses == OidnDenoiser::Passes::AlbedoNormal;
    const bool requestNormalGuide = oidnOptions.GuidePasses == OidnDenoiser::Passes::AlbedoNormal;
    if (requestAlbedoGuide || requestNormalGuide)
    {
        SampleMiniConstants miniConstants = { uint4(0, 0, 0, 0) };
        nvrhi::TextureDesc tdesc = m_bindings.renderTargets->outputColor->getDesc();
        commandList->beginMarker("OIDN_PrepareGuides");
        m_bindings.postProcess->apply(
            commandList,
            PostProcess::ComputePassType::DLSSRRDenoiserPrepareInputs,
            m_bindings.constantBuffer,
            miniConstants,
            m_bindings.bindingSet,
            m_bindings.bindingLayout,
            tdesc.width,
            tdesc.height);
        commandList->endMarker();
    }

    nvrhi::StagingTextureHandle stagingTexture = device->createStagingTexture(
        makeReadbackTextureDesc(sourceDesc, "ReferenceOIDN AccumulatedRadiance Readback"),
        nvrhi::CpuAccessMode::Read);
    if (stagingTexture == nullptr)
    {
        caustica::warning("OIDN reference denoiser failed to create a staging texture.");
        m_oidnDenoiserFailed = true;
        return;
    }

    nvrhi::StagingTextureHandle albedoStagingTexture;
    nvrhi::StagingTextureHandle normalStagingTexture;
    if (requestAlbedoGuide && m_bindings.renderTargets->rrDiffuseAlbedo != nullptr)
    {
        albedoStagingTexture = device->createStagingTexture(
            makeReadbackTextureDesc(
                m_bindings.renderTargets->rrDiffuseAlbedo->getDesc(),
                "ReferenceOIDN Albedo Readback"),
            nvrhi::CpuAccessMode::Read);
        if (albedoStagingTexture != nullptr)
            commandList->copyTexture(
                albedoStagingTexture, nvrhi::TextureSlice(),
                m_bindings.renderTargets->rrDiffuseAlbedo, nvrhi::TextureSlice());
    }
    if (requestNormalGuide && m_bindings.renderTargets->rrNormalsAndRoughness != nullptr)
    {
        normalStagingTexture = device->createStagingTexture(
            makeReadbackTextureDesc(
                m_bindings.renderTargets->rrNormalsAndRoughness->getDesc(),
                "ReferenceOIDN Normal Readback"),
            nvrhi::CpuAccessMode::Read);
        if (normalStagingTexture != nullptr)
            commandList->copyTexture(
                normalStagingTexture, nvrhi::TextureSlice(),
                m_bindings.renderTargets->rrNormalsAndRoughness, nvrhi::TextureSlice());
    }

    commandList->copyTexture(stagingTexture, nvrhi::TextureSlice(), sourceTexture, nvrhi::TextureSlice());
    commandList->close();
    device->executeCommandList(commandList);
    if (!device->waitForIdle())
    {
        commandList->open();
        caustica::warning("OIDN reference denoiser readback failed because the GPU device was lost.");
        m_oidnDenoiserFailed = true;
        return;
    }

    size_t rowPitch = 0;
    const uint8_t* mappedData = static_cast<const uint8_t*>(device->mapStagingTexture(
        stagingTexture, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch));
    if (mappedData == nullptr)
    {
        commandList->open();
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

    commandList->writeTexture(m_oidnDenoisedOutput, 0, 0, outputHalf.data(), size_t(width) * sizeof(float16_t4));
    commandList->copyTexture(
        m_bindings.renderTargets->processedOutputColor, nvrhi::TextureSlice(),
        m_oidnDenoisedOutput, nvrhi::TextureSlice());
    m_oidnDenoisedOutputValid = true;

    caustica::info(
        "OIDN reference denoiser completed on %s for %ux%u image.",
        m_oidnDenoiser->getDeviceDescription().c_str(), width, height);
#else
    if (!m_oidnDenoiserFailed)
    {
        caustica::warning("OIDN reference denoiser requested, but CAUSTICA_WITH_OIDN is disabled in this build.");
        m_oidnDenoiserFailed = true;
    }
#endif
}
