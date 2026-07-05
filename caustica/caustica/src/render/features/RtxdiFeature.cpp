#include <render/features/RenderFeature.h>

#include <render/features/PathTraceGraphResources.h>
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

    const PathTraceGraphTargets handles = importPathTraceGraphTargets(*ctx.graph, *ctx.renderTargets);

    ctx.graph->addPass(
        "RtxdiBeginFrame",
        [handles](rg::PassBuilder& setup) {
            declareRtxdiBeginFrameAccess(setup, handles);
        },
        [ctx](rg::RenderPassContext& passCtx) {
            RtxdiPass* rtxdiPass = ctx.renderer->getRtxdiPass();
            if (rtxdiPass == nullptr)
                return;

            rtxdiPass->BeginFrame(
                passCtx.commandList(),
                *ctx.renderTargets,
                ctx.renderer->getBindingLayout(),
                ctx.renderer->getBindingSet());
        },
        rg::PassOptions{ .sideEffect = true });
}

void registerRtxdiExecuteFeature(RenderFeatureContext ctx)
{
    assert(ctx.graph);
    assert(ctx.renderer);
    assert(ctx.renderTargets);
    assert(ctx.settings);

    if (!ctx.hasScene || !ctx.settings->ActualUseRTXDIPasses())
        return;

    const PathTraceGraphTargets handles = importPathTraceGraphTargets(*ctx.graph, *ctx.renderTargets);

    ctx.graph->addPass(
        "Rtxdi",
        [handles](rg::PassBuilder& setup) {
            declareRtxdiExecuteAccess(setup, handles);
        },
        [ctx](rg::RenderPassContext& passCtx) {
            ctx.renderer->executeRtxdi(passCtx.commandList());
        },
        rg::PassOptions{ .sideEffect = true });
}

} // namespace caustica::render
