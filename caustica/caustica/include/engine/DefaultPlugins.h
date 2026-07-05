#pragma once

#include <engine/GpuRenderSubsystem.h>
#include <engine/Plugin.h>
#include <engine/SceneRuntimeSubsystem.h>

namespace caustica
{

// Minimal runtime: GPU infra + scene session (no editor UI).
struct DefaultPlugins : Plugin
{
    explicit DefaultPlugins(SceneRuntimeSubsystemConfig sceneConfig)
        : sceneConfig(std::move(sceneConfig))
    {
    }

    void build(App& app) override;

    SceneRuntimeSubsystemConfig sceneConfig;
};

} // namespace caustica
