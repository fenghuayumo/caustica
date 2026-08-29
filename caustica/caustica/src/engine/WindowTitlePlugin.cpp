#include <engine/ScenePlugins.h>

#include <engine/App.h>
#include <engine/AppSchedules.h>
#include <engine/internal/ActiveSceneAccess.h>
#include <engine/SceneQuery.h>
#include <engine/RenderSessionApi.h>
#include <engine/SceneViewState.h>
#include <engine/SystemLabels.h>

#include <backend/GpuDevice.h>
#include <scene/Scene.h>
#include <caustica/version.h>

#include <string>

extern const char* g_windowTitle;

namespace caustica
{

void updateWindowTitle(App& app)
{
    GpuDevice* device = app.getGpuDevice();
    if (!device)
        return;

    const std::string versionedTitle = std::string(g_windowTitle ? g_windowTitle : "caustica")
        + " " + caustica::kVersionString;
    device->setInformativeWindowTitle(versionedTitle.c_str(), false);
}

void WindowTitlePlugin::configureSchedules(App& app)
{
    app.addSystemAfter<system_label::SceneUpdateWindowTitle, system_label::SceneTickSimulation>(
        AppSchedule::update,
        [](SystemContext& ctx) {
            if (!ctx.windowFocused)
                return;

            updateWindowTitle(ctx.app);
        });
    // Last DefaultPlugins scene-schedule member: mark after systems exist so
    // App::buildPlugins skips the registerSceneSchedules fallback.
    app.markSceneSchedulesRegistered();
}

} // namespace caustica
