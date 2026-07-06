#pragma once

#include <engine/GpuRenderSubsystem.h>
#include <engine/App.h>
#include <render/pipeline/RenderPipelineRegistry.h>
#include <render/worldRenderer/WorldRenderer.h>

#include <memory>

namespace caustica
{

template<typename T, typename... Args>
void registerRenderPipelinePlugin(App& app, Args&&... args)
{
    auto* gpuRender = app.tryResource<GpuRenderSubsystem>();
    render::WorldRenderer* worldRenderer = gpuRender ? gpuRender->worldRenderer() : nullptr;
    if (!worldRenderer)
        return;

    worldRenderer->addRenderPipelinePlugin(
        std::make_unique<T>(std::forward<Args>(args)...));
}

inline void registerRenderPipelinePlugin(
    App& app,
    std::unique_ptr<render::IRenderPipelinePlugin> plugin)
{
    auto* gpuRender = app.tryResource<GpuRenderSubsystem>();
    render::WorldRenderer* worldRenderer = gpuRender ? gpuRender->worldRenderer() : nullptr;
    if (!worldRenderer)
        return;

    worldRenderer->addRenderPipelinePlugin(std::move(plugin));
}

} // namespace caustica
