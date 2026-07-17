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

    SceneLoadingPlugin{}.configureSchedules(app);
    SceneAnimationPlugin{}.configureSchedules(app);
    CameraPlugin{}.configureSchedules(app);
    PathTracingPlugin{}.configureSchedules(app);
    RenderExtractPlugin{}.configureSchedules(app);
    WindowTitlePlugin{}.configureSchedules(app);

    app.markSceneSchedulesRegistered();
}

} // namespace caustica
