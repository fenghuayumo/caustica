#include <engine/App.h>
#include <engine/AppResources.h>
#include <engine/SceneViewState.h>
#include <cassert>
#include <engine/RenderFrameApi.h>
#include <engine/GpuSharedCaches.h>
#include <engine/SceneQuery.h>
#include <engine/SceneLifecycle.h>
#include <engine/RenderSessionApi.h>
#include <engine/SceneApiInternal.h>
#include <engine/RenderThread.h>
#include <assets/AssetSystem.h>
#include <scene/SceneManager.h>
#include <scene/SceneAnimationAccess.h>
#include <scene/SceneEcs.h>
#include <scene/Scene.h>
#include <scene/scene_utils.h>
#include <engine/SceneMeshEditing.h>
#include <render/core/PathTracerSettings.h>
#include <render/passes/postProcess/ToneMappingPasses.h>
#include <render/worldRenderer/WorldRenderer.h>
#include <render/RenderRuntimeState.h>
#include <backend/GpuDevice.h>
#include <core/format.h>
#include <core/log.h>
#include <core/Timer.h>
#include <cmath>
#include <algorithm>
#include <optional>


using namespace caustica::math;
using namespace caustica::render;

const char* g_windowTitle = "caustica";
FPSLimiter g_FPSLimiter;

namespace
{
    void updateFpsInfo(App& app, double frameTimeSeconds)
    {
        SceneViewState* vs = caustica::viewState(app);
        PathTracerSettings* cfg = caustica::settings(app);
        if (!vs || !cfg || frameTimeSeconds <= 0.0)
            return;

#if CAUSTICA_WITH_STREAMLINE
        if (cfg->actualDLSSFGMode() != SI::DLSSGMode::eOff)
        {
            uint32_t presentedFrames = cfg->DLSSFGMultiplier;
            if (presentedFrames == 0)
                presentedFrames = 1u + cfg->DLSSFGNumFramesToGenerate;

            vs->fpsInfo = stringFormat("%.3f ms/%d-frames* (%.1f FPS*) *DLSS-G",
                frameTimeSeconds * 1e3, presentedFrames, presentedFrames / frameTimeSeconds);
            return;
        }
#endif

        vs->fpsInfo = stringFormat("%.3f ms/frame (%.1f FPS)", frameTimeSeconds * 1e3, 1.0 / frameTimeSeconds);
    }

    void recordFrameTiming(App& app, const GpuDevice& gpuDevice)
    {
        SceneViewState* vs = caustica::viewState(app);
        double frameTime = gpuDevice.getAverageFrameTimeSeconds();
        if (frameTime <= 0.0 && vs && vs->lastDeltaTime > 0.0f)
            frameTime = static_cast<double>(vs->lastDeltaTime);
        updateFpsInfo(app, frameTime);
    }

    bool processPendingSceneSwitch(App& app)
    {
        SceneViewState* vs = caustica::viewState(app);
        if (!vs)
            return false;

        std::optional<SceneViewState::PendingSceneSwitch> pending;
        {
            std::lock_guard lock(vs->pendingSceneSwitchMutex);
            pending.swap(vs->pendingSceneSwitch);
        }

        if (!pending)
            return false;

        detail::applySceneSwitch(app, pending->sceneName, pending->forceReload);
        return true;
    }

    bool processHotReloadChanges(App& app)
    {
        AssetSystem* assets = app.tryResource<AssetSystem>();
        ::SceneManager* manager = detail::sessionManager(app);
        if (!assets || !manager || manager->isSceneLoading())
            return false;

        const std::vector<HotReloadChange> changes = assets->pollHotReloadChanges();
        if (changes.empty())
            return false;

        const std::string sceneName = manager->getCurrentSceneName();
        const std::filesystem::path scenePath = manager->getCurrentScenePath();
        if (sceneName.empty() || isInlineScenePath(scenePath))
            return false;

        caustica::info("Hot reload: detected %zu asset source change(s), reloading scene '%s'",
            changes.size(), sceneName.c_str());
        detail::applySceneSwitch(app, sceneName, true);
        return true;
    }

