#include <render/features/RenderFeature.h>

#include <render/features/PathTraceGraphResources.h>
#include <render/features/RenderFeatureContext.h>
#include <render/graph/GraphBuilder.h>
#include <render/worldRenderer/WorldRenderer.h>

#include <cassert>

namespace caustica::render
{

void registerMainPathTraceFeature(RenderFeatureContext ctx)
{
    assert(ctx.graph);
    assert(ctx.renderer);
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
            ctx.renderer->mainPathTrace(passCtx.commandList());
        },
        passOptions);
}

} // namespace caustica::render
