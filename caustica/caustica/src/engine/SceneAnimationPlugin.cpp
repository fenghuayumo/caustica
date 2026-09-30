#include <engine/ScenePlugins.h>

#include <engine/App.h>
#include <engine/AppResources.h>
#include <engine/AppSchedules.h>
#include <engine/internal/SceneApiInternal.h>
#include <engine/MeshDeformApi.h>
#include <engine/RenderFrameApi.h>
#include <engine/SceneLifecycle.h>
#include <engine/internal/ActiveSceneAccess.h>
#include <engine/SceneQuery.h>
#include <engine/SceneViewState.h>
#include <engine/SystemLabels.h>
#include <engine/SystemSets.h>
#include <engine/Time.h>

#include <render/core/PathTracerSettings.h>
#include <render/RenderRuntimeState.h>

#include <scene/Scene.h>
#include <scene/SceneAnimationAccess.h>
#include <scene/SceneEcs.h>
#include <scene/SceneManager.h>
#include <scene/scene_utils.h>

#include <math/math.h>

#include <cassert>
#include <cmath>
#include <unordered_set>
#include <vector>

using namespace caustica::math;
using namespace caustica::render;

namespace caustica
{
namespace
{

bool isSpatialAnimationChannel(AnimationAttribute attribute)
{
    return attribute == AnimationAttribute::Translation
        || attribute == AnimationAttribute::Rotation
        || attribute == AnimationAttribute::Scaling;
}

bool loopEndpointsMatch(const scene::AnimationChannelData& channel)
{
    if (!channel.sampler)
        return true;

    const auto& keys = channel.sampler->getKeyframes();
    if (keys.size() < 2)
        return true;

    const math::float4 a = keys.front().value;
    const math::float4 b = keys.back().value;
    if (channel.attribute == AnimationAttribute::Rotation)
    {
        const float aLength2 = dot(a, a);
        const float bLength2 = dot(b, b);
        if (!(aLength2 > 0.f) || !(bLength2 > 0.f))
            return false;

        // q and -q encode the same rotation and therefore form a continuous loop.
        const float normalizedDot = std::abs(dot(a, b)) / std::sqrt(aLength2 * bLength2);
        return normalizedDot >= 1.f - 1e-5f;
    }

    const math::float4 delta = abs(a - b);
    const math::float4 scale = max(max(abs(a), abs(b)), math::float4(1.f));
    return all(delta <= scale * 1e-5f);
}

bool hasDiscontinuousSpatialLoop(const scene::AnimationComponent& animation)
{
    for (const scene::AnimationChannelData& channel : animation.channels)
    {
        if (isSpatialAnimationChannel(channel.attribute) && !loopEndpointsMatch(channel))
            return true;
    }
    return false;
}

} // namespace

void animate(App& app, float fElapsedTimeSeconds)
{
    PathTracerSettings* cfg = settings(app);
    RenderRuntimeState* runtime = runtimeState(app);
    SceneViewState* vs = viewState(app);
    assert(cfg && runtime && vs);

    // Simulation step cap. Presentation pacing sleeps on the render thread.
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
                // clocks. SceneSettings.enableAnimations must never move Timeline.
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
                // pose for skinned PrevPosition. Keep this invalidation local:
                // disocclusion handles animated objects, while clearing NRD/TAA/
                // ReSTIR here would flash the entire frame for a local animation.
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
                        if (animation && hasDiscontinuousSpatialLoop(*animation))
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
                    ew->resetSkinnedMeshMotionHistory();

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

void tickSimulationAndFrameTiming(
    SceneViewState& viewState,
    const PathTracerSettings& settings,
    const Time& time)
{
    const double frameTime = time.averageFrameSeconds > 0.0
        ? time.averageFrameSeconds
        : static_cast<double>(time.deltaSeconds);
    updateFpsInfo(viewState, settings, frameTime);
}

void refreshEntityWorld(App& app, uint32_t frameIndex)
{
    const std::shared_ptr<Scene> scene = activeScene(app);
    if (!scene)
        return;

    scene->refreshEntityWorldForFrame(frameIndex);
}

void SceneAnimationPlugin::configureSchedules(App& app)
{
    // Join CPU import + advance LoadSession (present continues during GpuStreaming).
    app.addSystem<system_label::SceneAnimate>(
        AppSchedule::update,
        [](SystemContext& ctx) {
            if (::SceneManager* manager = detail::sessionManager(ctx.app))
                manager->updateLoading();
            tickLoadSession(ctx.app);

            // Keep the animation clock one-to-one with submitted render frames.
            // During scene streaming/skip-render gaps, windowFocused may remain
            // true solely to pump loading work.
            if (!ctx.runRender)
                return;

            animate(ctx.app, ctx.deltaTimeSeconds);
        },
        AppSystemOrdering{}.inSet<system_set::Simulation>());

    app.addSystem<system_label::SceneRefreshEntityWorld>(
        AppSchedule::PostUpdate,
        [](SystemContext& ctx) {
            refreshEntityWorld(ctx.app, ctx.frameIndex);
        },
        AppSystemOrdering{}.inSet<system_set::TransformPropagate>());

    app.addSystemAfter<system_label::SceneTickSimulation, system_label::SceneUpdateCamera>(
        AppSchedule::update,
        [](Res<Time> time,
           ResMut<SceneViewState> viewState,
           Res<PathTracerSettings> settings) {
            if (!time->simulationActive)
                return;

            tickSimulationAndFrameTiming(*viewState, *settings, *time);
        });
}

} // namespace caustica
