#pragma once

#include <engine/Plugin.h>
#include <engine/SceneStartup.h>

namespace caustica
{

// Core scene runtime resources + startup (without AssetPlugin / scene schedule plugins).
struct SceneRuntimePlugin : Plugin
{
    explicit SceneRuntimePlugin(SceneAppConfig appConfig)
        : appConfig(std::move(appConfig))
    {
    }

    void build(App& app) override;
    void configureSchedules(App& app) override;

    SceneAppConfig appConfig;
};

// Shared runtime bootstrap for headless apps and the editor:
// AssetPlugin + SceneRuntimePlugin + scene schedule plugins from ScenePlugins.h.
struct DefaultPlugins : PluginGroup
{
    explicit DefaultPlugins(SceneAppConfig appConfig)
        : appConfig(std::move(appConfig))
    {
    }

    void build(App& app) override;

    SceneAppConfig appConfig;
};

} // namespace caustica
