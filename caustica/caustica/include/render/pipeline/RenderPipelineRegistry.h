#pragma once

#include <render/pipeline/IRenderPipelinePlugin.h>

#include <memory>
#include <vector>

namespace caustica::render
{

class RenderFrameContext;
class WorldRenderer;

class RenderPipelineRegistry
{
public:
    RenderPipelineRegistry();

    void addPlugin(std::unique_ptr<IRenderPipelinePlugin> plugin);
    void addPlugin(IRenderPipelinePlugin& plugin);

    void runFrame(WorldRenderer& renderer, RenderFrameContext& ctx);

    void clear();

private:
    std::vector<std::unique_ptr<IRenderPipelinePlugin>> m_ownedPlugins;
    std::vector<IRenderPipelinePlugin*> m_plugins;
};

} // namespace caustica::render
