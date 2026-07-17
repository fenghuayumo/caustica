#include <render/pipeline/PathTracingPipelinePlugin.h>

#include <render/features/RenderFeature.h>
#include <render/worldRenderer/WorldRenderer.h>

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

void PathTracingPipelinePlugin::registerGraphPasses(
    RenderGraphRegistry& registry,
    WorldRenderer& /*renderer*/,
    RenderFrameContext& /*ctx*/)
{
    registry.add([](RenderFeatureContext& featureCtx) {
        registerDefaultGraphFeatures(featureCtx);
    });
}

void PathTracingPipelinePlugin::onFinalizeFrame(WorldRenderer& renderer, RenderFrameContext& ctx)
{
    if (ctx.frame.aborted)
        return;

    renderer.framePassFinalize(ctx.frame);
}

} // namespace caustica::render
