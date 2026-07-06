#pragma once

#include <engine/GpuRenderSubsystem.h>
#include <engine/Plugin.h>
#include <engine/SceneSessionSubsystem.h>

namespace caustica
{

// Minimal runtime: GPU infra + scene session (no editor UI).
struct DefaultPlugins : Plugin
{
    explicit DefaultPlugins(SceneSessionConfig sessionConfig)
        : sessionConfig(std::move(sessionConfig))
    {
    }

    void build(App& app) override;
    void configureSchedules(App& app) override;

    SceneSessionConfig sessionConfig;
};

} // namespace caustica
