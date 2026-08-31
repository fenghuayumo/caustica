#pragma once

#include <math/math.h>
#include <rhi/rhi.h>
#include <ecs/Entity.h>

#include <filesystem>
#include <string>
#include <vector>

namespace caustica::editor
{

class SceneEditor;

enum class BuiltinPrimitiveKind
{
    Plane,
    Cube,
    Sphere,
    Cylinder,
};

enum class EditorLightKind
{
    Directional,
    Point,
    Spot,
    Rect,
    Environment,
};

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
    [[nodiscard]] caustica::ecs::Entity createBuiltinMesh(BuiltinPrimitiveKind kind);
    [[nodiscard]] caustica::ecs::Entity createLight(EditorLightKind kind);
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
    [[nodiscard]] std::string makeUniqueAuthoringId(const std::string& baseName) const;
    void registerAuthoredEntity(caustica::ecs::Entity entity);
    void placeInFrontOfCamera(caustica::ecs::Entity entity, bool snapToGround);
    void selectCreatedEntity(caustica::ecs::Entity entity, bool isLight);

    SceneEditor& m_sceneEditor;
};

} // namespace caustica::editor
