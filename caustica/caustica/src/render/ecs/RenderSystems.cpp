#include <render/ecs/RenderSystems.h>
#include <render/WorldRenderer/WorldRenderer.h>

namespace caustica::render
{

void FrameSetupSystem(WorldRenderer& renderer, RenderFrameContext& ctx)
{
    renderer.framePassSetup(ctx.frame);
}

void EnsureRenderTargetsSystem(WorldRenderer& renderer, RenderFrameContext& ctx)
{
    renderer.framePassEnsureRenderTargets(ctx.frame);
}

void RendererInitSystem(WorldRenderer& renderer, RenderFrameContext& ctx)
{
    renderer.framePassRendererInit(ctx.frame);
}

void ShaderUpdateSystem(WorldRenderer& renderer, RenderFrameContext& ctx)
{
    renderer.framePassShaderUpdate(ctx.frame);
}

void BeginCommandListSystem(WorldRenderer& renderer, RenderFrameContext& ctx)
{
    renderer.framePassBeginCommandList(ctx.frame);
}

void SceneUpdateSystem(WorldRenderer& renderer, RenderFrameContext& ctx)
{
    renderer.framePassSceneUpdate(ctx.frame);
}

void PathTracePrepareSystem(WorldRenderer& renderer, RenderFrameContext& ctx)
{
    renderer.framePassPathTracePrepare(ctx.frame);
}

void PathTraceSystem(WorldRenderer& renderer, RenderFrameContext& ctx)
{
    renderer.framePassPathTrace(ctx.frame);
}

void DenoiseAndAASystem(WorldRenderer& renderer, RenderFrameContext& ctx)
{
    renderer.framePassDenoiseAndAA(ctx.frame);
}

void BuildPostProcessGraphSystem(WorldRenderer& renderer, RenderFrameContext& ctx)
{
    renderer.buildPostProcessGraphPasses(ctx);
}

void BuildCompositeGraphSystem(WorldRenderer& renderer, RenderFrameContext& ctx)
{
    renderer.buildCompositeGraphPasses(ctx);
}

void ExecuteRenderGraphSystem(WorldRenderer& renderer, RenderFrameContext& ctx)
{
    renderer.executeFrameRenderGraph(ctx);
}

void DebugLinesSystem(WorldRenderer& renderer, RenderFrameContext& ctx)
{
    renderer.framePassDebugOverlay(ctx.frame);
}

void FinalizeSystem(WorldRenderer& renderer, RenderFrameContext& ctx)
{
    renderer.framePassFinalize(ctx.frame);
}

} // namespace caustica::render
