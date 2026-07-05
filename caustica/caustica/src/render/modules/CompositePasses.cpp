#include <render/modules/CompositePasses.h>

#include <render/core/FullscreenBlitPass.h>
#include <render/core/RenderTargets.h>
#include <render/graph/GraphBuilder.h>
#include <render/modules/RenderModuleContext.h>

#include <cassert>

namespace caustica::render
{

void registerCompositePasses(RenderModuleContext ctx)
{
    assert(ctx.targetFramebuffer);
    assert(ctx.bindingCache);
    assert(ctx.blitPass);
    assert(ctx.renderTargets);
    assert(ctx.graph);

    const rg::TextureHandle ldrColor = ctx.graph->importTexture(
        ctx.renderTargets->ldrColor,
        nvrhi::ResourceStates::ShaderResource);

    nvrhi::ITexture* targetColor = ctx.targetFramebuffer->getDesc().colorAttachments[0].texture;
    assert(targetColor);

    const rg::TextureHandle targetColorHandle = ctx.graph->importTexture(targetColor, rg::TextureAccess::RenderTarget);
    ctx.graph->extractTexture(targetColorHandle, rg::TextureAccess::RenderTarget);

    ctx.graph->addPass(
        "Blit",
        [ldrColor, targetColorHandle](rg::PassBuilder& setup) {
            setup.read(ldrColor, rg::TextureAccess::ShaderResource);
            setup.write(targetColorHandle, rg::TextureAccess::RenderTarget);
        },
        [ctx, ldrColor](rg::RenderPassContext& passCtx) {
            BlitParameters blitParams{};
            blitParams.targetFramebuffer = ctx.targetFramebuffer;
            blitParams.sourceTexture = passCtx.texture(ldrColor);
            ctx.blitPass->blitTexture(passCtx.commandList(), blitParams, nullptr);
        },
        rg::PassOptions{ .sideEffect = true });
}

} // namespace caustica::render
