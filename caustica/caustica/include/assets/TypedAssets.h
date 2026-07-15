#pragma once

#include <assets/AssetId.h>

#include <filesystem>
#include <memory>
#include <string>

namespace caustica
{

struct Material;
struct MeshInfo;
class Scene;
struct SceneImportResult;

struct MeshAsset
{
    AssetId id = AssetId::invalid();
    std::string name;
    std::filesystem::path sourcePath;
    std::shared_ptr<MeshInfo> mesh;
};

struct MaterialAsset
{
    AssetId id = AssetId::invalid();
    std::string name;
    std::filesystem::path sourcePath;
    std::shared_ptr<Material> material;
};

struct SceneAsset
{
    AssetId id = AssetId::invalid();
    std::string name;
    std::filesystem::path sourcePath;
    std::shared_ptr<Scene> scene;
};

// CPU-only imported hierarchy (glTF/OBJ/…) ready to attachImportedScene / spawn.
struct ScenePrefabAsset
{
    AssetId id = AssetId::invalid();
    std::string name;
    std::filesystem::path sourcePath;
    std::shared_ptr<SceneImportResult> import;
};

} // namespace caustica
