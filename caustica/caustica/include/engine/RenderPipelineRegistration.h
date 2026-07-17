#pragma once

#include <engine/App.h>
#include <engine/AppResources.h>
#include <render/pipeline/RenderPipelineRegistry.h>
#include <render/worldRenderer/WorldRenderer.h>

#include <memory>

namespace caustica
{

template<typename T, typename... Args>
void registerRenderPipelinePlugin(App& app, Args&&... args)
{
    render::WorldRenderer* wr = worldRenderer(app);
    if (!wr)
        return;

    wr->addRenderPipelinePlugin(std::make_unique<T>(std::forward<Args>(args)...));
}

inline void registerRenderPipelinePlugin(
    App& app,
    std::unique_ptr<render::IRenderPipelinePlugin> plugin)
{
    render::WorldRenderer* wr = worldRenderer(app);
    if (!wr)
        return;

    wr->addRenderPipelinePlugin(std::move(plugin));
}

} // namespace caustica
