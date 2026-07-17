#include "EditorSceneStartup.h"

#include "EditorUISubsystem.h"
#include "SceneEditor.h"
#include "common/LocalConfig.h"

#include <engine/App.h>
#include <render/RenderAppState.h>

namespace caustica::editor
{

void registerEditorSceneStartup(caustica::App& app, const EditorSceneStartupConfig& config)
{
    app.addSystemBefore(
        AppSchedule::Startup,
        "EditorScene.PreStartup",
        "Scene.Startup",
        [&app, config](SystemContext& ctx) {
            (void)ctx;

            if (config.sceneEditor)
                config.sceneEditor->setApp(app);

            if (config.sceneEditor)
                config.sceneEditor->onBeforeInitialSceneLoad();
        });

    app.addSystemAfter(
        AppSchedule::Startup,
        "EditorScene.PostStartup",
        "Scene.Startup",
        [&app, config](SystemContext& ctx) {
            (void)ctx;

            if (config.sceneEditor)
            {
                config.sceneEditor->bindSessionCameraSideEffects();
            }

            if (config.appConfig.renderState && config.postAppInit)
                LocalConfig::PostAppInit(*config.appConfig.renderState);
        });
}

void registerEditorUISubsystemLifecycle(caustica::App& app)
{
    app.addSystemAfter(
        AppSchedule::Startup,
        "EditorUI.Startup",
        "EditorScene.PostStartup",
        [&app](SystemContext& ctx) {
            auto* uiSubsystem = ctx.tryRes<EditorUISubsystem>();
            if (!uiSubsystem || !ctx.gpuDevice)
                return;

            uiSubsystem->startup(*ctx.gpuDevice, *app.getWindow(), app);
            app.syncSwapChain();
        });

    // ImGui/UI must tear down before GpuRender destroys device-owned resources.
    app.addSystemBefore(AppSchedule::shutdown, "EditorUI.shutdown", "GpuRender.shutdown", [](SystemContext& ctx) {
        if (auto* uiSubsystem = ctx.tryRes<EditorUISubsystem>())
            uiSubsystem->shutdown();
    });
}

} // namespace caustica::editor
