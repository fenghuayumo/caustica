#include <engine/ScenePlugins.h>

#include <engine/App.h>
#include <engine/AppSchedules.h>
#include <engine/GpuRenderSubsystem.h>
#include <engine/SceneApi.h>
#include <engine/SceneViewState.h>

#include <backend/GpuDevice.h>
#include <scene/Scene.h>
#include <scene/SceneManager.h>

#include <string>

extern const char* g_windowTitle;

namespace caustica
{

void updateWindowTitle(App& app)
{
    auto* gr = app.tryResource<GpuRenderSubsystem>();
    auto* vs = app.tryResource<SceneViewState>();
    GpuDevice* device = app.getGpuDevice();
    if (!gr || !vs || !device || !gr->sceneManager())
        return;

    auto activeScene = gr->sceneManager()->getScene();
    if (!activeScene)
        return;

    std::string extraInfo = ", " + vs->fpsInfo + ", " + gr->sceneManager()->getCurrentSceneName() + ", "
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

void registerWindowTitlePlugin(App& app)
{
    app.addSystemAfter(AppSchedule::update, "Scene.UpdateWindowTitle", "Scene.TickSimulation", [](SystemContext& ctx) {
        if (!ctx.windowFocused)
            return;

        updateWindowTitle(ctx.app);
    });
}

} // namespace caustica
