#include <engine/SceneSessionPlugins.h>

#include <engine/App.h>
#include <engine/AppSchedules.h>
#include <engine/SceneViewState.h>
#include <engine/SceneSessionSystems.h>
#include <engine/GpuRenderSubsystem.h>

#include <render/SessionDiagnostics.h>
#include <render/worldRenderer/WorldRenderer.h>
#include <scene/SceneRenderData.h>

namespace caustica::sceneSession
{

void prepareRenderFrame(App& app)
{
    SceneViewState* vs = viewState(app);
    render::SessionDiagnostics* diag = diagnostics(app);
    GpuDevice* device = gpuDevice(app);
    if (vs)
        vs->progressLoading.stop();
    if (diag)
        diag->asyncLoadingInProgress = false;

    if (!device)
        return;

    const std::shared_ptr<Scene> activeScene = scene(app);
    if (!activeScene)
        return;

    scene::SessionRenderExtractInputs sessionInputs;
    if (GpuRenderSubsystem* gr = gpuRender(app))
        sessionInputs.camera = &gr->camera();
    if (render::WorldRenderer* wr = worldRenderer(app))
        sessionInputs.gaussianSplatTemporalReset = wr->consumeGaussianSplatTemporalReset();
    sessionInputs.settings = settings(app);
    sessionInputs.runtime = runtimeState(app);
    if (vs)
        sessionInputs.sceneTime = vs->sceneTime;

    activeScene->extractAndPublishRenderSnapshot(device->getPreparedRenderFrameIndex(), &sessionInputs);
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
