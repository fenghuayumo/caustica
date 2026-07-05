#pragma once

#include <ecs/Entity.h>
#include <render/core/PathTracerSettings.h>
#include <render/passes/gaussian/GaussianSplatPass.h>
#include <render/passes/gaussian/GaussianSplatEmissionProxy.h>
#include <render/RenderRuntimeState.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class RenderTargets;
struct GaussianSplatRenderSettings;

namespace caustica
{
class AccelStructManager;
class GaussianSplat;
class GpuDevice;
class ShaderFactory;
namespace render { class RenderDevice; }
} // namespace caustica

namespace caustica::render
{
class WorldRenderer;
struct ScenePassWireParams;
}

class SceneManager;
struct CommandLineOptions;

namespace caustica::render
{

struct GaussianSplatBinding
{
    const GaussianSplatPass* splatPass = nullptr;
    dm::float4x4             objectToWorld = dm::float4x4::identity();
};

// Per-scene Gaussian splat passes, emission proxies, and WorldRenderer hooks.
class SceneGaussianSplatPasses
{
    friend struct PathTracerScenePasses;

public:
    struct SceneObject
    {
        std::shared_ptr<caustica::GaussianSplat> splat;
        ecs::Entity entity = ecs::NullEntity;
        std::unique_ptr<GaussianSplatPass> pass;
    };

    void setOnRequestFullRebuild(std::function<void()> callback);

    void sceneUnloading();
    void onSceneLoaded(const CommandLineOptions& cmdLine);
    bool loadFromFile(const std::filesystem::path& fileName, bool convertRdfToRub = true);
    bool removeObjectsUnderEntity(ecs::Entity rootEntity);

    void preparePasses();
    void buildEmissionProxyList();
    bool isEmissionEnabled() const;
    bool objectsEmpty() const;
    caustica::render::GaussianSplatBinding getPrimaryBinding() const;
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
    void wireSession(const ScenePassWireParams& params);

    std::filesystem::path resolveSplatPath(const caustica::GaussianSplat& splat) const;
    void preparePass(GaussianSplatPass& pass);
    void loadFromSceneEntities();
    bool attachToScene(const std::filesystem::path& fileName, bool convertRdfToRub);
    void updateUIState();
    uint32_t totalSplatCount() const;
    SceneObject* primaryObject();
    const SceneObject* primaryObject() const;
    dm::float4x4 objectToWorld(const SceneObject& object) const;

    caustica::GpuDevice* m_gpuDevice = nullptr;
    SceneManager* m_sceneManager = nullptr;
    caustica::AccelStructManager* m_accelStructs = nullptr;
    caustica::render::WorldRenderer* m_worldRenderer = nullptr;
    PathTracerSettings* m_settings = nullptr;
    caustica::render::GaussianSplatSceneSummary* m_summary = nullptr;
    std::shared_ptr<caustica::ShaderFactory> m_shaderFactory;
    caustica::render::RenderDevice* m_renderDevice = nullptr;

    std::shared_ptr<GPUSort> m_gpuSort;
    std::vector<SceneObject> m_objects;
    std::vector<GaussianSplatEmissionProxy> m_emissionProxies;
    std::string m_fileNameSummary;
    bool m_initialCmdLineSplatAttached = false;
    std::function<void()> m_onRequestFullRebuild;
};

} // namespace caustica::render
