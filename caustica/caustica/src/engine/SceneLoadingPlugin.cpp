#include <engine/ScenePlugins.h>

#include <engine/App.h>
#include <engine/AppSchedules.h>
#include <engine/RenderFrameApi.h>
#include <engine/SystemLabels.h>

namespace caustica
{

void SceneLoadingPlugin::configureSchedules(App& app)
{
    app.addSystemAfter<system_label::SceneBeginFrame, system_label::SyncRenderThread>(
        AppSchedule::First,
        [](SystemContext& ctx) {
            caustica::beginFrameScheduled(ctx.app);
        });
}

} // namespace caustica
