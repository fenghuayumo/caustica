#include <engine/App.h>
#include <engine/AppResources.h>
#include <engine/internal/WorldRendererAccess.h>
#include <engine/SceneViewState.h>
#include <cassert>
#include <engine/RenderFrameApi.h>
#include <engine/RenderFramebufferOverride.h>
#include <engine/GpuSharedCaches.h>
#include <engine/internal/ActiveSceneAccess.h>
#include <engine/SceneQuery.h>
#include <engine/SceneLifecycle.h>
#include <engine/RenderSessionApi.h>
#include <engine/internal/SceneApiInternal.h>
#include <engine/RenderThread.h>
#include <assets/AssetSystem.h>
#include <scene/SceneManager.h>
#include <scene/SceneAnimationAccess.h>
#include <scene/SceneEcs.h>
#include <scene/Scene.h>
#include <scene/scene_utils.h>
#include <engine/MeshDeformApi.h>
#include <render/core/PathTracerSettings.h>
#include <render/passes/postProcess/ToneMappingPasses.h>
#include <render/WorldRenderer.h>
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

            // Fixed-width fields avoid ImGui layout flicker in narrow docks.
            vs->fpsInfo = stringFormat("%6.2f ms/%u-frames* (%5.1f FPS*) *DLSS-G",
                frameTimeSeconds * 1e3, presentedFrames, presentedFrames / frameTimeSeconds);
            return;
        }
