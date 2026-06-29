#pragma once

#include <functional>
#include <memory>
#include <span>
#include <vector>

#include <rhi/nvrhi.h>
#include <render/Core/PathTracerSettings.h>
#include <render/SessionDiagnostics.h>
#include <render/WorldRenderer/PathTracingContext.h>
#include <render/WorldRenderer/PathTracingFrameExtension.h>

class SceneManager;

namespace caustica
{

class BindingCache;
class BindlessTable;
class CommonRenderPasses;
class DescriptorTableManager;
class GpuDevice;
class RenderCore;
class SceneTypeFactory;
class ShaderFactory;
class TextureLoader;

namespace render
{
class PathTracingWorldRenderer;
class SceneGaussianSplatPasses;
class SceneLightingPasses;
class SceneRayTracingResources;
} // namespace render

struct EngineSceneCallbacks
{
    std::function<void()> OnSceneLoaded;
    std::function<void()> OnSceneUnloading;
};

// Per-session inputs for creating the path tracer.
struct PathTracerSessionParams
{
    GpuDevice& gpuDevice;
    PathTracerSettings& settings;
    render::RenderRuntimeState& runtimeState;

    render::SceneRayTracingResources& rayTracing;
    render::SceneGaussianSplatPasses& gaussianSplats;
    render::SceneLightingPasses& lighting;

    double& sceneTime;
    std::vector<GaussianSplatEmissionProxy>& gaussianSplatEmissionProxies;

    render::SessionDiagnostics& diagnostics;

    std::span<render::IPathTracingFrameExtension* const> frameExtensions = {};
};

// Owns shared GPU infrastructure and the path-tracing world renderer.
class EngineRenderer
{
public:
    EngineRenderer();
    ~EngineRenderer();

    EngineRenderer(const EngineRenderer&) = delete;
    EngineRenderer& operator=(const EngineRenderer&) = delete;

    bool initialize(GpuDevice& gpuDevice,
        std::shared_ptr<SceneTypeFactory> sceneTypeFactory,
        EngineSceneCallbacks sceneCallbacks = {});

    void createPathTracer(const PathTracerSessionParams& session);

    void shutdown();
    void endFrame();

    [[nodiscard]] std::shared_ptr<ShaderFactory> shaderFactory() const { return m_shaderFactory; }
    [[nodiscard]] std::shared_ptr<CommonRenderPasses> commonPasses() const { return m_commonPasses; }
    [[nodiscard]] std::shared_ptr<ShaderFactory>& shaderFactoryRef() { return m_shaderFactory; }
    [[nodiscard]] std::shared_ptr<CommonRenderPasses>& commonPassesRef() { return m_commonPasses; }
    [[nodiscard]] std::shared_ptr<TextureLoader>& textureLoaderRef() { return m_textureCache; }
    [[nodiscard]] std::shared_ptr<DescriptorTableManager>& descriptorTableRef() { return m_descriptorTable; }
    [[nodiscard]] BindingCache* bindingCache() const { return m_bindingCache.get(); }
    [[nodiscard]] std::shared_ptr<DescriptorTableManager> descriptorTable() const { return m_descriptorTable; }
    [[nodiscard]] BindlessTable* bindlessTable() const { return m_bindlessTable.get(); }
    [[nodiscard]] std::shared_ptr<TextureLoader> textureLoader() const { return m_textureCache; }
    [[nodiscard]] RenderCore* renderCore() const { return m_renderCore.get(); }
    [[nodiscard]] SceneManager* sceneManager() const { return m_sceneManager.get(); }
    [[nodiscard]] render::PathTracingWorldRenderer* worldRenderer() const { return m_worldRenderer.get(); }
    [[nodiscard]] nvrhi::BindingLayoutHandle bindlessLayout() const { return m_bindlessLayout; }

private:
    void createShaderFactory(GpuDevice& gpuDevice);
    void attachScenePasses(const PathTracerSessionParams& session);

    nvrhi::BindingLayoutHandle m_bindlessLayout;
    std::shared_ptr<ShaderFactory> m_shaderFactory;
    std::shared_ptr<CommonRenderPasses> m_commonPasses;
    std::unique_ptr<BindingCache> m_bindingCache;
    std::unique_ptr<BindlessTable> m_bindlessTable;
    std::shared_ptr<DescriptorTableManager> m_descriptorTable;
    std::shared_ptr<TextureLoader> m_textureCache;
    std::unique_ptr<RenderCore> m_renderCore;
    std::unique_ptr<SceneManager> m_sceneManager;
    std::unique_ptr<render::PathTracingContext> m_pathTracingContext;
    std::unique_ptr<render::PathTracingWorldRenderer> m_worldRenderer;
};

} // namespace caustica
