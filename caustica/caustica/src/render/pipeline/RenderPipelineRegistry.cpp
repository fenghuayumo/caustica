#include <render/pipeline/RenderPipelineRegistry.h>

#include <render/pipeline/PathTracingPipelinePlugin.h>
#include <render/pipeline/RenderGraphRegistry.h>
#include <render/WorldRenderer.h>

namespace caustica::render
{

RenderPipelineRegistry::RenderPipelineRegistry()
{
    addPlugin(std::make_unique<PathTracingPipelinePlugin>());
}

void RenderPipelineRegistry::addPlugin(std::unique_ptr<IRenderPipelinePlugin> plugin)
{
    if (!plugin)
        return;

    m_plugins.push_back(plugin.get());
    m_ownedPlugins.push_back(std::move(plugin));
}

void RenderPipelineRegistry::addPlugin(IRenderPipelinePlugin& plugin)
{
    m_plugins.push_back(&plugin);
}

void RenderPipelineRegistry::runFrame(WorldRenderer& renderer, RenderFrameContext& ctx)
{
    for (IRenderPipelinePlugin* plugin : m_plugins)
    {
        if (!plugin || ctx.frame.aborted)
            break;
        plugin->onPrepareFrame(renderer, ctx);
    }

    if (ctx.frame.aborted)
        return;

    RenderGraphRegistry graphRegistry;
    for (IRenderPipelinePlugin* plugin : m_plugins)
    {
        if (plugin)
            plugin->registerGraphPasses(graphRegistry, renderer, ctx);
    }

    renderer.buildFrameGraphPasses(ctx, graphRegistry);
    if (ctx.frame.aborted)
        return;

    renderer.executeFrameRenderGraph(ctx);
    if (ctx.frame.aborted)
        return;

    for (IRenderPipelinePlugin* plugin : m_plugins)
    {
        if (!plugin || ctx.frame.aborted)
            break;
        plugin->onFinalizeFrame(renderer, ctx);
    }
}

void RenderPipelineRegistry::clear()
{
    m_ownedPlugins.clear();
    m_plugins.clear();
}

} // namespace caustica::render
