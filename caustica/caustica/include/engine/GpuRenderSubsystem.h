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
#include <render/SessionDiagnostics.h>
#include <render/worldRenderer/PathTracingContext.h>
#include <engine/ISubsystem.h>

class SceneManager;
struct CommandLineOptions;

namespace caustica
{

namespace render
{
class RenderDevice;
}

class BindingCache;
class BindlessTable;
class DescriptorTableManager;
class GpuDevice;
class SceneTypeFactory;
class ShaderFactory;
class TextureLoader;

namespace render
{
class WorldRenderer;
} // namespace render

struct EngineSceneCallbacks
{
    std::function<void()> OnSceneLoaded;
    std::function<void()> OnSceneUnloading;
};

// Inputs for initializing GpuRenderSubsystem (shared GPU infra + path tracer).
struct GpuRenderSubsystemInitParams
{
    GpuDevice& gpuDevice;
    PathTracerSettings& settings;
    render::RenderRuntimeState& runtimeState;

    double& sceneTime;

    render::SessionDiagnostics& diagnostics;

    const CommandLineOptions* cmdLine = nullptr;

    std::shared_ptr<SceneTypeFactory> sceneTypeFactory;
    EngineSceneCallbacks sceneCallbacks = {};
};

// Owns shared GPU infrastructure and the path-tracing WorldRenderer.
class GpuRenderSubsystem : public ISubsystem
{
public:
    GpuRenderSubsystem();
    ~GpuRenderSubsystem() override;

    GpuRenderSubsystem(const GpuRenderSubsystem&) = delete;
    GpuRenderSubsystem& operator=(const GpuRenderSubsystem&) = delete;

    [[nodiscard]] int priority() const override { return 100; }

    void initialize(EngineInitContext& context) override;
    void shutdown() override;

    bool initializeSession(const GpuRenderSubsystemInitParams& params);

    void endFrame();

    void onSceneUnloading();
    void refreshEnvironmentMapMediaList(const std::filesystem::path& assetsRoot,
        const std::filesystem::path& scenePath);
    void onSceneLoadedBegin();
    void onSceneLoadedGpuPrep();
    void onSceneLoadedGpuFinish();
    void applyCmdLinePostLoadOverrides();

    [[nodiscard]] std::shared_ptr<ShaderFactory> shaderFactory() const { return m_shaderFactory; }
    [[nodiscard]] caustica::render::RenderDevice& renderDevice();
    [[nodiscard]] const caustica::render::RenderDevice& renderDevice() const;
    [[nodiscard]] std::shared_ptr<ShaderFactory>& shaderFactoryRef() { return m_shaderFactory; }
    [[nodiscard]] std::shared_ptr<TextureLoader>& textureLoaderRef() { return m_textureCache; }
    [[nodiscard]] std::shared_ptr<DescriptorTableManager>& descriptorTableRef() { return m_descriptorTable; }
    [[nodiscard]] BindingCache* bindingCache() const { return m_bindingCache.get(); }
    [[nodiscard]] std::shared_ptr<DescriptorTableManager> descriptorTable() const { return m_descriptorTable; }
    [[nodiscard]] BindlessTable* bindlessTable() const { return m_bindlessTable.get(); }
    [[nodiscard]] std::shared_ptr<TextureLoader> textureLoader() const { return m_textureCache; }
    [[nodiscard]] CameraController& camera() { return m_camera; }
    [[nodiscard]] const CameraController& camera() const { return m_camera; }
    [[nodiscard]] AccelStructManager& accelStructs() { return m_accelStructs; }
    [[nodiscard]] const AccelStructManager& accelStructs() const { return m_accelStructs; }
    [[nodiscard]] SceneManager* sceneManager() const { return m_sceneManager.get(); }
    [[nodiscard]] render::WorldRenderer* worldRenderer() const { return m_worldRenderer.get(); }
    [[nodiscard]] nvrhi::BindingLayoutHandle bindlessLayout() const { return m_bindlessLayout; }

    [[nodiscard]] render::SceneLightingPasses& lightingPasses() { return m_scenePasses.lighting; }
    [[nodiscard]] const render::SceneLightingPasses& lightingPasses() const { return m_scenePasses.lighting; }
    [[nodiscard]] render::SceneRayTracingResources& rayTracingResources() { return m_scenePasses.rayTracing; }
    [[nodiscard]] const render::SceneRayTracingResources& rayTracingResources() const { return m_scenePasses.rayTracing; }
    [[nodiscard]] render::SceneGaussianSplatPasses& gaussianSplatPasses() { return m_scenePasses.gaussianSplats; }
    [[nodiscard]] const render::SceneGaussianSplatPasses& gaussianSplatPasses() const { return m_scenePasses.gaussianSplats; }

private:
    void createShaderFactory(GpuDevice& gpuDevice);
    void applySampleSettingsFromScene();

    render::PathTracerScenePasses m_scenePasses;
    nvrhi::BindingLayoutHandle m_bindlessLayout;
    std::shared_ptr<ShaderFactory> m_shaderFactory;
    std::unique_ptr<caustica::render::RenderDevice> m_renderDevice;
    std::unique_ptr<BindingCache> m_bindingCache;
    std::unique_ptr<BindlessTable> m_bindlessTable;
    std::shared_ptr<DescriptorTableManager> m_descriptorTable;
    std::shared_ptr<TextureLoader> m_textureCache;
    CameraController m_camera;
    AccelStructManager m_accelStructs;
    std::unique_ptr<SceneManager> m_sceneManager;
    std::unique_ptr<render::PathTracingContext> m_pathTracingContext;
    std::unique_ptr<render::WorldRenderer> m_worldRenderer;

    GpuDevice* m_gpuDevice = nullptr;
    PathTracerSettings* m_settings = nullptr;
    render::RenderRuntimeState* m_runtimeState = nullptr;
    render::SessionDiagnostics* m_diagnostics = nullptr;
    double* m_sceneTime = nullptr;
    const CommandLineOptions* m_cmdLine = nullptr;
};

} // namespace caustica
