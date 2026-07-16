#pragma once

#include <engine/GpuRenderSubsystem.h>
#include <engine/Plugin.h>
#include <engine/SceneStartup.h>

namespace caustica
{

// Minimal runtime: GPU infra + scene plugins (no editor UI).
struct DefaultPlugins : Plugin
{
    explicit DefaultPlugins(SceneAppConfig appConfig)
        : appConfig(std::move(appConfig))
    {
    }

    void build(App& app) override;
    void configureSchedules(App& app) override;

    SceneAppConfig appConfig;
};

} // namespace caustica
