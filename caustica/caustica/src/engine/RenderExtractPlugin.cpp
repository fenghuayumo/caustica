#include <engine/ScenePlugins.h>

#include <engine/App.h>
#include <engine/AppSchedules.h>
#include <engine/ResolvedActiveCamera.h>
#include <engine/SceneQuery.h>
#include <engine/SceneViewState.h>
#include <engine/SystemLabels.h>
#include <engine/SystemSets.h>
#include <engine/internal/WorldRendererAccess.h>

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
    auto* worldRendererResource = worldRenderer(app);
    auto* resolvedCamera = app.tryResource<ResolvedActiveCamera>();
    GpuDevice* device = app.getGpuDevice();
    // Do not tear down the native loading card while a scene switch is in flight —
    // Extract can resume before onSceneLoaded finishes painting 50→100.
    const bool sceneSwitchInFlight = (vs && vs->sceneGpuSuspended.load(std::memory_order_acquire))
        || isSceneLoading(app);
    if (vs && !sceneSwitchInFlight)
        vs->progressLoading.stop();
    if (diag && !sceneSwitchInFlight)
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
    if (!device || !worldRendererResource || !resolvedCamera)
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

    // Pure frame copy: active camera already resolved after TransformPropagate.
    scene::FrameExtractInputs frameInputs;
    frameInputs.activeCamera = &resolvedCamera->camera;
    frameInputs.gaussianSplatTemporalReset = worldRendererResource->consumeGaussianSplatTemporalReset();
    frameInputs.settings = app.tryResource<PathTracerSettings>();
    frameInputs.runtime = app.tryResource<render::RenderRuntimeState>();
    if (vs)
        frameInputs.sceneTime = vs->sceneTime;

    // Sole Extract publish for this frame (includes active camera/settings).
    scene->extractAndPublishRenderSnapshot(device->getPreparedRenderFrameIndex(), &frameInputs);

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
    app.addSystem<system_label::ScenePrepareRenderFrame>(
        AppSchedule::Extract,
        [](SystemContext& ctx) {
            if (!ctx.gpuDevice || !activeScene(ctx.app))
                return;

            prepareRenderFrame(ctx.app);
        },
        AppSystemOrdering{}
            .runAfter<system_label::SetRenderFrameIndex>()
            .inSet<system_set::Extract>());
}

} // namespace caustica
