#pragma once

#include <render/graph/GraphBuilder.h>
#include <render/Core/FullscreenBlitPass.h>

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
    caustica::render::FullscreenBlitPass& blitPass);

} // namespace caustica::rg
