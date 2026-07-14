#include <engine/AssetPlugin.h>

#include <assets/AssetSystem.h>
#include <engine/App.h>

namespace caustica
{

void registerAssetPlugin(App& app)
{
    if (!app.tryResource<AssetSystem>())
        app.emplaceResource<AssetSystem>();

    app.addSystemAfter(AppSchedule::shutdown, "AssetSystem.shutdown", "GpuRender.shutdown", [](SystemContext& ctx) {
        if (auto* assets = ctx.tryRes<AssetSystem>())
            assets->shutdown();
    });
}

} // namespace caustica
