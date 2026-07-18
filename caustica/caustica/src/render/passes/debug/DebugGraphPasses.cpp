#include <render/FrameGraphPasses.h>

#include <render/FrameGraphContext.h>
#include <render/WorldRenderer.h>

#include <cassert>

namespace caustica::render
{

void registerDebugOverlayGraphPasses(FrameGraphContext ctx)
{
    assert(ctx.graph);
    assert(ctx.renderer);
    assert(ctx.targetFramebuffer);

    ctx.renderer->registerDebugOverlayGraphPasses(ctx);
}

} // namespace caustica::render
