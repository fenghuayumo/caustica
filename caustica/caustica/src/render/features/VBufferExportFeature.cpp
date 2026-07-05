#include <render/features/RenderFeature.h>

#include <render/features/PathTraceGraphResources.h>
#include <render/features/RenderFeatureContext.h>
#include <render/graph/GraphBuilder.h>
#include <render/worldRenderer/WorldRenderer.h>

#include <cassert>

namespace caustica::render
{

void registerVBufferExportFeature(RenderFeatureContext ctx)
{
    assert(ctx.graph);
    assert(ctx.renderer);
    assert(ctx.renderTargets);
    assert(ctx.settings);

    if (!ctx.hasScene || !ctx.settings->RealtimeMode)
        return;

    const PathTraceGraphTargets handles = importPathTraceGraphTargets(*ctx.graph, *ctx.renderTargets);

    ctx.graph->addPass(
        "VBufferExport",
        [handles](rg::PassBuilder& setup) {
            declareVBufferExportAccess(setup, handles);
        },
        [ctx](rg::RenderPassContext& passCtx) {
            ctx.renderer->vBufferExport(passCtx.commandList());
        },
        rg::PassOptions{ .sideEffect = true });
}

} // namespace caustica::render
