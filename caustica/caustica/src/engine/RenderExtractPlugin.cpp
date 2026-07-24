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
#include <render/WorldRenderer.h>
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

    const std::shared_ptr<Scene> scene = activeScene(app);
    auto endChangeDetection = [&]() {
        if (scene)
        {
            if (scene::SceneEntityWorld* ew = scene->getEntityWorld())
                ew->endChangeDetectionFrame();
        }
    };

    // PostUpdate may have refreshed with the change tick still open. Always close it
    // after the Extract system — even when we cannot publish a snapshot this frame.
    if (!device || !worldRendererResource || !sessionCam)
    {
        endChangeDetection();
        return;
    }

    if (!scene)
        return;

    const bool structureSync = scene->needsGpuStructureSync();
    const bool canStartStructure = structureSync && !scene->structureGpuBuildInFlight();

    // Serve the last committed (TLAS-compatible) packet during build. Only freeze from
    // the pre-edit cache when we have never committed before — never overwrite an
    // existing committed snapshot with newer ECS state that is not AS-ready yet.
    if (canStartStructure && !scene->committedRenderData())
        scene->freezeCommittedFromLogicCache();

    const bool haveCommittedServeTarget =
        !canStartStructure || static_cast<bool>(scene->committedRenderData());

    scene::SessionRenderExtractInputs sessionInputs;
    sessionInputs.camera = &sessionCam->camera;
    sessionInputs.gaussianSplatTemporalReset = worldRendererResource->consumeGaussianSplatTemporalReset();
    sessionInputs.settings = app.tryResource<PathTracerSettings>();
    sessionInputs.runtime = app.tryResource<render::RenderRuntimeState>();
    if (vs)
        sessionInputs.sceneTime = vs->sceneTime;

    // Sole Extract publish for this frame (includes session camera/settings).
    scene->extractAndPublishRenderSnapshot(device->getPreparedRenderFrameIndex(), &sessionInputs);

    if (!canStartStructure)
        return;

    // No prior proxies to serve during build (first structure publish) — exclusive sync.
    if (!haveCommittedServeTarget)
    {
        device->waitForRenderThreadIdle();
        flushPendingStructureGpuSync(app);
        return;
    }

    enqueuePendingStructureGpu(app);
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
