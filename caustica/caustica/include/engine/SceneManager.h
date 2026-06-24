#pragma once

#include <memory>
#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <scene/Scene.h>
#include <scene/SceneGraph.h>

namespace caustica
{
class GpuDevice;
class ShaderFactory;
class TextureCache;
class DescriptorTableManager;
class Scene;
class SceneGraphNode;
class Material;
} // namespace caustica

// =============================================================================
// SceneManager — Engine layer: scene discovery, queries, env map listing.
//
// All methods that need scene data take it as parameters — SceneManager does
// NOT own the scene.  This keeps it safe to use before/during scene loading.
// Methods are inline to avoid cross-library linker issues during migration.
// =============================================================================
class SceneManager
{
public:
    SceneManager(caustica::GpuDevice&                     device,
                 caustica::ShaderFactory&                 shaderFactory,
                 std::shared_ptr<caustica::TextureCache>  textureCache,
                 std::shared_ptr<caustica::DescriptorTableManager> descriptorTable);

    ~SceneManager();

    // --- Scene discovery ---
    void discoverAvailableScenes(const std::filesystem::path& assetsPath);
    const std::vector<std::string>& getAvailableScenes() const { return m_sceneFilesAvailable; }

    // --- Scene queries (static, take scene as parameter) ---
    static std::shared_ptr<caustica::Material> findMaterial(
        const std::shared_ptr<caustica::Scene>& scene, int materialID)
    {
        if (!scene) return nullptr;
        for (const auto& mat : scene->GetSceneGraph()->GetMaterials())
            if (mat->materialID == materialID) return mat;
        return nullptr;
    }

    static std::shared_ptr<caustica::SceneGraphNode> findNodeByInstanceIndex(
        const std::shared_ptr<caustica::Scene>& scene, int instanceIndex)
    {
        if (!scene || instanceIndex < 0) return nullptr;
        const auto& instances = scene->GetSceneGraph()->GetMeshInstances();
        if (instanceIndex >= static_cast<int>(instances.size())) return nullptr;
        return instances[instanceIndex]->GetNodeSharedPtr();
    }

    // --- Environment map listing ---
    static void refreshEnvironmentMapMediaList(
        const std::filesystem::path&              assetsPath,
        const std::filesystem::path&              envMapSubFolder,
        const std::filesystem::path&              currentScenePath,
        std::vector<std::filesystem::path>&       outMediaList,
        std::filesystem::path&                    outMediaFolder);

    // --- Dependencies ---
    caustica::GpuDevice&                      getDevice()        { return m_device; }
    caustica::ShaderFactory&                  getShaderFactory()  { return m_shaderFactory; }
    std::shared_ptr<caustica::TextureCache>   getTextureCache()   { return m_textureCache; }
    std::shared_ptr<caustica::DescriptorTableManager> getDescriptorTable() { return m_descriptorTable; }

private:
    caustica::GpuDevice&                      m_device;
    caustica::ShaderFactory&                  m_shaderFactory;
    std::shared_ptr<caustica::TextureCache>   m_textureCache;
    std::shared_ptr<caustica::DescriptorTableManager> m_descriptorTable;
    std::vector<std::string>                  m_sceneFilesAvailable;
};
