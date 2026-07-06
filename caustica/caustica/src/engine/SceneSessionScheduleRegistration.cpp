#include <engine/SceneSessionScheduleRegistration.h>

#include <engine/App.h>
#include <engine/SceneSessionPlugins.h>
#include <engine/SceneViewState.h>

namespace caustica
{

void registerSceneSessionSchedules(App& app)
{
    if (app.sceneSessionSchedulesRegistered())
        return;

    if (!app.tryResource<SceneViewState>())
        return;

    sceneSession::registerSceneLoadingPlugin(app);
    sceneSession::registerSceneAnimationPlugin(app);
    sceneSession::registerCameraPlugin(app);
    sceneSession::registerPathTracingPlugin(app);
    sceneSession::registerRenderExtractPlugin(app);
    sceneSession::registerWindowTitlePlugin(app);

    app.markSceneSessionSchedulesRegistered();
}

} // namespace caustica
