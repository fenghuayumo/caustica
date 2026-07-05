#include <render/features/RenderFeature.h>

#include <render/features/PathTraceGraphResources.h>
#include <render/features/RenderFeatureContext.h>
#include <render/graph/GraphBuilder.h>
#include <render/worldRenderer/WorldRenderer.h>

#include <cassert>

namespace caustica::render
{

void registerPathTraceLightingEndFeature(RenderFeatureContext ctx)
{
    assert(ctx.graph);
    assert(ctx.renderer);
    assert(ctx.renderTargets);
    assert(ctx.settings);

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
            ctx.renderer->pathTraceLightingEndUpdate(passCtx.commandList());
        },
        passOptions);
}

} // namespace caustica::render
