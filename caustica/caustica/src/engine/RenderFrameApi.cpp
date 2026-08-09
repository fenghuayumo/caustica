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
#include <unordered_set>


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
                float keyframeDuration = 0.f;
                bool hasImportedAnim = false;
                bool hasEditorKeyframes = false;
                bool hasGeometrySequence = false;
                std::unordered_set<uint64_t> claimedImportedTransformChannels;
                std::unordered_set<uint32_t> activeImportedAnimations;
                for (ecs::Entity animEntity : scene->getAnimationEntities())
                {
                    auto* animation = scene::tryGetAnimation(world, animEntity);
                    if (!animation)
                        continue;
                    const float duration = scene::getAnimationDuration(*animation);
                    if (animation->channels.empty() || !(duration > 0.f))
                        continue;
                    if (animation->editorAuthored)
                    {
                        keyframeDuration = std::max(keyframeDuration, duration);
                        hasEditorKeyframes = hasEditorKeyframes
                            || (!animation->channels.empty() && duration > 0.f);
                    }
                    else
                    {
                        // glTF animations are clips, not layers. Do not evaluate
                        // Idle/Walk/Run simultaneously onto the same joint channel.
                        // Clips whose transform targets are disjoint (for example,
                        // animations from separate imported models) remain active.
                        std::vector<uint64_t> channelKeys;
                        bool conflictsWithActiveClip = false;
                        channelKeys.reserve(animation->channels.size());
                        for (const scene::AnimationChannelData& channel : animation->channels)
                        {
                            if (!ecs::isValid(channel.targetEntity)
                                || (channel.attribute != AnimationAttribute::Translation
                                    && channel.attribute != AnimationAttribute::Rotation
                                    && channel.attribute != AnimationAttribute::Scaling))
                            {
                                continue;
                            }

                            const uint64_t key =
                                (uint64_t(static_cast<uint32_t>(channel.targetEntity)) << 32u)
                                | uint64_t(channel.attribute);
                            channelKeys.push_back(key);
                            conflictsWithActiveClip |= claimedImportedTransformChannels.contains(key);
                        }
                        if (conflictsWithActiveClip)
                            continue;

                        claimedImportedTransformChannels.insert(channelKeys.begin(), channelKeys.end());
                        activeImportedAnimations.insert(static_cast<uint32_t>(animEntity));
                        hasImportedAnim = hasImportedAnim
                            || (!animation->channels.empty() && duration > 0.f);
                    }
                }
                world.each<scene::GeometrySequenceComponent>(
                    [&](ecs::Entity, scene::GeometrySequenceComponent& sequence) {
                        if (!sequence.timesSeconds.empty())
                        {
                            hasGeometrySequence = true;
                        }
                    });

                // Imported/skeletal playback and editor keyframes have independent
                // clocks. SampleSettings.enableAnimations must never move Timeline.
                const bool advanceImportedClock =
                    enableSkeletal && (hasImportedAnim || hasGeometrySequence);
                const bool advanceKeyframeClock = enableKeyframes && hasEditorKeyframes;
                const double previousImportedClock = vs->sceneTime;
                const double previousKeyframeClock = vs->keyframeTime;
                if (advanceImportedClock)
                    vs->sceneTime += fElapsedTimeSeconds;
                if (advanceKeyframeClock)
                    vs->keyframeTime += fElapsedTimeSeconds;

                const auto crossedLoopBoundary = [](double previous, double current, float duration) {
                    if (!(duration > 0.f))
                        return false;
                    if (!std::isfinite(previous) || !std::isfinite(current))
                        return true;
                    const double d = double(duration);
                    return std::floor(previous / d) != std::floor(current / d);
                };
                // A loop wrap/seek (or a long stall) has no meaningful adjacent
                // pose for temporal reprojection. Reset both the realtime filters
                // and the skinned PrevPosition written by the compute pass.
                bool importedLoopBoundary = false;
                if (advanceImportedClock)
                {
                    // Imported clips share a playback clock, but each asset loops at
                    // its own duration. Using the longest clip for every asset makes
                    // short clips clamp at their last keyframe for most of the cycle.
                    for (ecs::Entity animEntity : scene->getAnimationEntities())
                    {
                        if (!activeImportedAnimations.contains(static_cast<uint32_t>(animEntity)))
                            continue;
                        const auto* animation = scene::tryGetAnimation(world, animEntity);
                        if (animation)
                        {
                            importedLoopBoundary |= crossedLoopBoundary(
                                previousImportedClock,
                                vs->sceneTime,
                                scene::getAnimationDuration(*animation));
                        }
                    }
                    world.each<scene::GeometrySequenceComponent>(
                        [&](ecs::Entity, scene::GeometrySequenceComponent& sequence) {
                            if (!sequence.timesSeconds.empty())
                            {
                                importedLoopBoundary |= crossedLoopBoundary(
                                    previousImportedClock,
                                    vs->sceneTime,
                                    sequence.timesSeconds.back());
                            }
                        });
                }

                const bool animationDiscontinuity =
                    importedLoopBoundary
                    || (advanceKeyframeClock && crossedLoopBoundary(
                        previousKeyframeClock, vs->keyframeTime, keyframeDuration))
                    || ((advanceImportedClock || advanceKeyframeClock)
                        && (!std::isfinite(fElapsedTimeSeconds) || fElapsedTimeSeconds < 0.f
                            || fElapsedTimeSeconds > 0.25f));
                if (animationDiscontinuity)
                {
                    ew->resetSkinnedMeshMotionHistory();
                    cfg->ResetAccumulation = true;
                    cfg->ResetRealtimeCaches = true;
                }

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
                        animation->editorAuthored
                            ? enableKeyframes
                            : (enableSkeletal && activeImportedAnimations.contains(
                                static_cast<uint32_t>(animEntity)));
                    if (!applyThis)
                        continue;

                    const float duration = scene::getAnimationDuration(*animation);
                    const float sampleTime = animation->editorAuthored
                        ? keyframeTime
                        : float(fmod(vs->sceneTime, double(duration)));
                    (void)scene::applyAnimation(*animation, sampleTime, *ew);
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

                // SceneRefreshEntityWorld owns the single hierarchy propagation in
                // PostUpdate. Refreshing here as well captures previous=current on
                // the second traversal and destroys animation motion vectors.
                if (!advanceImportedClock && !advanceKeyframeClock)
                    ew->syncPreviousTransformsFromCurrent();

                if (touchedGaussianVisibility)
                    runtime->Invalidation.AccelerationStructRebuildRequested = true;

                // Fixed-topology USD / soft-body point caches (MeshDeformApi hides GPU wiring).
                // Geometry sequences follow imported/skeletal playback, not editor keyframes.
                if (enableSkeletal)
                {
                    const PathTracerSettings* before = cfg;
                    const bool hadResetAccumulation = before && before->ResetAccumulation;
                    world.each<scene::GeometrySequenceComponent>(
                        [&](ecs::Entity entity, scene::GeometrySequenceComponent& sequence) {
                            const float duration = sequence.timesSeconds.empty()
                                ? 0.f
                                : sequence.timesSeconds.back();
                            const float sampleTime = duration > 0.f
                                ? float(fmod(vs->sceneTime, double(duration)))
                                : float(vs->sceneTime);
                            (void)applyGeometrySequence(
                                app,
                                entity,
                                sampleTime,
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
