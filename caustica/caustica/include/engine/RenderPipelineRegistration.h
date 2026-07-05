#pragma once

#include <engine/SceneRuntime.h>
#include <render/pipeline/RenderPipelineRegistry.h>
#include <render/worldRenderer/WorldRenderer.h>

#include <memory>

namespace caustica
{

template<typename T, typename... Args>
void registerRenderPipelinePlugin(SceneRuntime& sceneRuntime, Args&&... args)
{
    render::WorldRenderer* worldRenderer = sceneRuntime.GetWorldRenderer();
    if (!worldRenderer)
        return;

    worldRenderer->addRenderPipelinePlugin(
        std::make_unique<T>(std::forward<Args>(args)...));
}

inline void registerRenderPipelinePlugin(
    SceneRuntime& sceneRuntime,
    std::unique_ptr<render::IRenderPipelinePlugin> plugin)
{
    render::WorldRenderer* worldRenderer = sceneRuntime.GetWorldRenderer();
    if (!worldRenderer)
        return;

    worldRenderer->addRenderPipelinePlugin(std::move(plugin));
}

} // namespace caustica
