#pragma once

#include <math/math.h>
#include <rhi/rhi.h>
#include <ecs/Entity.h>

#include <filesystem>
#include <vector>

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
    bool deleteEntity(caustica::ecs::Entity entity);
    void requestFullRebuild();

    std::vector<caustica::math::float3> getMeshVertices(caustica::ecs::Entity entity) const;
    std::vector<caustica::math::float3> getMeshVerticesWorld(caustica::ecs::Entity entity) const;
    void setMeshVertices(caustica::ecs::Entity entity,
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
