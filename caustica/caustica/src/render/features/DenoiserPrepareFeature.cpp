#include <render/features/RenderFeature.h>

#include <render/core/PathTracerSettings.h>
#include <render/features/PathTraceGraphResources.h>
#include <render/features/RenderFeatureContext.h>
#include <render/graph/GraphBuilder.h>
#include <render/worldRenderer/WorldRenderer.h>
#include <shaders/PathTracer/Config.h>

#include <cassert>

namespace caustica::render
{

namespace
{
    bool needsStablePlanesDebugViz(const PathTracerSettings& settings)
    {
        if (!settings.RealtimeMode)
            return false;

        return (settings.DebugView > DebugViewType::Disabled
                && settings.DebugView <= DebugViewType::StablePlane_SpecRadiance)
            || settings.DebugView == DebugViewType::StableRadiance;
    }
}

void registerDenoiserPrepareFeature(RenderFeatureContext ctx)
{
    assert(ctx.graph);
    assert(ctx.renderer);
    assert(ctx.renderTargets);
    assert(ctx.settings);

    if (!ctx.hasScene)
        return;

    const PathTraceGraphTargets handles = importPathTraceGraphTargets(*ctx.graph, *ctx.renderTargets);
    extractPathTraceGraphOutputs(*ctx.graph, handles);

    rg::PassOptions denoiserPassOptions{};
    denoiserPassOptions.executeAfter = ctx.settings->ActualUseRTXDIPasses() ? "Rtxdi" : "MainPathTrace";

    ctx.graph->addPass(
        "DenoiserPrepare",
        [handles](rg::PassBuilder& setup) {
            declareDenoiserPrepareAccess(setup, handles);
        },
        [ctx](rg::RenderPassContext& passCtx) {
            ctx.renderer->prepareDenoiserGuides(passCtx.commandList());
        },
        denoiserPassOptions);

    if (needsStablePlanesDebugViz(*ctx.settings))
    {
        rg::PassOptions debugVizPassOptions{};
        debugVizPassOptions.executeAfter = "DenoiserPrepare";

        ctx.graph->addPass(
            "StablePlanesDebugViz",
            [handles](rg::PassBuilder& setup) {
                declareStablePlanesDebugVizAccess(setup, handles);
            },
            [ctx](rg::RenderPassContext& passCtx) {
                ctx.renderer->stablePlanesDebugViz(passCtx.commandList());
            },
            debugVizPassOptions);
    }
}

} // namespace caustica::render
