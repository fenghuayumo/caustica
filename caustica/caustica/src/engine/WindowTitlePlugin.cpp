#include <engine/SceneSessionPlugins.h>

#include <engine/App.h>
#include <engine/AppSchedules.h>
#include <engine/SceneViewState.h>
#include <engine/SceneSessionSystems.h>

#include <string>

extern const char* g_windowTitle;

namespace caustica::sceneSession
{

void updateWindowTitle(App& app)
{
    if (auto activeScene = scene(app))
    {
        SceneViewState* vs = viewState(app);
        ::SceneManager* manager = sceneManager(app);
        GpuDevice* device = gpuDevice(app);
        if (!vs || !manager || !device)
            return;

        std::string extraInfo = ", " + vs->fpsInfo + ", " + manager->getCurrentSceneName() + ", "
            + resolutionInfo(app) + ", (L: " + std::to_string(activeScene->getLightEntities().size())
            + ", MAT: " + std::to_string(activeScene->getMaterials().size())
            + ", MESH: " + std::to_string(activeScene->getMeshes().size())
            + ", I: " + std::to_string(activeScene->getMeshInstances().size())
            + ", SI: " + std::to_string(activeScene->getSkinnedMeshInstances().size())
#if ENABLE_DEBUG_VIZUALISATIONS
            + ", ENABLE_DEBUG_VIZUALISATIONS: 1"
#endif
            + ")";

        device->setInformativeWindowTitle(g_windowTitle, false, extraInfo.c_str());
    }
}

void registerWindowTitlePlugin(App& app)
{
    app.addSystemAfter(AppSchedule::update, "SceneSession.UpdateWindowTitle", "SceneSession.TickSimulation", [](SystemContext& ctx) {
        if (!ctx.windowFocused)
            return;

        sceneSession::updateWindowTitle(ctx.app);
    });
}

} // namespace caustica::sceneSession
