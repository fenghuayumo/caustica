#include <render/graph/BlitGraphPass.h>

#include <cassert>

namespace caustica::rg
{

void registerFinalBlitPass(
    GraphBuilder& graph,
    const FinalBlitPassParams& params,
    rhi::FullscreenBlitPass& blitPass)
{
    assert(params.sourceLdrColor.valid());
    assert(params.targetFramebuffer);

    nvrhi::ITexture* targetColor = params.targetFramebuffer->getDesc().colorAttachments[0].texture;
    assert(targetColor);

    const TextureHandle targetColorHandle = graph.importTexture(targetColor, TextureAccess::RenderTarget);

    graph.addPass(
        "Blit",
        [&](PassBuilder& setup) {
            setup.read(params.sourceLdrColor, TextureAccess::ShaderResource);
            setup.write(targetColorHandle, TextureAccess::RenderTarget);
        },
        [&](RenderPassContext& ctx) {
            rhi::BlitParameters blitParams{};
            blitParams.targetFramebuffer = params.targetFramebuffer;
            blitParams.sourceTexture = ctx.texture(params.sourceLdrColor);
            blitPass.blitTexture(ctx.commandList(), blitParams, params.bindingCache);
        });
}

} // namespace caustica::rg
