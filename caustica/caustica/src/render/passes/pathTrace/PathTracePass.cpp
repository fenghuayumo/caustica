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
#include <shaders/FrameConstantBuffer.h>

#include <algorithm>
#include <cassert>
#include <cmath>

using namespace caustica::math;

namespace caustica::render
{

bool PathTracePass::createExportPipeline(
    caustica::rhi::Device* device,
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
    auto GetStfMagnificationMethod = [](StfMagnificationMethod method) { return static_cast<int>(method); };
    auto GetStfFilterMode = [](StfFilterMode mode) { return static_cast<int>(mode); };
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
    caustica::rhi::CommandList* commandList,
    caustica::rhi::BindingSetHandle bindingSet,
    caustica::rhi::DescriptorTable* descriptorTable,
    dm::uint2 viewSize,
    PTPipelineVariant* pipeline)
{
    assert(commandList);
    assert(pipeline);
    assert(bindingSet);
    assert(descriptorTable);

    if (!pipeline->hasPipeline() || !pipeline->getShaderTable())
        return;

    caustica::rhi::rt::State state;
    caustica::rhi::rt::DispatchRaysArguments args;
    args.width = viewSize.x;
    args.height = viewSize.y;

    FrameMiniConstants miniConstants = { uint4(0, 0, 0, 0) };

    RAII_SCOPE(commandList->beginMarker("PathTracePrePass");, commandList->endMarker(););

    state.shaderTable = pipeline->getShaderTable();
    state.bindings = { bindingSet, descriptorTable };
    commandList->setRayTracingState(state);
    commandList->setPushConstants(&miniConstants, sizeof(miniConstants));
    commandList->dispatchRays(args);
}

void PathTracePass::exportVBuffer(
    caustica::rhi::CommandList* commandList,
    caustica::rhi::BindingSetHandle bindingSet,
    caustica::rhi::DescriptorTable* descriptorTable,
    dm::uint2 viewSize,
    caustica::rhi::ComputePipeline* pipeline)
{
    assert(commandList);
    assert(pipeline);
    assert(bindingSet);
    assert(descriptorTable);

    FrameMiniConstants miniConstants = { uint4(0, 0, 0, 0) };

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
    caustica::rhi::CommandList* commandList,
    caustica::rhi::BindingSetHandle bindingSet,
    caustica::rhi::DescriptorTable* descriptorTable,
    dm::uint2 viewSize,
    PTPipelineVariant* pipeline,
    uint32_t samplesPerPixel)
{
    assert(commandList);
    assert(pipeline);
    assert(bindingSet);
    assert(descriptorTable);
    assert(samplesPerPixel > 0);

    if (!pipeline->hasPipeline() || !pipeline->getShaderTable())
        return;

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

        FrameMiniConstants miniConstants = { uint4(subSampleIndex, 0, 0, 0) };
        commandList->setPushConstants(&miniConstants, sizeof(miniConstants));

        commandList->dispatchRays(args);
    }
}

rg::PassHandle registerPathTracePrePass(FrameGraphContext ctx, rg::PassHandle after)
{
    assert(ctx.graph);
    assert(ctx.pathTrace);
    assert(ctx.renderTargets);
    assert(ctx.settings);
    assert(after.isValid());

    if (!ctx.hasScene || !ctx.settings->RealtimeMode)
        return after;

    const PathTraceGraphTargets handles = importPathTraceGraphTargets(*ctx.graph, *ctx.renderTargets);
    PathTracePass* const pathTrace = ctx.pathTrace;
    const caustica::rhi::BindingSetHandle bindingSet = ctx.bindingSet;
    caustica::rhi::DescriptorTable* const descriptorTable = ctx.descriptorTable;
    const dm::uint2 renderSize = ctx.renderSize;
    PTPipelineVariant* const pipeline = ctx.ptBuildStablePlanes;

    rg::PassOptions passOptions{};
    passOptions.after = after;

    return ctx.graph->addPass(
        "PathTracePrePass",
        [handles](rg::PassBuilder& setup) {
            declarePathTracePrePassAccess(setup, handles);
        },
        [pathTrace, bindingSet, descriptorTable, renderSize, pipeline](rg::RenderPassContext& passCtx) {
            if (!pipeline || !bindingSet || !descriptorTable)
                return;
            pathTrace->prePass(
                passCtx.commandList(),
                bindingSet,
                descriptorTable,
                renderSize,
                pipeline);
        },
        passOptions);
}

