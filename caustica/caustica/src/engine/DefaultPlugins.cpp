#include <engine/DefaultPlugins.h>
#include <engine/App.h>

namespace caustica
{

void DefaultPlugins::build(App& app)
{
    app.emplaceSubsystem<GpuRenderSubsystem>();
    app.emplaceSubsystem<SceneRuntimeSubsystem>(sceneConfig);
}

} // namespace caustica
