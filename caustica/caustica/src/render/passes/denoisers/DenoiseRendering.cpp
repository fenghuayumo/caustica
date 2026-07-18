#include <render/WorldRenderer.h>

#include <render/PathTracingContext.h>
#include <render/passes/postProcess/DenoisingGuidesPass.h>
#include <render/passes/postProcess/PostProcess.h>
#include <render/passes/denoisers/NrdIntegration.h>
#include <render/passes/denoisers/OidnDenoiser.h>
#include <render/core/PathTracerSettings.h>
#include <assets/loader/TextureLoader.h>
#include <backend/GpuDevice.h>
#include <core/log.h>
#include <core/path_utils.h>
#include <core/scope.h>
#include <core/system_utils.h>
#include <math/float.h>
#include <rhi/utils.h>
#include <shaders/SampleConstantBuffer.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

using namespace caustica;
using namespace caustica::math;
using namespace caustica::render;

void caustica::render::WorldRenderer::createDenoiserRenderPasses()
{
    m_denoisingGuidesPass = std::make_shared<DenoisingGuidesPass>(device(), m_context->shaderFactory, m_renderTargets, m_shaderDebug, m_bindingLayout);
}

void caustica::render::WorldRenderer::prepareDenoiserGuides(nvrhi::ICommandList* commandList)
{
    nvrhi::CommandListHandle savedCommandList = m_commandList;
    m_commandList = commandList;

    RAII_SCOPE(m_commandList->beginMarker("Denoising Guides Bake"); , m_commandList->endMarker(); );

    m_denoisingGuidesPass->denoiseSpecHitT(m_commandList, m_bindingSet);
    m_denoisingGuidesPass->computeAvgLayerRadiance(m_commandList, m_bindingSet);

    if (m_context->activeSettings().DebugView != DebugViewType::Disabled)
        m_denoisingGuidesPass->renderDebugViz(m_commandList, m_context->activeSettings().DebugView, m_bindingSet);

    m_commandList = savedCommandList;
}

void caustica::render::WorldRenderer::ensureNrdIntegrations()
{
    if (!m_context->activeSettings().actualUseStandaloneDenoiser())
        return;

    for (int i = 0; i < std::size(m_nrd); i++)
    {
        if (m_nrd[i] != nullptr)
            continue;

        nrd::Denoiser denoiserMethod = m_context->activeSettings().NRDMethod == NrdConfig::DenoiserMethod::REBLUR
            ? nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR
            : nrd::Denoiser::RELAX_DIFFUSE_SPECULAR;

        m_nrd[i] = std::make_unique<NrdIntegration>(device(), denoiserMethod);
        m_nrd[i]->initialize(m_renderSize.x, m_renderSize.y, *m_context->shaderFactory);
    }
}

void caustica::render::WorldRenderer::denoiseStablePlane(
    nvrhi::ICommandList* commandList,
    nvrhi::IFramebuffer* framebuffer,
    int planeIndex)
{
    (void)framebuffer;

    if (!m_context->activeSettings().actualUseStandaloneDenoiser())
        return;

    nvrhi::CommandListHandle savedCommandList = m_commandList;
    m_commandList = commandList;

    const char* passNames[] = { "Denoising plane 0", "Denoising plane 1", "Denoising plane 2", "Denoising plane 3" };
    assert(planeIndex >= 0 && planeIndex < static_cast<int>(std::size(m_nrd)));
    assert(planeIndex < static_cast<int>(std::size(passNames)));

    const bool nrdUseRelax = m_context->activeSettings().NRDMethod == NrdConfig::DenoiserMethod::RELAX;
    const PostProcess::ComputePassType preparePassType = nrdUseRelax
        ? PostProcess::ComputePassType::RELAXDenoiserPrepareInputs
        : PostProcess::ComputePassType::REBLURDenoiserPrepareInputs;
    const PostProcess::ComputePassType mergePassType = nrdUseRelax
        ? PostProcess::ComputePassType::RELAXDenoiserFinalMerge
        : PostProcess::ComputePassType::REBLURDenoiserFinalMerge;

    const bool resetHistory = m_context->activeSettings().ResetRealtimeCaches;
    const int maxPassCount = std::min(m_context->activeSettings().StablePlanesActiveCount, static_cast<int>(std::size(m_nrd)));
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

    const float timeDeltaBetweenFrames = m_context->gpuDevice.isHeadless() ? 1.f / 60.f : -1.f;
    const bool enableValidation = m_context->activeSettings().DebugView == DebugViewType::StablePlane_DenoiserValidation;
    if (nrdUseRelax)
    {
        m_nrd[planeIndex]->runDenoiserPasses(
            m_commandList,
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
            m_commandList,
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
    if (!m_context->activeSettings().actualUseStandaloneDenoiser())
        return;

    ensureNrdIntegrations();

    const int maxPassCount = std::min(m_context->activeSettings().StablePlanesActiveCount, static_cast<int>(std::size(m_nrd)));
    for (int pass = maxPassCount - 1; pass >= 0; pass--)
        denoiseStablePlane(commandList, framebuffer, pass);
}

void caustica::render::WorldRenderer::runNoDenoiserFinalMerge(nvrhi::ICommandList* commandList)
{
    if (!m_context->activeSettings().RealtimeMode || m_context->activeSettings().actualUseStandaloneDenoiser())
        return;

    if (m_context->activeSettings().RealtimeAA == 2 || m_context->activeSettings().RealtimeAA == 3)
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

void caustica::render::WorldRenderer::resetReferenceOIDN()
{
    m_oidnDenoisedOutputValid = false;
    m_oidnDenoiserFailed = false;

    if (m_oidnDenoiser)
        m_oidnDenoiser->reset();
}
void caustica::render::WorldRenderer::applyReferenceOIDN()
{
    if (m_context->activeSettings().RealtimeMode || !m_context->activeSettings().ReferenceOIDNDenoiser || m_renderTargets == nullptr)
        return;

#if CAUSTICA_WITH_OIDN
    const bool accumulationReady = m_accumulationCompleted || m_accumulationSampleIndex >= m_context->activeSettings().AccumulationTarget;
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
    oidnOptions.UseGPU = m_context->activeSettings().ReferenceOIDNUseGPU;
    oidnOptions.GuidePasses = static_cast<OidnDenoiser::Passes>(std::clamp(m_context->activeSettings().ReferenceOIDNPasses, 0, 2));
    oidnOptions.GuidePrefilter = static_cast<OidnDenoiser::Prefilter>(std::clamp(m_context->activeSettings().ReferenceOIDNPrefilter, 0, 2));
    oidnOptions.FilterQuality = static_cast<OidnDenoiser::Quality>(std::clamp(m_context->activeSettings().ReferenceOIDNQuality, 0, 2));

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

