#pragma once

#include <render/core/PathTracerSettings.h>
#include <render/AppDiagnostics.h>
#include <render/RenderRuntimeState.h>

namespace caustica
{

class GpuDevice;
class AssetSystem;
struct GpuSharedCaches;
struct SceneSession;

namespace render
{
class WorldRenderer;
}
namespace scene { class SceneRenderData; }

// Wire already-created App resources for scene load/unload GPU orchestration.
// Bootstrap (caches / SceneSession / WorldRenderer create): SceneStartup
// Sample settings / hierarchy / cmdline overrides / asset register: SceneLifecycle / AssetSystem
struct gpuRenderSubsystemInitParams
{
    GpuDevice& gpuDevice;
    AssetSystem& assetSystem;
    GpuSharedCaches& gpuSharedCaches;
    SceneSession& sceneSession;
    render::WorldRenderer& worldRenderer;
    PathTracerSettings& settings;
    render::RenderRuntimeState& runtimeState;
    render::AppDiagnostics& diagnostics;
};

class GpuRenderSubsystem
{
public:
    GpuRenderSubsystem();
    ~GpuRenderSubsystem();

    GpuRenderSubsystem(const GpuRenderSubsystem&) = delete;
    GpuRenderSubsystem& operator=(const GpuRenderSubsystem&) = delete;

    void shutdown();

    // Store pointers only; callers must create GpuSharedCaches / SceneSession / WorldRenderer first.
    bool initialize(const gpuRenderSubsystemInitParams& params);

    void onSceneUnloading();
    void onSceneLoadedGpuPrep(const scene::SceneRenderData& renderData);
    void onSceneLoadedGpuFinish(const scene::SceneRenderData& renderData);

private:
    GpuSharedCaches* m_gpuSharedCaches = nullptr;
    SceneSession* m_sceneSession = nullptr;
    render::WorldRenderer* m_worldRenderer = nullptr;
    GpuDevice* m_gpuDevice = nullptr;
    AssetSystem* m_assetSystem = nullptr;
    PathTracerSettings* m_settings = nullptr;
    render::RenderRuntimeState* m_runtimeState = nullptr;
    render::AppDiagnostics* m_diagnostics = nullptr;
    bool m_shutdown = false;
};

} // namespace caustica
