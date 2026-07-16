#include <engine/SceneScheduleRegistration.h>

#include <engine/App.h>
#include <engine/ScenePlugins.h>
#include <engine/SceneViewState.h>

namespace caustica
{

void registerSceneSchedules(App& app)
{
    if (app.sceneSchedulesRegistered())
        return;

    if (!app.tryResource<SceneViewState>())
        return;

    caustica::registerSceneLoadingPlugin(app);
    caustica::registerSceneAnimationPlugin(app);
    caustica::registerCameraPlugin(app);
    caustica::registerPathTracingPlugin(app);
    caustica::registerRenderExtractPlugin(app);
    caustica::registerWindowTitlePlugin(app);

    app.markSceneSchedulesRegistered();
}

} // namespace caustica
