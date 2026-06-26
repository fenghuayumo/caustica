#pragma once

#include <filesystem>
#include <memory>

namespace caustica
{

class SceneTypeFactory;
class TextureLoader;
struct SceneImportResult;

struct RuntimeMeshLoadParams
{
    TextureLoader* TextureCache = nullptr;
    std::shared_ptr<SceneTypeFactory> SceneTypes;
    std::filesystem::path TextureSearchDirectory;
};

struct RuntimeMeshLoadResult
{
    bool Success = false;
    std::filesystem::path SourcePath;
    std::shared_ptr<SceneImportResult> ImportResult;

    explicit operator bool() const { return Success && ImportResult != nullptr; }
};

RuntimeMeshLoadResult LoadRuntimeMeshFile(
    const RuntimeMeshLoadParams& params,
    const std::filesystem::path& filePath);
RuntimeMeshLoadResult LoadRuntimeGltfMeshFile(
    const RuntimeMeshLoadParams& params,
    const std::filesystem::path& filePath);
RuntimeMeshLoadResult LoadRuntimeObjMeshFile(
    const RuntimeMeshLoadParams& params,
    const std::filesystem::path& filePath);

} // namespace caustica
