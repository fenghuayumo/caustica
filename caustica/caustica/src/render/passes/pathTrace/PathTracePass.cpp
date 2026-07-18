#include <render/passes/pathTrace/PathTracePass.h>

#include <render/FrameGraphPasses.h>
#include <render/FrameGraphContext.h>
#include <render/core/LightingUpdate.h>
#include <render/core/PathTracerSettings.h>
#include <render/core/PathTracingShaderCompiler.h>
#include <render/core/RenderTargets.h>
#include <render/graph/GraphBuilder.h>
#include <render/passes/pathTrace/PathTraceGraphResources.h>
#include <core/scope.h>
#include <shaders/PathTracer/Config.h>
#include <shaders/SampleConstantBuffer.h>

#include <cassert>

using namespace caustica::math;

namespace caustica::render
{

void PathTracePass::prePass(
    nvrhi::ICommandList* commandList,
    nvrhi::BindingSetHandle bindingSet,
    nvrhi::IDescriptorTable* descriptorTable,
    dm::uint2 viewSize,
    PTPipelineVariant* pipeline)
{
    assert(commandList);
    assert(pipeline);
    assert(bindingSet);
    assert(descriptorTable);

    nvrhi::rt::State state;
    nvrhi::rt::DispatchRaysArguments args;
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
    nvrhi::ICommandList* commandList,
    nvrhi::BindingSetHandle bindingSet,
    nvrhi::IDescriptorTable* descriptorTable,
    dm::uint2 viewSize,
    nvrhi::IComputePipeline* pipeline)
{
    assert(commandList);
    assert(pipeline);
    assert(bindingSet);
    assert(descriptorTable);

    SampleMiniConstants miniConstants = { uint4(0, 0, 0, 0) };

    RAII_SCOPE(commandList->beginMarker("VBufferExport");, commandList->endMarker(););

    nvrhi::ComputeState state;
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
    nvrhi::ICommandList* commandList,
    nvrhi::BindingSetHandle bindingSet,
    nvrhi::IDescriptorTable* descriptorTable,
    dm::uint2 viewSize,
    PTPipelineVariant* pipeline,
    uint32_t samplesPerPixel)
{
    assert(commandList);
    assert(pipeline);
    assert(bindingSet);
    assert(descriptorTable);
    assert(samplesPerPixel > 0);

    nvrhi::rt::State state;
    nvrhi::rt::DispatchRaysArguments args;
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
    assert(ctx.extractedView);

    if (!ctx.hasScene || !ctx.settings->RealtimeMode)
        return;

    const PathTraceGraphTargets handles = importPathTraceGraphTargets(*ctx.graph, *ctx.renderTargets);

    rg::PassOptions passOptions{};
    passOptions.executeAfter = ctx.settings->actualUseRTXDIPasses() ? "RtxdiBeginFrame" : "FrameClear";

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
                ctx.extractedView->renderSize,
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
    assert(ctx.extractedView);

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
                ctx.extractedView->renderSize,
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

    const PathTraceGraphTargets handles = importPathTraceGraphTargets(*ctx.graph, *ctx.renderTargets);

    rg::PassOptions passOptions{};
    passOptions.executeAfter = pathTraceLightingEndExecuteAfterPass(*ctx.settings);

    ctx.graph->addPass(
        "PathTraceLightingEnd",
        [handles](rg::PassBuilder& setup) {
            declarePathTraceLightingEndAccess(setup, handles);
        },
        [ctx](rg::RenderPassContext& passCtx) {
            UpdateLightingEndParams lightingEndParams{
                .commandList = passCtx.commandList(),
                .lightSampling = ctx.lightSampling,
                .bindingCache = ctx.bindingCache,
                .gpuHandles = ctx.gpuHandles,
                .materials = ctx.materials,
                .opacityMaps = ctx.opacityMaps,
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
    assert(ctx.extractedView);

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
                ctx.extractedView->renderSize,
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
