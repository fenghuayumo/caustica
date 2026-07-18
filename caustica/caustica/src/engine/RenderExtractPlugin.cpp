#include <engine/ScenePlugins.h>

#include <engine/App.h>
#include <engine/AppSchedules.h>
#include <engine/SessionCamera.h>
#include <engine/SceneQuery.h>
#include <engine/SceneViewState.h>
#include <engine/SystemLabels.h>

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
    auto* worldRendererResource = app.tryResource<render::WorldRenderer>();
    auto* sessionCam = app.tryResource<SessionCamera>();
    GpuDevice* device = app.getGpuDevice();
    if (vs)
        vs->progressLoading.stop();
    if (diag)
        diag->asyncLoadingInProgress = false;

    if (!device || !worldRendererResource || !sessionCam)
        return;

    const std::shared_ptr<Scene> scene = activeScene(app);
    if (!scene)
        return;

    const bool structureSync = scene->needsGpuStructureSync();
    if (structureSync)
    {
        // Exclusive access before publishing a new structure into the triple buffer.
        device->waitForRenderThreadIdle();
    }

    scene::SessionRenderExtractInputs sessionInputs;
    sessionInputs.camera = &sessionCam->camera;
    sessionInputs.gaussianSplatTemporalReset = worldRendererResource->consumeGaussianSplatTemporalReset();
    sessionInputs.settings = app.tryResource<PathTracerSettings>();
    sessionInputs.runtime = app.tryResource<render::RenderRuntimeState>();
    if (vs)
        sessionInputs.sceneTime = vs->sceneTime;

    // Sole Extract publish for this frame (includes session camera/settings).
    scene->extractAndPublishRenderSnapshot(device->getPreparedRenderFrameIndex(), &sessionInputs);

    // GPU structure flush consumes the published slot — never a second extract.
    if (structureSync)
        flushPendingStructureGpu(app);
}

void RenderExtractPlugin::configureSchedules(App& app)
{
    app.addSystemAfter<system_label::ScenePrepareRenderFrame, system_label::SetRenderFrameIndex>(
        AppSchedule::Extract,
        [](SystemContext& ctx) {
            if (!ctx.gpuDevice || !activeScene(ctx.app))
                return;

            prepareRenderFrame(ctx.app);
        });
}

} // namespace caustica