rg::PassHandle registerVBufferExportPass(FrameGraphContext ctx, rg::PassHandle after)
{
    assert(ctx.graph);
    assert(ctx.pathTrace);
    assert(ctx.renderTargets);
    assert(ctx.settings);
    assert(after.isValid());

    if (!ctx.hasScene || !ctx.settings->RealtimeMode)
        return after;

    const PathTraceGraphTargets handles = importPathTraceGraphTargets(*ctx.graph, *ctx.renderTargets);
    PathTracePass* const pathTrace = ctx.pathTrace;
    const caustica::rhi::BindingSetHandle bindingSet = ctx.bindingSet;
    caustica::rhi::DescriptorTable* const descriptorTable = ctx.descriptorTable;
    const dm::uint2 renderSize = ctx.renderSize;
    const caustica::rhi::ComputePipelineHandle exportVBufferPSO = ctx.exportVBufferPSO;

    rg::PassOptions passOptions{};
    passOptions.after = after;

    return ctx.graph->addPass(
        kVBufferExportPass,
        [handles](rg::PassBuilder& setup) {
            declareVBufferExportAccess(setup, handles);
        },
        [pathTrace, bindingSet, descriptorTable, renderSize, exportVBufferPSO](rg::RenderPassContext& passCtx) {
            pathTrace->exportVBuffer(
                passCtx.commandList(),
                bindingSet,
                descriptorTable,
                renderSize,
                exportVBufferPSO);
        },
        passOptions);
}

rg::PassHandle registerPathTraceLightingEndPass(FrameGraphContext ctx, rg::PassHandle after)
{
    assert(ctx.graph);
    assert(ctx.renderTargets);
    assert(ctx.settings);
    assert(ctx.bindingCache);
    assert(after.isValid());

    if (!ctx.hasScene || !needsPathTraceLightingEndPass(*ctx.settings))
        return after;

    const PathTraceLightingEndTargets handles = importPathTraceLightingEndTargets(
        *ctx.graph,
        *ctx.renderTargets,
        ctx.lightSampling,
        ctx.subInstanceDataBuffer);
    LightSamplingCache* const lightSampling = ctx.lightSampling;
    caustica::BindingCache* const bindingCache = ctx.bindingCache;
    const SceneGpuFrameHandles gpuHandles = ctx.gpuHandles;
    PathTracingContext* const pathTracingContext = ctx.pathTracingContext;
    const caustica::rhi::BufferHandle subInstanceDataBuffer = ctx.subInstanceDataBuffer;
    caustica::rhi::Texture* const depthBuffer = ctx.renderTargets->depth;
    caustica::rhi::Texture* const motionVectors = ctx.renderTargets->screenMotionVectors;

    rg::PassOptions passOptions{};
    passOptions.after = after;

    return ctx.graph->addPass(
        kPathTraceLightingEndPass,
        [handles](rg::PassBuilder& setup) {
            declarePathTraceLightingEndAccess(setup, handles);
        },
        [lightSampling, bindingCache, gpuHandles, pathTracingContext,
         subInstanceDataBuffer, depthBuffer, motionVectors](rg::RenderPassContext& passCtx) {
            // Resolve owning shared_ptrs from the live session, not from FrameGraphContext
            // (graph lambdas must not extend EnvMap/material lifetime past destroy()).
            UpdateLightingEndParams lightingEndParams{
                .commandList = passCtx.commandList(),
                .lightSampling = lightSampling,
                .bindingCache = bindingCache,
                .gpuHandles = gpuHandles,
                .materials = pathTracingContext
                    ? pathTracingContext->scenePasses.lighting.materials()
                    : nullptr,
                .opacityMaps = pathTracingContext
                    ? pathTracingContext->scenePasses.lighting.opacityMaps()
                    : nullptr,
                .subInstanceDataBuffer = subInstanceDataBuffer,
                .depthBuffer = depthBuffer,
                .motionVectors = motionVectors,
            };
            caustica::updateLightingEnd(lightingEndParams);
        },
        passOptions);
}

rg::PassHandle registerMainPathTracePass(FrameGraphContext ctx, rg::PassHandle after)
{
    assert(ctx.graph);
    assert(ctx.pathTrace);
    assert(ctx.renderTargets);
    assert(ctx.settings);
    assert(after.isValid());

    if (!ctx.hasScene)
        return after;

    const PathTraceGraphTargets handles = importPathTraceGraphTargets(*ctx.graph, *ctx.renderTargets);
    PathTracePass* const pathTrace = ctx.pathTrace;
    PTPipelineVariant* const pipeline = ctx.settings->RealtimeMode
        ? ctx.ptFillStablePlanes
        : ctx.ptReference;
    const caustica::rhi::BindingSetHandle bindingSet = ctx.bindingSet;
    caustica::rhi::DescriptorTable* const descriptorTable = ctx.descriptorTable;
    const dm::uint2 renderSize = ctx.renderSize;
    const uint32_t samplesPerPixel = ctx.settings->actualSamplesPerPixel();

    rg::PassOptions passOptions{};
    passOptions.after = after;

    return ctx.graph->addPass(
        kMainPathTracePass,
        [handles](rg::PassBuilder& setup) {
            declareMainPathTraceAccess(setup, handles);
        },
        [pathTrace, pipeline, bindingSet, descriptorTable, renderSize,
         samplesPerPixel](rg::RenderPassContext& passCtx) {
            if (!pipeline || !bindingSet || !descriptorTable)
                return;
            pathTrace->mainPass(
                passCtx.commandList(),
                bindingSet,
                descriptorTable,
                renderSize,
                pipeline,
                samplesPerPixel);
        },
        passOptions);
}

} // namespace caustica::render
