#include <render/features/RenderFeature.h>

#include <render/features/PathTraceGraphResources.h>
#include <render/features/RtxdiGraphResources.h>
#include <render/features/RenderFeatureContext.h>
#include <render/graph/GraphBuilder.h>
#include <render/passes/rtxdi/RtxdiPass.h>
#include <render/worldRenderer/WorldRenderer.h>

#include <cassert>

namespace caustica::render
{

void registerRtxdiBeginFrameFeature(RenderFeatureContext ctx)
{
    assert(ctx.graph);
    assert(ctx.renderer);
    assert(ctx.renderTargets);
    assert(ctx.settings);

    if (!ctx.hasScene || !ctx.settings->ActualUseRTXDIPasses())
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

            rtxdiPass->BeginFrame(
                passCtx.commandList(),
                *ctx.renderTargets,
                ctx.renderer->getBindingLayout(),
                ctx.renderer->getBindingSet());
        },
        passOptions);
}

void registerRtxdiExecuteFeature(RenderFeatureContext ctx)
{
    assert(ctx.graph);
    assert(ctx.renderer);
    assert(ctx.renderTargets);
    assert(ctx.settings);

    if (!ctx.hasScene || !ctx.settings->ActualUseRTXDIPasses())
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

} // namespace caustica::render
