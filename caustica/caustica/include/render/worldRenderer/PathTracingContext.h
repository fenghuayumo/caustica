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
#include <scene/SceneManager.h>
#include <scene/SceneRenderData.h>

#include <backend/GpuDevice.h>

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
struct PathTracingContext
{
    GpuDevice& gpuDevice;
    SceneManager& sceneManager;
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

    // Per-frame: pointed at SceneRenderData / snapshot copies for the render phase.
    // Valid only between beginGpuReadFrame and endGpuReadFrame in WorldRenderer::render().
    const scene::SceneRenderData* frameScene = nullptr;
    SceneGpuFrameHandles frameGpu{};
    bool frameSceneStructureChanged = false;
    bool frameSceneTransformsChanged = false;
    PathTracerSettings* frameSettings = nullptr;
    RenderRuntimeState* frameRuntime = nullptr;

    [[nodiscard]] bool hasFrameScene() const { return frameScene != nullptr; }

    // Prefer frameGpu; fall back to live SceneGpuResources when rebuilding outside render().
    [[nodiscard]] SceneGpuFrameHandles resolveGpuHandles() const
    {
        if (frameGpu.valid())
            return frameGpu;
        if (const auto& scene = sceneManager.getScene())
            return scene->getGpuResources().frameHandles();
        return {};
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
