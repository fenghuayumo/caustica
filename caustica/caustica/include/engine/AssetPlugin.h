#pragma once

#include <engine/Plugin.h>

namespace caustica
{

class App;

// Sole owner of AssetSystem App-resource lifecycle (emplace + shutdown schedule).
// GpuSharedCaches only calls AssetSystem::initialize(); GpuRenderSubsystem may clear
// scene assets but must not shut the system down.
struct AssetPlugin : Plugin
{
    void build(App& app) override;
    void configureSchedules(App& app) override;
};

} // namespace caustica
