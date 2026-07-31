#include <engine/AssetPlugin.h>

#include <assets/AssetSystem.h>
#include <engine/App.h>
#include <engine/SceneQuery.h>
#include <engine/SceneSession.h>
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
    // Drop Scene ownership before AssetSystem tears down registry/image stores —
    // Scene holds a TextureLoader shared_ptr that would otherwise outlive AssetSystem
    // and crash in ~TextureLoader during m_world.clear().
    app.addSystemAfter<system_label::AssetSystemShutdown, system_label::GpuRenderShutdown>(
        AppSchedule::shutdown,
        [](SystemContext& ctx) {
            clearActiveScene(ctx.app);
            if (auto* session = ctx.tryRes<SceneSession>())
                session->reset();
            if (auto* assets = ctx.tryRes<AssetSystem>())
                assets->shutdown();
        });
}

} // namespace caustica
