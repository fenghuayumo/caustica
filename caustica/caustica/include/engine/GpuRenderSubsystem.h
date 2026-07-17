#pragma once

#include <functional>
#include <memory>

#include <engine/EngineSceneCallbacks.h>
#include <render/core/PathTracerSettings.h>
#include <render/AppDiagnostics.h>
#include <render/RenderRuntimeState.h>

struct CommandLineOptions;
class SceneManager;

namespace caustica
{

class GpuDevice;
class SceneTypeFactory;
class AssetSystem;
struct GpuSharedCaches;
struct SessionCamera;
struct SceneSession;

namespace render
{
class WorldRenderer;
}

struct GpuRenderSubsystemInitParams
{
    GpuDevice& gpuDevice;
    AssetSystem& assetSystem;
    GpuSharedCaches& gpuSharedCaches;
    SessionCamera& sessionCamera;
    SceneSession& sceneSession;
    render::WorldRenderer& worldRenderer;
    PathTracerSettings& settings;
    render::RenderRuntimeState& runtimeState;

    double& sceneTime;

    render::AppDiagnostics& diagnostics;

    const CommandLineOptions* cmdLine = nullptr;

    std::shared_ptr<SceneTypeFactory> sceneTypeFactory;
    EngineSceneCallbacks sceneCallbacks = {};
};

// Scene load/unload orchestration only.
// Path-tracing GPU ownership: WorldRenderer
// Shared caches: GpuSharedCaches
// Logic camera: SessionCamera
// SceneManager: SceneSession
// AssetSystem: borrowed for scene asset register/clear; lifecycle owned by AssetPlugin
class GpuRenderSubsystem
{
public:
    GpuRenderSubsystem();
    ~GpuRenderSubsystem();

    GpuRenderSubsystem(const GpuRenderSubsystem&) = delete;
    GpuRenderSubsystem& operator=(const GpuRenderSubsystem&) = delete;

    void shutdown();

    bool initialize(const GpuRenderSubsystemInitParams& params);

    void onSceneUnloading();
    void onSceneLoadedBegin();
    void onSceneLoadedGpuPrep();
    void onSceneLoadedGpuFinish();
    void applyCmdLinePostLoadOverrides();
    void setSceneLoadingCallbacks(std::function<void()> onLoaded, std::function<void()> onUnloading);

private:
    void applySampleSettingsFromScene();
    void registerLoadedSceneAssets();

    [[nodiscard]] ::SceneManager* sceneManager() const;

    GpuSharedCaches* m_gpuSharedCaches = nullptr;
    SessionCamera* m_sessionCamera = nullptr;
    SceneSession* m_sceneSession = nullptr;
    render::WorldRenderer* m_worldRenderer = nullptr;
    GpuDevice* m_gpuDevice = nullptr;
    AssetSystem* m_assetSystem = nullptr;
    PathTracerSettings* m_settings = nullptr;
    render::RenderRuntimeState* m_runtimeState = nullptr;
    render::AppDiagnostics* m_diagnostics = nullptr;
    double* m_sceneTime = nullptr;
    const CommandLineOptions* m_cmdLine = nullptr;
    bool m_shutdown = false;
};

} // namespace caustica
