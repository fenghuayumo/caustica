#include <engine/ScenePlugins.h>

#include <engine/App.h>
#include <engine/AppSchedules.h>
#include <engine/RenderFrameApi.h>

namespace caustica
{

void SceneLoadingPlugin::configureSchedules(App& app)
{
    app.addSystemAfter(AppSchedule::First, "Scene.beginFrame", "SyncRenderThread", [](SystemContext& ctx) {
        caustica::beginFrameScheduled(ctx.app);
    });
}

} // namespace caustica
