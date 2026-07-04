#pragma once

#include <render/graph/GraphBuilder.h>
#include <rhi/FullscreenBlitPass.h>

namespace caustica
{
class BindingCache;
}

namespace caustica::rg
{

struct FinalBlitPassParams
{
    TextureHandle sourceLdrColor{};
    nvrhi::IFramebuffer* targetFramebuffer = nullptr;
    caustica::BindingCache* bindingCache = nullptr;
};

void registerFinalBlitPass(
    GraphBuilder& graph,
    const FinalBlitPassParams& params,
    rhi::FullscreenBlitPass& blitPass);

} // namespace caustica::rg
