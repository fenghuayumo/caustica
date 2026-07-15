#pragma once

#include <math/math.h>
#include <rhi/nvrhi.h>
#include <ecs/Entity.h>

#include <filesystem>
#include <memory>

namespace caustica
{
struct MeshInfo;
}

namespace caustica::editor
{

class SceneEditor;

// Runtime mesh import, drag-drop handling, and scene-graph mesh editing.
class SceneContentEditor
{
public:
    explicit SceneContentEditor(SceneEditor& sceneEditor);

    void handleDroppedFiles(std::vector<std::string>& pendingFiles);

    bool loadMeshFile(const std::filesystem::path& filePath);
    bool loadGltfMeshFile(const std::filesystem::path& filePath);
    bool loadObjMeshFile(const std::filesystem::path& filePath);
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
    bool importMeshFile(const std::filesystem::path& filePath);

    SceneEditor& m_sceneEditor;
};

} // namespace caustica::editor
