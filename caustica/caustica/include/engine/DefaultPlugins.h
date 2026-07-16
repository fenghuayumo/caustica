#pragma once

#include <engine/GpuRenderSubsystem.h>
#include <engine/Plugin.h>
#include <engine/SceneStartup.h>

namespace caustica
{

// Shared runtime bootstrap for headless apps and the editor:
// assets, SceneApp resources, SceneAccess, GpuRenderSubsystem, schedule bridge, scene startup.
// EditorPlugin composes this and only adds editor resources / systems.
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
