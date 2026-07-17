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

    const std::shared_ptr<Scene> scene = activeScene(app);
    if (!scene)
        return;

    // Structure mutations from update systems are ECS-only until here.
    syncSceneAccess(app);
    flushPendingStructureGpu(app);

    scene::SessionRenderExtractInputs sessionInputs;
    sessionInputs.camera = &gr->camera();
    sessionInputs.gaussianSplatPasses = &gr->gaussianSplatPasses();
    if (render::WorldRenderer* wr = gr->worldRenderer())
        sessionInputs.gaussianSplatTemporalReset = wr->consumeGaussianSplatTemporalReset();
    sessionInputs.settings = app.tryResource<PathTracerSettings>();
    sessionInputs.runtime = app.tryResource<render::RenderRuntimeState>();
    if (vs)
        sessionInputs.sceneTime = vs->sceneTime;

    scene->extractAndPublishRenderSnapshot(device->getPreparedRenderFrameIndex(), &sessionInputs);
}

void RenderExtractPlugin::configureSchedules(App& app)
{
    app.addSystemAfter(AppSchedule::Extract, "Scene.PrepareRenderFrame", "SetRenderFrameIndex", [](SystemContext& ctx) {
        if (!ctx.gpuDevice || !activeScene(ctx.app))
            return;

        prepareRenderFrame(ctx.app);
    });
}

} // namespace caustica
