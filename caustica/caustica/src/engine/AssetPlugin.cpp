#include <engine/AssetPlugin.h>

#include <assets/AssetSystem.h>
#include <engine/App.h>

namespace caustica
{

void AssetPlugin::build(App& app)
{
    if (!app.tryResource<AssetSystem>())
        app.emplaceResource<AssetSystem>();
}

void AssetPlugin::configureSchedules(App& app)
{
    // After GpuRender so RenderInfra has dropped its TextureLoader shared_ptr first.
    app.addSystemAfter(AppSchedule::shutdown, "AssetSystem.shutdown", "GpuRender.shutdown", [](SystemContext& ctx) {
        if (auto* assets = ctx.tryRes<AssetSystem>())
            assets->shutdown();
    });
}

} // namespace caustica
