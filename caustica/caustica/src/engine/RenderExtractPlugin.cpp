#include <engine/ScenePlugins.h>

#include <engine/App.h>
#include <engine/AppSchedules.h>
#include <engine/RenderExtractScratch.h>
#include <engine/ResolvedActiveCamera.h>
#include <engine/SensorApi.h>
#include <engine/internal/ActiveSceneAccess.h>
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
#include <scene/SceneRenderExtract.h>
namespace caustica
{
namespace
{

void beginExtractFrame(App& app)
{
    RenderExtractScratch* scratchPtr = app.tryResource<RenderExtractScratch>();
    if (!scratchPtr)
        scratchPtr = &app.emplaceResource<RenderExtractScratch>();
    auto& scratch = *scratchPtr;
    scratch = {};

    auto* vs = app.tryResource<SceneViewState>();
    auto* diag = app.tryResource<render::AppDiagnostics>();
    auto* worldRendererResource = worldRenderer(app);
    auto* resolvedCamera = app.tryResource<ResolvedActiveCamera>();
    GpuDevice* device = app.getGpuDevice();

    const bool loadBusy = vs && vs->loadSession.isBusy();
    const bool loadSessionActive = vs && vs->loadSession.isActive();
    if (vs && !loadBusy)
        vs->progressLoading.stop();
    if (vs && !loadSessionActive
        && !(diag && diag->asyncLoadingInProgress.load(std::memory_order_acquire)))
        vs->loadSession.secondaryStreaming.store(false, std::memory_order_relaxed);

    const std::shared_ptr<Scene> scene = activeScene(app);
    auto endChangeDetection = [&]() {
        if (scene)
        {
            if (scene::SceneEntityWorld* ew = scene->getEntityWorld())
                ew->endChangeDetectionFrame();
        }
    };

    if (!device || !worldRendererResource || !resolvedCamera)
    {
        endChangeDetection();
        return;
    }

    if (!scene)
        return;

    const bool structureSync = scene->needsGpuStructureSync();
    scratch.canStartStructure = structureSync && !scene->structureGpuBuildInFlight();

    if (scratch.canStartStructure && !scene->committedRenderData())
        scene->freezeCommittedFromLogicCache();

    scratch.frameIndex = device->getPreparedRenderFrameIndex();
    scratch.frameInputs.activeCamera = &resolvedCamera->camera;
    if (auto* sensorProducts = app.tryResource<RenderProductRegistry>();
        sensorProducts && sensorProducts->pendingPreviousCamera)
    {
        scratch.frameInputs.previousCamera = *sensorProducts->pendingPreviousCamera;
        // The override is scoped to precisely one frozen Extract.
        sensorProducts->pendingPreviousCamera.reset();
    }
    scratch.frameInputs.gaussianSplatTemporalReset =
        worldRendererResource->consumeGaussianSplatTemporalReset();
    scratch.frameInputs.settings = app.tryResource<PathTracerSettings>();
    scratch.frameInputs.runtime = app.tryResource<render::RenderRuntimeState>();
    if (vs)
        scratch.frameInputs.sceneTime = vs->sceneTime;

    scene->extractLogicRenderCache(scratch.frameIndex);
    scratch.active = true;
}

void extractGaussianSplatsSystem(App& app)
{
    auto* scratch = app.tryResource<RenderExtractScratch>();
    if (!scratch || !scratch->active)
        return;

    const std::shared_ptr<Scene> scene = activeScene(app);
    if (!scene)
        return;

    scene::SceneRenderData* cache = scene->logicExtractCache();
    scene::SceneEntityWorld* world = scene->getEntityWorld();
    if (!cache || !world)
        return;

    scene::extractGaussianSplatProxies(*world, *cache);
}

void publishExtractFrame(App& app)
{
    auto* scratch = app.tryResource<RenderExtractScratch>();
    if (!scratch || !scratch->active)
        return;

    const std::shared_ptr<Scene> scene = activeScene(app);
    if (!scene)
    {
        *scratch = {};
        return;
    }

    scene->publishRenderSnapshot(scratch->frameIndex, &scratch->frameInputs);

    const bool canStartStructure = scratch->canStartStructure;
    *scratch = {};

    if (!canStartStructure)
        return;

    enqueuePendingStructureGpu(app);
}

} // namespace

void prepareRenderFrame(App& app)
{
    // Compatibility entry (tests / non-schedule callers): full Extract pipeline.
    beginExtractFrame(app);
    extractGaussianSplatsSystem(app);
    publishExtractFrame(app);
}

void RenderExtractPlugin::build(App& app)
{
    app.emplaceResource<RenderExtractScratch>();
}

void RenderExtractPlugin::configureSchedules(App& app)
{
    const AppSystemOrdering extractSet = AppSystemOrdering{}
        .runAfter<system_label::SetRenderFrameIndex>()
        .inSet<system_set::Extract>();

    app.addSystem<system_label::SceneExtractCore>(
        AppSchedule::Extract,
        [](SystemContext& ctx) {
            if (!ctx.gpuDevice || !activeScene(ctx.app))
                return;
            beginExtractFrame(ctx.app);
        },
        extractSet);

    app.addSystem<system_label::SceneExtractGaussianSplats>(
        AppSchedule::Extract,
        [](SystemContext& ctx) { extractGaussianSplatsSystem(ctx.app); },
        AppSystemOrdering{}
            .runAfter<system_label::SceneExtractCore>()
            .inSet<system_set::Extract>());

    app.addSystem<system_label::ScenePublishRenderSnapshot>(
        AppSchedule::Extract,
        [](SystemContext& ctx) { publishExtractFrame(ctx.app); },
        AppSystemOrdering{}
            .runAfter<system_label::SceneExtractGaussianSplats>()
            .inSet<system_set::Extract>());

    // Legacy label kept as an alias ordering anchor after publish.
    app.addSystem<system_label::ScenePrepareRenderFrame>(
        AppSchedule::Extract,
        [](SystemContext&) {},
        AppSystemOrdering{}
            .runAfter<system_label::ScenePublishRenderSnapshot>()
            .inSet<system_set::Extract>());
}

} // namespace caustica
