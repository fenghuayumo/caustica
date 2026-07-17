#pragma once

#include <math/math.h>

#include <render/core/PathTracerSettings.h>
#include <render/PathTracerScenePasses.h>
#include <render/RenderRuntimeState.h>
#include <render/AppDiagnostics.h>
#include <render/SceneGpuResources.h>
#include <assets/loader/TextureLoader.h>
#include <render/core/DescriptorTableManager.h>
#include <scene/Scene.h>
#include <scene/SceneRenderData.h>

#include <backend/GpuDevice.h>

#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace caustica
{
class AccelStructManager;
class BindingCache;
class CameraController;
class ShaderFactory;

namespace render
{
class RenderDevice;

// Session-scoped references wired once when the path tracer is created.
// Per-frame camera pose/settings come from SceneRenderData (ActiveCameraRenderProxy /
// RenderSettingsSnapshot); camera is the RT frame camera, filled from ActiveCameraRenderProxy
// every frame and never used as the logic-thread free-camera controller.
// Session Scene is bound via WorldRenderer::onSceneLoaded (SceneManager stays on SceneSession).
struct PathTracingContext
{
    GpuDevice& gpuDevice;
    CameraController& camera;
    AccelStructManager& accelStructs;
    PathTracerSettings& settings;
    RenderRuntimeState& runtimeState;
    PathTracerScenePasses& scenePasses;

    std::shared_ptr<ShaderFactory>& shaderFactory;
    caustica::render::RenderDevice& renderDevice;
    BindingCache& bindingCache;
    std::shared_ptr<TextureLoader>& textureCache;
    std::shared_ptr<DescriptorTableManager>& descriptorTable;

    double& sceneTime;

    AppDiagnostics& diagnostics;

    // Render-owned GPU state for the active scene. Scene/ECS never owns GPU handles.
    SceneGpuResources sceneGpuResources;

    // Sole session Scene ownership for the path tracer. Submodules take non-owning
    // Scene* or SceneRenderData / SceneGpuFrameHandles per call — do not copy this.
    // Bound via WorldRenderer::onSceneLoaded; not a substitute for frameScene reads.
    std::shared_ptr<Scene> sessionScene;
    std::filesystem::path sessionScenePath;

    // Per-frame: pointed at SceneRenderData / snapshot copies for the render phase.
    // Valid only between beginGpuReadFrame and endGpuReadFrame in WorldRenderer::render().
    const scene::SceneRenderData* frameScene = nullptr;
    SceneGpuFrameHandles frameGpu{};
    bool frameSceneStructureChanged = false;
    bool frameSceneTransformsChanged = false;
    PathTracerSettings* frameSettings = nullptr;
    RenderRuntimeState* frameRuntime = nullptr;

    [[nodiscard]] bool hasFrameScene() const { return frameScene != nullptr; }
    [[nodiscard]] bool hasSessionScene() const { return sessionScene != nullptr; }

    // Prefer frameGpu; fall back to the render-owned active-scene resources.
    [[nodiscard]] SceneGpuFrameHandles resolveGpuHandles() const
    {
        if (frameGpu.valid())
            return frameGpu;
        return sceneGpuResources.frameHandles();
    }

    [[nodiscard]] std::span<const scene::GaussianSplatRenderProxy> frameGaussianSplats() const
    {
        if (!frameScene)
            return {};
        return frameScene->gaussianSplats;
    }

    [[nodiscard]] std::span<const scene::LightRenderProxy> frameLights() const
    {
        if (!frameScene)
            return {};
        return frameScene->lights;
    }

    [[nodiscard]] PathTracerSettings& activeSettings()
    {
        return frameSettings ? *frameSettings : settings;
    }
    [[nodiscard]] const PathTracerSettings& activeSettings() const
    {
        return frameSettings ? *frameSettings : settings;
    }
    [[nodiscard]] RenderRuntimeState& activeRuntime()
    {
        return frameRuntime ? *frameRuntime : runtimeState;
    }
    [[nodiscard]] const RenderRuntimeState& activeRuntime() const
    {
        return frameRuntime ? *frameRuntime : runtimeState;
    }
};

} // namespace render
} // namespace caustica
