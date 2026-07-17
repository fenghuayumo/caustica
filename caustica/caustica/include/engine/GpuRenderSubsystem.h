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
#include <render/worldRenderer/PathTracingContext.h>

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
    PathTracerSettings& settings;
    render::RenderRuntimeState& runtimeState;

    double& sceneTime;

    render::AppDiagnostics& diagnostics;

    const CommandLineOptions* cmdLine = nullptr;

    std::shared_ptr<SceneTypeFactory> sceneTypeFactory;
    EngineSceneCallbacks sceneCallbacks = {};
};

// Path-tracing runtime: scene passes, accel structs, render-thread camera,
// PathTracingContext, and WorldRenderer. Shared GPU caches live on RenderInfra;
// logic camera on SessionCamera; SceneManager on SceneSession.
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
    [[nodiscard]] AccelStructManager& accelStructs() { return m_accelStructs; }
    [[nodiscard]] const AccelStructManager& accelStructs() const { return m_accelStructs; }
    [[nodiscard]] SceneManager* sceneManager() const;
    [[nodiscard]] render::WorldRenderer* worldRenderer() const { return m_worldRenderer.get(); }
    [[nodiscard]] nvrhi::BindingLayoutHandle bindlessLayout() const;

    [[nodiscard]] render::SceneLightingPasses& lightingPasses() { return m_scenePasses.lighting; }
    [[nodiscard]] const render::SceneLightingPasses& lightingPasses() const { return m_scenePasses.lighting; }
    [[nodiscard]] render::SceneRayTracingResources& rayTracingResources() { return m_scenePasses.rayTracing; }
    [[nodiscard]] const render::SceneRayTracingResources& rayTracingResources() const { return m_scenePasses.rayTracing; }
    [[nodiscard]] render::SceneGaussianSplatPasses& gaussianSplatPasses() { return m_scenePasses.gaussianSplats; }
    [[nodiscard]] const render::SceneGaussianSplatPasses& gaussianSplatPasses() const { return m_scenePasses.gaussianSplats; }

private:
    void applySampleSettingsFromScene();
    void registerLoadedSceneAssets();

    render::PathTracerScenePasses m_scenePasses;
    // Render-thread camera populated from ActiveCameraRenderProxy each frame.
    CameraController m_renderCamera;
    AccelStructManager m_accelStructs;
    std::unique_ptr<render::PathTracingContext> m_pathTracingContext;
    std::unique_ptr<render::WorldRenderer> m_worldRenderer;

    RenderInfra* m_renderInfra = nullptr;
    SessionCamera* m_sessionCamera = nullptr;
    SceneSession* m_sceneSession = nullptr;
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
