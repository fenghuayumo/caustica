#include <render/features/RenderFeature.h>

#include <render/features/RenderFeatureContext.h>
#include <render/worldRenderer/WorldRenderer.h>

#include <cassert>

namespace caustica::render
{

void registerDebugOverlayFeature(RenderFeatureContext ctx)
{
    assert(ctx.graph);
    assert(ctx.renderer);
    assert(ctx.targetFramebuffer);

    ctx.renderer->registerDebugOverlayGraphPasses(ctx);
}

} // namespace caustica::render