#endif

        vs->fpsInfo = stringFormat("%6.2f ms/frame (%5.1f FPS)", frameTimeSeconds * 1e3, 1.0 / frameTimeSeconds);
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
        if (!assets || !manager || isSceneLoading(app))
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

    caustica::rhi::Framebuffer* target = gpuDevice.getCurrentFramebuffer(true);
    if (auto* overrideFb = app.tryResource<RenderFramebufferOverride>();
        overrideFb && overrideFb->framebuffer)
    {
        target = overrideFb->framebuffer;
    }

    wr->render(target);
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

    if (runtime->Invalidation.ShaderAndACRefreshDelayedRequest > 0)
    {
        runtime->Invalidation.ShaderAndACRefreshDelayedRequest -= fElapsedTimeSeconds;
        if (runtime->Invalidation.ShaderAndACRefreshDelayedRequest <= 0)
        {
            runtime->Invalidation.ShaderAndACRefreshDelayedRequest = 0;
            // UE-style RT pipeline cache: delayed material/scene edits must not CreateStateObject.
            // Only refresh acceleration structures; SBT remap happens on the frozen-PSO path.
            runtime->Invalidation.AccelerationStructRebuildRequested = true;
        }
    }

    const bool enableSkeletal = cfg->EnableAnimations && cfg->RealtimeMode;
    const bool enableKeyframes = cfg->EnableKeyframes && cfg->RealtimeMode;
    const bool anyPlayback = enableSkeletal || enableKeyframes;
    // Do not treat ResetAccumulation (material/UI edits) as a timeline seek.
    // Scrubbing applies poses via SceneEditor::evaluateAnimationsAt directly.
    const bool enableAnimationUpdate = anyPlayback;

    if (auto* wr = worldRenderer(app))
    {
        if (auto* toneMappingPass = wr->getToneMappingPass())
            toneMappingPass->advanceFrame(fElapsedTimeSeconds);
    }

    if (isSceneLoaded(app) && enableAnimationUpdate)
    {
        const std::shared_ptr<Scene> scene = activeScene(app);
        if (scene)
        {
            auto* ew = scene->getEntityWorld();
            if (ew)
            {
                auto& world = ew->world();
                float importedDuration = 0.f;
                float keyframeDuration = 0.f;
                bool hasImportedAnim = false;
                bool hasEditorKeyframes = false;
                bool hasGeometrySequence = false;
                for (ecs::Entity animEntity : scene->getAnimationEntities())
                {
                    auto* animation = scene::tryGetAnimation(world, animEntity);
                    if (!animation)
                        continue;
                    const float duration = scene::getAnimationDuration(*animation);
                    if (animation->editorAuthored)
                    {
                        keyframeDuration = std::max(keyframeDuration, duration);
                        hasEditorKeyframes = hasEditorKeyframes
                            || (!animation->channels.empty() && duration > 0.f);
                    }
                    else
                    {
                        importedDuration = std::max(importedDuration, duration);
                        hasImportedAnim = hasImportedAnim
                            || (!animation->channels.empty() && duration > 0.f);
                    }
                }
                world.each<scene::GeometrySequenceComponent>(
                    [&](ecs::Entity, scene::GeometrySequenceComponent& sequence) {
                        if (!sequence.timesSeconds.empty())
                        {
                            hasGeometrySequence = true;
                            importedDuration =
                                std::max(importedDuration, sequence.timesSeconds.back());
                        }
                    });

                // Imported/skeletal playback and editor keyframes have independent
                // clocks. SampleSettings.enableAnimations must never move Timeline.
                const bool advanceImportedClock =
                    enableSkeletal && (hasImportedAnim || hasGeometrySequence);
                const bool advanceKeyframeClock = enableKeyframes && hasEditorKeyframes;
                if (advanceImportedClock)
                    vs->sceneTime += fElapsedTimeSeconds;
                if (advanceKeyframeClock)
                    vs->keyframeTime += fElapsedTimeSeconds;

                const float importedTime = (importedDuration > 0.f)
                    ? float(fmod(vs->sceneTime, double(importedDuration)))
                    : float(vs->sceneTime);
                const float keyframeTime = (keyframeDuration > 0.f)
                    ? float(fmod(vs->keyframeTime, double(keyframeDuration)))
                    : float(vs->keyframeTime);

                bool touchedGaussianVisibility = false;
                for (ecs::Entity animEntity : scene->getAnimationEntities())
                {
                    auto* animation = scene::tryGetAnimation(world, animEntity);
                    if (!animation || animation->channels.empty())
                        continue;

                    if (scene::getAnimationDuration(*animation) <= 0.0f)
                        continue;

                    const bool applyThis =
                        animation->editorAuthored ? enableKeyframes : enableSkeletal;
                    if (!applyThis)
                        continue;

                    (void)scene::applyAnimation(
                        *animation,
                        animation->editorAuthored ? keyframeTime : importedTime,
                        *ew);
                    for (const auto& channel : animation->channels)
                    {
                        if (channel.attribute != AnimationAttribute::Visibility)
                            continue;
                        if (!ecs::isValid(channel.targetEntity))
                            continue;
                        if (world.tryGet<scene::GaussianSplatComponent>(channel.targetEntity))
                            touchedGaussianVisibility = true;
                    }
                }

                if (advanceImportedClock || advanceKeyframeClock)
                {
                    ew->refreshHierarchy(scene::PreviousTransformPolicy::CaptureCurrent);
                }
                else
                {
                    // Playback gated but neither clock is advancing: keep
                    // previous==current so temporal filters stay quiet.
                    ew->refreshHierarchy(scene::PreviousTransformPolicy::PreserveExisting);
                    ew->syncPreviousTransformsFromCurrent();
                }

                if (touchedGaussianVisibility)
                    runtime->Invalidation.AccelerationStructRebuildRequested = true;

                // Fixed-topology USD / soft-body point caches (MeshDeformApi hides GPU wiring).
                // Geometry sequences follow imported/skeletal playback, not editor keyframes.
                if (enableSkeletal)
                {
                    const PathTracerSettings* before = cfg;
                    const bool hadResetAccumulation = before && before->ResetAccumulation;
                    world.each<scene::GeometrySequenceComponent>(
                        [&](ecs::Entity entity, scene::GeometrySequenceComponent&) {
                            (void)applyGeometrySequence(
                                app,
                                entity,
                                importedTime,
                                MeshDeformOptions{ .resetAccumulationOnAccelRebuild = false });
                        });
                    // Loop wraps may request accumulation reset via mesh-edit internals.
                    if (cfg && cfg->ResetAccumulation && !hadResetAccumulation)
                        cfg->ResetRealtimeCaches = true;
                }
            }
        }
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
