#pragma once

#include <core/progress.h>
#include <render/core/TextureUtils.h>

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace caustica
{

// Mutable per-session view state (scene time, loading UI, scene switches).
// Interactive camera lives on GpuRenderSubsystem::camera() (see CameraApi / bindSideEffects).
struct SceneViewState
{
    double sceneTime = 0.;
    float lastDeltaTime = 0.0f;

    std::map<Handle<ImageAsset>, TextureCompressionType> uncompressedTextures;

    std::string fpsInfo;

    ProgressBar progressLoading;

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
