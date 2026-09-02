#include <engine/ScenePlugins.h>

#include <engine/App.h>
#include <engine/AppSchedules.h>
#include <engine/internal/ActiveSceneAccess.h>
#include <engine/SceneQuery.h>
#include <engine/RenderSessionApi.h>
#include <engine/SceneViewState.h>
#include <engine/SystemLabels.h>

#include <backend/GpuSurface.h>
#include <scene/Scene.h>
#include <caustica/version.h>

#include <string>

extern const char* g_windowTitle;

namespace caustica
{

void updateWindowTitle(App& app)
{
    GpuSurface* gpuSurface = app.getSurface();
    if (!gpuSurface)
        return;

    const std::string versionedTitle = std::string(g_windowTitle ? g_windowTitle : "caustica")
        + " " + caustica::kVersionString;
    gpuSurface->setInformativeWindowTitle(versionedTitle.c_str(), false);
}

void WindowTitlePlugin::configureSchedules(App& app)
{
    // The built-in title contains only application/version/backend metadata.
    // Updating it every frame made this main-thread-only GLFW call an exclusive
    // barrier in the update schedule, preventing otherwise disjoint systems
    // from overlapping. Graphics is initialized before Startup runs, so once is
    // sufficient; editor-specific dynamic titles remain editor-owned.
    app.addSystem<system_label::SceneUpdateWindowTitle>(
        AppSchedule::Startup,
        [](SystemContext& ctx) { updateWindowTitle(ctx.app); });
    // Last DefaultPlugins scene-schedule member: mark after systems exist so
    // App::buildPlugins skips the registerSceneSchedules fallback.
    app.markSceneSchedulesRegistered();
}

} // namespace caustica
