#include "EditorSceneStartup.h"

#include "EditorSystemLabels.h"
#include "EditorUISubsystem.h"
#include "SceneEditor.h"
#include "common/LocalConfig.h"

#include <engine/App.h>
#include <engine/SystemLabels.h>
#include <render/RenderAppState.h>

namespace caustica::editor
{

void registerEditorSceneStartup(caustica::App& app, const EditorSceneStartupConfig& config)
{
    app.addSystemBefore<system_label::EditorScenePreStartup, caustica::system_label::SceneStartup>(
        AppSchedule::Startup,
        [&app, config](SystemContext& ctx) {
            (void)ctx;

            if (config.sceneEditor)
                config.sceneEditor->setApp(app);

            if (config.sceneEditor)
                config.sceneEditor->onBeforeInitialSceneLoad();
        });

    app.addSystemAfter<system_label::EditorScenePostStartup, caustica::system_label::SceneStartup>(
        AppSchedule::Startup,
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
    app.addSystemAfter<system_label::EditorUIStartup, system_label::EditorScenePostStartup>(
        AppSchedule::Startup,
        [&app](SystemContext& ctx) {
            auto* uiSubsystem = ctx.tryRes<EditorUISubsystem>();
            if (!uiSubsystem || !ctx.gpuDevice)
                return;

            uiSubsystem->startup(*ctx.gpuDevice, *app.getWindow(), app);
            app.syncSwapChain();
        });

    // ImGui/UI must tear down before GpuRender destroys device-owned resources.
    app.addSystemBefore<system_label::EditorUIShutdown, caustica::system_label::GpuRenderShutdown>(
        AppSchedule::shutdown,
        [](SystemContext& ctx) {
            if (auto* uiSubsystem = ctx.tryRes<EditorUISubsystem>())
                uiSubsystem->shutdown();
        });
}

} // namespace caustica::editor
