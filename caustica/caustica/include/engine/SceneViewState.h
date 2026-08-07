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
    double sceneTime = 0.;
    float lastDeltaTime = 0.0f;

    std::map<Handle<ImageAsset>, TextureCompressionType> uncompressedTextures;

    std::string fpsInfo;

    ProgressBar progressLoading;

    // Exclusive teardown / AS cutover only (ADR 0001 P3). Not held for whole Open Scene.
    // When true, App must not submit/present — overlapping DispatchRays or DLSS-RR with
    // Streamline teardown hard-hangs the GPU.
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
