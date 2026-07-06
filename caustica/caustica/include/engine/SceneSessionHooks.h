#pragma once

#include <functional>
#include <string>

namespace caustica
{

class GpuDevice;
class GpuRenderSubsystem;
namespace render
{
class RenderPipelineRegistry;
}

// Optional editor / host hooks invoked by scene session schedule systems.
struct SceneSessionHooks
{
    std::function<void()> onBeginFrameScheduled;
    std::function<void()> onBeforeInitialSceneLoad;
    std::function<void(float& elapsedTimeSeconds)> onAnimateBegin;
    std::function<void(float elapsedTimeSeconds, bool enableAnimations)> onAnimateGameTick;
    std::function<void(float elapsedTimeSeconds, bool enableAnimations, bool enableAnimationUpdate)> onAnimateUpdateSceneTime;
    std::function<void(float elapsedTimeSeconds)> onAnimateGameCamera;
    std::function<void(float elapsedTimeSeconds)> onAnimateEnd;
    std::function<void()> onSceneLoadedEarly;
    std::function<void()> onSceneLoadedBeforeGpuPrep;
    std::function<void()> onSceneLoadedAfterCollectTextures;
    std::function<void()> onSceneLoadedComplete;
    std::function<void()> updateWindowTitle;
    std::function<void(GpuDevice& gpuDevice)> afterWorldRender;
    std::function<bool()> shouldRenderWhenUnfocused;
    std::function<void(GpuRenderSubsystem&)> registerRenderPipelinePlugins;
};

} // namespace caustica
