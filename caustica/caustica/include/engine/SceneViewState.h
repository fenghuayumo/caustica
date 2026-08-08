#pragma once

#include <core/progress.h>
#include <engine/LoadSession.h>
#include <render/core/TextureUtils.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace caustica
{

namespace scene { class SceneRenderData; }

// Mutable per-session view state (scene time, loading UI, scene switches).
// Interactive camera lives on CameraController App resource (see CameraApi / bindSideEffects).
struct SceneViewState
{
    // Runtime/imported animation clock. Editor-authored keyframes deliberately use
    // a separate clock so enabling scene animations cannot move the editor timeline.
    double sceneTime = 0.;
    double keyframeTime = 0.;
    float lastDeltaTime = 0.0f;

    std::map<Handle<ImageAsset>, TextureCompressionType> uncompressedTextures;

    std::string fpsInfo;

    ProgressBar progressLoading;

    // Exclusive teardown and high-pressure streaming window. When true, App must not
    // submit/present: overlapping DispatchRays/DLSS-RR with teardown or hundreds of
    // scene allocations can remove the device or exhaust system commit.
    std::atomic<bool> sceneGpuSuspended{false};

    // Sole Open Scene load state machine (replaces GpuBindPhase).
    LoadSession loadSession;

    std::mutex pendingSceneSwitchMutex;
    struct PendingSceneSwitch
    {
        std::string sceneName;
        bool forceReload = false;
    };
    std::optional<PendingSceneSwitch> pendingSceneSwitch;

    int sceneSwitchTestFramesUntilSwitch = 0;
    size_t sceneSwitchTestSceneIndex = 0;
    int sceneSwitchTestSwitchesDone = 0;
};

} // namespace caustica
