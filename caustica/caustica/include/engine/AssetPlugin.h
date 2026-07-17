#pragma once

#include <engine/Plugin.h>

namespace caustica
{

class App;

struct AssetPlugin : Plugin
{
    void build(App& app) override;
    void configureSchedules(App& app) override;
};

} // namespace caustica