    void tickSceneSwitchTest(App& app)
    {
        const CommandLineOptions* cmd = caustica::cmdLine(app);
        SceneViewState* vs = caustica::viewState(app);
        if (!cmd || !vs || cmd->sceneSwitchTestInterval <= 0)
            return;

        ::SceneManager* manager = detail::sessionManager(app);
        if (!manager)
            return;

        if (--vs->sceneSwitchTestFramesUntilSwitch > 0)
            return;

        vs->sceneSwitchTestFramesUntilSwitch = cmd->sceneSwitchTestInterval;

        const std::vector<std::string>& scenes = caustica::availableScenes(app);
        if (scenes.size() < 2)
            return;

        if (vs->sceneSwitchTestSceneIndex >= scenes.size())
            vs->sceneSwitchTestSceneIndex = 0;

        const std::string& nextScene = scenes[vs->sceneSwitchTestSceneIndex++];
        caustica::info("SceneSwitchTest: requesting '%s' from render thread", nextScene.c_str());
        caustica::setCurrentScene(app, nextScene);

        ++vs->sceneSwitchTestSwitchesDone;
        if (cmd->sceneSwitchTestCount > 0
            && vs->sceneSwitchTestSwitchesDone >= cmd->sceneSwitchTestCount)
        {
            app.requestExit();
        }
    }

    void beginFrame(App& app)
    {
        if (!processPendingSceneSwitch(app))
            processHotReloadChanges(app);
        tickSceneSwitchTest(app);
    }

    void afterWorldRenderDefault(App& app, GpuDevice& /*gpuDevice*/)
    {
        RenderRuntimeState* runtime = caustica::runtimeState(app);
        if (!runtime)
            return;

        const auto* wr = caustica::worldRenderer(app);
        const caustica::render::RenderPickState renderedPick = wr
            ? wr->getLastRenderedPicking()
            : caustica::render::RenderPickState{};
        if (renderedPick.MaterialRequested)
            runtime->Picking.MaterialRequested = false;
        if (renderedPick.InstanceRequested)
            runtime->Picking.InstanceRequested = false;
    }

    void afterWorldRender(App& app, GpuDevice& gpuDevice)
    {
        afterWorldRenderDefault(app, gpuDevice);
    }
}

using namespace caustica::render;

