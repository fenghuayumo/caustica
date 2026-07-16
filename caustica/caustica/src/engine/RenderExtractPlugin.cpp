#include <engine/ScenePlugins.h>

#include <engine/App.h>
#include <engine/AppSchedules.h>
#include <engine/GpuRenderSubsystem.h>
#include <engine/SceneApi.h>
#include <engine/SceneViewState.h>

#include <backend/GpuDevice.h>
#include <render/RenderRuntimeState.h>
#include <render/AppDiagnostics.h>
#include <render/core/PathTracerSettings.h>
#include <render/worldRenderer/WorldRenderer.h>
#include <scene/Scene.h>
#include <scene/SceneManager.h>
#include <scene/SceneRenderData.h>

namespace caustica
{

void prepareRenderFrame(App& app)
{
    auto* vs = app.tryResource<SceneViewState>();
    auto* diag = app.tryResource<render::AppDiagnostics>();
    auto* gr = app.tryResource<GpuRenderSubsystem>();
    GpuDevice* device = app.getGpuDevice();
    if (vs)
        vs->progressLoading.stop();
    if (diag)
        diag->asyncLoadingInProgress = false;

    if (!device || !gr)
        return;

    const std::shared_ptr<Scene> activeScene =
        gr->sceneManager() ? gr->sceneManager()->getScene() : nullptr;
    if (!activeScene)
        return;

    // Structure mutations from update systems are ECS-only until here.
    syncSceneAccess(app);
    flushPendingStructureGpu(app);

    scene::SessionRenderExtractInputs sessionInputs;
    sessionInputs.camera = &gr->camera();
    if (render::WorldRenderer* wr = gr->worldRenderer())
        sessionInputs.gaussianSplatTemporalReset = wr->consumeGaussianSplatTemporalReset();
    sessionInputs.settings = app.tryResource<PathTracerSettings>();
    sessionInputs.runtime = app.tryResource<render::RenderRuntimeState>();
    if (vs)
        sessionInputs.sceneTime = vs->sceneTime;

    activeScene->extractAndPublishRenderSnapshot(device->getPreparedRenderFrameIndex(), &sessionInputs);
}

void registerRenderExtractPlugin(App& app)
{
    app.addSystemAfter(AppSchedule::Extract, "Scene.PrepareRenderFrame", "SetRenderFrameIndex", [](SystemContext& ctx) {
        if (!ctx.gpuDevice)
            return;

        auto* gr = ctx.tryRes<GpuRenderSubsystem>();
        if (!gr || !gr->sceneManager() || !gr->sceneManager()->getScene())
            return;

        prepareRenderFrame(ctx.app);
    });
}

} // namespace caustica
