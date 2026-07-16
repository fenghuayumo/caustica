#pragma once

#include <math/math.h>

#include <render/core/PathTracerSettings.h>
#include <render/PathTracerScenePasses.h>
#include <render/RenderRuntimeState.h>
#include <render/AppDiagnostics.h>
#include <assets/loader/TextureLoader.h>
#include <render/core/DescriptorTableManager.h>
#include <scene/SceneManager.h>

#include <backend/GpuDevice.h>

#include <memory>
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
// RenderSettingsSnapshot); CameraController here holds RT-owned views updated from that proxy.
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

    // Per-frame: pointed at SceneRenderData snapshot copies for the render phase.
    PathTracerSettings* frameSettings = nullptr;
    RenderRuntimeState* frameRuntime = nullptr;

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
