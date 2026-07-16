#pragma once

#include <ecs/Entity.h>
#include <render/core/PathTracerSettings.h>
#include <render/passes/gaussian/GaussianSplatPass.h>
#include <render/RenderRuntimeState.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class RenderTargets;
class ShaderDebug;

namespace caustica
{
class GaussianSplat;
class GpuDevice;
class ShaderFactory;
namespace render { class RenderDevice; }
} // namespace caustica

class SceneManager;

namespace caustica::render
{
struct ScenePassWireParams;

// Per-scene Gaussian splat asset ownership and ECS wiring.
class SceneGaussianSplatPasses
{
    friend struct PathTracerScenePasses;

public:
    struct SceneObject
    {
        std::shared_ptr<caustica::GaussianSplat> splat;
        ecs::Entity entity = ecs::NullEntity;
        std::shared_ptr<GaussianSplatPass> pass;
    };

    void setOnRequestFullRebuild(std::function<void()> callback);

    void sceneUnloading();
    void onSceneLoaded();
    bool loadFromFile(const std::filesystem::path& fileName, bool convertRdfToRub = true);
    bool removeObjectsUnderEntity(ecs::Entity rootEntity);

    [[nodiscard]] const std::vector<SceneObject>& objects() const { return m_objects; }
    [[nodiscard]] std::vector<SceneObject>& objects() { return m_objects; }
    [[nodiscard]] bool objectsEmpty() const { return m_objects.empty(); }

    uint32_t splatCount() const;
    uint32_t objectCount() const;
    const std::string& fileNameSummary() const { return m_fileNameSummary; }

private:
    void wireSession(const ScenePassWireParams& params);

    std::filesystem::path resolveSplatPath(const caustica::GaussianSplat& splat) const;
    void loadFromSceneEntities();
    bool attachToScene(const std::filesystem::path& fileName, bool convertRdfToRub);
    void updateUIState();
    void onPassLoaded(GaussianSplatPass& pass);
    uint32_t totalSplatCount() const;

    caustica::GpuDevice* m_gpuDevice = nullptr;
    SceneManager* m_sceneManager = nullptr;
    PathTracerSettings* m_settings = nullptr;
    caustica::render::GaussianSplatSceneSummary* m_summary = nullptr;
    std::shared_ptr<caustica::ShaderFactory> m_shaderFactory;
    caustica::render::RenderDevice* m_renderDevice = nullptr;

    std::function<void()> m_onTemporalReset;
    std::function<RenderTargets*()> m_getRenderTargets;
    std::function<std::shared_ptr<ShaderDebug>()> m_getShaderDebug;

    std::vector<SceneObject> m_objects;
    std::string m_fileNameSummary;
    std::function<void()> m_onRequestFullRebuild;
};

} // namespace caustica::render
