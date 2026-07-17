#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

#include <rhi/nvrhi.h>
#include <render/core/PathTracerSettings.h>
#include <render/core/CameraController.h>
#include <render/core/AccelStructManager.h>
#include <render/PathTracerScenePasses.h>
#include <render/AppDiagnostics.h>

class SceneManager;
struct CommandLineOptions;

namespace caustica
{

namespace render
{
class RenderDevice;
class WorldRenderer;
} // namespace render

class BindingCache;
class BindlessTable;
class DescriptorTableManager;
class GpuDevice;
class SceneTypeFactory;
class ShaderFactory;
class TextureLoader;
class AssetSystem;
struct RenderInfra;
struct SessionCamera;
struct SceneSession;
struct PathTracingRuntime;

struct EngineSceneCallbacks
{
    std::function<void()> OnSceneLoaded;
    std::function<void()> OnSceneUnloading;
};

struct GpuRenderSubsystemInitParams
{
    GpuDevice& gpuDevice;
    AssetSystem& assetSystem;
    RenderInfra& renderInfra;
    SessionCamera& sessionCamera;
    SceneSession& sceneSession;
    PathTracingRuntime& pathTracingRuntime;
    PathTracerSettings& settings;
    render::RenderRuntimeState& runtimeState;

    double& sceneTime;

    render::AppDiagnostics& diagnostics;

    const CommandLineOptions* cmdLine = nullptr;

    std::shared_ptr<SceneTypeFactory> sceneTypeFactory;
    EngineSceneCallbacks sceneCallbacks = {};
};

// Scene load/unload orchestration. Path-tracing GPU ownership lives on PathTracingRuntime;
// shared caches on RenderInfra; logic camera on SessionCamera; SceneManager on SceneSession.
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
    void refreshEnvironmentMapMediaList(const std::filesystem::path& assetsRoot,
        const std::filesystem::path& scenePath);
    void onSceneLoadedBegin();
    void onSceneLoadedGpuPrep();
    void onSceneLoadedGpuFinish();
    void applyCmdLinePostLoadOverrides();
    void setSceneLoadingCallbacks(std::function<void()> onLoaded, std::function<void()> onUnloading);

    // Temporary facades — prefer renderInfra / sessionCamera / sceneSession / pathTracingRuntime.
    [[nodiscard]] std::shared_ptr<ShaderFactory> shaderFactory() const;
    [[nodiscard]] caustica::render::RenderDevice& renderDevice();
    [[nodiscard]] const caustica::render::RenderDevice& renderDevice() const;
    [[nodiscard]] std::shared_ptr<ShaderFactory>& shaderFactoryRef();
    [[nodiscard]] std::shared_ptr<TextureLoader>& textureLoaderRef();
    [[nodiscard]] std::shared_ptr<DescriptorTableManager>& descriptorTableRef();
    [[nodiscard]] BindingCache* bindingCache() const;
    [[nodiscard]] std::shared_ptr<DescriptorTableManager> descriptorTable() const;
    [[nodiscard]] BindlessTable* bindlessTable() const;
    [[nodiscard]] std::shared_ptr<TextureLoader> textureLoader() const;
    [[nodiscard]] CameraController& camera();
    [[nodiscard]] const CameraController& camera() const;
    [[nodiscard]] AccelStructManager& accelStructs();
    [[nodiscard]] const AccelStructManager& accelStructs() const;
    [[nodiscard]] SceneManager* sceneManager() const;
    [[nodiscard]] render::WorldRenderer* worldRenderer() const;
    [[nodiscard]] nvrhi::BindingLayoutHandle bindlessLayout() const;

    [[nodiscard]] render::SceneLightingPasses& lightingPasses();
    [[nodiscard]] const render::SceneLightingPasses& lightingPasses() const;
    [[nodiscard]] render::SceneRayTracingResources& rayTracingResources();
    [[nodiscard]] const render::SceneRayTracingResources& rayTracingResources() const;
    [[nodiscard]] render::SceneGaussianSplatPasses& gaussianSplatPasses();
    [[nodiscard]] const render::SceneGaussianSplatPasses& gaussianSplatPasses() const;

private:
    void applySampleSettingsFromScene();
    void registerLoadedSceneAssets();

    RenderInfra* m_renderInfra = nullptr;
    SessionCamera* m_sessionCamera = nullptr;
    SceneSession* m_sceneSession = nullptr;
    PathTracingRuntime* m_pathTracing = nullptr;
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
