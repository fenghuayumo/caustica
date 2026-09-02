#pragma once

// ENGINE-INTERNAL bootstrap. EngineApp::create installs this group.
// Hosts do not include or assemble it.

#include <engine/Plugin.h>

namespace caustica
{

class App;

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

struct DefaultPlugins : PluginGroup
{
    void build(App& app) override;
};

} // namespace caustica
