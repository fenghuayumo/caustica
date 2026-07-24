#include <render/passes/pathTrace/PathTracePass.h>

#include <render/FrameGraphPasses.h>
#include <render/FrameGraphContext.h>
#include <render/PathTracingContext.h>
#include <render/core/CameraController.h>
#include <render/core/LightingUpdate.h>
#include <render/core/PathTracerSettings.h>
#include <render/core/PathTracingShaderCompiler.h>
#include <render/core/RenderTargets.h>
#include <render/graph/GraphBuilder.h>
#include <render/passes/pathTrace/PathTraceGraphResources.h>
#include <render/passes/postProcess/ToneMappingPasses.h>
#include <render/pipeline/FrameGraphPassNames.h>
#include <assets/loader/ShaderFactory.h>
#include <core/scope.h>
#include <math/math.h>
#include <scene/View.h>
#include <shaders/PathTracer/Config.h>
#include <shaders/PathTracer/StablePlanes.hlsli>
#include <shaders/SampleConstantBuffer.h>

#include <algorithm>
#include <cassert>
#include <cmath>

using namespace caustica::math;

namespace caustica::render
{

bool PathTracePass::createExportPipeline(
    caustica::rhi::IDevice* device,
    caustica::ShaderFactory* shaderFactory,
    caustica::rhi::BindingLayoutHandle bindingLayout,
    caustica::rhi::BindingLayoutHandle bindlessLayout)
{
    assert(device);
    assert(shaderFactory);

    std::vector<caustica::ShaderMacro> shaderMacros;
    m_exportVBufferCS = shaderFactory->createShader(
        "caustica/shaders/render/processingPasses/ExportVisibilityBuffer.hlsl",
        "main",
        &shaderMacros,
        caustica::rhi::ShaderType::Compute);

    caustica::rhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.bindingLayouts = { bindingLayout, bindlessLayout };
    pipelineDesc.CS = m_exportVBufferCS;
    m_exportVBufferPSO = device->createComputePipeline(pipelineDesc);
    return m_exportVBufferPSO != nullptr;
}

void PathTracePass::fillConstants(
    PathTracerConstants& constants,
    const PathTracerCameraData& cameraData,
    const FillConstantsParams& params) const
{
    assert(params.context);

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
#endif

    const PathTracerSettings& settings = params.context->activeSettings();

    constants.camera = cameraData;
    constants.prevCamera = cameraData;
    if (params.context->camera.viewPrevious())
        constants.prevCamera.PosW = params.context->camera.viewPrevious()->getInverseViewMatrix().m_translation;

    constants.bounceCount = settings.BounceCount;
    constants.diffuseBounceCount = settings.DiffuseBounceCount;
    constants.perPixelJitterAAScale = (!settings.RealtimeMode && settings.AccumulationAA)
        ? 1.0f
        : ((settings.RealtimeMode && settings.RealtimeAA == 3) ? settings.DLSSRRMicroJitter : 0.0f);

    const float dlssBias = -dm::log2f(sqrtf(
        (params.displaySize.x * params.displaySize.y) / float(params.renderSize.x * params.renderSize.y)));

    constants.texLODBias = settings.TexLODBias + dlssBias;
    constants.sampleBaseIndex = params.sampleIndex * settings.actualSamplesPerPixel();
    constants.invSubSampleCount = 1.0f / float(settings.actualSamplesPerPixel());

    constants.imageWidth = params.renderSize.x;
    constants.imageHeight = params.renderSize.y;
    if (params.renderTargets != nullptr)
    {
        assert(params.renderSize.x == params.renderTargets->outputColor->getDesc().width);
        assert(params.renderSize.y == params.renderTargets->outputColor->getDesc().height);
    }

    if (settings.EnableToneMapping && params.toneMapping != nullptr)
        constants.preExposedGrayLuminance = dm::luminance(params.toneMapping->getPreExposedGray(0));
    else
        constants.preExposedGrayLuminance = 1.0f;

    constexpr float disabledFF = 0.0f;
    if (settings.RealtimeMode)
        constants.fireflyFilterThreshold = settings.RealtimeFireflyFilterEnabled
            ? (settings.RealtimeFireflyFilterThreshold * sqrtf(constants.preExposedGrayLuminance) * 1e3f)
            : disabledFF;
    else
        constants.fireflyFilterThreshold = settings.ReferenceFireflyFilterEnabled
            ? (settings.ReferenceFireflyFilterThreshold * sqrtf(constants.preExposedGrayLuminance) * 1e3f)
            : disabledFF;

    constants.useReSTIRDI = settings.actualUseReSTIRDI();
    constants.useReSTIRGI = settings.actualUseReSTIRGI();
    constants.useReSTIRPT = settings.actualUseReSTIRPT();
    constants.environmentMapVisibleToCamera = settings.EnvironmentMapParams.VisibleToCamera ? 1u : 0u;
    constants.denoiserRadianceClampK = settings.DenoiserRadianceClampK;
    constants.DLSSRRBrightnessClampK = (settings.DLSSRRBrightnessClampK > 0)
        ? (settings.DLSSRRBrightnessClampK * constants.preExposedGrayLuminance)
        : 0.0f;

    constants.denoisingEnabled = settings.actualUseStandaloneDenoiser() || settings.RealtimeAA == 3;

    constants._activeStablePlaneCount = settings.StablePlanesActiveCount;
    constants.maxStablePlaneVertexDepth = std::min(
        std::min((uint)settings.StablePlanesMaxVertexDepth, cStablePlaneMaxVertexIndex),
        (uint)settings.BounceCount);
    constants.allowPrimarySurfaceReplacement = settings.AllowPrimarySurfaceReplacement;
    constants.stablePlanesSplitStopThreshold = settings.StablePlanesSplitStopThreshold;
    constants._padding3 = 0;
    constants.stablePlanesSuppressPrimaryIndirectSpecularK = settings.StablePlanesSuppressPrimaryIndirectSpecular
        ? settings.StablePlanesSuppressPrimaryIndirectSpecularK
        : 0.0f;
    constants.stablePlanesAntiAliasingFallthrough = settings.StablePlanesAntiAliasingFallthrough;
    constants.frameIndex = params.frameIndex & 0xFFFFFFFFu;
    constants.genericTSLineStride = GenericTSComputeLineStride(constants.imageWidth, constants.imageHeight);
    constants.genericTSPlaneStride = GenericTSComputePlaneStride(constants.imageWidth, constants.imageHeight);

    constants.NEEEnabled = settings.UseNEE;
    constants.NEEType = settings.NEEType;
    constants.NEECandidateSamples = settings.NEECandidateSamples;
    constants.NEEFullSamples = settings.NEEFullSamples;
    constants.EnvironmentMapDiffuseSampleMIPLevel = settings.EnvironmentMapDiffuseSampleMIPLevel;

#if CAUSTICA_STOCHASTIC_TEXTURE_FILTERING_ENABLE
    constants.STFMagnificationMethod = GetStfMagnificationMethod(settings.STFMagnificationMethod);
    constants.STFFilterMode = GetStfFilterMode(settings.STFFilterMode);
    constants.STFGaussianSigma = settings.STFGaussianSigma;
#endif
}

void PathTracePass::prePass(
    caustica::rhi::ICommandList* commandList,
    caustica::rhi::BindingSetHandle bindingSet,
    caustica::rhi::IDescriptorTable* descriptorTable,
    dm::uint2 viewSize,
    PTPipelineVariant* pipeline)
{
    assert(commandList);
    assert(pipeline);
    assert(bindingSet);
    assert(descriptorTable);

    caustica::rhi::rt::State state;
    caustica::rhi::rt::DispatchRaysArguments args;
    args.width = viewSize.x;
    args.height = viewSize.y;

    SampleMiniConstants miniConstants = { uint4(0, 0, 0, 0) };

    RAII_SCOPE(commandList->beginMarker("PathTracePrePass");, commandList->endMarker(););

    state.shaderTable = pipeline->getShaderTable();
    state.bindings = { bindingSet, descriptorTable };
    commandList->setRayTracingState(state);
    commandList->setPushConstants(&miniConstants, sizeof(miniConstants));
    commandList->dispatchRays(args);
}

void PathTracePass::exportVBuffer(
    caustica::rhi::ICommandList* commandList,
    caustica::rhi::BindingSetHandle bindingSet,
    caustica::rhi::IDescriptorTable* descriptorTable,
    dm::uint2 viewSize,
    caustica::rhi::IComputePipeline* pipeline)
{
    assert(commandList);
    assert(pipeline);
    assert(bindingSet);
    assert(descriptorTable);

    SampleMiniConstants miniConstants = { uint4(0, 0, 0, 0) };

    RAII_SCOPE(commandList->beginMarker("VBufferExport");, commandList->endMarker(););

    caustica::rhi::ComputeState state;
    state.bindings = { bindingSet, descriptorTable };
    state.pipeline = pipeline;
    commandList->setComputeState(state);

    const dm::uint2 dispatchSize = {
        (viewSize.x + NUM_COMPUTE_THREADS_PER_DIM - 1) / NUM_COMPUTE_THREADS_PER_DIM,
        (viewSize.y + NUM_COMPUTE_THREADS_PER_DIM - 1) / NUM_COMPUTE_THREADS_PER_DIM };
    commandList->setPushConstants(&miniConstants, sizeof(miniConstants));
    commandList->dispatch(dispatchSize.x, dispatchSize.y);
}

void PathTracePass::mainPass(
    caustica::rhi::ICommandList* commandList,
    caustica::rhi::BindingSetHandle bindingSet,
    caustica::rhi::IDescriptorTable* descriptorTable,
    dm::uint2 viewSize,
    PTPipelineVariant* pipeline,
    uint32_t samplesPerPixel)
{
    assert(commandList);
    assert(pipeline);
    assert(bindingSet);
    assert(descriptorTable);
    assert(samplesPerPixel > 0);

    caustica::rhi::rt::State state;
    caustica::rhi::rt::DispatchRaysArguments args;
    args.width = viewSize.x;
    args.height = viewSize.y;

    RAII_SCOPE(commandList->beginMarker("PathTrace");, commandList->endMarker(););

    state.shaderTable = pipeline->getShaderTable();
    state.bindings = { bindingSet, descriptorTable };

    for (uint32_t subSampleIndex = 0; subSampleIndex < samplesPerPixel; subSampleIndex++)
    {
        commandList->setRayTracingState(state);

        SampleMiniConstants miniConstants = { uint4(subSampleIndex, 0, 0, 0) };
        commandList->setPushConstants(&miniConstants, sizeof(miniConstants));

        commandList->dispatchRays(args);
    }
}

void registerPathTracePrePass(FrameGraphContext ctx)
{
    assert(ctx.graph);
    assert(ctx.pathTrace);
    assert(ctx.renderTargets);
    assert(ctx.settings);

    if (!ctx.hasScene || !ctx.settings->RealtimeMode)
        return;

    const PathTraceGraphTargets handles = importPathTraceGraphTargets(*ctx.graph, *ctx.renderTargets);

    rg::PassOptions passOptions{};
    passOptions.executeAfter = ctx.settings->actualUseRTXDIPasses()
        ? rtxdiBeginReadyPass(ctx.settings->actualUseReSTIRDI())
        : kLightingReadyPass;

    ctx.graph->addPass(
        "PathTracePrePass",
        [handles](rg::PassBuilder& setup) {
            declarePathTracePrePassAccess(setup, handles);
        },
        [ctx](rg::RenderPassContext& passCtx) {
            ctx.pathTrace->prePass(
                passCtx.commandList(),
                ctx.bindingSet,
                ctx.descriptorTable,
                ctx.renderSize,
                ctx.ptBuildStablePlanes);
        },
        passOptions);
}

void registerVBufferExportPass(FrameGraphContext ctx)
{
    assert(ctx.graph);
    assert(ctx.pathTrace);
    assert(ctx.renderTargets);
    assert(ctx.settings);

    if (!ctx.hasScene || !ctx.settings->RealtimeMode)
        return;

    const PathTraceGraphTargets handles = importPathTraceGraphTargets(*ctx.graph, *ctx.renderTargets);

    rg::PassOptions passOptions{};
    passOptions.executeAfter = "PathTracePrePass";

    ctx.graph->addPass(
        "VBufferExport",
        [handles](rg::PassBuilder& setup) {
            declareVBufferExportAccess(setup, handles);
        },
        [ctx](rg::RenderPassContext& passCtx) {
            ctx.pathTrace->exportVBuffer(
                passCtx.commandList(),
                ctx.bindingSet,
                ctx.descriptorTable,
                ctx.renderSize,
                ctx.exportVBufferPSO);
        },
        passOptions);
}

void registerPathTraceLightingEndPass(FrameGraphContext ctx)
{
    assert(ctx.graph);
    assert(ctx.renderTargets);
    assert(ctx.settings);
    assert(ctx.bindingCache);

    if (!ctx.hasScene || !needsPathTraceLightingEndPass(*ctx.settings))
        return;

    const PathTraceLightingEndTargets handles = importPathTraceLightingEndTargets(
        *ctx.graph,
        *ctx.renderTargets,
        ctx.lightSampling,
        ctx.subInstanceDataBuffer);

    rg::PassOptions passOptions{};
    passOptions.executeAfter = pathTraceLightingEndExecuteAfterPass(*ctx.settings);

    ctx.graph->addPass(
        kPathTraceLightingEndPass,
        [handles](rg::PassBuilder& setup) {
            declarePathTraceLightingEndAccess(setup, handles);
        },
        [ctx](rg::RenderPassContext& passCtx) {
            // Resolve owning shared_ptrs from the live session, not from FrameGraphContext
            // (graph lambdas must not extend EnvMap/material lifetime past destroy()).
            UpdateLightingEndParams lightingEndParams{
                .commandList = passCtx.commandList(),
                .lightSampling = ctx.lightSampling,
                .bindingCache = ctx.bindingCache,
                .gpuHandles = ctx.gpuHandles,
                .materials = ctx.pathTracingContext
                    ? ctx.pathTracingContext->scenePasses.lighting.materials()
                    : nullptr,
                .opacityMaps = ctx.pathTracingContext
                    ? ctx.pathTracingContext->scenePasses.lighting.opacityMaps()
                    : nullptr,
                .subInstanceDataBuffer = ctx.subInstanceDataBuffer,
                .depthBuffer = ctx.renderTargets->depth,
                .motionVectors = ctx.renderTargets->screenMotionVectors,
            };
            caustica::updateLightingEnd(lightingEndParams);
        },
        passOptions);
}

void registerMainPathTracePass(FrameGraphContext ctx)
{
    assert(ctx.graph);
    assert(ctx.pathTrace);
    assert(ctx.renderTargets);
    assert(ctx.settings);

    if (!ctx.hasScene)
        return;

    const PathTraceGraphTargets handles = importPathTraceGraphTargets(*ctx.graph, *ctx.renderTargets);

    rg::PassOptions passOptions{};
    passOptions.executeAfter = pathTraceMainExecuteAfterPass(*ctx.settings);

    ctx.graph->addPass(
        "MainPathTrace",
        [handles](rg::PassBuilder& setup) {
            declareMainPathTraceAccess(setup, handles);
        },
        [ctx](rg::RenderPassContext& passCtx) {
            PTPipelineVariant* pipeline = ctx.settings->RealtimeMode
                ? ctx.ptFillStablePlanes
                : ctx.ptReference;
            ctx.pathTrace->mainPass(
                passCtx.commandList(),
                ctx.bindingSet,
                ctx.descriptorTable,
                ctx.renderSize,
                pipeline,
                ctx.settings->actualSamplesPerPixel());
        },
        passOptions);
}

void registerPathTraceGraphPasses(FrameGraphContext ctx)
{
    registerPathTracePrePass(ctx);
    registerVBufferExportPass(ctx);
    registerPathTraceLightingEndPass(ctx);
    registerMainPathTracePass(ctx);
}

} // namespace caustica::render
