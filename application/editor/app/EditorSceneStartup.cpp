#include "EditorSceneStartup.h"

#include "EditorUISubsystem.h"
#include "SceneEditor.h"
#include "common/LocalConfig.h"

#include <engine/App.h>
#include <engine/GpuRenderSubsystem.h>
#include <render/RenderSessionState.h>

namespace caustica::editor
{

void registerEditorSceneStartup(caustica::App& app, const EditorSceneStartupConfig& config)
{
    app.addSystemBefore(
        AppSchedule::Startup,
        "EditorScene.PreStartup",
        "SceneSession.Startup",
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
        "SceneSession.Startup",
        [&app, config](SystemContext& ctx) {
            (void)ctx;

            if (config.sceneEditor)
            {
                if (auto* gpuRender = ctx.tryRes<caustica::GpuRenderSubsystem>())
                    config.sceneEditor->attachGpuRenderSubsystem(*gpuRender);
            }

            if (config.session.sessionState && config.postAppInit)
                LocalConfig::PostAppInit(*config.session.sessionState);
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

    app.addSystem(AppSchedule::shutdown, "EditorUI.shutdown", [](SystemContext& ctx) {
        if (auto* uiSubsystem = ctx.tryRes<EditorUISubsystem>())
            uiSubsystem->shutdown();
    });
}

} // namespace caustica::editor
