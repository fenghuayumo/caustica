#include <engine/AssetPlugin.h>

#include <assets/AssetSystem.h>
#include <engine/App.h>
#include <engine/SystemLabels.h>

namespace caustica
{

void AssetPlugin::build(App& app)
{
    if (!app.tryResource<AssetSystem>())
        app.emplaceResource<AssetSystem>();
}

void AssetPlugin::configureSchedules(App& app)
{
    // After GpuRender so GpuSharedCaches has dropped its TextureLoader shared_ptr first.
    app.addSystemAfter<system_label::AssetSystemShutdown, system_label::GpuRenderShutdown>(
        AppSchedule::shutdown,
        [](SystemContext& ctx) {
            if (auto* assets = ctx.tryRes<AssetSystem>())
                assets->shutdown();
        });
}

} // namespace caustica
