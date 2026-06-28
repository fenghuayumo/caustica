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

} // namespace caustica
