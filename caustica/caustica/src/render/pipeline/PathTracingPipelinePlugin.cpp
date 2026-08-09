#include <render/pipeline/PathTracingPipelinePlugin.h>

#include <render/FrameGraphContext.h>
#include <render/FrameGraphPasses.h>
#include <render/WorldRenderer.h>

namespace caustica::render
{

void PathTracingPipelinePlugin::onPrepareFrame(WorldRenderer& renderer, RenderFrameContext& ctx)
{
    if (ctx.frame.aborted)
        return;

    renderer.framePassSetup(ctx.frame);
    if (ctx.frame.aborted)
        return;

    renderer.framePassEnsureRenderTargets(ctx.frame);
    if (ctx.frame.aborted)
        return;

    renderer.framePassRendererInit(ctx.frame);
    if (ctx.frame.aborted)
        return;

    renderer.framePassShaderUpdate(ctx.frame);
    if (ctx.frame.aborted)
        return;

    renderer.framePassBeginCommandList(ctx.frame);
    if (ctx.frame.aborted)
        return;

    renderer.framePassSceneUpdate(ctx.frame);
    if (ctx.frame.aborted)
        return;

    renderer.framePassPathTracePrepare(ctx.frame);
    if (ctx.frame.aborted)
        return;

    renderer.framePassPathTrace(ctx.frame);
    if (ctx.frame.aborted)
        return;

    renderer.framePassDenoiseAndAA(ctx.frame);
}

void PathTracingPipelinePlugin::buildGraph(FrameGraphContext& ctx)
{
    registerDefaultFrameGraphPasses(ctx);
}

void PathTracingPipelinePlugin::onFinalizeFrame(WorldRenderer& renderer, RenderFrameContext& ctx)
{
    if (ctx.frame.aborted)
        return;

    renderer.framePassFinalize(ctx.frame);
}

} // namespace caustica::render
