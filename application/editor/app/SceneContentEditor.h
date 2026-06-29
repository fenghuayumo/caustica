#pragma once

#include <math/math.h>
#include <rhi/nvrhi.h>
#include <ecs/Entity.h>

#include <assets/RuntimeMeshLoadTypes.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace caustica
{
class TextureLoader;
struct MeshInfo;
}

class SceneManager;
struct PathTracerSettings;

namespace caustica::render
{
class SceneGaussianSplatPasses;
class SceneLightingPasses;
class SceneRayTracingResources;
}

namespace caustica::editor
{
class EditorUIState;

// Runtime mesh import, drag-drop handling, and scene-graph mesh editing.
class SceneContentEditor
{
public:
    struct Context
    {
        SceneManager* sceneManager = nullptr;
        caustica::TextureLoader* textureLoader = nullptr;
        EditorUIState* editor = nullptr;
        PathTracerSettings* settings = nullptr;
        caustica::render::SceneLightingPasses* lightingPasses = nullptr;
        caustica::render::SceneRayTracingResources* rayTracingResources = nullptr;
        caustica::render::SceneGaussianSplatPasses* gaussianSplatPasses = nullptr;
        std::function<uint32_t()> frameIndex;
        std::function<nvrhi::IDevice*()> device;
        std::function<bool(const std::filesystem::path&)> loadGaussianSplat;
        std::function<uint32_t()> gaussianSplatCount;
        std::function<uint32_t()> gaussianSplatObjectCount;
    };

    explicit SceneContentEditor(Context context);

    void updateContext(Context context);

    void handleDroppedFiles(std::vector<std::string>& pendingFiles);

    bool loadMeshFile(const std::filesystem::path& filePath);
    bool loadGltfMeshFile(const std::filesystem::path& filePath);
    bool loadObjMeshFile(const std::filesystem::path& filePath);
    void finalizeRuntimeSceneMutation(caustica::ecs::Entity importedRoot);
    bool deleteSceneNode(caustica::ecs::Entity entity);
    void requestFullRebuild();

    std::vector<caustica::math::float3> getMeshVertices(const std::shared_ptr<caustica::MeshInfo>& mesh) const;
    std::vector<caustica::math::float3> getMeshVerticesWorld(const std::shared_ptr<caustica::MeshInfo>& mesh) const;
    std::vector<caustica::math::float3> getMeshVerticesWorld(caustica::ecs::Entity entity) const;
    void setMeshVertices(const std::shared_ptr<caustica::MeshInfo>& mesh,
        const std::vector<caustica::math::float3>& vertices,
        bool recomputeNormals = true,
        bool rebuildAccelerationStructure = true);
    void setMeshVerticesWorld(const std::shared_ptr<caustica::MeshInfo>& mesh,
        const std::vector<caustica::math::float3>& vertices,
        bool recomputeNormals = true,
        bool rebuildAccelerationStructure = true);
    void setMeshVerticesWorld(caustica::ecs::Entity entity,
        const std::vector<caustica::math::float3>& vertices,
        bool recomputeNormals = true,
        bool rebuildAccelerationStructure = true);

private:
    bool importMeshFile(const std::filesystem::path& filePath,
        caustica::RuntimeMeshLoadResult (*loadFile)(const caustica::RuntimeMeshLoadParams&, const std::filesystem::path&));

    Context m_ctx;
};

} // namespace caustica::editor
