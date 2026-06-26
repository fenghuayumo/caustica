#pragma once

#include <render/Core/PathTracerSettings.h>
#include <render/Passes/Gaussian/GaussianSplatPass.h>
#include <render/Passes/Gaussian/GaussianSplatEmissionProxy.h>
#include <render/RenderRuntimeState.h>
#include <render/WorldRenderer/WorldRendererServices.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class RenderTargets;
struct GaussianSplatRenderSettings;

namespace caustica
{
class CommonRenderPasses;
class GaussianSplat;
class GpuDevice;
class RenderCore;
class SceneGraphNode;
class ShaderFactory;
} // namespace caustica

namespace caustica::render
{
class PathTracingWorldRenderer;
}

class SceneManager;
struct CommandLineOptions;

namespace caustica::editor
{

// Per-scene Gaussian splat passes, emission proxies, and WorldRenderer hooks.
class SceneGaussianSplatPasses
{
public:
    struct SceneObject
    {
        std::shared_ptr<caustica::GaussianSplat> splat;
        std::weak_ptr<caustica::SceneGraphNode> node;
        std::unique_ptr<GaussianSplatPass> pass;
    };

    [[nodiscard]] bool isAttached() const { return m_worldRenderer != nullptr; }

    void attach(caustica::GpuDevice& gpuDevice,
        SceneManager& sceneManager,
        caustica::RenderCore& renderCore,
        caustica::render::PathTracingWorldRenderer& worldRenderer,
        PathTracerSettings& settings,
        caustica::render::GaussianSplatSceneSummary& summary,
        const std::shared_ptr<caustica::ShaderFactory>& shaderFactory,
        const std::shared_ptr<caustica::CommonRenderPasses>& commonPasses);

    void setOnRequestFullRebuild(std::function<void()> callback);

    void sceneUnloading();
    void onSceneLoaded(const CommandLineOptions& cmdLine);
    bool loadFromFile(const std::filesystem::path& fileName, bool convertRdfToRub = true);
    bool removeObjectsUnderNode(const std::shared_ptr<caustica::SceneGraphNode>& node);

    void preparePasses();
    void buildEmissionProxyList();
    bool isEmissionEnabled() const;
    bool objectsEmpty() const;
    caustica::render::WorldRendererGaussianSplatBinding getPrimaryBinding() const;
    void renderSceneGaussianSplats(nvrhi::ICommandList* commandList,
        const caustica::PlanarView& splatView,
        RenderTargets& renderTargets,
        const GaussianSplatRenderSettings& settings,
        bool& renderedAny);
    void buildAccelStructs(nvrhi::ICommandList* commandList);

    std::vector<GaussianSplatEmissionProxy>& emissionProxies() { return m_emissionProxies; }
    const std::vector<GaussianSplatEmissionProxy>& emissionProxies() const { return m_emissionProxies; }

    uint32_t splatCount() const;
    uint32_t objectCount() const;
    const std::string& fileNameSummary() const { return m_fileNameSummary; }

private:
    std::filesystem::path resolveSplatPath(const caustica::GaussianSplat& splat) const;
    void preparePass(GaussianSplatPass& pass);
    void loadFromSceneGraph();
    bool attachToScene(const std::filesystem::path& fileName, bool convertRdfToRub);
    void updateUIState();
    uint32_t totalSplatCount() const;
    SceneObject* primaryObject();
    const SceneObject* primaryObject() const;
    dm::float4x4 objectToWorld(const SceneObject& object) const;

    caustica::GpuDevice* m_gpuDevice = nullptr;
    SceneManager* m_sceneManager = nullptr;
    caustica::RenderCore* m_renderCore = nullptr;
    caustica::render::PathTracingWorldRenderer* m_worldRenderer = nullptr;
    PathTracerSettings* m_settings = nullptr;
    caustica::render::GaussianSplatSceneSummary* m_summary = nullptr;
    std::shared_ptr<caustica::ShaderFactory> m_shaderFactory;
    std::shared_ptr<caustica::CommonRenderPasses> m_commonPasses;

    std::shared_ptr<GPUSort> m_gpuSort;
    std::vector<SceneObject> m_objects;
    std::vector<GaussianSplatEmissionProxy> m_emissionProxies;
    std::string m_fileNameSummary;
    bool m_initialCmdLineSplatAttached = false;
    std::function<void()> m_onRequestFullRebuild;
};

} // namespace caustica::editor
