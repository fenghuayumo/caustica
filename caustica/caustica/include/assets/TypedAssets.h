#pragma once

#include <assets/AssetId.h>
#include <assets/Handle.h>

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

// App-facing mesh identity (AssetSystem). Prefer over digging MeshInfo / GPU ids.
using MeshHandle = Handle<MeshAsset>;

struct MaterialAsset
{
    AssetId id = AssetId::invalid();
    std::string name;
    std::filesystem::path sourcePath;
    std::shared_ptr<Material> material;
};

// App-facing material identity (AssetSystem). Prefer over digging Material GPU ids.
using MaterialHandle = Handle<MaterialAsset>;

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
