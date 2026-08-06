#pragma once

#include <core/progress.h>
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

    // When true, App must not submit/present. Scene switch tears down AS / materials /
    // Streamline history; overlapping DispatchRays or DLSS-RR hard-hangs the GPU.
    std::atomic<bool> sceneGpuSuspended{false};

    // UE-style multi-frame GPU bind after CPU import. Advanced by tickSceneGpuBind().
    enum class GpuBindPhase : uint8_t
    {
        None = 0,
        Textures,
        World,
        Meshes,
        Finalize,
        LogicFinish,
    };
    struct GpuBindJob
    {
        GpuBindPhase phase = GpuBindPhase::None;
        const scene::SceneRenderData* renderData = nullptr;
        size_t texturesTotal = 0;
        size_t meshBegin = 0;
        size_t meshTotal = 0;
    };
    GpuBindJob gpuBind;

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
