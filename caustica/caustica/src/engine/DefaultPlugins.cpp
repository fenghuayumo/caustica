#include <engine/DefaultPlugins.h>
#include <engine/App.h>
#include <engine/EngineScheduleRegistration.h>

namespace caustica
{

void DefaultPlugins::build(App& app)
{
    app.emplaceSubsystem<GpuRenderSubsystem>();
    app.emplaceSubsystem<SceneRuntimeSubsystem>(sceneConfig);
}

void DefaultPlugins::configureSchedules(App& app)
{
    registerEngineScheduleBridge(app);
}

} // namespace caustica
