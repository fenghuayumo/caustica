#pragma once

#include <math/math.h>

#include <render/Core/PathTracerSettings.h>
#include <render/PathTracerScenePasses.h>
#include <render/RenderRuntimeState.h>
#include <render/SessionDiagnostics.h>
#include <assets/loader/TextureLoader.h>
#include <render/Core/DescriptorTableManager.h>
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

namespace rhi
{
class RenderDevice;
}

namespace render
{

// Session-scoped references wired once when the path tracer is created.
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
    rhi::RenderDevice& renderDevice;
    BindingCache& bindingCache;
    std::shared_ptr<TextureLoader>& textureCache;
    std::shared_ptr<DescriptorTableManager>& descriptorTable;

    double& sceneTime;

    SessionDiagnostics& diagnostics;
};

} // namespace render
} // namespace caustica
