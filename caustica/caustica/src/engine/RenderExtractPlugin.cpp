#include <engine/SceneSessionPlugins.h>

#include <engine/App.h>
#include <engine/AppSchedules.h>
#include <engine/SceneViewState.h>
#include <engine/SceneSessionSystems.h>

#include <render/SessionDiagnostics.h>

namespace caustica::sceneSession
{

void prepareRenderFrame(App& app)
{
    SceneViewState* vs = viewState(app);
    render::SessionDiagnostics* diag = diagnostics(app);
    GpuDevice* device = gpuDevice(app);
    if (vs)
        vs->progressLoading.Stop();
    if (diag)
        diag->asyncLoadingInProgress = false;

    if (!device)
        return;

    const std::shared_ptr<Scene> activeScene = scene(app);
    if (!activeScene)
        return;

    activeScene->extractAndPublishRenderSnapshot(device->GetPreparedRenderFrameIndex());
}

void registerRenderExtractPlugin(App& app)
{
    app.addSystemAfter(AppSchedule::Extract, "SceneSession.PrepareRenderFrame", "SetRenderFrameIndex", [](SystemContext& ctx) {
        if (!ctx.gpuDevice)
            return;

        if (sceneSession::shouldSkipRender(ctx.app))
            return;

        sceneSession::prepareRenderFrame(ctx.app);
    });
}

} // namespace caustica::sceneSession
