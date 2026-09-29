#pragma once

#include <filesystem>
#include <memory>

namespace caustica
{

class IFileSystem;
class SceneTypeFactory;
class TextureLoader;
struct SceneImportResult;

struct RuntimeMeshLoadParams
{
    TextureLoader* TextureCache = nullptr;
    std::shared_ptr<SceneTypeFactory> SceneTypes;
    // Scene VFS when available; null falls back to native reads inside loaders.
    std::shared_ptr<IFileSystem> FileSystem;
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
