#include <render/FrameGraphPasses.h>

#include <render/FrameGraphContext.h>
#include <render/WorldRenderer.h>
#include <render/graph/GraphBuilder.h>
#include <render/passes/pathTrace/PathTraceGraphResources.h>
#include <render/passes/rtxdi/RtxdiGraphResources.h>
#include <render/passes/rtxdi/RtxdiPass.h>

#include <cassert>

namespace caustica::render
{

void registerRtxdiBeginFramePass(FrameGraphContext ctx)
{
    assert(ctx.graph);
    assert(ctx.renderer);
    assert(ctx.renderTargets);
    assert(ctx.settings);

    if (!ctx.hasScene || !ctx.settings->actualUseRTXDIPasses())
        return;

    RtxdiPass* rtxdiPass = ctx.renderer->getRtxdiPass();
    RtxdiGraphResources rtxdiResources{};
    if (!tryImportRtxdiGraphResources(*ctx.graph, rtxdiPass, rtxdiResources))
        return;

    const PathTraceGraphTargets pathTraceTargets = importPathTraceGraphTargets(*ctx.graph, *ctx.renderTargets);

    rg::PassOptions passOptions{};
    passOptions.executeAfter = "FrameClear";

    ctx.graph->addPass(
        "RtxdiBeginFrame",
        [rtxdiResources, pathTraceTargets](rg::PassBuilder& setup) {
            declareRtxdiBeginFrameAccess(setup, rtxdiResources, pathTraceTargets);
        },
        [ctx, rtxdiPass](rg::RenderPassContext& passCtx) {
            if (rtxdiPass == nullptr)
                return;

            rtxdiPass->beginFrame(
                passCtx.commandList(),
                *ctx.renderTargets,
                ctx.renderer->getBindingLayout(),
                ctx.renderer->getBindingSet());
        },
        passOptions);
}

void registerRtxdiExecutePass(FrameGraphContext ctx)
{
    assert(ctx.graph);
    assert(ctx.renderer);
    assert(ctx.renderTargets);
    assert(ctx.settings);

    if (!ctx.hasScene || !ctx.settings->actualUseRTXDIPasses())
        return;

    RtxdiPass* rtxdiPass = ctx.renderer->getRtxdiPass();
    RtxdiGraphResources rtxdiResources{};
    if (!tryImportRtxdiGraphResources(*ctx.graph, rtxdiPass, rtxdiResources))
        return;

    const PathTraceGraphTargets pathTraceTargets = importPathTraceGraphTargets(*ctx.graph, *ctx.renderTargets);

    rg::PassOptions passOptions{};
    passOptions.executeAfter = "MainPathTrace";

    ctx.graph->addPass(
        "Rtxdi",
        [rtxdiResources, pathTraceTargets, settings = *ctx.settings](rg::PassBuilder& setup) {
            declareRtxdiExecuteAccess(setup, rtxdiResources, pathTraceTargets, settings);
        },
        [ctx](rg::RenderPassContext& passCtx) {
            ctx.renderer->executeRtxdi(passCtx.commandList());
        },
        passOptions);
}

void registerRtxdiGraphPasses(FrameGraphContext ctx)
{
    registerRtxdiBeginFramePass(ctx);
    registerRtxdiExecutePass(ctx);
}

} // namespace caustica::render