namespace caustica
{

void beginFrameScheduled(App& app)
{
    ::beginFrame(app);
}

void renderScene(App& app, GpuDevice& gpuDevice)
{
    if (shouldSkipRender(app))
        return;

    auto* wr = worldRenderer(app);
    if (!wr)
        return;

    wr->render(gpuDevice.getCurrentFramebuffer(true));
    recordFrameTiming(app, gpuDevice);
}

void afterWorldRenderScheduled(App& app, GpuDevice& gpuDevice)
{
    ::afterWorldRender(app, gpuDevice);
}

void animate(App& app, float fElapsedTimeSeconds)
{
    PathTracerSettings* cfg = settings(app);
    RenderRuntimeState* runtime = runtimeState(app);
    SceneViewState* vs = viewState(app);
    assert(cfg && runtime && vs);

    if (cfg->actualFPSLimiter() > 0)
        fElapsedTimeSeconds = 1.0f / (float)cfg->actualFPSLimiter();

    vs->lastDeltaTime = fElapsedTimeSeconds;

    if (::SceneManager* manager = detail::sessionManager(app))
        manager->updateLoading();

    if (runtime->Invalidation.ShaderAndACRefreshDelayedRequest > 0)
    {
        runtime->Invalidation.ShaderAndACRefreshDelayedRequest -= fElapsedTimeSeconds;
        if (runtime->Invalidation.ShaderAndACRefreshDelayedRequest <= 0)
        {
            runtime->Invalidation.ShaderAndACRefreshDelayedRequest = 0;
            runtime->Invalidation.ShaderReloadRequested = true;
            runtime->Invalidation.AccelerationStructRebuildRequested = true;
        }
    }

    const bool enableAnimations = cfg->EnableAnimations && cfg->RealtimeMode;
    const bool enableAnimationUpdate = enableAnimations || cfg->ResetAccumulation;

    if (auto* wr = worldRenderer(app))
    {
        if (auto* toneMappingPass = wr->getToneMappingPass())
            toneMappingPass->advanceFrame(fElapsedTimeSeconds);
    }

    if (isSceneLoaded(app) && enableAnimationUpdate)
    {
        if (enableAnimations)
            vs->sceneTime += fElapsedTimeSeconds;

        const std::shared_ptr<Scene> scene = activeScene(app);
        if (scene)
        {
            auto* ew = scene->getEntityWorld();
            if (ew)
            {
                auto& world = ew->world();
                float loopDuration = 0.f;
                for (ecs::Entity animEntity : scene->getAnimationEntities())
                {
                    auto* animation = scene::tryGetAnimation(world, animEntity);
                    if (animation)
                        loopDuration = std::max(loopDuration, scene::getAnimationDuration(*animation));
                }
                world.each<scene::GeometrySequenceComponent>(
                    [&](ecs::Entity, scene::GeometrySequenceComponent& sequence) {
                        if (!sequence.timesSeconds.empty())
                            loopDuration = std::max(loopDuration, sequence.timesSeconds.back());
                    });

                const float animTime = (loopDuration > 0.f)
                    ? float(fmod(vs->sceneTime, double(loopDuration)))
                    : float(vs->sceneTime);

                for (ecs::Entity animEntity : scene->getAnimationEntities())
                {
                    auto* animation = scene::tryGetAnimation(world, animEntity);
                    if (!animation || animation->channels.empty())
                        continue;

                    if (scene::getAnimationDuration(*animation) <= 0.0f)
                        continue;

                    (void)scene::applyAnimation(*animation, animTime, *ew);
                }

                if (enableAnimations)
                    ew->refreshHierarchy(scene::PreviousTransformPolicy::CaptureCurrent);

                // Fixed-topology USD / soft-body point caches.
                if (GpuDevice* device = gpuDevice(app))
                {
                    bool temporalResetNeeded = false;
                    SetSceneMeshVerticesParams deformParams;
                    deformParams.device = device->getDevice();
                    if (GpuSharedCaches* caches = gpuSharedCaches(app))
                        deformParams.descriptorTable = caches->descriptorTable.get();
                    if (auto* renderer = worldRenderer(app))
                        deformParams.gpuResources = &renderer->sceneGpuResources();
                    deformParams.scene = scene;
                    deformParams.frameIndex = device->getFrameIndex();
                    deformParams.recomputeNormals = true;
                    deformParams.rebuildAccelerationStructure = true;
                    // Continuous playback must not wipe temporal denoise/TAA history every
                    // source frame ??that reads as whole-scene shimmer. Loop wraps are
                    // handled separately via resetAccumulation when the sample index jumps.
                    deformParams.resetAccumulation = &temporalResetNeeded;
                    deformParams.requestMeshAccelRebuild = [&app](const std::shared_ptr<MeshInfo>& mesh) {
                        requestMeshAccelRebuild(app, mesh, /*resetAccumulation=*/false);
                    };

                    world.each<scene::GeometrySequenceComponent>(
                        [&](ecs::Entity, scene::GeometrySequenceComponent& sequence) {
                            (void)applyGeometrySequence(sequence, animTime, deformParams);
                        });

                    if (temporalResetNeeded)
                    {
                        // Drop NRD/TAA history on loop wrap so the end?start pose jump
                        // does not thrash temporal filters for many frames.
                        cfg->ResetRealtimeCaches = true;
                        cfg->ResetAccumulation = true;
                    }
                }
            }
        }
    }
    else
    {
        vs->sceneTime = 0.0f;
    }

}

void tickSimulationAndFrameTiming(App& app, float fElapsedTimeSeconds)
{
    GpuDevice* device = gpuDevice(app);
    double frameTime = device ? device->getAverageFrameTimeSeconds() : 0.0;
    if (frameTime <= 0.0 && fElapsedTimeSeconds > 0.0f)
        frameTime = static_cast<double>(fElapsedTimeSeconds);
    updateFpsInfo(app, frameTime);
}

void backBufferResizing(App& app)
{
    if (auto* wr = worldRenderer(app))
        wr->onBackBufferResizing();
}

void setSceneTime(App& app, double sceneTime)
{
    assert(viewState(app));
    viewState(app)->sceneTime = sceneTime;
}

double sceneTime(const App& app)
{
    assert(viewState(app));
    return viewState(app)->sceneTime;
}

double& sceneTimeRef(App& app)
{
    assert(viewState(app));
    return viewState(app)->sceneTime;
}

} // namespace caustica
