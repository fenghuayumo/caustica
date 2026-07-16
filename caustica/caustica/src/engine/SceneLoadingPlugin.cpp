#include <engine/ScenePlugins.h>

#include <engine/App.h>
#include <engine/AppSchedules.h>
#include <engine/SceneApi.h>

namespace caustica
{

void registerSceneLoadingPlugin(App& app)
{
    app.addSystemAfter(AppSchedule::First, "Scene.beginFrame", "SyncRenderThread", [](SystemContext& ctx) {
        caustica::beginFrameScheduled(ctx.app);
    });
}

} // namespace caustica
