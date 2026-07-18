#include <render/FrameGraphPasses.h>

#include <render/FrameGraphContext.h>
#include <render/WorldRenderer.h>
#include <render/graph/GraphBuilder.h>
#include <render/passes/pathTrace/PathTraceGraphResources.h>

#include <cassert>

namespace caustica::render
{

void registerPathTracePrePass(FrameGraphContext ctx)
{
    assert(ctx.graph);
    assert(ctx.renderer);
    assert(ctx.renderTargets);
    assert(ctx.settings);

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
            ctx.renderer->pathTracePrePass(passCtx.commandList());
        },
        passOptions);
}

void registerVBufferExportPass(FrameGraphContext ctx)
{
    assert(ctx.graph);
    assert(ctx.renderer);
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
            ctx.renderer->vBufferExport(passCtx.commandList());
        },
        passOptions);
}

void registerPathTraceLightingEndPass(FrameGraphContext ctx)
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

void registerMainPathTracePass(FrameGraphContext ctx)
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

void registerPathTraceGraphPasses(FrameGraphContext ctx)
{
    registerPathTracePrePass(ctx);
    registerVBufferExportPass(ctx);
    registerPathTraceLightingEndPass(ctx);
    registerMainPathTracePass(ctx);
}

} // namespace caustica::render
