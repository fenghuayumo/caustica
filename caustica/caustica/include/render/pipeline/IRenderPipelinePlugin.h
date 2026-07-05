#pragma once

#include <render/pipeline/RenderGraphRegistry.h>

namespace caustica::render
{

class RenderFrameContext;
class WorldRenderer;

class IRenderPipelinePlugin
{
public:
    virtual ~IRenderPipelinePlugin() = default;

    [[nodiscard]] virtual const char* name() const = 0;

    virtual void onPrepareFrame(WorldRenderer& renderer, RenderFrameContext& ctx) {}
    virtual void registerGraphPasses(RenderGraphRegistry& registry, WorldRenderer& renderer, RenderFrameContext& ctx) {}
    virtual void onFinalizeFrame(WorldRenderer& renderer, RenderFrameContext& ctx) {}
};

} // namespace caustica::render
