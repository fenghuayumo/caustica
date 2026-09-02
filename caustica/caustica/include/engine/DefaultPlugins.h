#pragma once

// ENGINE-INTERNAL bootstrap — not part of the public API.
// Apps use EngineApp::create / <caustica.h>; do not assemble DefaultPlugins yourself.

#include <engine/Plugin.h>

namespace caustica
{

// Core scene runtime resources + startup (without AssetPlugin / scene schedule plugins).
struct SceneRuntimePlugin : Plugin
{
    void build(App& app) override;
    void configureSchedules(App& app) override;
};

struct InputPlugin : Plugin
{
    void build(App& app) override;
    void configureSchedules(App& app) override;
};

// Shared runtime bootstrap for headless apps and the editor:
// InputPlugin + AssetPlugin + SceneRuntimePlugin + scene schedule plugins.
struct DefaultPlugins : PluginGroup
{
    void build(App& app) override;
};

} // namespace caustica
