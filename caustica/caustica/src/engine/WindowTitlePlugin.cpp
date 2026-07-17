#include <engine/ScenePlugins.h>

#include <engine/App.h>
#include <engine/AppSchedules.h>
#include <engine/SceneQuery.h>
#include <engine/RenderSessionApi.h>
#include <engine/SceneViewState.h>

#include <backend/GpuDevice.h>
#include <scene/Scene.h>

#include <string>

extern const char* g_windowTitle;

namespace caustica
{

void updateWindowTitle(App& app)
{
    auto* vs = app.tryResource<SceneViewState>();
    GpuDevice* device = app.getGpuDevice();
    const std::shared_ptr<Scene> scene = activeScene(app);
    if (!vs || !device || !scene)
        return;

    std::string extraInfo = ", " + vs->fpsInfo + ", " + currentSceneName(app) + ", "
        + resolutionInfo(app) + ", (L: " + std::to_string(scene->getLightEntities().size())
        + ", MAT: " + std::to_string(scene->getMaterials().size())
        + ", MESH: " + std::to_string(scene->getMeshes().size())
        + ", I: " + std::to_string(scene->getMeshInstances().size())
        + ", SI: " + std::to_string(scene->getSkinnedMeshInstances().size())
#if ENABLE_DEBUG_VIZUALISATIONS
        + ", ENABLE_DEBUG_VIZUALISATIONS: 1"
#endif
        + ")";

    device->setInformativeWindowTitle(g_windowTitle, false, extraInfo.c_str());
}

void WindowTitlePlugin::configureSchedules(App& app)
{
    app.addSystemAfter(AppSchedule::update, "Scene.UpdateWindowTitle", "Scene.TickSimulation", [](SystemContext& ctx) {
        if (!ctx.windowFocused)
            return;

        updateWindowTitle(ctx.app);
    });
}

} // namespace caustica
