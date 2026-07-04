#pragma once

#include <engine/EngineBuilder.h>
#include <engine/GpuRenderSubsystem.h>
#include <engine/IEnginePlugin.h>
#include <engine/SceneRuntimeSubsystem.h>

namespace caustica
{

// Bundles GpuRenderSubsystem + SceneRuntimeSubsystem (minimal runtime stack).
struct DefaultRuntimePlugin : IEnginePlugin
{
    explicit DefaultRuntimePlugin(SceneRuntimeSubsystemConfig sceneConfig)
        : sceneConfig(std::move(sceneConfig))
    {
    }

    void build(EngineBuilder& builder) override
    {
        builder.emplaceSubsystem<GpuRenderSubsystem>();
        builder.emplaceSubsystem<SceneRuntimeSubsystem>(sceneConfig);
    }

    SceneRuntimeSubsystemConfig sceneConfig;
};

} // namespace caustica
